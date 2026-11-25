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

#include <string.h>
#include <limits.h>
#include <float.h>
#include <math.h>

#include "video_views.h"
#include "video_defines.h"

void video_views_xr_reset(video_screen_xr_t *screens, unsigned count)
{
   unsigned i;
   if (!screens)
      return;
   if (count > RETRO_VIDEO_VIEWS_MAX)
      count = RETRO_VIDEO_VIEWS_MAX;
   for (i = 0; i < count; i++)
   {
      memset(&screens[i], 0, sizeof(screens[i]));
      screens[i].scale   = 1.0f;
      screens[i].visible = true;
   }
}

static float video_views_xr_value(float value, float low, float high,
      float fallback)
{
   if (!(value >= -FLT_MAX && value <= FLT_MAX))
      return fallback;
   if (value < low)
      return low;
   if (value > high)
      return high;
   return value;
}

void video_views_xr_normalize(video_screen_xr_t *screen)
{
   if (!screen)
      return;
   screen->x = video_views_xr_value(screen->x, -10.0f, 10.0f, 0.0f);
   screen->y = video_views_xr_value(screen->y, -10.0f, 10.0f, 0.0f);
   screen->z = video_views_xr_value(screen->z, -10.0f, 10.0f, 0.0f);
   screen->yaw = video_views_xr_value(screen->yaw, -180.0f, 180.0f, 0.0f);
   screen->pitch = video_views_xr_value(screen->pitch, -180.0f, 180.0f, 0.0f);
   screen->roll = video_views_xr_value(screen->roll, -180.0f, 180.0f, 0.0f);
   screen->scale = video_views_xr_value(screen->scale, 0.1f, 10.0f, 1.0f);
}

/* A core's screen name: cut to fit without splitting a UTF-8
 * sequence, control characters as spaces. */
static void video_views_screen_name(char *s, const char *name)
{
   size_t i;
   size_t len = 0;
   if (name)
   {
      while (len < VIDEO_VIEWS_NAME_SIZE - 1 && name[len])
         len++;
      if (((unsigned char)name[len] & 0xC0) == 0x80)
      {
         while (len && ((unsigned char)name[len - 1] & 0xC0) == 0x80)
            len--;
         if (len && (unsigned char)name[len - 1] >= 0xC0)
            len--;
      }
   }
   for (i = 0; i < len; i++)
   {
      unsigned char c = (unsigned char)name[i];
      s[i]            = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
   }
   s[len] = '\0';
}

/* Screen-indexed arrays across the views code are RETRO_VIDEO_VIEWS_MAX
 * long. */
typedef char video_views_screens_fit[
   (RETRO_VIDEO_SCREENS_MAX <= RETRO_VIDEO_VIEWS_MAX) ? 1 : -1];

/* Written so NaN fails. */
static bool video_views_layer_in_extent(float f)
{
   return f >= -VIDEO_VIEWS_LAYER_EXTENT && f <= VIDEO_VIEWS_LAYER_EXTENT;
}

/* The map's rules, with screen information when @num_scr is not 0. */
static bool video_views_screen_validate(
      const struct retro_video_view *views, unsigned num_views,
      const struct retro_video_screen *scr, unsigned num_scr,
      video_views_map_t *out, video_views_screens_t *screens)
{
   unsigned i;
   unsigned num_screens = num_scr;
   unsigned max_screen  = num_scr ? num_scr : RETRO_VIDEO_VIEWS_MAX;
   unsigned no_input    = 0;
   unsigned viewless    = 0;
   unsigned image       = 0;
   unsigned image_out   = 0;   /* images not taken: no size kept */
   unsigned rules_out   = 0;
   unsigned layer       = 0;
   unsigned upright     = 0;
   unsigned first       = 0;   /* the first screen that is not a LAYER */
   unsigned count_none[RETRO_VIDEO_VIEWS_MAX];
   unsigned count_left[RETRO_VIDEO_VIEWS_MAX];
   unsigned count_right[RETRO_VIDEO_VIEWS_MAX];
   int left[RETRO_VIDEO_VIEWS_MAX];
   int right[RETRO_VIDEO_VIEWS_MAX];
   int partner[RETRO_VIDEO_VIEWS_MAX];

   if (num_views == 0)
   {
      if (out)
         memset(out, 0, sizeof(*out));
      if (screens)
         memset(screens, 0, sizeof(*screens));
      return true;
   }
   if (!views || num_views > RETRO_VIDEO_VIEWS_MAX)
      return false;
   if (num_scr > RETRO_VIDEO_SCREENS_MAX || (num_scr && !scr))
      return false;

   for (i = 0; i < RETRO_VIDEO_VIEWS_MAX; i++)
   {
      count_none [i] = 0;
      count_left [i] = 0;
      count_right[i] = 0;
      partner    [i] = -1;
   }

   for (i = 0; i < num_views; i++)
   {
      const struct retro_video_view *v = &views[i];
      if (!v->width || !v->height)
         return false;
      if (v->x > UINT_MAX - v->width || v->y > UINT_MAX - v->height)
         return false;
      if (v->screen >= max_screen)
         return false;
      /* Rejects infinities too; NaN falls through to width / height. */
      if (     v->aspect_ratio > 0.0f
            && (v->aspect_ratio < 0.01f || v->aspect_ratio > 100.0f))
         return false;
      switch (v->eye)
      {
         case RETRO_VIDEO_VIEW_EYE_NONE:
            count_none[v->screen]++;
            break;
         case RETRO_VIDEO_VIEW_EYE_LEFT:
            count_left[v->screen]++;
            left[v->screen]  = (int)i;
            break;
         case RETRO_VIDEO_VIEW_EYE_RIGHT:
            count_right[v->screen]++;
            right[v->screen] = (int)i;
            break;
         default:
            return false;
      }
      if (!num_scr && v->screen + 1 > num_screens)
         num_screens = v->screen + 1;
   }

   while (     first < num_scr
            && scr[first].show == RETRO_VIDEO_SCREEN_SHOW_LAYER)
      first++;

   for (i = 0; i < num_scr; i++)
   {
      const struct retro_video_screen *s = &scr[i];
      bool is_layer = s->show == RETRO_VIDEO_SCREEN_SHOW_LAYER;
      if (s->image)
      {
         const struct retro_video_screen_image *im = s->image;
         size_t px = 0;
         switch (im->format)
         {
            case RETRO_PIXEL_FORMAT_XRGB8888:
            case RETRO_VIDEO_SCREEN_FORMAT_ARGB8888:
               px = 4;
               break;
            case RETRO_PIXEL_FORMAT_RGB565:
            case RETRO_PIXEL_FORMAT_0RGB1555:
               px = 2;
               break;
            default:
               break;
         }
         if (     !im->width || !im->height
               || (px && im->pitch / px < im->width))
            return false;
         /* As a view's: NaN falls through to width / height. */
         if (     im->aspect_ratio > 0.0f
               && (im->aspect_ratio < 0.01f || im->aspect_ratio > 100.0f))
            return false;
         /* ARGB8888 on a LAYER is the frontend's to judge. */
         if (     (   im->format != RETRO_PIXEL_FORMAT_XRGB8888
                   && im->format != RETRO_PIXEL_FORMAT_RGB565
                   && !(   is_layer
                        && im->format == RETRO_VIDEO_SCREEN_FORMAT_ARGB8888))
               || im->width  > VIDEO_VIEWS_IMAGE_MAX
               || im->height > VIDEO_VIEWS_IMAGE_MAX)
            image_out |= 1u << i;
         image    |= 1u << i;
         no_input |= 1u << i;
      }
      if (     s->show   > RETRO_VIDEO_SCREEN_SHOW_LAYER
            || s->state  > RETRO_VIDEO_SCREEN_STATE_ABSENT
            || s->base  >= num_scr)
         return false;
      if (!(s->opacity >= 0.0f && s->opacity <= 1.0f))
         return false;
      if ((s->flags & RETRO_VIDEO_SCREEN_FLAG_PRIVATE) && !s->owners)
         return false;
      if (is_layer)
      {
         /* The root is SHOWN or HIDDEN, so not the layer itself. */
         if (     scr[s->base].show > RETRO_VIDEO_SCREEN_SHOW_HIDDEN
               || s->swap_with || s->group)
            return false;
         /* Both sides 0 or both above 0; an image gives a side left 0. */
         if (     !video_views_layer_in_extent(s->x)
               || !video_views_layer_in_extent(s->y)
               || !video_views_layer_in_extent(s->width)
               || !video_views_layer_in_extent(s->height)
               || s->width < 0.0f || s->height < 0.0f
               || (   !s->image
                   && (s->width > 0.0f) != (s->height > 0.0f)))
            return false;
         layer |= 1u << i;
         if (s->blend > RETRO_VIDEO_SCREEN_BLEND_NONE)
            rules_out |= 1u << i;
      }
      else
      {
         if (s->anchor > RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT)
            return false;
         /* Written so NaN fails. */
         if (!(s->size >= 0.0f) || s->size > FLT_MAX)
            return false;
         /* A base that is not OVER is not the screen itself. */
         if (     s->show == RETRO_VIDEO_SCREEN_SHOW_OVER
               && (   scr[s->base].show == RETRO_VIDEO_SCREEN_SHOW_OVER
                   || s->size > 1.0f))
            return false;
         /* order is 0 on every screen that is not a LAYER, or on none. */
         if (!s->order != !scr[first].order)
            return false;
      }
      if (s->swap_with)
      {
         unsigned t = s->swap_with - 1;
         if (s->swap_with > num_scr || t == i)
            return false;
         /* One side makes a pair, and a LAYER is in none. */
         if (scr[t].show == RETRO_VIDEO_SCREEN_SHOW_LAYER)
            return false;
         if (     (partner[i] >= 0 && partner[i] != (int)t)
               || (partner[t] >= 0 && partner[t] != (int)i))
            return false;
         partner[i] = (int)t;
         partner[t] = (int)i;
      }
      if (s->flags & RETRO_VIDEO_SCREEN_FLAG_NO_INPUT)
         no_input |= 1u << i;
      if (     (s->flags & RETRO_VIDEO_SCREEN_FLAG_UPRIGHT)
            && (is_layer || !s->image))
         upright |= 1u << i;
   }

   for (i = 0; i < num_screens; i++)
   {
      if (image & (1u << i))
      {
         if (count_none[i] || count_left[i] || count_right[i])
            return false;
         continue;
      }
      if (count_none[i] == 1 && !count_left[i] && !count_right[i])
         continue;
      if (     !count_none[i] && count_left[i] == 1 && count_right[i] == 1
            && views[left[i]].width  == views[right[i]].width
            && views[left[i]].height == views[right[i]].height)
         continue;
      if (     !count_none[i] && !count_left[i] && !count_right[i]
            && num_scr && scr[i].state == RETRO_VIDEO_SCREEN_STATE_ABSENT)
      {
         viewless |= 1u << i;
         continue;
      }
      return false;
   }

   if (out)
   {
      memset(out, 0, sizeof(*out));
      memcpy(out->views, views, num_views * sizeof(*views));
      out->num_views   = num_views;
      out->num_screens = num_screens;
      out->no_input    = no_input;
      out->viewless    = viewless;
      out->image       = image;
      out->rules_out   = rules_out | image_out;
      out->layer       = layer;
      out->upright     = upright;
      for (i = 0; i < num_scr; i++)
      {
         const struct retro_video_screen_image *im = scr[i].image;
         if (!im || (image_out & (1u << i)))
            continue;
         out->image_dims[i]   = VIDEO_SCALE_PACK(im->width, im->height);
         out->image_aspect[i] = im->aspect_ratio > 0.0f
               ? im->aspect_ratio
               : (float)im->width / im->height;
      }
   }
   if (screens)
   {
      memset(screens, 0, sizeof(*screens));
      for (i = 0; i < num_scr; i++)
      {
         const struct retro_video_screen *s = &scr[i];
         video_views_screen_t *d            = &screens->screens[i];
         d->flags     = s->flags;
         d->state     = s->state;
         d->show      = s->show;
         d->base      = s->base;
         d->owners    = s->owners;
         d->swap_with = s->swap_with;
         d->group     = s->group;
         d->opacity   = s->opacity;
         /* What a screen's show does not read is kept 0. */
         if (s->show == RETRO_VIDEO_SCREEN_SHOW_LAYER)
         {
            d->blend  = s->blend;
            d->z      = s->z;
            d->x      = s->x;
            d->y      = s->y;
            d->width  = s->width;
            d->height = s->height;
         }
         else
         {
            d->anchor = s->anchor;
            d->order  = s->order;
            d->size   = s->size;
         }
         video_views_screen_name(d->name, s->name);
      }
      screens->count = num_scr;
   }
   return true;
}

bool video_views_validate(const struct retro_video_view *views,
      unsigned num_views, video_views_map_t *out)
{
   return video_views_screen_validate(views, num_views, NULL, 0,
         out, NULL);
}

bool video_views_validate_map(const struct retro_video_views *in,
      video_views_map_t *out, video_views_screens_t *screens)
{
   /* Without the flag, the struct may end at recommended_view_height. */
   if (!(in->flags & RETRO_VIDEO_VIEWS_FLAG_SCREENS))
      return video_views_screen_validate(in->views, in->num_views,
            NULL, 0, out, screens);
   return video_views_screen_validate(in->views, in->num_views,
         in->screens, in->num_screens, out, screens);
}

void video_views_names_publish(video_views_names_box_t *box,
      const video_views_names_t *names)
{
   retro_seqlatch_publish(&box->seq, box->words, names, sizeof(*names));
}

int video_views_names_read(video_views_names_box_t *box,
      video_views_names_t *out)
{
   return retro_seqlatch_read(&box->seq, box->words, out, sizeof(*out));
}

bool video_views_fit_frame(const video_views_map_t *map,
      unsigned frame_dims)
{
   unsigned i;
   unsigned frame_width  = VIDEO_SCALE_W(frame_dims);
   unsigned frame_height = VIDEO_SCALE_H(frame_dims);
   if (!map || !map->num_views)
      return false;
   for (i = 0; i < map->num_views; i++)
   {
      const struct retro_video_view *v = &map->views[i];
      if (     v->x + v->width  > frame_width
            || v->y + v->height > frame_height)
         return false;
   }
   return true;
}

void video_views_scale(video_views_map_t *map,
      unsigned in_dims, unsigned out_dims)
{
   unsigned i;
   unsigned in_width   = VIDEO_SCALE_W(in_dims);
   unsigned in_height  = VIDEO_SCALE_H(in_dims);
   unsigned out_width  = VIDEO_SCALE_W(out_dims);
   unsigned out_height = VIDEO_SCALE_H(out_dims);
   if (!map || !in_width || !in_height || in_dims == out_dims)
      return;
   for (i = 0; i < map->num_views; i++)
   {
      struct retro_video_view *v = &map->views[i];
      v->aspect_ratio = (float)video_views_aspect(v);
      v->x      = (unsigned)(((double)v->x      * out_width)  / in_width);
      v->width  = (unsigned)(((double)v->width  * out_width)  / in_width);
      v->y      = (unsigned)(((double)v->y      * out_height) / in_height);
      v->height = (unsigned)(((double)v->height * out_height) / in_height);
      if (!v->width)
         v->width  = 1;
      if (!v->height)
         v->height = 1;
   }
}

double video_views_aspect(const struct retro_video_view *v)
{
   /* NaN falls through to width / height, as validate expects. */
   if (v->aspect_ratio > 0.0f)
      return v->aspect_ratio;
   return (double)v->width / v->height;
}

int video_views_find(const video_views_map_t *map,
      unsigned screen, unsigned eye)
{
   unsigned i;
   int left = -1;
   for (i = 0; i < map->num_views; i++)
   {
      const struct retro_video_view *v = &map->views[i];
      if (v->screen != screen)
         continue;
      if (v->eye == RETRO_VIDEO_VIEW_EYE_NONE || v->eye == eye)
         return (int)i;
      if (v->eye == RETRO_VIDEO_VIEW_EYE_LEFT)
         left = (int)i;
   }
   return (eye == RETRO_VIDEO_VIEW_EYE_NONE) ? left : -1;
}

bool video_views_has_place(const video_views_map_t *map, unsigned screen)
{
   return (map->image & (1u << screen))
      || video_views_find(map, screen, RETRO_VIDEO_VIEW_EYE_NONE) >= 0;
}

/* @screen's chain of sized bases, from @screen, up to the screen that
 * sets its height: the chain's end, or the lowest-numbered screen
 * of a cycle in it. Returns that screen's place in @chain. Each base is
 * read once: a redraw may walk a map the main thread is arranging. */
static unsigned video_views_extent_chain(const video_views_map_t *map,
      unsigned screen, unsigned *chain)
{
   unsigned n    = 0;
   unsigned seen = 0;
   unsigned s    = screen;
   for (;;)
   {
      unsigned b, i, low;
      seen     |= 1u << s;
      chain[n]  = s;
      if ((map->over & (1u << s)) || !(map->size[s] > 0.0f))
         return n;
      b = VIDEO_VIEWS_NIBBLE(map->base, s);
      if (b >= RETRO_VIDEO_VIEWS_MAX || !video_views_has_place(map, b))
         return n;
      if (!(seen & (1u << b)))
      {
         s = b;
         n++;
         continue;
      }
      for (i = 0; chain[i] != b; i++) { }
      for (low = i; i <= n; i++)
         if (chain[i] < chain[low])
            low = i;
      return low;
   }
}

bool video_views_extent(const video_views_map_t *map, unsigned screen,
      unsigned rotation, double *w, double *h)
{
   unsigned k, root, r;
   unsigned chain[RETRO_VIDEO_VIEWS_MAX];
   double aspect;
   if (!video_views_has_place(map, screen))
      return false;
   root = video_views_extent_chain(map, screen, chain);
   r    = chain[root];
   if (map->image & (1u << r))
      *h = VIDEO_SCALE_H(map->image_dims[r]);
   else
   {
      const struct retro_video_view *v = &map->views[video_views_find(map,
            r, RETRO_VIDEO_VIEW_EYE_NONE)];
      *h = (rotation & 1) ? v->width : v->height;
   }
   for (k = 0; k < root; k++)
      *h *= map->size[chain[k]];
   if (map->image & (1u << screen))
      aspect = map->image_aspect[screen];
   else
   {
      aspect = video_views_aspect(&map->views[video_views_find(map, screen,
            RETRO_VIDEO_VIEW_EYE_NONE)]);
      if (rotation & 1)
         aspect = 1.0 / aspect;
   }
   *w = *h * aspect;
   return true;
}

void video_views_inset(const video_views_frect_t *base, unsigned anchor,
      double size, double aspect, video_views_frect_t *out)
{
   unsigned cell;
   double w, h;
   if (!(size <= 1.0))   /* NaN too */
      size = 1.0;
   h = base->h * size;
   w = h * aspect;
   if (w > base->w)
   {
      w = base->w;
      h = w / aspect;
   }
   if (     anchor < RETRO_VIDEO_SCREEN_ANCHOR_TOP_LEFT
         || anchor > RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT)
      anchor = RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT;
   /* The anchors run through a 3x3 grid's cells, skipping its centre. */
   cell   = (anchor < RETRO_VIDEO_SCREEN_ANCHOR_RIGHT) ? anchor - 1 : anchor;
   out->x = base->x + (base->w - w) * (cell % 3) / 2.0;
   out->y = base->y + (base->h - h) * (cell / 3) / 2.0;
   out->w = w;
   out->h = h;
}

bool video_views_eye_pair(const video_views_map_t *map, int eyes[2])
{
   unsigned i;
   eyes[0] = -1;
   eyes[1] = -1;
   for (i = 0; i < map->num_views; i++)
   {
      int e;
      if (map->views[i].eye == RETRO_VIDEO_VIEW_EYE_LEFT)
         e = 0;
      else if (map->views[i].eye == RETRO_VIDEO_VIEW_EYE_RIGHT)
         e = 1;
      else
         continue;
      if (eyes[e] >= 0)
         break;
      eyes[e] = (int)i;
   }
   if (     i < map->num_views || eyes[0] < 0 || eyes[1] < 0
         || (map->left_out & (  (1u << map->views[eyes[0]].screen)
                              | (1u << map->views[eyes[1]].screen))))
   {
      eyes[0] = -1;
      eyes[1] = -1;
      return false;
   }
   return true;
}

bool video_views_select_screens(const video_views_map_t *map,
      unsigned roots, video_views_map_t *out, unsigned *index)
{
   unsigned i, s, sel, n = 0;
   unsigned num[RETRO_VIDEO_VIEWS_MAX];
   memset(out, 0, sizeof(*out));
   if (!map)
      return false;
   /* An inset only with its base */
   sel = video_views_with_insets(map, roots & ~map->over)
         & ((1u << map->num_screens) - 1);
   for (s = 0; s < map->num_screens; s++)
      if (sel & (1u << s))
         num[s] = n++;
   for (i = 0; i < map->num_views; i++)
   {
      const struct retro_video_view *v = &map->views[i];
      if (!(sel & (1u << v->screen)))
         continue;
      index[out->num_views]               = i;
      out->views[out->num_views]          = *v;
      out->views[out->num_views++].screen = num[v->screen];
   }
   if (!out->num_views && !(sel & map->image))
      return false;
   /* Selected screens with no view count too; viewless marks them. */
   out->num_screens = n;
   for (s = 0; s < map->num_screens; s++)
   {
      unsigned j, b;
      if (!(sel & (1u << s)))
         continue;
      j              = num[s];
      b              = VIDEO_VIEWS_NIBBLE(map->base, s);
      out->no_input |= ((map->no_input >> s) & 1u) << j;
      out->viewless |= ((map->viewless >> s) & 1u) << j;
      out->left_out |= ((map->left_out >> s) & 1u) << j;
      out->blank    |= ((map->blank    >> s) & 1u) << j;
      out->over     |= ((map->over     >> s) & 1u) << j;
      out->image    |= ((map->image    >> s) & 1u) << j;
      out->nearest  |= ((map->nearest  >> s) & 1u) << j;
      /* A sized root's base on another display: it sizes itself. */
      out->base     |= (uint32_t)((sel & (1u << b)) ? num[b] : j) << (j * 4);
      out->anchor   |= (uint32_t)VIDEO_VIEWS_NIBBLE(map->anchor, s)
            << (j * 4);
      out->opacity[j]      = map->opacity[s];
      out->size[j]         = map->size[s];
      out->image_dims[j]   = map->image_dims[s];
      out->image_aspect[j] = map->image_aspect[s];
      out->image_tex[j]    = map->image_tex[s];
      out->image_gen[j]    = map->image_gen[s];
   }
   return true;
}

bool video_views_snapshot(const video_views_map_t *map,
      unsigned frame_dims, unsigned out_dims,
      video_views_map_t *out)
{
   if (!video_views_fit_frame(map, frame_dims))
   {
      memset(out, 0, sizeof(*out));
      return false;
   }
   *out = *map;
   video_views_scale(out, frame_dims, out_dims);
   return true;
}

static int video_views_round(double v)
{
   return (int)floor(v + 0.5);
}

/* Rectangles of the screens not in @skip within an area of size cdims,
 * in screen order. */
static void video_views_fit(const video_views_layout_params_t *p,
      unsigned cdims, unsigned count, unsigned first, unsigned skip,
      video_views_rect_t *fit)
{
   unsigned s;
   double disp_w[RETRO_VIDEO_VIEWS_MAX];
   double disp_h[RETRO_VIDEO_VIEWS_MAX];
   double tw = 0.0, th = 0.0, scale, ox, oy, pos = 0.0;
   const video_views_map_t *map = p->map;
   unsigned cw   = VIDEO_SCALE_W(cdims);
   unsigned ch   = VIDEO_SCALE_H(cdims);
   bool vertical = (p->screen_layout != VIDEO_SCREEN_LAYOUT_HORIZONTAL);
   /* The one-screen settings; an image keeps its aspect */
   bool single   = count == 1 && !(map->image & (1u << first));

   if (single && p->stretch && !p->scale_integer)
   {
      fit[first].pos  = VIDEO_POS_PACK(0, 0);
      fit[first].dims = cdims;
      return;
   }

   if (single && p->custom_vp && cdims == p->dims)
   {
      fit[first] = *p->custom_vp;
      return;
   }

   for (s = 0; s < map->num_screens; s++)
   {
      if (skip & (1u << s))
         continue;
      video_views_extent(map, s, p->rotation, &disp_w[s], &disp_h[s]);
      if (single && p->single_aspect > 0.0f)
         disp_w[s] = disp_h[s] * ((p->rotation & 1)
               ? 1.0 / p->single_aspect : p->single_aspect);
      if (vertical)
      {
         if (disp_w[s] > tw)
            tw = disp_w[s];
         th += disp_h[s];
      }
      else
      {
         tw += disp_w[s];
         if (disp_h[s] > th)
            th = disp_h[s];
      }
   }

   scale = (double)cw / tw;
   if ((double)ch / th < scale)
      scale = (double)ch / th;
   if (p->scale_integer && scale >= 1.0)
      scale = floor(scale);
   ox = ((double)cw - tw * scale) / 2.0;
   oy = ((double)ch - th * scale) / 2.0;

   for (s = 0; s < map->num_screens; s++)
   {
      double x, y;
      int x0, y0;
      double w, h;
      if (skip & (1u << s))
         continue;
      w = disp_w[s] * scale;
      h = disp_h[s] * scale;
      if (vertical)
      {
         x    = ox + (tw - disp_w[s]) * scale / 2.0;
         y    = oy + pos;
         pos += h;
      }
      else
      {
         x    = ox + pos;
         y    = oy + (th - disp_h[s]) * scale / 2.0;
         pos += w;
      }
      x0          = video_views_round(x);
      y0          = video_views_round(y);
      fit[s].pos  = VIDEO_POS_PACK(x0, y0);
      fit[s].dims = VIDEO_SCALE_PACK(video_views_round(x + w) - x0,
            video_views_round(y + h) - y0);
   }
}

/* Inset @s's rectangle over @base's, both in fit's units. */
static void video_views_fit_inset(const video_views_layout_params_t *p,
      unsigned s, unsigned base, video_views_rect_t *fit)
{
   int x0, y0, x1, y1;
   video_views_frect_t b, r;
   double w = 1.0, h = 1.0;
   const video_views_map_t *map = p->map;
   const video_views_rect_t *br = &fit[base];
   b.x = VIDEO_POS_X(br->pos);
   b.y = VIDEO_POS_Y(br->pos);
   b.w = VIDEO_SCALE_W(br->dims);
   b.h = VIDEO_SCALE_H(br->dims);
   video_views_extent(map, s, p->rotation, &w, &h);
   video_views_inset(&b, VIDEO_VIEWS_NIBBLE(map->anchor, s), map->size[s],
         w / h, &r);
   x0 = video_views_round(r.x);
   y0 = video_views_round(r.y);
   x1 = video_views_round(r.x + r.w);
   y1 = video_views_round(r.y + r.h);
   if (x1 <= x0)
      x1 = x0 + 1;
   if (y1 <= y0)
      y1 = y0 + 1;
   fit[s].pos  = VIDEO_POS_PACK(x0, y0);
   fit[s].dims = VIDEO_SCALE_PACK(x1 - x0, y1 - y0);
}

/* Map r from an area of size cdims into area, scaling as needed. */
static void video_views_place(const video_views_rect_t *r,
      unsigned cdims, const video_views_rect_t *area,
      video_views_rect_t *out)
{
   int rx    = VIDEO_POS_X(r->pos);
   int ry    = VIDEO_POS_Y(r->pos);
   double sx = (double)VIDEO_SCALE_W(area->dims) / VIDEO_SCALE_W(cdims);
   double sy = (double)VIDEO_SCALE_H(area->dims) / VIDEO_SCALE_H(cdims);
   int x0    = video_views_round(rx * sx);
   int y0    = video_views_round(ry * sy);
   int x1    = video_views_round((rx + (int)VIDEO_SCALE_W(r->dims)) * sx);
   int y1    = video_views_round((ry + (int)VIDEO_SCALE_H(r->dims)) * sy);
   out->pos  = VIDEO_POS_PACK(VIDEO_POS_X(area->pos) + x0,
         VIDEO_POS_Y(area->pos) + y0);
   out->dims = VIDEO_SCALE_PACK(x1 - x0, y1 - y0);
}

unsigned video_views_roots(const video_views_map_t *map)
{
   return ((1u << map->num_screens) - 1)
         & ~(map->viewless | map->left_out | map->over);
}

unsigned video_views_with_insets(const video_views_map_t *map,
      unsigned roots)
{
   unsigned s;
   unsigned out    = roots;
   unsigned insets = map->over & ~(map->viewless | map->left_out);
   for (s = 0; s < map->num_screens; s++)
      if (     (insets & (1u << s))
            && (roots & (1u << VIDEO_VIEWS_NIBBLE(map->base, s))))
         out |= 1u << s;
   return out;
}

void video_views_layout(const video_views_layout_params_t *p,
      video_views_layout_t *out)
{
   unsigned a, i, s, n = 0, count = 0, first = 0;
   unsigned w, h, cdims, roots, insets;
   unsigned order[RETRO_VIDEO_VIEWS_MAX];
   video_views_rect_t fit[RETRO_VIDEO_VIEWS_MAX];
   const video_views_map_t *map = p->map;

   memset(out, 0, sizeof(*out));
   w = VIDEO_SCALE_W(p->dims);
   h = VIDEO_SCALE_H(p->dims);
   if (!map || !(map->num_views || map->image) || !w || !h)
      return;

   roots = video_views_roots(map) & ~p->exclude_screens;
   for (s = 0; s < map->num_screens; s++)
      if (roots & (1u << s))
      {
         first = s;
         count++;
      }
   if (!count)
      return;

   out->stereo_mode   = p->stereo_mode;
   cdims              = p->dims;
   out->num_areas     = 2;
   out->canvas_dims   = p->dims;
   out->ui_dims       = p->dims;

   switch (p->stereo_mode)
   {
      case VIDEO_STEREO_MODE_SBS_HALF:
      case VIDEO_STEREO_MODE_SBS_FULL:
         out->areas[0].dims = VIDEO_SCALE_PACK(w / 2, h);
         out->areas[1].pos  = VIDEO_POS_PACK(w / 2, 0);
         out->areas[1].dims = VIDEO_SCALE_PACK(w - w / 2, h);
         out->ui_per_eye    = true;
         if (p->stereo_mode == VIDEO_STEREO_MODE_SBS_FULL)
         {
            cdims        = out->areas[0].dims;
            out->ui_dims = cdims;
         }
         break;
      case VIDEO_STEREO_MODE_TOP_BOTTOM:
         out->areas[0].dims = VIDEO_SCALE_PACK(w, h / 2);
         out->areas[1].pos  = VIDEO_POS_PACK(0, h / 2);
         out->areas[1].dims = VIDEO_SCALE_PACK(w, h - h / 2);
         out->ui_per_eye    = true;
         break;
      case VIDEO_STEREO_MODE_ANAGLYPH:
      case VIDEO_STEREO_MODE_INTERLACED:
         out->areas[0].dims = p->dims;
         out->areas[1].pos  = VIDEO_POS_PACK(w, 0);
         out->areas[1].dims = p->dims;
         out->canvas_dims   = VIDEO_SCALE_PACK(w * 2, h);
         out->offscreen     = true;
         break;
      default:
         out->num_areas     = 1;
         out->areas[0].dims = p->dims;
         break;
   }

   video_views_fit(p, cdims, count, first, ~roots, fit);

   /* Each root, then the insets over it in screen order. */
   insets = video_views_with_insets(map, roots) & ~roots;
   for (s = 0; s < map->num_screens; s++)
   {
      if (!(roots & (1u << s)))
         continue;
      order[n++] = s;
      for (i = 0; i < map->num_screens; i++)
         if (     (insets & (1u << i))
               && VIDEO_VIEWS_NIBBLE(map->base, i) == s)
         {
            video_views_fit_inset(p, i, s, fit);
            order[n++] = i;
         }
   }

   for (a = 0; a < out->num_areas; a++)
   {
      unsigned eye = RETRO_VIDEO_VIEW_EYE_LEFT;
      if (out->num_areas == 2 && ((a == 1) != p->swap_eyes))
         eye = RETRO_VIDEO_VIEW_EYE_RIGHT;
      for (i = 0; i < n; i++)
      {
         video_views_placement_t *pl =
               &out->placements[out->num_placements++];
         s        = order[i];
         pl->area = a;
         if (map->image & (1u << s))
         {
            pl->view  = s;
            pl->flags = VIDEO_VIEWS_PLACEMENT_IMAGE;
         }
         else
            pl->view  = (unsigned)video_views_find(map, s, eye);
         if (map->blank & (1u << s))
            pl->flags |= VIDEO_VIEWS_PLACEMENT_BLANK;
         if (insets & (1u << s))
            pl->flags |= VIDEO_VIEWS_PLACEMENT_INSET
                  | VIDEO_VIEWS_PLACEMENT_ALPHA_BITS(map->opacity[s]);
         video_views_place(&fit[s], cdims, &out->areas[a], &pl->dst);
      }
   }
}

unsigned video_views_placement_screen(const video_views_map_t *map,
      const video_views_placement_t *p)
{
   if (p->flags & VIDEO_VIEWS_PLACEMENT_IMAGE)
      return p->view;
   return map->views[p->view].screen;
}

bool video_views_layout_same(const video_views_layout_t *a,
      const video_views_layout_t *b)
{
   return a->num_placements == b->num_placements
      && a->num_areas       == b->num_areas
      && a->canvas_dims     == b->canvas_dims
      && a->ui_dims         == b->ui_dims
      && a->stereo_mode     == b->stereo_mode
      && a->offscreen       == b->offscreen
      && a->ui_per_eye      == b->ui_per_eye
      && !memcmp(a->placements, b->placements,
            a->num_placements * sizeof(a->placements[0]))
      && !memcmp(a->areas, b->areas, a->num_areas * sizeof(a->areas[0]));
}

void video_views_touch_publish(video_views_touch_t *t,
      const video_views_map_t *map, const video_views_layout_t *layout,
      const unsigned *index, unsigned dims, unsigned frame_dims)
{
   unsigned i;
   retro_atomic_fetch_add_seq_cst_int(&t->seq, 1);
   t->count    = 0;
   t->no_input = 0;
   for (i = 0; layout && i < layout->num_placements
         && t->count < RETRO_VIDEO_VIEWS_MAX; i++)
   {
      const video_views_placement_t *p = &layout->placements[i];
      unsigned n                       = t->count;
      if (p->flags & VIDEO_VIEWS_PLACEMENT_IMAGE)
         continue;
      t->dst[n] = p->dst;
      t->src[n] = map->views[index[p->view]];
      if (map->no_input & (1u << t->src[n].screen))
         t->no_input |= 1u << n;
      t->count++;
   }
   t->dims       = dims;
   t->frame_dims = frame_dims;
   retro_atomic_fetch_add_seq_cst_int(&t->seq, 1);
}

bool video_views_touch_point(video_views_touch_t *t, int x, int y,
      int16_t *res_x, int16_t *res_y)
{
   unsigned i, tries;
   for (tries = 0; tries < 4; tries++)
   {
      long px, py;
      video_views_rect_t dst[RETRO_VIDEO_VIEWS_MAX];
      struct retro_video_view src[RETRO_VIDEO_VIEWS_MAX];
      int seq           = retro_atomic_load_acquire_int(&t->seq);
      unsigned count    = t->count;
      unsigned no_input = t->no_input;
      unsigned dims     = t->dims;
      unsigned fdims    = t->frame_dims;
      if (seq & 1)
         continue;
      memcpy(dst, t->dst, sizeof(dst));
      memcpy(src, t->src, sizeof(src));
      /* A full barrier: the copy is read before the count is again */
      if (retro_atomic_fetch_add_seq_cst_int(&t->seq, 0) != seq)
         continue;
      if (     !count || count > RETRO_VIDEO_VIEWS_MAX
            || !VIDEO_SCALE_W(fdims) || !VIDEO_SCALE_H(fdims))
         return false;
      /* The touch in the display's pixels */
      px     = ((long)x + 0x7fff) * (long)VIDEO_SCALE_W(dims) / 0xfffe;
      py     = ((long)y + 0x7fff) * (long)VIDEO_SCALE_H(dims) / 0xfffe;
      *res_x = -0x8000;
      *res_y = -0x8000;
      /* Topmost first */
      for (i = count; i-- > 0; )
      {
         long fx, fy;
         long dx = VIDEO_POS_X(dst[i].pos);
         long dy = VIDEO_POS_Y(dst[i].pos);
         long dw = (long)VIDEO_SCALE_W(dst[i].dims);
         long dh = (long)VIDEO_SCALE_H(dst[i].dims);
         if (no_input & (1u << i))
            continue;
         if (px < dx || py < dy || px >= dx + dw || py >= dy + dh)
            continue;
         /* That point of the view, in the packed frame */
         fx     = (long)src[i].x + (px - dx) * (long)src[i].width / dw;
         fy     = (long)src[i].y + (py - dy) * (long)src[i].height / dh;
         *res_x = (int16_t)(fx * 0xfffe / (long)VIDEO_SCALE_W(fdims)
               - 0x7fff);
         *res_y = (int16_t)(fy * 0xfffe / (long)VIDEO_SCALE_H(fdims)
               - 0x7fff);
         break;
      }
      return true;
   }
   return false;
}

void video_views_blend_xrgb8888(const uint32_t *canvas,
      size_t canvas_stride, uint32_t *out, size_t out_stride,
      unsigned dims, unsigned stereo_mode)
{
   unsigned x, y;
   unsigned w = VIDEO_SCALE_W(dims);
   unsigned h = VIDEO_SCALE_H(dims);
   for (y = 0; y < h; y++, canvas += canvas_stride, out += out_stride)
   {
      const uint32_t *l = canvas;
      const uint32_t *r = canvas + w;
      if (stereo_mode == VIDEO_STEREO_MODE_INTERLACED)
      {
         memcpy(out, (y & 1) ? r : l, (size_t)w * sizeof(uint32_t));
         continue;
      }
      for (x = 0; x < w; x++)
      {
         unsigned k;
         float c[3];
         uint32_t px = 0;
         float lr    = (float)((l[x] >> 16) & 0xff);
         float lg    = (float)((l[x] >>  8) & 0xff);
         float lb    = (float)( l[x]        & 0xff);
         float rr    = (float)((r[x] >> 16) & 0xff);
         float rg    = (float)((r[x] >>  8) & 0xff);
         float rb    = (float)( r[x]        & 0xff);
         c[0] =  0.456100f * lr + 0.500484f * lg + 0.176381f * lb
               - 0.0434706f * rr - 0.0879388f * rg - 0.00155529f * rb;
         c[1] = -0.0400822f * lr - 0.0378246f * lg - 0.0157589f * lb
               + 0.378476f * rr + 0.73364f * rg - 0.0184503f * rb;
         c[2] = -0.0152161f * lr - 0.0205971f * lg - 0.00546856f * lb
               - 0.0721527f * rr - 0.112961f * rg + 1.2264f * rb;
         for (k = 0; k < 3; k++)
         {
            int v = (int)(c[k] + 0.5f);
            v     = (v < 0) ? 0 : (v > 255) ? 255 : v;
            px    = (px << 8) | (uint32_t)v;
         }
         out[x] = px;
      }
   }
}

unsigned video_views_ui_dims(const video_views_layout_t *layout,
      unsigned dims)
{
   if (layout && layout->ui_per_eye)
      return layout->ui_dims;
   return dims;
}

/* The part of r inside the w x h rectangle at (x, y). */
static bool video_views_cut(const video_views_rect_t *r,
      int x, int y, int w, int h, video_views_rect_t *out)
{
   int x0 = VIDEO_POS_X(r->pos);
   int y0 = VIDEO_POS_Y(r->pos);
   int x1 = x0 + (int)VIDEO_SCALE_W(r->dims);
   int y1 = y0 + (int)VIDEO_SCALE_H(r->dims);
   if (x0 < x)
      x0 = x;
   if (y0 < y)
      y0 = y;
   if (x1 > x + w)
      x1 = x + w;
   if (y1 > y + h)
      y1 = y + h;
   if (x1 <= x0 || y1 <= y0)
      return false;
   out->pos  = VIDEO_POS_PACK(x0, y0);
   out->dims = VIDEO_SCALE_PACK(x1 - x0, y1 - y0);
   return true;
}

bool video_views_clip(const video_views_rect_t *r,
      unsigned dims, video_views_rect_t *out)
{
   return video_views_cut(r, 0, 0, (int)VIDEO_SCALE_W(dims),
         (int)VIDEO_SCALE_H(dims), out);
}

bool video_views_visible(const video_views_layout_t *layout,
      const video_views_placement_t *p, unsigned dims,
      video_views_rect_t *out)
{
   video_views_rect_t r;
   const video_views_rect_t *a = &layout->areas[p->area];
   return video_views_cut(&p->dst, VIDEO_POS_X(a->pos), VIDEO_POS_Y(a->pos),
            (int)VIDEO_SCALE_W(a->dims), (int)VIDEO_SCALE_H(a->dims), &r)
      && video_views_clip(&r, dims, out);
}

void video_views_cut_source(const struct retro_video_view *v,
      const video_views_rect_t *dst, const video_views_rect_t *cut,
      video_views_rect_t *out)
{
   unsigned long dw = VIDEO_SCALE_W(dst->dims);
   unsigned long dh = VIDEO_SCALE_H(dst->dims);
   long dx          = (long)VIDEO_POS_X(cut->pos) - VIDEO_POS_X(dst->pos);
   long dy          = (long)VIDEO_POS_Y(cut->pos) - VIDEO_POS_Y(dst->pos);
   unsigned long x0, y0, x1, y1;

   if (!dw || !dh)
   {
      out->pos  = VIDEO_POS_PACK(v->x, v->y);
      out->dims = VIDEO_SCALE_PACK(v->width, v->height);
      return;
   }
   if (dx < 0)
      dx = 0;
   if (dy < 0)
      dy = 0;
   /* The cut's edges through the scale, kept within the view */
   x0 = (unsigned long)dx * v->width  / dw;
   y0 = (unsigned long)dy * v->height / dh;
   x1 = ((unsigned long)dx + VIDEO_SCALE_W(cut->dims)) * v->width  / dw;
   y1 = ((unsigned long)dy + VIDEO_SCALE_H(cut->dims)) * v->height / dh;
   if (x1 > v->width)
      x1 = v->width;
   if (y1 > v->height)
      y1 = v->height;
   if (x0 >= x1)
      x0 = x1 ? x1 - 1 : 0;
   if (y0 >= y1)
      y0 = y1 ? y1 - 1 : 0;
   if (x1 <= x0)
      x1 = x0 + 1;
   if (y1 <= y0)
      y1 = y0 + 1;
   out->pos  = VIDEO_POS_PACK(v->x + x0, v->y + y0);
   out->dims = VIDEO_SCALE_PACK(x1 - x0, y1 - y0);
}

bool video_views_first_drawn(const video_views_layout_t *layout,
      unsigned view, bool all_areas, unsigned dims,
      video_views_rect_t *out)
{
   unsigned i;
   for (i = 0; i < layout->num_placements; i++)
   {
      video_views_rect_t r;
      const video_views_placement_t *p = &layout->placements[i];
      if (     p->view == view
            && !(p->flags & (VIDEO_VIEWS_PLACEMENT_BLANK
                  | VIDEO_VIEWS_PLACEMENT_IMAGE))
            && (all_areas || p->area == 0)
            && video_views_visible(layout, p, dims, &r))
      {
         *out = p->dst;
         return true;
      }
   }
   return false;
}

static int16_t video_views_norm(unsigned p, unsigned size)
{
   if (p == 0 || size < 2)
      return -0x7fff;
   if (p >= size - 1)
      return 0x7fff;
   return (int16_t)((int)(((unsigned long)p * 0xffffUL) / (size - 1)) - 0x8000);
}

/* Squared distance from a point to r, 0 inside it. */
static double video_views_rect_dist(const video_views_rect_t *r,
      int x, int y)
{
   int x0    = VIDEO_POS_X(r->pos);
   int y0    = VIDEO_POS_Y(r->pos);
   int w     = (int)VIDEO_SCALE_W(r->dims);
   int h     = (int)VIDEO_SCALE_H(r->dims);
   double dx = 0.0, dy = 0.0;
   if (x < x0)
      dx = x0 - x;
   else if (x >= x0 + w)
      dx = x - (x0 + w - 1);
   if (y < y0)
      dy = y0 - y;
   else if (y >= y0 + h)
      dy = y - (y0 + h - 1);
   return dx * dx + dy * dy;
}

bool video_views_map_point(const video_views_layout_t *layout,
      const video_views_map_t *map, unsigned frame_dims,
      int x, int y, bool report_oob, uint32_t *res_pos)
{
   unsigned i;
   int best      = -1;
   double best_d = 0.0;
   bool no_input = false;
   const video_views_placement_t *pl;
   const struct retro_video_view *ref;
   int cx, cy, r, dst_w, dst_h;
   int16_t px, py;

   if (!map->num_views)
      return false;
   /* Topmost first; it keeps a tie. */
   for (i = layout->num_placements; i-- > 0; )
   {
      const video_views_rect_t *d = &layout->placements[i].dst;
      double dist;
      /* The offscreen canvas's right half never reaches the window. */
      if (layout->offscreen && layout->placements[i].area != 0)
         continue;
      if (!VIDEO_SCALE_W(d->dims) || !VIDEO_SCALE_H(d->dims))
         continue;
      if (     (layout->placements[i].flags & VIDEO_VIEWS_PLACEMENT_IMAGE)
            || (map->no_input
               & (1u << map->views[layout->placements[i].view].screen)))
      {
         no_input = true;
         continue;
      }
      dist = video_views_rect_dist(d, x, y);
      if (best < 0 || dist < best_d)
      {
         best   = (int)i;
         best_d = dist;
         if (dist == 0.0)
            break;
      }
   }
   if (best < 0 && !no_input)
      return false;
   /* Off every view, or only NO_INPUT screens placed */
   if (best < 0 || (best_d > 0.0 && report_oob))
   {
      *res_pos = VIDEO_POS_PACK(-0x8000, -0x8000);
      return true;
   }

   pl = &layout->placements[best];
   r  = video_views_find(map, map->views[pl->view].screen,
         RETRO_VIDEO_VIEW_EYE_NONE);
   if (r < 0)
      return false;
   ref = &map->views[r];

   dst_w = (int)VIDEO_SCALE_W(pl->dst.dims);
   dst_h = (int)VIDEO_SCALE_H(pl->dst.dims);
   cx    = x - VIDEO_POS_X(pl->dst.pos);
   cy    = y - VIDEO_POS_Y(pl->dst.pos);
   if (cx < 0)
      cx = 0;
   if (cx >= dst_w)
      cx = dst_w - 1;
   if (cy < 0)
      cy = 0;
   if (cy >= dst_h)
      cy = dst_h - 1;

   px       = video_views_norm(ref->x
         + (unsigned)cx * ref->width  / (unsigned)dst_w,
         VIDEO_SCALE_W(frame_dims));
   py       = video_views_norm(ref->y
         + (unsigned)cy * ref->height / (unsigned)dst_h,
         VIDEO_SCALE_H(frame_dims));
   *res_pos = VIDEO_POS_PACK(px, py);
   return true;
}

bool video_views_point_no_input(const video_views_map_t *map, int x, int y)
{
   unsigned i;
   bool no_input = false;
   for (i = 0; i < map->num_views; i++)
   {
      const struct retro_video_view *v = &map->views[i];
      if (     x <  (int)v->x || y <  (int)v->y
            || x >= (int)(v->x + v->width) || y >= (int)(v->y + v->height))
         continue;
      if (!(map->no_input & (1u << v->screen)))
         return false;
      no_input = true;
   }
   return no_input;
}

void video_views_ui_point(const video_views_layout_t *layout,
      int x, int y, int *res_x, int *res_y)
{
   unsigned i;
   const video_views_rect_t *best = NULL;
   double best_d                  = 0.0;

   *res_x = x;
   *res_y = y;
   if (!layout || !layout->ui_per_eye)
      return;

   for (i = 0; i < layout->num_areas; i++)
   {
      const video_views_rect_t *a = &layout->areas[i];
      double dist;
      if (!VIDEO_SCALE_W(a->dims) || !VIDEO_SCALE_H(a->dims))
         continue;
      dist = video_views_rect_dist(a, x, y);
      if (!best || dist < best_d)
      {
         best   = a;
         best_d = dist;
      }
   }
   if (!best)
      return;

   *res_x = (int)floor((double)(x - VIDEO_POS_X(best->pos))
         * VIDEO_SCALE_W(layout->ui_dims) / VIDEO_SCALE_W(best->dims));
   *res_y = (int)floor((double)(y - VIDEO_POS_Y(best->pos))
         * VIDEO_SCALE_H(layout->ui_dims) / VIDEO_SCALE_H(best->dims));
}
