#include "usd_writer_mesh.h"
#include "usd_hierarchy_iterator.h"

#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>

extern "C" {
#include "BLI_assert.h"

#include "BKE_anim.h"
#include "BKE_library.h"
#include "BKE_material.h"

#include "DEG_depsgraph.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_particle_types.h"
}

USDGenericMeshWriter::USDGenericMeshWriter(const USDExporterContext &ctx) : USDAbstractWriter(ctx)
{
}

bool USDGenericMeshWriter::is_supported(const Object *object) const
{
  // Reject meshes that have a particle system that should have its emitter hidden.
  if (object->particlesystem.first != NULL) {
    char check_flag = export_params.evaluation_mode == DAG_EVAL_RENDER ? OB_DUPLI_FLAG_RENDER :
                                                                         OB_DUPLI_FLAG_VIEWPORT;
    return object->duplicator_visibility_flag & check_flag;
  }

  return true;
}

void USDGenericMeshWriter::do_write(HierarchyContext &context)
{
  Object *object_eval = context.object;
  bool needsfree = false;
  Mesh *mesh = get_export_mesh(object_eval, needsfree);

  if (mesh == NULL) {
    return;
  }

  try {
    write_mesh(context, mesh);

    if (needsfree) {
      free_export_mesh(mesh);
    }
  }
  catch (...) {
    if (needsfree) {
      free_export_mesh(mesh);
    }
    throw;
  }
}

void USDGenericMeshWriter::free_export_mesh(Mesh *mesh)
{
  BKE_id_free(NULL, mesh);
}

struct USDMeshData {
  pxr::VtArray<pxr::GfVec3f> points;
  pxr::VtIntArray face_vertex_counts;
  pxr::VtIntArray face_indices;
  std::map<short, pxr::VtIntArray> face_groups;

  /* The length of this array specifies the number of creases on the surface. Each element gives
   * the number of (must be adjacent) vertices in each crease, whose indices are linearly laid out
   * in the 'creaseIndices' attribute. Since each crease must be at least one edge long, each
   * element of this array should be greater than one. */
  pxr::VtIntArray crease_lengths;
  /* The indices of all vertices forming creased edges. The size of this array must be equal to the
   * sum of all elements of the 'creaseLengths' attribute. */
  pxr::VtIntArray crease_vertex_indices;
  /* The per-crease or per-edge sharpness for all creases (Usd.Mesh.SHARPNESS_INFINITE for a
   * perfectly sharp crease). Since 'creaseLengths' encodes the number of vertices in each crease,
   * the number of elements in this array will be either len(creaseLengths) or the sum over all X
   * of (creaseLengths[X] - 1). Note that while the RI spec allows each crease to have either a
   * single sharpness or a value per-edge, USD will encode either a single sharpness per crease on
   * a mesh, or sharpnesses for all edges making up the creases on a mesh. */
  pxr::VtFloatArray crease_sharpnesses;
};

void USDGenericMeshWriter::write_uv_maps(const Mesh *mesh, pxr::UsdGeomMesh usd_mesh)
{
  pxr::UsdTimeCode timecode = get_export_time_code();

  const CustomData *ldata = &mesh->ldata;
  for (int layer_idx = 0; layer_idx < ldata->totlayer; layer_idx++) {
    const CustomDataLayer *layer = &ldata->layers[layer_idx];
    if (layer->type != CD_MLOOPUV) {
      continue;
    }

    // UV coordinates are stored in a Primvar on the Mesh, and can be referenced from materials.
    pxr::TfToken primvar_name(pxr::TfMakeValidIdentifier(std::string("uv_") + layer->name));
    pxr::UsdGeomPrimvar uv_coords_primvar = usd_mesh.CreatePrimvar(
        primvar_name, pxr::SdfValueTypeNames->TexCoord2fArray, pxr::UsdGeomTokens->faceVarying);

    MLoopUV *mloopuv = static_cast<MLoopUV *>(layer->data);
    pxr::VtArray<pxr::GfVec2f> uv_coords;
    for (int loop_idx = 0; loop_idx < mesh->totloop; loop_idx++) {
      uv_coords.push_back(pxr::GfVec2f(mloopuv[loop_idx].uv));
    }
    uv_coords_primvar.Set(uv_coords, timecode);
  }
}

void USDGenericMeshWriter::write_mesh(HierarchyContext &context, Mesh *mesh)
{
  pxr::UsdTimeCode timecode = get_export_time_code();

  pxr::UsdGeomMesh usd_mesh = pxr::UsdGeomMesh::Define(stage, usd_path_);
  USDMeshData usd_mesh_data;
  get_geometry_data(mesh, usd_mesh_data);

  if (export_params.use_instancing && context.is_instance()) {
    // This object data is instanced, just reference the original instead of writing a copy.
    if (context.export_path == context.original_export_path) {
      printf("USD ref error: export path is reference path: %s\n", context.export_path.c_str());
      BLI_assert(!"USD reference error");
      return;
    }
    pxr::SdfPath ref_path(context.original_export_path);
    if (!usd_mesh.GetPrim().GetReferences().AddInternalReference(ref_path)) {
      /* See this URL for a description fo why referencing may fail"
       * https://graphics.pixar.com/usd/docs/api/class_usd_references.html#Usd_Failing_References
       */
      printf("USD Export warning: unable to add reference from %s to %s, not instancing object\n",
             context.export_path.c_str(),
             context.original_export_path.c_str());
      return;
    }
    /* The material path will be of the form </_materials/{material name}>, which is outside the
    subtree pointed to by ref_path. As a result, the referenced data is not allowed to point out
    of its own subtree. It does work when we override the material with exactly the same path,
    though.*/
    assign_materials(context, usd_mesh, usd_mesh_data.face_groups);
    return;
  }

  usd_mesh.CreatePointsAttr().Set(usd_mesh_data.points, timecode);
  usd_mesh.CreateFaceVertexCountsAttr().Set(usd_mesh_data.face_vertex_counts, timecode);
  usd_mesh.CreateFaceVertexIndicesAttr().Set(usd_mesh_data.face_indices, timecode);

  if (!usd_mesh_data.crease_lengths.empty()) {
    usd_mesh.CreateCreaseLengthsAttr().Set(usd_mesh_data.crease_lengths, timecode);
    usd_mesh.CreateCreaseIndicesAttr().Set(usd_mesh_data.crease_vertex_indices, timecode);
    usd_mesh.CreateCreaseSharpnessesAttr().Set(usd_mesh_data.crease_sharpnesses, timecode);
  }

  write_uv_maps(mesh, usd_mesh);

  // TODO(Sybren): figure out what happens when the face groups change.
  if (frame_has_been_written_) {
    return;
  }

  assign_materials(context, usd_mesh, usd_mesh_data.face_groups);
}

static void get_vertices(const Mesh *mesh, struct USDMeshData &usd_mesh_data)
{
  usd_mesh_data.points.reserve(mesh->totvert);

  const MVert *verts = mesh->mvert;
  for (int i = 0; i < mesh->totvert; ++i) {
    usd_mesh_data.points.push_back(pxr::GfVec3f(verts[i].co));
  }
}

static void get_loops_polys(const Mesh *mesh, struct USDMeshData &usd_mesh_data)
{
  /* Only construct face groups (a.k.a. geometry subsets) when we need them for material
   * assignments. */
  bool construct_face_groups = mesh->totcol > 1;

  usd_mesh_data.face_vertex_counts.reserve(mesh->totpoly);
  usd_mesh_data.face_indices.reserve(mesh->totloop);

  MLoop *mloop = mesh->mloop;
  MPoly *mpoly = mesh->mpoly;
  for (int i = 0; i < mesh->totpoly; ++i, ++mpoly) {
    MLoop *loop = mloop + mpoly->loopstart;
    usd_mesh_data.face_vertex_counts.push_back(mpoly->totloop);
    for (int j = 0; j < mpoly->totloop; ++j, ++loop) {
      usd_mesh_data.face_indices.push_back(loop->v);
    }

    if (construct_face_groups) {
      usd_mesh_data.face_groups[mpoly->mat_nr].push_back(i);
    }
  }
}

static void get_creases(const Mesh *mesh, struct USDMeshData &usd_mesh_data)
{
  const float factor = 1.0f / 255.0f;

  MEdge *edge = mesh->medge;
  float sharpness;
  for (int edge_idx = 0, totedge = mesh->totedge; edge_idx < totedge; ++edge_idx, ++edge) {
    if (edge->crease == 0) {
      continue;
    }

    if (edge->crease == 255) {
      sharpness = pxr::UsdGeomMesh::SHARPNESS_INFINITE;
    }
    else {
      sharpness = static_cast<float>(edge->crease) * factor;
    }

    usd_mesh_data.crease_vertex_indices.push_back(edge->v1);
    usd_mesh_data.crease_vertex_indices.push_back(edge->v2);
    usd_mesh_data.crease_lengths.push_back(2);
    usd_mesh_data.crease_sharpnesses.push_back(sharpness);
  }
}

void USDGenericMeshWriter::get_geometry_data(const Mesh *mesh, struct USDMeshData &usd_mesh_data)
{
  get_vertices(mesh, usd_mesh_data);
  get_loops_polys(mesh, usd_mesh_data);
  get_creases(mesh, usd_mesh_data);
}

void USDGenericMeshWriter::assign_materials(
    const HierarchyContext &context,
    pxr::UsdGeomMesh usd_mesh,
    const std::map<short, pxr::VtIntArray> &usd_face_groups)
{
  if (context.object->totcol == 0) {
    return;
  }

  /* Binding a material to a geometry subset isn't supported by the Hydra GL viewport yet,
   * which is why we always bind the first material to the entire mesh. See
   * https://github.com/PixarAnimationStudios/USD/issues/542 for more info. */
  bool mesh_material_bound = false;
  for (short mat_num = 0; mat_num < context.object->totcol; mat_num++) {
    Material *material = give_current_material(context.object, mat_num + 1);
    if (material == nullptr) {
      continue;
    }

    pxr::UsdShadeMaterial usd_material = ensure_usd_material(material);
    usd_material.Bind(usd_mesh.GetPrim());

    /* USD seems to support neither per-material nor per-face-group double-sidedness, so we just
     * use the flag from the first non-empty material slot. */
    usd_mesh.CreateDoubleSidedAttr(
        pxr::VtValue((material->blend_flag & MA_BL_CULL_BACKFACE) == 0));

    mesh_material_bound = true;
    break;
  }

  if (!mesh_material_bound) {
    /* Blender defaults to double-sided, but USD to single-sided. */
    usd_mesh.CreateDoubleSidedAttr(pxr::VtValue(true));
  }

  if (!mesh_material_bound || usd_face_groups.size() < 2) {
    /* Either all material slots were empty or there is only one material in use. As geometry
     * subsets are only written when actually used to assign a material, and the mesh already has
     * the material assigned, there is no need to continue. */
    return;
  }

  // Define a geometry subset per material.
  for (auto face_group_iter : usd_face_groups) {
    short material_number = face_group_iter.first;
    const pxr::VtIntArray &face_indices = face_group_iter.second;

    Material *material = give_current_material(context.object, material_number + 1);
    if (material == nullptr) {
      continue;
    }

    pxr::UsdShadeMaterial usd_material = ensure_usd_material(material);
    pxr::TfToken material_name = usd_material.GetPath().GetNameToken();

    pxr::UsdShadeMaterialBindingAPI api = pxr::UsdShadeMaterialBindingAPI(usd_mesh);
    pxr::UsdGeomSubset usd_face_subset = api.CreateMaterialBindSubset(material_name, face_indices);
    usd_material.Bind(usd_face_subset.GetPrim());
  }
}

USDMeshWriter::USDMeshWriter(const USDExporterContext &ctx) : USDGenericMeshWriter(ctx)
{
}

Mesh *USDMeshWriter::get_export_mesh(Object *object_eval, bool & /*r_needsfree*/)
{
  return object_eval->runtime.mesh_eval;
}