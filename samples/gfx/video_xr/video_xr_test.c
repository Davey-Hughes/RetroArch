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

/* Harness for gfx/video_xr.c: quad placement, swapchain image sizes,
 * recentering, ray hits and quad points mapped back to the core's
 * frame. Exit 0 on pass, 1 on any failure. */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "gfx/video_xr.h"
#include "gfx/video_defines.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { \
   printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
   failures++; } } while (0)

#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-3)

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

/* 3DS: top eyes side by side, bottom centred below. Frame 800x480. */
static void map_3ds(video_views_map_t *map)
{
   struct retro_video_view v[3];
   v[0] = mkview(  0,   0, 400, 240, 0, RETRO_VIDEO_VIEW_EYE_LEFT);
   v[1] = mkview(400,   0, 400, 240, 0, RETRO_VIDEO_VIEW_EYE_RIGHT);
   v[2] = mkview(240, 240, 320, 240, 1, RETRO_VIDEO_VIEW_EYE_NONE);
   video_views_validate(v, 3, map);
}

/* DS: two 256x192 screens stacked. Frame 256x384. */
static void map_ds(video_views_map_t *map)
{
   struct retro_video_view v[2];
   v[0] = mkview(0,   0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_NONE);
   v[1] = mkview(0, 192, 256, 192, 1, RETRO_VIDEO_VIEW_EYE_NONE);
   video_views_validate(v, 2, map);
}

static void params_init(video_xr_params_t *p, const video_views_map_t *map)
{
   memset(p, 0, sizeof(*p));
   p->map        = map;
   video_xr_pose_identity(&p->anchor);
   p->distance   = 1.8f;
   p->width      = 1.6f;
   p->px_per_rad = 1000.0f;
   p->max_dim    = 4096;
}

static int pos_is(const video_xr_quad_t *q, double x, double y, double z)
{
   return NEAR(q->pose.position.x, x)
      && NEAR(q->pose.position.y, y)
      && NEAR(q->pose.position.z, z);
}

static int dims_is(unsigned dims, unsigned w, unsigned h)
{
   return VIDEO_SCALE_W(dims) == w && VIDEO_SCALE_H(dims) == h;
}

static void test_image_dims(void)
{
   /* 1.6 m at 1.8 m spans 0.8364 rad: 837 px at 1000 px/rad. */
   CHECK(dims_is(video_xr_image_dims(1.6f, 0.96f, 1.8f, 1000.0f,
               VIDEO_SCALE_PACK(400, 240), 4096), 837, 502));
   /* Capped, keeping the shape. */
   CHECK(dims_is(video_xr_image_dims(1.6f, 0.96f, 1.8f, 1000.0f,
               VIDEO_SCALE_PACK(400, 240), 500), 500, 300));
   CHECK(dims_is(video_xr_image_dims(1.28f, 0.96f, 1.8f, 1000.0f,
               VIDEO_SCALE_PACK(320, 240), 4096), 684, 513));
   /* Never below the source's own pixels. */
   CHECK(dims_is(video_xr_image_dims(1.6f, 0.96f, 1.8f, 0.0f,
               VIDEO_SCALE_PACK(400, 240), 4096), 400, 240));
   CHECK(dims_is(video_xr_image_dims(1.6f, 0.96f, 1.8f, 200.0f,
               VIDEO_SCALE_PACK(400, 240), 4096), 400, 240));
   CHECK(video_xr_image_dims(0.0f, 0.96f, 1.8f, 1000.0f, 0, 4096) == 0);
}

static void test_place_3ds(void)
{
   video_views_map_t map;
   video_xr_params_t p;
   video_xr_quad_set_t set;

   map_3ds(&map);
   params_init(&p, &map);
   video_xr_place(&p, &set);

   CHECK(set.num_quads == 3);
   /* Screen 0: one image with an eye per layer, one quad per eye. */
   CHECK(set.quads[0].eye == VIDEO_XR_EYE_LEFT  && set.quads[0].layer == 0);
   CHECK(set.quads[1].eye == VIDEO_XR_EYE_RIGHT && set.quads[1].layer == 1);
   CHECK(set.quads[0].slot == 0 && set.quads[1].slot == 0);
   CHECK(set.quads[0].kind == VIDEO_XR_QUAD_SCREEN && set.quads[0].screen == 0);
   CHECK(pos_is(&set.quads[0], 0.0, 0.0, -1.8));
   CHECK(pos_is(&set.quads[1], 0.0, 0.0, -1.8));
   CHECK(NEAR(set.quads[0].width, 1.6) && NEAR(set.quads[0].height, 0.96));
   CHECK(NEAR(set.quads[0].pose.orientation.w, 1.0));
   CHECK(set.slots[0].layers == 2);
   CHECK(set.slots[0].view[0] == 0 && set.slots[0].view[1] == 1);
   CHECK(dims_is(set.slots[0].dims, 837, 502));
   /* Screen 1: 320 of 400 px wide, a 2% gap below screen 0. */
   CHECK(set.quads[2].eye == VIDEO_XR_EYE_BOTH && set.quads[2].slot == 1);
   CHECK(set.quads[2].screen == 1 && set.quads[2].layer == 0);
   CHECK(pos_is(&set.quads[2], 0.0, -0.992, -1.8));
   CHECK(NEAR(set.quads[2].width, 1.28) && NEAR(set.quads[2].height, 0.96));
   CHECK(set.slots[1].layers == 1 && set.slots[1].view[0] == 2);
   CHECK(dims_is(set.slots[1].dims, 684, 513));
   /* Nothing else, and no menu. */
   CHECK(set.slots[2].dims == 0 && set.slots[VIDEO_XR_MENU_SLOT].dims == 0);
}

static void test_place_swap_horizontal(void)
{
   video_views_map_t map;
   video_xr_params_t p;
   video_xr_quad_set_t set;

   map_3ds(&map);
   params_init(&p, &map);
   p.swap_eyes = true;
   video_xr_place(&p, &set);
   CHECK(set.quads[0].eye == VIDEO_XR_EYE_LEFT  && set.quads[0].layer == 1);
   CHECK(set.quads[1].eye == VIDEO_XR_EYE_RIGHT && set.quads[1].layer == 0);
   /* The image still holds the left view in layer 0. */
   CHECK(set.slots[0].view[0] == 0 && set.slots[0].view[1] == 1);

   params_init(&p, &map);
   p.screen_layout = VIDEO_SCREEN_LAYOUT_HORIZONTAL;
   video_xr_place(&p, &set);
   CHECK(pos_is(&set.quads[2], 1.472, 0.0, -1.8));
}

static void test_place_ds_rotation(void)
{
   video_views_map_t map;
   video_xr_params_t p;
   video_xr_quad_set_t set;

   map_ds(&map);
   params_init(&p, &map);
   video_xr_place(&p, &set);
   CHECK(set.num_quads == 2);
   CHECK(set.quads[0].eye == VIDEO_XR_EYE_BOTH && set.quads[1].eye == VIDEO_XR_EYE_BOTH);
   CHECK(NEAR(set.quads[1].width, 1.6) && NEAR(set.quads[1].height, 1.2));
   CHECK(pos_is(&set.quads[1], 0.0, -1.232, -1.8));

   /* A quarter turn swaps each view's display width and height. */
   map_3ds(&map);
   params_init(&p, &map);
   p.rotation = 1;
   video_xr_place(&p, &set);
   CHECK(NEAR(set.quads[0].width, 1.6) && NEAR(set.quads[0].height, 2.6667));
   CHECK(NEAR(set.quads[2].width, 1.6) && NEAR(set.quads[2].height, 2.1333));
}

static void test_place_frame_menu(void)
{
   video_xr_params_t p;
   video_xr_quad_set_t set;

   /* No map: one quad of the whole frame at its display aspect. */
   params_init(&p, NULL);
   p.frame_dims   = VIDEO_SCALE_PACK(800, 480);
   p.frame_aspect = 800.0f / 480.0f;
   video_xr_place(&p, &set);
   CHECK(set.num_quads == 1);
   CHECK(set.quads[0].kind == VIDEO_XR_QUAD_FRAME && set.quads[0].slot == 0);
   CHECK(NEAR(set.quads[0].width, 1.6) && NEAR(set.quads[0].height, 0.96));
   CHECK(set.slots[0].layers == 1 && set.slots[0].view[0] == -1);
   CHECK(dims_is(set.slots[0].dims, 837, 502));

   /* The UI floats 10 cm in front of screen 0, as wide, at its size. */
   p.ui_dims = VIDEO_SCALE_PACK(1600, 960);
   video_xr_place(&p, &set);
   CHECK(set.num_quads == 2);
   CHECK(set.quads[1].kind == VIDEO_XR_QUAD_MENU);
   CHECK(set.quads[1].slot == VIDEO_XR_MENU_SLOT);
   CHECK(pos_is(&set.quads[1], 0.0, 0.0, -1.7));
   CHECK(NEAR(set.quads[1].width, 1.6) && NEAR(set.quads[1].height, 0.96));
   CHECK(set.slots[VIDEO_XR_MENU_SLOT].dims == VIDEO_SCALE_PACK(1600, 960));

   /* Nothing to place. */
   p.width = 0.0f;
   video_xr_place(&p, &set);
   CHECK(set.num_quads == 0);
}

static void test_place_menu_size(void)
{
   video_xr_params_t p;
   video_xr_quad_set_t set;

   params_init(&p, NULL);
   p.frame_dims = VIDEO_SCALE_PACK(800, 480);

   /* At 200 px/rad the headset shows 176 px across the menu: its image
    * gets twice that, in the UI's shape. */
   p.px_per_rad = 200.0f;
   p.ui_dims    = VIDEO_SCALE_PACK(1600, 960);
   video_xr_place(&p, &set);
   CHECK(set.quads[1].kind == VIDEO_XR_QUAD_MENU);
   CHECK(NEAR(set.quads[1].width, 1.6) && NEAR(set.quads[1].height, 0.96));
   CHECK(dims_is(set.slots[VIDEO_XR_MENU_SLOT].dims, 352, 211));

   /* Wider than a swapchain may be: capped, keeping the shape, with or
    * without a density; a lower density caps it first. */
   p.ui_dims    = VIDEO_SCALE_PACK(5120, 2880);
   p.px_per_rad = 0.0f;
   video_xr_place(&p, &set);
   CHECK(dims_is(set.slots[VIDEO_XR_MENU_SLOT].dims, 4096, 2304));
   p.px_per_rad = 3000.0f;
   video_xr_place(&p, &set);
   CHECK(dims_is(set.slots[VIDEO_XR_MENU_SLOT].dims, 4096, 2304));
   p.px_per_rad = 1000.0f;
   video_xr_place(&p, &set);
   CHECK(dims_is(set.slots[VIDEO_XR_MENU_SLOT].dims, 1760, 990));

   /* Taller than 3:4, as one eye of a side-by-side window: as tall as
    * 3/4 of the screen width allows, narrower, at the UI's own size. */
   p.ui_dims = VIDEO_SCALE_PACK(800, 960);
   video_xr_place(&p, &set);
   CHECK(pos_is(&set.quads[1], 0.0, 0.0, -1.7));
   CHECK(NEAR(set.quads[1].width, 1.0) && NEAR(set.quads[1].height, 1.2));
   CHECK(set.slots[VIDEO_XR_MENU_SLOT].dims == VIDEO_SCALE_PACK(800, 960));
}

static void test_anchor(void)
{
   video_views_map_t map;
   video_xr_params_t p;
   video_xr_quad_set_t set;
   video_xr_pose_t head, anchor;
   double s45 = sin(M_PI / 4.0), c45 = cos(M_PI / 4.0);
   double s15 = sin(M_PI / 12.0), c15 = cos(M_PI / 12.0);

   /* Turned 90 degrees left and tilted 30 degrees up: the anchor keeps
    * the position and the heading only. */
   head.position.x    = 0.5f;
   head.position.y    = 0.2f;
   head.position.z    = 0.3f;
   head.orientation.x = (float)(c45 * s15);
   head.orientation.y = (float)(s45 * c15);
   head.orientation.z = (float)(-s45 * s15);
   head.orientation.w = (float)(c45 * c15);
   CHECK(video_xr_anchor_from_head(&head, &anchor));
   CHECK(NEAR(anchor.orientation.x, 0.0) && NEAR(anchor.orientation.z, 0.0));
   CHECK(NEAR(anchor.orientation.y, 0.70711) && NEAR(anchor.orientation.w, 0.70711));
   CHECK(NEAR(anchor.position.x, 0.5) && NEAR(anchor.position.y, 0.2)
         && NEAR(anchor.position.z, 0.3));

   /* Placed from that anchor, screen 0 is 1.8 m to the left. */
   map_3ds(&map);
   params_init(&p, &map);
   p.anchor = anchor;
   video_xr_place(&p, &set);
   CHECK(pos_is(&set.quads[0], -1.3, 0.2, 0.3));
   CHECK(pos_is(&set.quads[2], -1.3, -0.792, 0.3));
   CHECK(NEAR(set.quads[0].pose.orientation.y, 0.70711));

   /* Straight up has no heading. */
   head.orientation.x = (float)s45;
   head.orientation.y = 0.0f;
   head.orientation.z = 0.0f;
   head.orientation.w = (float)c45;
   CHECK(!video_xr_anchor_from_head(&head, &anchor));
}

static void test_ray_hit(void)
{
   video_views_map_t map;
   video_xr_params_t p;
   video_xr_quad_set_t set;
   video_xr_vec3_t o, d;
   video_xr_quad_t q;
   float u, v, t;

   map_3ds(&map);
   params_init(&p, &map);
   video_xr_place(&p, &set);
   q = set.quads[0];

   o.x = 0.0f; o.y = 0.0f; o.z = 0.0f;
   d.x = 0.0f; d.y = 0.0f; d.z = -1.0f;
   CHECK(video_xr_ray_hit(&q, &o, &d, &u, &v, &t));
   CHECK(NEAR(u, 0.5) && NEAR(v, 0.5) && NEAR(t, 1.8));

   /* The top-right corner is on the quad. */
   d.x = 0.8f; d.y = 0.48f; d.z = -1.8f;
   CHECK(video_xr_ray_hit(&q, &o, &d, &u, &v, &t));
   CHECK(NEAR(u, 1.0) && NEAR(v, 0.0));

   d.x = -0.4f; d.y = -0.24f; d.z = -1.8f;
   CHECK(video_xr_ray_hit(&q, &o, &d, &u, &v, &t));
   CHECK(NEAR(u, 0.25) && NEAR(v, 0.75));

   /* Beside it, pointing away, and from behind. */
   d.x = 1.0f; d.y = 0.0f; d.z = -1.8f;
   CHECK(!video_xr_ray_hit(&q, &o, &d, &u, &v, &t));
   d.x = 0.0f; d.y = 0.0f; d.z = 1.0f;
   CHECK(!video_xr_ray_hit(&q, &o, &d, &u, &v, &t));
   o.z = -3.0f;
   CHECK(!video_xr_ray_hit(&q, &o, &d, &u, &v, &t));

   /* A quad turned to face +X, 1.8 m to the left. Its right edge is
    * towards -Z. */
   video_xr_pose_identity(&p.anchor);
   p.anchor.orientation.y = 0.70711f;
   p.anchor.orientation.w = 0.70711f;
   video_xr_place(&p, &set);
   q   = set.quads[0];
   o.x = 0.0f; o.y = 0.0f; o.z = 0.0f;
   d.x = -1.0f; d.y = 0.0f; d.z = 0.0f;
   CHECK(video_xr_ray_hit(&q, &o, &d, &u, &v, &t));
   CHECK(NEAR(u, 0.5) && NEAR(v, 0.5) && NEAR(t, 1.8));
   d.x = -1.8f; d.y = 0.0f; d.z = -0.4f;
   CHECK(video_xr_ray_hit(&q, &o, &d, &u, &v, &t));
   CHECK(NEAR(u, 0.75) && NEAR(v, 0.5));
}

static void test_quad_to_frame(void)
{
   video_views_map_t map;
   video_xr_params_t p;
   video_xr_quad_set_t set;
   int16_t x, y;
   unsigned frame = VIDEO_SCALE_PACK(800, 480);

   map_3ds(&map);
   params_init(&p, &map);
   p.ui_dims = VIDEO_SCALE_PACK(1600, 960);
   video_xr_place(&p, &set);

   /* The right eye's copy lands in the left view, at the same point. */
   CHECK(video_xr_quad_to_frame(&set.quads[1], 0.5f, 0.5f, &map, frame, &x, &y));
   CHECK(x == -16364 && y == -16351);
   CHECK(video_xr_quad_to_frame(&set.quads[0], 0.5f, 0.5f, &map, frame, &x, &y));
   CHECK(x == -16364 && y == -16351);
   /* The bottom screen's corners. */
   CHECK(video_xr_quad_to_frame(&set.quads[2], 0.0f, 0.0f, &map, frame, &x, &y));
   CHECK(x == -13083 && y == 67);
   CHECK(video_xr_quad_to_frame(&set.quads[2], 1.0f, 1.0f, &map, frame, &x, &y));
   CHECK(x == 13081 && y == 32767);
   /* The menu is not the core's. */
   CHECK(!video_xr_quad_to_frame(&set.quads[3], 0.5f, 0.5f, &map, frame, &x, &y));

   /* A whole frame spreads over the frame. */
   params_init(&p, NULL);
   p.frame_dims = frame;
   video_xr_place(&p, &set);
   CHECK(video_xr_quad_to_frame(&set.quads[0], 0.25f, 0.75f, NULL, frame, &x, &y));
   CHECK(x == -16364 && y == 16485);
}

int main(void)
{
   test_image_dims();
   test_place_3ds();
   test_place_swap_horizontal();
   test_place_ds_rotation();
   test_place_frame_menu();
   test_place_menu_size();
   test_anchor();
   test_ray_hit();
   test_quad_to_frame();

   if (failures)
   {
      printf("video_xr: FAILED (%d)\n", failures);
      return 1;
   }
   printf("video_xr: ok\n");
   return 0;
}
