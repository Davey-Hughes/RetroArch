/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2026 - The RetroArch team
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* Harness for gfx/video_views.c: map validation, frame helpers,
 * layout and touch mapping. Exit 0 on pass, 1 on any failure. */

#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <math.h>

#include "gfx/video_views.h"
#include "gfx/video_defines.h"

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { \
   printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
   failures++; } } while (0)

static struct retro_video_view mkview(unsigned x, unsigned y,
      unsigned w, unsigned h, unsigned screen, unsigned eye)
{
   struct retro_video_view v;
   v.x            = x;
   v.y            = y;
   v.width        = w;
   v.height       = h;
   v.screen       = screen;
   v.eye          = eye;
   v.aspect_ratio = 0.0f;
   return v;
}

/* 3DS packing: both top eyes side by side, bottom centred below.
 * Frame 800x480. */
static void map_3ds(video_views_map_t *map)
{
   struct retro_video_view v[3];
   v[0] = mkview(  0,   0, 400, 240, 0, RETRO_VIDEO_VIEW_EYE_LEFT);
   v[1] = mkview(400,   0, 400, 240, 0, RETRO_VIDEO_VIEW_EYE_RIGHT);
   v[2] = mkview(240, 240, 320, 240, 1, RETRO_VIDEO_VIEW_EYE_NONE);
   video_views_validate(v, 3, map);
}

/* DS packing: two 256x192 screens stacked. Frame 256x384. */
static void map_ds(video_views_map_t *map)
{
   struct retro_video_view v[2];
   v[0] = mkview(0,   0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   v[1] = mkview(0, 192, 256, 192, 1, RETRO_VIDEO_VIEW_EYE_NONE);
   video_views_validate(v, 2, map);
}

/* Virtual Boy packing: one screen, eyes side by side. Frame 768x224. */
static void map_vb(video_views_map_t *map)
{
   struct retro_video_view v[2];
   v[0] = mkview(  0, 0, 384, 224, 0, RETRO_VIDEO_VIEW_EYE_LEFT);
   v[1] = mkview(384, 0, 384, 224, 0, RETRO_VIDEO_VIEW_EYE_RIGHT);
   video_views_validate(v, 2, map);
}

static void test_validate(void)
{
   video_views_map_t map;
   struct retro_video_view v[RETRO_VIDEO_VIEWS_MAX + 1];
   unsigned i;

   CHECK(video_views_validate(NULL, 0, &map));
   CHECK(map.num_views == 0);

   v[0] = mkview(0, 0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   CHECK(video_views_validate(v, 1, &map));
   CHECK(map.num_views == 1 && map.num_screens == 1);

   map_3ds(&map);
   CHECK(map.num_views == 3 && map.num_screens == 2);

   /* Zero size. */
   v[0] = mkview(0, 0, 0, 192, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   CHECK(!video_views_validate(v, 1, &map));
   /* Unknown eye. */
   v[0] = mkview(0, 0, 256, 192, 0, 3);
   CHECK(!video_views_validate(v, 1, &map));
   /* Gap in screen numbers. */
   v[0] = mkview(0, 0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   v[1] = mkview(0, 0, 256, 192, 2, RETRO_VIDEO_VIEW_EYE_NONE);
   CHECK(!video_views_validate(v, 2, &map));
   /* A mono view and an eye on one screen. */
   v[1] = mkview(0, 0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_LEFT);
   CHECK(!video_views_validate(v, 2, &map));
   /* A left eye alone. */
   v[0] = mkview(0, 0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_LEFT);
   CHECK(!video_views_validate(v, 1, &map));
   /* Eyes of different sizes. */
   v[1] = mkview(256, 0, 255, 192, 0, RETRO_VIDEO_VIEW_EYE_RIGHT);
   CHECK(!video_views_validate(v, 2, &map));
   /* Too many views. */
   for (i = 0; i < RETRO_VIDEO_VIEWS_MAX + 1; i++)
      v[i] = mkview(0, 0, 1, 1, i, RETRO_VIDEO_VIEW_EYE_NONE);
   CHECK(!video_views_validate(v, RETRO_VIDEO_VIEWS_MAX + 1, &map));
   CHECK(video_views_validate(v, RETRO_VIDEO_VIEWS_MAX, &map));
   /* Overflowing rectangle. */
   v[0] = mkview(UINT_MAX, 0, 2, 1, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   CHECK(!video_views_validate(v, 1, &map));
   /* Absurd aspect ratio; zero is fine. */
   v[0] = mkview(0, 0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   v[0].aspect_ratio = 1000.0f;
   CHECK(!video_views_validate(v, 1, &map));
   v[0].aspect_ratio = 0.0f;
   CHECK(video_views_validate(v, 1, &map));
}

static void test_frame_helpers(void)
{
   video_views_map_t map, out;
   struct retro_video_view v[2];

   map_3ds(&map);
   CHECK(video_views_fit_frame(&map, VIDEO_SCALE_PACK(800, 480)));
   CHECK(!video_views_fit_frame(&map, VIDEO_SCALE_PACK(799, 480)));
   CHECK(!video_views_fit_frame(&map, VIDEO_SCALE_PACK(800, 479)));

   /* A 2x filter doubles every rectangle. */
   v[0] = mkview(100, 0, 100, 50, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   video_views_validate(v, 1, &map);
   video_views_scale(&map, VIDEO_SCALE_PACK(200, 50),
         VIDEO_SCALE_PACK(400, 100));
   CHECK(map.views[0].x == 200 && map.views[0].width  == 200);
   CHECK(map.views[0].y == 0   && map.views[0].height == 100);

   /* An NTSC-style 512 -> 602 stretch keeps the eyes the same size. */
   v[0] = mkview(  0, 0, 256, 224, 0, RETRO_VIDEO_VIEW_EYE_LEFT);
   v[1] = mkview(256, 0, 256, 224, 0, RETRO_VIDEO_VIEW_EYE_RIGHT);
   video_views_validate(v, 2, &map);
   video_views_scale(&map, VIDEO_SCALE_PACK(512, 224),
         VIDEO_SCALE_PACK(602, 224));
   CHECK(map.views[0].width == map.views[1].width);
   CHECK(map.views[0].width == 301 && map.views[1].x == 301);
   CHECK(video_views_fit_frame(&map, VIDEO_SCALE_PACK(602, 224)));
   /* ... and keeps each view's display aspect. */
   CHECK(fabs(video_views_aspect(&map.views[0]) - 256.0 / 224.0) < 1e-5);
   CHECK(fabs(video_views_aspect(&map.views[1]) - 256.0 / 224.0) < 1e-5);

   /* A view's own aspect ratio wins over its shape; zero or below doesn't. */
   v[0] = mkview(0, 0, 400, 200, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   CHECK(video_views_aspect(&v[0]) == 2.0);
   v[0].aspect_ratio = 1.5f;
   CHECK(video_views_aspect(&v[0]) == 1.5);
   v[0].aspect_ratio = -1.0f;
   CHECK(video_views_aspect(&v[0]) == 2.0);

   map_3ds(&map);
   CHECK(video_views_find(&map, 0, RETRO_VIDEO_VIEW_EYE_LEFT)  == 0);
   CHECK(video_views_find(&map, 0, RETRO_VIDEO_VIEW_EYE_RIGHT) == 1);
   CHECK(video_views_find(&map, 0, RETRO_VIDEO_VIEW_EYE_NONE)  == 0);
   CHECK(video_views_find(&map, 1, RETRO_VIDEO_VIEW_EYE_LEFT)  == 2);
   CHECK(video_views_find(&map, 1, RETRO_VIDEO_VIEW_EYE_RIGHT) == 2);
   CHECK(video_views_find(&map, 2, RETRO_VIDEO_VIEW_EYE_LEFT)  == -1);
   CHECK(video_views_has_stereo(&map));
   map_ds(&map);
   CHECK(!video_views_has_stereo(&map));

   /* Snapshot: copied and scaled when it fits, empty when not. */
   map_3ds(&map);
   CHECK(video_views_snapshot(&map, VIDEO_SCALE_PACK(800, 480),
            VIDEO_SCALE_PACK(1600, 960), &out));
   CHECK(out.num_views == 3 && out.views[2].x == 480 && out.views[2].width == 640);
   CHECK(!video_views_snapshot(&map, VIDEO_SCALE_PACK(640, 480),
            VIDEO_SCALE_PACK(640, 480), &out));
   CHECK(out.num_views == 0);
   map.num_views = 0;
   CHECK(!video_views_snapshot(&map, VIDEO_SCALE_PACK(800, 480),
            VIDEO_SCALE_PACK(800, 480), &out));
}

static void params_init(video_views_layout_params_t *p,
      const video_views_map_t *map, unsigned w, unsigned h)
{
   memset(p, 0, sizeof(*p));
   p->map  = map;
   p->dims = VIDEO_SCALE_PACK(w, h);
}

static int rect_is(const video_views_rect_t *r, int x, int y,
      unsigned w, unsigned h)
{
   if (     r->pos  == VIDEO_POS_PACK(x, y)
         && r->dims == VIDEO_SCALE_PACK(w, h))
      return 1;
   printf("    got (%d,%d %ux%u), want (%d,%d %ux%u)\n",
         VIDEO_POS_X(r->pos), VIDEO_POS_Y(r->pos),
         VIDEO_SCALE_W(r->dims), VIDEO_SCALE_H(r->dims), x, y, w, h);
   return 0;
}

static void test_layout_screens(void)
{
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;

   /* DS stacked in 1024x768: scale 2, centred. */
   map_ds(&map);
   params_init(&p, &map, 1024, 768);
   video_views_layout(&p, &l);
   CHECK(l.num_areas == 1 && l.num_placements == 2 && !l.offscreen);
   CHECK(l.canvas_dims == VIDEO_SCALE_PACK(1024, 768));
   CHECK(l.placements[0].view == 0 && rect_is(&l.placements[0].dst, 256,   0, 512, 384));
   CHECK(l.placements[1].view == 1 && rect_is(&l.placements[1].dst, 256, 384, 512, 384));
   CHECK(!l.ui_per_eye && l.ui_dims == VIDEO_SCALE_PACK(1024, 768));

   /* DS side by side. */
   p.screen_layout = VIDEO_SCREEN_LAYOUT_HORIZONTAL;
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.placements[0].dst,   0, 192, 512, 384));
   CHECK(rect_is(&l.placements[1].dst, 512, 192, 512, 384));

   /* Non-integer fit, and rounding that leaves no seam. */
   p.screen_layout = VIDEO_SCREEN_LAYOUT_VERTICAL;
   p.dims          = VIDEO_SCALE_PACK(1100, 800);
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.placements[0].dst, 283,   0, 534, 400));
   CHECK(rect_is(&l.placements[1].dst, 283, 400, 534, 400));

   /* Integer scaling floors the factor to 2. */
   p.scale_integer = true;
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.placements[0].dst, 294,  16, 512, 384));
   CHECK(rect_is(&l.placements[1].dst, 294, 400, 512, 384));

   /* Rotated 90 degrees: each screen is 192x256 on screen. */
   p.scale_integer = false;
   p.dims          = VIDEO_SCALE_PACK(1024, 768);
   p.rotation      = 1;
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.placements[0].dst, 368,   0, 288, 384));
   CHECK(rect_is(&l.placements[1].dst, 368, 384, 288, 384));

   /* The view's own aspect ratio. */
   map_ds(&map);
   map.views[0].aspect_ratio = 2.0f;
   map.num_views   = 1;
   map.num_screens = 1;
   params_init(&p, &map, 800, 800);
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.placements[0].dst, 0, 200, 800, 400));

   /* An empty map places nothing. */
   map.num_views = 0;
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 0);
}

static void test_layout_stereo(void)
{
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;

   map_3ds(&map);

   /* 2D shows the left eye and the bottom screen. */
   params_init(&p, &map, 800, 960);
   video_views_layout(&p, &l);
   CHECK(l.num_areas == 1 && l.num_placements == 2);
   CHECK(l.placements[0].view == 0 && rect_is(&l.placements[0].dst,  0,   0, 800, 480));
   CHECK(l.placements[1].view == 2 && rect_is(&l.placements[1].dst, 80, 480, 640, 480));

   /* Full side by side: each half laid out at its own shape. */
   params_init(&p, &map, 1600, 960);
   p.stereo_mode = VIDEO_STEREO_MODE_SBS_FULL;
   video_views_layout(&p, &l);
   CHECK(l.num_areas == 2 && l.num_placements == 4);
   CHECK(rect_is(&l.areas[0],   0, 0, 800, 960));
   CHECK(rect_is(&l.areas[1], 800, 0, 800, 960));
   CHECK(l.placements[0].area == 0 && l.placements[0].view == 0 && rect_is(&l.placements[0].dst,   0,   0, 800, 480));
   CHECK(l.placements[1].area == 0 && l.placements[1].view == 2 && rect_is(&l.placements[1].dst,  80, 480, 640, 480));
   CHECK(l.placements[2].area == 1 && l.placements[2].view == 1 && rect_is(&l.placements[2].dst, 800,   0, 800, 480));
   CHECK(l.placements[3].area == 1 && l.placements[3].view == 2 && rect_is(&l.placements[3].dst, 880, 480, 640, 480));
   CHECK(l.ui_per_eye && l.ui_dims == VIDEO_SCALE_PACK(800, 960));
   CHECK(video_views_ui_dims(&l, VIDEO_SCALE_PACK(1600, 960))
         == VIDEO_SCALE_PACK(800, 960));
   CHECK(video_views_ui_dims(NULL, VIDEO_SCALE_PACK(1600, 960))
         == VIDEO_SCALE_PACK(1600, 960));

   /* Swap Eyes exchanges the eyes, not the mono screen. */
   p.swap_eyes = true;
   video_views_layout(&p, &l);
   CHECK(l.placements[0].view == 1 && l.placements[2].view == 0);
   CHECK(l.placements[1].view == 2 && l.placements[3].view == 2);
   p.swap_eyes = false;

   /* Half side by side: laid out for the window, then squeezed. */
   p.stereo_mode = VIDEO_STEREO_MODE_SBS_HALF;
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.placements[0].dst,  200,   0, 400, 480));
   CHECK(rect_is(&l.placements[1].dst,  240, 480, 320, 480));
   CHECK(rect_is(&l.placements[2].dst, 1000,   0, 400, 480));
   CHECK(rect_is(&l.placements[3].dst, 1040, 480, 320, 480));
   CHECK(l.ui_per_eye && l.ui_dims == VIDEO_SCALE_PACK(1600, 960));

   /* Top-bottom: squeezed vertically. */
   params_init(&p, &map, 800, 1920);
   p.stereo_mode = VIDEO_STEREO_MODE_TOP_BOTTOM;
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.areas[0], 0,   0, 800, 960));
   CHECK(rect_is(&l.areas[1], 0, 960, 800, 960));
   CHECK(rect_is(&l.placements[0].dst,  0,  240, 800, 240));
   CHECK(rect_is(&l.placements[1].dst, 80,  480, 640, 240));
   CHECK(rect_is(&l.placements[2].dst,  0, 1200, 800, 240));
   CHECK(rect_is(&l.placements[3].dst, 80, 1440, 640, 240));

   /* Anaglyph: an offscreen canvas twice the window's width. */
   params_init(&p, &map, 800, 960);
   p.stereo_mode = VIDEO_STEREO_MODE_ANAGLYPH;
   video_views_layout(&p, &l);
   CHECK(l.offscreen && l.canvas_dims == VIDEO_SCALE_PACK(1600, 960));
   CHECK(l.stereo_mode == VIDEO_STEREO_MODE_ANAGLYPH);
   CHECK(!l.ui_per_eye && l.ui_dims == VIDEO_SCALE_PACK(800, 960));
   /* Not the layout's UI size, so ignoring ui_per_eye fails here. */
   CHECK(video_views_ui_dims(&l, VIDEO_SCALE_PACK(640, 480))
         == VIDEO_SCALE_PACK(640, 480));
   CHECK(rect_is(&l.placements[0].dst,   0,   0, 800, 480));
   CHECK(rect_is(&l.placements[1].dst,  80, 480, 640, 480));
   CHECK(rect_is(&l.placements[2].dst, 800,   0, 800, 480));
   CHECK(rect_is(&l.placements[3].dst, 880, 480, 640, 480));
   p.stereo_mode = VIDEO_STEREO_MODE_INTERLACED;
   video_views_layout(&p, &l);
   CHECK(l.offscreen && VIDEO_SCALE_W(l.canvas_dims) == 1600);
}

static void test_layout_one_screen(void)
{
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;
   video_views_rect_t custom;

   map_vb(&map);

   /* A 4:3 aspect setting overrides the view's own. */
   params_init(&p, &map, 1200, 900);
   p.single_aspect = 4.0f / 3.0f;
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 1 && rect_is(&l.placements[0].dst, 0, 0, 1200, 900));

   /* A custom viewport applies when the area is the whole window... */
   custom.pos  = VIDEO_POS_PACK(100, 50);
   custom.dims = VIDEO_SCALE_PACK(640, 480);
   params_init(&p, &map, 1024, 768);
   p.custom_vp = &custom;
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.placements[0].dst, 100, 50, 640, 480));

   /* ...and is squeezed with it in half side by side... */
   p.stereo_mode = VIDEO_STEREO_MODE_SBS_HALF;
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.placements[0].dst,  50, 50, 320, 480));
   CHECK(rect_is(&l.placements[1].dst, 562, 50, 320, 480));

   /* ...but not in full side by side, where the area has its own shape. */
   p.stereo_mode = VIDEO_STEREO_MODE_SBS_FULL;
   video_views_layout(&p, &l);
   CHECK(     VIDEO_POS_X(l.placements[0].dst.pos)    == 0
         && VIDEO_SCALE_W(l.placements[0].dst.dims) == 512);

   /* Custom viewports are one-screen only. */
   map_ds(&map);
   params_init(&p, &map, 1024, 768);
   p.custom_vp = &custom;
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.placements[0].dst, 256, 0, 512, 384));
}

static void test_clip(void)
{
   video_views_rect_t r, out;
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;

   /* Inside, cut at the right and bottom edges, and outside. */
   r.pos  = VIDEO_POS_PACK(100, 50);
   r.dims = VIDEO_SCALE_PACK(200, 100);
   CHECK(video_views_clip(&r, VIDEO_SCALE_PACK(1000, 1000), &out));
   CHECK(rect_is(&out, 100, 50, 200, 100));
   CHECK(video_views_clip(&r, VIDEO_SCALE_PACK(250, 120), &out));
   CHECK(rect_is(&out, 100, 50, 150, 70));
   CHECK(!video_views_clip(&r, VIDEO_SCALE_PACK(100, 1000), &out));
   CHECK(!video_views_clip(&r, VIDEO_SCALE_PACK(1000, 50), &out));
   /* A negative origin is not drawn, nor is an empty rectangle. */
   VIDEO_POS_PUT_X(r.pos, -1);
   CHECK(!video_views_clip(&r, VIDEO_SCALE_PACK(1000, 1000), &out));
   r.pos = VIDEO_POS_PACK(0, -1);
   CHECK(!video_views_clip(&r, VIDEO_SCALE_PACK(1000, 1000), &out));
   r.pos = VIDEO_POS_PACK(0, 0);
   VIDEO_SCALE_PUT_W(r.dims, 0);
   CHECK(!video_views_clip(&r, VIDEO_SCALE_PACK(1000, 1000), &out));

   /* 3DS anaglyph in 800x960: a canvas of both eyes side by side. */
   map_3ds(&map);
   params_init(&p, &map, 800, 960);
   p.stereo_mode = VIDEO_STEREO_MODE_ANAGLYPH;
   video_views_layout(&p, &l);

   /* The mono bottom screen is placed in both eyes; the first sizes it. */
   CHECK(video_views_first_drawn(&l, 2, true,
            VIDEO_SCALE_PACK(1600, 960), &out));
   CHECK(rect_is(&out, 80, 480, 640, 480));
   /* The right eye's view is in the second area only. */
   CHECK(video_views_first_drawn(&l, 1, true,
            VIDEO_SCALE_PACK(1600, 960), &out));
   CHECK(rect_is(&out, 800, 0, 800, 480));
   CHECK(!video_views_first_drawn(&l, 1, false,
            VIDEO_SCALE_PACK(1600, 960), &out));
   CHECK(!video_views_first_drawn(&l, 3, true,
            VIDEO_SCALE_PACK(1600, 960), &out));
   /* A target shrunk since the layout: clipped, or not drawn. */
   CHECK(video_views_first_drawn(&l, 0, true,
            VIDEO_SCALE_PACK(700, 300), &out));
   CHECK(rect_is(&out, 0, 0, 700, 300));
   CHECK(!video_views_first_drawn(&l, 2, true,
            VIDEO_SCALE_PACK(1600, 400), &out));
   /* The first placement off the target, the second on it. */
   VIDEO_POS_PUT_X(l.placements[1].dst.pos, 1600);
   CHECK(video_views_first_drawn(&l, 2, true,
            VIDEO_SCALE_PACK(1600, 960), &out));
   CHECK(rect_is(&out, 880, 480, 640, 480));
}

static void test_map_point(void)
{
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;
   int16_t rx, ry;

   /* DS stacked in 1024x768: screens at (256,0) and (256,384), 512x384. */
   map_ds(&map);
   params_init(&p, &map, 1024, 768);
   video_views_layout(&p, &l);

   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(256, 384), 256, 0, true, &rx, &ry));
   CHECK(rx == -0x7fff && ry == -0x7fff);

   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(256, 384), 767, 767, true, &rx, &ry));
   CHECK(rx == 0x7fff && ry == 0x7fff);

   /* Middle of the bottom screen: packed pixel (128, 288). */
   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(256, 384), 512, 576, true, &rx, &ry));
   CHECK(rx == 128 && ry == 16511);

   /* Beside both screens. */
   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(256, 384), 100, 100, true, &rx, &ry));
   CHECK(rx == -0x8000 && ry == -0x8000);
   /* Confined: clamps into the nearest screen, packed pixel (0, 50). */
   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(256, 384), 100, 100, false, &rx, &ry));
   CHECK(rx == -0x7fff && ry == -24213);

   /* 3DS, full side by side: a tap on the right eye maps to the left eye's
    * rectangle, packed pixel (200, 120). */
   map_3ds(&map);
   params_init(&p, &map, 1600, 960);
   p.stereo_mode = VIDEO_STEREO_MODE_SBS_FULL;
   video_views_layout(&p, &l);
   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(800, 480), 1200, 240, true, &rx, &ry));
   CHECK(rx == -16364 && ry == -16351);
   /* The bottom screen in the left half: packed pixel (400, 360). */
   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(800, 480), 400, 720, true, &rx, &ry));
   CHECK(rx == 40 && ry == 16485);

   /* Anaglyph: the window is the left half of the canvas. */
   params_init(&p, &map, 800, 960);
   p.stereo_mode = VIDEO_STEREO_MODE_ANAGLYPH;
   video_views_layout(&p, &l);
   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(800, 480), 400, 240, true, &rx, &ry));
   CHECK(rx == -16364 && ry == -16351);

   /* Nothing to map. */
   map.num_views = 0;
   video_views_layout(&p, &l);
   CHECK(!video_views_map_point(&l, &map, VIDEO_SCALE_PACK(800, 480), 400, 240, true, &rx, &ry));
}

int main(void)
{
   test_validate();
   test_frame_helpers();
   test_layout_screens();
   test_layout_stereo();
   test_layout_one_screen();
   test_clip();
   test_map_point();

   if (failures)
   {
      printf("video_views: FAILED (%d)\n", failures);
      return 1;
   }
   printf("video_views: ok\n");
   return 0;
}
