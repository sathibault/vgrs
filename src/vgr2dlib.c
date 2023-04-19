/*

Copyright 2023 StreamLogic, LLC.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

#ifdef __MINGW32__
#include <stdio.h>
#else
#define printf(...)
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "vgr2dlib.h"

#define ABS(a)		(((a)<0) ? -(a) : (a))
#define SIGN(x) ((x)>=0 ? 1 : -1)
#define UDIFF(a,b) ((a)>(b) ? (a)-(b) : 0)

extern void *vgr2d_alloc(size_t size, int n);


//////////////////////////////////////// Utils

void list_minmax(uint16_t *pts, int n, uint16_t *minx_p, uint16_t *maxx_p, uint16_t *miny_p, uint16_t *maxy_p)

{
  *minx_p = 0xffffu;
  *miny_p = 0xffffu;
  *maxx_p = 0;
  *maxy_p = 0;
  for (int i = 0; i < n; i += 2) {
    uint16_t x = pts[i];
    uint16_t y = pts[i+1];
    if (x < *minx_p) *minx_p = x;
    if (x > *maxx_p) *maxx_p = x;
    if (y < *miny_p) *miny_p = y;
    if (y > *maxy_p) *maxy_p = y;
  }
}


//////////////////////////////////////// Edge

static void fill_edges(uint16_t id, uint16_t *pts, int n, int y0, edge_t **edges) {
  int i, j;
  int X1,Y1,X2,Y2,Y3;
  edge_t *e;

  i=0;
  do {
    i += 2;
    if (i == n)
      break;
    X1 = pts[i-2];
    Y1 = pts[i-1];
    X2 = pts[i];
    Y2 = pts[i+1];
    if (Y1==Y2)
      continue;   /* Skip horiz. edges */
    /* Find next vertex not level with p2 */
    j = i;
    do {
      j += 2;
      if (j == n)
	j = 2; // skip first point which is same as last
      Y3 = pts[j+1];
      if (Y2 != Y3)
	break;
    } while (1);
    e = (edge_t *) vgr2d_alloc(sizeof(edge_t), 1);
    e->id = id;
    e->xNowNumStep = ABS(X1-X2);
    if (Y2 > Y1) {
      e->yTop = Y1;
      e->yBot = Y2;
      e->xNowWhole = X1;
      e->xNowDir = SIGN(X2 - X1);
      e->xNowDen = e->yBot - e->yTop;
      e->xNowNum = (e->xNowDen >> 1);
      if (Y3 > Y2)
	e->yBot--;
    } else {
      e->yTop = Y2;
      e->yBot = Y1;
      e->xNowWhole = X2;
      e->xNowDir = SIGN(X1 - X2);
      e->xNowDen = e->yBot - e->yTop;
      e->xNowNum = (e->xNowDen >> 1);
      if (Y3 < Y2) {
	e->yTop++;
	e->xNowNum += e->xNowNumStep;
	while (e->xNowNum >= e->xNowDen) {
	  e->xNowWhole += e->xNowDir;
	  e->xNowNum -= e->xNowDen;
	}
      }
    }
    e->next = edges[YFX_INT(e->yTop) - y0];
    edges[YFX_INT(e->yTop) - y0] = e;
  } while (1);
}


//////////////////////////////////////// Transform

void init_transform(transform_t *tr) {
  tr->tx = 0;
  tr->ty = 0;
}

//////////////////////////////////////// Rectangle

bool rect_next_line(void *arg, uint16_t* y) {
  rect_iter_t * iter = (rect_iter_t *)arg;
  *y = iter->y;
  return (*y <= iter->y2);
}

bool rect_next_run(void *arg, uint16_t y, uint16_t* x1, uint16_t *x2, uint8_t* clr) {
  rect_iter_t * iter = (rect_iter_t *)arg;
  if (iter->y == y) {
    *x1 = iter->x1;
    *x2 = iter->x2;
    *clr = iter->clr;
    iter->y += 1;
    return true;
  }
  return false;
};

void init_rectangle_iter(rectangle_t *rect, rect_iter_t *iter) {
  iter->base.size = sizeof(rect_iter_t);
  iter->base.nextLine = rect_next_line;
  iter->base.nextRun = rect_next_run;
  iter->x1 = (uint16_t)rect->tr.tx;
  iter->x2 = iter->x1 + XFX(rect->w-1);
  iter->y = (uint16_t)rect->tr.ty;
  iter->y2 = iter->y + YFX(rect->h-1);
  iter->clr = rect->fclr;
}


//////////////////////////////////////// Polygon

static void poly_advance(poly_iter_t *iter, uint16_t curY) {
  int i, j;
  int subY = YFX(curY);
  // filter out finished edges
  for (i = 0, j = 0; i < iter->n_active; i++) {
    edge_t *e = iter->active[i];
    if (e->yBot >= subY)
      iter->active[j++] = e;
  }

  // push new edges starting
  int idx = curY-iter->y0;
  if (idx < iter->n_edges) {
    edge_t *cur = iter->edges[idx];
    while (cur != NULL && j < MAX_ACTIVE) {
      iter->active[j++] = cur;
      cur = cur->next;
    }
    iter->idx = idx;
  } else
    iter->idx = iter->n_edges;
  iter->n_active = j;
  iter->y = curY;
}

static void poly_get_active(poly_iter_t *iter) {
  int num_coords, i, j;

  poly_advance(iter, iter->y);
  while (iter->n_active == 0 && iter->idx < iter->n_edges) {
    iter->y += 1;
  }

  // sort and update
  num_coords = 0;
  for (i = 0; i < iter->n_active; i++) {
    edge_t *e = iter->active[i];
    int16_t x = e->xNowWhole;
    for (j = num_coords; j > 0 && iter->x_coords[j-1] > x; j--) {
      iter->x_coords[j] = iter->x_coords[j-1];
      iter->idmap[j] = iter->idmap[j-1];
    }
    iter->x_coords[j] = x;
    iter->idmap[j] = e->id;
    num_coords++;
    e->xNowNum += e->xNowNumStep;
    while (e->xNowNum >= e->xNowDen) {
      e->xNowWhole += e->xNowDir;
      e->xNowNum -= e->xNowDen;
    }
  }

  iter->cur = 0;
}

static bool poly_next_line(void *arg, uint16_t* y) {
  poly_iter_t * iter = (poly_iter_t *)arg;
  *y = iter->ty + iter->y;
  return (iter->n_active > 0);
}

static bool polyfill_next_run(void *arg, uint16_t yin, uint16_t* x1out, uint16_t *x2out, uint8_t* clr) {
  uint16_t X1, X2;
  poly_iter_t * iter = (poly_iter_t *)arg;
  uint16_t y = yin - iter->ty;
  
  if (y == iter->y && iter->cur < iter->n_active) {
    X1 = iter->x_coords[iter->cur];
    X2 = iter->x_coords[iter->cur+1];
    *x1out = iter->tx + X1;
    *x2out = iter->tx + X2;
    *clr = iter->fclr;
    iter->cur += 2;
    if (iter->cur >= iter->n_active) {
      iter->y += 1;
      poly_get_active(iter);
    }
    return true;
  }
  return false;
};

static void init_polyfill_iter(polygon_t *poly, poly_iter_t *iter) {
  uint16_t mnx, mxx, mny, mxy;

  iter->base.nextLine = poly_next_line;
  iter->base.nextRun = polyfill_next_run;
  list_minmax(poly->pts, poly->n_pts, &mnx, &mxx, &mny, &mxy);
  iter->tx = (uint16_t)poly->tr.tx;
  iter->ty = (uint16_t)poly->tr.ty;
  iter->y0 = mny;
  iter->n_edges = mxy-mny+1;
  iter->edges = (edge_t **)vgr2d_alloc(sizeof(edge_t *), iter->n_edges);
  for (int i = 0; i < iter->n_edges; i++)
    iter->edges[i] = NULL;
  fill_edges(0, poly->pts, poly->n_pts, mny, iter->edges);
  iter->idx = 0;
  iter->n_active = 0;
  iter->y = mny;
  poly_get_active(iter);
  iter->width = poly->width;
  iter->fill = poly->fill;
  iter->stroke = poly->stroke;
  iter->fclr = poly->fclr;
  iter->sclr = poly->sclr;
}

static int merge_spans(poly_iter_t *iter, int endpoint, uint16_t* x1, uint16_t *x2) {
  int i, j;
  int n_past=0;
  uint16_t past_ids[MAX_ACTIVE];
  int16_t past_x[MAX_ACTIVE];
  int16_t x;

  for (i = iter->cur; i < iter->n_active; i++) {
    for (j = 0; j < n_past; j++)
      if (past_ids[j] == iter->idmap[i]) break;
    if (j == n_past) {
      // first coord
      past_ids[n_past] = iter->idmap[i];
      past_x[n_past] = iter->x_coords[i];
      n_past++;
    } else {
      // second coord
      x = past_x[j];
      if (*x1 <= x && x <= *x2) {
	// overlap
	x = iter->x_coords[i];
	if (x >= *x2) {
	  // equals in condition is import to get greatest endpoint at the position
	  *x2 = x;
	  endpoint = i;
	}
      }
    }
  }
  return endpoint;
}

static bool polystroke_next_run(void *arg, uint16_t yin, uint16_t* x1out, uint16_t *x2out, uint8_t* clr) {
  uint16_t X1, X2;
  poly_iter_t * iter = (poly_iter_t *)arg;
  uint16_t y = yin - iter->ty;
  
  if (y == iter->y && iter->cur < iter->n_active) {
    int cur = iter->cur;
    uint16_t id = iter->idmap[cur];
    X1 = iter->x_coords[cur++];
    if (iter->idmap[cur] != id) {
      // overlapping span, need merge
      // find current span
      while (cur < iter->n_active && iter->idmap[cur] != id) cur++;
      if (cur < iter->n_active) {
	X2 = iter->x_coords[cur];
	cur = merge_spans(iter, cur, &X1, &X2);
      }
    } else
      X2 = iter->x_coords[cur];
    printf("%d:(%d,%d)",y,X1,X2);
    iter->cur = cur+1;
    if (iter->cur >= iter->n_active) {
      printf("\n");
      iter->y += 1;
      poly_get_active(iter);
    }
    *x1out = iter->tx + X1;
    *x2out = iter->tx + X2;
    *clr = iter->sclr;
    return true;
  }
  return false;
};

static void init_polystroke_iter(polygon_t *poly, poly_iter_t *iter) {
  int i;
  uint16_t xr, yr;
  uint16_t mnx, mxx, mny, mxy;
  int X1,Y1,X2,Y2,dx,dy;
  uint16_t pts[14];

  iter->base.nextLine = poly_next_line;
  iter->base.nextRun = polystroke_next_run;
  xr = (poly->width >= 3) ? XFX(poly->width)>>1 : XFX(3)>>1;
  yr = (poly->width >= 3) ? (YFX(poly->width)-1)>>1 : 1;
  list_minmax(poly->pts, poly->n_pts, &mnx, &mxx, &mny, &mxy);
  mny = (mny > yr) ? mny - yr : 0;
  mxy += yr;
  iter->tx = (uint16_t)poly->tr.tx;
  iter->ty = (uint16_t)poly->tr.ty;
  iter->y0 = mny;
  iter->n_edges = mxy-mny+1;
  iter->edges = (edge_t **)vgr2d_alloc(sizeof(edge_t *), iter->n_edges);
  for (i = 0; i < iter->n_edges; i++)
    iter->edges[i] = NULL;
  for (i = 2; i < poly->n_pts; i += 2) {
    X1 = poly->pts[i-2];
    Y1 = poly->pts[i-1];
    X2 = poly->pts[i];
    Y2 = poly->pts[i+1];
    dx = X2-X1;
    dy = Y2-Y1;
    if (dx < 0) {
      X2 = X1;
      Y2 = Y1;
      X1 = poly->pts[i];
      Y1 = poly->pts[i+1];
    }
    if (SIGN(dx) == SIGN(dy)) {
      // SE, NW swapped
      pts[0] = X1+xr;
      pts[1] = UDIFF(Y1,yr);
      pts[2] = UDIFF(X1,xr);
      pts[3] = pts[1];
      pts[4] = pts[2];
      pts[5] = Y1+yr;
      pts[6] = UDIFF(X2,xr);
      pts[7] = Y2+yr;
      pts[8] = X2+xr;
      pts[9] = pts[7];
      pts[10] = pts[8];
      pts[11] = UDIFF(Y2,yr);
    } else {
      // NE, SW swapped
      pts[0] = UDIFF(X1, xr);
      pts[1] = UDIFF(Y1, yr);
      pts[2] = pts[0];
      pts[3] = Y1+yr;
      pts[4] = X1+xr;
      pts[5] = pts[3];
      pts[6] = X2+xr;
      pts[7] = Y2+yr;
      pts[8] = pts[6];
      pts[9] = UDIFF(Y2, yr);
      pts[10] = UDIFF(X2, xr);
      pts[11] = pts[9];
    }
    pts[12] = pts[0];
    pts[13] = pts[1];
    fill_edges(i>>1, pts, 14, mny, iter->edges);
  }
  iter->idx = 0;
  iter->n_active = 0;
  iter->y = mny;
  poly_get_active(iter);
  iter->width = poly->width;
  iter->fill = poly->fill;
  iter->stroke = poly->stroke;
  iter->fclr = poly->fclr;
  iter->sclr = poly->sclr;
}

void init_polygon_iter(polygon_t *poly, poly_iter_t *iter) {
  iter->base.size = sizeof(poly_iter_t);
  if (poly->fill)
    init_polyfill_iter(poly, iter);
  else
    init_polystroke_iter(poly, iter);
}
