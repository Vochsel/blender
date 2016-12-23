/*
 * ***** BEGIN GPL LICENSE BLOCK *****
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
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Dalai Felinto
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#ifndef __BKE_LAYER_H__
#define __BKE_LAYER_H__

/** \file blender/blenkernel/BKE_layer.h
 *  \ingroup bke
 */

#include "BKE_collection.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TODO_LAYER_SYNC /* syncing of SceneCollection and LayerCollection trees*/
#define TODO_LAYER_SYNC_FILTER /* syncing of filter_objects across all trees */
#define TODO_LAYER_OVERRIDE /* CollectionOverride */
#define TODO_LAYER_CONTEXT /* get/set current (context) SceneLayer */
#define TODO_LAYER /* generic todo */

struct LayerCollection;
struct ID;
struct Main;
struct Object;
struct ObjectBase;
struct Scene;
struct SceneCollection;
struct SceneLayer;

struct SceneLayer *BKE_scene_layer_add(struct Scene *scene, const char *name);

bool BKE_scene_layer_remove(struct Main *bmain, struct Scene *scene, struct SceneLayer *sl);

void BKE_scene_layer_free(struct SceneLayer *sl);

void BKE_scene_layer_engine_set(struct SceneLayer *sl, const char *engine);

void BKE_scene_layer_selected_objects_tag(struct SceneLayer *sl, const int tag);

struct ObjectBase *BKE_scene_layer_base_find(struct SceneLayer *sl, struct Object *ob);

void BKE_layer_collection_free(struct SceneLayer *sl, struct LayerCollection *lc);

struct LayerCollection *BKE_layer_collection_active(struct SceneLayer *sl);

int BKE_layer_collection_count(struct SceneLayer *sl);

int BKE_layer_collection_findindex(struct SceneLayer *sl, struct LayerCollection *lc);

struct LayerCollection *BKE_collection_link(struct SceneLayer *sl, struct SceneCollection *sc);

void BKE_collection_unlink(struct SceneLayer *sl, struct LayerCollection *lc);

/* syncing */

void BKE_layer_sync_new_scene_collection(struct Scene *scene, const struct SceneCollection *sc_parent, struct SceneCollection *sc);
void BKE_layer_sync_object_link(struct Scene *scene, struct SceneCollection *sc, struct Object *ob);
void BKE_layer_sync_object_unlink(struct Scene *scene, struct SceneCollection *sc, struct Object *ob);

/* override */

void BKE_collection_override_datablock_add(struct LayerCollection *lc, const char *data_path, struct ID *id);

/* iterators */

void BKE_selected_objects_Iterator_begin(Iterator *iter, void *data_in);
void BKE_selected_objects_Iterator_next(Iterator *iter);
void BKE_selected_objects_Iterator_end(Iterator *iter);

#define FOREACH_SELECTED_OBJECT(sl, _ob)                                      \
	ITER_BEGIN(BKE_selected_objects_Iterator_begin,                           \
	           BKE_selected_objects_Iterator_next,                            \
	           BKE_selected_objects_Iterator_end,                             \
	           sl, _ob)

#define FOREACH_SELECTED_OBJECT_END                                           \
	ITER_END

#define FOREACH_OBJECT_FLAG(scene, sl, flag, _ob)                             \
{                                                                             \
	IteratorBeginCb func_begin;                                               \
	IteratorCb func_next, func_end;                                           \
	void *data_in;                                                            \
	                                                                          \
	if (flag == SELECT) {                                                     \
	    func_begin = &BKE_selected_objects_Iterator_begin;                    \
	    func_next = &BKE_selected_objects_Iterator_next;                      \
	    func_end = &BKE_selected_objects_Iterator_end;                        \
	    data_in = sl;                                                         \
    }                                                                         \
	else {                                                                    \
	    func_begin = BKE_scene_objects_Iterator_begin;                        \
	    func_next = BKE_scene_objects_Iterator_next;                          \
	    func_end = BKE_scene_objects_Iterator_end;                            \
	    data_in = scene;                                                      \
    }                                                                         \
	ITER_BEGIN(func_begin, func_next, func_end, data_in, _ob)


#define FOREACH_OBJECT_FLAG_END                                               \
	ITER_END                                                                  \
}

#ifdef __cplusplus
}
#endif

#endif /* __BKE_LAYER_H__ */
