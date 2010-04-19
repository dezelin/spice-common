/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include <math.h>
#include "cairo_canvas.h"
#define CANVAS_USE_PIXMAN
#define CANVAS_SINGLE_INSTANCE
#include "canvas_base.c"
#include "rect.h"
#include "region.h"
#include "pixman_utils.h"

typedef struct CairoCanvas CairoCanvas;

struct CairoCanvas {
    CanvasBase base;
    uint32_t *private_data;
    int private_data_size;
    pixman_image_t *image;
};

static pixman_image_t *canvas_get_pixman_brush(CairoCanvas *canvas,
                                               SpiceBrush *brush)
{
    switch (brush->type) {
    case SPICE_BRUSH_TYPE_SOLID: {
        uint32_t color = brush->u.color;
        pixman_color_t c;

        c.blue = ((color & canvas->base.color_mask) * 0xffff) / canvas->base.color_mask;
        color >>= canvas->base.color_shift;
        c.green = ((color & canvas->base.color_mask) * 0xffff) / canvas->base.color_mask;
        color >>= canvas->base.color_shift;
        c.red = ((color & canvas->base.color_mask) * 0xffff) / canvas->base.color_mask;
        c.alpha = 0xffff;

        return pixman_image_create_solid_fill(&c);
    }
    case SPICE_BRUSH_TYPE_PATTERN: { 
        CairoCanvas *surface_canvas;
        pixman_image_t* surface;
        pixman_transform_t t;

        surface_canvas = (CairoCanvas *)canvas_get_surface(&canvas->base, brush->u.pattern.pat);
        if (surface_canvas) {
            surface = surface_canvas->image;
            surface = pixman_image_ref(surface);
        } else {
            surface = canvas_get_image(&canvas->base, brush->u.pattern.pat);
        }
        pixman_transform_init_translate(&t,
                                        pixman_int_to_fixed(-brush->u.pattern.pos.x),
                                        pixman_int_to_fixed(-brush->u.pattern.pos.y));
        pixman_image_set_transform(surface, &t);
        pixman_image_set_repeat(surface, PIXMAN_REPEAT_NORMAL);
        return surface;
    }
    case SPICE_BRUSH_TYPE_NONE:
        return NULL;
    default:
        CANVAS_ERROR("invalid brush type");
    }
}

static pixman_image_t *get_image(SpiceCanvas *canvas)
{
    CairoCanvas *cairo_canvas = (CairoCanvas *)canvas;

    pixman_image_ref(cairo_canvas->image);

    return cairo_canvas->image;
}

static void copy_region(SpiceCanvas *spice_canvas,
                        pixman_region32_t *dest_region,
                        int dx, int dy)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    pixman_box32_t *dest_rects;
    int n_rects;
    int i, j, end_line;

    dest_rects = pixman_region32_rectangles(dest_region, &n_rects);

    if (dy > 0) {
        if (dx >= 0) {
            /* south-east: copy x and y in reverse order */
            for (i = n_rects - 1; i >= 0; i--) {
                spice_pixman_copy_rect(canvas->image,
                                       dest_rects[i].x1 - dx, dest_rects[i].y1 - dy,
                                       dest_rects[i].x2 - dest_rects[i].x1,
                                       dest_rects[i].y2 - dest_rects[i].y1,
                                       dest_rects[i].x1, dest_rects[i].y1);
            }
        } else {
            /* south-west: Copy y in reverse order, but x in forward order */
            i = n_rects - 1;

            while (i >= 0) {
                /* Copy all rects with same y in forward order */
                for (end_line = i - 1; end_line >= 0 && dest_rects[end_line].y1 == dest_rects[i].y1; end_line--) {
                }
                for (j = end_line + 1; j <= i; j++) {
                    spice_pixman_copy_rect(canvas->image,
                                           dest_rects[j].x1 - dx, dest_rects[j].y1 - dy,
                                           dest_rects[j].x2 - dest_rects[j].x1,
                                           dest_rects[j].y2 - dest_rects[j].y1,
                                           dest_rects[j].x1, dest_rects[j].y1);
                }
                i = end_line;
            }
        }
    } else {
        if (dx > 0) {
            /* north-east: copy y in forward order, but x in reverse order */
            i = 0;

            while (i < n_rects) {
                /* Copy all rects with same y in reverse order */
                for (end_line = i; end_line < n_rects && dest_rects[end_line].y1 == dest_rects[i].y1; end_line++) {
                }
                for (j = end_line - 1; j >= i; j--) {
                    spice_pixman_copy_rect(canvas->image,
                                           dest_rects[j].x1 - dx, dest_rects[j].y1 - dy,
                                           dest_rects[j].x2 - dest_rects[j].x1,
                                           dest_rects[j].y2 - dest_rects[j].y1,
                                           dest_rects[j].x1, dest_rects[j].y1);
                }
                i = end_line;
            }
        } else {
            /* north-west: Copy x and y in forward order */
            for (i = 0; i < n_rects; i++) {
                spice_pixman_copy_rect(canvas->image,
                                       dest_rects[i].x1 - dx, dest_rects[i].y1 - dy,
                                       dest_rects[i].x2 - dest_rects[i].x1,
                                       dest_rects[i].y2 - dest_rects[i].y1,
                                       dest_rects[i].x1, dest_rects[i].y1);
            }
        }
    }
}

static inline uint8_t get_converted_color(uint8_t color)
{
    uint8_t msb;

    msb = color & 0xE0;
    msb = msb >> 5;
    color |= msb;
    return color;
}

static inline uint32_t get_color(CairoCanvas *canvas, uint32_t color)
{
    int shift = canvas->base.color_shift == 8 ? 0 : 3;
    uint32_t ret;

    if (!shift) {
        return color;
    }

    ret = ((color & 0x001f) << 3) | ((color & 0x001c) >> 2);
    ret |= ((color & 0x03e0) << 6) | ((color & 0x0380) << 1);
    ret |= ((color & 0x7c00) << 9) | ((color & 0x7000) << 4);

    return ret;
}


static void fill_solid_spans(SpiceCanvas *spice_canvas,
                             SpicePoint *points,
                             int *widths,
                             int n_spans,
                             uint32_t color)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    int i;

   for (i = 0; i < n_spans; i++) {
        spice_pixman_fill_rect(canvas->image,
                               points[i].x, points[i].y,
                               widths[i],
                               1,
                               get_color(canvas, color));
    }
}

static void fill_solid_rects(SpiceCanvas *spice_canvas,
                             pixman_box32_t *rects,
                             int n_rects,
                             uint32_t color)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    int i;

   for (i = 0; i < n_rects; i++) {
        spice_pixman_fill_rect(canvas->image,
                               rects[i].x1, rects[i].y1,
                               rects[i].x2 - rects[i].x1,
                               rects[i].y2 - rects[i].y1,
                               get_color(canvas, color));
    }
}

static void fill_solid_rects_rop(SpiceCanvas *spice_canvas,
                                 pixman_box32_t *rects,
                                 int n_rects,
                                 uint32_t color,
                                 SpiceROP rop)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    int i;

   for (i = 0; i < n_rects; i++) {
        spice_pixman_fill_rect_rop(canvas->image,
                                   rects[i].x1, rects[i].y1,
                                   rects[i].x2 - rects[i].x1,
                                   rects[i].y2 - rects[i].y1,
                                   get_color(canvas, color), rop);
    }
}

static void __fill_tiled_rects(SpiceCanvas *spice_canvas,
                               pixman_box32_t *rects,
                               int n_rects,
                               pixman_image_t *tile,
                               int offset_x, int offset_y)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    int i;

    for (i = 0; i < n_rects; i++) {
        spice_pixman_tile_rect(canvas->image,
                               rects[i].x1, rects[i].y1,
                               rects[i].x2 - rects[i].x1,
                               rects[i].y2 - rects[i].y1,
                               tile, offset_x, offset_y);
    }
}

static void fill_tiled_rects(SpiceCanvas *spice_canvas,
                               pixman_box32_t *rects,
                               int n_rects,
                               pixman_image_t *tile,
                               int offset_x, int offset_y)
{
    __fill_tiled_rects(spice_canvas, rects, n_rects, tile, offset_x, offset_y);
}

static void fill_tiled_rects_from_surface(SpiceCanvas *spice_canvas,
                                          pixman_box32_t *rects,
                                          int n_rects,
                                          SpiceCanvas *surface_canvas,
                                          int offset_x, int offset_y)
{
    CairoCanvas *cairo_surface_canvas = (CairoCanvas *)surface_canvas;
    __fill_tiled_rects(spice_canvas, rects, n_rects, cairo_surface_canvas->image, offset_x,
                       offset_y);
}

static void __fill_tiled_rects_rop(SpiceCanvas *spice_canvas,
                                   pixman_box32_t *rects,
                                   int n_rects,
                                   pixman_image_t *tile,
                                   int offset_x, int offset_y,
                                   SpiceROP rop)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    int i;

    for (i = 0; i < n_rects; i++) {
        spice_pixman_tile_rect_rop(canvas->image,
                                   rects[i].x1, rects[i].y1,
                                   rects[i].x2 - rects[i].x1,
                                   rects[i].y2 - rects[i].y1,
                                   tile, offset_x, offset_y,
                                   rop);
    }
}
static void fill_tiled_rects_rop(SpiceCanvas *spice_canvas,
                                 pixman_box32_t *rects,
                                 int n_rects,
                                 pixman_image_t *tile,
                                 int offset_x, int offset_y,
                                 SpiceROP rop)
{
    __fill_tiled_rects_rop(spice_canvas, rects, n_rects, tile, offset_x, offset_y, rop);
}

static void fill_tiled_rects_rop_from_surface(SpiceCanvas *spice_canvas,
                                              pixman_box32_t *rects,
                                              int n_rects,
                                              SpiceCanvas *surface_canvas,
                                              int offset_x, int offset_y,
                                              SpiceROP rop)
{
    CairoCanvas *cairo_surface_canvas = (CairoCanvas *)surface_canvas;
    __fill_tiled_rects_rop(spice_canvas, rects, n_rects, cairo_surface_canvas->image, offset_x,
                           offset_y, rop);
}

static void __blit_image(SpiceCanvas *spice_canvas,
                         pixman_region32_t *region,
                         pixman_image_t *src_image,
                         int offset_x, int offset_y)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    pixman_box32_t *rects;
    int n_rects, i;

    rects = pixman_region32_rectangles(region, &n_rects);

    for (i = 0; i < n_rects; i++) {
        int src_x, src_y, dest_x, dest_y, width, height;

        dest_x = rects[i].x1;
        dest_y = rects[i].y1;
        width = rects[i].x2 - rects[i].x1;
        height = rects[i].y2 - rects[i].y1;

        src_x = rects[i].x1 - offset_x;
        src_y = rects[i].y1 - offset_y;

        spice_pixman_blit(canvas->image,
                          src_image,
                          src_x, src_y,
                          dest_x, dest_y,
                          width, height);
    }
}

static void blit_image(SpiceCanvas *spice_canvas,
                       pixman_region32_t *region,
                       pixman_image_t *src_image,
                       int offset_x, int offset_y)
{
    __blit_image(spice_canvas, region, src_image, offset_x, offset_y);
}

static void blit_image_from_surface(SpiceCanvas *spice_canvas,
                                    pixman_region32_t *region,
                                    SpiceCanvas *surface_canvas,
                                    int offset_x, int offset_y)
{
    CairoCanvas *cairo_surface_canvas = (CairoCanvas *)surface_canvas;
    __blit_image(spice_canvas, region, cairo_surface_canvas->image, offset_x, offset_y);
}

static void __blit_image_rop(SpiceCanvas *spice_canvas,
                             pixman_region32_t *region,
                             pixman_image_t *src_image,
                             int offset_x, int offset_y,
                             SpiceROP rop)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    pixman_box32_t *rects;
    int n_rects, i;

    rects = pixman_region32_rectangles(region, &n_rects);

    for (i = 0; i < n_rects; i++) {
        int src_x, src_y, dest_x, dest_y, width, height;

        dest_x = rects[i].x1;
        dest_y = rects[i].y1;
        width = rects[i].x2 - rects[i].x1;
        height = rects[i].y2 - rects[i].y1;

        src_x = rects[i].x1 - offset_x;
        src_y = rects[i].y1 - offset_y;

        spice_pixman_blit_rop(canvas->image,
                              src_image,
                              src_x, src_y,
                              dest_x, dest_y,
                              width, height, rop);
    }
}

static void blit_image_rop(SpiceCanvas *spice_canvas,
                           pixman_region32_t *region,
                           pixman_image_t *src_image,
                           int offset_x, int offset_y,
                           SpiceROP rop)
{
    __blit_image_rop(spice_canvas, region, src_image, offset_x, offset_y, rop);
}

static void blit_image_rop_from_surface(SpiceCanvas *spice_canvas,
                                        pixman_region32_t *region,
                                        SpiceCanvas *surface_canvas,
                                        int offset_x, int offset_y,
                                        SpiceROP rop)
{
    CairoCanvas *cairo_surface_canvas = (CairoCanvas *)surface_canvas;
    __blit_image_rop(spice_canvas, region, cairo_surface_canvas->image, offset_x, offset_y, rop);
}



static void __scale_image(SpiceCanvas *spice_canvas,
                          pixman_region32_t *region,
                          pixman_image_t *src,
                          int src_x, int src_y,
                          int src_width, int src_height,
                          int dest_x, int dest_y,
                          int dest_width, int dest_height,
                          int scale_mode)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    pixman_transform_t transform;
    double sx, sy;

    sx = (double)(src_width) / (dest_width);
    sy = (double)(src_height) / (dest_height);

    pixman_image_set_clip_region32(canvas->image, region);

    pixman_transform_init_scale(&transform,
                                pixman_double_to_fixed(sx),
                                pixman_double_to_fixed(sy));

    pixman_image_set_transform(src, &transform);
    pixman_image_set_repeat(src, PIXMAN_REPEAT_NONE);
    ASSERT(scale_mode == SPICE_IMAGE_SCALE_MODE_INTERPOLATE ||
           scale_mode == SPICE_IMAGE_SCALE_MODE_NEAREST);
    pixman_image_set_filter(src,
                            (scale_mode == SPICE_IMAGE_SCALE_MODE_NEAREST) ?
                            PIXMAN_FILTER_NEAREST : PIXMAN_FILTER_GOOD,
                            NULL, 0);

    pixman_image_composite32(PIXMAN_OP_SRC,
                             src, NULL, canvas->image,
                             ROUND(src_x / sx), ROUND(src_y / sy), /* src */
                             0, 0, /* mask */
                             dest_x, dest_y, /* dst */
                             dest_width, dest_height);

    pixman_transform_init_identity(&transform);
    pixman_image_set_transform(src, &transform);

    pixman_image_set_clip_region32(canvas->image, NULL);
}

static void scale_image(SpiceCanvas *spice_canvas,
                        pixman_region32_t *region,
                        pixman_image_t *src,
                        int src_x, int src_y,
                        int src_width, int src_height,
                        int dest_x, int dest_y,
                        int dest_width, int dest_height,
                        int scale_mode)
{
    __scale_image(spice_canvas, region, src, src_x, src_y, src_width, src_height, dest_x, dest_y,
                  dest_width,dest_height,scale_mode);
}

static void scale_image_from_surface(SpiceCanvas *spice_canvas,
                                     pixman_region32_t *region,
                                     SpiceCanvas *surface_canvas,
                                     int src_x, int src_y,
                                     int src_width, int src_height,
                                     int dest_x, int dest_y,
                                     int dest_width, int dest_height,
                                     int scale_mode)
{
    CairoCanvas *cairo_surface_canvas = (CairoCanvas *)surface_canvas;
    __scale_image(spice_canvas, region, cairo_surface_canvas->image, src_x, src_y, src_width,
                  src_height, dest_x, dest_y, dest_width,dest_height,scale_mode);
}

static void __scale_image_rop(SpiceCanvas *spice_canvas,
                              pixman_region32_t *region,
                              pixman_image_t *src,
                              int src_x, int src_y,
                              int src_width, int src_height,
                              int dest_x, int dest_y,
                              int dest_width, int dest_height,
                              int scale_mode, SpiceROP rop)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    pixman_transform_t transform;
    pixman_image_t *scaled;
    pixman_box32_t *rects;
    int n_rects, i;
    double sx, sy;

    sx = (double)(src_width) / (dest_width);
    sy = (double)(src_height) / (dest_height);

    scaled = pixman_image_create_bits(PIXMAN_x8r8g8b8,
                                      dest_width,
                                      dest_height,
                                      NULL, 0);

    pixman_region32_translate(region, -dest_x, -dest_y);
    pixman_image_set_clip_region32(scaled, region);

    pixman_transform_init_scale(&transform,
                                pixman_double_to_fixed(sx),
                                pixman_double_to_fixed(sy));

    pixman_image_set_transform(src, &transform);
    pixman_image_set_repeat(src, PIXMAN_REPEAT_NONE);
    ASSERT(scale_mode == SPICE_IMAGE_SCALE_MODE_INTERPOLATE ||
           scale_mode == SPICE_IMAGE_SCALE_MODE_NEAREST);
    pixman_image_set_filter(src,
                            (scale_mode == SPICE_IMAGE_SCALE_MODE_NEAREST) ?
                            PIXMAN_FILTER_NEAREST : PIXMAN_FILTER_GOOD,
                            NULL, 0);

    pixman_image_composite32(PIXMAN_OP_SRC,
                             src, NULL, scaled,
                             ROUND(src_x / sx), ROUND(src_y / sy), /* src */
                             0, 0, /* mask */
                             0, 0, /* dst */
                             dest_width,
                             dest_height);

    pixman_transform_init_identity(&transform);
    pixman_image_set_transform(src, &transform);

    /* Translate back */
    pixman_region32_translate(region, dest_x, dest_y);

    rects = pixman_region32_rectangles(region, &n_rects);

    for (i = 0; i < n_rects; i++) {
        spice_pixman_blit_rop(canvas->image,
                              scaled,
                              rects[i].x1 - dest_x,
                              rects[i].y1 - dest_y,
                              rects[i].x1, rects[i].y1,
                              rects[i].x2 - rects[i].x1,
                              rects[i].y2 - rects[i].y1,
                              rop);
    }

    pixman_image_unref(scaled);
}

static void scale_image_rop(SpiceCanvas *spice_canvas,
                            pixman_region32_t *region,
                            pixman_image_t *src,
                            int src_x, int src_y,
                            int src_width, int src_height,
                            int dest_x, int dest_y,
                            int dest_width, int dest_height,
                            int scale_mode, SpiceROP rop)
{
    __scale_image_rop(spice_canvas, region, src, src_x, src_y, src_width, src_height, dest_x,
                      dest_y, dest_width, dest_height, scale_mode, rop);
}

static void scale_image_rop_from_surface(SpiceCanvas *spice_canvas,
                                         pixman_region32_t *region,
                                         SpiceCanvas *surface_canvas,
                                         int src_x, int src_y,
                                         int src_width, int src_height,
                                         int dest_x, int dest_y,
                                         int dest_width, int dest_height,
                                         int scale_mode, SpiceROP rop)
{
    CairoCanvas *cairo_surface_canvas = (CairoCanvas *)surface_canvas;
    __scale_image_rop(spice_canvas, region, cairo_surface_canvas->image, src_x, src_y, src_width,
                      src_height, dest_x, dest_y, dest_width, dest_height, scale_mode, rop);
}

static void __blend_image(SpiceCanvas *spice_canvas,
                          pixman_region32_t *region,
                          pixman_image_t *src,
                          int src_x, int src_y,
                          int dest_x, int dest_y,
                          int width, int height,
                          int overall_alpha)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    pixman_image_t *mask;

    pixman_image_set_clip_region32(canvas->image, region);

    mask = NULL;
    if (overall_alpha != 0xff) {
        pixman_color_t color = { 0 };
        color.alpha = overall_alpha * 0x101;
        mask = pixman_image_create_solid_fill(&color);
    }

    pixman_image_set_repeat(src, PIXMAN_REPEAT_NONE);

    pixman_image_composite32(PIXMAN_OP_OVER,
                             src, mask, canvas->image,
                             src_x, src_y, /* src */
                             0, 0, /* mask */
                             dest_x, dest_y, /* dst */
                             width,
                             height);

    if (mask) {
        pixman_image_unref(mask);
    }

    pixman_image_set_clip_region32(canvas->image, NULL);
}

static void blend_image(SpiceCanvas *spice_canvas,
                        pixman_region32_t *region,
                        pixman_image_t *src,
                        int src_x, int src_y,
                        int dest_x, int dest_y,
                        int width, int height,
                        int overall_alpha)
{
    __blend_image(spice_canvas, region, src, src_x, src_y, dest_x, dest_y, width, height,
                  overall_alpha);
}

static void blend_image_from_surface(SpiceCanvas *spice_canvas,
                                     pixman_region32_t *region,
                                     SpiceCanvas *surface_canvas,
                                     int src_x, int src_y,
                                     int dest_x, int dest_y,
                                     int width, int height,
                                     int overall_alpha)
{
    CairoCanvas *cairo_surface_canvas = (CairoCanvas *)surface_canvas;
    __blend_image(spice_canvas, region, cairo_surface_canvas->image, src_x, src_y, dest_x, dest_y,
                  width, height, overall_alpha);
}

static void __blend_scale_image(SpiceCanvas *spice_canvas,
                                pixman_region32_t *region,
                                pixman_image_t *src,
                                int src_x, int src_y,
                                int src_width, int src_height,
                                int dest_x, int dest_y,
                                int dest_width, int dest_height,
                                int scale_mode,
                                int overall_alpha)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    pixman_transform_t transform;
    pixman_image_t *mask;
    double sx, sy;

    sx = (double)(src_width) / (dest_width);
    sy = (double)(src_height) / (dest_height);

    pixman_image_set_clip_region32(canvas->image, region);

    pixman_transform_init_scale(&transform,
                                pixman_double_to_fixed(sx),
                                pixman_double_to_fixed(sy));

    mask = NULL;
    if (overall_alpha != 0xff) {
        pixman_color_t color = { 0 };
        color.alpha = overall_alpha * 0x101;
        mask = pixman_image_create_solid_fill(&color);
    }

    pixman_image_set_transform(src, &transform);
    pixman_image_set_repeat(src, PIXMAN_REPEAT_NONE);
    ASSERT(scale_mode == SPICE_IMAGE_SCALE_MODE_INTERPOLATE ||
           scale_mode == SPICE_IMAGE_SCALE_MODE_NEAREST);
    pixman_image_set_filter(src,
                            (scale_mode == SPICE_IMAGE_SCALE_MODE_NEAREST) ?
                            PIXMAN_FILTER_NEAREST : PIXMAN_FILTER_GOOD,
                            NULL, 0);

    pixman_image_composite32(PIXMAN_OP_OVER,
                             src, mask, canvas->image,
                             ROUND(src_x / sx), ROUND(src_y / sy), /* src */
                             0, 0, /* mask */
                             dest_x, dest_y, /* dst */
                             dest_width, dest_height);

    pixman_transform_init_identity(&transform);
    pixman_image_set_transform(src, &transform);

    if (mask) {
        pixman_image_unref(mask);
    }

    pixman_image_set_clip_region32(canvas->image, NULL);
}

static void blend_scale_image(SpiceCanvas *spice_canvas,
                              pixman_region32_t *region,
                              pixman_image_t *src,
                              int src_x, int src_y,
                              int src_width, int src_height,
                              int dest_x, int dest_y,
                              int dest_width, int dest_height,
                              int scale_mode,
                              int overall_alpha)
{
    __blend_scale_image(spice_canvas, region, src, src_x, src_y, src_width, src_height, dest_x,
                        dest_y, dest_width, dest_height, scale_mode, overall_alpha);
}

static void blend_scale_image_from_surface(SpiceCanvas *spice_canvas,
                                           pixman_region32_t *region,
                                           SpiceCanvas *surface_canvas,
                                           int src_x, int src_y,
                                           int src_width, int src_height,
                                           int dest_x, int dest_y,
                                           int dest_width, int dest_height,
                                           int scale_mode,
                                           int overall_alpha)
{
    CairoCanvas *cairo_surface_canvas = (CairoCanvas *)surface_canvas;
    __blend_scale_image(spice_canvas, region, cairo_surface_canvas->image, src_x, src_y, src_width,
                        src_height, dest_x, dest_y, dest_width, dest_height, scale_mode,
                        overall_alpha);
}

static void __colorkey_image(SpiceCanvas *spice_canvas,
                             pixman_region32_t *region,
                             pixman_image_t *src_image,
                             int offset_x, int offset_y,
                             uint32_t transparent_color)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    pixman_box32_t *rects;
    int n_rects, i;

    rects = pixman_region32_rectangles(region, &n_rects);

    for (i = 0; i < n_rects; i++) {
        int src_x, src_y, dest_x, dest_y, width, height;

        dest_x = rects[i].x1;
        dest_y = rects[i].y1;
        width = rects[i].x2 - rects[i].x1;
        height = rects[i].y2 - rects[i].y1;

        src_x = rects[i].x1 - offset_x;
        src_y = rects[i].y1 - offset_y;

        spice_pixman_blit_colorkey(canvas->image,
                                   src_image,
                                   src_x, src_y,
                                   dest_x, dest_y,
                                   width, height,
                                   transparent_color);
    }
}

static void colorkey_image(SpiceCanvas *spice_canvas,
                           pixman_region32_t *region,
                           pixman_image_t *src_image,
                           int offset_x, int offset_y,
                           uint32_t transparent_color)
{
    __colorkey_image(spice_canvas, region, src_image, offset_x, offset_y, transparent_color);
}

static void colorkey_image_from_surface(SpiceCanvas *spice_canvas,
                                        pixman_region32_t *region,
                                        SpiceCanvas *surface_canvas,
                                        int offset_x, int offset_y,
                                        uint32_t transparent_color)
{
    CairoCanvas *cairo_surface_canvas = (CairoCanvas *)surface_canvas;
    __colorkey_image(spice_canvas, region, cairo_surface_canvas->image, offset_x, offset_y,
                     transparent_color);
}

static void __colorkey_scale_image(SpiceCanvas *spice_canvas,
                                   pixman_region32_t *region,
                                   pixman_image_t *src,
                                   int src_x, int src_y,
                                   int src_width, int src_height,
                                   int dest_x, int dest_y,
                                   int dest_width, int dest_height,
                                   uint32_t transparent_color)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    pixman_transform_t transform;
    pixman_image_t *scaled;
    pixman_box32_t *rects;
    int n_rects, i;
    double sx, sy;

    sx = (double)(src_width) / (dest_width);
    sy = (double)(src_height) / (dest_height);

    scaled = pixman_image_create_bits(PIXMAN_x8r8g8b8,
                                      dest_width,
                                      dest_height,
                                      NULL, 0);

    pixman_region32_translate(region, -dest_x, -dest_y);
    pixman_image_set_clip_region32(scaled, region);

    pixman_transform_init_scale(&transform,
                                pixman_double_to_fixed(sx),
                                pixman_double_to_fixed(sy));

    pixman_image_set_transform(src, &transform);
    pixman_image_set_repeat(src, PIXMAN_REPEAT_NONE);
    pixman_image_set_filter(src,
                            PIXMAN_FILTER_NEAREST,
                            NULL, 0);

    pixman_image_composite32(PIXMAN_OP_SRC,
                             src, NULL, scaled,
                             ROUND(src_x / sx), ROUND(src_y / sy), /* src */
                             0, 0, /* mask */
                             0, 0, /* dst */
                             dest_width,
                             dest_height);

    pixman_transform_init_identity(&transform);
    pixman_image_set_transform(src, &transform);

    /* Translate back */
    pixman_region32_translate(region, dest_x, dest_y);

    rects = pixman_region32_rectangles(region, &n_rects);

    for (i = 0; i < n_rects; i++) {
        spice_pixman_blit_colorkey(canvas->image,
                                   scaled,
                                   rects[i].x1 - dest_x,
                                   rects[i].y1 - dest_y,
                                   rects[i].x1, rects[i].y1,
                                   rects[i].x2 - rects[i].x1,
                                   rects[i].y2 - rects[i].y1,
                                   transparent_color);
    }

    pixman_image_unref(scaled);
}

static void colorkey_scale_image(SpiceCanvas *spice_canvas,
                                 pixman_region32_t *region,
                                 pixman_image_t *src,
                                 int src_x, int src_y,
                                 int src_width, int src_height,
                                 int dest_x, int dest_y,
                                 int dest_width, int dest_height,
                                 uint32_t transparent_color)
{
    __colorkey_scale_image(spice_canvas, region, src, src_x, src_y, src_width, src_height, dest_x,
                           dest_y, dest_width, dest_height, transparent_color);
}

static void colorkey_scale_image_from_surface(SpiceCanvas *spice_canvas,
                                              pixman_region32_t *region,
                                              SpiceCanvas *surface_canvas,
                                              int src_x, int src_y,
                                              int src_width, int src_height,
                                              int dest_x, int dest_y,
                                              int dest_width, int dest_height,
                                              uint32_t transparent_color)
{
    CairoCanvas *cairo_surface_canvas = (CairoCanvas *)surface_canvas;
    __colorkey_scale_image(spice_canvas, region, cairo_surface_canvas->image, src_x, src_y,
                           src_width, src_height, dest_x, dest_y, dest_width, dest_height,
                           transparent_color);
}

static void canvas_put_image(SpiceCanvas *spice_canvas,
#ifdef WIN32
                             HDC dc,
#endif
                             const SpiceRect *dest, const uint8_t *src_data,
                             uint32_t src_width, uint32_t src_height, int src_stride,
                             const QRegion *clip)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    pixman_image_t *src;
    int dest_width;
    int dest_height;
    double sx, sy;
    pixman_transform_t transform;

    src = pixman_image_create_bits(PIXMAN_x8r8g8b8,
                                   src_width,
                                   src_height,
                                   (uint32_t*)src_data,
                                   src_stride);


    if (clip) {
        pixman_image_set_clip_region32 (canvas->image, (pixman_region32_t *)clip);
    }

    dest_width = dest->right - dest->left;
    dest_height = dest->bottom - dest->top;

    if (dest_width != src_width || dest_height != src_height) {
        sx = (double)(src_width) / (dest_width);
        sy = (double)(src_height) / (dest_height);

        pixman_transform_init_scale(&transform,
                                    pixman_double_to_fixed(sx),
                                    pixman_double_to_fixed(sy));
        pixman_image_set_transform(src, &transform);
        pixman_image_set_filter(src,
                                PIXMAN_FILTER_NEAREST,
                                NULL, 0);
    }

    pixman_image_set_repeat(src, PIXMAN_REPEAT_NONE);

    pixman_image_composite32(PIXMAN_OP_SRC,
                             src, NULL, canvas->image,
                             0, 0, /* src */
                             0, 0, /* mask */
                             dest->left, dest->top, /* dst */
                             dest_width, dest_height);


    if (clip) {
        pixman_image_set_clip_region32(canvas->image, NULL);
    }
    pixman_image_unref(src);
}


static void canvas_draw_text(SpiceCanvas *spice_canvas, SpiceRect *bbox, SpiceClip *clip, SpiceText *text)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    pixman_region32_t dest_region;
    pixman_image_t *str_mask, *brush;
    SpiceString *str;
    SpicePoint pos;
    int depth;

    pixman_region32_init_rect(&dest_region,
                              bbox->left, bbox->top,
                              bbox->right - bbox->left,
                              bbox->bottom - bbox->top);

    canvas_clip_pixman(&canvas->base, &dest_region, clip);

    if (!pixman_region32_not_empty(&dest_region)) {
        touch_brush(&canvas->base, &text->fore_brush);
        touch_brush(&canvas->base, &text->back_brush);
        pixman_region32_fini(&dest_region);
        return;
    }

    if (!rect_is_empty(&text->back_area)) {
        pixman_region32_t back_region;

        /* Nothing else makes sense for text and we should deprecate it
         * and actually it means OVER really */
        ASSERT(text->fore_mode == SPICE_ROPD_OP_PUT);

        pixman_region32_init_rect(&back_region,
                                  text->back_area.left,
                                  text->back_area.top,
                                  text->back_area.right - text->back_area.left,
                                  text->back_area.bottom - text->back_area.top);

        pixman_region32_intersect(&back_region, &back_region, &dest_region);

        if (pixman_region32_not_empty(&back_region)) {
            draw_brush(spice_canvas, &back_region, &text->back_brush, SPICE_ROP_COPY);
        }

        pixman_region32_fini(&back_region);
    }
    str = (SpiceString *)SPICE_GET_ADDRESS(text->str);

    if (str->flags & SPICE_STRING_FLAGS_RASTER_A1) {
        depth = 1;
    } else if (str->flags & SPICE_STRING_FLAGS_RASTER_A4) {
        depth = 4;
    } else if (str->flags & SPICE_STRING_FLAGS_RASTER_A8) {
        WARN("untested path A8 glyphs");
        depth = 8;
    } else {
        WARN("unsupported path vector glyphs");
        pixman_region32_fini (&dest_region);
        return;
    }

    brush = canvas_get_pixman_brush(canvas, &text->fore_brush);

    str_mask = canvas_get_str_mask(&canvas->base, str, depth, &pos);
    if (brush) {
        pixman_image_set_clip_region32(canvas->image, &dest_region);

        pixman_image_composite32(PIXMAN_OP_OVER,
                                 brush,
                                 str_mask,
                                 canvas->image,
                                 0, 0,
                                 0, 0,
                                 pos.x, pos.y,
                                 pixman_image_get_width(str_mask),
                                 pixman_image_get_height(str_mask));
        pixman_image_unref(brush);

        pixman_image_set_clip_region32(canvas->image, NULL);
    }
    pixman_image_unref(str_mask);
    pixman_region32_fini(&dest_region);
}

static void canvas_read_bits(SpiceCanvas *spice_canvas, uint8_t *dest, int dest_stride, const SpiceRect *area)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    pixman_image_t* surface;
    uint8_t *src;
    int src_stride;
    uint8_t *dest_end;

    ASSERT(canvas && area);

    surface = canvas->image;
    src_stride = pixman_image_get_stride(surface);
    src = (uint8_t *)pixman_image_get_data(surface) +
        area->top * src_stride + area->left * sizeof(uint32_t);
    dest_end = dest + (area->bottom - area->top) * dest_stride;
    for (; dest != dest_end; dest += dest_stride, src += src_stride) {
        memcpy(dest, src, dest_stride);
    }
}

static void canvas_clear(SpiceCanvas *spice_canvas)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    spice_pixman_fill_rect(canvas->image,
                           0, 0,
                           pixman_image_get_width(canvas->image),
                           pixman_image_get_height(canvas->image),
                           0);
}

static void canvas_set_access_params(SpiceCanvas *spice_canvas, unsigned long base, unsigned long max)
{
#ifdef CAIRO_CANVAS_ACCESS_TEST
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    __canvas_set_access_params(&canvas->base, base, max);
#endif
}

static void canvas_destroy(SpiceCanvas *spice_canvas)
{
    CairoCanvas *canvas = (CairoCanvas *)spice_canvas;
    if (!canvas) {
        return;
    }
    pixman_image_unref(canvas->image);
    canvas_base_destroy(&canvas->base);
    if (canvas->private_data) {
        free(canvas->private_data);
    }
    free(canvas);
}

static int need_init = 1;
static SpiceCanvasOps cairo_canvas_ops;

static SpiceCanvas *canvas_create_common(pixman_image_t *image,
                                         uint32_t format
#ifdef CAIRO_CANVAS_CACHE
                           , SpiceImageCache *bits_cache
                           , SpicePaletteCache *palette_cache
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
                           , SpiceImageCache *bits_cache
#endif
                           , SpiceImageSurfaces *surfaces
                           , SpiceGlzDecoder *glz_decoder
#ifndef CAIRO_CANVAS_NO_CHUNKS
                           , SpiceVirtMapping *virt_mapping
#endif
                           )
{
    CairoCanvas *canvas;
    int init_ok;

    if (need_init) {
        return NULL;
    }
    canvas = spice_new0(CairoCanvas, 1);
    init_ok = canvas_base_init(&canvas->base, &cairo_canvas_ops,
                               pixman_image_get_width (image),
                               pixman_image_get_height (image),
                               format
#ifdef CAIRO_CANVAS_CACHE
                               , bits_cache
                               , palette_cache
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
                               , bits_cache
#endif
                               , surfaces
                               , glz_decoder
#ifndef CAIRO_CANVAS_NO_CHUNKS
                               , virt_mapping
#endif
                               );
    canvas->private_data = NULL;
    canvas->private_data_size = 0;

    canvas->image = image;

    return (SpiceCanvas *)canvas;
}

SpiceCanvas *canvas_create(int width, int height, uint32_t format
#ifdef CAIRO_CANVAS_CACHE
                           , SpiceImageCache *bits_cache
                           , SpicePaletteCache *palette_cache
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
                           , SpiceImageCache *bits_cache
#endif
                           , SpiceImageSurfaces *surfaces
                           , SpiceGlzDecoder *glz_decoder
#ifndef CAIRO_CANVAS_NO_CHUNKS
                           , SpiceVirtMapping *virt_mapping
#endif
                           )
{
    pixman_image_t *image;

    image = pixman_image_create_bits(spice_surface_format_to_pixman (format),
                                     width, height, NULL, 0);

    return canvas_create_common(image, format
#ifdef CAIRO_CANVAS_CACHE
                                , bits_cache
                                , palette_cache
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
                                , bits_cache
#endif
                                , surfaces
                                , glz_decoder
#ifndef CAIRO_CANVAS_NO_CHUNKS
                                , virt_mapping
#endif
                                );
}

SpiceCanvas *canvas_create_for_data(int width, int height, uint32_t format,
                                    uint8_t *data, size_t stride
#ifdef CAIRO_CANVAS_CACHE
                           , SpiceImageCache *bits_cache
                           , SpicePaletteCache *palette_cache
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
                           , SpiceImageCache *bits_cache
#endif
			   , SpiceImageSurfaces *surfaces
                           , SpiceGlzDecoder *glz_decoder
#ifndef CAIRO_CANVAS_NO_CHUNKS
                           , SpiceVirtMapping *virt_mapping
#endif
                           )
{
    pixman_image_t *image;

    image = pixman_image_create_bits(spice_surface_format_to_pixman (format),
                                     width, height, (uint32_t *)data, stride);

    return canvas_create_common(image, format
#ifdef CAIRO_CANVAS_CACHE
                                , bits_cache
                                , palette_cache
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
                                , bits_cache
#endif
                                , surfaces
                                , glz_decoder
#ifndef CAIRO_CANVAS_NO_CHUNKS
                                , virt_mapping
#endif
                                );
}

void cairo_canvas_init() //unsafe global function
{
    if (!need_init) {
        return;
    }
    need_init = 0;

    canvas_base_init_ops(&cairo_canvas_ops);
    cairo_canvas_ops.draw_text = canvas_draw_text;
    cairo_canvas_ops.put_image = canvas_put_image;
    cairo_canvas_ops.clear = canvas_clear;
    cairo_canvas_ops.read_bits = canvas_read_bits;
    cairo_canvas_ops.set_access_params = canvas_set_access_params;
    cairo_canvas_ops.destroy = canvas_destroy;

    cairo_canvas_ops.fill_solid_spans = fill_solid_spans;
    cairo_canvas_ops.fill_solid_rects = fill_solid_rects;
    cairo_canvas_ops.fill_solid_rects_rop = fill_solid_rects_rop;
    cairo_canvas_ops.fill_tiled_rects = fill_tiled_rects;
    cairo_canvas_ops.fill_tiled_rects_from_surface = fill_tiled_rects_from_surface;
    cairo_canvas_ops.fill_tiled_rects_rop = fill_tiled_rects_rop;
    cairo_canvas_ops.fill_tiled_rects_rop_from_surface = fill_tiled_rects_rop_from_surface;
    cairo_canvas_ops.blit_image = blit_image;
    cairo_canvas_ops.blit_image_from_surface = blit_image_from_surface;
    cairo_canvas_ops.blit_image_rop = blit_image_rop;
    cairo_canvas_ops.blit_image_rop_from_surface = blit_image_rop_from_surface;
    cairo_canvas_ops.scale_image = scale_image;
    cairo_canvas_ops.scale_image_from_surface = scale_image_from_surface;
    cairo_canvas_ops.scale_image_rop = scale_image_rop;
    cairo_canvas_ops.scale_image_rop_from_surface = scale_image_rop_from_surface;
    cairo_canvas_ops.blend_image = blend_image;
    cairo_canvas_ops.blend_image_from_surface = blend_image_from_surface;
    cairo_canvas_ops.blend_scale_image = blend_scale_image;
    cairo_canvas_ops.blend_scale_image_from_surface = blend_scale_image_from_surface;
    cairo_canvas_ops.colorkey_image = colorkey_image;
    cairo_canvas_ops.colorkey_image_from_surface = colorkey_image_from_surface;
    cairo_canvas_ops.colorkey_scale_image = colorkey_scale_image;
    cairo_canvas_ops.colorkey_scale_image_from_surface = colorkey_scale_image_from_surface;
    cairo_canvas_ops.copy_region = copy_region;
    cairo_canvas_ops.get_image = get_image;
    rop3_init();
}
