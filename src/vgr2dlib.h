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

#ifndef VGR2D_H
#define VGR2D_H

#define XFRAC 4

#define XFX(x) ((x)<<XFRAC)
#define XFX_INT(x) ((x)>>XFRAC)
#define YFX(y) (y)
#define YFX_INT(y) (y)

#define XSCALE (1<<4)
#define YSCALE 1

#define MAX_ACTIVE 8


typedef struct iter_base_s {
  size_t size;
  bool (*nextLine)(void *, int16_t*);
  bool (*nextRun)(void *, int16_t, int16_t*, int16_t*, uint8_t*);
} iter_base_t;

typedef struct edge {
  struct edge *next;
  uint16_t id;
  int16_t yTop, yBot;
  int16_t xNowWhole, xNowNum, xNowDen, xNowDir;
  int16_t xNowNumStep;
} edge_t;

typedef struct transform_s {
  float tx;
  float ty;
} transform_t;


typedef struct rectangle_s {
  transform_t tr;
  bool fill, stroke;
  uint8_t fclr,sclr;
  int16_t w, h;
} rectangle_t;

typedef struct rect_iter_s {
  iter_base_t base;
  int16_t y, y2;
  int16_t x1, x2;
  uint8_t clr;
} rect_iter_t;


typedef struct polygon_s {
  transform_t tr;
  bool fill, stroke;
  uint8_t fclr,sclr;
  int16_t *pts;
  int n_pts, width;
} polygon_t;

typedef struct poly_iter_s {
  iter_base_t base;
  int idx; // next y not processed
  edge_t **edges;
  int n_edges;
  edge_t *active[MAX_ACTIVE];
  int16_t x_coords[MAX_ACTIVE];
  uint16_t idmap[MAX_ACTIVE];
  int n_active, cur, width;
  int16_t ty, tx, y0, y;
  bool fill, stroke;
  uint8_t fclr, sclr;
} poly_iter_t;


extern void init_transform(transform_t *tr);
extern void init_rectangle_iter(rectangle_t *rect, rect_iter_t *iter);
extern void init_polygon_iter(polygon_t *poly, poly_iter_t *iter);

#endif
