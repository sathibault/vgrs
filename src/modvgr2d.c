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

#include "py/runtime.h"

#include "vgr2dlib.h"

#if MICROPY_MALLOC_USES_ALLOCATED_SIZE
#define MFREE(ptr, sz) m_free(ptr, sz)
#else
#define MFREE(ptr, sz) m_free(ptr)
#endif

#define MAX_RUNS 128

#define MAX_DX 0x7fff // 9.4
#define MAX_NLX 0x7fff // 9.4
#define MAX_SPANX 0x7fff // 9.4
#define MAX_CLRX 0xff // 4.4
#define MIN_DX 0x10

extern uint8_t fpga_graphics_dev();
extern void fpga_write_internal(uint8_t *buf, unsigned int len, bool hold);


void *vgr2d_alloc(size_t size, int n) {
  return m_malloc(size * n);
}


//////////////////////////////////////// Shared

static transform_t *get_transform(mp_obj_t obj);

static void transform_print(const mp_print_t *print, transform_t *tr) {
  mp_printf(print, "(%d,%d)", (int)(tr->tx/XSCALE), (int)(tr->ty/YSCALE));
}

static mp_obj_t set_position(mp_obj_t obj, mp_obj_t x_obj, mp_obj_t y_obj) {
  transform_t *tr = get_transform(obj);
  if (tr != NULL) {
    int x = mp_obj_get_int(x_obj);
    int y = mp_obj_get_int(y_obj);
    tr->tx = XFX(x);
    tr->ty = YFX(y);
  }
  return obj;
}

static MP_DEFINE_CONST_FUN_OBJ_3(set_position_obj, set_position);


//////////////////////////////////////// Rect

typedef struct rect_obj_s {
  mp_obj_base_t base;
  rectangle_t rect;
} rect_obj_t;

static mp_obj_t rect_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
  mp_arg_check_num(n_args, n_kw, 3, 3, false);

  rect_obj_t *self = m_new_obj(rect_obj_t);
  self->base.type = (mp_obj_type_t *)type;

  init_transform(&(self->rect.tr));

  self->rect.stroke = false;
  self->rect.fill = true;
  self->rect.w = mp_obj_get_int(args[0]);
  self->rect.h = mp_obj_get_int(args[1]);
  self->rect.fclr = mp_obj_get_int(args[2]);

  return MP_OBJ_FROM_PTR(self);
}

static void rect_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
  (void)kind;
  
  rect_obj_t * self = (rect_obj_t *)MP_OBJ_TO_PTR(self_in);
  mp_printf(print, "Rect(%d x %d,color[%d])@", self->rect.w, self->rect.h, self->rect.fclr);
  transform_print(print, &(self->rect.tr));
}

static const mp_rom_map_elem_t rect_locals_dict_table[] = {
  { MP_ROM_QSTR(MP_QSTR_position), MP_ROM_PTR(&set_position_obj) },
};

static MP_DEFINE_CONST_DICT(rect_locals_dict, rect_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    rect_type,
    MP_QSTR_Rect,
    MP_TYPE_FLAG_NONE,
    make_new, (const void *)rect_make_new,
    print, (const void *)rect_print,
    locals_dict, &rect_locals_dict
);


//////////////////////////////////////// Polygon

typedef struct polygon_obj_s {
  mp_obj_base_t base;
  polygon_t poly;
} polygon_obj_t;

static mp_obj_t polygon_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
  mp_arg_check_num(n_args, n_kw, 1, 1, true);

  polygon_obj_t *self = m_new_obj(polygon_obj_t);
  self->base.type = (mp_obj_type_t *)type;

  init_transform(&(self->poly.tr));

  mp_map_t kwargs;
  mp_map_init_fixed_table(&kwargs, n_kw, args + n_args);

  static const mp_arg_t allowed_args[] = {
    { MP_QSTR_fill, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_stroke, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_width, MP_ARG_INT, {.u_int = 3} },
  };

  mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
  mp_arg_parse_all(0, args, &kwargs, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

  self->poly.fill = false;
  self->poly.stroke = false;
  if (mp_obj_is_int(parsed_args[0].u_obj)) {
    self->poly.fclr = mp_obj_get_int(parsed_args[0].u_obj);
    self->poly.fill = true;
  } else if (mp_obj_is_int(parsed_args[1].u_obj)) {
    self->poly.sclr = mp_obj_get_int(parsed_args[1].u_obj);
    self->poly.stroke = true;
  } else {
    mp_raise_ValueError(MP_ERROR_TEXT("Must provide at least one of the fill or stroke arguments."));
  }
  self->poly.width = parsed_args[2].u_int;
  if (self->poly.width < 1)
    mp_raise_ValueError(MP_ERROR_TEXT("Stoke width must be at least 1"));

  size_t list_len = 0;
  mp_obj_t *list = NULL;
  mp_obj_list_get(args[0], &list_len, &list);

  self->poly.n_pts = 2*list_len+2;
  self->poly.pts = m_new(uint16_t, self->poly.n_pts);

  int i, j;
  for (i = 0, j = 0; i < list_len; i++, j+=2) {
    size_t tpl_len;
    mp_obj_t *tpl;
    mp_obj_tuple_get(list[i], &tpl_len, &tpl);
    if (tpl_len==2) {
      self->poly.pts[j] = XFX(mp_obj_get_int(tpl[0]));
      self->poly.pts[j+1] = YFX(mp_obj_get_int(tpl[1]));
    } else {
      mp_raise_ValueError(MP_ERROR_TEXT("List element is not a pair"));
    }
  }
  self->poly.pts[j++] = self->poly.pts[0];
  self->poly.pts[j++] = self->poly.pts[1];

  return MP_OBJ_FROM_PTR(self);
}

static void polygon_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
  (void)kind;
  
  polygon_obj_t * self = (polygon_obj_t *)MP_OBJ_TO_PTR(self_in);
  mp_printf(print, "Polygon([");

  for (int i = 0; i < self->poly.n_pts; i += 2) {
    if (i > 0) mp_printf(print, ",");
    mp_printf(print, "(%d,%d)", self->poly.pts[i], self->poly.pts[i+1]);
  }
  mp_printf(print, "]");
  if (self->poly.fill)
    mp_printf(print, ",fill=color%d", self->poly.fclr);
  if (self->poly.stroke) {
    mp_printf(print, ",stroke=color%d,width=%d", self->poly.sclr, self->poly.width);
  }
  mp_printf(print, ")@");
  transform_print(print, &(self->poly.tr));
}

static const mp_rom_map_elem_t polygon_locals_dict_table[] = {
  { MP_ROM_QSTR(MP_QSTR_position), MP_ROM_PTR(&set_position_obj) },
};

static MP_DEFINE_CONST_DICT(polygon_locals_dict, polygon_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    polygon_type,
    MP_QSTR_Polygon,
    MP_TYPE_FLAG_NONE,
    make_new, (const void *)polygon_make_new,
    print, (const void *)polygon_print,
    locals_dict, &polygon_locals_dict
);


//////////////////////////////////////// Polyline

typedef struct polyline_obj_s {
  mp_obj_base_t base;
  polygon_t poly;
} polyline_obj_t;

static mp_obj_t polyline_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
  mp_arg_check_num(n_args, n_kw, 2, 3, false);

  polyline_obj_t *self = m_new_obj(polyline_obj_t);
  self->base.type = (mp_obj_type_t *)type;

  init_transform(&(self->poly.tr));

  self->poly.fill = false;
  self->poly.stroke = true;
  self->poly.sclr = mp_obj_get_int(args[1]);
  self->poly.width = (n_args >= 3) ? mp_obj_get_int(args[2]) : 2;
  if (self->poly.width < 1)
    mp_raise_ValueError(MP_ERROR_TEXT("Stoke width must be at least 1"));

  size_t list_len = 0;
  mp_obj_t *list = NULL;
  mp_obj_list_get(args[0], &list_len, &list);

  self->poly.n_pts = 2*list_len;
  self->poly.pts = m_new(uint16_t, self->poly.n_pts);

  int i, j;
  for (i = 0, j = 0; i < list_len; i++, j+=2) {
    size_t tpl_len;
    mp_obj_t *tpl;
    mp_obj_tuple_get(list[i], &tpl_len, &tpl);
    if (tpl_len==2) {
      self->poly.pts[j] = XFX(mp_obj_get_int(tpl[0]));
      self->poly.pts[j+1] = YFX(mp_obj_get_int(tpl[1]));
    } else {
      mp_raise_ValueError(MP_ERROR_TEXT("List element is not a pair"));
    }
  }

  return MP_OBJ_FROM_PTR(self);
}

static void polyline_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
  (void)kind;
  
  polyline_obj_t * self = (polyline_obj_t *)MP_OBJ_TO_PTR(self_in);
  mp_printf(print, "Polyline([");

  for (int i = 0; i < self->poly.n_pts; i += 2) {
    if (i > 0) mp_printf(print, ",");
    mp_printf(print, "(%d,%d)", self->poly.pts[i], self->poly.pts[i+1]);
  }
  mp_printf(print, "]");
  if (self->poly.fill)
    mp_printf(print, ",fill=color%d", self->poly.fclr);
  if (self->poly.stroke) {
    mp_printf(print, ",stroke=color%d,width=%d", self->poly.sclr, self->poly.width);
  }
  mp_printf(print, ")@");
  transform_print(print, &(self->poly.tr));
}

static const mp_rom_map_elem_t polyline_locals_dict_table[] = {
  { MP_ROM_QSTR(MP_QSTR_position), MP_ROM_PTR(&set_position_obj) },
};

static MP_DEFINE_CONST_DICT(polyline_locals_dict, polyline_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    polyline_type,
    MP_QSTR_Polyline,
    MP_TYPE_FLAG_NONE,
    make_new, (const void *)polyline_make_new,
    print, (const void *)polyline_print,
    locals_dict, &polyline_locals_dict
);


//////////////////////////////////////// Line

typedef struct line_obj_s {
  mp_obj_base_t base;
  polygon_t poly;
} line_obj_t;

static mp_obj_t line_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
  mp_arg_check_num(n_args, n_kw, 5, 6, false);

  line_obj_t *self = m_new_obj(line_obj_t);
  self->base.type = (mp_obj_type_t *)type;

  init_transform(&(self->poly.tr));

  self->poly.fill = false;
  self->poly.stroke = true;
  self->poly.sclr = mp_obj_get_int(args[4]);
  self->poly.width = (n_args >= 6) ? mp_obj_get_int(args[5]) : 2;
  if (self->poly.width < 1)
    mp_raise_ValueError(MP_ERROR_TEXT("Stoke width must be at least 1"));

  self->poly.n_pts = 4;
  self->poly.pts = m_new(uint16_t, self->poly.n_pts);

  int i, j;
  for (i = 0, j = 0; i < 2; i++, j+=2) {
    self->poly.pts[j] = XFX(mp_obj_get_int(args[j]));
    self->poly.pts[j+1] = YFX(mp_obj_get_int(args[j+1]));
  }
  
  return MP_OBJ_FROM_PTR(self);
}

static void line_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
  (void)kind;
  
  line_obj_t * self = (line_obj_t *)MP_OBJ_TO_PTR(self_in);
  mp_printf(print, "Line([");

  for (int i = 0; i < self->poly.n_pts; i += 2) {
    if (i > 0) mp_printf(print, ",");
    mp_printf(print, "(%d,%d)", self->poly.pts[i], self->poly.pts[i+1]);
  }
  mp_printf(print, "]");
  if (self->poly.fill)
    mp_printf(print, ",fill=color%d", self->poly.fclr);
  if (self->poly.stroke) {
    mp_printf(print, ",stroke=color%d,width=%d", self->poly.sclr, self->poly.width);
  }
  mp_printf(print, ")@");
  transform_print(print, &(self->poly.tr));
}

static const mp_rom_map_elem_t line_locals_dict_table[] = {
  { MP_ROM_QSTR(MP_QSTR_position), MP_ROM_PTR(&set_position_obj) },
};

static MP_DEFINE_CONST_DICT(line_locals_dict, line_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    line_type,
    MP_QSTR_Line,
    MP_TYPE_FLAG_NONE,
    make_new, (const void *)line_make_new,
    print, (const void *)line_print,
    locals_dict, &line_locals_dict
);



//////////////////////////////////////// Dynamic methods

static transform_t *get_transform(mp_obj_t obj) {
  transform_t *tr = NULL;

  const mp_obj_type_t *otype = mp_obj_get_type(obj);
  if (otype == &rect_type) {
    rect_obj_t *rect_obj = (rect_obj_t *)MP_OBJ_TO_PTR(obj);
    tr = &(rect_obj->rect.tr);
  } else if (otype == &polygon_type || otype == &polyline_type || otype == &line_type) {
    polygon_obj_t *polygon_obj = (polygon_obj_t *)MP_OBJ_TO_PTR(obj);
    tr = &(polygon_obj->poly.tr);
  }
  return tr;
}


//////////////////////////////////////// Compile

static iter_base_t *make_iter(mp_obj_t obj) {
  const mp_obj_type_t * otype = mp_obj_get_type(obj);
  if (otype == &rect_type) {
    rect_obj_t *rect_obj = (rect_obj_t *)MP_OBJ_TO_PTR(obj);
    rectangle_t *rect = &(rect_obj->rect);
    rect_iter_t *iter = (rect_iter_t *)m_malloc(sizeof(rect_iter_t));
    init_rectangle_iter(rect, iter);
    return (iter_base_t *)iter;
  } else if (otype == &polygon_type || otype == &polyline_type || otype == &line_type) {
    polygon_obj_t *polygon_obj = (polygon_obj_t *)MP_OBJ_TO_PTR(obj);
    polygon_t *poly = &(polygon_obj->poly);
    poly_iter_t *iter = (poly_iter_t *)m_malloc(sizeof(poly_iter_t));
    init_polygon_iter(poly, iter);
    return (iter_base_t *)iter;
  }
  return NULL;
}

static void sort_runs(uint16_t *runs, uint8_t *clr, int n) {
  uint8_t c;
  uint16_t x1, x2;
  
  for (int i = 0; i < n; i++) {
    for (int j = n-1; j > i; j--) {
      int ri = j<<1;
      if (runs[ri-2] > runs[ri]) {
	c = clr[j-1];
	x1 = runs[ri-2];
	x2 = runs[ri-1];
	runs[ri-2] = runs[ri];
	runs[ri-1] = runs[ri+1];
	clr[j-1] = clr[j];
	runs[ri] = x1;
	runs[ri+1] = x2;
	clr[j] = c;
      }
    }
  }
  for (int i = 1; i < n; i++) {
    int ri = i<<1;
    int dx = (int)runs[ri]-(int)(runs[ri-1]+1);
    if (dx < MIN_DX) {
      runs[ri] = runs[ri-1]+1;
      if (runs[ri] > runs[ri+1]) // close small gaps
	runs[ri] = runs[ri+1]; // will be discarded later
    }
  }
}

static mp_obj_t generate(mp_obj_t addr_in, mp_obj_t list_in) {
  uint16_t addr = mp_obj_get_int(addr_in);

  size_t list_len = 0;
  mp_obj_t *list = NULL;
  mp_obj_list_get(list_in, &list_len, &list);
  int len = (int)list_len;

  uint16_t cmd;
  uint16_t curY, y, prevY;
  uint16_t x1, x2, dx, s, curX;
  uint8_t c;
  int i, ri;

  iter_base_t ** iters =(iter_base_t **)m_malloc(len * sizeof(iter_base_t*));
  for (int i = 0; i < len; i++)
    iters[i] = make_iter(list[i]);

  uint16_t * runs = (uint16_t *)m_malloc(2 * MAX_RUNS * sizeof(uint16_t));
  uint8_t * clr = (uint8_t *)m_malloc(MAX_RUNS * sizeof(uint8_t));

  mp_obj_t return_list = mp_obj_new_list(0, NULL);

  mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(addr>>8));
  mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(addr&0xff));
  
  prevY = 0xffff;
  do {
    // find next closest line
    curY = 0xffff;
    for (i = 0; i < len; i++) {
      if (iters[i] != NULL) {
	if (iters[i]->nextLine(iters[i], &y)) {
	  if (y < curY)
	    curY = y;
	} else {
	  MFREE(iters[i], iters[i]->size);
	  iters[i] = NULL;
	}
      }
    }
    if (curY == 0xffff) break;

    // collect runs on this line
    ri = 0;
    for (i = 0; i < len; i++) {
      if (iters[i] != NULL) {
	while (iters[i]->nextRun(iters[i], curY, &x1, &x2, &c)) {
	  if (x2 > x1 && (ri>>1) < MAX_RUNS) {
	    clr[ri>>1] = c;
	    runs[ri++] = x1;
	    runs[ri++] = x2;
	  }
	}
      }
    }

    if (ri > 0) {
      sort_runs(runs, clr, ri>>1);

      x1 = runs[0];
      if (curY > 0) {
	if (curY == (prevY+1) && x1 <= MAX_NLX) {
	  cmd = 0xa000|x1;
	  curX = x1;
	} else {
	  cmd = 0xf000|curY;
	  curX = 0;
	}
	mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(cmd>>8));
	mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(cmd&0xff));
      }
      for (i = 0; i < ri; i+=2) {
	s = runs[i+1] - runs[i];
	if (s < MIN_DX)
	  continue;

	dx = runs[i] - curX;
	if (dx > 0 && dx < MIN_DX) {
	  printf("ERR");
	  for (int k = 0; k < ri; k+=2)
	    printf(" %d,%d",runs[k],runs[k+1]);
	  printf("\n");
	}
	while (dx > MAX_DX) {
	  cmd = 0x8000|MAX_DX;
	  mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(cmd>>8));
	  mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(cmd&0xff));
	  dx -= MAX_DX;
	}
	if (dx > 0) {
	  cmd = 0x8000|dx;
	  mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(cmd>>8));
	  mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(cmd&0xff));
	}
	
	if (s > MAX_CLRX) {
	  cmd = (((uint16_t)clr[i>>1])<<8)|MAX_CLRX;
	  s -= MAX_CLRX;
	} else {
	  cmd = (((uint16_t)clr[i>>1])<<8)|s;
	  s = 0;
	}
	mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(cmd>>8));
	mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(cmd&0xff));
	while (s > MAX_SPANX) {
	  cmd = 0xc000|MAX_SPANX;
	  mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(cmd>>8));
	  mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(cmd&0xff));
	  s -= MAX_SPANX;
	}
	if (s > 0) {
	  cmd = 0xc000|s;
	  mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(cmd>>8));
	  mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(cmd&0xff));
	}
	
	curX = runs[i+1] + 1;
      }
      prevY = curY;
    } else {
      // on fail revert curY
      curY = prevY;
    }
  } while (true);

  mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(0xff));
  mp_obj_list_append(return_list, MP_OBJ_NEW_SMALL_INT(0xff));

  for (i = 0; i < len; i++) {
    if (iters[i] != NULL) {
      MFREE(iters[i], iters[i]->size);
    }
  }

  MFREE(clr, MAX_RUNS * sizeof(uint8_t));
  MFREE(runs, 2 * MAX_RUNS * sizeof(uint16_t));
  MFREE(iters, len * sizeof(iter_base_t*));
  
  return MP_OBJ_FROM_PTR(return_list);
}

const mp_obj_fun_builtin_fixed_t generate_fun = {{&mp_type_fun_builtin_2}, {._2 = &generate}};

#define SPI_SIZE 128

static mp_obj_t display2d(mp_obj_t ops_in) {
  uint8_t *buf = (uint8_t *)m_malloc(128);
  
  size_t len = 0;
  mp_obj_t *list = NULL;
  mp_obj_list_get(ops_in, &len, &list);

  buf[0] = fpga_graphics_dev();
  buf[1] = 0x03;
  fpga_write_internal(buf, 2, true);

  for (unsigned i = 0; len > 0; ) {
    unsigned sz = (len <= SPI_SIZE) ? len : SPI_SIZE;
    for (unsigned j = 0; j < sz; j++)
      buf[j] = mp_obj_get_int(list[i+j]);
    fpga_write_internal(buf, sz, len > sz);
    i += sz;
    len -= sz;
  }
  MFREE(buf, 128);
  return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(display2d_fun, display2d);

static const mp_rom_map_elem_t module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_rvgr) },
    { MP_ROM_QSTR(MP_QSTR_Rect), MP_ROM_PTR(&rect_type) },
    { MP_ROM_QSTR(MP_QSTR_Polygon), MP_ROM_PTR(&polygon_type) },
    { MP_ROM_QSTR(MP_QSTR_Polyline), MP_ROM_PTR(&polyline_type) },
    { MP_ROM_QSTR(MP_QSTR_Line), MP_ROM_PTR(&line_type) },
    { MP_ROM_QSTR(MP_QSTR_generate), MP_ROM_PTR(&generate_fun) },
    { MP_ROM_QSTR(MP_QSTR_display2d), MP_ROM_PTR(&display2d_fun) },
};
static MP_DEFINE_CONST_DICT(module_globals, module_globals_table);

const mp_obj_module_t vgr2d_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_vgr2d, vgr2d_cmodule);
