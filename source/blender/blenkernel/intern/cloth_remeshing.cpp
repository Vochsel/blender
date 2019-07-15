/*
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
 * The Original Code is Copyright (C) Blender Foundation
 * All rights reserved.
 */

/** \file
 * \ingroup bke
 */

extern "C" {

#include "MEM_guardedalloc.h"

#include "DNA_cloth_types.h"
#include "DNA_scene_types.h"
#include "DNA_object_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_customdata_types.h"

#include "BLI_utildefines.h"
#include "BLI_math.h"
#include "BLI_edgehash.h"
#include "BLI_linklist.h"
#include "BLI_array.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "BKE_bvhutils.h"
#include "BKE_cloth.h"
#include "BKE_effect.h"
#include "BKE_global.h"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_mesh_mapping.h"
#include "BKE_modifier.h"
#include "BKE_library.h"
#if USE_CLOTH_CACHE
#  include "BKE_pointcache.h"
#endif

#include "bmesh.h"
#include "bmesh_tools.h"

#include "ED_mesh.h"
}

#include "BKE_cloth_remeshing.h"

#include "BPH_mass_spring.h"

#include <vector>
#include <utility>
using namespace std;

/******************************************************************************
 * cloth_remeshing_step - remeshing function for adaptive cloth modifier
 * reference http://graphics.berkeley.edu/papers/Narain-AAR-2012-11/index.html
 ******************************************************************************/

static bool cloth_remeshing_boundary_test(BMVert *v);
static bool cloth_remeshing_boundary_test(BMEdge *e);
static bool cloth_remeshing_edge_on_seam_test(BMesh *bm, BMEdge *e);
static bool cloth_remeshing_vert_on_seam_test(BMesh *bm, BMVert *v);
static void cloth_remeshing_uv_of_vert_in_face(BMesh *bm, BMFace *f, BMVert *v, float r_uv[2]);
static float cloth_remeshing_wedge(float v_01[2], float v_02[2]);
static float cloth_remeshing_edge_size_with_vert(
    BMesh *bm, BMVert *v1, BMVert *v2, BMVert *v, vector<ClothSizing> &sizing);
static void cloth_remeshing_update_active_faces(vector<BMFace *> &active_faces,
                                                BMesh *bm,
                                                BMVert *v);
static void cloth_remeshing_update_active_faces(vector<BMFace *> &active_faces,
                                                BMesh *bm,
                                                BMEdge *e);

static CustomData_MeshMasks cloth_remeshing_get_cd_mesh_masks(void)
{
  CustomData_MeshMasks cddata_masks;

  cddata_masks.vmask = CD_MASK_ORIGINDEX;
  cddata_masks.emask = CD_MASK_ORIGINDEX;
  cddata_masks.pmask = CD_MASK_ORIGINDEX;

  return cddata_masks;
}

static void cloth_remeshing_init_bmesh(Object *ob, ClothModifierData *clmd, Mesh *mesh)
{
  if (clmd->sim_parms->remeshing_reset || !clmd->clothObject->bm_prev) {
    cloth_to_mesh(ob, clmd, mesh);

    CustomData_MeshMasks cddata_masks = cloth_remeshing_get_cd_mesh_masks();
    if (clmd->clothObject->bm_prev) {
      BM_mesh_free(clmd->clothObject->bm_prev);
      clmd->clothObject->bm_prev = NULL;
    }
    struct BMeshCreateParams bmesh_create_params;
    bmesh_create_params.use_toolflags = 0;
    struct BMeshFromMeshParams bmesh_from_mesh_params;
    bmesh_from_mesh_params.calc_face_normal = true;
    bmesh_from_mesh_params.cd_mask_extra = cddata_masks;
    clmd->clothObject->bm_prev = BKE_mesh_to_bmesh_ex(
        mesh, &bmesh_create_params, &bmesh_from_mesh_params);
    BMVert *v;
    BMIter viter;
    int i = 0;
    BM_ITER_MESH_INDEX (v, &viter, clmd->clothObject->bm_prev, BM_VERTS_OF_MESH, i) {
      copy_v3_v3(v->co, clmd->clothObject->verts[i].x);
    }

    BM_mesh_normals_update(clmd->clothObject->bm_prev);

    BM_mesh_triangulate(clmd->clothObject->bm_prev,
                        MOD_TRIANGULATE_QUAD_SHORTEDGE,
                        MOD_TRIANGULATE_NGON_BEAUTY,
                        4,
                        false,
                        NULL,
                        NULL,
                        NULL);
    printf("remeshing_reset has been set to true or bm_prev does not exist\n");
  }
  else {
    BMVert *v;
    BMIter viter;
    int i = 0;
    BM_ITER_MESH_INDEX (v, &viter, clmd->clothObject->bm_prev, BM_VERTS_OF_MESH, i) {
      copy_v3_v3(v->co, clmd->clothObject->verts[i].x);
    }
  }
  clmd->clothObject->mvert_num_prev = clmd->clothObject->mvert_num;
  clmd->clothObject->bm = clmd->clothObject->bm_prev;
}

static ClothVertex cloth_remeshing_mean_cloth_vert(ClothVertex *v1, ClothVertex *v2)
{
  ClothVertex new_vert;

  new_vert.flags = 0;
  if ((v1->flags & CLOTH_VERT_FLAG_PINNED) && (v2->flags & CLOTH_VERT_FLAG_PINNED)) {
    new_vert.flags |= CLOTH_VERT_FLAG_PINNED;
  }
  if ((v1->flags & CLOTH_VERT_FLAG_NOSELFCOLL) && (v2->flags & CLOTH_VERT_FLAG_NOSELFCOLL)) {
    new_vert.flags |= CLOTH_VERT_FLAG_NOSELFCOLL;
  }

  add_v3_v3v3(new_vert.v, v1->v, v2->v);
  mul_v3_fl(new_vert.v, 0.5f);

  add_v3_v3v3(new_vert.xconst, v1->xconst, v2->xconst);
  mul_v3_fl(new_vert.xconst, 0.5f);

  add_v3_v3v3(new_vert.x, v1->x, v2->x);
  mul_v3_fl(new_vert.x, 0.5f);

  add_v3_v3v3(new_vert.xold, v1->xold, v2->xold);
  mul_v3_fl(new_vert.xold, 0.5f);

  add_v3_v3v3(new_vert.tx, v1->tx, v2->tx);
  mul_v3_fl(new_vert.tx, 0.5f);

  add_v3_v3v3(new_vert.txold, v1->txold, v2->txold);
  mul_v3_fl(new_vert.txold, 0.5f);

  add_v3_v3v3(new_vert.tv, v1->tv, v2->tv);
  mul_v3_fl(new_vert.tv, 0.5f);

  add_v3_v3v3(new_vert.impulse, v1->impulse, v2->impulse);
  mul_v3_fl(new_vert.impulse, 0.5f);

  add_v3_v3v3(new_vert.xrest, v1->xrest, v2->xrest);
  mul_v3_fl(new_vert.xrest, 0.5f);

  add_v3_v3v3(new_vert.dcvel, v1->dcvel, v2->dcvel);
  mul_v3_fl(new_vert.dcvel, 0.5f);

  new_vert.mass = (v1->mass + v2->mass) * 0.5;
  new_vert.goal = (v1->goal + v2->goal) * 0.5;
  new_vert.impulse_count = (v1->impulse_count + v2->impulse_count) * 0.5;
  /* new_vert.avg_spring_len = (v1->avg_spring_len + v2->avg_spring_len) * 0.5; */
  new_vert.struct_stiff = (v1->struct_stiff + v2->struct_stiff) * 0.5;
  new_vert.bend_stiff = (v1->bend_stiff + v2->bend_stiff) * 0.5;
  new_vert.shear_stiff = (v1->shear_stiff + v2->shear_stiff) * 0.5;
  /* new_vert.spring_count = (v1->spring_count + v2->spring_count) * 0.5; */
  new_vert.shrink_factor = (v1->shrink_factor + v2->shrink_factor) * 0.5;

  return new_vert;
}

static Mesh *cloth_remeshing_update_cloth_object_bmesh(Object *ob, ClothModifierData *clmd)
{
  Mesh *mesh_result = NULL;
  CustomData_MeshMasks cddata_masks = cloth_remeshing_get_cd_mesh_masks();
  mesh_result = BKE_mesh_from_bmesh_for_eval_nomain(clmd->clothObject->bm, &cddata_masks);
  if (clmd->clothObject->mvert_num_prev == clmd->clothObject->mvert_num) {
    clmd->clothObject->bm_prev = BM_mesh_copy(clmd->clothObject->bm);
    BM_mesh_free(clmd->clothObject->bm);
    clmd->clothObject->bm = NULL;
    return mesh_result;
  }
  /* cloth_remeshing_update_cloth_object_mesh(clmd, mesh_result); */
  /**/

  Cloth *cloth = clmd->clothObject;
  // Free the springs.
  if (cloth->springs != NULL) {
    LinkNode *search = cloth->springs;
    while (search) {
      ClothSpring *spring = (ClothSpring *)search->link;

      MEM_SAFE_FREE(spring->pa);
      MEM_SAFE_FREE(spring->pb);

      MEM_freeN(spring);
      search = search->next;
    }
    BLI_linklist_free(cloth->springs, NULL);

    cloth->springs = NULL;
  }

  cloth->springs = NULL;
  cloth->numsprings = 0;

  // free BVH collision tree
  if (cloth->bvhtree) {
    BLI_bvhtree_free(cloth->bvhtree);
    cloth->bvhtree = NULL;
  }

  if (cloth->bvhselftree) {
    BLI_bvhtree_free(cloth->bvhselftree);
    cloth->bvhselftree = NULL;
  }

  if (cloth->implicit) {
    BPH_mass_spring_solver_free(cloth->implicit);
    cloth->implicit = NULL;
  }

  // we save our faces for collision objects
  if (cloth->tri) {
    MEM_freeN(cloth->tri);
    cloth->tri = NULL;
  }

  if (cloth->edgeset) {
    BLI_edgeset_free(cloth->edgeset);
    cloth->edgeset = NULL;
  }

  /* Now build those things */
  const MLoop *mloop = mesh_result->mloop;
  const MLoopTri *looptri = BKE_mesh_runtime_looptri_ensure(mesh_result);
  const unsigned int looptri_num = mesh_result->runtime.looptris.len;

  /* save face information */
  clmd->clothObject->tri_num = looptri_num;
  clmd->clothObject->tri = (MVertTri *)MEM_mallocN(sizeof(MVertTri) * looptri_num,
                                                   "clothLoopTris");
  if (clmd->clothObject->tri == NULL) {
    cloth_free_modifier(clmd);
    modifier_setError(&(clmd->modifier), "Out of memory on allocating clmd->clothObject->looptri");
    printf("cloth_free_modifier clmd->clothObject->looptri\n");
    return NULL;
  }
  BKE_mesh_runtime_verttri_from_looptri(clmd->clothObject->tri, mloop, looptri, looptri_num);

  if (!cloth_build_springs(clmd, mesh_result)) {
    cloth_free_modifier(clmd);
    modifier_setError(&(clmd->modifier), "Cannot build springs");
    return NULL;
  }

  // init our solver
  BPH_cloth_solver_init(ob, clmd);

  BKE_cloth_solver_set_positions(clmd);

  clmd->clothObject->bvhtree = bvhtree_build_from_cloth(clmd, clmd->coll_parms->epsilon);
  clmd->clothObject->bvhselftree = bvhtree_build_from_cloth(clmd, clmd->coll_parms->selfepsilon);

  /**/
  clmd->clothObject->bm_prev = BM_mesh_copy(clmd->clothObject->bm);
  BM_mesh_free(clmd->clothObject->bm);
  clmd->clothObject->bm = NULL;

  clmd->clothObject->mvert_num_prev = clmd->clothObject->mvert_num;

  return mesh_result;
}

/* returns a pair of vertices that are on the edge faces of the given
 * edge */
static pair<BMVert *, BMVert *> cloth_remeshing_edge_side_verts(BMEdge *e)
{
  BMFace *f1, *f2;
  pair<BMVert *, BMVert *> edge_side_verts;
  edge_side_verts.first = NULL;
  edge_side_verts.second = NULL;
  BM_edge_face_pair(e, &f1, &f2);
  if (f1) {
    BMVert *v;
    BMIter viter;
    BM_ITER_ELEM (v, &viter, f1, BM_VERTS_OF_FACE) {
      if (v != e->v1 && v != e->v2) {
        edge_side_verts.first = v;
        break;
      }
    }
  }
  if (f2) {
    BMVert *v;
    BMIter viter;
    BM_ITER_ELEM (v, &viter, f2, BM_VERTS_OF_FACE) {
      if (v != e->v1 && v != e->v2) {
        edge_side_verts.second = v;
        break;
      }
    }
  }
  return edge_side_verts;
}

/* from Bossen and Heckbert 1996 */
#define CLOTH_REMESHING_EDGE_FLIP_THRESHOLD 0.001f
static bool cloth_remeshing_should_flip(
    BMesh *bm, BMVert *v1, BMVert *v2, BMVert *v3, BMVert *v4, vector<ClothSizing> &sizing)
{
  BMFace *f1, *f2;
  BM_edge_face_pair(BM_edge_exists(v1, v2), &f1, &f2);
  float x[2], y[2], z[2], w[2];
  cloth_remeshing_uv_of_vert_in_face(bm, f1, v1, x);
  cloth_remeshing_uv_of_vert_in_face(bm, f2, v4, y);
  cloth_remeshing_uv_of_vert_in_face(bm, f2, v2, z);
  cloth_remeshing_uv_of_vert_in_face(bm, f1, v3, w);

  /* TODO(Ish): fix sizing when properly implemented */
  float m[2][2];
  copy_m2_m2(m, sizing[0].m);

  float zy[2], xy[2], xw[2], mzw[2], mxy[2], zw[2];
  copy_v2_v2(zy, z);
  sub_v2_v2(zy, y);
  copy_v2_v2(xy, x);
  sub_v2_v2(xy, y);
  copy_v2_v2(xw, x);
  sub_v2_v2(xw, w);
  copy_v2_v2(zw, z);
  sub_v2_v2(zw, w);

  mul_v2_m2v2(mzw, m, zw);
  mul_v2_m2v2(mxy, m, xy);

  return cloth_remeshing_wedge(zy, xy) * dot_v2v2(xw, mzw) +
             dot_v2v2(zy, mxy) * cloth_remeshing_wedge(xw, zw) <
         -CLOTH_REMESHING_EDGE_FLIP_THRESHOLD *
             (cloth_remeshing_wedge(zy, xy) + cloth_remeshing_wedge(xw, zw));
}

static vector<BMEdge *> cloth_remeshing_find_edges_to_flip(BMesh *bm,
                                                           vector<ClothSizing> &sizing,
                                                           vector<BMFace *> &active_faces)
{
  printf("starting: %s\n", __func__);
  vector<BMEdge *> edges;
  BMVert *v1;
  BMIter viter;
  for (int i = 0; i < active_faces.size(); i++) {
    BMFace *f = active_faces[i];
    BM_ITER_ELEM (v1, &viter, f, BM_VERTS_OF_FACE) {
      BMEdge *e;
      BMIter eiter;
      BM_ITER_ELEM (e, &eiter, v1, BM_EDGES_OF_VERT) {
        BMVert *v2 = e->v2;
        if (v2 == v1) {
          v2 = e->v1;
        }

        /* Done to prevent same edge being checked multiple times */
        if (v2 < v1) {
          continue;
        }

        pair<BMVert *, BMVert *> edge_side_verts = cloth_remeshing_edge_side_verts(e);
        BMFace *f1, *f2;
        BM_edge_face_pair(e, &f1, &f2);
        BMVert *v3, *v4;
        v3 = edge_side_verts.first;
        v4 = edge_side_verts.second;

        if (!v3 || !v4) {
          continue;
        }
        float uv_01[2], uv_02[2];
        cloth_remeshing_uv_of_vert_in_face(bm, f1, e->v1, uv_01);
        cloth_remeshing_uv_of_vert_in_face(bm, f2, e->v1, uv_02);
        if (!equals_v2v2(uv_01, uv_02)) {
          continue;
        }
        cloth_remeshing_uv_of_vert_in_face(bm, f1, e->v2, uv_01);
        cloth_remeshing_uv_of_vert_in_face(bm, f2, e->v2, uv_02);
        if (!equals_v2v2(uv_01, uv_02)) {
          continue;
        }

        if (cloth_remeshing_edge_size_with_vert(bm, v3, v4, e->v1, sizing) > 1.0f) {
          continue;
        }

        if (!cloth_remeshing_should_flip(bm, v1, v2, v3, v4, sizing)) {
          continue;
        }

        edges.push_back(e);
      }
    }
  }
  printf("ending: %s\n", __func__);
  return edges;
}

static bool cloth_remeshing_independent_edge_test(BMEdge *e, vector<BMEdge *> edges)
{
  for (int i = 0; i < edges.size(); i++) {
    if (e->v1 == edges[i]->v1 || e->v1 == edges[i]->v2 || e->v2 == edges[i]->v1 ||
        e->v2 == edges[i]->v2) {
      return false;
    }
  }
  return true;
}

static vector<BMEdge *> cloth_remeshing_find_independent_edges(vector<BMEdge *> edges)
{
  printf("starting: %s\n", __func__);
  vector<BMEdge *> i_edges;
  for (int i = 0; i < edges.size(); i++) {
    if (cloth_remeshing_independent_edge_test(edges[i], i_edges)) {
      i_edges.push_back(edges[i]);
    }
  }
  printf("ending: %s\n", __func__);
  return i_edges;
}

static void cloth_remeshing_flip_edges(BMesh *bm,
                                       vector<ClothSizing> &sizing,
                                       vector<BMFace *> &active_faces)
{
  printf("starting: %s\n", __func__);
  static int prev_num_flipable_edges = 0;
  /* TODO(Ish): This loop might cause problems */
  while (active_faces.size() != 0) {
    vector<BMEdge *> flipable_edges = cloth_remeshing_find_edges_to_flip(bm, sizing, active_faces);
    if (flipable_edges.size() == prev_num_flipable_edges) {
      break;
    }
    prev_num_flipable_edges = flipable_edges.size();
    vector<BMEdge *> independent_edges = cloth_remeshing_find_independent_edges(flipable_edges);

    for (int i = 0; i < independent_edges.size(); i++) {
      BMEdge *edge = independent_edges[i];
      /* BM_EDGEROT_CHECK_SPLICE sets it up for BM_CREATE_NO_DOUBLE */
      BMEdge *new_edge = BM_edge_rotate(bm, edge, true, BM_EDGEROT_CHECK_SPLICE);
      BLI_assert(new_edge != NULL);
      /* TODO(Ish): need to update active_faces */
      cloth_remeshing_update_active_faces(active_faces, bm, new_edge);
    }
  }
  printf("ending: %s\n", __func__);
}

static bool cloth_remeshing_fix_mesh(BMesh *bm,
                                     vector<ClothSizing> &sizing,
                                     vector<BMFace *> active_faces)
{
  cloth_remeshing_flip_edges(bm, sizing, active_faces);
  return true;
}

static float cloth_remeshing_edge_size(BMesh *bm, BMEdge *edge, vector<ClothSizing> &sizing)
{
  /* BMVert v1 = *edge->v1; */
  float u1[2], u2[2];
  float u12[2];

  /**
   * Get UV Coordinates of v1 and v2
   */
  BMLoop *l;
  BMLoop *l2;
  MLoopUV *luv;
  const int cd_loop_uv_offset = CustomData_get_offset(&bm->ldata, CD_MLOOPUV);

  /* find edge_size only if edge has a loop (so it has a face attached) */
  if (!edge->l) {
    /* TODO(Ish): Might want to mark such an edge as sewing edge, so it can be used when an edge
     * connected to 2 sewing edges is split, new sewing edges can be added */
    return 0.0f;
  }

  l = edge->l;
  if (l->v == edge->v1) {
    if (l->next->v == edge->v2) {
      l2 = l->next;
    }
    else {
      l2 = l->prev;
    }
  }
  else {
    if (l->next->v == edge->v1) {
      l2 = l->next;
    }
    else {
      l2 = l->prev;
    }
  }

  luv = (MLoopUV *)BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
  copy_v2_v2(u1, luv->uv);
  luv = (MLoopUV *)BM_ELEM_CD_GET_VOID_P(l2, cd_loop_uv_offset);
  copy_v2_v2(u2, luv->uv);
  copy_v2_v2(u12, u1);
  sub_v2_v2(u12, u2);

  /**
   *TODO(Ish): Need to add functionality when the vertex is on a seam
   */

  float value = 0.0;
  float temp_v2[2];
  /* int index = BM_elem_index_get(&v1); */
  /* ClothSizing sizing_temp = sizing[index] */
  ClothSizing sizing_temp = sizing[0];
  /* TODO(Ish): sizing_temp needs to be average of the both vertices, for static it doesn't
   * matter since all sizing are same */
  mul_v2_m2v2(temp_v2, sizing_temp.m, u12);
  value += dot_v2v2(u12, temp_v2);

  return sqrtf(fmax(value, 0.0f));
}

static int cloth_remeshing_edge_pair_compare(const void *a, const void *b)
{
  Edge_Pair *ea = (Edge_Pair *)a;
  Edge_Pair *eb = (Edge_Pair *)b;
  if (ea->size < eb->size) {
    return 1;
  }
  if (ea->size > eb->size) {
    return -1;
  }
  return 0;
}

static void cloth_remeshing_find_bad_edges(BMesh *bm,
                                           vector<ClothSizing> sizing,
                                           vector<BMEdge *> &r_edges)
{
  Edge_Pair *edge_pairs = (Edge_Pair *)MEM_mallocN(sizeof(Edge_Pair) * bm->totedge, "Edge Pairs");

  int tagged = 0;
  BMEdge *e;
  BMIter eiter;
  BM_ITER_MESH (e, &eiter, bm, BM_EDGES_OF_MESH) {
    float size = cloth_remeshing_edge_size(bm, e, sizing);
    if (size > 1.0f) {
      edge_pairs[tagged].size = size;
      edge_pairs[tagged].edge = e;
      tagged++;
    }
  }

  /* sort the list based on the size */
  qsort(edge_pairs, tagged, sizeof(Edge_Pair), cloth_remeshing_edge_pair_compare);

  for (int i = 0; i < tagged; i++) {
    r_edges.push_back(edge_pairs[i].edge);
  }

  MEM_freeN(edge_pairs);
}

static BMVert *cloth_remeshing_split_edge_keep_triangles(BMesh *bm,
                                                         BMEdge *e,
                                                         BMVert *v,
                                                         float fac)
{
  BLI_assert(BM_vert_in_edge(e, v) == true);

  /* find faces containing edge, should be only 2 */
  BMFace *f;
  BMIter fiter;
  int face_i = 0;
  BMFace *f1 = NULL, *f2 = NULL;
  BM_ITER_ELEM_INDEX (f, &fiter, e, BM_FACES_OF_EDGE, face_i) {
    if (face_i == 0) {
      f1 = f;
    }
    else {
      f2 = f;
    }
    /* printf("face_i: %d\n", face_i); */
  }

  if (!f1) {
    return NULL;
  }

  /* split the edge */
  BMEdge *new_edge;
  BMVert *new_v = BM_edge_split(bm, e, v, &new_edge, fac);

  BMVert *vert;
  BMIter viter;
  /* search for vert within the face that is not part of input edge
   * create new edge between this
   * vert and newly created vert */
  BM_ITER_ELEM (vert, &viter, f1, BM_VERTS_OF_FACE) {
    if (vert == e->v1 || vert == e->v2 || vert == new_edge->v1 || vert == new_edge->v2 ||
        vert == new_v) {
      continue;
    }
    BMLoop *l_a = NULL, *l_b = NULL;
    l_a = BM_face_vert_share_loop(f1, vert);
    l_b = BM_face_vert_share_loop(f1, new_v);
    if (!BM_face_split(bm, f1, l_a, l_b, NULL, NULL, true)) {
      printf("face not split: f1\n");
    }
    break;
  }
  if (f2) {
    BM_ITER_ELEM (vert, &viter, f2, BM_VERTS_OF_FACE) {
      if (vert == e->v1 || vert == e->v2 || vert == new_edge->v1 || vert == new_edge->v2 ||
          vert == new_v) {
        continue;
      }
      BMLoop *l_a = NULL, *l_b = NULL;
      l_a = BM_face_vert_share_loop(f2, vert);
      l_b = BM_face_vert_share_loop(f2, new_v);
      if (!BM_face_split(bm, f2, l_a, l_b, NULL, NULL, true)) {
        printf("face not split: f2\n");
      }
      break;
    }
  }

  return new_v;
}

static void cloth_remeshing_export_obj(BMesh *bm, char *file_name)
{
  FILE *fout = fopen(file_name, "w");
  if (!fout) {
    printf("File not written, couldn't find path to %s\n", file_name);
    return;
  }
  printf("Writing file %s\n", file_name);
  typedef struct Vert {
    float co[3];
    float no[3];
  } Vert;
  typedef struct UV {
    float uv[2];
  } UV;
  typedef struct Face {
    int *vi;
    int *uv;
    int len;
    BMFace *bm_face;
  } Face;
  Vert *verts;
  verts = (Vert *)MEM_mallocN(sizeof(Vert) * bm->totvert, "Verts");
  UV *uvs;
  uvs = (UV *)MEM_mallocN(sizeof(UV) * bm->totvert, "UVs");
  Face *faces;
  faces = (Face *)MEM_mallocN(sizeof(Face) * bm->totface, "Faces");
  for (int i = 0; i < bm->totface; i++) {
    faces[i].vi = NULL;
    faces[i].uv = NULL;
    faces[i].len = 0;
    faces[i].bm_face = NULL;
  }
  BMVert *v;
  BMIter viter;
  int vert_i = 0;
  int face_i = 0;
  BM_ITER_MESH (v, &viter, bm, BM_VERTS_OF_MESH) {
    copy_v3_v3(verts[vert_i].co, v->co);
    copy_v3_v3(verts[vert_i].no, v->no);
    BMFace *f;
    BMIter fiter;
    BM_ITER_ELEM (f, &fiter, v, BM_FACES_OF_VERT) {
      int curr_i = face_i;
      for (int i = 0; i < face_i; i++) {
        if (faces[i].bm_face == f) {
          curr_i = i;
          break;
        }
      }
      if (faces[curr_i].len == 0) {
        faces[curr_i].len = 1;
        faces[curr_i].vi = (int *)MEM_mallocN(sizeof(int) * 6, "face vi");
        faces[curr_i].uv = (int *)MEM_mallocN(sizeof(int) * 6, "face uv");
        faces[curr_i].bm_face = f;
        face_i++;
      }
      faces[curr_i].vi[faces[curr_i].len - 1] = vert_i + 1;
      faces[curr_i].uv[faces[curr_i].len - 1] = vert_i + 1;
      faces[curr_i].len++;
    }
    BMLoop *l;
    BMIter liter;
    MLoopUV *luv;
    const int cd_loop_uv_offset = CustomData_get_offset(&bm->ldata, CD_MLOOPUV);

    BM_ITER_ELEM (l, &liter, v, BM_LOOPS_OF_VERT) {
      luv = (MLoopUV *)BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
      copy_v2_v2(uvs[vert_i].uv, luv->uv);
    }
    vert_i++;
  }

  for (int i = 0; i < bm->totvert; i++) {
    fprintf(fout, "v %f %f %f\n", verts[i].co[0], verts[i].co[1], verts[i].co[2]);
  }

  for (int i = 0; i < bm->totvert; i++) {
    fprintf(fout, "vt %f %f\n", uvs[i].uv[0], uvs[i].uv[1]);
  }

  for (int i = 0; i < bm->totvert; i++) {
    fprintf(fout, "vn %f %f %f\n", verts[i].no[0], verts[i].no[1], verts[i].no[2]);
  }

  for (int i = 0; i < bm->totface; i++) {
    fprintf(fout, "f ");
    for (int j = 0; j < faces[i].len - 1; j++) {
      fprintf(fout, "%d/%d/%d ", faces[i].vi[j], faces[i].uv[j], faces[i].vi[j]);
    }
    fprintf(fout, "\n");
  }

  for (int i = 0; i < bm->totface; i++) {
    if (faces[i].vi) {
      MEM_freeN(faces[i].vi);
    }
    if (faces[i].uv) {
      MEM_freeN(faces[i].uv);
    }
  }

  MEM_freeN(faces);
  MEM_freeN(uvs);
  MEM_freeN(verts);

  fclose(fout);
  printf("File %s written\n", file_name);
}

#define EPSILON_CLOTH 0.01
static ClothVertex *cloth_remeshing_find_cloth_vertex(BMVert *v, ClothVertex *verts, int vert_num)
{
  ClothVertex *cv = NULL;

  for (int i = 0; i < vert_num; i++) {
    /* if (equals_v3v3(v->co, verts[i].xold)) { */
    if (fabs(v->co[0] - verts[i].x[0]) < EPSILON_CLOTH &&
        fabs(v->co[1] - verts[i].x[1]) < EPSILON_CLOTH &&
        fabs(v->co[2] - verts[i].x[2]) < EPSILON_CLOTH) {
      cv = &verts[i];
      break;
    }
  }

  return cv;
}

static int cloth_remeshing_find_cloth_vertex_index(BMVert *v, ClothVertex *verts, int vert_num)
{
  for (int i = 0; i < vert_num; i++) {
    /* if (equals_v3v3(v->co, verts[i].xold)) { */
    if (fabs(v->co[0] - verts[i].x[0]) < EPSILON_CLOTH &&
        fabs(v->co[1] - verts[i].x[1]) < EPSILON_CLOTH &&
        fabs(v->co[2] - verts[i].x[2]) < EPSILON_CLOTH) {
      return i;
    }
  }
  return -1;
}

static void cloth_remeshing_print_all_verts(ClothVertex *verts, int vert_num)
{
  for (int i = 0; i < vert_num; i++) {
    printf("%f %f %f\n", verts[i].xold[0], verts[i].xold[1], verts[i].xold[2]);
  }
}

static BMEdge *cloth_remeshing_find_next_loose_edge(BMVert *v)
{
  BMEdge *e;
  BMIter eiter;

  BM_ITER_ELEM (e, &eiter, v, BM_EDGES_OF_VERT) {
    if (BM_edge_face_count(e) == 0) {
      return e;
    }
  }

  return NULL;
}

static void cloth_remeshing_fix_sewing_verts(BMesh *bm, BMVert *new_vert, BMEdge *edge)
{
  BMEdge *next_loose_edge = NULL;
  BMVert *vert;
  edge->v1 == new_vert ? vert = edge->v2 : vert = edge->v1;
  next_loose_edge = cloth_remeshing_find_next_loose_edge(vert);
  BLI_assert(next_loose_edge != NULL);

  /* TODO(Ish): this works only if the loose edges are not subdivided,
   * need to figure out how to add this to a loop so that the last vertex
   * of the loose edge is found */
  BMVert *other_loose_vert;
  next_loose_edge->v1 == vert ? other_loose_vert = next_loose_edge->v2 :
                                other_loose_vert = next_loose_edge->v1;

  /* Now need to find an edge that can form a loop with 2 edges being
   * loose edges
   *
   *   d   c
   *   *---*
   *       |
   *     *-*
   *     a b
   *
   * abcd forms the loop with loose edges
   * a-> new_vert
   * b-> vert
   * c-> other_loose_vert
   * d-> ?
   **/

  BMEdge *other_loose_edge;
  BMVert *other_vert_e;
  BMEdge *e;
  BMIter eiter;
  bool found = false;
  BM_ITER_ELEM (e, &eiter, other_loose_vert, BM_EDGES_OF_VERT) {
    if (e == next_loose_edge) {
      continue;
    }

    e->v1 == other_loose_vert ? other_vert_e = e->v2 : other_vert_e = e->v1;

    other_loose_edge = cloth_remeshing_find_next_loose_edge(other_vert_e);
    if (!other_loose_edge) {
      continue;
    }
  }

  BLI_assert(found == true);
  BMVert *a = new_vert;
  BMVert *b = vert;
  BMVert *c = other_loose_vert;
  BMVert *d = other_vert_e;
}

static bool cloth_remeshing_split_edges(ClothModifierData *clmd, vector<ClothSizing> &sizing)
{
  BMesh *bm = clmd->clothObject->bm;
  static int prev_num_bad_edges = 0;
  vector<BMEdge *> bad_edges;
  cloth_remeshing_find_bad_edges(bm, sizing, bad_edges);
  printf("split edges tagged: %d\n", (int)bad_edges.size());
  if (bad_edges.size() == 0 || bad_edges.size() == prev_num_bad_edges) {
    return false;
  }
  prev_num_bad_edges = bad_edges.size();
  Cloth *cloth = clmd->clothObject;
  cloth->verts = (ClothVertex *)MEM_reallocN(
      cloth->verts, (cloth->mvert_num + bad_edges.size()) * sizeof(ClothVertex));
  BMEdge *e;
  for (int i = 0; i < bad_edges.size(); i++) {
    e = bad_edges[i];
    BMEdge old_edge = *e;
    BMVert *new_vert = cloth_remeshing_split_edge_keep_triangles(bm, e, e->v1, 0.5);
    BM_elem_flag_enable(new_vert, BM_ELEM_SELECT);
    BLI_assert(cloth->verts != NULL);
    ClothVertex *v1, *v2;
    v1 = cloth_remeshing_find_cloth_vertex(old_edge.v1, cloth->verts, cloth->mvert_num);
    v2 = cloth_remeshing_find_cloth_vertex(old_edge.v2, cloth->verts, cloth->mvert_num);
#if 0
    printf("v1: %f %f %f\n", old_edge.v1->co[0], old_edge.v1->co[1], old_edge.v1->co[2]);
    cloth_remeshing_print_all_verts(cloth->verts, cloth->mvert_num);
    printf("v2: %f %f %f\n", old_edge.v2->co[0], old_edge.v2->co[1], old_edge.v2->co[2]);
    cloth_remeshing_print_all_verts(cloth->verts, cloth->mvert_num);
#endif
    BLI_assert(v1 != NULL);
    BLI_assert(v2 != NULL);
    cloth->mvert_num += 1;
    cloth->verts[cloth->mvert_num - 1] = cloth_remeshing_mean_cloth_vert(v1, v2);
    if (clmd->sim_parms->flags & CLOTH_SIMSETTINGS_FLAG_SEW) {
      if (cloth_remeshing_find_next_loose_edge(old_edge.v1) != NULL &&
          cloth_remeshing_find_next_loose_edge(old_edge.v2) != NULL) {
        cloth_remeshing_fix_sewing_verts(bm, new_vert, &old_edge);
      }
    }
  }
  bad_edges.clear();
  return true;
}

static void cloth_remeshing_uv_of_vert_in_face(BMesh *bm, BMFace *f, BMVert *v, float r_uv[2])
{
  BLI_assert(BM_vert_in_face(v, f));

  const int cd_loop_uv_offset = CustomData_get_offset(&bm->ldata, CD_MLOOPUV);
  BMLoop *l = BM_face_vert_share_loop(f, v);
  MLoopUV *luv = (MLoopUV *)BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
  copy_v2_v2(r_uv, luv->uv);
}

static pair<BMFace *, BMFace *> cloth_remeshing_find_match(BMesh *bm,
                                                           pair<BMFace *, BMFace *> &face_pair_01,
                                                           pair<BMFace *, BMFace *> &face_pair_02,
                                                           BMVert *vert)
{
  for (int i = 0; i < 2; i++) {
    for (int j = 0; j < 2; j++) {
      BMFace *f1 = i == 0 ? face_pair_01.first : face_pair_01.second;
      BMFace *f2 = j == 0 ? face_pair_02.first : face_pair_02.second;

      if (f1 && f2) {
        float uv_01[2], uv_02[2];
        cloth_remeshing_uv_of_vert_in_face(bm, f1, vert, uv_01);
        cloth_remeshing_uv_of_vert_in_face(bm, f2, vert, uv_02);
        if (equals_v2v2(uv_01, uv_02)) {
          return make_pair(f1, f2);
        }
      }
    }
  }
  return make_pair((BMFace *)NULL, (BMFace *)NULL);
}

static float cloth_remeshing_edge_size_with_vert(
    BMesh *bm, BMVert *v1, BMVert *v2, BMVert *v, vector<ClothSizing> &sizing)
{
  BMFace *f1, *f2;
  BM_edge_face_pair(BM_edge_exists(v1, v), &f1, &f2);
  pair<BMFace *, BMFace *> face_pair_01 = make_pair(f1, f2);
  BM_edge_face_pair(BM_edge_exists(v2, v), &f1, &f2);
  pair<BMFace *, BMFace *> face_pair_02 = make_pair(f1, f2);

  pair<BMFace *, BMFace *> face_pair = cloth_remeshing_find_match(
      bm, face_pair_01, face_pair_02, v);
  if (!face_pair.first || !face_pair.second) {
    /* TODO(Ish): need to figure out when the face_pair are NULL */
    return 100.0f;
  }
  float uv_01[2], uv_02[2];
  cloth_remeshing_uv_of_vert_in_face(bm, face_pair.first, v1, uv_01);
  cloth_remeshing_uv_of_vert_in_face(bm, face_pair.second, v2, uv_02);

  /* TODO(Ish): Need to fix this for when sizing is fixed */
  float value = 0.0;
  float temp_v2[2];
  float u12[2];
  copy_v2_v2(u12, uv_01);
  sub_v2_v2(u12, uv_02);
  ClothSizing sizing_temp = sizing[0];
  mul_v2_m2v2(temp_v2, sizing_temp.m, u12);
  value += dot_v2v2(u12, temp_v2);

  return sqrtf(fmax(value, 0.0f));
}

static float cloth_remeshing_edge_size_with_vert(BMesh *bm,
                                                 BMEdge *e,
                                                 BMVert *v,
                                                 vector<ClothSizing> &sizing)
{
  return cloth_remeshing_edge_size_with_vert(bm, e->v1, e->v2, v, sizing);
}

static void cloth_remeshing_replace_uvs(float uv_01[2], float uv_02[2], float uvs[3][2])
{
  for (int i = 0; i < 3; i++) {
    if (equals_v2v2(uv_01, uvs[i])) {
      copy_v2_v2(uvs[i], uv_02);
      break;
    }
  }
}

static inline float cloth_remeshing_wedge(float v_01[2], float v_02[2])
{
  return v_01[0] * v_02[1] - v_01[1] * v_02[0];
}

#define SQRT3 1.732050808f

static bool cloth_remeshing_aspect_ratio(ClothModifierData *clmd, BMesh *bm, BMEdge *e)
{
  BMFace *f1, *f2;
  BM_edge_face_pair(e, &f1, &f2);

  BMFace *f;
  BMIter fiter;
  BM_ITER_ELEM (f, &fiter, e->v1, BM_FACES_OF_VERT) {
    if (BM_vert_in_face(e->v2, f)) { /* This might be wrong */
      continue;
    }
    float uvs[3][2];
    BMVert *v;
    BMIter viter;
    int i = 0;
    BM_ITER_ELEM_INDEX (v, &viter, f, BM_VERTS_OF_FACE, i) {
      cloth_remeshing_uv_of_vert_in_face(bm, f, v, uvs[i]);
    }
    if (f1) {
      float uv_01[2], uv_02[2];
      cloth_remeshing_uv_of_vert_in_face(bm, f1, e->v1, uv_01);
      cloth_remeshing_uv_of_vert_in_face(bm, f1, e->v2, uv_02);
      cloth_remeshing_replace_uvs(uv_01, uv_02, uvs);
    }
    if (f2) {
      float uv_01[2], uv_02[2];
      cloth_remeshing_uv_of_vert_in_face(bm, f2, e->v1, uv_01);
      cloth_remeshing_uv_of_vert_in_face(bm, f2, e->v2, uv_02);
      cloth_remeshing_replace_uvs(uv_01, uv_02, uvs);
    }

    float temp_01[2], temp_02[2], temp_03[2];
    copy_v2_v2(temp_01, uvs[1]);
    sub_v2_v2(temp_01, uvs[0]);
    copy_v2_v2(temp_02, uvs[2]);
    sub_v2_v2(temp_02, uvs[0]);
    copy_v2_v2(temp_03, uvs[1]);
    sub_v2_v2(temp_03, uvs[2]);
    float a = cloth_remeshing_wedge(temp_01, temp_02) * 0.5f;
    float p = len_v2(temp_01) + len_v2(temp_02) + len_v3(temp_03); /* This might be wrong */
    float aspect = 12.0f * SQRT3 * a / (p * p);
    if (a < 1e-6 || aspect < clmd->sim_parms->aspect_min) {
      return false;
    }
  }
  return true;
}

#define REMESHING_HYSTERESIS_PARAMETER 0.2
static bool cloth_remeshing_can_collapse_edge(ClothModifierData *clmd,
                                              BMesh *bm,
                                              BMEdge *e,
                                              vector<ClothSizing> &sizing)
{
  if (BM_edge_face_count(e) < 2) {
    return false;
  }

  /* aspect ratio parameter */
  if (!cloth_remeshing_aspect_ratio(clmd, bm, e)) {
    return false;
  }

  BMFace *f1, *f2;
  BM_edge_face_pair(e, &f1, &f2);
#if 1
  /* TODO(Ish): This was a hack, figure out why it doesn't give the
   * fair pair even though face count is 2 or more
   * It might be fixed if the UV seams if fixed */
  if (!f1 || !f2) {
    return false;
  }
#endif
  BMVert *v_01 = BM_face_other_vert_loop(f1, e->v1, e->v2)->v;
  float size_01 = cloth_remeshing_edge_size_with_vert(bm, e, v_01, sizing);
  if (size_01 > (1.0f - REMESHING_HYSTERESIS_PARAMETER)) {
    return false;
  }
  BMVert *v_02 = BM_face_other_vert_loop(f2, e->v1, e->v2)->v;
  float size_02 = cloth_remeshing_edge_size_with_vert(bm, e, v_02, sizing);
  if (size_02 > (1.0f - REMESHING_HYSTERESIS_PARAMETER)) {
    return false;
  }

  return true;
}

static void cloth_remeshing_remove_vertex_from_cloth(Cloth *cloth, BMVert *v)
{
  int v_index = cloth_remeshing_find_cloth_vertex_index(v, cloth->verts, cloth->mvert_num);
  cloth->verts[v_index] = cloth->verts[cloth->mvert_num - 1];
#if 0
  printf("removed: %f %f %f\n",
         cloth->verts[cloth->mvert_num - 1].x[0],
         cloth->verts[cloth->mvert_num - 1].x[1],
         cloth->verts[cloth->mvert_num - 1].x[2]);
#endif
  cloth->mvert_num--;
}

static bool cloth_remeshing_boundary_test(BMEdge *e)
{
  BMFace *f1, *f2;
  BM_edge_face_pair(e, &f1, &f2);
  return !f1 || !f2;
}

static bool cloth_remeshing_boundary_test(BMVert *v)
{
  BMFace *f;
  BMEdge *e;
  BMIter iter;
  int face_count = 0, vert_count = 0;

  BM_ITER_ELEM (f, &iter, v, BM_FACES_OF_VERT) {
    face_count++;
  }

  BM_ITER_ELEM (e, &iter, v, BM_EDGES_OF_VERT) {
    if (e->v1 != v || e->v2 != v) {
      vert_count++;
    }
  }

  return face_count != vert_count;
}

static bool cloth_remeshing_edge_on_seam_test(BMesh *bm, BMEdge *e)
{
  BMFace *f1, *f2;
  BM_edge_face_pair(e, &f1, &f2);
  if (!f1 || !f2) {
    return false;
  }
  float uv_f1_v1[2], uv_f1_v2[2], uv_f2_v1[2], uv_f2_v2[2];
  cloth_remeshing_uv_of_vert_in_face(bm, f1, e->v1, uv_f1_v1);
  cloth_remeshing_uv_of_vert_in_face(bm, f1, e->v2, uv_f1_v2);
  cloth_remeshing_uv_of_vert_in_face(bm, f2, e->v1, uv_f2_v1);
  cloth_remeshing_uv_of_vert_in_face(bm, f2, e->v2, uv_f2_v2);

  return (!equals_v2v2(uv_f1_v1, uv_f2_v1) || !equals_v2v2(uv_f1_v2, uv_f2_v2));
}

static bool cloth_remeshing_vert_on_seam_test(BMesh *bm, BMVert *v)
{
  BMEdge *e;
  BMIter eiter;
  BM_ITER_ELEM (e, &eiter, v, BM_EDGES_OF_VERT) {
    if (cloth_remeshing_edge_on_seam_test(bm, e)) {
      return true;
    }
  }
  return false;
}

static BMVert *cloth_remeshing_collapse_edge(Cloth *cloth, BMesh *bm, BMEdge *e)
{
  BMVert v1 = *e->v1;
  BMVert *v2 = BM_edge_collapse(bm, e, e->v1, true, true);

  cloth_remeshing_remove_vertex_from_cloth(cloth, &v1);
  return v2;
}

static BMVert *cloth_remeshing_try_edge_collapse(ClothModifierData *clmd,
                                                 BMEdge *e,
                                                 vector<ClothSizing> &sizing)
{
  Cloth *cloth = clmd->clothObject;
  BMesh *bm = cloth->bm;

  if (cloth_remeshing_boundary_test(e->v1) && !cloth_remeshing_boundary_test(e)) {
    return NULL;
  }

  if (cloth_remeshing_vert_on_seam_test(bm, e->v1) && !cloth_remeshing_edge_on_seam_test(bm, e)) {
    return NULL;
  }

  if (!cloth_remeshing_can_collapse_edge(clmd, bm, e, sizing)) {
    return NULL;
  }

  return cloth_remeshing_collapse_edge(cloth, bm, e);
}

static void cloth_remeshing_remove_face(vector<BMFace *> &faces, int index)
{
  faces[index] = faces[faces.size() - 1];
  faces.pop_back();
}

static void cloth_remeshing_update_active_faces(vector<BMFace *> &active_faces,
                                                BMesh *bm,
                                                BMEdge *e)
{
  BMFace *f, *f2;
  BMIter fiter;
  vector<BMFace *> new_active_faces;
  /* add the newly created faces, all those that have that vertex v */
  BM_ITER_ELEM (f, &fiter, e, BM_FACES_OF_EDGE) {
    new_active_faces.push_back(f);
  }

  /* remove the faces from active_faces that have been removed from
   * bmesh */
  for (int i = 0; i < active_faces.size(); i++) {
    bool already_exists = false;
    f = active_faces[i];
    for (int j = 0; j < new_active_faces.size(); j++) {
      if (f == new_active_faces[j]) {
        already_exists = true;
        break;
      }
    }
    if (already_exists) {
      break;
    }
    BM_ITER_MESH (f2, &fiter, bm, BM_FACES_OF_MESH) {
      if (f == f2) {
        new_active_faces.push_back(f);
        break;
      }
    }
  }

  active_faces.swap(new_active_faces);
  new_active_faces.clear();
}

static void cloth_remeshing_update_active_faces(vector<BMFace *> &active_faces,
                                                BMesh *bm,
                                                BMVert *v)
{
  BMFace *f, *f2;
  BMIter fiter;
  vector<BMFace *> new_active_faces;
  /* add the newly created faces, all those that have that vertex v */
  BM_ITER_ELEM (f, &fiter, v, BM_FACES_OF_VERT) {
    new_active_faces.push_back(f);
  }

  /* remove the faces from active_faces that have been removed from
   * bmesh */
  for (int i = 0; i < active_faces.size(); i++) {
    bool already_exists = false;
    f = active_faces[i];
    for (int j = 0; j < new_active_faces.size(); j++) {
      if (f == new_active_faces[j]) {
        already_exists = true;
        break;
      }
    }
    if (already_exists) {
      break;
    }
    BM_ITER_MESH (f2, &fiter, bm, BM_FACES_OF_MESH) {
      if (f == f2) {
        new_active_faces.push_back(f);
        break;
      }
    }
  }

  active_faces.swap(new_active_faces);
  new_active_faces.clear();
}

int count;

static bool cloth_remeshing_collapse_edges(ClothModifierData *clmd,
                                           vector<ClothSizing> &sizing,
                                           vector<BMFace *> &active_faces)
{
  for (int i = 0; i < active_faces.size(); i++) {
    BMFace *f = active_faces[i];
    BMEdge *e;
    BMIter eiter;
    BM_ITER_ELEM (e, &eiter, f, BM_EDGES_OF_FACE) {
      BMVert *v1 = e->v1, *v2 = e->v2;

      BMVert *temp_vert;
      temp_vert = cloth_remeshing_try_edge_collapse(clmd, e, sizing);
      if (!temp_vert) {
        BM_edge_verts_swap(e);
        temp_vert = cloth_remeshing_try_edge_collapse(clmd, e, sizing);
        BM_edge_verts_swap(e);
        if (!temp_vert) {
          continue;
        }
      }

      /* update active_faces */
      cloth_remeshing_update_active_faces(active_faces, clmd->clothObject->bm, temp_vert);

      /* run cloth_remeshing_fix_mesh on newly created faces by
       * cloth_remeshing_try_edge_collapse */
      cloth_remeshing_fix_mesh(clmd->clothObject->bm, sizing, active_faces);

      /* update active_faces */

      count++;
      return true;
    }
    cloth_remeshing_remove_face(active_faces, i--);
  }
  return false;
}

static void cloth_remeshing_reindex_cloth_vertices(Cloth *cloth, BMesh *bm)
{
  ClothVertex *new_verts = (ClothVertex *)MEM_mallocN(sizeof(ClothVertex) * cloth->mvert_num,
                                                      "New ClothVertex Array");
  BMVert *v;
  BMIter viter;
  int i = 0;
  BM_ITER_MESH_INDEX (v, &viter, bm, BM_VERTS_OF_MESH, i) {
    new_verts[i] = *cloth_remeshing_find_cloth_vertex(v, cloth->verts, cloth->mvert_num);
  }

  MEM_freeN(cloth->verts);
  cloth->verts = new_verts;
}

static void cloth_remeshing_static(ClothModifierData *clmd)
{
  /**
   * sizing indices match vertex indices
   * while appending new verticies ensure sizing is added at same index
   */
  vector<ClothSizing> sizing;
  int numVerts = clmd->clothObject->bm->totvert;

  /**
   * Define sizing staticly
   */
  for (int i = 0; i < numVerts; i++) {
    ClothSizing size;
    unit_m2(size.m);
    mul_m2_fl(size.m, 1.0f / clmd->sim_parms->size_min);
    sizing.push_back(size);
  }

  /**
   * Split edges
   */
  while (cloth_remeshing_split_edges(clmd, sizing)) {
    /* empty while */
  }

  /**
   * Collapse edges
   */

#if 1
  vector<BMFace *> active_faces;
  BMFace *f;
  BMIter fiter;
  BM_ITER_MESH (f, &fiter, clmd->clothObject->bm, BM_FACES_OF_MESH) {
    active_faces.push_back(f);
  }
  for (int i = 0; i < active_faces.size(); i++) {
    BMFace *temp_f = active_faces[i];
    if (!(temp_f->head.htype == BM_FACE)) {
      printf("htype didn't match: %d\n", i);
    }
  }
  int prev_mvert_num = clmd->clothObject->mvert_num;
  count = 0;
  while (cloth_remeshing_collapse_edges(clmd, sizing, active_faces)) {
    /* empty while */
  }
  printf("collapse edges count: %d\n", count);
  /* delete excess vertices after collpase edges */
  if (prev_mvert_num != clmd->clothObject->mvert_num) {
    clmd->clothObject->verts = (ClothVertex *)MEM_reallocN(
        clmd->clothObject->verts, sizeof(ClothVertex) * clmd->clothObject->mvert_num);
  }

#endif

  /**
   * Reindex clmd->clothObject->verts to match clmd->clothObject->bm
   */

#if 1
  cloth_remeshing_reindex_cloth_vertices(clmd->clothObject, clmd->clothObject->bm);
#endif

  /**
   * Delete sizing
   */
  sizing.clear();
#if 1
  active_faces.clear();
#endif

#if 1
  static int file_no = 0;
  file_no++;
  char file_name[100];
  sprintf(file_name, "/tmp/objs/%03d.obj", file_no);
  cloth_remeshing_export_obj(clmd->clothObject->bm, file_name);
#endif
}

Mesh *cloth_remeshing_step(Object *ob, ClothModifierData *clmd, Mesh *mesh)
{
  cloth_remeshing_init_bmesh(ob, clmd, mesh);

  cloth_remeshing_static(clmd);

  return cloth_remeshing_update_cloth_object_bmesh(ob, clmd);
}