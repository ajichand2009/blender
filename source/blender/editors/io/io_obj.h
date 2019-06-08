
/* * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2016 Blender Foundation.
 * All rights reserved.
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 */

#ifndef __IO_OBJ_H__
#define __IO_OBJ_H__

struct wmOperatorType;

/* #include "WM_types.h" */

#include "io_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OBJExportSettings {
	float frame_start;
	float frame_end;

	bool flatten_hierarchy;

} OBJExportSettings;

void WM_OT_obj_export(struct wmOperatorType *ot);
void WM_OT_obj_import(struct wmOperatorType *ot);

#ifdef __cplusplus
}
#endif

#endif /* __IO_OBJ_H__ */