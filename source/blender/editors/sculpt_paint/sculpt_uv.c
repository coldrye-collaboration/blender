/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2002-2009 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup edsculpt
 * UV Sculpt tools.
 */

#include "MEM_guardedalloc.h"

#include "BLI_ghash.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "DNA_brush_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_brush.h"
#include "BKE_colortools.h"
#include "BKE_context.h"
#include "BKE_customdata.h"
#include "BKE_editmesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_paint.h"

#include "DEG_depsgraph.h"

#include "ED_image.h"
#include "ED_mesh.h"
#include "ED_screen.h"

#include "WM_api.h"
#include "WM_types.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "paint_intern.h"
#include "uvedit_intern.h"

#include "UI_view2d.h"

#define MARK_BOUNDARY 1

typedef struct UvAdjacencyElement {
  /* pointer to original uvelement */
  UvElement *element;
  /* uv pointer for convenience. Caution, this points to the original UVs! */
  float *uv;
  /* general use flag (Used to check if Element is boundary here) */
  char flag;
} UvAdjacencyElement;

typedef struct UvEdge {
  uint uv1;
  uint uv2;
  /* general use flag
   * (Used to check if edge is boundary here, and propagates to adjacency elements) */
  char flag;
} UvEdge;

typedef struct UVInitialStrokeElement {
  /* index to unique uv */
  int uv;

  /* strength of brush on initial position */
  float strength;

  /* initial uv position */
  float initial_uv[2];
} UVInitialStrokeElement;

typedef struct UVInitialStroke {
  /* Initial Selection,for grab brushes for instance */
  UVInitialStrokeElement *initialSelection;

  /* Total initially selected UV's. */
  int totalInitialSelected;

  /* initial mouse coordinates */
  float init_coord[2];
} UVInitialStroke;

/* custom data for uv smoothing brush */
typedef struct UvSculptData {
  /* Contains the first of each set of coincident UV's.
   * These will be used to perform smoothing on and propagate the changes
   * to their coincident UV's */
  UvAdjacencyElement *uv;

  /* ...Is what it says */
  int totalUniqueUvs;

  /* Edges used for adjacency info, used with laplacian smoothing */
  UvEdge *uvedges;

  /* need I say more? */
  int totalUvEdges;

  /* data for initial stroke, used by tools like grab */
  UVInitialStroke *initial_stroke;

  /* timer to be used for airbrush-type brush */
  wmTimer *timer;

  /* to determine quickly adjacent UV's */
  UvElementMap *elementMap;

  /* uvsmooth Paint for fast reference */
  Paint *uvsculpt;

  /* tool to use. duplicating here to change if modifier keys are pressed */
  char tool;

  /* store invert flag here */
  char invert;
} UvSculptData;

/*********** Improved Laplacian Relaxation Operator ************************/
/* original code by Raul Fernandez Hernandez "farsthary"                   *
 * adapted to uv smoothing by Antony Riakiatakis                           *
 ***************************************************************************/

typedef struct Temp_UvData {
  float sum_co[2], p[2], b[2], sum_b[2];
  int ncounter;
} Temp_UVData;

static void HC_relaxation_iteration_uv(BMEditMesh *em,
                                       UvSculptData *sculptdata,
                                       const float mouse_coord[2],
                                       float alpha,
                                       float radius,
                                       float aspectRatio)
{
  Temp_UVData *tmp_uvdata;
  float diff[2];
  int i;
  float radius_root = sqrtf(radius);
  Brush *brush = BKE_paint_brush(sculptdata->uvsculpt);

  tmp_uvdata = (Temp_UVData *)MEM_callocN(sculptdata->totalUniqueUvs * sizeof(Temp_UVData),
                                          "Temporal data");

  /* counting neighbors */
  for (i = 0; i < sculptdata->totalUvEdges; i++) {
    UvEdge *tmpedge = sculptdata->uvedges + i;
    tmp_uvdata[tmpedge->uv1].ncounter++;
    tmp_uvdata[tmpedge->uv2].ncounter++;

    add_v2_v2(tmp_uvdata[tmpedge->uv2].sum_co, sculptdata->uv[tmpedge->uv1].uv);
    add_v2_v2(tmp_uvdata[tmpedge->uv1].sum_co, sculptdata->uv[tmpedge->uv2].uv);
  }

  for (i = 0; i < sculptdata->totalUniqueUvs; i++) {
    copy_v2_v2(diff, tmp_uvdata[i].sum_co);
    mul_v2_fl(diff, 1.0f / tmp_uvdata[i].ncounter);
    copy_v2_v2(tmp_uvdata[i].p, diff);

    tmp_uvdata[i].b[0] = diff[0] - sculptdata->uv[i].uv[0];
    tmp_uvdata[i].b[1] = diff[1] - sculptdata->uv[i].uv[1];
  }

  for (i = 0; i < sculptdata->totalUvEdges; i++) {
    UvEdge *tmpedge = sculptdata->uvedges + i;
    add_v2_v2(tmp_uvdata[tmpedge->uv1].sum_b, tmp_uvdata[tmpedge->uv2].b);
    add_v2_v2(tmp_uvdata[tmpedge->uv2].sum_b, tmp_uvdata[tmpedge->uv1].b);
  }

  for (i = 0; i < sculptdata->totalUniqueUvs; i++) {
    float dist;
    /* This is supposed to happen only if "Pin Edges" is on,
     * since we have initialization on stroke start.
     * If ever uv brushes get their own mode we should check for toolsettings option too. */
    if (sculptdata->uv[i].flag & MARK_BOUNDARY) {
      continue;
    }

    sub_v2_v2v2(diff, sculptdata->uv[i].uv, mouse_coord);
    diff[1] /= aspectRatio;
    if ((dist = dot_v2v2(diff, diff)) <= radius) {
      UvElement *element;
      float strength;
      strength = alpha * BKE_brush_curve_strength_clamped(brush, sqrtf(dist), radius_root);

      sculptdata->uv[i].uv[0] = (1.0f - strength) * sculptdata->uv[i].uv[0] +
                                strength *
                                    (tmp_uvdata[i].p[0] -
                                     0.5f * (tmp_uvdata[i].b[0] +
                                             tmp_uvdata[i].sum_b[0] / tmp_uvdata[i].ncounter));
      sculptdata->uv[i].uv[1] = (1.0f - strength) * sculptdata->uv[i].uv[1] +
                                strength *
                                    (tmp_uvdata[i].p[1] -
                                     0.5f * (tmp_uvdata[i].b[1] +
                                             tmp_uvdata[i].sum_b[1] / tmp_uvdata[i].ncounter));

      for (element = sculptdata->uv[i].element; element; element = element->next) {
        MLoopUV *luv;
        BMLoop *l;

        if (element->separate && element != sculptdata->uv[i].element) {
          break;
        }

        l = element->l;
        luv = CustomData_bmesh_get(&em->bm->ldata, l->head.data, CD_MLOOPUV);
        copy_v2_v2(luv->uv, sculptdata->uv[i].uv);
      }
    }
  }

  MEM_SAFE_FREE(tmp_uvdata);
}

static void laplacian_relaxation_iteration_uv(BMEditMesh *em,
                                              UvSculptData *sculptdata,
                                              const float mouse_coord[2],
                                              float alpha,
                                              float radius,
                                              float aspectRatio)
{
  Temp_UVData *tmp_uvdata;
  float diff[2];
  int i;
  float radius_root = sqrtf(radius);
  Brush *brush = BKE_paint_brush(sculptdata->uvsculpt);

  tmp_uvdata = (Temp_UVData *)MEM_callocN(sculptdata->totalUniqueUvs * sizeof(Temp_UVData),
                                          "Temporal data");

  /* counting neighbors */
  for (i = 0; i < sculptdata->totalUvEdges; i++) {
    UvEdge *tmpedge = sculptdata->uvedges + i;
    tmp_uvdata[tmpedge->uv1].ncounter++;
    tmp_uvdata[tmpedge->uv2].ncounter++;

    add_v2_v2(tmp_uvdata[tmpedge->uv2].sum_co, sculptdata->uv[tmpedge->uv1].uv);
    add_v2_v2(tmp_uvdata[tmpedge->uv1].sum_co, sculptdata->uv[tmpedge->uv2].uv);
  }

  /* Original Laplacian algorithm included removal of normal component of translation.
   * here it is not needed since we translate along the UV plane always. */
  for (i = 0; i < sculptdata->totalUniqueUvs; i++) {
    copy_v2_v2(tmp_uvdata[i].p, tmp_uvdata[i].sum_co);
    mul_v2_fl(tmp_uvdata[i].p, 1.0f / tmp_uvdata[i].ncounter);
  }

  for (i = 0; i < sculptdata->totalUniqueUvs; i++) {
    float dist;
    /* This is supposed to happen only if "Pin Edges" is on,
     * since we have initialization on stroke start.
     * If ever uv brushes get their own mode we should check for toolsettings option too. */
    if (sculptdata->uv[i].flag & MARK_BOUNDARY) {
      continue;
    }

    sub_v2_v2v2(diff, sculptdata->uv[i].uv, mouse_coord);
    diff[1] /= aspectRatio;
    if ((dist = dot_v2v2(diff, diff)) <= radius) {
      UvElement *element;
      float strength;
      strength = alpha * BKE_brush_curve_strength_clamped(brush, sqrtf(dist), radius_root);

      sculptdata->uv[i].uv[0] = (1.0f - strength) * sculptdata->uv[i].uv[0] +
                                strength * tmp_uvdata[i].p[0];
      sculptdata->uv[i].uv[1] = (1.0f - strength) * sculptdata->uv[i].uv[1] +
                                strength * tmp_uvdata[i].p[1];

      for (element = sculptdata->uv[i].element; element; element = element->next) {
        MLoopUV *luv;
        BMLoop *l;

        if (element->separate && element != sculptdata->uv[i].element) {
          break;
        }

        l = element->l;
        luv = CustomData_bmesh_get(&em->bm->ldata, l->head.data, CD_MLOOPUV);
        copy_v2_v2(luv->uv, sculptdata->uv[i].uv);
      }
    }
  }

  MEM_SAFE_FREE(tmp_uvdata);
}

static void uv_sculpt_stroke_apply(bContext *C,
                                   wmOperator *op,
                                   const wmEvent *event,
                                   Object *obedit)
{
  float co[2], radius, radius_root;
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  BMEditMesh *em = BKE_editmesh_from_object(obedit);
  uint tool;
  UvSculptData *sculptdata = (UvSculptData *)op->customdata;
  SpaceImage *sima;
  int invert;
  int width, height;
  float aspectRatio;
  float alpha, zoomx, zoomy;
  Brush *brush = BKE_paint_brush(sculptdata->uvsculpt);
  ToolSettings *toolsettings = CTX_data_tool_settings(C);
  tool = sculptdata->tool;
  invert = sculptdata->invert ? -1 : 1;
  alpha = BKE_brush_alpha_get(scene, brush);
  UI_view2d_region_to_view(&region->v2d, event->mval[0], event->mval[1], &co[0], &co[1]);

  sima = CTX_wm_space_image(C);
  ED_space_image_get_size(sima, &width, &height);
  ED_space_image_get_zoom(sima, region, &zoomx, &zoomy);

  radius = BKE_brush_size_get(scene, brush) / (width * zoomx);
  aspectRatio = width / (float)height;

  /* We will compare squares to save some computation */
  radius = radius * radius;
  radius_root = sqrtf(radius);

  /*
   * Pinch Tool
   */
  if (tool == UV_SCULPT_TOOL_PINCH) {
    int i;
    alpha *= invert;
    for (i = 0; i < sculptdata->totalUniqueUvs; i++) {
      float dist, diff[2];
      /* This is supposed to happen only if "Lock Borders" is on,
       * since we have initialization on stroke start.
       * If ever uv brushes get their own mode we should check for toolsettings option too. */
      if (sculptdata->uv[i].flag & MARK_BOUNDARY) {
        continue;
      }

      sub_v2_v2v2(diff, sculptdata->uv[i].uv, co);
      diff[1] /= aspectRatio;
      if ((dist = dot_v2v2(diff, diff)) <= radius) {
        UvElement *element;
        float strength;
        strength = alpha * BKE_brush_curve_strength_clamped(brush, sqrtf(dist), radius_root);
        normalize_v2(diff);

        sculptdata->uv[i].uv[0] -= strength * diff[0] * 0.001f;
        sculptdata->uv[i].uv[1] -= strength * diff[1] * 0.001f;

        for (element = sculptdata->uv[i].element; element; element = element->next) {
          MLoopUV *luv;
          BMLoop *l;

          if (element->separate && element != sculptdata->uv[i].element) {
            break;
          }

          l = element->l;
          luv = CustomData_bmesh_get(&em->bm->ldata, l->head.data, CD_MLOOPUV);
          copy_v2_v2(luv->uv, sculptdata->uv[i].uv);
        }
      }
    }
  }

  /*
   * Smooth Tool
   */
  else if (tool == UV_SCULPT_TOOL_RELAX) {
    uint method = toolsettings->uv_relax_method;
    if (method == UV_SCULPT_TOOL_RELAX_HC) {
      HC_relaxation_iteration_uv(em, sculptdata, co, alpha, radius, aspectRatio);
    }
    else {
      laplacian_relaxation_iteration_uv(em, sculptdata, co, alpha, radius, aspectRatio);
    }
  }

  /*
   * Grab Tool
   */
  else if (tool == UV_SCULPT_TOOL_GRAB) {
    int i;
    float diff[2];
    sub_v2_v2v2(diff, co, sculptdata->initial_stroke->init_coord);

    for (i = 0; i < sculptdata->initial_stroke->totalInitialSelected; i++) {
      UvElement *element;
      int uvindex = sculptdata->initial_stroke->initialSelection[i].uv;
      float strength = sculptdata->initial_stroke->initialSelection[i].strength;
      sculptdata->uv[uvindex].uv[0] =
          sculptdata->initial_stroke->initialSelection[i].initial_uv[0] + strength * diff[0];
      sculptdata->uv[uvindex].uv[1] =
          sculptdata->initial_stroke->initialSelection[i].initial_uv[1] + strength * diff[1];

      for (element = sculptdata->uv[uvindex].element; element; element = element->next) {
        MLoopUV *luv;
        BMLoop *l;

        if (element->separate && element != sculptdata->uv[uvindex].element) {
          break;
        }

        l = element->l;
        luv = CustomData_bmesh_get(&em->bm->ldata, l->head.data, CD_MLOOPUV);
        copy_v2_v2(luv->uv, sculptdata->uv[uvindex].uv);
      }
    }
  }
}

static void uv_sculpt_stroke_exit(bContext *C, wmOperator *op)
{
  UvSculptData *data = op->customdata;
  if (data->timer) {
    WM_event_remove_timer(CTX_wm_manager(C), CTX_wm_window(C), data->timer);
  }
  BM_uv_element_map_free(data->elementMap);
  data->elementMap = NULL;
  MEM_SAFE_FREE(data->uv);
  MEM_SAFE_FREE(data->uvedges);
  if (data->initial_stroke) {
    MEM_SAFE_FREE(data->initial_stroke->initialSelection);
    MEM_SAFE_FREE(data->initial_stroke);
  }

  MEM_SAFE_FREE(data);
  op->customdata = NULL;
}

static int uv_element_offset_from_face_get(
    UvElementMap *map, BMFace *efa, BMLoop *l, int island_index, const bool doIslands)
{
  UvElement *element = BM_uv_element_get(map, efa, l);
  if (!element || (doIslands && element->island != island_index)) {
    return -1;
  }
  return element - map->storage;
}

static uint uv_edge_hash(const void *key)
{
  const UvEdge *edge = key;
  return (BLI_ghashutil_uinthash(edge->uv2) + BLI_ghashutil_uinthash(edge->uv1));
}

static bool uv_edge_compare(const void *a, const void *b)
{
  const UvEdge *edge1 = a;
  const UvEdge *edge2 = b;

  if ((edge1->uv1 == edge2->uv1) && (edge1->uv2 == edge2->uv2)) {
    return false;
  }
  return true;
}

static UvSculptData *uv_sculpt_stroke_init(bContext *C, wmOperator *op, const wmEvent *event)
{
  Scene *scene = CTX_data_scene(C);
  Object *obedit = CTX_data_edit_object(C);
  ToolSettings *ts = scene->toolsettings;
  UvSculptData *data = MEM_callocN(sizeof(*data), "UV Smooth Brush Data");
  BMEditMesh *em = BKE_editmesh_from_object(obedit);
  BMesh *bm = em->bm;

  op->customdata = data;

  BKE_curvemapping_init(ts->uvsculpt->paint.brush->curve);

  if (data) {
    ARegion *region = CTX_wm_region(C);
    float co[2];
    BMFace *efa;
    MLoopUV *luv;
    BMLoop *l;
    BMIter iter, liter;

    UvEdge *edges;
    GHash *edgeHash;
    GHashIterator gh_iter;

    bool do_island_optimization = !(ts->uv_sculpt_settings & UV_SCULPT_ALL_ISLANDS);
    int island_index = 0;
    /* Holds, for each UvElement in elementMap, an index of its unique UV. */
    int *uniqueUv;
    data->tool = (RNA_enum_get(op->ptr, "mode") == BRUSH_STROKE_SMOOTH) ?
                     UV_SCULPT_TOOL_RELAX :
                     ts->uvsculpt->paint.brush->uv_sculpt_tool;
    data->invert = (RNA_enum_get(op->ptr, "mode") == BRUSH_STROKE_INVERT) ? 1 : 0;

    data->uvsculpt = &ts->uvsculpt->paint;

    /* Winding was added to island detection in 5197aa04c6bd
     * However the sculpt tools can flip faces, potentially creating orphaned islands.
     * See T100132 */
    bool use_winding = false;
    data->elementMap = BM_uv_element_map_create(
        bm, scene, false, use_winding, do_island_optimization);

    if (!data->elementMap) {
      uv_sculpt_stroke_exit(C, op);
      return NULL;
    }

    /* Mouse coordinates, useful for some functions like grab and sculpt all islands */
    UI_view2d_region_to_view(&region->v2d, event->mval[0], event->mval[1], &co[0], &co[1]);

    /* we need to find the active island here */
    if (do_island_optimization) {
      UvElement *element;
      UvNearestHit hit = UV_NEAREST_HIT_INIT_MAX(&region->v2d);
      uv_find_nearest_vert(scene, obedit, co, 0.0f, &hit);

      element = BM_uv_element_get(data->elementMap, hit.efa, hit.l);
      island_index = element->island;
    }

    /* Count 'unique' UV's */
    int unique_uvs = data->elementMap->total_unique_uvs;
    if (do_island_optimization) {
      unique_uvs = data->elementMap->island_total_unique_uvs[island_index];
    }

    /* Allocate the unique uv buffers */
    data->uv = MEM_mallocN(sizeof(*data->uv) * unique_uvs, "uv_brush_unique_uvs");
    uniqueUv = MEM_mallocN(sizeof(*uniqueUv) * data->elementMap->total_uvs,
                           "uv_brush_unique_uv_map");
    edgeHash = BLI_ghash_new(uv_edge_hash, uv_edge_compare, "uv_brush_edge_hash");
    /* we have at most totalUVs edges */
    edges = MEM_mallocN(sizeof(*edges) * data->elementMap->total_uvs, "uv_brush_all_edges");
    if (!data->uv || !uniqueUv || !edgeHash || !edges) {
      MEM_SAFE_FREE(edges);
      MEM_SAFE_FREE(uniqueUv);
      if (edgeHash) {
        BLI_ghash_free(edgeHash, NULL, NULL);
      }
      uv_sculpt_stroke_exit(C, op);
      return NULL;
    }

    data->totalUniqueUvs = unique_uvs;
    /* Index for the UvElements. */
    int counter = -1;
    /* initialize the unique UVs */
    for (int i = 0; i < bm->totvert; i++) {
      UvElement *element = data->elementMap->vertex[i];
      for (; element; element = element->next) {
        if (element->separate) {
          if (do_island_optimization && (element->island != island_index)) {
            /* skip this uv if not on the active island */
            for (; element->next && !(element->next->separate); element = element->next) {
              /* pass */
            }
            continue;
          }

          l = element->l;
          luv = CustomData_bmesh_get(&em->bm->ldata, l->head.data, CD_MLOOPUV);

          counter++;
          data->uv[counter].element = element;
          data->uv[counter].flag = 0;
          data->uv[counter].uv = luv->uv;
        }
        /* Pointer arithmetic to the rescue, as always :). */
        uniqueUv[element - data->elementMap->storage] = counter;
      }
    }
    BLI_assert(counter + 1 == unique_uvs);

    /* Now, on to generate our uv connectivity data */
    counter = 0;
    BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
      BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
        int offset1, itmp1 = uv_element_offset_from_face_get(
                         data->elementMap, efa, l, island_index, do_island_optimization);
        int offset2, itmp2 = uv_element_offset_from_face_get(
                         data->elementMap, efa, l->next, island_index, do_island_optimization);
        char *flag;

        /* Skip edge if not found(unlikely) or not on valid island */
        if (itmp1 == -1 || itmp2 == -1) {
          continue;
        }

        offset1 = uniqueUv[itmp1];
        offset2 = uniqueUv[itmp2];

        edges[counter].flag = 0;
        /* Using an order policy, sort UV's according to address space.
         * This avoids having two different UvEdges with the same UV's on different positions. */
        if (offset1 < offset2) {
          edges[counter].uv1 = offset1;
          edges[counter].uv2 = offset2;
        }
        else {
          edges[counter].uv1 = offset2;
          edges[counter].uv2 = offset1;
        }
        /* Hack! Set the value of the key to its flag.
         * Now we can set the flag when an edge exists twice :) */
        flag = BLI_ghash_lookup(edgeHash, &edges[counter]);
        if (flag) {
          *flag = 1;
        }
        else {
          /* Hack mentioned */
          BLI_ghash_insert(edgeHash, &edges[counter], &edges[counter].flag);
        }
        counter++;
      }
    }

    MEM_SAFE_FREE(uniqueUv);

    /* Allocate connectivity data, we allocate edges once */
    data->uvedges = MEM_mallocN(sizeof(*data->uvedges) * BLI_ghash_len(edgeHash),
                                "uv_brush_edge_connectivity_data");
    if (!data->uvedges) {
      BLI_ghash_free(edgeHash, NULL, NULL);
      MEM_SAFE_FREE(edges);
      uv_sculpt_stroke_exit(C, op);
      return NULL;
    }

    /* fill the edges with data */
    {
      int i = 0;
      GHASH_ITER (gh_iter, edgeHash) {
        data->uvedges[i++] = *((UvEdge *)BLI_ghashIterator_getKey(&gh_iter));
      }
      data->totalUvEdges = BLI_ghash_len(edgeHash);
    }

    /* cleanup temporary stuff */
    BLI_ghash_free(edgeHash, NULL, NULL);
    MEM_SAFE_FREE(edges);

    /* transfer boundary edge property to UV's */
    if (ts->uv_sculpt_settings & UV_SCULPT_LOCK_BORDERS) {
      for (int i = 0; i < data->totalUvEdges; i++) {
        if (!data->uvedges[i].flag) {
          data->uv[data->uvedges[i].uv1].flag |= MARK_BOUNDARY;
          data->uv[data->uvedges[i].uv2].flag |= MARK_BOUNDARY;
        }
      }
    }

    /* Allocate initial selection for grab tool */
    if (data->tool == UV_SCULPT_TOOL_GRAB) {
      float radius, radius_root;
      UvSculptData *sculptdata = (UvSculptData *)op->customdata;
      SpaceImage *sima;
      int width, height;
      float aspectRatio;
      float alpha, zoomx, zoomy;
      Brush *brush = BKE_paint_brush(sculptdata->uvsculpt);

      alpha = BKE_brush_alpha_get(scene, brush);

      radius = BKE_brush_size_get(scene, brush);
      sima = CTX_wm_space_image(C);
      ED_space_image_get_size(sima, &width, &height);
      ED_space_image_get_zoom(sima, region, &zoomx, &zoomy);

      aspectRatio = width / (float)height;
      radius /= (width * zoomx);
      radius = radius * radius;
      radius_root = sqrtf(radius);

      /* Allocate selection stack */
      data->initial_stroke = MEM_mallocN(sizeof(*data->initial_stroke),
                                         "uv_sculpt_initial_stroke");
      if (!data->initial_stroke) {
        uv_sculpt_stroke_exit(C, op);
      }
      data->initial_stroke->initialSelection = MEM_mallocN(
          sizeof(*data->initial_stroke->initialSelection) * data->totalUniqueUvs,
          "uv_sculpt_initial_selection");
      if (!data->initial_stroke->initialSelection) {
        uv_sculpt_stroke_exit(C, op);
      }

      copy_v2_v2(data->initial_stroke->init_coord, co);

      counter = 0;

      for (int i = 0; i < data->totalUniqueUvs; i++) {
        float dist, diff[2];
        if (data->uv[i].flag & MARK_BOUNDARY) {
          continue;
        }

        sub_v2_v2v2(diff, data->uv[i].uv, co);
        diff[1] /= aspectRatio;
        if ((dist = dot_v2v2(diff, diff)) <= radius) {
          float strength;
          strength = alpha * BKE_brush_curve_strength_clamped(brush, sqrtf(dist), radius_root);

          data->initial_stroke->initialSelection[counter].uv = i;
          data->initial_stroke->initialSelection[counter].strength = strength;
          copy_v2_v2(data->initial_stroke->initialSelection[counter].initial_uv, data->uv[i].uv);
          counter++;
        }
      }

      data->initial_stroke->totalInitialSelected = counter;
    }
  }

  return op->customdata;
}

static int uv_sculpt_stroke_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  UvSculptData *data;
  Object *obedit = CTX_data_edit_object(C);

  if (!(data = uv_sculpt_stroke_init(C, op, event))) {
    return OPERATOR_CANCELLED;
  }

  uv_sculpt_stroke_apply(C, op, event, obedit);

  data->timer = WM_event_add_timer(CTX_wm_manager(C), CTX_wm_window(C), TIMER, 0.001f);

  if (!data->timer) {
    uv_sculpt_stroke_exit(C, op);
    return OPERATOR_CANCELLED;
  }
  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static int uv_sculpt_stroke_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  UvSculptData *data = (UvSculptData *)op->customdata;
  Object *obedit = CTX_data_edit_object(C);

  switch (event->type) {
    case LEFTMOUSE:
    case MIDDLEMOUSE:
    case RIGHTMOUSE:
      uv_sculpt_stroke_exit(C, op);
      return OPERATOR_FINISHED;

    case MOUSEMOVE:
    case INBETWEEN_MOUSEMOVE:
      uv_sculpt_stroke_apply(C, op, event, obedit);
      break;
    case TIMER:
      if (event->customdata == data->timer) {
        uv_sculpt_stroke_apply(C, op, event, obedit);
      }
      break;
    default:
      return OPERATOR_RUNNING_MODAL;
  }

  ED_region_tag_redraw(CTX_wm_region(C));
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit->data);
  DEG_id_tag_update(obedit->data, 0);
  return OPERATOR_RUNNING_MODAL;
}

static bool uv_sculpt_stroke_poll(bContext *C)
{
  if (ED_operator_uvedit_space_image(C)) {
    /* While these values could be initialized on demand,
     * the only case this would be useful is running from the operator search popup.
     * This is such a corner case that it's simpler to check a brush has already been created
     * (something the tool system ensures). */
    Scene *scene = CTX_data_scene(C);
    ToolSettings *ts = scene->toolsettings;
    Brush *brush = BKE_paint_brush(&ts->uvsculpt->paint);
    if (brush != NULL) {
      return true;
    }
  }
  return false;
}

void SCULPT_OT_uv_sculpt_stroke(wmOperatorType *ot)
{
  static const EnumPropertyItem stroke_mode_items[] = {
      {BRUSH_STROKE_NORMAL, "NORMAL", 0, "Regular", "Apply brush normally"},
      {BRUSH_STROKE_INVERT,
       "INVERT",
       0,
       "Invert",
       "Invert action of brush for duration of stroke"},
      {BRUSH_STROKE_SMOOTH,
       "RELAX",
       0,
       "Relax",
       "Switch brush to relax mode for duration of stroke"},
      {0},
  };

  /* identifiers */
  ot->name = "Sculpt UVs";
  ot->description = "Sculpt UVs using a brush";
  ot->idname = "SCULPT_OT_uv_sculpt_stroke";

  /* api callbacks */
  ot->invoke = uv_sculpt_stroke_invoke;
  ot->modal = uv_sculpt_stroke_modal;
  ot->poll = uv_sculpt_stroke_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_enum(ot->srna, "mode", stroke_mode_items, BRUSH_STROKE_NORMAL, "Mode", "Stroke Mode");
}
