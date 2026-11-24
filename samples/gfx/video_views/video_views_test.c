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
 * layout, and touch and UI pointer mapping. Exit 0 on pass, 1 on any
 * failure. */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>
#include <math.h>

#ifdef HAVE_THREADS
#include <time.h>
#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#endif

#include "gfx/video_views.h"
#include "gfx/video_defines.h"
#include "gfx/video_views_cpu.h"
#include "gfx/video_screen_prefs.h"

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { \
   printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
   failures++; } } while (0)

static void test_screen_defaults(void)
{
   video_screen_xr_t screens[RETRO_VIDEO_VIEWS_MAX + 1];
   unsigned i;
   memset(screens, 0, sizeof(screens));
   screens[RETRO_VIDEO_VIEWS_MAX].scale = 7.0f;
   video_views_xr_reset(screens, RETRO_VIDEO_VIEWS_MAX + 1);
   for (i = 0; i < RETRO_VIDEO_VIEWS_MAX; i++)
   {
      CHECK(screens[i].x == 0.0f && screens[i].y == 0.0f
            && screens[i].z == 0.0f);
      CHECK(screens[i].yaw == 0.0f && screens[i].pitch == 0.0f
            && screens[i].roll == 0.0f);
      CHECK(screens[i].scale == 1.0f && screens[i].visible);
   }
   CHECK(screens[RETRO_VIDEO_VIEWS_MAX].scale == 7.0f);
   screens[1].scale = 3.0f;
   video_views_xr_reset(screens, 1);
   CHECK(screens[1].scale == 3.0f);
   video_views_xr_reset(NULL, RETRO_VIDEO_VIEWS_MAX);
   video_views_xr_normalize(NULL);
}

static void test_screen_normalize(void)
{
   video_screen_xr_t s;
   float invalid[3];
   unsigned i;
   video_views_xr_reset(&s, 1);
   s.x = -10.0f;
   s.y = 10.0f;
   s.z = 2.5f;
   s.yaw = -180.0f;
   s.pitch = 180.0f;
   s.roll = 45.0f;
   s.scale = 0.1f;
   s.visible = false;
   video_views_xr_normalize(&s);
   CHECK(s.x == -10.0f && s.y == 10.0f && s.z == 2.5f);
   CHECK(s.yaw == -180.0f && s.pitch == 180.0f && s.roll == 45.0f);
   CHECK(s.scale == 0.1f && !s.visible);
   s.x = -11.0f;
   s.y = 11.0f;
   s.z = -11.0f;
   s.yaw = -181.0f;
   s.pitch = 181.0f;
   s.roll = -181.0f;
   s.scale = 11.0f;
   video_views_xr_normalize(&s);
   CHECK(s.x == -10.0f && s.y == 10.0f && s.z == -10.0f);
   CHECK(s.yaw == -180.0f && s.pitch == 180.0f && s.roll == -180.0f);
   CHECK(s.scale == 10.0f);
   s.scale = 0.0f;
   video_views_xr_normalize(&s);
   CHECK(s.scale == 0.1f);
   invalid[0] = (float)HUGE_VAL;
   invalid[1] = -(float)HUGE_VAL;
   invalid[2] = (float)sqrt(-1.0);
   for (i = 0; i < 3; i++)
   {
      s.x = s.y = s.z = invalid[i];
      s.yaw = s.pitch = s.roll = s.scale = invalid[i];
      video_views_xr_normalize(&s);
      CHECK(s.x == 0.0f && s.y == 0.0f && s.z == 0.0f);
      CHECK(s.yaw == 0.0f && s.pitch == 0.0f && s.roll == 0.0f);
      CHECK(s.scale == 1.0f && !s.visible);
   }
}

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

/* "dsi": DS packing with screens, and a VMU, screen 2, drawn from a
 * 48x32 XRGB8888 image. Frame 256x384. */
static struct retro_video_view         t_dsi_views[3];
static struct retro_video_screen       t_dsi_screens[3];
static struct retro_video_screen_image t_dsi_image;
static uint32_t t_dsi_pixels[48 * 32];

static void t_dsi(struct retro_video_views *in)
{
   t_dsi_views[0] = mkview(0,   0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   t_dsi_views[1] = mkview(0, 192, 256, 192, 1, RETRO_VIDEO_VIEW_EYE_NONE);
   memset(t_dsi_screens, 0, sizeof(t_dsi_screens));
   t_dsi_screens[0].name    = "Top";
   t_dsi_screens[1].name    = "Bottom";
   t_dsi_screens[2].name    = "VMU";
   t_dsi_screens[2].image   = &t_dsi_image;
   t_dsi_image.data         = t_dsi_pixels;
   t_dsi_image.width        = 48;
   t_dsi_image.height       = 32;
   t_dsi_image.pitch        = 48 * sizeof(uint32_t);
   t_dsi_image.format       = RETRO_PIXEL_FORMAT_XRGB8888;
   t_dsi_image.aspect_ratio = 0.0f;
   t_dsi_image.serial       = 1;
   memset(in, 0, sizeof(*in));
   in->views       = t_dsi_views;
   in->num_views   = 2;
   in->flags       = RETRO_VIDEO_VIEWS_FLAG_SCREENS;
   in->screens     = t_dsi_screens;
   in->num_screens = 3;
}

static void map_dsi(video_views_map_t *map)
{
   struct retro_video_views in;
   t_dsi(&in);
   video_views_validate_map(&in, map, NULL);
}

/* dsi with Bottom left out and the VMU an inset a quarter of Top's
 * height in its bottom right. */
static void map_dsi_inset(video_views_map_t *map)
{
   map_dsi(map);
   map->left_out   = 0x2;
   map->over       = 0x4;
   map->base       = 0;
   map->anchor     = RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT << 8;
   map->size[2]    = 0.25f;
   map->opacity[2] = 255;
}

/* dsi with Top a stereo pair and Bottom left out, so the image's
 * screen 2 is also a view's index: Bottom's. */
static bool map_dsi_index(video_views_map_t *map)
{
   struct retro_video_views in;
   t_dsi(&in);
   t_dsi_views[0] = mkview(  0,   0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_LEFT);
   t_dsi_views[1] = mkview(256,   0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_RIGHT);
   t_dsi_views[2] = mkview(  0, 192, 256, 192, 1, RETRO_VIDEO_VIEW_EYE_NONE);
   in.num_views   = 3;
   if (!video_views_validate_map(&in, map, NULL))
      return false;
   map->left_out = 0x2;
   return true;
}

static void map_ds_inset(video_views_map_t *map);
static void map_three(video_views_map_t *map);

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

/* Scaling the views leaves an image's size and aspect alone. */
static void test_snapshot_image(void)
{
   video_views_map_t map, out;

   map_dsi(&map);
   CHECK(video_views_snapshot(&map, VIDEO_SCALE_PACK(256, 384),
            VIDEO_SCALE_PACK(512, 768), &out));
   CHECK(out.views[1].y == 384 && out.views[1].width == 512);
   CHECK(out.image == 0x4);
   CHECK(out.image_dims[2] == VIDEO_SCALE_PACK(48, 32));
   CHECK(out.image_aspect[2] == 1.5f);
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

/* @map side by side in 1600x960. The DS map's screens land at (0,180)
 * and (800,180), each 800x600. */
static void t_side_layout(const video_views_map_t *map, unsigned mode,
      video_views_layout_t *l)
{
   video_views_layout_params_t p;
   params_init(&p, map, 1600, 960);
   p.screen_layout = VIDEO_SCREEN_LAYOUT_HORIZONTAL;
   p.stereo_mode   = mode;
   video_views_layout(&p, l);
}

static int t_dst_same(const video_views_placement_t *a,
      const video_views_placement_t *b)
{
   return a->dst.pos == b->dst.pos && a->dst.dims == b->dst.dims;
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

/* The same views in the same places with the same flags, whatever lies
 * past the count; a blank screen, one left out or another size is
 * another layout. */
static void test_layout_same(void)
{
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t a, b;

   map_ds(&map);
   params_init(&p, &map, 1024, 768);
   video_views_layout(&p, &a);
   video_views_layout(&p, &b);
   b.placements[2].flags = VIDEO_VIEWS_PLACEMENT_BLANK;
   CHECK(video_views_layout_same(&a, &b));
   map.blank = 1u << 1;
   video_views_layout(&p, &b);
   CHECK(!video_views_layout_same(&a, &b));
   map.blank    = 0;
   map.left_out = 1u << 1;
   video_views_layout(&p, &b);
   CHECK(!video_views_layout_same(&a, &b));
   map.left_out = 0;
   p.dims       = VIDEO_SCALE_PACK(1100, 800);
   video_views_layout(&p, &b);
   CHECK(!video_views_layout_same(&a, &b));
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

   /* Stretched, one screen fills its area, custom viewport or not... */
   map_vb(&map);
   params_init(&p, &map, 1000, 400);
   p.stretch   = true;
   p.custom_vp = &custom;
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.placements[0].dst, 0, 0, 1000, 400));
   p.stereo_mode = VIDEO_STEREO_MODE_SBS_HALF;
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.placements[0].dst,   0, 0, 500, 400));
   CHECK(rect_is(&l.placements[1].dst, 500, 0, 500, 400));
   /* ...unless the scale is an integer one... */
   params_init(&p, &map, 1000, 400);
   p.scale_integer = true;
   video_views_layout(&p, &l);
   custom = l.placements[0].dst;
   p.stretch = true;
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.placements[0].dst, VIDEO_POS_X(custom.pos),
            VIDEO_POS_Y(custom.pos), VIDEO_SCALE_W(custom.dims),
            VIDEO_SCALE_H(custom.dims)));
   /* ...and several screens keep their shapes. */
   map_ds(&map);
   params_init(&p, &map, 1024, 768);
   p.stretch = true;
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.placements[0].dst, 256, 0, 512, 384));
}

static void test_clip(void)
{
   video_views_rect_t r, out, custom;
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
   /* Cut at the left and top edges too; an empty rectangle is not
    * drawn. */
   VIDEO_POS_PUT_X(r.pos, -1);
   CHECK(video_views_clip(&r, VIDEO_SCALE_PACK(1000, 1000), &out));
   CHECK(rect_is(&out, 0, 50, 199, 100));
   r.pos = VIDEO_POS_PACK(0, -1);
   CHECK(video_views_clip(&r, VIDEO_SCALE_PACK(1000, 1000), &out));
   CHECK(rect_is(&out, 0, 0, 200, 99));
   r.pos = VIDEO_POS_PACK(-200, 0);
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
   /* A target shrunk since the layout: whole while part of it shows,
    * or not drawn. */
   CHECK(video_views_first_drawn(&l, 0, true,
            VIDEO_SCALE_PACK(700, 300), &out));
   CHECK(rect_is(&out, 0, 0, 800, 480));
   CHECK(video_views_visible(&l, &l.placements[0],
            VIDEO_SCALE_PACK(700, 300), &out));
   CHECK(rect_is(&out, 0, 0, 700, 300));
   CHECK(!video_views_first_drawn(&l, 2, true,
            VIDEO_SCALE_PACK(1600, 400), &out));
   /* The first placement off the target, the second on it. */
   VIDEO_POS_PUT_X(l.placements[1].dst.pos, 1600);
   CHECK(video_views_first_drawn(&l, 2, true,
            VIDEO_SCALE_PACK(1600, 960), &out));
   CHECK(rect_is(&out, 880, 480, 640, 480));

   /* A custom viewport larger than the window keeps its size and is cut
    * at the window... */
   map_vb(&map);
   custom.pos  = VIDEO_POS_PACK(-100, -50);
   custom.dims = VIDEO_SCALE_PACK(2000, 1500);
   params_init(&p, &map, 1024, 768);
   p.custom_vp = &custom;
   video_views_layout(&p, &l);
   CHECK(video_views_first_drawn(&l, 0, true,
            VIDEO_SCALE_PACK(1024, 768), &out));
   CHECK(rect_is(&out, -100, -50, 2000, 1500));
   CHECK(video_views_visible(&l, &l.placements[0],
            VIDEO_SCALE_PACK(1024, 768), &out));
   CHECK(rect_is(&out, 0, 0, 1024, 768));
   /* ...and in side by side at each eye's half. */
   p.stereo_mode = VIDEO_STEREO_MODE_SBS_HALF;
   video_views_layout(&p, &l);
   CHECK(video_views_visible(&l, &l.placements[0],
            VIDEO_SCALE_PACK(1024, 768), &out));
   CHECK(rect_is(&out, 0, 0, 512, 768));
   CHECK(video_views_visible(&l, &l.placements[1],
            VIDEO_SCALE_PACK(1024, 768), &out));
   CHECK(rect_is(&out, 512, 0, 512, 768));
}

static void test_map_point(void)
{
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;
   uint32_t pos;

   /* DS stacked in 1024x768: screens at (256,0) and (256,384), 512x384. */
   map_ds(&map);
   params_init(&p, &map, 1024, 768);
   video_views_layout(&p, &l);

   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(256, 384), 256, 0, true, &pos));
   CHECK(VIDEO_POS_X(pos) == -0x7fff && VIDEO_POS_Y(pos) == -0x7fff);

   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(256, 384), 767, 767, true, &pos));
   CHECK(VIDEO_POS_X(pos) == 0x7fff && VIDEO_POS_Y(pos) == 0x7fff);

   /* Middle of the bottom screen: packed pixel (128, 288). */
   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(256, 384), 512, 576, true, &pos));
   CHECK(VIDEO_POS_X(pos) == 128 && VIDEO_POS_Y(pos) == 16511);

   /* Beside both screens. */
   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(256, 384), 100, 100, true, &pos));
   CHECK(VIDEO_POS_X(pos) == -0x8000 && VIDEO_POS_Y(pos) == -0x8000);
   /* Confined: clamps into the nearest screen, packed pixel (0, 50). */
   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(256, 384), 100, 100, false, &pos));
   CHECK(VIDEO_POS_X(pos) == -0x7fff && VIDEO_POS_Y(pos) == -24213);

   /* 3DS, full side by side: a tap on the right eye maps to the left eye's
    * rectangle, packed pixel (200, 120). */
   map_3ds(&map);
   params_init(&p, &map, 1600, 960);
   p.stereo_mode = VIDEO_STEREO_MODE_SBS_FULL;
   video_views_layout(&p, &l);
   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(800, 480), 1200, 240, true, &pos));
   CHECK(VIDEO_POS_X(pos) == -16364 && VIDEO_POS_Y(pos) == -16351);
   /* The bottom screen in the left half: packed pixel (400, 360). */
   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(800, 480), 400, 720, true, &pos));
   CHECK(VIDEO_POS_X(pos) == 40 && VIDEO_POS_Y(pos) == 16485);

   /* Anaglyph: the window is the left half of the canvas. */
   params_init(&p, &map, 800, 960);
   p.stereo_mode = VIDEO_STEREO_MODE_ANAGLYPH;
   video_views_layout(&p, &l);
   CHECK(video_views_map_point(&l, &map, VIDEO_SCALE_PACK(800, 480), 400, 240, true, &pos));
   CHECK(VIDEO_POS_X(pos) == -16364 && VIDEO_POS_Y(pos) == -16351);

   /* Nothing to map. */
   map.num_views = 0;
   video_views_layout(&p, &l);
   CHECK(!video_views_map_point(&l, &map, VIDEO_SCALE_PACK(800, 480), 400, 240, true, &pos));
}

static int ui_point_is(const video_views_layout_t *l, int x, int y,
      int want_x, int want_y)
{
   int rx = INT_MIN, ry = INT_MIN;
   video_views_ui_point(l, x, y, &rx, &ry);
   if (rx == want_x && ry == want_y)
      return 1;
   printf("    (%d,%d) got (%d,%d), want (%d,%d)\n",
         x, y, rx, ry, want_x, want_y);
   return 0;
}

static void test_ui_point(void)
{
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;

   map_3ds(&map);

   /* A frame drawn whole, and 2D: the UI is the window. */
   CHECK(ui_point_is(NULL, 1200, 300, 1200, 300));
   params_init(&p, &map, 1600, 960);
   video_views_layout(&p, &l);
   CHECK(ui_point_is(&l, 1200, 300, 1200, 300));
   CHECK(ui_point_is(&l, -10, 2000, -10, 2000));

   /* Full side by side: one eye's size, so each copy maps as it is. */
   p.stereo_mode = VIDEO_STEREO_MODE_SBS_FULL;
   video_views_layout(&p, &l);
   CHECK(ui_point_is(&l,  400, 300, 400, 300));
   CHECK(ui_point_is(&l, 1200, 300, 400, 300));
   CHECK(ui_point_is(&l,  799, 959, 799, 959));
   CHECK(ui_point_is(&l,  800,   0,   0,   0));
   CHECK(ui_point_is(&l, 1599, 959, 799, 959));
   /* Outside the window stays outside the UI, by the nearer copy. */
   CHECK(ui_point_is(&l,  -10,  50, -10,  50));
   CHECK(ui_point_is(&l, 1700,  -5, 900,  -5));
   CHECK(ui_point_is(&l, 1200, 970, 400, 970));

   /* Swap Eyes moves the views, not the areas. */
   p.swap_eyes = true;
   video_views_layout(&p, &l);
   CHECK(ui_point_is(&l,  400, 300, 400, 300));
   CHECK(ui_point_is(&l, 1200, 300, 400, 300));
   p.swap_eyes = false;

   /* An odd width: the right copy is a pixel wider than the UI. */
   p.dims = VIDEO_SCALE_PACK(1601, 960);
   video_views_layout(&p, &l);
   CHECK(ui_point_is(&l,  799, 0, 799, 0));
   CHECK(ui_point_is(&l,  800, 0,   0, 0));
   CHECK(ui_point_is(&l,  801, 0,   0, 0));
   CHECK(ui_point_is(&l, 1600, 0, 799, 0));
   p.dims = VIDEO_SCALE_PACK(1600, 960);

   /* Half side by side: the window's size, squeezed into each half. */
   p.stereo_mode = VIDEO_STEREO_MODE_SBS_HALF;
   video_views_layout(&p, &l);
   CHECK(ui_point_is(&l,  300, 300,  600, 300));
   CHECK(ui_point_is(&l, 1100, 300,  600, 300));
   CHECK(ui_point_is(&l,  799, 959, 1598, 959));
   CHECK(ui_point_is(&l,  800,   0,    0,   0));
   CHECK(ui_point_is(&l,   -1,   0,   -2,   0));
   CHECK(ui_point_is(&l, 1700,  -5, 1800,  -5));

   /* Top-bottom: squeezed into each half's height. */
   p.stereo_mode = VIDEO_STEREO_MODE_TOP_BOTTOM;
   video_views_layout(&p, &l);
   CHECK(ui_point_is(&l,  400, 200,  400,  400));
   CHECK(ui_point_is(&l,  400, 680,  400,  400));
   CHECK(ui_point_is(&l, 1599, 479, 1599,  958));
   CHECK(ui_point_is(&l,    0, 480,    0,    0));
   CHECK(ui_point_is(&l,  100,  -3,  100,   -6));
   CHECK(ui_point_is(&l,  100, 1000, 100, 1040));
   p.swap_eyes = true;
   video_views_layout(&p, &l);
   CHECK(ui_point_is(&l,  400, 680,  400,  400));
   p.swap_eyes = false;

   /* Areas with a gap between them: the nearer one, the first on a
    * tie, and the point stays outside the UI. */
   p.stereo_mode = VIDEO_STEREO_MODE_SBS_FULL;
   video_views_layout(&p, &l);
   l.areas[0].dims = VIDEO_SCALE_PACK(700, 960);
   l.areas[1].pos  = VIDEO_POS_PACK(900, 0);
   l.areas[1].dims = VIDEO_SCALE_PACK(700, 960);
   l.ui_dims       = VIDEO_SCALE_PACK(700, 960);
   CHECK(ui_point_is(&l,  750, 10, 750, 10));
   CHECK(ui_point_is(&l,  860, 10, -40, 10));
   l.areas[1].pos  = VIDEO_POS_PACK(901, 0);
   CHECK(ui_point_is(&l,  800, 10, 800, 10));
   CHECK(ui_point_is(&l,  801, 10, -100, 10));

   /* Anaglyph and interlaced blend both eyes into the window and draw
    * the UI once, over it. */
   params_init(&p, &map, 800, 960);
   p.stereo_mode = VIDEO_STEREO_MODE_ANAGLYPH;
   video_views_layout(&p, &l);
   CHECK(ui_point_is(&l, 400, 240, 400, 240));
   CHECK(ui_point_is(&l, 799, 959, 799, 959));
   CHECK(ui_point_is(&l, 1200, 240, 1200, 240));
   p.stereo_mode = VIDEO_STEREO_MODE_INTERLACED;
   video_views_layout(&p, &l);
   CHECK(ui_point_is(&l, 400, 241, 400, 241));

   /* Nothing laid out, and areas without a size. */
   map.num_views = 0;
   p.stereo_mode = VIDEO_STEREO_MODE_SBS_HALF;
   video_views_layout(&p, &l);
   CHECK(ui_point_is(&l, 400, 240, 400, 240));
   memset(&l, 0, sizeof(l));
   l.ui_per_eye = true;
   l.num_areas  = 2;
   l.ui_dims    = VIDEO_SCALE_PACK(800, 960);
   CHECK(ui_point_is(&l, 400, 240, 400, 240));
}

static void test_eye_pair(void)
{
   video_views_map_t map;
   struct retro_video_view v[4];
   int eyes[2];

   map_3ds(&map);
   CHECK(video_views_eye_pair(&map, eyes) && eyes[0] == 0 && eyes[1] == 1);
   map_vb(&map);
   CHECK(video_views_eye_pair(&map, eyes) && eyes[0] == 0 && eyes[1] == 1);
   map_ds(&map);
   CHECK(!video_views_eye_pair(&map, eyes) && eyes[0] == -1 && eyes[1] == -1);

   /* Two pairs are not one. */
   v[0] = mkview(  0,   0, 400, 240, 0, RETRO_VIDEO_VIEW_EYE_LEFT);
   v[1] = mkview(400,   0, 400, 240, 0, RETRO_VIDEO_VIEW_EYE_RIGHT);
   v[2] = mkview(  0, 240, 400, 240, 1, RETRO_VIDEO_VIEW_EYE_LEFT);
   v[3] = mkview(400, 240, 400, 240, 1, RETRO_VIDEO_VIEW_EYE_RIGHT);
   CHECK(video_views_validate(v, 4, &map));
   CHECK(!video_views_eye_pair(&map, eyes) && eyes[0] == -1 && eyes[1] == -1);

   /* The right eye first. */
   v[0] = mkview(400, 0, 400, 240, 0, RETRO_VIDEO_VIEW_EYE_RIGHT);
   v[1] = mkview(  0, 0, 400, 240, 0, RETRO_VIDEO_VIEW_EYE_LEFT);
   CHECK(video_views_validate(v, 2, &map));
   CHECK(video_views_eye_pair(&map, eyes) && eyes[0] == 1 && eyes[1] == 0);

   /* Eyes on a screen left out are no pair; the other screen's Off
    * leaves them. */
   map_3ds(&map);
   map.left_out = 0x1u;
   CHECK(!video_views_eye_pair(&map, eyes) && eyes[0] == -1 && eyes[1] == -1);
   map.left_out = 0x2u;
   CHECK(video_views_eye_pair(&map, eyes) && eyes[0] == 0 && eyes[1] == 1);
}

/* A screen's views as a map of their own: the 3DS driver's top and
 * bottom screens. */
static void test_select_screens_roots(void)
{
   video_views_map_t map, sub;
   struct retro_video_view v[4];
   unsigned index[RETRO_VIDEO_VIEWS_MAX];

   map_3ds(&map);
   CHECK(video_views_select_screens(&map, 0x1, &sub, index));
   CHECK(sub.num_views == 2 && sub.num_screens == 1);
   CHECK(index[0] == 0 && index[1] == 1);
   CHECK(sub.views[1].eye == RETRO_VIDEO_VIEW_EYE_RIGHT);
   CHECK(video_views_find(&sub, 0, RETRO_VIDEO_VIEW_EYE_RIGHT) == 1);

   CHECK(video_views_select_screens(&map, 0x2, &sub, index));
   CHECK(sub.num_views == 1 && sub.num_screens == 1);
   CHECK(index[0] == 2 && sub.views[0].screen == 0);
   CHECK(sub.views[0].x == 240 && sub.views[0].y == 240);

   /* Bits past the map's screens select nothing. */
   CHECK(video_views_select_screens(&map, 0x3e, &sub, index));
   CHECK(sub.num_screens == 1 && sub.num_views == 1);
   CHECK(!video_views_select_screens(&map, 0x4, &sub, index));
   CHECK(sub.num_views == 0 && sub.num_screens == 0);
   CHECK(!video_views_select_screens(&map, 0, &sub, index));

   /* Screens 1 and 2 of three, in the map's order, which needn't be the
    * screens'. */
   v[0] = mkview(0,   0, 100, 50, 2, RETRO_VIDEO_VIEW_EYE_NONE);
   v[1] = mkview(0,  50, 100, 50, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   v[2] = mkview(0, 100, 100, 50, 1, RETRO_VIDEO_VIEW_EYE_LEFT);
   v[3] = mkview(100, 100, 100, 50, 1, RETRO_VIDEO_VIEW_EYE_RIGHT);
   CHECK(video_views_validate(v, 4, &map));
   CHECK(video_views_select_screens(&map, 0x6, &sub, index));
   CHECK(sub.num_views == 3 && sub.num_screens == 2);
   CHECK(index[0] == 0 && sub.views[0].screen == 1);
   CHECK(index[1] == 2 && sub.views[1].screen == 0);
   CHECK(index[2] == 3 && sub.views[2].screen == 0);
   /* A layout of the selection places its screens like a map of two. */
   {
      video_views_layout_params_t p;
      video_views_layout_t l;
      memset(&p, 0, sizeof(p));
      p.map         = &sub;
      p.dims        = VIDEO_SCALE_PACK(320, 240);
      p.stereo_mode = VIDEO_STEREO_MODE_2D;
      video_views_layout(&p, &l);
      CHECK(l.num_placements == 2);
      CHECK(l.placements[0].view == 1 && l.placements[1].view == 0);
   }

   /* An inset comes with its root, its arrangement renumbered. */
   map_three(&map);
   map.over       = 0x4;
   map.anchor     = RETRO_VIDEO_SCREEN_ANCHOR_TOP_LEFT << 8;
   map.size[2]    = 0.5f;
   map.opacity[2] = 128;
   CHECK(video_views_select_screens(&map, 0x1, &sub, index));
   CHECK(sub.num_screens == 2 && sub.num_views == 2 && sub.over == 0x2);
   CHECK(VIDEO_VIEWS_NIBBLE(sub.base, 1) == 0);
   CHECK(index[1] == 2 && sub.views[1].screen == 1);
   CHECK(   VIDEO_VIEWS_NIBBLE(sub.anchor, 1)
         == RETRO_VIDEO_SCREEN_ANCHOR_TOP_LEFT);
   CHECK(sub.size[1] == 0.5f && sub.opacity[1] == 128);
   CHECK(sub.size[0] == 0.0f && sub.opacity[0] == 0);
   CHECK(video_views_select_screens(&map, 0x2, &sub, index));
   CHECK(sub.num_screens == 1 && sub.over == 0 && index[0] == 1);

   /* Over screen 1: the base is renumbered with it. */
   map.base = 1u << 8;
   CHECK(video_views_select_screens(&map, 0x2, &sub, index));
   CHECK(sub.num_screens == 2 && sub.over == 0x2);
   CHECK(index[0] == 1 && index[1] == 2);
   CHECK(VIDEO_VIEWS_NIBBLE(sub.base, 1) == 0);
   CHECK(   VIDEO_VIEWS_NIBBLE(sub.anchor, 1)
         == RETRO_VIDEO_SCREEN_ANCHOR_TOP_LEFT);
   CHECK(sub.size[1] == 0.5f && sub.opacity[1] == 128);
   /* An inset asked for without its base is not taken. */
   CHECK(!video_views_select_screens(&map, 0x4, &sub, index));
   CHECK(sub.num_screens == 0 && sub.over == 0);

   /* A sized root on another display than its base: based on itself. */
   map.over = 0;
   map.base = 0;
   CHECK(video_views_select_screens(&map, 0x6, &sub, index));
   CHECK(sub.num_screens == 2 && sub.size[1] == 0.5f);
   CHECK(VIDEO_VIEWS_NIBBLE(sub.base, 1) == 1);
   CHECK(video_views_select_screens(&map, 0x5, &sub, index));
   CHECK(VIDEO_VIEWS_NIBBLE(sub.base, 1) == 0);
}

/* The "full" map: two stacked screens and a VMU, screen 2, with no
 * view. */
static struct retro_video_view   t_full_views[2];
static struct retro_video_screen t_full_screens[3];

static struct retro_video_screen t_screen(const char *name,
      unsigned flags, unsigned state, unsigned show, unsigned base,
      float size, unsigned anchor, float opacity, unsigned order,
      unsigned owners, unsigned swap_with, unsigned group)
{
   struct retro_video_screen s;
   s.name      = name;
   s.flags     = flags;
   s.state     = state;
   s.show      = show;
   s.base      = base;
   s.size      = size;
   s.anchor    = anchor;
   s.opacity   = opacity;
   s.order     = order;
   s.owners    = owners;
   s.swap_with = swap_with;
   s.group     = group;
   s.image     = NULL;
   return s;
}

static void t_screens_full(struct retro_video_views *in)
{
   t_full_views[0]   = mkview(0,   0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   t_full_views[1]   = mkview(0, 192, 256, 192, 1, RETRO_VIDEO_VIEW_EYE_NONE);
   t_full_screens[0] = t_screen("Top",
         RETRO_VIDEO_SCREEN_FLAG_GROUP_VERTICAL
         | RETRO_VIDEO_SCREEN_FLAG_EMPHASIS,
         RETRO_VIDEO_SCREEN_STATE_ACTIVE, RETRO_VIDEO_SCREEN_SHOW_SHOWN,
         0, 0.0f, 0, 0.0f, 2, 0, 2, 1);
   t_full_screens[1] = t_screen("Bottom",
         RETRO_VIDEO_SCREEN_FLAG_NO_INPUT | RETRO_VIDEO_SCREEN_FLAG_SHARP
         | RETRO_VIDEO_SCREEN_FLAG_INTEGER,
         RETRO_VIDEO_SCREEN_STATE_BLANK, RETRO_VIDEO_SCREEN_SHOW_SHOWN,
         0, 0.5f, 0, 0.0f, 1, 0, 0, 1);
   t_full_screens[2] = t_screen("VMU A1",
         RETRO_VIDEO_SCREEN_FLAG_PRIVATE | RETRO_VIDEO_SCREEN_FLAG_NO_INPUT
         | RETRO_VIDEO_SCREEN_FLAG_SHARP,
         RETRO_VIDEO_SCREEN_STATE_ABSENT, RETRO_VIDEO_SCREEN_SHOW_OVER,
         0, 0.25f, 8, 0.75f, 3, 0x1, 0, 0);
   memset(in, 0, sizeof(*in));
   in->views       = t_full_views;
   in->num_views   = 2;
   in->flags       = RETRO_VIDEO_VIEWS_FLAG_SCREENS;
   in->screens     = t_full_screens;
   in->num_screens = 3;
}

static int t_screen_is(const video_views_screen_t *o,
      const struct retro_video_screen *s)
{
   return o->flags == s->flags && o->state == s->state
      && o->show == s->show && o->base == s->base
      && o->size == s->size && o->anchor == s->anchor
      && o->opacity == s->opacity && o->order == s->order
      && o->owners == s->owners && o->swap_with == s->swap_with
      && o->group == s->group && !strcmp(o->name, s->name);
}

static int t_all_a5(const void *p, size_t len)
{
   const unsigned char *b = (const unsigned char*)p;
   size_t i;
   for (i = 0; i < len; i++)
      if (b[i] != 0xA5)
         return 0;
   return 1;
}

static void test_screens_full(void)
{
   struct retro_video_views in;
   video_views_map_t map, plain;
   video_views_screens_t scr;
   unsigned i;

   t_screens_full(&in);
   CHECK(video_views_validate_map(&in, &map, &scr));
   CHECK(scr.count == 3);
   for (i = 0; i < 3; i++)
      CHECK(t_screen_is(&scr.screens[i], &t_full_screens[i]));
   CHECK(scr.screens[0].flags == 0x22 && scr.screens[1].flags == 0x19
         && scr.screens[2].flags == 0xd);
   CHECK(!strcmp(scr.screens[0].name, "Top")
         && !strcmp(scr.screens[1].name, "Bottom")
         && !strcmp(scr.screens[2].name, "VMU A1"));
   CHECK(map.num_screens == 3);
   CHECK(map.no_input == 0x6 && map.viewless == 0x4);
   CHECK(video_views_validate(t_full_views, 2, &plain));
   CHECK(map.num_views == plain.num_views);
   CHECK(!memcmp(map.views, plain.views, sizeof(map.views)));
}

/* Without the flag, or with no screens, the screens are not read. */
static void test_screens_absent_flag(void)
{
   struct retro_video_views in;
   struct retro_video_screen junk[2];
   video_views_map_t map;
   video_views_screens_t scr;

   memset(junk, 0, sizeof(junk));
   junk[0].anchor = 99;
   junk[1].anchor = 99;
   t_screens_full(&in);
   in.flags       = 0;
   in.num_screens = 99;
   in.screens     = junk;
   memset(&scr, 0xA5, sizeof(scr));
   CHECK(video_views_validate_map(&in, &map, &scr));
   CHECK(scr.count == 0 && map.num_screens == 2);
   CHECK(map.no_input == 0 && map.viewless == 0);

   t_screens_full(&in);
   in.num_screens = 0;
   in.screens     = NULL;
   memset(&scr, 0xA5, sizeof(scr));
   CHECK(video_views_validate_map(&in, &map, &scr));
   CHECK(scr.count == 0 && map.num_screens == 2);
   CHECK(map.no_input == 0 && map.viewless == 0);
}

/* A core built with the earlier header: its struct ends before screens. */
static void test_screens_old_struct(void)
{
   struct retro_video_views in;
   struct retro_video_views *old;
   video_views_map_t map;
   video_views_screens_t scr;
   size_t size = offsetof(struct retro_video_views, screens);

   t_screens_full(&in);
   in.flags = 0;
   old      = (struct retro_video_views*)malloc(size);
   CHECK(old != NULL);
   if (!old)
      return;
   memcpy(old, &in, size);
   CHECK(video_views_validate_map(old, &map, &scr));
   CHECK(map.num_views == 2 && map.num_screens == 2 && scr.count == 0);
   free(old);
}

static void t_too_many(struct retro_video_views *in)
{
   in->num_screens = RETRO_VIDEO_SCREENS_MAX + 1;
}

static void t_null_screens(struct retro_video_views *in)
{
   in->screens     = NULL;
   in->num_screens = 2;
}

static void t_view_past_count(struct retro_video_views *in)
{
   in->num_screens             = 1;
   t_full_screens[0].swap_with = 0;
}

static void t_active_viewless(struct retro_video_views *in)
{
   t_full_screens[2].state = RETRO_VIDEO_SCREEN_STATE_ACTIVE;
}

static void t_blank_viewless(struct retro_video_views *in)
{
   t_full_screens[2].state = RETRO_VIDEO_SCREEN_STATE_BLANK;
}

static void t_eye_unpaired(struct retro_video_views *in)
{
   t_full_views[1].eye = RETRO_VIDEO_VIEW_EYE_LEFT;
}

static void t_base_past_count(struct retro_video_views *in)
{
   t_full_screens[1].base = 3;
}

static void t_over_self(struct retro_video_views *in)
{
   t_full_screens[2].base = 2;
}

static void t_over_over(struct retro_video_views *in)
{
   t_full_screens[0].show = RETRO_VIDEO_SCREEN_SHOW_OVER;
   t_full_screens[0].base = 1;
}

static void t_size_negative(struct retro_video_views *in)
{
   t_full_screens[1].size = -0.5f;
}

static void t_size_nan(struct retro_video_views *in)
{
   t_full_screens[1].size = (float)sqrt(-1.0);
}

static void t_size_inf(struct retro_video_views *in)
{
   t_full_screens[1].size = (float)HUGE_VAL;
}

static void t_anchor(struct retro_video_views *in)
{
   t_full_screens[2].anchor = 9;
}

static void t_over_size(struct retro_video_views *in)
{
   t_full_screens[2].size = 1.5f;
}

static void t_opacity(struct retro_video_views *in)
{
   t_full_screens[2].opacity = 1.5f;
}

static void t_opacity_negative(struct retro_video_views *in)
{
   t_full_screens[2].opacity = -0.1f;
}

static void t_opacity_nan(struct retro_video_views *in)
{
   t_full_screens[2].opacity = (float)sqrt(-1.0);
}

static void t_private_owners(struct retro_video_views *in)
{
   t_full_screens[2].owners = 0;
}

static void t_swap_self(struct retro_video_views *in)
{
   t_full_screens[0].swap_with = 1;
}

static void t_swap_past_count(struct retro_video_views *in)
{
   t_full_screens[0].swap_with = 4;
}

static void t_swap_disagree(struct retro_video_views *in)
{
   t_full_screens[0].swap_with = 2;
   t_full_screens[1].swap_with = 3;
}

static void t_show_unknown(struct retro_video_views *in)
{
   t_full_screens[2].show = 3;
}

static void t_state_unknown(struct retro_video_views *in)
{
   t_full_screens[2].state = 3;
}

/* Screen 2 has no view, so state-unknown breaks the viewless rule too. */
static void t_state_unknown_viewed(struct retro_video_views *in)
{
   t_full_screens[0].state = 3;
}

static void t_order_mixed(struct retro_video_views *in)
{
   t_full_screens[2].order = 0;
}

static void t_order_mixed_first(struct retro_video_views *in)
{
   t_full_screens[0].order = 0;
}

/* The full map with one rule broken: refused, with out and screens
 * untouched. */
static void test_screens_refused(void)
{
   static const struct
   {
      const char *name;
      void (*change)(struct retro_video_views *in);
   } cases[] = {
      { "too-many",             t_too_many             },
      { "null-screens",         t_null_screens         },
      { "view-past-count",      t_view_past_count      },
      { "active-viewless",      t_active_viewless      },
      { "blank-viewless",       t_blank_viewless       },
      { "eye-unpaired",         t_eye_unpaired         },
      { "base-past-count",      t_base_past_count      },
      { "over-self",            t_over_self            },
      { "over-over",            t_over_over            },
      { "size-negative",        t_size_negative        },
      { "size-nan",             t_size_nan             },
      { "size-inf",             t_size_inf             },
      { "anchor",               t_anchor               },
      { "over-size",            t_over_size            },
      { "opacity",              t_opacity              },
      { "opacity-negative",     t_opacity_negative     },
      { "opacity-nan",          t_opacity_nan          },
      { "private-owners",       t_private_owners       },
      { "swap-self",            t_swap_self            },
      { "swap-past-count",      t_swap_past_count      },
      { "swap-disagree",        t_swap_disagree        },
      { "show-unknown",         t_show_unknown         },
      { "state-unknown",        t_state_unknown        },
      { "state-unknown-viewed", t_state_unknown_viewed },
      { "order-mixed",          t_order_mixed          },
      { "order-mixed-first",    t_order_mixed_first    }
   };
   unsigned i;

   for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      struct retro_video_views in;
      video_views_map_t map;
      video_views_screens_t scr;
      t_screens_full(&in);
      cases[i].change(&in);
      memset(&map, 0xA5, sizeof(map));
      memset(&scr, 0xA5, sizeof(scr));
      if (     video_views_validate_map(&in, &map, &scr)
            || !t_all_a5(&map, sizeof(map))
            || !t_all_a5(&scr, sizeof(scr)))
      {
         printf("  FAIL refused case %s\n", cases[i].name);
         failures++;
      }
   }
}

/* An image screen needs no view; validation keeps its size and aspect,
 * and makes it NO_INPUT. */
static void test_screens_image(void)
{
   struct retro_video_views in;
   video_views_map_t map;
   video_views_screens_t scr;
   unsigned i;

   t_dsi(&in);
   memset(&map, 0xA5, sizeof(map));
   CHECK(video_views_validate_map(&in, &map, &scr));
   CHECK(map.num_views == 2 && map.num_screens == 3 && scr.count == 3);
   CHECK(map.image == 0x4);
   CHECK(map.image_dims[2] == VIDEO_SCALE_PACK(48, 32));
   CHECK(map.image_aspect[2] == 1.5f);
   CHECK(map.no_input == 0x4 && map.viewless == 0);
   CHECK(!map.nearest);
   for (i = 0; i < RETRO_VIDEO_VIEWS_MAX; i++)
      CHECK(!map.image_tex[i] && !map.image_gen[i]);

   t_dsi_image.aspect_ratio = 2.0f;
   CHECK(video_views_validate_map(&in, &map, &scr));
   CHECK(map.image_aspect[2] == 2.0f);

   /* NaN gives width over height, as for a view. */
   t_dsi_image.aspect_ratio = (float)sqrt(-1.0);
   CHECK(video_views_validate_map(&in, &map, &scr));
   CHECK(map.image_aspect[2] == 1.5f);

   t_dsi(&in);
   t_dsi_image.format = RETRO_PIXEL_FORMAT_RGB565;
   t_dsi_image.pitch  = 96;
   CHECK(video_views_validate_map(&in, &map, &scr));
   CHECK(map.image == 0x4 && map.image_dims[2] == VIDEO_SCALE_PACK(48, 32));
}

static void t_img_viewed(struct retro_video_views *in)
{
   t_dsi_views[2] = mkview(0, 384, 48, 32, 2, RETRO_VIDEO_VIEW_EYE_NONE);
   in->num_views  = 3;
}

static void t_img_width_zero(struct retro_video_views *in)
{
   t_dsi_image.width = 0;
}

static void t_img_height_zero(struct retro_video_views *in)
{
   t_dsi_image.height = 0;
}

static void t_img_format(struct retro_video_views *in)
{
   t_dsi_image.format = RETRO_PIXEL_FORMAT_0RGB1555;
}

static void t_img_pitch(struct retro_video_views *in)
{
   t_dsi_image.pitch = 191;
}

static void t_img_pitch_565(struct retro_video_views *in)
{
   t_dsi_image.format = RETRO_PIXEL_FORMAT_RGB565;
   t_dsi_image.pitch  = 95;
}

static void t_img_width_max(struct retro_video_views *in)
{
   t_dsi_image.width = VIDEO_VIEWS_IMAGE_MAX + 1;
   t_dsi_image.pitch = (VIDEO_VIEWS_IMAGE_MAX + 1) * sizeof(uint32_t);
}

static void t_img_height_max(struct retro_video_views *in)
{
   t_dsi_image.height = VIDEO_VIEWS_IMAGE_MAX + 1;
}

static void t_img_aspect_low(struct retro_video_views *in)
{
   t_dsi_image.aspect_ratio = 0.005f;
}

static void t_img_aspect_high(struct retro_video_views *in)
{
   t_dsi_image.aspect_ratio = 101.0f;
}

/* dsi with one image rule broken: refused, with out and screens
 * untouched. */
static void test_screens_image_refused(void)
{
   static const struct
   {
      const char *name;
      void (*change)(struct retro_video_views *in);
   } cases[] = {
      { "viewed",      t_img_viewed      },
      { "width-zero",  t_img_width_zero  },
      { "height-zero", t_img_height_zero },
      { "format",      t_img_format      },
      { "pitch",       t_img_pitch       },
      { "pitch-565",   t_img_pitch_565   },
      { "width-max",   t_img_width_max   },
      { "height-max",  t_img_height_max  },
      { "aspect-low",  t_img_aspect_low  },
      { "aspect-high", t_img_aspect_high }
   };
   unsigned i;

   for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      struct retro_video_views in;
      video_views_map_t map;
      video_views_screens_t scr;
      t_dsi(&in);
      cases[i].change(&in);
      memset(&map, 0xA5, sizeof(map));
      memset(&scr, 0xA5, sizeof(scr));
      if (     video_views_validate_map(&in, &map, &scr)
            || !t_all_a5(&map, sizeof(map))
            || !t_all_a5(&scr, sizeof(scr)))
      {
         printf("  FAIL refused image case %s\n", cases[i].name);
         failures++;
      }
   }
}

static void test_screens_accepted(void)
{
   struct retro_video_views in;
   video_views_map_t map;
   video_views_screens_t scr;

   /* An empty map clears, whatever its screens. */
   t_screens_full(&in);
   in.num_views = 0;
   memset(&map, 0xA5, sizeof(map));
   memset(&scr, 0xA5, sizeof(scr));
   CHECK(video_views_validate_map(&in, &map, &scr));
   CHECK(map.num_views == 0 && map.num_screens == 0 && scr.count == 0);
   CHECK(map.no_input == 0 && map.viewless == 0);

   /* Screen 0 absent, with no view. */
   t_screens_full(&in);
   t_full_screens[0].state = RETRO_VIDEO_SCREEN_STATE_ABSENT;
   in.views     = &t_full_views[1];
   in.num_views = 1;
   CHECK(video_views_validate_map(&in, &map, &scr));
   CHECK(map.num_screens == 3 && map.viewless == 0x5);

   /* An absent screen with no view between two with views. */
   t_screens_full(&in);
   t_full_views[1].screen  = 2;
   t_full_screens[1].state = RETRO_VIDEO_SCREEN_STATE_ABSENT;
   t_full_screens[2].state = RETRO_VIDEO_SCREEN_STATE_ACTIVE;
   CHECK(video_views_validate_map(&in, &map, &scr));
   CHECK(map.num_screens == 3 && map.viewless == 0x2);

   /* A swap declared by both sides. */
   t_screens_full(&in);
   t_full_screens[1].swap_with = 1;
   CHECK(video_views_validate_map(&in, &map, &scr));
   CHECK(scr.screens[0].swap_with == 2 && scr.screens[1].swap_with == 1);

   /* OVER a shown screen other than 0. */
   t_screens_full(&in);
   t_full_screens[2].base = 1;
   CHECK(video_views_validate_map(&in, &map, &scr));
   CHECK(scr.screens[2].base == 1);

   /* The bounds themselves. */
   t_screens_full(&in);
   t_full_screens[2].size = 1.0f;
   CHECK(video_views_validate_map(&in, &map, &scr));
   t_full_screens[2].opacity = 0.0f;
   CHECK(video_views_validate_map(&in, &map, &scr));
   t_full_screens[2].opacity = 1.0f;
   CHECK(video_views_validate_map(&in, &map, &scr));
   CHECK(scr.screens[2].size == 1.0f && scr.screens[2].opacity == 1.0f);

   /* order 0 on every screen, and ties. */
   t_screens_full(&in);
   t_full_screens[0].order = 0;
   t_full_screens[1].order = 0;
   t_full_screens[2].order = 0;
   CHECK(video_views_validate_map(&in, &map, &scr));
   t_full_screens[0].order = 1;
   t_full_screens[1].order = 1;
   t_full_screens[2].order = 1;
   CHECK(video_views_validate_map(&in, &map, &scr));
   CHECK(scr.screens[2].order == 1);

   /* Flag bits the frontend does not know are ignored; UPRIGHT is kept. */
   t_screens_full(&in);
   in.flags                |= 1u << 31;
   t_full_screens[1].flags |= RETRO_VIDEO_SCREEN_FLAG_UPRIGHT | (1u << 31);
   CHECK(video_views_validate_map(&in, &map, &scr));
   CHECK(scr.count == 3 && map.no_input == 0x6);
   CHECK(scr.screens[1].flags == (0x59u | (1u << 31)));
}

/* Screen 2's name as validate_map keeps it, or NULL if refused. */
static const char *t_name_kept(const char *name, video_views_screens_t *scr)
{
   struct retro_video_views in;
   video_views_map_t map;
   t_screens_full(&in);
   t_full_screens[2].name = name;
   if (!video_views_validate_map(&in, &map, scr))
      return NULL;
   return scr->screens[2].name;
}

static void test_screens_names(void)
{
   video_views_screens_t scr;
   char longer[41];
   char cut[33];
   const char *got;

   got = t_name_kept(NULL, &scr);
   CHECK(got && !strcmp(got, ""));
   got = t_name_kept("VMU A1", &scr);
   CHECK(got && !strcmp(got, "VMU A1"));

   memset(longer, 'a', 40);
   longer[40] = '\0';
   got = t_name_kept(longer, &scr);
   CHECK(got && strlen(got) == 31 && !strncmp(got, longer, 31));

   /* 30 'a' then U+00E9: the cut lands inside it, so it goes. */
   memset(cut, 'a', 30);
   strcpy(cut + 30, "\xc3\xa9");
   got = t_name_kept(cut, &scr);
   CHECK(got && strlen(got) == 30 && !strncmp(got, cut, 30));
   /* With one 'a' less it fits whole. */
   got = t_name_kept(cut + 1, &scr);
   CHECK(got && !strcmp(got, cut + 1));

   got = t_name_kept("A\nB\tC\x7f", &scr);
   CHECK(got && !strcmp(got, "A B C "));
}

static void test_screens_select_masks(void)
{
   struct retro_video_views in;
   video_views_map_t full, sub;
   video_views_screens_t scr;
   unsigned index[RETRO_VIDEO_VIEWS_MAX];

   t_screens_full(&in);
   CHECK(video_views_validate_map(&in, &full, &scr));
   CHECK(video_views_select_screens(&full, 0x6, &sub, index));
   CHECK(sub.num_screens == 2 && sub.num_views == 1);
   CHECK(sub.no_input == 0x3 && sub.viewless == 0x2);
   CHECK(video_views_select_screens(&full, 0x3, &sub, index));
   CHECK(sub.no_input == 0x2 && sub.viewless == 0);
   /* A screen with no view, alone, selects nothing. */
   CHECK(!video_views_select_screens(&full, 0x4, &sub, index));
   CHECK(sub.num_screens == 0 && sub.no_input == 0 && sub.viewless == 0);
}

/* The frontend's masks follow the selected screens too. */
static void test_select_screens_left_out(void)
{
   struct retro_video_views in;
   video_views_map_t full, out;
   video_views_screens_t scr;
   unsigned index[RETRO_VIDEO_VIEWS_MAX];

   t_screens_full(&in);
   CHECK(video_views_validate_map(&in, &full, &scr));
   CHECK(full.left_out == 0 && full.blank == 0);
   full.left_out = 0x5;
   full.blank    = 0x2;
   CHECK(video_views_select_screens(&full, 0x6, &out, index));
   CHECK(out.left_out == 0x2 && out.blank == 0x1);
}

/* A console's touchscreen through the views it shows: the 3DS's bottom
 * screen showing the DS map's second screen. */
static void test_touch(void)
{
   video_views_map_t map, sub;
   video_views_layout_params_t p;
   video_views_layout_t l;
   video_views_touch_t t;
   unsigned index[RETRO_VIDEO_VIEWS_MAX];
   int16_t x = 123, y = 456;
   unsigned frame = VIDEO_SCALE_PACK(256, 384);

   memset(&t, 0, sizeof(t));
   CHECK(!video_views_touch_point(&t, 0, 0, &x, &y));
   CHECK(x == 123 && y == 456);

   map_ds(&map);
   CHECK(video_views_select_screens(&map, 0x2, &sub, index));
   memset(&p, 0, sizeof(p));
   p.map         = &sub;
   p.dims        = VIDEO_SCALE_PACK(320, 240);
   p.stereo_mode = VIDEO_STEREO_MODE_2D;
   video_views_layout(&p, &l);
   /* 256x192 into 320x240: the whole screen, aspect kept. */
   CHECK(l.num_placements == 1
         && l.placements[0].dst.dims == VIDEO_SCALE_PACK(320, 240));
   video_views_touch_publish(&t, &map, &l, index, p.dims, frame);
   CHECK(!(retro_atomic_load_acquire_int(&t.seq) & 1));

   /* The display's middle is the second screen's: (128, 288) packed. */
   CHECK(video_views_touch_point(&t, 0, 0, &x, &y));
   CHECK(x >= -2 && x <= 2);
   CHECK(y >= 16383 - 200 && y <= 16383 + 200);
   /* The top left corner is the second screen's: (0, 192). */
   CHECK(video_views_touch_point(&t, -0x7fff, -0x7fff, &x, &y));
   CHECK(x == -0x7fff && y >= -2 && y <= 2);

   /* Pillarboxed: off the view is off the frame. */
   p.dims = VIDEO_SCALE_PACK(400, 240);
   video_views_layout(&p, &l);
   video_views_touch_publish(&t, &map, &l, index, p.dims, frame);
   CHECK(video_views_touch_point(&t, -0x7fff, 0, &x, &y));
   CHECK(x == -0x8000 && y == -0x8000);

   /* Taken back: none. */
   video_views_touch_publish(&t, NULL, NULL, NULL, 0, 0);
   x = 7;
   CHECK(!video_views_touch_point(&t, 0, 0, &x, &y) && x == 7);
}

/* The CPU drivers' composer: a layout made for a screen twice the DS
 * map's size is composed at half that, each view copied as it is, for
 * display hardware to enlarge; and side by side at full size. */
/* What a blitter scales into each placement's visible part */
static void test_cut_source(void)
{
   struct retro_video_view v = { 0 };
   video_views_rect_t dst, cut, out;

   v.x = 10; v.y = 20; v.width = 100; v.height = 50;
   /* Drawn whole, at twice its size: all of it */
   dst.pos  = VIDEO_POS_PACK(4, 6);
   dst.dims = VIDEO_SCALE_PACK(200, 100);
   video_views_cut_source(&v, &dst, &dst, &out);
   CHECK(VIDEO_POS_X(out.pos) == 10 && VIDEO_POS_Y(out.pos) == 20);
   CHECK(VIDEO_SCALE_W(out.dims) == 100 && VIDEO_SCALE_H(out.dims) == 50);
   /* Its right half and bottom half cut away */
   cut.pos  = dst.pos;
   cut.dims = VIDEO_SCALE_PACK(100, 50);
   video_views_cut_source(&v, &dst, &cut, &out);
   CHECK(VIDEO_POS_X(out.pos) == 10 && VIDEO_POS_Y(out.pos) == 20);
   CHECK(VIDEO_SCALE_W(out.dims) == 50 && VIDEO_SCALE_H(out.dims) == 25);
   /* Off the left and top of the screen: the view's far quarter */
   dst.pos  = VIDEO_POS_PACK(-150, -75);
   cut.pos  = VIDEO_POS_PACK(0, 0);
   cut.dims = VIDEO_SCALE_PACK(50, 25);
   video_views_cut_source(&v, &dst, &cut, &out);
   CHECK(VIDEO_POS_X(out.pos) == 85 && VIDEO_POS_Y(out.pos) == 57);
   CHECK(VIDEO_SCALE_W(out.dims) == 25 && VIDEO_SCALE_H(out.dims) == 13);
   /* Shrunk to less than a pixel's worth: still a pixel */
   dst.pos  = VIDEO_POS_PACK(0, 0);
   dst.dims = VIDEO_SCALE_PACK(10, 5);
   cut.pos  = VIDEO_POS_PACK(9, 4);
   cut.dims = VIDEO_SCALE_PACK(1, 1);
   video_views_cut_source(&v, &dst, &cut, &out);
   CHECK(VIDEO_POS_X(out.pos) == 100 && VIDEO_POS_Y(out.pos) == 60);
   CHECK(VIDEO_SCALE_W(out.dims) == 10 && VIDEO_SCALE_H(out.dims) == 10);
   /* A one-pixel view stays one pixel */
   v.width  = 1;
   v.height = 1;
   dst.dims = VIDEO_SCALE_PACK(40, 40);
   cut.pos  = VIDEO_POS_PACK(39, 39);
   video_views_cut_source(&v, &dst, &cut, &out);
   CHECK(VIDEO_POS_X(out.pos) == 10 && VIDEO_POS_Y(out.pos) == 20);
   CHECK(VIDEO_SCALE_W(out.dims) == 1 && VIDEO_SCALE_H(out.dims) == 1);
}

static void test_cpu_compose(void)
{
   unsigned i, x, y;
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;
   video_views_cpu_t cpu;
   static uint32_t frame[256 * 384];
   static uint32_t out[512 * 384];
   unsigned dims;
   float scale;
   bool ok = true;

   memset(&cpu, 0, sizeof(cpu));
   for (y = 0; y < 384; y++)
      for (x = 0; x < 256; x++)
         frame[y * 256 + x] = (y << 12) | (x << 1) | (y >= 192);

   map_ds(&map);
   memset(&p, 0, sizeof(p));
   p.map         = &map;
   p.dims        = VIDEO_SCALE_PACK(512, 768);
   p.stereo_mode = VIDEO_STEREO_MODE_2D;
   video_views_layout(&p, &l);
   scale = video_views_cpu_scale(&map, &l);
   CHECK(scale == 0.5f);
   dims  = video_views_cpu_dims(p.dims, scale);
   CHECK(dims == VIDEO_SCALE_PACK(256, 384));
   video_views_cpu_compose(&cpu, &map, &l, frame, 256 * 4,
         SCALER_FMT_ARGB8888, SCALER_TYPE_POINT, scale, dims, out, 256,
         NULL);
   /* The screens stacked as they are packed: the frame itself */
   for (i = 0; i < 256 * 384; i++)
      if ((out[i] & 0xffffff) != (frame[i] & 0xffffff))
         ok = false;
   CHECK(ok);

   /* Side by side, full: each eye's half its own. */
   p.dims        = VIDEO_SCALE_PACK(512, 384);
   p.stereo_mode = VIDEO_STEREO_MODE_SBS_FULL;
   video_views_layout(&p, &l);
   video_views_cpu_compose(&cpu, &map, &l, frame, 256 * 4,
         SCALER_FMT_ARGB8888, SCALER_TYPE_POINT, 1.0f, p.dims, out, 512,
         NULL);
   /* Both halves show the top screen at their top left */
   CHECK((out[0] & 0xffffff) == (frame[0] & 0xffffff));
   CHECK((out[256] & 0xffffff) == (frame[0] & 0xffffff));

   /* Turned a quarter counter-clockwise, each view in its own place:
    * the top screen's right column read downward becomes its top row */
   p.dims        = VIDEO_SCALE_PACK(384, 512);
   p.stereo_mode = VIDEO_STEREO_MODE_2D;
   p.stretch     = true;
   p.rotation    = 1;
   video_views_layout(&p, &l);
   cpu.rot       = 1;
   memset(out, 0, sizeof(out));
   video_views_cpu_compose(&cpu, &map, &l, frame, 256 * 4,
         SCALER_FMT_ARGB8888, SCALER_TYPE_POINT, 1.0f, p.dims, out, 384,
         NULL);
   {
      video_views_rect_t r = l.placements[0].dst;
      int px = VIDEO_POS_X(r.pos), py = VIDEO_POS_Y(r.pos);
      unsigned rw = VIDEO_SCALE_W(r.dims), rh = VIDEO_SCALE_H(r.dims);
      /* Turned, the view stands taller than it is wide */
      CHECK(rw > 0 && rh > rw);
      /* Its top left is the view's top right; its bottom left the
       * view's top left */
      CHECK((out[(size_t)py * 384 + px] & 0xffffff)
            == (frame[255] & 0xffffff));
      CHECK((out[(size_t)(py + rh - 1) * 384 + px] & 0xffffff)
            == (frame[0] & 0xffffff));
   }
   cpu.rot       = 0;
   p.rotation    = 0;
   p.stretch     = false;

   /* Interlaced is composed at the screen's own size. */
   p.dims        = VIDEO_SCALE_PACK(512, 384);
   p.stereo_mode = VIDEO_STEREO_MODE_INTERLACED;
   video_views_layout(&p, &l);
   CHECK(video_views_cpu_scale(&map, &l) == 1.0f);
   video_views_cpu_free(&cpu);
}

/* A blank screen's place is cleared and nothing drawn in it. */
static void test_cpu_draw_blank(void)
{
   unsigned i, x, y;
   video_views_map_t map;
   video_views_layout_t l;
   video_views_cpu_t cpu;
   const video_views_rect_t *r;
   static uint32_t frame[256 * 384];
   static uint32_t out[1600 * 960];
   bool ok = true;

   memset(&cpu, 0, sizeof(cpu));
   for (i = 0; i < 256 * 384; i++)
      frame[i] = (i < 256 * 192) ? 0x336699 : 0xcc9966;
   for (i = 0; i < 1600 * 960; i++)
      out[i] = 0xffffffff;
   map_ds(&map);
   map.blank = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 2);
   video_views_cpu_compose(&cpu, &map, &l, frame, 256 * 4,
         SCALER_FMT_ARGB8888, SCALER_TYPE_POINT, 1.0f,
         VIDEO_SCALE_PACK(1600, 960), out, 1600, NULL);
   r = &l.placements[1].dst;
   for (y = 0; y < VIDEO_SCALE_H(r->dims); y++)
      for (x = 0; x < VIDEO_SCALE_W(r->dims); x++)
         if (out[(size_t)(VIDEO_POS_Y(r->pos) + y) * 1600
               + VIDEO_POS_X(r->pos) + x])
            ok = false;
   CHECK(ok);
   r = &l.placements[0].dst;
   CHECK((out[(size_t)(VIDEO_POS_Y(r->pos) + VIDEO_SCALE_H(r->dims) / 2)
            * 1600 + VIDEO_POS_X(r->pos) + VIDEO_SCALE_W(r->dims) / 2]
         & 0xffffff) == 0x336699);
   video_views_cpu_free(&cpu);
}

/* A blank inset is black over its base. */
static void test_cpu_draw_blank_inset(void)
{
   unsigned i, x, y;
   video_views_map_t map;
   video_views_layout_t l;
   video_views_cpu_t cpu;
   const video_views_rect_t *base, *in;
   static uint32_t frame[256 * 384];
   static uint32_t out[1600 * 960];
   bool black = true, around = true;

   memset(&cpu, 0, sizeof(cpu));
   for (i = 0; i < 256 * 384; i++)
      frame[i] = (i < 256 * 192) ? 0x336699 : 0xcc9966;
   map_ds_inset(&map);
   map.blank = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 2);
   video_views_cpu_compose(&cpu, &map, &l, frame, 256 * 4,
         SCALER_FMT_ARGB8888, SCALER_TYPE_POINT, 1.0f,
         VIDEO_SCALE_PACK(1600, 960), out, 1600, NULL);
   base = &l.placements[0].dst;
   in   = &l.placements[1].dst;
   CHECK(rect_is(in, 1120, 720, 320, 240));
   for (y = 0; y < VIDEO_SCALE_H(base->dims); y++)
      for (x = 0; x < VIDEO_SCALE_W(base->dims); x++)
      {
         int px     = VIDEO_POS_X(base->pos) + (int)x;
         int py     = VIDEO_POS_Y(base->pos) + (int)y;
         uint32_t c = out[(size_t)py * 1600 + px];
         if (     px >= VIDEO_POS_X(in->pos)
               && px <  VIDEO_POS_X(in->pos) + (int)VIDEO_SCALE_W(in->dims)
               && py >= VIDEO_POS_Y(in->pos)
               && py <  VIDEO_POS_Y(in->pos) + (int)VIDEO_SCALE_H(in->dims))
         {
            if (c)
               black = false;
         }
         else if ((c & 0xffffff) != 0x336699)
            around = false;
      }
   CHECK(black);
   CHECK(around);
   video_views_cpu_free(&cpu);
}

/* Nothing drawn: composed at the window's size. */
static void test_cpu_scale_none(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   float scale;

   map_ds(&map);
   map.left_out = 0x3;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 0);
   CHECK(video_views_cpu_scale(&map, &l) == 1.0f);

   /* The only placement blank. */
   map.left_out = 0x2;
   map.blank    = 0x1;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 1);
   scale = video_views_cpu_scale(&map, &l);
   CHECK(scale == 1.0f);
   CHECK(video_views_cpu_dims(VIDEO_SCALE_PACK(1600, 960), scale)
         == VIDEO_SCALE_PACK(1600, 960));
}

/* The CPU drivers' blend of a canvas's halves. */
static void test_blend(void)
{
   /* Two 2x2 halves side by side, and an output wider than the eye. */
   uint32_t canvas[2 * 4];
   uint32_t out[2 * 3];
   unsigned i;

   for (i = 0; i < 2; i++)
   {
      canvas[i * 4 + 0] = 0xffffff;
      canvas[i * 4 + 1] = 0x000000;
      canvas[i * 4 + 2] = 0x000000;
      canvas[i * 4 + 3] = 0xffffff;
   }
   for (i = 0; i < 6; i++)
      out[i] = 0x123456;

   video_views_blend_xrgb8888(canvas, 4, out, 3, VIDEO_SCALE_PACK(2, 2),
         VIDEO_STEREO_MODE_ANAGLYPH);
   /* White left only is red, white right only cyan. */
   CHECK(out[0] == 0xff0000 && out[1] == 0x00ffff);
   CHECK(out[3] == 0xff0000 && out[4] == 0x00ffff);
   /* Past the eye's width is left alone. */
   CHECK(out[2] == 0x123456 && out[5] == 0x123456);

   canvas[4] = 0x0000ff;
   canvas[6] = 0x00ff00;
   video_views_blend_xrgb8888(canvas, 4, out, 3, VIDEO_SCALE_PACK(2, 2),
         VIDEO_STEREO_MODE_INTERLACED);
   /* Row 0 from the left half, row 1 from the right. */
   CHECK(out[0] == 0xffffff && out[1] == 0x000000);
   CHECK(out[3] == 0x00ff00 && out[4] == 0xffffff);
}

/* ---- the compositor, instantiated over fakes ---- */

typedef struct fake_chain
{
   int id;
} fake_chain_t;

typedef struct fake_target
{
   int made;
} fake_target_t;

typedef struct fake_drv
{
   int stock_made, stock_fail, synced;
   int made, retired, retire_refuses, make_fails;
} fake_drv_t;

typedef struct fake_ctx
{
   int tag;
} fake_ctx_t;

static const fake_ctx_t fake_ctx = { 42 };
static int bad_ctx;

#define MAX_DRAWS 16
static struct
{
   char kind;             /* D direct, P pass, A alpha, F fill, I image */
   int view;              /* -1: a pass; an image's screen */
   fake_chain_t *chain;
   video_views_rect_t dst, cut;
   int again;
   int nearest;
   unsigned alpha;
} draws[MAX_DRAWS];
static int num_draws;
static fake_chain_t chain_objs[RETRO_VIDEO_VIEWS_MAX * 2];

static void *fake_stock(fake_drv_t *d)
{
   if (d->stock_fail)
      return NULL;
   return &chain_objs[RETRO_VIDEO_VIEWS_MAX + d->stock_made++];
}

static void fake_sync(fake_drv_t *d, fake_chain_t *c)
{
   (void)c;
   d->synced++;
}

static void fake_direct(fake_drv_t *d, const fake_ctx_t *x, unsigned view,
      const video_views_rect_t *dst, const video_views_rect_t *cut)
{
   (void)d;
   if (x != &fake_ctx)
      bad_ctx++;
   if (num_draws >= MAX_DRAWS)
      return;
   draws[num_draws].kind  = 'D';
   draws[num_draws].view  = (int)view;
   draws[num_draws].chain = NULL;
   draws[num_draws].dst   = *dst;
   draws[num_draws].cut   = *cut;
   draws[num_draws].again = 0;
   num_draws++;
}

static void fake_pass(fake_drv_t *d, const fake_ctx_t *x, fake_chain_t *c,
      const video_views_rect_t *dst, const video_views_rect_t *cut,
      bool again)
{
   (void)d;
   if (x != &fake_ctx)
      bad_ctx++;
   if (num_draws >= MAX_DRAWS)
      return;
   draws[num_draws].kind  = 'P';
   draws[num_draws].view  = -1;
   draws[num_draws].chain = c;
   draws[num_draws].dst   = *dst;
   draws[num_draws].cut   = *cut;
   draws[num_draws].again = again ? 1 : 0;
   num_draws++;
}

static void fake_alpha(fake_drv_t *d, const fake_ctx_t *x, unsigned alpha)
{
   (void)d;
   if (x != &fake_ctx)
      bad_ctx++;
   if (num_draws >= MAX_DRAWS)
      return;
   draws[num_draws].kind  = 'A';
   draws[num_draws].alpha = alpha;
   num_draws++;
}

static void fake_fill(fake_drv_t *d, const fake_ctx_t *x,
      const video_views_rect_t *cut, unsigned alpha)
{
   (void)d;
   if (x != &fake_ctx)
      bad_ctx++;
   if (num_draws >= MAX_DRAWS)
      return;
   draws[num_draws].kind  = 'F';
   draws[num_draws].cut   = *cut;
   draws[num_draws].alpha = alpha;
   num_draws++;
}

static const video_views_map_t *fake_map;

static const video_views_map_t *fake_image_map(fake_drv_t *d,
      const fake_ctx_t *x)
{
   (void)d;
   if (x != &fake_ctx)
      bad_ctx++;
   return fake_map;
}

static void fake_image(fake_drv_t *d, const fake_ctx_t *x, unsigned screen,
      const video_views_rect_t *dst, const video_views_rect_t *cut,
      bool nearest)
{
   (void)d;
   if (x != &fake_ctx)
      bad_ctx++;
   if (num_draws >= MAX_DRAWS)
      return;
   draws[num_draws].kind    = 'I';
   draws[num_draws].view    = (int)screen;
   draws[num_draws].chain   = NULL;
   draws[num_draws].dst     = *dst;
   draws[num_draws].cut     = *cut;
   draws[num_draws].nearest = nearest ? 1 : 0;
   num_draws++;
}

/* The calls logged, one letter each, are @want */
static int t_calls_are(const char *want)
{
   int i;
   if ((int)strlen(want) != num_draws)
      return 0;
   for (i = 0; i < num_draws; i++)
      if (draws[i].kind != want[i])
         return 0;
   return 1;
}

static bool fake_make(fake_drv_t *d, fake_target_t *t, unsigned dims)
{
   (void)dims;
   d->made++;
   t->made = !d->make_fails;
   return !d->make_fails;
}

static bool fake_retire(fake_drv_t *d, fake_target_t *t)
{
   if (d->retire_refuses)
      return false;
   d->retired++;
   t->made = 0;
   return true;
}

#define VV_FN(name)                     fake_vv_##name
#define VV_DRV                          fake_drv_t
#define VV_CHAIN                        fake_chain_t
#define VV_CREATE_STOCK(drv)            fake_stock(drv)
#define VV_SYNC_PARAMS(drv, c)          fake_sync(drv, c)
#define VV_DRAW                         fake_ctx_t
#define VV_DIRECT(drv, x, v, dst, cut)  fake_direct(drv, x, v, dst, cut)
#define VV_PASS(drv, x, c, dst, cut, a) fake_pass(drv, x, c, dst, cut, a)
#define VV_ALPHA(drv, x, a)             fake_alpha(drv, x, a)
#define VV_FILL(drv, x, cut, a)         fake_fill(drv, x, cut, a)
#define VV_IMAGE_MAP(drv, x)            fake_image_map(drv, x)
#define VV_IMAGE(drv, x, s, dst, cut, n) fake_image(drv, x, s, dst, cut, n)
#define VV_TARGET                       fake_target_t
#define VV_TARGET_MAKE(drv, t, dims)    fake_make(drv, t, dims)
#define VV_TARGET_RETIRE(drv, t)        fake_retire(drv, t)
#define VV_ROWS_UP
#include "gfx/video_views_draw.h"

/* The same driver without the alpha, fill and image hooks */
#define VV_FN(name)                     plain_vv_##name
#define VV_DRV                          fake_drv_t
#define VV_CHAIN                        fake_chain_t
#define VV_CREATE_STOCK(drv)            fake_stock(drv)
#define VV_SYNC_PARAMS(drv, c)          fake_sync(drv, c)
#define VV_DRAW                         fake_ctx_t
#define VV_DIRECT(drv, x, v, dst, cut)  fake_direct(drv, x, v, dst, cut)
#define VV_PASS(drv, x, c, dst, cut, a) fake_pass(drv, x, c, dst, cut, a)
#define VV_TARGET                       fake_target_t
#define VV_TARGET_MAKE(drv, t, dims)    fake_make(drv, t, dims)
#define VV_TARGET_RETIRE(drv, t)        fake_retire(drv, t)
#define VV_ROWS_UP
#include "gfx/video_views_draw.h"

static void layout_3ds(video_views_map_t *map, video_views_layout_t *l,
      unsigned w, unsigned h, unsigned mode)
{
   video_views_layout_params_t p;
   map_3ds(map);
   params_init(&p, map, w, h);
   p.stereo_mode = mode;
   video_views_layout(&p, l);
}

static void test_pick_chains(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   video_views_chains_t set;
   fake_drv_t d;
   fake_chain_t *chains[RETRO_VIDEO_VIEWS_MAX];
   video_views_rect_t rects[RETRO_VIDEO_VIEWS_MAX];
   unsigned full = VIDEO_SCALE_PACK(1600, 960);

   layout_3ds(&map, &l, 1600, 960, VIDEO_STEREO_MODE_SBS_FULL);
   memset(&set, 0, sizeof(set));
   memset(&d, 0, sizeof(d));
   set.count = 3;

   /* Stock chains made on first use, once each, and nothing synced. */
   CHECK(fake_vv_pick_chains(&d, &set, &map, &l, true, true, full, chains,
            rects));
   CHECK(d.stock_made == 3 && d.synced == 0);
   CHECK(chains[0] && chains[1] && chains[2] && chains[0] != chains[1]);
   CHECK(rect_is(&rects[1], 800, 0, 800, 480));
   CHECK(fake_vv_pick_chains(&d, &set, &map, &l, true, true, full, chains,
            rects));
   CHECK(d.stock_made == 3);

   /* The left area only: the right eye isn't drawn, so has no chain. */
   CHECK(fake_vv_pick_chains(&d, &set, &map, &l, true, false, full, chains,
            rects));
   CHECK(chains[0] && !chains[1] && chains[2]);

   /* Preset chains are the driver's: none made here, each synced. */
   CHECK(!fake_vv_pick_chains(&d, &set, &map, &l, false, true, full, chains,
            rects));
   set.preset[0] = &chain_objs[0];
   set.preset[1] = &chain_objs[1];
   set.preset[2] = &chain_objs[2];
   CHECK(fake_vv_pick_chains(&d, &set, &map, &l, false, true, full, chains,
            rects));
   CHECK(d.synced == 3 && chains[2] == &chain_objs[2] && d.stock_made == 3);
   CHECK(!video_views_chains_behind(&set, &map));

   /* A frame queued before the count shrank: behind, so it keeps the
    * last FALLBACK. */
   set.count = 2;
   CHECK(!fake_vv_pick_chains(&d, &set, &map, &l, false, true, full, chains,
            rects));
   CHECK(video_views_chains_behind(&set, &map));

   /* A stock chain that can't be made is not tried every frame. */
   memset(&set, 0, sizeof(set));
   set.count    = 3;
   d.stock_fail = 1;
   CHECK(!fake_vv_pick_chains(&d, &set, &map, &l, true, true, full, chains,
            rects));
   CHECK(set.stock_failed);
   d.stock_fail = 0;
   CHECK(!fake_vv_pick_chains(&d, &set, &map, &l, true, true, full, chains,
            rects));
   CHECK(d.stock_made == 3);
}

static void test_draw_placements(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   fake_drv_t d;
   fake_chain_t *chains[RETRO_VIDEO_VIEWS_MAX];
   unsigned full = VIDEO_SCALE_PACK(1600, 960);

   layout_3ds(&map, &l, 1600, 960, VIDEO_STEREO_MODE_SBS_FULL);
   memset(&d, 0, sizeof(d));
   memset(chains, 0, sizeof(chains));
   chains[0] = &chain_objs[0];
   chains[1] = &chain_objs[1];
   chains[2] = &chain_objs[2];

   /* Each placement through its chain; the bottom screen, in both eyes,
    * the second time again. */
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, false, false);
   CHECK(num_draws == 4);
   CHECK(draws[0].chain == chains[0] && !draws[0].again);
   CHECK(draws[1].chain == chains[2] && !draws[1].again);
   CHECK(draws[2].chain == chains[1] && !draws[2].again);
   CHECK(draws[3].chain == chains[2] && draws[3].again);
   CHECK(rect_is(&draws[3].dst, 880, 480, 640, 480));

   /* The left area only, and a view without a chain left out. */
   num_draws = 0;
   chains[2] = NULL;
   fake_vv_draw_placements(&d, &fake_ctx, &l,
         chains, false, full, false, false);
   CHECK(num_draws == 1 && draws[0].chain == chains[0]);
   chains[2] = &chain_objs[2];

   /* Straight from the frame: by view, never through a chain. */
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(num_draws == 4 && draws[0].view == 0 && draws[2].view == 1
         && draws[3].view == 2);

   /* A placement cut to its target. */
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l,
         chains, true, VIDEO_SCALE_PACK(1200, 960),
         false, false);
   CHECK(num_draws == 4 && rect_is(&draws[3].cut, 880, 480, 320, 480)
         && rect_is(&draws[3].dst, 880, 480, 640, 480));

   /* Interlaced into a 3200x960 canvas whose rows count from the bottom:
    * with an even height the eyes trade halves - the left eye's top
    * screen at 400, the right's at 2000. */
   layout_3ds(&map, &l, 1600, 960, VIDEO_STEREO_MODE_INTERLACED);
   CHECK(l.offscreen);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l,
         chains, true, l.canvas_dims, false, true);
   CHECK(num_draws == 4);
   CHECK(VIDEO_POS_X(draws[0].dst.pos) == 2000
         && VIDEO_POS_X(draws[0].cut.pos) == 2000);
   CHECK(VIDEO_POS_X(draws[2].dst.pos) == 400
         && VIDEO_POS_X(draws[2].cut.pos) == 400);
   /* From the top they stay. */
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l,
         chains, true, l.canvas_dims, false, false);
   CHECK(num_draws == 4 && VIDEO_POS_X(draws[0].dst.pos) == 400);
   /* And with an odd height. */
   layout_3ds(&map, &l, 1600, 961, VIDEO_STEREO_MODE_INTERLACED);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l,
         chains, true, l.canvas_dims, false, true);
   CHECK(num_draws == 4
         && VIDEO_POS_X(draws[0].dst.pos)
            == VIDEO_POS_X(l.placements[0].dst.pos));
   CHECK(!bad_ctx);
}

/* A blank screen is not drawn, through its chain or straight. */
static void test_draw_placements_blank(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   video_views_chains_t set;
   fake_drv_t d;
   fake_chain_t *chains[RETRO_VIDEO_VIEWS_MAX];
   fake_chain_t *picked[RETRO_VIDEO_VIEWS_MAX];
   video_views_rect_t rects[RETRO_VIDEO_VIEWS_MAX];
   unsigned full = VIDEO_SCALE_PACK(1600, 960);

   map_ds(&map);
   memset(&d, 0, sizeof(d));
   memset(chains, 0, sizeof(chains));
   chains[0] = &chain_objs[0];
   chains[1] = &chain_objs[1];

   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, false, false);
   CHECK(num_draws == 2);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(num_draws == 2);

   map.blank = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, false, false);
   CHECK(num_draws == 1 && draws[0].chain == chains[0] && !draws[0].again);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(num_draws == 1 && draws[0].view == 0);
   CHECK(!bad_ctx);
   /* Nor is its view given a chain: nothing reads its pixels */
   memset(&set, 0, sizeof(set));
   set.count = 2;
   CHECK(fake_vv_pick_chains(&d, &set, &map, &l, true, true, full, picked,
            rects));
   CHECK(picked[0] && !picked[1] && d.stock_made == 1);
   CHECK(!video_views_first_drawn(&l, 1, true, full, &rects[1]));
}

/* An inset drawn at its opacity, and the driver left solid after. */
static void test_draw_placements_alpha(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   fake_drv_t d;
   fake_chain_t *chains[RETRO_VIDEO_VIEWS_MAX];
   unsigned full = VIDEO_SCALE_PACK(1600, 960);

   memset(&d, 0, sizeof(d));
   memset(chains, 0, sizeof(chains));
   chains[0] = &chain_objs[0];
   chains[1] = &chain_objs[1];
   chains[2] = &chain_objs[2];

   map_ds_inset(&map);
   map.opacity[1] = 128;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(t_calls_are("DADA"));
   CHECK(   draws[0].view == 0 && draws[1].alpha == 128
         && draws[2].view == 1 && draws[3].alpha == 255);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, false, false);
   CHECK(t_calls_are("PAPA"));
   CHECK(   draws[0].chain == chains[0] && draws[1].alpha == 128
         && draws[2].chain == chains[1] && draws[3].alpha == 255);

   /* Two insets at one opacity: set once. */
   map_three(&map);
   map.over       = 0x6;
   map.base       = 0;
   map.anchor     = (RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT << 4)
                  | (RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_LEFT  << 8);
   map.size[1]    = 0.25f;
   map.size[2]    = 0.25f;
   map.opacity[1] = 128;
   map.opacity[2] = 128;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(t_calls_are("DADDA"));
   CHECK(draws[1].alpha == 128 && draws[4].alpha == 255);

   /* All solid: no alpha set. */
   map_ds_inset(&map);
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(t_calls_are("DD"));
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, false, false);
   CHECK(t_calls_are("PP"));
   CHECK(!bad_ctx);
}

/* A blank inset filled black at its opacity, without a chain; a blank
 * root left alone. */
static void test_draw_placements_fill(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   video_views_rect_t want;
   fake_drv_t d;
   fake_chain_t *chains[RETRO_VIDEO_VIEWS_MAX];
   unsigned full = VIDEO_SCALE_PACK(1600, 960);

   memset(&d, 0, sizeof(d));
   memset(chains, 0, sizeof(chains));
   chains[0] = &chain_objs[0];

   map_ds_inset(&map);
   map.blank      = 0x2;
   map.opacity[1] = 128;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(   video_views_visible(&l, &l.placements[1], full, &want)
         && rect_is(&want, 1120, 720, 320, 240));
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(t_calls_are("DAFA"));
   CHECK(   draws[0].view == 0 && draws[1].alpha == 128
         && draws[2].alpha == 128
         && rect_is(&draws[2].cut, 1120, 720, 320, 240)
         && draws[3].alpha == 255);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, false, false);
   CHECK(t_calls_are("PAFA"));
   CHECK(draws[2].alpha == 128 && rect_is(&draws[2].cut, 1120, 720, 320, 240));

   /* Solid: filled with no alpha set. */
   map.opacity[1] = 255;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(t_calls_are("DF") && draws[1].alpha == 255);

   /* Interlaced from the bottom: the fill trades halves with its eye. */
   {
      video_views_layout_params_t p;
      int shift;
      params_init(&p, &map, 1600, 960);
      p.screen_layout = VIDEO_SCREEN_LAYOUT_HORIZONTAL;
      p.stereo_mode   = VIDEO_STEREO_MODE_INTERLACED;
      video_views_layout(&p, &l);
      shift = (int)VIDEO_SCALE_W(l.canvas_dims) / 2;
      num_draws = 0;
      fake_vv_draw_placements(&d, &fake_ctx, &l,
            chains, true, l.canvas_dims, false, true);
      CHECK(t_calls_are("PFPF"));
      CHECK(l.placements[1].area == 0 && l.placements[3].area == 1);
      CHECK(video_views_visible(&l, &l.placements[1], l.canvas_dims, &want));
      CHECK(VIDEO_POS_X(draws[1].cut.pos) == VIDEO_POS_X(want.pos) + shift);
      CHECK(video_views_visible(&l, &l.placements[3], l.canvas_dims, &want));
      CHECK(VIDEO_POS_X(draws[3].cut.pos) == VIDEO_POS_X(want.pos) - shift);
   }

   /* A blank root is not filled. */
   map_ds_inset(&map);
   map.blank = 0x1;
   chains[1] = &chain_objs[1];
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(t_calls_are("D") && draws[0].view == 1);
   CHECK(!bad_ctx);
}

/* Without the hooks an inset is drawn solid and a blank one not at all. */
static void test_draw_placements_no_hooks(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   fake_drv_t d;
   fake_chain_t *chains[RETRO_VIDEO_VIEWS_MAX];
   unsigned full = VIDEO_SCALE_PACK(1600, 960);

   /* Only draw_placements is wanted of this instantiation */
   (void)plain_vv_pick_chains;
   (void)plain_vv_target_ensure;
   (void)plain_vv_target_drop;
   memset(&d, 0, sizeof(d));
   memset(chains, 0, sizeof(chains));
   chains[0] = &chain_objs[0];
   chains[1] = &chain_objs[1];

   map_ds_inset(&map);
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   num_draws = 0;
   plain_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(t_calls_are("DD") && draws[0].view == 0 && draws[1].view == 1);
   map.opacity[1] = 128;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   num_draws = 0;
   plain_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, false, false);
   CHECK(   t_calls_are("PP") && draws[0].chain == chains[0]
         && draws[1].chain == chains[1]);

   map.blank = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   num_draws = 0;
   plain_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(t_calls_are("D") && draws[0].view == 0);
   num_draws = 0;
   plain_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, false, false);
   CHECK(t_calls_are("P") && draws[0].chain == chains[0]);
   CHECK(!bad_ctx);
}

static void test_targets(void)
{
   video_views_target_t t;
   fake_target_t obj;
   fake_drv_t d;
   memset(&t, 0, sizeof(t));
   memset(&obj, 0, sizeof(obj));
   memset(&d, 0, sizeof(d));

   CHECK(!fake_vv_target_ensure(&d, &t, &obj, 0) && d.made == 0);
   CHECK(fake_vv_target_ensure(&d, &t, &obj, VIDEO_SCALE_PACK(64, 32))
         && d.made == 1);
   /* The same size is kept. */
   CHECK(fake_vv_target_ensure(&d, &t, &obj, VIDEO_SCALE_PACK(64, 32))
         && d.made == 1 && d.retired == 0);
   /* Another is remade, the old one let go first. */
   CHECK(fake_vv_target_ensure(&d, &t, &obj, VIDEO_SCALE_PACK(80, 32))
         && d.made == 2 && d.retired == 1);
   /* Not while it can't be let go. */
   d.retire_refuses = 1;
   CHECK(!fake_vv_target_ensure(&d, &t, &obj, VIDEO_SCALE_PACK(96, 32))
         && d.made == 2);
   fake_vv_target_drop(&d, &t, &obj);
   CHECK(t.made && t.dims == VIDEO_SCALE_PACK(80, 32));
   d.retire_refuses = 0;
   /* A size that failed isn't tried again until the size changes. */
   d.make_fails = 1;
   CHECK(!fake_vv_target_ensure(&d, &t, &obj, VIDEO_SCALE_PACK(96, 32))
         && d.made == 3);
   CHECK(!fake_vv_target_ensure(&d, &t, &obj, VIDEO_SCALE_PACK(96, 32))
         && d.made == 3 && d.retired == 2);
   d.make_fails = 0;
   CHECK(fake_vv_target_ensure(&d, &t, &obj, VIDEO_SCALE_PACK(64, 64))
         && d.made == 4 && d.retired == 2);
   fake_vv_target_drop(&d, &t, &obj);
   CHECK(!t.made && !t.dims && d.retired == 3);
   fake_vv_target_drop(&d, &t, &obj);
   CHECK(d.retired == 3);
}

static int widget_polls;
static bool poll_widgets(void)
{
   widget_polls++;
   return true;
}

static void test_ui_helpers(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   video_views_ui_dims_t saved;
   unsigned dims = 11, video_dims = 22;
   const char *none = NULL;

   layout_3ds(&map, &l, 1600, 960, VIDEO_STEREO_MODE_SBS_FULL);
   CHECK(video_views_ui_per_eye(&map, &l, true));
   CHECK(!video_views_ui_per_eye(&map, &l, false));
   layout_3ds(&map, &l, 1600, 960, VIDEO_STEREO_MODE_2D);
   CHECK(!video_views_ui_per_eye(&map, &l, true));

   CHECK(!VIDEO_VIEWS_UI_WANTED(true, false, none, false, false));
   CHECK(!VIDEO_VIEWS_UI_WANTED(true, false, "", false, false));
   CHECK(VIDEO_VIEWS_UI_WANTED(true, false, "msg", false, false));
   CHECK(VIDEO_VIEWS_UI_WANTED(true, true, none, false, false));
   CHECK(VIDEO_VIEWS_UI_WANTED(true, false, none, true, false));
   CHECK(VIDEO_VIEWS_UI_WANTED(true, false, none, false, true));
   CHECK(!VIDEO_VIEWS_UI_WANTED(false, true, "msg", true, true));
   /* The widgets aren't asked when something else already wants it. */
   CHECK(VIDEO_VIEWS_UI_WANTED(true, true, none, false, poll_widgets())
         && !widget_polls);
   CHECK(!VIDEO_VIEWS_UI_WANTED(false, false, none, false, poll_widgets())
         && !widget_polls);
   CHECK(VIDEO_VIEWS_UI_WANTED(true, false, none, false, poll_widgets())
         && widget_polls == 1);

   video_views_ui_enter(&saved, &dims, &video_dims, 33);
   CHECK(dims == 33 && video_dims == 33);
   video_views_ui_leave(&saved, &dims, &video_dims);
   CHECK(dims == 11 && video_dims == 22);
}

static void test_screen_api(void)
{
   CHECK(RETRO_VIDEO_VIEWS_FLAG_SCREENS == 2);
   CHECK(RETRO_VIDEO_VIEWS_STATUS_SCREENS == 8 && RETRO_VIDEO_VIEWS_STATUS_IMAGES == 16);
   CHECK(RETRO_ENVIRONMENT_GET_VIDEO_SCREEN_BUFFER == (108 | RETRO_ENVIRONMENT_EXPERIMENTAL));
   CHECK(RETRO_VIDEO_SCREEN_SHOW_OVER == 2 && RETRO_VIDEO_SCREEN_STATE_ABSENT == 2);
   CHECK(RETRO_VIDEO_SCREEN_ANCHOR_DEFAULT == 0 && RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT == 8);
   CHECK(RETRO_VIDEO_SCREEN_FLAG_NO_INPUT == 1 && RETRO_VIDEO_SCREEN_FLAG_GROUP_VERTICAL == 32);
   CHECK(RETRO_VIDEO_SCREEN_FLAG_UPRIGHT == 64);
   CHECK(RETRO_VIDEO_SCREENS_MAX == RETRO_VIDEO_VIEWS_MAX);
   CHECK(offsetof(struct retro_video_views, screens)
         > offsetof(struct retro_video_views, recommended_view_height));
   CHECK(offsetof(struct retro_video_views, num_screens)
         > offsetof(struct retro_video_views, screens));
   CHECK(offsetof(struct retro_video_screen, image)
         > offsetof(struct retro_video_screen, group));
   CHECK(offsetof(struct retro_video_screen_buffer, pitch)
         > offsetof(struct retro_video_screen_buffer, data));
}

/* @num_scr screens around @views, those in @absent ABSENT, as
 * video_views_validate_map makes them. */
static void t_absent_map(const struct retro_video_view *views,
      unsigned num_views, unsigned num_scr, unsigned absent,
      video_views_map_t *map)
{
   struct retro_video_views in;
   struct retro_video_screen scr[RETRO_VIDEO_VIEWS_MAX];
   unsigned i;
   memset(scr, 0, sizeof(scr));
   for (i = 0; i < num_scr; i++)
      if (absent & (1u << i))
         scr[i].state = RETRO_VIDEO_SCREEN_STATE_ABSENT;
   memset(&in, 0, sizeof(in));
   in.views       = views;
   in.num_views   = num_views;
   in.flags       = RETRO_VIDEO_VIEWS_FLAG_SCREENS;
   in.screens     = scr;
   in.num_screens = num_scr;
   CHECK(video_views_validate_map(&in, map, NULL));
}

/* A screen without views takes no place: the others lay out as if
 * it were not there. */
static void test_layout_viewless(void)
{
   struct retro_video_view v[2];
   video_views_map_t map, plain;
   video_views_layout_params_t p;
   video_views_layout_t l, want;
   unsigned layout, i;

   v[0] = mkview(0,   0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   v[1] = mkview(0, 192, 256, 192, 2, RETRO_VIDEO_VIEW_EYE_NONE);
   t_absent_map(v, 2, 3, 0x2, &map);
   CHECK(map.num_screens == 3 && map.viewless == 0x2);
   map_ds(&plain);
   for (layout = 0; layout < VIDEO_SCREEN_LAYOUT_LAST; layout++)
   {
      params_init(&p, &plain, 1600, 960);
      p.screen_layout = layout;
      video_views_layout(&p, &want);
      p.map = &map;
      video_views_layout(&p, &l);
      CHECK(want.num_placements == 2 && l.num_placements == 2);
      for (i = 0; i < 2; i++)
         CHECK(   l.placements[i].dst.pos  == want.placements[i].dst.pos
               && l.placements[i].dst.dims == want.placements[i].dst.dims);
   }

   /* Screen 0 without views: one placement, a one-screen map's. */
   v[0] = mkview(0, 192, 256, 192, 1, RETRO_VIDEO_VIEW_EYE_NONE);
   t_absent_map(v, 1, 2, 0x1, &map);
   CHECK(map.num_screens == 2 && map.viewless == 0x1);
   v[0].screen = 0;
   CHECK(video_views_validate(v, 1, &plain));
   params_init(&p, &plain, 1600, 960);
   video_views_layout(&p, &want);
   p.map = &map;
   video_views_layout(&p, &l);
   CHECK(want.num_placements == 1 && l.num_placements == 1);
   CHECK(   l.placements[0].view     == 0
         && l.placements[0].dst.pos  == want.placements[0].dst.pos
         && l.placements[0].dst.dims == want.placements[0].dst.dims);
}

/* A screen left out takes no place: the others lay out as a map of
 * their own. */
static void test_layout_left_out(void)
{
   struct retro_video_view v;
   video_views_map_t map, alone;
   video_views_layout_t l, want;

   map_ds(&map);
   v = mkview(0, 0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   CHECK(video_views_validate(&v, 1, &alone));
   t_side_layout(&alone, VIDEO_STEREO_MODE_2D, &want);
   CHECK(rect_is(&want.placements[0].dst, 160, 0, 1280, 960));
   map.left_out = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(want.num_placements == 1 && l.num_placements == 1);
   CHECK(l.placements[0].view == 0
         && t_dst_same(&l.placements[0], &want.placements[0]));

   v = mkview(0, 192, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   CHECK(video_views_validate(&v, 1, &alone));
   t_side_layout(&alone, VIDEO_STEREO_MODE_2D, &want);
   map.left_out = 0x1;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(want.num_placements == 1 && l.num_placements == 1);
   CHECK(l.placements[0].view == 1
         && t_dst_same(&l.placements[0], &want.placements[0]));

   /* Every screen left out: a window that shows no screen. */
   map.left_out = 0x3;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 0 && l.ui_dims == 0 && l.num_areas == 0);

   /* Side by side, full: screen 0 in each eye. */
   v = mkview(0, 0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   CHECK(video_views_validate(&v, 1, &alone));
   t_side_layout(&alone, VIDEO_STEREO_MODE_SBS_FULL, &want);
   map.left_out = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_SBS_FULL, &l);
   CHECK(want.num_placements == 2 && l.num_placements == 2);
   CHECK(   l.placements[0].view == 0 && l.placements[0].area == 0
         && l.placements[1].view == 0 && l.placements[1].area == 1);
   CHECK(   t_dst_same(&l.placements[0], &want.placements[0])
         && t_dst_same(&l.placements[1], &want.placements[1]));
}

/* A blank screen keeps its place, marked to be drawn black. */
static void test_layout_blank(void)
{
   video_views_map_t map;
   video_views_layout_t l, want;
   unsigned i;

   map_ds(&map);
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &want);
   map.blank = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(want.num_placements == 2 && l.num_placements == 2);
   for (i = 0; i < 2; i++)
      CHECK(t_dst_same(&l.placements[i], &want.placements[i]));
   CHECK(l.placements[0].flags == 0);
   CHECK(l.placements[1].flags == VIDEO_VIEWS_PLACEMENT_BLANK);

   /* In each eye. */
   t_side_layout(&map, VIDEO_STEREO_MODE_SBS_FULL, &l);
   CHECK(l.num_placements == 4);
   CHECK(   l.placements[0].flags == 0
         && l.placements[1].flags == VIDEO_VIEWS_PLACEMENT_BLANK
         && l.placements[2].flags == 0
         && l.placements[3].flags == VIDEO_VIEWS_PLACEMENT_BLANK);

   /* Left out wins. */
   map.left_out = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 1 && l.placements[0].view == 0
         && l.placements[0].flags == 0);
}

/* NO_INPUT: the DS map stacked in 1600x960, screen 0 at (480,0) and
 * screen 1 at (480,480), each 640x480. */
static void test_map_point_no_input(void)
{
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;
   uint32_t pos, want;
   unsigned frame = VIDEO_SCALE_PACK(256, 384);

   map_ds(&map);
   params_init(&p, &map, 1600, 960);
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.placements[0].dst, 480,   0, 640, 480));
   CHECK(rect_is(&l.placements[1].dst, 480, 480, 640, 480));

   /* Control: the bottom screen's centre is packed pixel (128, 288). */
   CHECK(video_views_map_point(&l, &map, frame, 800, 720, false, &pos));
   CHECK(VIDEO_POS_X(pos) == 128 && VIDEO_POS_Y(pos) == 16511);

   /* NO_INPUT on screen 1: its centre clamps into screen 0, to the same
    * bottom-row point as (800, 479), packed pixel (128, 191). */
   CHECK(video_views_map_point(&l, &map, frame, 800, 479, false, &want));
   CHECK(VIDEO_POS_X(want) == 128 && VIDEO_POS_Y(want) == -87);
   map.no_input = 0x2;
   pos = 0;
   CHECK(video_views_map_point(&l, &map, frame, 800, 720, false, &pos));
   CHECK(pos == want);
   CHECK(video_views_map_point(&l, &map, frame, 800, 720, true, &pos));
   CHECK(pos == VIDEO_POS_PACK(-0x8000, -0x8000));

   /* Screen 0 is as it was. */
   map.no_input = 0;
   CHECK(video_views_map_point(&l, &map, frame, 800, 240, true, &want));
   map.no_input = 0x2;
   pos = 0;
   CHECK(video_views_map_point(&l, &map, frame, 800, 240, true, &pos));
   CHECK(pos == want);

   /* Only the NO_INPUT screen placed: off every view. */
   p.exclude_screens = 0x1;
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 1);
   pos = 0;
   CHECK(video_views_map_point(&l, &map, frame, 800, 720, false, &pos));
   CHECK(pos == VIDEO_POS_PACK(-0x8000, -0x8000));
   pos = 0;
   CHECK(video_views_map_point(&l, &map, frame, 800, 720, true, &pos));
   CHECK(pos == VIDEO_POS_PACK(-0x8000, -0x8000));
}

/* A blank screen still takes input. */
static void test_map_point_blank(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   uint32_t pos, want;
   unsigned frame = VIDEO_SCALE_PACK(256, 384);

   map_ds(&map);
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(video_views_map_point(&l, &map, frame, 1200, 480, true, &want));
   CHECK(VIDEO_POS_X(want) == 128 && VIDEO_POS_Y(want) == 16511);
   map.blank = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   pos = 0;
   CHECK(video_views_map_point(&l, &map, frame, 1200, 480, true, &pos));
   CHECK(pos == want);
}

static void test_touch_no_input(void)
{
   video_views_map_t map, sub;
   video_views_layout_params_t p;
   video_views_layout_t l;
   video_views_touch_t t;
   unsigned index[RETRO_VIDEO_VIEWS_MAX];
   int16_t x, y, top_x, top_y;
   unsigned frame = VIDEO_SCALE_PACK(256, 384);

   memset(&t, 0, sizeof(t));
   map_ds(&map);
   CHECK(video_views_select_screens(&map, 0x3, &sub, index));
   params_init(&p, &sub, 1600, 960);
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 2);

   /* Without the flag: the bottom screen's centre, and the top's. */
   video_views_touch_publish(&t, &map, &l, index, p.dims, frame);
   CHECK(video_views_touch_point(&t, 0, 16384, &x, &y));
   CHECK(x != -0x8000 && y > 0);
   CHECK(video_views_touch_point(&t, 0, -16384, &top_x, &top_y));
   CHECK(top_x != -0x8000 && top_y < 0);

   map.no_input = 0x2;
   video_views_touch_publish(&t, &map, &l, index, p.dims, frame);
   x = y = 0;
   CHECK(video_views_touch_point(&t, 0, 16384, &x, &y));
   CHECK(x == -0x8000 && y == -0x8000);
   x = y = 0;
   CHECK(video_views_touch_point(&t, 0, -16384, &x, &y));
   CHECK(x == top_x && y == top_y);
}

/* A packed-frame point on a NO_INPUT screen, the DS map's bottom one. */
static void test_point_no_input(void)
{
   video_views_map_t map;

   map_ds(&map);
   CHECK(!video_views_point_no_input(&map, 128, 288));
   map.no_input = 0x2;
   CHECK(video_views_point_no_input(&map, 128, 288));
   CHECK(video_views_point_no_input(&map, 0, 192));
   CHECK(video_views_point_no_input(&map, 255, 383));
   CHECK(!video_views_point_no_input(&map, 128, 96));
   CHECK(!video_views_point_no_input(&map, 128, 191));
   /* Off both */
   CHECK(!video_views_point_no_input(&map, 256, 288));
   CHECK(!video_views_point_no_input(&map, 128, 384));
   CHECK(!video_views_point_no_input(&map, -1, 288));

   /* Over a view that takes input, in either order. */
   map.views[1].y = 96;
   CHECK(!video_views_point_no_input(&map, 128, 150));
   CHECK(video_views_point_no_input(&map, 128, 200));
   map.no_input = 0x1;
   CHECK(!video_views_point_no_input(&map, 128, 150));
   CHECK(video_views_point_no_input(&map, 128, 50));
}

/* Every name of @n the letter @c plus its index, @len long. */
static void t_names_fill(video_views_names_t *n, char c, size_t len)
{
   unsigned i;
   memset(n, 0, sizeof(*n));
   for (i = 0; i < RETRO_VIDEO_VIEWS_MAX; i++)
      memset(n->name[i], c + (char)i, len);
}

static int t_names_are(const video_views_names_t *got,
      const video_views_names_t *want)
{
   unsigned i;
   for (i = 0; i < RETRO_VIDEO_VIEWS_MAX; i++)
      if (strcmp(got->name[i], want->name[i]))
         return 0;
   return 1;
}

static void test_names_box(void)
{
   static video_views_names_box_t box;
   video_views_names_t a, b, none, got;
   unsigned i;

   memset(&box, 0, sizeof(box));
   memset(&none, 0, sizeof(none));
   t_names_fill(&a, 'a', VIDEO_VIEWS_NAME_SIZE - 1);
   t_names_fill(&b, 'A', 5);

   memset(&got, 0xA5, sizeof(got));
   CHECK(video_views_names_read(&box, &got) == 0);
   CHECK(t_names_are(&got, &none));

   video_views_names_publish(&box, &a);
   CHECK(video_views_names_read(&box, &got) == 2);
   CHECK(t_names_are(&got, &a));
   video_views_names_publish(&box, &b);
   CHECK(video_views_names_read(&box, &got) == 4);
   CHECK(t_names_are(&got, &b));

   /* Alternating: each read is one whole set. */
   for (i = 0; i < 8; i++)
   {
      const video_views_names_t *want = (i & 1) ? &b : &a;
      video_views_names_publish(&box, want);
      CHECK(video_views_names_read(&box, &got) == (int)(6 + 2 * i));
      CHECK(t_names_are(&got, want));
   }

   /* A writer stopped while it stores copy 0: a reader takes copy 1,
    * the last whole set. */
   retro_atomic_store_release_int(&box.seq, 21);
   for (i = 0; i < VIDEO_VIEWS_NAMES_WORDS; i++)
      retro_atomic_store_relaxed_int(&box.words[i], 0x5A5A5A5A);
   CHECK(video_views_names_read(&box, &got) == 21);
   CHECK(t_names_are(&got, &b));
}

/* Resolved and global values start as the built-in defaults. */
static void t_prefs_values(video_screen_values_t *v)
{
   memset(v, 0, sizeof(*v));
   video_views_xr_reset(&v->xr, 1);
}

#define T_PREF(p) VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_##p)

static void test_prefs_resolve_order(void)
{
   video_screen_prefs_t prefs;
   video_views_screens_t scr;
   video_screen_values_t global, mine, out;

   memset(&prefs, 0, sizeof(prefs));
   memset(&scr, 0, sizeof(scr));
   t_prefs_values(&global);
   scr.count             = 2;
   scr.screens[1].show   = RETRO_VIDEO_SCREEN_SHOW_HIDDEN;
   /* The core's default beats the global setting. */
   CHECK(video_screen_prefs_resolve(&prefs, &scr, &global, 1, &out)
         == T_PREF(DISPLAY));
   CHECK(!strcmp(out.display, VIDEO_WINDOW_DISPLAY_OFF));
   /* The user's choice beats the core's. */
   t_prefs_values(&mine);
   video_screen_prefs_set(&prefs, 1, T_PREF(DISPLAY), &mine);
   CHECK(video_screen_prefs_resolve(&prefs, &scr, &global, 1, &out) == 0);
   CHECK(!strcmp(out.display, ""));
   /* SHOWN gives no default. */
   memset(&prefs, 0, sizeof(prefs));
   scr.screens[1].show = RETRO_VIDEO_SCREEN_SHOW_SHOWN;
   strcpy(global.display, VIDEO_WINDOW_DISPLAY_OFF);
   CHECK(video_screen_prefs_resolve(&prefs, &scr, &global, 1, &out) == 0);
   CHECK(!strcmp(out.display, VIDEO_WINDOW_DISPLAY_OFF));
   /* No screen information, and a stale entry past count. */
   scr.screens[1].show = RETRO_VIDEO_SCREEN_SHOW_HIDDEN;
   global.display[0]   = '\0';
   CHECK(video_screen_prefs_resolve(NULL, NULL, &global, 1, &out) == 0);
   CHECK(!strcmp(out.display, ""));
   scr.count = 1;
   CHECK(video_screen_prefs_resolve(&prefs, &scr, &global, 1, &out) == 0);
   CHECK(!strcmp(out.display, ""));
}

static void test_prefs_resolve_over(void)
{
   video_screen_prefs_t prefs;
   video_views_screens_t scr;
   video_screen_values_t global, mine, out;
   const unsigned all = T_PREF(DISPLAY) | T_PREF(CORNER) | T_PREF(SIZE)
      | T_PREF(OPACITY);

   memset(&prefs, 0, sizeof(prefs));
   memset(&scr, 0, sizeof(scr));
   t_prefs_values(&global);
   scr.count           = 3;
   scr.screens[1].show = RETRO_VIDEO_SCREEN_SHOW_OVER;
   CHECK(video_screen_prefs_resolve(NULL, &scr, &global, 1, &out)
         == T_PREF(DISPLAY));
   CHECK(!strcmp(out.display, "over:1"));
   CHECK(!out.corner && !out.size && !out.opacity);
   scr.screens[1].base = 2;
   video_screen_prefs_resolve(NULL, &scr, &global, 1, &out);
   CHECK(!strcmp(out.display, "over:3"));
   /* The global values where the core gives none */
   global.corner  = RETRO_VIDEO_SCREEN_ANCHOR_LEFT;
   global.size    = 30;
   global.opacity = 20;
   video_screen_prefs_resolve(NULL, &scr, &global, 1, &out);
   CHECK(out.corner == RETRO_VIDEO_SCREEN_ANCHOR_LEFT && out.size == 30
         && out.opacity == 20);
   /* The core's, as rounded percents */
   scr.screens[1].base    = 0;
   scr.screens[1].anchor  = RETRO_VIDEO_SCREEN_ANCHOR_TOP_LEFT;
   scr.screens[1].size    = 0.5f;
   scr.screens[1].opacity = 0.75f;
   CHECK(video_screen_prefs_resolve(NULL, &scr, &global, 1, &out) == all);
   CHECK(out.corner == RETRO_VIDEO_SCREEN_ANCHOR_TOP_LEFT && out.size == 50
         && out.opacity == 75);
   scr.screens[1].opacity = 0.334f;
   video_screen_prefs_resolve(NULL, &scr, &global, 1, &out);
   CHECK(out.opacity == 33);
   /* A non-zero value stays non-zero */
   scr.screens[1].opacity = 0.001f;
   video_screen_prefs_resolve(NULL, &scr, &global, 1, &out);
   CHECK(out.opacity == 1);
   /* A store entry beats each */
   t_prefs_values(&mine);
   strcpy(mine.display, VIDEO_WINDOW_DISPLAY_MAIN);
   mine.corner  = RETRO_VIDEO_SCREEN_ANCHOR_TOP_RIGHT;
   mine.size    = 40;
   mine.opacity = 60;
   video_screen_prefs_set(&prefs, 1, T_PREF(DISPLAY), &mine);
   CHECK(video_screen_prefs_resolve(&prefs, &scr, &global, 1, &out)
         == (all & ~T_PREF(DISPLAY)));
   CHECK(!strcmp(out.display, VIDEO_WINDOW_DISPLAY_MAIN));
   video_screen_prefs_set(&prefs, 1, T_PREF(CORNER), &mine);
   CHECK(video_screen_prefs_resolve(&prefs, &scr, &global, 1, &out)
         == (T_PREF(SIZE) | T_PREF(OPACITY)));
   CHECK(out.corner == RETRO_VIDEO_SCREEN_ANCHOR_TOP_RIGHT && out.size == 50);
   video_screen_prefs_set(&prefs, 1, T_PREF(SIZE), &mine);
   CHECK(video_screen_prefs_resolve(&prefs, &scr, &global, 1, &out)
         == T_PREF(OPACITY));
   CHECK(out.size == 40 && out.opacity == 1);
   video_screen_prefs_set(&prefs, 1, T_PREF(OPACITY), &mine);
   CHECK(video_screen_prefs_resolve(&prefs, &scr, &global, 1, &out) == 0);
   CHECK(out.opacity == 60);
}

static void test_prefs_over_value(void)
{
   static const char *const bad[] = {
      "over:0", "over:9", "over:x", "over:", "off", "", "main",
      "over:01", "over:1x", "over:-1", "over:10", "over: 1", "Over:1"
   };
   char s[16];
   unsigned i;

   CHECK(video_screen_prefs_over_base("over:1") == 0);
   CHECK(video_screen_prefs_over_base("over:8") == 7);
   for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
      CHECK(video_screen_prefs_over_base(bad[i]) == -1);
   CHECK(video_screen_prefs_over_value(0, s, sizeof(s)) == strlen("over:1"));
   CHECK(!strcmp(s, "over:1"));
   for (i = 0; i < RETRO_VIDEO_VIEWS_MAX; i++)
   {
      video_screen_prefs_over_value(i, s, sizeof(s));
      CHECK(video_screen_prefs_over_base(s) == (int)i);
   }
}

static void test_prefs_resolve_fields(void)
{
   video_screen_prefs_t prefs;
   video_screen_values_t global, mine, out;

   memset(&prefs, 0, sizeof(prefs));
   t_prefs_values(&global);
   t_prefs_values(&mine);
   global.window_pos   = VIDEO_POS_PACK(5, 6);
   global.window_dims  = VIDEO_SCALE_PACK(320, 240);
   global.xr.x         = 0.5f;
   global.xr.pitch     = 10.0f;
   mine.window_pos     = VIDEO_POS_PACK(100, 77);
   mine.window_dims    = VIDEO_SCALE_PACK(640, 480);
   mine.xr.x           = 9.0f;
   mine.xr.scale       = 2.0f;
   mine.xr.visible     = false;
   /* X and Y, width and height, each from its own layer. */
   video_screen_prefs_set(&prefs, 0, T_PREF(WINDOW_X) | T_PREF(WINDOW_WIDTH),
         &mine);
   CHECK(video_screen_prefs_resolve(&prefs, NULL, &global, 0, &out) == 0);
   CHECK(out.window_pos == VIDEO_POS_PACK(100, 6));
   CHECK(out.window_dims == VIDEO_SCALE_PACK(640, 240));
   /* A lone width is no saved size. */
   global.window_dims = 0;
   video_screen_prefs_resolve(&prefs, NULL, &global, 0, &out);
   CHECK(out.window_dims == 0);

   video_screen_prefs_set(&prefs, 0, T_PREF(XR_SCALE), &mine);
   video_screen_prefs_resolve(&prefs, NULL, &global, 0, &out);
   CHECK(out.xr.scale == 2.0f);
   CHECK(out.xr.x == 0.5f && out.xr.y == 0.0f && out.xr.z == 0.0f);
   CHECK(out.xr.yaw == 0.0f && out.xr.pitch == 10.0f && out.xr.roll == 0.0f);
   CHECK(out.xr.visible);
   video_screen_prefs_set(&prefs, 0, T_PREF(XR_VISIBLE), &mine);
   video_screen_prefs_resolve(&prefs, NULL, &global, 0, &out);
   CHECK(!out.xr.visible && out.xr.scale == 2.0f);
}

/* The arrange step's inputs: the core's screens, the user's store for
 * this core and the global values, resolved as the frame resolves them */
typedef struct
{
   video_views_screens_t scr;
   video_screen_prefs_t prefs;
   video_screen_values_t global[RETRO_VIDEO_VIEWS_MAX];
   video_screen_values_t resolved[RETRO_VIDEO_VIEWS_MAX];
   unsigned defaults[RETRO_VIDEO_VIEWS_MAX];
   video_views_map_t map;
   bool images;
} t_arrange_t;

/* @n screens, each SHOWN and ACTIVE, nothing set */
static void t_arrange_begin(t_arrange_t *a, unsigned n)
{
   unsigned i;
   memset(a, 0, sizeof(*a));
   for (i = 0; i < RETRO_VIDEO_VIEWS_MAX; i++)
      t_prefs_values(&a->global[i]);
   a->scr.count       = n;
   a->map.num_screens = n;
   a->images          = true;
}

static void t_arrange_user(t_arrange_t *a, unsigned s, const char *display)
{
   video_screen_values_t v;
   t_prefs_values(&v);
   strcpy(v.display, display);
   video_screen_prefs_set(&a->prefs, s, T_PREF(DISPLAY), &v);
}

static void t_arrange_core_over(t_arrange_t *a, unsigned s, unsigned base)
{
   a->scr.screens[s].show = RETRO_VIDEO_SCREEN_SHOW_OVER;
   a->scr.screens[s].base = base;
}

/* Junk in every field the step writes, so it must write them all */
static void t_arrange_junk(video_views_map_t *map)
{
   unsigned i;
   map->left_out = map->blank = map->over = ~0u;
   map->base     = map->anchor = 0xffffffffu;
   memset(map->opacity, 0xa5, sizeof(map->opacity));
   for (i = 0; i < RETRO_VIDEO_VIEWS_MAX; i++)
      map->size[i] = 9.0f;
}

static void t_arrange_run(t_arrange_t *a)
{
   unsigned s;
   for (s = 0; s < RETRO_VIDEO_VIEWS_MAX; s++)
      a->defaults[s] = video_screen_prefs_resolve(&a->prefs, &a->scr,
            &a->global[s], s, &a->resolved[s]);
   t_arrange_junk(&a->map);
   video_screen_prefs_arrange(a->resolved, a->defaults, &a->scr, a->images,
         &a->map);
}

/* Nothing at or past @n */
static int t_arrange_clear_past(const video_views_map_t *map, unsigned n)
{
   unsigned s;
   unsigned high = ~((1u << n) - 1u);
   if ((map->over | map->left_out | map->blank) & high)
      return 0;
   for (s = n; s < RETRO_VIDEO_VIEWS_MAX; s++)
      if (     VIDEO_VIEWS_NIBBLE(map->base, s)
            || VIDEO_VIEWS_NIBBLE(map->anchor, s)
            || map->opacity[s] || map->size[s] != 0.0f)
         return 0;
   return 1;
}

static void test_arrange_masks(void)
{
   video_views_screens_t scr;
   video_views_map_t map;
   video_screen_values_t resolved[RETRO_VIDEO_VIEWS_MAX];
   unsigned defaults[RETRO_VIDEO_VIEWS_MAX];
   unsigned i;

   memset(&scr, 0, sizeof(scr));
   memset(&map, 0, sizeof(map));
   memset(defaults, 0, sizeof(defaults));
   for (i = 0; i < RETRO_VIDEO_VIEWS_MAX; i++)
      t_prefs_values(&resolved[i]);
   scr.count            = 4;
   scr.screens[1].state = RETRO_VIDEO_SCREEN_STATE_BLANK;
   scr.screens[2].state = RETRO_VIDEO_SCREEN_STATE_ABSENT;
   scr.screens[3].state = RETRO_VIDEO_SCREEN_STATE_BLANK;
   strcpy(resolved[3].display, VIDEO_WINDOW_DISPLAY_OFF);
   map.num_screens = 4;
   t_arrange_junk(&map);
   video_screen_prefs_arrange(resolved, defaults, &scr, true, &map);
   CHECK(map.left_out == 0xC && map.blank == 0x2 && !map.over);
   CHECK(t_arrange_clear_past(&map, 4));
   map.num_screens = 2;
   t_arrange_junk(&map);
   video_screen_prefs_arrange(resolved, defaults, &scr, true, &map);
   CHECK(map.left_out == 0 && map.blank == 0x2);
   CHECK(t_arrange_clear_past(&map, 2));
   /* Entries past count are stale. */
   map.num_screens        = 4;
   scr.count              = 2;
   scr.screens[3].state   = RETRO_VIDEO_SCREEN_STATE_ABSENT;
   resolved[3].display[0] = '\0';
   t_arrange_junk(&map);
   video_screen_prefs_arrange(resolved, defaults, &scr, true, &map);
   CHECK(map.left_out == 0 && map.blank == 0x2);
   /* No screen information. */
   strcpy(resolved[1].display, VIDEO_WINDOW_DISPLAY_OFF);
   t_arrange_junk(&map);
   video_screen_prefs_arrange(resolved, defaults, NULL, true, &map);
   CHECK(map.left_out == 0x2 && map.blank == 0);
   scr.count = 0;
   t_arrange_junk(&map);
   video_screen_prefs_arrange(resolved, defaults, &scr, true, &map);
   CHECK(map.left_out == 0x2 && map.blank == 0);
   CHECK(t_arrange_clear_past(&map, 4));
}

static void test_prefs_set(void)
{
   video_screen_prefs_t prefs;
   video_screen_values_t v;
   unsigned both = T_PREF(WINDOW_FULLSCREEN) | T_PREF(XR_SCALE);

   CHECK(VIDEO_SCREEN_PREF_XR_PLACE == (T_PREF(XR_X) | T_PREF(XR_Y)
         | T_PREF(XR_Z) | T_PREF(XR_YAW) | T_PREF(XR_PITCH)
         | T_PREF(XR_ROLL) | T_PREF(XR_SCALE)));
   CHECK(VIDEO_SCREEN_PREF_XR_ALL
         == (VIDEO_SCREEN_PREF_XR_PLACE | T_PREF(XR_VISIBLE)));

   memset(&prefs, 0, sizeof(prefs));
   t_prefs_values(&v);
   v.window_fullscreen = true;
   video_screen_prefs_set(&prefs, 2, both, &v);
   CHECK(prefs.set[2] == both && prefs.dirty);
   CHECK(prefs.values[2].window_fullscreen && prefs.values[2].xr.scale == 1.0f);
   CHECK(!prefs.set[0] && !prefs.set[1] && !prefs.set[3]);
   /* The same values again change nothing. */
   prefs.dirty = false;
   video_screen_prefs_set(&prefs, 2, both, &v);
   CHECK(!prefs.dirty);
   v.xr.scale = 3.0f;
   video_screen_prefs_set(&prefs, 2, T_PREF(XR_SCALE), &v);
   CHECK(prefs.dirty && prefs.values[2].xr.scale == 3.0f);
   /* Clearing an unset bit changes nothing; a set one is cleared. */
   prefs.dirty = false;
   video_screen_prefs_set(&prefs, 2, T_PREF(DISPLAY), NULL);
   CHECK(!prefs.dirty && prefs.set[2] == both);
   video_screen_prefs_set(&prefs, 2, T_PREF(XR_SCALE), NULL);
   CHECK(prefs.dirty && prefs.set[2] == T_PREF(WINDOW_FULLSCREEN));
}

static void test_prefs_key(void)
{
   static const char *const suffix[VIDEO_SCREEN_PREF_COUNT] = {
      "display", "window_fullscreen", "window_x", "window_y",
      "window_width", "window_height", "window_pos_set", "xr_x", "xr_y",
      "xr_z", "xr_yaw", "xr_pitch", "xr_roll", "xr_scale", "xr_visible",
      "corner", "size", "opacity"
   };
   char key[64], want[64];
   unsigned p;

   CHECK(video_screen_prefs_key(1, VIDEO_SCREEN_PREF_XR_VISIBLE, key,
         sizeof(key)) == strlen("video_screen2_xr_visible"));
   CHECK(!strcmp(key, "video_screen2_xr_visible"));
   video_screen_prefs_key(0, VIDEO_SCREEN_PREF_WINDOW_POS_SET, key, sizeof(key));
   CHECK(!strcmp(key, "video_screen1_window_pos_set"));
   video_screen_prefs_key(7, VIDEO_SCREEN_PREF_DISPLAY, key, sizeof(key));
   CHECK(!strcmp(key, "video_screen8_display"));
   video_screen_prefs_key(0, VIDEO_SCREEN_PREF_WINDOW_WIDTH, key, sizeof(key));
   CHECK(!strcmp(key, "video_screen1_window_width"));
   video_screen_prefs_key(0, VIDEO_SCREEN_PREF_CORNER, key, sizeof(key));
   CHECK(!strcmp(key, "video_screen1_corner"));
   video_screen_prefs_key(7, VIDEO_SCREEN_PREF_OPACITY, key, sizeof(key));
   CHECK(!strcmp(key, "video_screen8_opacity"));
   /* Every key as configuration.c names it. */
   for (p = 0; p < VIDEO_SCREEN_PREF_COUNT; p++)
   {
      strcpy(want, "video_screen3_");
      strcat(want, suffix[p]);
      video_screen_prefs_key(2, p, key, sizeof(key));
      CHECK(!strcmp(key, want));
   }
   CHECK(video_screen_prefs_key(0, VIDEO_SCREEN_PREF_COUNT, key,
         sizeof(key)) == 0 && !*key);
}

#define T_NIB(bits, s) VIDEO_VIEWS_NIBBLE(bits, s)

static void test_arrange_core_over(void)
{
   static t_arrange_t a;

   t_arrange_begin(&a, 2);
   t_arrange_core_over(&a, 1, 0);
   t_arrange_run(&a);
   CHECK(a.map.over == 0x2 && !a.map.left_out && !a.map.blank);
   CHECK(T_NIB(a.map.base, 1) == 0);
   CHECK(T_NIB(a.map.anchor, 1) == RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT);
   CHECK(a.map.size[1] == 0.25f && a.map.opacity[1] == 255);
   /* The root has none of an inset's fields */
   CHECK(!T_NIB(a.map.anchor, 0) && !a.map.opacity[0] && !a.map.size[0]);
   CHECK(t_arrange_clear_past(&a.map, 2));
   a.scr.screens[1].anchor  = RETRO_VIDEO_SCREEN_ANCHOR_TOP_LEFT;
   a.scr.screens[1].size    = 0.5f;
   a.scr.screens[1].opacity = 0.75f;
   t_arrange_run(&a);
   CHECK(a.map.over == 0x2);
   CHECK(T_NIB(a.map.anchor, 1) == RETRO_VIDEO_SCREEN_ANCHOR_TOP_LEFT);
   CHECK(a.map.size[1] == 0.5f && a.map.opacity[1] == 191);
   /* The core's own opacity, not its rounded percent (33 %: 84) */
   a.scr.screens[1].opacity = 0.334f;
   t_arrange_run(&a);
   CHECK(a.map.opacity[1] == 85);
   /* Sent with opacity 0: the default */
   a.scr.screens[1].opacity = 0.0f;
   t_arrange_run(&a);
   CHECK(a.map.opacity[1] == 255);
   /* Never invisible */
   a.scr.screens[1].opacity = 0.001f;
   t_arrange_run(&a);
   CHECK(a.map.opacity[1] == 1);
}

static void test_arrange_user_values(void)
{
   static t_arrange_t a;
   video_screen_values_t v;
   const unsigned three = T_PREF(CORNER) | T_PREF(SIZE) | T_PREF(OPACITY);

   t_arrange_begin(&a, 2);
   t_arrange_core_over(&a, 1, 0);
   a.scr.screens[1].anchor  = RETRO_VIDEO_SCREEN_ANCHOR_TOP_LEFT;
   a.scr.screens[1].size    = 0.5f;
   a.scr.screens[1].opacity = 0.75f;
   t_prefs_values(&v);
   v.corner  = RETRO_VIDEO_SCREEN_ANCHOR_TOP_RIGHT;
   v.size    = 40;
   v.opacity = 50;
   video_screen_prefs_set(&a.prefs, 1, three, &v);
   t_arrange_run(&a);
   CHECK(a.map.over == 0x2);
   CHECK(T_NIB(a.map.anchor, 1) == RETRO_VIDEO_SCREEN_ANCHOR_TOP_RIGHT);
   CHECK(a.map.size[1] == 0.40f && a.map.opacity[1] == 128);
   v.size = 150;
   video_screen_prefs_set(&a.prefs, 1, T_PREF(SIZE), &v);
   t_arrange_run(&a);
   CHECK(a.map.size[1] == 1.0f);
   /* The global values where neither the user nor the core gives one */
   video_screen_prefs_set(&a.prefs, 1, three, NULL);
   a.scr.screens[1].anchor  = 0;
   a.scr.screens[1].size    = 0.0f;
   a.scr.screens[1].opacity = 0.0f;
   a.global[1].corner       = RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_LEFT;
   a.global[1].size         = 30;
   a.global[1].opacity      = 20;
   t_arrange_run(&a);
   CHECK(T_NIB(a.map.anchor, 1) == RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_LEFT);
   CHECK(a.map.size[1] == 0.3f && a.map.opacity[1] == 51);
}

static void test_arrange_user_over(void)
{
   static t_arrange_t a;

   t_arrange_begin(&a, 2);
   t_arrange_user(&a, 1, "over:1");
   t_arrange_run(&a);
   CHECK(a.map.over == 0x2 && !a.map.left_out);
   CHECK(T_NIB(a.map.base, 1) == 0);
   CHECK(T_NIB(a.map.anchor, 1) == RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT);
   CHECK(a.map.size[1] == 0.25f && a.map.opacity[1] == 255);
   CHECK(t_arrange_clear_past(&a.map, 2));
   /* The global setting too */
   memset(&a.prefs, 0, sizeof(a.prefs));
   strcpy(a.global[1].display, "over:1");
   t_arrange_run(&a);
   CHECK(a.map.over == 0x2 && T_NIB(a.map.base, 1) == 0);
}

/* Review Focus 3: a hand-edited or stale Over value is laid out as on
 * Auto */
static void test_arrange_user_over_invalid(void)
{
   static t_arrange_t a;
   static const char *const bad[] = { "over:2", "over:9", "over:3", "over:x" };
   unsigned i;

   for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      t_arrange_begin(&a, 2);
      t_arrange_user(&a, 1, bad[i]);
      t_arrange_run(&a);
      CHECK(a.map.over == 0 && !a.map.left_out);
      CHECK(!T_NIB(a.map.anchor, 1) && !a.map.opacity[1] && !a.map.size[1]);
      CHECK(!strcmp(a.resolved[1].display, bad[i]));
   }
   /* A base with no view */
   t_arrange_begin(&a, 2);
   t_arrange_user(&a, 1, "over:1");
   a.map.viewless = 0x1;
   t_arrange_run(&a);
   CHECK(a.map.over == 0);
   /* Auto is the core's own show: its OVER, its HIDDEN */
   t_arrange_begin(&a, 2);
   t_arrange_core_over(&a, 1, 0);
   t_arrange_user(&a, 1, "over:2");
   t_arrange_run(&a);
   CHECK(a.map.over == 0x2 && T_NIB(a.map.base, 1) == 0);
   CHECK(a.map.size[1] == 0.25f && a.map.opacity[1] == 255);
   a.scr.screens[1].show = RETRO_VIDEO_SCREEN_SHOW_HIDDEN;
   t_arrange_run(&a);
   CHECK(a.map.over == 0 && a.map.left_out == 0x2);
   /* A stale entry past count is SHOWN */
   a.scr.screens[1].show = RETRO_VIDEO_SCREEN_SHOW_OVER;
   a.scr.count           = 1;
   t_arrange_run(&a);
   CHECK(a.map.over == 0 && !a.map.left_out);
}

/* One level: an inset over an inset goes over that one's root */
static void test_arrange_flatten(void)
{
   static t_arrange_t a;

   t_arrange_begin(&a, 3);
   t_arrange_core_over(&a, 1, 0);
   t_arrange_user(&a, 0, "over:3");
   t_arrange_run(&a);
   CHECK(a.map.over == 0x3 && !a.map.left_out);
   CHECK(T_NIB(a.map.base, 0) == 2 && T_NIB(a.map.base, 1) == 2);
   CHECK(a.map.opacity[0] == 255 && a.map.opacity[1] == 255);
   /* A chain of the user's own */
   t_arrange_begin(&a, 3);
   t_arrange_user(&a, 0, "over:2");
   t_arrange_user(&a, 1, "over:3");
   t_arrange_run(&a);
   CHECK(a.map.over == 0x3);
   CHECK(T_NIB(a.map.base, 0) == 2 && T_NIB(a.map.base, 1) == 2);
}

static void test_arrange_mutual(void)
{
   static t_arrange_t a;

   t_arrange_begin(&a, 2);
   t_arrange_user(&a, 0, "over:2");
   t_arrange_user(&a, 1, "over:1");
   t_arrange_run(&a);
   CHECK(a.map.over == 0 && !a.map.left_out);
   /* Both on Auto: the core's OVER, flattened again */
   t_arrange_core_over(&a, 1, 0);
   t_arrange_run(&a);
   CHECK(a.map.over == 0x2 && T_NIB(a.map.base, 1) == 0);
   /* A chain into a loop has no root */
   t_arrange_begin(&a, 3);
   t_arrange_user(&a, 0, "over:2");
   t_arrange_user(&a, 1, "over:3");
   t_arrange_user(&a, 2, "over:2");
   t_arrange_run(&a);
   CHECK(a.map.over == 0);
}

static void test_arrange_user_wins(void)
{
   static t_arrange_t a;

   t_arrange_begin(&a, 2);
   t_arrange_user(&a, 0, "over:2");
   t_arrange_core_over(&a, 1, 0);
   t_arrange_run(&a);
   CHECK(a.map.over == 0x1 && T_NIB(a.map.base, 0) == 1);
   CHECK(!T_NIB(a.map.anchor, 1) && !a.map.opacity[1]);
}

static void test_arrange_base_left_out(void)
{
   static t_arrange_t a;

   t_arrange_begin(&a, 2);
   t_arrange_core_over(&a, 1, 0);
   a.scr.screens[0].state = RETRO_VIDEO_SCREEN_STATE_ABSENT;
   t_arrange_run(&a);
   CHECK(a.map.left_out == 0x3 && a.map.over == 0x2);
   a.scr.screens[0].state = RETRO_VIDEO_SCREEN_STATE_ACTIVE;
   a.scr.screens[0].show  = RETRO_VIDEO_SCREEN_SHOW_HIDDEN;
   t_arrange_run(&a);
   CHECK(a.map.left_out == 0x3);
   t_arrange_begin(&a, 2);
   t_arrange_user(&a, 0, VIDEO_WINDOW_DISPLAY_OFF);
   t_arrange_user(&a, 1, "over:1");
   t_arrange_run(&a);
   CHECK(a.map.left_out == 0x3 && a.map.over == 0x2);
   /* Re-based past a base left out: still left out with it */
   t_arrange_begin(&a, 3);
   t_arrange_core_over(&a, 1, 0);
   t_arrange_user(&a, 0, "over:3");
   a.scr.screens[0].state = RETRO_VIDEO_SCREEN_STATE_ABSENT;
   t_arrange_run(&a);
   CHECK(a.map.left_out == 0x3 && T_NIB(a.map.base, 1) == 2);
}

static void test_arrange_blank_inset(void)
{
   static t_arrange_t a;

   t_arrange_begin(&a, 2);
   t_arrange_core_over(&a, 1, 0);
   a.scr.screens[1].state = RETRO_VIDEO_SCREEN_STATE_BLANK;
   t_arrange_run(&a);
   CHECK(a.map.over == 0x2 && a.map.blank == 0x2 && !a.map.left_out);
   /* Over a blank base, drawn */
   a.scr.screens[0].state = RETRO_VIDEO_SCREEN_STATE_BLANK;
   a.scr.screens[1].state = RETRO_VIDEO_SCREEN_STATE_ACTIVE;
   t_arrange_run(&a);
   CHECK(a.map.over == 0x2 && a.map.blank == 0x1 && !a.map.left_out);
}

/* A shown screen keeps the core's base and size; the user's Size is an
 * inset's */
static void test_arrange_shown_sized(void)
{
   static t_arrange_t a;
   video_screen_values_t v;

   t_arrange_begin(&a, 3);
   a.scr.screens[1].size = 0.25f;
   t_arrange_run(&a);
   CHECK(a.map.over == 0 && T_NIB(a.map.base, 1) == 0);
   CHECK(a.map.size[1] == 0.25f && !a.map.opacity[1]);
   CHECK(!T_NIB(a.map.anchor, 1));
   a.scr.screens[1].base = 2;
   t_prefs_values(&v);
   v.size = 40;
   video_screen_prefs_set(&a.prefs, 1, T_PREF(SIZE), &v);
   video_screen_prefs_set(&a.prefs, 0, T_PREF(SIZE), &v);
   t_arrange_run(&a);
   CHECK(T_NIB(a.map.base, 1) == 2 && a.map.size[1] == 0.25f);
   CHECK(!a.map.size[0] && !a.map.size[2]);
   CHECK(t_arrange_clear_past(&a.map, 3));
}

/* @l's IMAGE placements */
static unsigned t_images_placed(const video_views_layout_t *l)
{
   unsigned i;
   unsigned n = 0;
   for (i = 0; i < l->num_placements; i++)
      if (l->placements[i].flags & VIDEO_VIEWS_PLACEMENT_IMAGE)
         n++;
   return n;
}

/* Without IMAGES an image screen, root or inset, is left out as ABSENT,
 * and an inset over it with it; the others keep their values. */
static void test_arrange_images_off(void)
{
   static t_arrange_t a;
   video_views_map_t on;
   video_views_layout_t l;
   unsigned s;

   t_arrange_begin(&a, 3);
   map_dsi(&a.map);
   t_arrange_run(&a);
   on = a.map;
   CHECK(on.left_out == 0);
   t_side_layout(&a.map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(t_images_placed(&l) == 1);
   a.images = false;
   t_arrange_run(&a);
   CHECK(a.map.left_out == 0x4);
   CHECK(!((a.map.over ^ on.over) & 0x3));
   CHECK(!((a.map.blank ^ on.blank) & 0x3));
   for (s = 0; s < 2; s++)
      CHECK(   T_NIB(a.map.base, s) == T_NIB(on.base, s)
            && T_NIB(a.map.anchor, s) == T_NIB(on.anchor, s)
            && a.map.size[s] == on.size[s]
            && a.map.opacity[s] == on.opacity[s]);
   CHECK(t_arrange_clear_past(&a.map, 3));
   t_side_layout(&a.map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 2 && !t_images_placed(&l));

   /* The user's Over Top on the VMU */
   a.images = true;
   t_arrange_user(&a, 2, "over:1");
   t_arrange_run(&a);
   CHECK(a.map.over == 0x4 && a.map.left_out == 0);
   t_side_layout(&a.map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 3 && t_images_placed(&l) == 1);
   a.images = false;
   t_arrange_run(&a);
   CHECK(a.map.over == 0x4 && a.map.left_out == 0x4);
   t_side_layout(&a.map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 2 && !t_images_placed(&l));

   /* Bottom over the VMU */
   t_arrange_begin(&a, 3);
   map_dsi(&a.map);
   t_arrange_user(&a, 1, "over:3");
   a.images = false;
   t_arrange_run(&a);
   CHECK(a.map.over == 0x2 && a.map.left_out == 0x6);
}

/* A screen in the map's rules_out is left out as ABSENT, an image or
 * not, and an inset over it with it; the others keep their values. */
static void test_arrange_rules_out(void)
{
   static t_arrange_t a;
   video_views_map_t on;
   video_views_layout_t l;
   unsigned s;

   t_arrange_begin(&a, 3);
   map_dsi(&a.map);
   t_arrange_run(&a);
   on = a.map;
   CHECK(on.left_out == 0);
   a.map.rules_out = 0x4;
   t_arrange_run(&a);
   CHECK(a.map.left_out == 0x4 && a.map.rules_out == 0x4);
   CHECK(!((a.map.over ^ on.over) & 0x3));
   CHECK(!((a.map.blank ^ on.blank) & 0x3));
   for (s = 0; s < 2; s++)
      CHECK(   T_NIB(a.map.base, s) == T_NIB(on.base, s)
            && T_NIB(a.map.anchor, s) == T_NIB(on.anchor, s)
            && a.map.size[s] == on.size[s]
            && a.map.opacity[s] == on.opacity[s]);
   CHECK(t_arrange_clear_past(&a.map, 3));
   t_side_layout(&a.map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 2 && !t_images_placed(&l));

   /* Bottom over the VMU */
   t_arrange_begin(&a, 3);
   map_dsi(&a.map);
   a.map.rules_out = 0x4;
   t_arrange_user(&a, 1, "over:3");
   t_arrange_run(&a);
   CHECK(a.map.over == 0x2 && a.map.left_out == 0x6);

   /* A screen with a view, BLANK: no place, so not drawn black */
   t_arrange_begin(&a, 2);
   a.scr.screens[0].state = RETRO_VIDEO_SCREEN_STATE_BLANK;
   a.map.rules_out        = 0x1;
   t_arrange_run(&a);
   CHECK(a.map.left_out == 0x1 && !a.map.blank && !a.map.over);
}

#undef T_NIB

#undef T_PREF

/* On a quarter turn a view's width stands tall at the inverse aspect:
 * a 256x224 view at 4:3 takes the place of a 256x192 one. */
static void test_layout_rotated_nonsquare(void)
{
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;
   struct retro_video_view v[2];

   v[0] = mkview(0,   0, 256, 224, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   v[1] = mkview(0, 224, 256, 192, 1, RETRO_VIDEO_VIEW_EYE_NONE);
   v[0].aspect_ratio = 4.0f / 3.0f;
   CHECK(video_views_validate(v, 2, &map));
   params_init(&p, &map, 1024, 768);
   p.screen_layout = VIDEO_SCREEN_LAYOUT_HORIZONTAL;
   p.rotation      = 1;
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 2);
   CHECK(rect_is(&l.placements[0].dst,   0, 43, 512, 682));
   CHECK(rect_is(&l.placements[1].dst, 512, 43, 512, 682));

   /* One screen: the aspect setting turns with it. */
   map.num_views   = 1;
   map.num_screens = 1;
   p.single_aspect = 16.0f / 9.0f;
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 1
         && rect_is(&l.placements[0].dst, 296, 0, 432, 768));
}

/* A shown screen with a size is a root sized off its base. */
static void test_layout_shown_sized(void)
{
   video_views_map_t map;
   video_views_layout_t l;

   map_ds(&map);
   map.over    = 0;
   map.base    = 0;
   map.size[1] = 0.25f;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 2);
   CHECK(rect_is(&l.placements[0].dst,    0,   0, 1280, 960));
   CHECK(rect_is(&l.placements[1].dst, 1280, 360,  320, 240));
}

static int extent_is(const video_views_map_t *map, unsigned screen,
      unsigned rotation, double w, double h)
{
   double gw = 0.0, gh = 0.0;
   if (     video_views_extent(map, screen, rotation, &gw, &gh)
         && fabs(gw - w) < 1e-9 && fabs(gh - h) < 1e-9)
      return 1;
   printf("    screen %u: got %gx%g, want %gx%g\n", screen, gw, gh, w, h);
   return 0;
}

/* Three 256x192 screens. */
static void map_three(video_views_map_t *map)
{
   struct retro_video_view v[3];
   v[0] = mkview(0,   0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   v[1] = mkview(0, 192, 256, 192, 1, RETRO_VIDEO_VIEW_EYE_NONE);
   v[2] = mkview(0, 384, 256, 192, 2, RETRO_VIDEO_VIEW_EYE_NONE);
   video_views_validate(v, 3, map);
}

static void test_extent_view_rule(void)
{
   video_views_map_t map;
   double w, h;

   map_ds(&map);
   CHECK(extent_is(&map, 0, 0, 256.0, 192.0));
   CHECK(extent_is(&map, 0, 1, 192.0, 256.0));
   map.num_views = 1;
   CHECK(!video_views_extent(&map, 1, 0, &w, &h));
}

static void test_extent_base_size(void)
{
   video_views_map_t map;

   map_ds(&map);
   map.base    = 0;
   map.size[1] = 0.25f;
   CHECK(extent_is(&map, 1, 0, 64.0, 48.0));
   /* Turned: a quarter of the base's turned height, at its own turned
    * aspect. */
   CHECK(extent_is(&map, 1, 1, 48.0, 64.0));
   /* An inset is sized by the inset rule, not here. */
   map.over = 0x2;
   CHECK(extent_is(&map, 1, 0, 256.0, 192.0));
   /* A base with no view, or the screen itself, gives the view rule. */
   map.over = 0;
   map.base = 0x30;
   CHECK(extent_is(&map, 1, 0, 256.0, 192.0));
   map.base = 0x10;
   CHECK(extent_is(&map, 1, 0, 256.0, 192.0));
}

static void test_extent_chain_cycle(void)
{
   video_views_map_t map;

   /* 2 at half of 1, 1 at half of 0. */
   map_three(&map);
   map.base    = 0x100;
   map.size[1] = 0.5f;
   map.size[2] = 0.5f;
   CHECK(extent_is(&map, 2, 0, 64.0, 48.0));
   CHECK(extent_is(&map, 1, 0, 128.0, 96.0));
   CHECK(extent_is(&map, 0, 0, 256.0, 192.0));

   /* 0 and 1 at half of each other: 0, the lowest, takes the view rule. */
   map.base    = 0x101;
   map.size[0] = 0.5f;
   CHECK(extent_is(&map, 0, 0, 256.0, 192.0));
   CHECK(extent_is(&map, 1, 0, 128.0, 96.0));
   /* 2 leads into the cycle. */
   CHECK(extent_is(&map, 2, 0, 64.0, 48.0));
}

#ifdef HAVE_THREADS
static video_views_map_t extent_torn_map;
static retro_atomic_int_t extent_torn_done;
static int extent_torn_bad;

/* A redraw's walk while the main thread arranges the map */
static void extent_torn_reader(void *data)
{
   unsigned i;
   (void)data;
   for (i = 0; i < 200000; i++)
   {
      double w = 0.0, h = 0.0;
      if (     !video_views_extent(&extent_torn_map, 2, 0, &w, &h)
            || h != 96.0)
         extent_torn_bad++;
   }
   retro_atomic_store_release_int(&extent_torn_done, 1);
}
#endif

/* Screen 2 at half of 1 or of 0, its base cleared and set again as
 * the arrange step does: every walk ends, at half of 192. */
static void test_extent_torn_base(void)
{
#ifdef HAVE_THREADS
   sthread_t *t;
   time_t end;
   unsigned flips = 0;
   volatile uint32_t *base = &extent_torn_map.base;
   map_three(&extent_torn_map);
   extent_torn_map.base    = 0x100;
   extent_torn_map.size[2] = 0.5f;
   retro_atomic_store_release_int(&extent_torn_done, 0);
   t = sthread_create(extent_torn_reader, NULL);
   CHECK(t != NULL);
   if (!t)
      return;
   end = time(NULL) + 10;
   while (!retro_atomic_load_acquire_int(&extent_torn_done))
   {
      *base = 0;
      *base = 0x100;
      if (!(++flips & 0xffffu) && time(NULL) > end)
      {
         /* The walk never ends; its thread cannot be joined */
         printf("  FAIL %s:%d: screen 2's extent walk did not end\n",
               __FILE__, __LINE__);
         printf("video_views: FAILED\n");
         fflush(stdout);
         exit(1);
      }
   }
   sthread_join(t);
   CHECK(!extent_torn_bad);
#endif
}

static void test_extent_image(void)
{
   video_views_map_t map;
   struct retro_video_views in;

   map_dsi(&map);
   CHECK(extent_is(&map, 2, 0, 48.0, 32.0));
   /* Never turned */
   CHECK(extent_is(&map, 2, 1, 48.0, 32.0));
   CHECK(extent_is(&map, 2, 3, 48.0, 32.0));

   /* Screen 1 at half the picture's height, at its own aspect */
   map.base    = 0x20;
   map.size[1] = 0.5f;
   CHECK(extent_is(&map, 1, 0, 64.0 / 3.0, 16.0));
   /* The picture at half of Top's: screen 1's chain passes through it. */
   map.size[2] = 0.5f;
   CHECK(extent_is(&map, 2, 0, 144.0, 96.0));
   CHECK(extent_is(&map, 1, 0, 64.0, 48.0));

   t_dsi(&in);
   t_dsi_image.aspect_ratio = 2.0f;
   CHECK(video_views_validate_map(&in, &map, NULL));
   CHECK(extent_is(&map, 2, 0, 64.0, 32.0));
}

static int frect_is(const video_views_frect_t *r, double x, double y,
      double w, double h)
{
   if (     fabs(r->x - x) < 1e-9 && fabs(r->y - y) < 1e-9
         && fabs(r->w - w) < 1e-9 && fabs(r->h - h) < 1e-9)
      return 1;
   printf("    got (%g,%g %gx%g), want (%g,%g %gx%g)\n",
         r->x, r->y, r->w, r->h, x, y, w, h);
   return 0;
}

static void test_inset_anchors(void)
{
   /* Top left to bottom right, as libretro.h numbers them. */
   static const double at[8][2] = {
      {  0.0,   0.0 }, { 480.0,   0.0 }, { 960.0,   0.0 },
      {  0.0, 360.0 },                   { 960.0, 360.0 },
      {  0.0, 720.0 }, { 480.0, 720.0 }, { 960.0, 720.0 }
   };
   video_views_frect_t base, out;
   unsigned a;

   base.x = 0.0;
   base.y = 0.0;
   base.w = 1280.0;
   base.h = 960.0;
   for (a = RETRO_VIDEO_SCREEN_ANCHOR_TOP_LEFT;
         a <= RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT; a++)
   {
      video_views_inset(&base, a, 0.25, 4.0 / 3.0, &out);
      CHECK(frect_is(&out, at[a - 1][0], at[a - 1][1], 320.0, 240.0));
   }
   video_views_inset(&base, RETRO_VIDEO_SCREEN_ANCHOR_DEFAULT, 0.25,
         4.0 / 3.0, &out);
   CHECK(frect_is(&out, 960.0, 720.0, 320.0, 240.0));
   video_views_inset(&base, 9, 0.25, 4.0 / 3.0, &out);
   CHECK(frect_is(&out, 960.0, 720.0, 320.0, 240.0));
   /* In the base's units, from its origin. */
   base.x = 100.0;
   base.y = 50.0;
   video_views_inset(&base, RETRO_VIDEO_SCREEN_ANCHOR_TOP, 0.25,
         4.0 / 3.0, &out);
   CHECK(frect_is(&out, 580.0, 50.0, 320.0, 240.0));
}

static void test_inset_shrink(void)
{
   video_views_frect_t base, out;

   base.x = 0.0;
   base.y = 0.0;
   base.w = 400.0;
   base.h = 300.0;
   video_views_inset(&base, RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT, 1.0,
         2.0, &out);
   CHECK(frect_is(&out, 0.0, 100.0, 400.0, 200.0));
   video_views_inset(&base, RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT, 1.5,
         2.0, &out);
   CHECK(frect_is(&out, 0.0, 100.0, 400.0, 200.0));
}

/* The ds map with screen 1 an inset over screen 0's bottom right, a
 * quarter of its height. */
static void map_ds_inset(video_views_map_t *map)
{
   map_ds(map);
   map->over       = 0x2;
   map->base       = 0;
   map->anchor     = RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT << 4;
   map->size[1]    = 0.25f;
   map->opacity[1] = 255;
}

/* @in sits in @base's bottom-right corner. */
static int t_bottom_right(const video_views_rect_t *in,
      const video_views_rect_t *base)
{
   return VIDEO_POS_X(in->pos) + (int)VIDEO_SCALE_W(in->dims)
         == VIDEO_POS_X(base->pos) + (int)VIDEO_SCALE_W(base->dims)
      && VIDEO_POS_Y(in->pos) + (int)VIDEO_SCALE_H(in->dims)
         == VIDEO_POS_Y(base->pos) + (int)VIDEO_SCALE_H(base->dims);
}

static void test_layout_inset(void)
{
   video_views_map_t map;
   video_views_layout_t l, want;

   map_ds_inset(&map);
   map.left_out = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &want);
   CHECK(want.num_placements == 1
         && rect_is(&want.placements[0].dst, 160, 0, 1280, 960));
   map.left_out = 0;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 2);
   CHECK(   l.placements[0].view == 0 && l.placements[0].flags == 0
         && t_dst_same(&l.placements[0], &want.placements[0]));
   CHECK(   l.placements[1].view == 1
         && rect_is(&l.placements[1].dst, 1120, 720, 320, 240));
   CHECK(l.placements[1].flags == VIDEO_VIEWS_PLACEMENT_INSET);
   CHECK(VIDEO_VIEWS_PLACEMENT_ALPHA(l.placements[1].flags) == 255);
}

/* The one-screen rules see the base alone. */
static void test_layout_inset_rules(void)
{
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;
   video_views_rect_t vp;

   map_ds_inset(&map);
   params_init(&p, &map, 1600, 960);
   p.screen_layout = VIDEO_SCREEN_LAYOUT_HORIZONTAL;
   p.stretch       = true;
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 2);
   CHECK(rect_is(&l.placements[0].dst,    0,   0, 1600, 960));
   CHECK(rect_is(&l.placements[1].dst, 1280, 720,  320, 240));

   p.stretch   = false;
   vp.pos      = VIDEO_POS_PACK(100, 100);
   vp.dims     = VIDEO_SCALE_PACK(800, 600);
   p.custom_vp = &vp;
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 2);
   CHECK(rect_is(&l.placements[0].dst, 100, 100, 800, 600));
   CHECK(rect_is(&l.placements[1].dst, 700, 550, 200, 150));

   /* The inset keeps its own aspect. */
   p.custom_vp     = NULL;
   p.single_aspect = 16.0f / 9.0f;
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 2);
   CHECK(rect_is(&l.placements[0].dst,    0,  30, 1600, 900));
   CHECK(rect_is(&l.placements[1].dst, 1300, 705,  300, 225));
}

static void test_layout_inset_exclude(void)
{
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;

   map_ds_inset(&map);
   params_init(&p, &map, 1600, 960);
   p.screen_layout   = VIDEO_SCREEN_LAYOUT_HORIZONTAL;
   p.exclude_screens = 0x1;
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 0);
   /* An inset goes where its base goes. */
   p.exclude_screens = 0x2;
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 2);
   CHECK(   l.placements[1].view == 1
         && rect_is(&l.placements[1].dst, 1120, 720, 320, 240));
}

static void test_layout_inset_order(void)
{
   video_views_map_t map;
   video_views_layout_t l;

   map_three(&map);
   map.over       = 0x6;
   map.base       = 0;
   map.anchor     = (RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT << 4)
                  | (RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_LEFT  << 8);
   map.size[1]    = 0.25f;
   map.size[2]    = 0.25f;
   map.opacity[1] = 255;
   map.opacity[2] = 255;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 3);
   CHECK(   l.placements[0].view == 0 && l.placements[1].view == 1
         && l.placements[2].view == 2);

   /* 2 over 0, 1 beside 0: 2 comes right after its base. */
   map.over    = 0x4;
   map.size[1] = 0.0f;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 3);
   CHECK(   l.placements[0].view == 0 && l.placements[1].view == 2
         && l.placements[2].view == 1);
   CHECK(   l.placements[1].flags == VIDEO_VIEWS_PLACEMENT_INSET
         && l.placements[2].flags == 0);
}

static void test_layout_inset_alpha(void)
{
   video_views_map_t map;
   video_views_layout_t l;

   map_ds_inset(&map);
   map.opacity[0] = 10;
   map.opacity[1] = 128;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 2);
   CHECK(   l.placements[0].flags == 0
         && VIDEO_VIEWS_PLACEMENT_ALPHA(l.placements[0].flags) == 255);
   CHECK(VIDEO_VIEWS_PLACEMENT_ALPHA(l.placements[1].flags) == 128);

   map.blank = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 2);
   CHECK(l.placements[1].flags == (VIDEO_VIEWS_PLACEMENT_BLANK
         | VIDEO_VIEWS_PLACEMENT_INSET
         | VIDEO_VIEWS_PLACEMENT_ALPHA_BITS(128)));
   CHECK(VIDEO_VIEWS_PLACEMENT_ALPHA(l.placements[1].flags) == 128);
}

/* A mono inset over a stereo base: once in each eye, after the base. */
static void test_layout_inset_stereo(void)
{
   struct retro_video_view v[3];
   video_views_map_t map;
   video_views_layout_t l;
   unsigned i;

   v[0] = mkview(  0,   0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_LEFT);
   v[1] = mkview(256,   0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_RIGHT);
   v[2] = mkview(  0, 192, 256, 192, 1, RETRO_VIDEO_VIEW_EYE_NONE);
   CHECK(video_views_validate(v, 3, &map));
   map.over       = 0x2;
   map.base       = 0;
   map.anchor     = RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT << 4;
   map.size[1]    = 0.25f;
   map.opacity[1] = 255;
   t_side_layout(&map, VIDEO_STEREO_MODE_SBS_FULL, &l);
   CHECK(l.num_placements == 4);
   CHECK(   l.placements[0].view == 0 && l.placements[0].area == 0
         && l.placements[1].view == 2 && l.placements[1].area == 0
         && l.placements[2].view == 1 && l.placements[2].area == 1
         && l.placements[3].view == 2 && l.placements[3].area == 1);
   for (i = 0; i < 4; i += 2)
   {
      CHECK(l.placements[i].flags == 0);
      CHECK(l.placements[i + 1].flags == VIDEO_VIEWS_PLACEMENT_INSET);
      CHECK(t_bottom_right(&l.placements[i + 1].dst,
               &l.placements[i].dst));
   }
   CHECK(rect_is(&l.placements[0].dst,    0, 180, 800, 600));
   CHECK(rect_is(&l.placements[1].dst,  600, 630, 200, 150));
   CHECK(rect_is(&l.placements[2].dst,  800, 180, 800, 600));
   CHECK(rect_is(&l.placements[3].dst, 1400, 630, 200, 150));

   /* Half width: squeezed with its base. */
   t_side_layout(&map, VIDEO_STEREO_MODE_SBS_HALF, &l);
   CHECK(l.num_placements == 4);
   CHECK(rect_is(&l.placements[0].dst,   80,   0, 640, 960));
   CHECK(rect_is(&l.placements[1].dst,  560, 720, 160, 240));
   CHECK(rect_is(&l.placements[2].dst,  880,   0, 640, 960));
   CHECK(rect_is(&l.placements[3].dst, 1360, 720, 160, 240));
}

/* A quarter turn: the inset turns too and stays in the turned base's
 * corner. */
static void test_layout_inset_rotated(void)
{
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;

   map_ds_inset(&map);
   params_init(&p, &map, 1600, 960);
   p.screen_layout = VIDEO_SCREEN_LAYOUT_HORIZONTAL;
   p.rotation      = 1;
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 2);
   CHECK(rect_is(&l.placements[0].dst, 440,   0, 720, 960));
   CHECK(rect_is(&l.placements[1].dst, 980, 720, 180, 240));
   CHECK(   VIDEO_SCALE_W(l.placements[1].dst.dims) * 4
         == VIDEO_SCALE_H(l.placements[1].dst.dims) * 3);
   CHECK(t_bottom_right(&l.placements[1].dst, &l.placements[0].dst));
}

static void test_layout_inset_integer(void)
{
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;
   video_views_frect_t base, in;
   const video_views_rect_t *d;

   map_ds_inset(&map);
   params_init(&p, &map, 1600, 1000);
   p.screen_layout = VIDEO_SCREEN_LAYOUT_HORIZONTAL;
   p.scale_integer = true;
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 2);
   d = &l.placements[0].dst;
   CHECK(VIDEO_SCALE_H(d->dims) % 192 == 0);
   CHECK(rect_is(d, 160, 20, 1280, 960));
   base.x = VIDEO_POS_X(d->pos);
   base.y = VIDEO_POS_Y(d->pos);
   base.w = VIDEO_SCALE_W(d->dims);
   base.h = VIDEO_SCALE_H(d->dims);
   video_views_inset(&base, RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT, 0.25,
         4.0 / 3.0, &in);
   CHECK(rect_is(&l.placements[1].dst, (int)in.x, (int)in.y,
            (unsigned)in.w, (unsigned)in.h));
}

/* @r is @aspect times as wide as tall, within a pixel. */
static int t_aspect_near(const video_views_rect_t *r, double aspect)
{
   double w = VIDEO_SCALE_W(r->dims);
   double h = VIDEO_SCALE_H(r->dims);
   if (fabs(w - h * aspect) <= 1.0)
      return 1;
   printf("    got %gx%g, want w:h %g\n", w, h, aspect);
   return 0;
}

/* @r is @frac of @base's height tall, within a pixel. */
static int t_height_near(const video_views_rect_t *r,
      const video_views_rect_t *base, double frac)
{
   double h    = VIDEO_SCALE_H(r->dims);
   double want = VIDEO_SCALE_H(base->dims) * frac;
   if (fabs(h - want) <= 1.0)
      return 1;
   printf("    got height %g, want %g\n", h, want);
   return 0;
}

/* The picture beside Top, at the one scale. */
static void test_layout_image_root(void)
{
   video_views_map_t map;
   video_views_layout_t l;

   map_dsi(&map);
   map.left_out = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 2);
   CHECK(l.placements[0].view == 0 && l.placements[0].flags == 0);
   CHECK(   l.placements[1].view == 2
         && l.placements[1].flags == VIDEO_VIEWS_PLACEMENT_IMAGE);
   CHECK(t_aspect_near(&l.placements[1].dst, 1.5));
   CHECK(t_height_near(&l.placements[1].dst, &l.placements[0].dst,
            32.0 / 192.0));
   CHECK(rect_is(&l.placements[1].dst, 1320, 400, 240, 160));
}

static void test_layout_image_sized(void)
{
   video_views_map_t map;
   video_views_layout_t l;

   map_dsi(&map);
   map.base    = 0;
   map.size[2] = 0.25f;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 3);
   CHECK(   l.placements[2].view == 2
         && l.placements[2].flags == VIDEO_VIEWS_PLACEMENT_IMAGE);
   CHECK(t_height_near(&l.placements[2].dst, &l.placements[0].dst, 0.25));
   CHECK(t_aspect_near(&l.placements[2].dst, 1.5));
}

static void test_layout_image_inset(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   const video_views_placement_t *img = &l.placements[1];

   map_dsi(&map);
   map.over       = 0x4;
   map.base       = 0;
   map.anchor     = RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT << 8;
   map.size[2]    = 0.25f;
   map.opacity[2] = 128;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 3);
   CHECK(   l.placements[0].view == 0 && img->view == 2
         && l.placements[2].view == 1);
   CHECK(img->flags == (VIDEO_VIEWS_PLACEMENT_IMAGE
         | VIDEO_VIEWS_PLACEMENT_INSET
         | VIDEO_VIEWS_PLACEMENT_ALPHA_BITS(128)));
   CHECK(t_bottom_right(&img->dst, &l.placements[0].dst));
   CHECK(t_height_near(&img->dst, &l.placements[0].dst, 0.25));
   CHECK(t_aspect_near(&img->dst, 1.5));
   CHECK(rect_is(&img->dst, 575, 630, 225, 150));

   map.blank = 0x4;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 3 && img->view == 2);
   CHECK(img->flags == (VIDEO_VIEWS_PLACEMENT_IMAGE
         | VIDEO_VIEWS_PLACEMENT_INSET | VIDEO_VIEWS_PLACEMENT_BLANK
         | VIDEO_VIEWS_PLACEMENT_ALPHA_BITS(128)));
}

/* Stretch, the aspect setting and a custom viewport leave the picture
 * its aspect. */
static void test_layout_image_alone(void)
{
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;
   video_views_rect_t vp;

   map_dsi(&map);
   map.left_out = 0x3;
   params_init(&p, &map, 1600, 960);
   p.screen_layout = VIDEO_SCREEN_LAYOUT_HORIZONTAL;
   p.stretch       = true;
   video_views_layout(&p, &l);
   CHECK(   l.num_placements == 1 && l.placements[0].view == 2
         && l.placements[0].flags == VIDEO_VIEWS_PLACEMENT_IMAGE);
   CHECK(rect_is(&l.placements[0].dst, 80, 0, 1440, 960));

   p.stretch       = false;
   p.single_aspect = 16.0f / 9.0f;
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 1);
   CHECK(rect_is(&l.placements[0].dst, 80, 0, 1440, 960));

   p.single_aspect = 0.0f;
   vp.pos          = VIDEO_POS_PACK(100, 100);
   vp.dims         = VIDEO_SCALE_PACK(800, 600);
   p.custom_vp     = &vp;
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 1);
   CHECK(rect_is(&l.placements[0].dst, 80, 0, 1440, 960));

   /* A map of the picture alone, with no view, as a selection of it
    * gives */
   memset(&map, 0, sizeof(map));
   map.num_screens     = 1;
   map.image           = 0x1;
   map.image_dims[0]   = VIDEO_SCALE_PACK(48, 32);
   map.image_aspect[0] = 1.5f;
   p.custom_vp         = NULL;
   video_views_layout(&p, &l);
   CHECK(   l.num_placements == 1 && l.placements[0].view == 0
         && l.placements[0].flags == VIDEO_VIEWS_PLACEMENT_IMAGE);
   CHECK(rect_is(&l.placements[0].dst, 80, 0, 1440, 960));
}

/* An image beside a stereo screen: one placement in each eye's area. */
static void test_layout_image_stereo(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   struct retro_video_views in;
   const video_views_rect_t *a, *b;

   t_dsi(&in);
   t_dsi_views[0]   = mkview(  0, 0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_LEFT);
   t_dsi_views[1]   = mkview(256, 0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_RIGHT);
   t_dsi_screens[1] = t_dsi_screens[2];
   in.num_screens   = 2;
   CHECK(video_views_validate_map(&in, &map, NULL));
   CHECK(map.image == 0x2);
   t_side_layout(&map, VIDEO_STEREO_MODE_SBS_FULL, &l);
   CHECK(l.num_placements == 4);
   CHECK(   l.placements[0].view == 0 && l.placements[0].area == 0
         && l.placements[0].flags == 0
         && l.placements[2].view == 1 && l.placements[2].area == 1
         && l.placements[2].flags == 0);
   CHECK(   l.placements[1].view == 1 && l.placements[1].area == 0
         && l.placements[1].flags == VIDEO_VIEWS_PLACEMENT_IMAGE);
   CHECK(   l.placements[3].view == 1 && l.placements[3].area == 1
         && l.placements[3].flags == VIDEO_VIEWS_PLACEMENT_IMAGE);
   CHECK(video_views_placement_screen(&map, &l.placements[2]) == 0);
   CHECK(video_views_placement_screen(&map, &l.placements[3]) == 1);
   /* The same spot in each eye */
   a = &l.placements[1].dst;
   b = &l.placements[3].dst;
   CHECK(   b->dims == a->dims
         && b->pos  == VIDEO_POS_PACK(VIDEO_POS_X(a->pos) + 800,
               VIDEO_POS_Y(a->pos)));
}

/* An IMAGE placement's view is its screen, here also a view's index. */
static void test_layout_image_view_index(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   video_views_rect_t r;

   CHECK(map_dsi_index(&map));
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 2);
   CHECK(l.placements[0].view == 0 && l.placements[0].flags == 0);
   CHECK(   l.placements[1].view == 2
         && l.placements[1].flags == VIDEO_VIEWS_PLACEMENT_IMAGE);
   CHECK(video_views_placement_screen(&map, &l.placements[0]) == 0);
   CHECK(video_views_placement_screen(&map, &l.placements[1]) == 2);
   CHECK(!video_views_first_drawn(&l, 2, true,
            VIDEO_SCALE_PACK(1600, 960), &r));
   CHECK(   video_views_first_drawn(&l, 0, true,
            VIDEO_SCALE_PACK(1600, 960), &r)
         && r.pos == l.placements[0].dst.pos
         && r.dims == l.placements[0].dst.dims);
}

/* Top an inset over the picture: after it, in its bottom-right corner. */
static void test_layout_view_over_image(void)
{
   video_views_map_t map;
   video_views_layout_t l;

   map_dsi(&map);
   map.over       = 0x1;
   map.base       = 2;
   map.anchor     = RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT;
   map.size[0]    = 0.5f;
   map.opacity[0] = 255;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 3);
   CHECK(l.placements[0].view == 1 && l.placements[0].flags == 0);
   CHECK(   l.placements[1].view == 2
         && l.placements[1].flags == VIDEO_VIEWS_PLACEMENT_IMAGE);
   CHECK(   l.placements[2].view == 0
         && l.placements[2].flags == VIDEO_VIEWS_PLACEMENT_INSET);
   CHECK(t_bottom_right(&l.placements[2].dst, &l.placements[1].dst));
   CHECK(rect_is(&l.placements[1].dst, 1320, 400, 240, 160));
   CHECK(rect_is(&l.placements[2].dst, 1453, 480, 107, 80));
}

/* An image screen comes with its fields, renumbered; selected alone it
 * has a place without a view. */
static void test_select_screens_image(void)
{
   video_views_map_t map, out;
   video_views_layout_params_t p;
   video_views_layout_t l;
   unsigned index[RETRO_VIDEO_VIEWS_MAX];

   map_dsi(&map);
   map.image_tex[2] = 77;
   map.image_gen[2] = 3;
   map.nearest      = 0x4;
   CHECK(video_views_select_screens(&map, 0x5, &out, index));
   CHECK(out.num_screens == 2 && out.num_views == 1 && index[0] == 0);
   CHECK(out.image == 0x2 && out.no_input == 0x2 && out.nearest == 0x2);
   CHECK(out.image_dims[1] == VIDEO_SCALE_PACK(48, 32));
   CHECK(out.image_aspect[1] == 1.5f);
   CHECK(out.image_tex[1] == 77 && out.image_gen[1] == 3);
   CHECK(out.image_dims[0] == 0 && out.image_tex[0] == 0);

   CHECK(video_views_select_screens(&map, 0x4, &out, index));
   CHECK(out.num_screens == 1 && out.num_views == 0);
   CHECK(out.image == 0x1 && out.nearest == 0x1);
   CHECK(   out.image_dims[0] == VIDEO_SCALE_PACK(48, 32)
         && out.image_aspect[0] == 1.5f && out.image_tex[0] == 77);
   params_init(&p, &out, 1600, 960);
   video_views_layout(&p, &l);
   CHECK(   l.num_placements == 1 && l.placements[0].view == 0
         && l.placements[0].flags == VIDEO_VIEWS_PLACEMENT_IMAGE);

   /* No view and no image: nothing selected. */
   map.image = 0;
   CHECK(!video_views_select_screens(&map, 0x4, &out, index));
   CHECK(out.num_screens == 0 && out.image == 0);
}

/* A touch never lands on an image, which takes none of the entries. */
static void test_touch_publish_image(void)
{
   video_views_map_t map, sub;
   video_views_layout_params_t p;
   video_views_layout_t l;
   video_views_touch_t t;
   unsigned i, index[RETRO_VIDEO_VIEWS_MAX];
   unsigned frame = VIDEO_SCALE_PACK(256, 384);

   memset(&t, 0, sizeof(t));
   memset(index, 0, sizeof(index));
   map_dsi(&map);
   CHECK(video_views_select_screens(&map, 0x5, &sub, index));
   params_init(&p, &sub, 1600, 960);
   p.screen_layout = VIDEO_SCREEN_LAYOUT_HORIZONTAL;
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 2);
   video_views_touch_publish(&t, &map, &l, index, p.dims, frame);
   CHECK(t.count == 1 && t.no_input == 0 && t.src[0].screen == 0);
   CHECK(t.dst[0].pos == l.placements[0].dst.pos);

   /* Placement view 2 is the image's screen, not Bottom's view. */
   for (i = 0; i < RETRO_VIDEO_VIEWS_MAX; i++)
      index[i] = i;
   CHECK(map_dsi_index(&map));
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 2);
   video_views_touch_publish(&t, &map, &l, index, p.dims,
         VIDEO_SCALE_PACK(512, 384));
   CHECK(t.count == 1);
   for (i = 0; i < t.count; i++)
      CHECK(t.src[i].y != 192);
}

/* A point on an image inset reaches its base; with the base NO_INPUT
 * too, it is off every view. */
static void test_map_point_image_inset(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   uint32_t pos, want;
   unsigned frame = VIDEO_SCALE_PACK(256, 384);

   map_dsi_inset(&map);
   map.left_out = 0x6;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(video_views_map_point(&l, &map, frame, 1260, 840, true, &want));
   CHECK(VIDEO_POS_Y(want) < 0 && VIDEO_POS_Y(want) != -0x8000);
   map.left_out = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(   l.num_placements == 2
         && rect_is(&l.placements[1].dst, 1080, 720, 360, 240));
   pos = 0;
   CHECK(video_views_map_point(&l, &map, frame, 1260, 840, true, &pos));
   CHECK(pos == want);

   map.no_input |= 0x1;
   pos = 0;
   CHECK(video_views_map_point(&l, &map, frame, 1260, 840, true, &pos));
   CHECK(pos == VIDEO_POS_PACK(-0x8000, -0x8000));
}

/* An image root alone: every point is off every view. */
static void test_map_point_image_root(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   uint32_t pos;
   int cx, cy;
   unsigned frame = VIDEO_SCALE_PACK(256, 384);

   map_dsi(&map);
   map.left_out = 0x3;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(   l.num_placements == 1
         && l.placements[0].flags == VIDEO_VIEWS_PLACEMENT_IMAGE);
   cx = VIDEO_POS_X(l.placements[0].dst.pos)
      + (int)VIDEO_SCALE_W(l.placements[0].dst.dims) / 2;
   cy = VIDEO_POS_Y(l.placements[0].dst.pos)
      + (int)VIDEO_SCALE_H(l.placements[0].dst.dims) / 2;
   pos = 0;
   CHECK(video_views_map_point(&l, &map, frame, cx, cy, true, &pos));
   CHECK(pos == VIDEO_POS_PACK(-0x8000, -0x8000));
   pos = 0;
   CHECK(video_views_map_point(&l, &map, frame, cx, cy, false, &pos));
   CHECK(pos == VIDEO_POS_PACK(-0x8000, -0x8000));
}

/* An image placement drawn through VV_IMAGE at its opacity, never
 * through its view number's chain or view. */
static void test_draw_placements_image(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   video_views_chains_t set;
   video_views_rect_t rects[RETRO_VIDEO_VIEWS_MAX];
   fake_drv_t d;
   fake_chain_t *chains[RETRO_VIDEO_VIEWS_MAX];
   unsigned full = VIDEO_SCALE_PACK(1600, 960);

   memset(&d, 0, sizeof(d));
   memset(chains, 0, sizeof(chains));
   chains[0] = &chain_objs[0];
   chains[1] = &chain_objs[1];
   chains[2] = &chain_objs[2];

   map_dsi_inset(&map);
   map.opacity[2]   = 128;
   map.image_tex[2] = 77;
   map.nearest      = 0x4;
   fake_map         = &map;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(t_calls_are("DAIA"));
   CHECK(   draws[0].view == 0 && draws[1].alpha == 128
         && draws[2].view == 2 && draws[2].nearest == 1
         && draws[3].alpha == 255);
   CHECK(   rect_is(&draws[2].dst, 1080, 720, 360, 240)
         && rect_is(&draws[2].cut, 1080, 720, 360, 240));
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, false, false);
   CHECK(t_calls_are("PAIA"));
   CHECK(draws[0].chain == chains[0] && draws[2].view == 2);
   map.nearest = 0x1;
   num_draws   = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(t_calls_are("DAIA") && draws[2].nearest == 0);

   /* No picture yet: not drawn. */
   map.image_tex[2] = 0;
   num_draws        = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(t_calls_are("D") && draws[0].view == 0);

   /* Blank: an inset filled, with or without a picture; a root left
    * alone. */
   map.image_tex[2] = 77;
   map.blank        = 0x4;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(t_calls_are("DAFA"));
   CHECK(   draws[2].alpha == 128
         && rect_is(&draws[2].cut, 1080, 720, 360, 240));
   map.image_tex[2] = 0;
   num_draws        = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(t_calls_are("DAFA"));
   map_dsi(&map);
   map.left_out     = 0x3;
   map.blank        = 0x4;
   map.image_tex[2] = 77;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 1);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(num_draws == 0);

   /* Placement view 2 is the image's screen: view 2, Bottom's, gets no
    * chain, and the image is drawn without one. */
   CHECK(map_dsi_index(&map));
   map.image_tex[2] = 77;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   memset(&set, 0, sizeof(set));
   set.count = 3;
   CHECK(fake_vv_pick_chains(&d, &set, &map, &l, true, true, full, chains,
            rects));
   CHECK(chains[0] && !chains[1] && !chains[2] && d.stock_made == 1);
   num_draws = 0;
   fake_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, false, false);
   CHECK(t_calls_are("PI") && draws[0].chain == chains[0]
         && draws[1].view == 2);
   fake_map = NULL;
   CHECK(!bad_ctx);
}

/* Without VV_IMAGE an image placement is not drawn. */
static void test_draw_placements_image_no_hook(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   fake_drv_t d;
   fake_chain_t *chains[RETRO_VIDEO_VIEWS_MAX];
   unsigned full = VIDEO_SCALE_PACK(1600, 960);

   memset(&d, 0, sizeof(d));
   memset(chains, 0, sizeof(chains));
   chains[0] = &chain_objs[0];
   chains[1] = &chain_objs[1];
   chains[2] = &chain_objs[2];

   map_dsi_inset(&map);
   map.image_tex[2] = 77;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 2);
   num_draws = 0;
   plain_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, true, false);
   CHECK(t_calls_are("D") && draws[0].view == 0);
   num_draws = 0;
   plain_vv_draw_placements(&d, &fake_ctx, &l, chains, true, full, false, false);
   CHECK(t_calls_are("P") && draws[0].chain == chains[0]);
   CHECK(!bad_ctx);
}

static void test_roots_with_insets(void)
{
   struct retro_video_view v[4];
   video_views_map_t map;

   v[0] = mkview(0,   0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   v[1] = mkview(0, 192, 256, 192, 1, RETRO_VIDEO_VIEW_EYE_NONE);
   v[2] = mkview(0, 384, 256, 192, 3, RETRO_VIDEO_VIEW_EYE_NONE);
   v[3] = mkview(0, 576, 256, 192, 4, RETRO_VIDEO_VIEW_EYE_NONE);
   t_absent_map(v, 4, 5, 0x4, &map);
   CHECK(map.num_screens == 5 && map.viewless == 0x4);
   map.left_out = 0x8;
   map.over     = 0x2;
   map.base     = 0;
   CHECK(video_views_roots(&map) == 0x11);
   CHECK(video_views_with_insets(&map, 0x1)  == 0x3);
   CHECK(video_views_with_insets(&map, 0x10) == 0x10);
   /* A left-out inset is not taken along. */
   map.over = 0xa;
   CHECK(video_views_roots(&map) == 0x11);
   CHECK(video_views_with_insets(&map, 0x1) == 0x3);
}

/* Screen 0 an inset of screen 1: the top LCD's or TV's share is empty,
 * and the other display takes both. */
static void test_select_screens_screen0_inset(void)
{
   video_views_map_t map, sub, empty;
   unsigned index[RETRO_VIDEO_VIEWS_MAX];

   map_ds(&map);
   map.over       = 0x1;
   map.base       = 1;
   map.anchor     = RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT;
   map.size[0]    = 0.25f;
   map.opacity[0] = 128;
   CHECK(video_views_roots(&map) == 0x2);
   memset(&empty, 0, sizeof(empty));
   memset(&sub, 0xa5, sizeof(sub));
   CHECK(!video_views_select_screens(&map, 0x2 & 1u, &sub, index));
   CHECK(!memcmp(&sub, &empty, sizeof(sub)));
   CHECK(video_views_select_screens(&map, 0x2, &sub, index));
   CHECK(sub.num_screens == 2 && sub.num_views == 2 && sub.over == 0x1);
   CHECK(VIDEO_VIEWS_NIBBLE(sub.base, 0) == 1);
   CHECK(index[0] == 0 && index[1] == 1);
   CHECK(   VIDEO_VIEWS_NIBBLE(sub.anchor, 0)
         == RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT);
   CHECK(sub.size[0] == 0.25f && sub.opacity[0] == 128);
}

/* A point on a drawn inset is the inset's, solid or not. */
static void test_map_point_inset(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   uint32_t pos, want;
   unsigned frame = VIDEO_SCALE_PACK(256, 384);

   map_ds_inset(&map);
   map.left_out = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(video_views_map_point(&l, &map, frame, 1000, 840, true, &want));
   CHECK(VIDEO_POS_Y(want) < 0);
   map.left_out = 0;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(rect_is(&l.placements[1].dst, 1120, 720, 320, 240));
   /* The inset's centre is Bottom's: packed pixel (128, 288). */
   CHECK(video_views_map_point(&l, &map, frame, 1280, 840, true, &pos));
   CHECK(VIDEO_POS_X(pos) == 128 && VIDEO_POS_Y(pos) == 16511);
   /* Beside it, Top's. */
   pos = 0;
   CHECK(video_views_map_point(&l, &map, frame, 1000, 840, true, &pos));
   CHECK(pos == want);

   map.opacity[1] = 26;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(VIDEO_VIEWS_PLACEMENT_ALPHA(l.placements[1].flags) == 26);
   pos = 0;
   CHECK(video_views_map_point(&l, &map, frame, 1280, 840, true, &pos));
   CHECK(VIDEO_POS_X(pos) == 128 && VIDEO_POS_Y(pos) == 16511);
}

/* A point on a NO_INPUT inset reaches its base. */
static void test_map_point_inset_no_input(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   uint32_t pos, want;
   unsigned frame = VIDEO_SCALE_PACK(256, 384);

   map_ds_inset(&map);
   map.left_out = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(video_views_map_point(&l, &map, frame, 1280, 840, true, &want));
   CHECK(VIDEO_POS_Y(want) < 0);
   map.left_out = 0;
   map.no_input = 0x2;
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(l.num_placements == 2);
   pos = 0;
   CHECK(video_views_map_point(&l, &map, frame, 1280, 840, true, &pos));
   CHECK(pos == want);
}

/* 61 px right of both rectangles' last column: the clamp's tie goes to
 * the inset, Bottom's packed pixel (255, 336). */
static void test_map_point_inset_tie(void)
{
   video_views_map_t map;
   video_views_layout_t l;
   uint32_t pos;
   unsigned frame = VIDEO_SCALE_PACK(256, 384);

   map_ds_inset(&map);
   t_side_layout(&map, VIDEO_STEREO_MODE_2D, &l);
   CHECK(rect_is(&l.placements[0].dst, 160, 0, 1280, 960));
   CHECK(rect_is(&l.placements[1].dst, 1120, 720, 320, 240));
   CHECK(video_views_map_point(&l, &map, frame, 1500, 900, false, &pos));
   CHECK(VIDEO_POS_X(pos) == 0x7fff && VIDEO_POS_Y(pos) == 24724);
}

static void test_map_point_inset_rotated(void)
{
   video_views_map_t map;
   video_views_layout_params_t p;
   video_views_layout_t l;
   uint32_t pos;
   unsigned frame = VIDEO_SCALE_PACK(256, 384);

   map_ds_inset(&map);
   params_init(&p, &map, 1600, 960);
   p.screen_layout = VIDEO_SCREEN_LAYOUT_HORIZONTAL;
   p.rotation      = 1;
   video_views_layout(&p, &l);
   CHECK(rect_is(&l.placements[1].dst, 980, 720, 180, 240));
   /* The inset's centre is Bottom's: packed pixel (128, 288). */
   CHECK(video_views_map_point(&l, &map, frame, 1070, 840, true, &pos));
   CHECK(VIDEO_POS_X(pos) == 128 && VIDEO_POS_Y(pos) == 16511);
}

static void test_touch_point_inset(void)
{
   video_views_map_t map, sub;
   video_views_layout_params_t p;
   video_views_layout_t l;
   video_views_touch_t t;
   unsigned index[RETRO_VIDEO_VIEWS_MAX];
   int16_t x, y, top_x, top_y;
   unsigned frame = VIDEO_SCALE_PACK(256, 384);

   memset(&t, 0, sizeof(t));
   map_ds_inset(&map);
   map.left_out = 0x2;
   CHECK(video_views_select_screens(&map, video_views_roots(&map),
            &sub, index));
   params_init(&p, &sub, 1600, 960);
   p.screen_layout = VIDEO_SCREEN_LAYOUT_HORIZONTAL;
   video_views_layout(&p, &l);
   video_views_touch_publish(&t, &map, &l, index, p.dims, frame);
   /* (1280, 840) of 1600x960, the inset's centre */
   CHECK(video_views_touch_point(&t, 19660, 24575, &top_x, &top_y));
   CHECK(top_x != -0x8000 && top_y < 0);

   map.left_out = 0;
   CHECK(video_views_select_screens(&map, video_views_roots(&map),
            &sub, index));
   video_views_layout(&p, &l);
   CHECK(l.num_placements == 2
         && rect_is(&l.placements[1].dst, 1120, 720, 320, 240));
   video_views_touch_publish(&t, &map, &l, index, p.dims, frame);
   /* Bottom's packed pixel (127, 287) */
   CHECK(video_views_touch_point(&t, 19660, 24575, &x, &y));
   CHECK(x == -256 && y == 16212);

   map.no_input = 0x2;
   video_views_touch_publish(&t, &map, &l, index, p.dims, frame);
   x = y = 0;
   CHECK(video_views_touch_point(&t, 19660, 24575, &x, &y));
   CHECK(x == top_x && y == top_y);
}

int main(void)
{
   test_screen_defaults();
   test_screen_normalize();
   test_validate();
   test_frame_helpers();
   test_snapshot_image();
   test_eye_pair();
   test_select_screens_roots();
   test_screens_full();
   test_screens_absent_flag();
   test_screens_old_struct();
   test_screens_refused();
   test_screens_image();
   test_screens_image_refused();
   test_screens_accepted();
   test_screens_names();
   test_screens_select_masks();
   test_select_screens_left_out();
   test_blend();
   test_cpu_compose();
   test_cpu_draw_blank();
   test_cpu_draw_blank_inset();
   test_cpu_scale_none();
   test_cut_source();
   test_touch();
   test_layout_screens();
   test_layout_same();
   test_layout_stereo();
   test_layout_one_screen();
   test_clip();
   test_map_point();
   test_ui_point();
   test_pick_chains();
   test_draw_placements();
   test_draw_placements_blank();
   test_draw_placements_alpha();
   test_draw_placements_fill();
   test_draw_placements_no_hooks();
   test_targets();
   test_ui_helpers();
   test_screen_api();
   test_layout_viewless();
   test_layout_left_out();
   test_layout_blank();
   test_layout_rotated_nonsquare();
   test_layout_shown_sized();
   test_extent_view_rule();
   test_extent_base_size();
   test_extent_chain_cycle();
   test_extent_torn_base();
   test_extent_image();
   test_inset_anchors();
   test_inset_shrink();
   test_layout_inset();
   test_layout_inset_rules();
   test_layout_inset_exclude();
   test_layout_inset_order();
   test_layout_inset_alpha();
   test_layout_inset_stereo();
   test_layout_inset_rotated();
   test_layout_inset_integer();
   test_layout_image_root();
   test_layout_image_sized();
   test_layout_image_inset();
   test_layout_image_alone();
   test_layout_image_stereo();
   test_layout_image_view_index();
   test_layout_view_over_image();
   test_select_screens_image();
   test_touch_publish_image();
   test_map_point_image_inset();
   test_map_point_image_root();
   test_draw_placements_image();
   test_draw_placements_image_no_hook();
   test_roots_with_insets();
   test_select_screens_screen0_inset();
   test_map_point_inset();
   test_map_point_inset_no_input();
   test_map_point_inset_tie();
   test_map_point_inset_rotated();
   test_touch_point_inset();
   test_map_point_no_input();
   test_map_point_blank();
   test_touch_no_input();
   test_point_no_input();
   test_names_box();
   test_prefs_resolve_order();
   test_prefs_resolve_over();
   test_prefs_over_value();
   test_prefs_resolve_fields();
   test_arrange_masks();
   test_prefs_set();
   test_prefs_key();
   test_arrange_core_over();
   test_arrange_user_values();
   test_arrange_user_over();
   test_arrange_user_over_invalid();
   test_arrange_flatten();
   test_arrange_mutual();
   test_arrange_user_wins();
   test_arrange_base_left_out();
   test_arrange_blank_inset();
   test_arrange_shown_sized();
   test_arrange_images_off();
   test_arrange_rules_out();

   if (failures)
   {
      printf("video_views: FAILED (%d)\n", failures);
      return 1;
   }
   printf("video_views: ok\n");
   return 0;
}
