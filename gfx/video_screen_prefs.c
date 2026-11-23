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

#include <stdio.h>
#include <string.h>

#include <compat/msvc.h>

#include "video_screen_prefs.h"
#include "video_defines.h"

static const char *const video_screen_prefs_names[VIDEO_SCREEN_PREF_COUNT] = {
   "display",
   "window_fullscreen",
   "window_x",
   "window_y",
   "window_width",
   "window_height",
   "window_pos_set",
   "xr_x",
   "xr_y",
   "xr_z",
   "xr_yaw",
   "xr_pitch",
   "xr_roll",
   "xr_scale",
   "xr_visible",
   "corner",
   "size",
   "opacity"
};

/* A non-zero share as a percent, at least 1, at most 100 */
static unsigned video_screen_prefs_percent(float f)
{
   unsigned p;
   if (f >= 1.0f)
      return 100;
   p = (unsigned)(f * 100.0f + 0.5f);
   return p ? p : 1;
}

/* Copies @pref from @from into @to; true when it differed */
static bool video_screen_prefs_take(video_screen_values_t *to,
      const video_screen_values_t *from, unsigned pref)
{
   bool changed = false;
   switch (pref)
   {
      case VIDEO_SCREEN_PREF_DISPLAY:
         changed = strncmp(to->display, from->display,
               sizeof(to->display)) != 0;
         memcpy(to->display, from->display, sizeof(to->display));
         to->display[sizeof(to->display) - 1] = '\0';
         break;
      case VIDEO_SCREEN_PREF_WINDOW_FULLSCREEN:
         changed = to->window_fullscreen != from->window_fullscreen;
         to->window_fullscreen = from->window_fullscreen;
         break;
      case VIDEO_SCREEN_PREF_WINDOW_X:
         changed = VIDEO_POS_X(to->window_pos) != VIDEO_POS_X(from->window_pos);
         to->window_pos = VIDEO_POS_PACK(VIDEO_POS_X(from->window_pos),
               VIDEO_POS_Y(to->window_pos));
         break;
      case VIDEO_SCREEN_PREF_WINDOW_Y:
         changed = VIDEO_POS_Y(to->window_pos) != VIDEO_POS_Y(from->window_pos);
         to->window_pos = VIDEO_POS_PACK(VIDEO_POS_X(to->window_pos),
               VIDEO_POS_Y(from->window_pos));
         break;
      case VIDEO_SCREEN_PREF_WINDOW_WIDTH:
         changed = VIDEO_SCALE_W(to->window_dims)
            != VIDEO_SCALE_W(from->window_dims);
         to->window_dims = VIDEO_SCALE_PACK(VIDEO_SCALE_W(from->window_dims),
               VIDEO_SCALE_H(to->window_dims));
         break;
      case VIDEO_SCREEN_PREF_WINDOW_HEIGHT:
         changed = VIDEO_SCALE_H(to->window_dims)
            != VIDEO_SCALE_H(from->window_dims);
         to->window_dims = VIDEO_SCALE_PACK(VIDEO_SCALE_W(to->window_dims),
               VIDEO_SCALE_H(from->window_dims));
         break;
      case VIDEO_SCREEN_PREF_WINDOW_POS_SET:
         changed = to->window_pos_set != from->window_pos_set;
         to->window_pos_set = from->window_pos_set;
         break;
      case VIDEO_SCREEN_PREF_XR_X:
         changed = to->xr.x != from->xr.x;
         to->xr.x = from->xr.x;
         break;
      case VIDEO_SCREEN_PREF_XR_Y:
         changed = to->xr.y != from->xr.y;
         to->xr.y = from->xr.y;
         break;
      case VIDEO_SCREEN_PREF_XR_Z:
         changed = to->xr.z != from->xr.z;
         to->xr.z = from->xr.z;
         break;
      case VIDEO_SCREEN_PREF_XR_YAW:
         changed = to->xr.yaw != from->xr.yaw;
         to->xr.yaw = from->xr.yaw;
         break;
      case VIDEO_SCREEN_PREF_XR_PITCH:
         changed = to->xr.pitch != from->xr.pitch;
         to->xr.pitch = from->xr.pitch;
         break;
      case VIDEO_SCREEN_PREF_XR_ROLL:
         changed = to->xr.roll != from->xr.roll;
         to->xr.roll = from->xr.roll;
         break;
      case VIDEO_SCREEN_PREF_XR_SCALE:
         changed = to->xr.scale != from->xr.scale;
         to->xr.scale = from->xr.scale;
         break;
      case VIDEO_SCREEN_PREF_XR_VISIBLE:
         changed = to->xr.visible != from->xr.visible;
         to->xr.visible = from->xr.visible;
         break;
      case VIDEO_SCREEN_PREF_CORNER:
         changed = to->corner != from->corner;
         to->corner = from->corner;
         break;
      case VIDEO_SCREEN_PREF_SIZE:
         changed = to->size != from->size;
         to->size = from->size;
         break;
      case VIDEO_SCREEN_PREF_OPACITY:
         changed = to->opacity != from->opacity;
         to->opacity = from->opacity;
         break;
      default:
         break;
   }
   return changed;
}

unsigned video_screen_prefs_resolve(const video_screen_prefs_t *prefs,
      const video_views_screens_t *screens,
      const video_screen_values_t *global, unsigned screen,
      video_screen_values_t *out)
{
   unsigned p;
   const video_views_screen_t *core;
   unsigned set      = 0;
   unsigned defaults = 0;

   *out = *global;
   if (screen >= RETRO_VIDEO_VIEWS_MAX)
      return 0;
   if (prefs)
   {
      set = prefs->set[screen];
      for (p = 0; p < VIDEO_SCREEN_PREF_COUNT; p++)
         if (set & VIDEO_SCREEN_PREF_BIT(p))
            video_screen_prefs_take(out, &prefs->values[screen], p);
   }
   /* A lone width or height, as a hand-edited file can give, is no size */
   if (   !VIDEO_SCALE_W(out->window_dims)
       || !VIDEO_SCALE_H(out->window_dims))
      out->window_dims = 0;
   if (!screens || screen >= screens->count)
      return 0;
   core = &screens->screens[screen];
   if (!(set & VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_DISPLAY)))
   {
      if (core->show == RETRO_VIDEO_SCREEN_SHOW_HIDDEN)
         memcpy(out->display, VIDEO_WINDOW_DISPLAY_OFF,
               sizeof(VIDEO_WINDOW_DISPLAY_OFF));
      else if (core->show == RETRO_VIDEO_SCREEN_SHOW_OVER)
         video_screen_prefs_over_value(core->base, out->display,
               sizeof(out->display));
      if (core->show != RETRO_VIDEO_SCREEN_SHOW_SHOWN)
         defaults |= VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_DISPLAY);
   }
   if (     !(set & VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_CORNER))
         && core->anchor)
   {
      out->corner = core->anchor;
      defaults   |= VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_CORNER);
   }
   if (     !(set & VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_SIZE))
         && core->size > 0.0f)
   {
      out->size = video_screen_prefs_percent(core->size);
      defaults |= VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_SIZE);
   }
   if (     !(set & VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_OPACITY))
         && core->opacity > 0.0f)
   {
      out->opacity = video_screen_prefs_percent(core->opacity);
      defaults    |= VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_OPACITY);
   }
   return defaults;
}

int video_screen_prefs_over_base(const char *display)
{
   unsigned n = 0;
   const char *p;
   if (strncmp(display, VIDEO_WINDOW_DISPLAY_OVER,
            sizeof(VIDEO_WINDOW_DISPLAY_OVER) - 1))
      return -1;
   p = display + sizeof(VIDEO_WINDOW_DISPLAY_OVER) - 1;
   /* No sign, space or leading zero */
   if (*p < '1' || *p > '9')
      return -1;
   for (; *p >= '0' && *p <= '9'; p++)
   {
      n = n * 10 + (unsigned)(*p - '0');
      if (n > RETRO_VIDEO_VIEWS_MAX)
         return -1;
   }
   return *p ? -1 : (int)n - 1;
}

size_t video_screen_prefs_over_value(unsigned base, char *s, size_t len)
{
   return (size_t)snprintf(s, len, VIDEO_WINDOW_DISPLAY_OVER "%u",
         base + 1);
}

/* Where @s's chain of bases through @insets ends; @s itself when the
 * chain comes back to @s or never ends */
static unsigned video_screen_prefs_root(const unsigned *base,
      unsigned insets, unsigned s, unsigned num_screens)
{
   unsigned i;
   unsigned t = base[s];
   for (i = 0; i < num_screens && t != s; i++)
   {
      if (!(insets & (1u << t)))
         return t;
      t = base[t];
   }
   return s;
}

/* @s as on Auto: the core's own show */
static void video_screen_prefs_auto(const video_views_screen_t *core,
      unsigned s, unsigned num_screens, unsigned *base, unsigned *over,
      unsigned *out)
{
   if (!core)
      return;
   if (core->show == RETRO_VIDEO_SCREEN_SHOW_HIDDEN)
      *out  |= 1u << s;
   else if (   core->show == RETRO_VIDEO_SCREEN_SHOW_OVER
            && core->base < num_screens && core->base != s)
   {
      *over |= 1u << s;
      base[s] = core->base;
   }
}

/* An inset's anchor, size and opacity: the core's own where they are
 * its defaults, else the resolved values, else the built-in defaults */
static void video_screen_prefs_inset(const video_screen_values_t *resolved,
      unsigned defaults, const video_views_screen_t *core, unsigned s,
      video_views_map_t *map)
{
   float size, opacity;
   unsigned anchor = resolved->corner;
   if (!anchor || anchor > RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT)
      anchor = RETRO_VIDEO_SCREEN_ANCHOR_BOTTOM_RIGHT;
   if (core && (defaults & VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_SIZE)))
      size = core->size;
   else
      size = (resolved->size ? resolved->size : VIDEO_SCREEN_SIZE_DEFAULT)
         / 100.0f;
   if (core && (defaults & VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_OPACITY)))
      opacity = core->opacity;
   else
      opacity = (resolved->opacity
            ? resolved->opacity : VIDEO_SCREEN_OPACITY_DEFAULT) / 100.0f;
   if (size > 1.0f)
      size = 1.0f;
   if (opacity > 1.0f)
      opacity = 1.0f;
   map->anchor    |= anchor << (s * 4);
   map->size[s]    = size;
   map->opacity[s] = (uint8_t)(opacity * 255.0f + 0.5f);
   if (!map->opacity[s])
      map->opacity[s] = 1;
}

void video_screen_prefs_arrange(const video_screen_values_t *resolved,
      const unsigned *defaults, const video_views_screens_t *screens,
      bool images, video_views_map_t *map)
{
   unsigned s, i, fall, insets;
   unsigned base[RETRO_VIDEO_VIEWS_MAX];
   unsigned user        = 0;   /* the user's insets */
   unsigned over        = 0;   /* the core's insets */
   unsigned out         = 0;   /* ABSENT, Off or HIDDEN */
   unsigned count       = screens ? screens->count : 0;
   unsigned num_screens = map->num_screens;

   map->left_out = 0;
   map->blank    = 0;
   map->over     = 0;
   map->base     = 0;
   map->anchor   = 0;
   memset(map->opacity, 0, sizeof(map->opacity));
   memset(map->size, 0, sizeof(map->size));
   if (num_screens > RETRO_VIDEO_VIEWS_MAX)
      num_screens = RETRO_VIDEO_VIEWS_MAX;

   for (s = 0; s < num_screens; s++)
   {
      /* Entries at or past count are stale */
      const video_views_screen_t *core = s < count
         ? &screens->screens[s] : NULL;
      int b = video_screen_prefs_over_base(resolved[s].display);
      base[s] = s;
      if (core && core->state == RETRO_VIDEO_SCREEN_STATE_ABSENT)
         out |= 1u << s;
      if (!images && (map->image & (1u << s)))
         out |= 1u << s;
      if (!strcmp(resolved[s].display, VIDEO_WINDOW_DISPLAY_OFF))
         out |= 1u << s;
      else if (b >= 0)
      {
         /* The user's, naming another screen with a view; else as on
          * Auto, which the core's own Over is */
         if (      (unsigned)b < num_screens && (unsigned)b != s
               && !(map->viewless & (1u << b))
               && !(defaults[s]
                  & VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_DISPLAY)))
         {
            user   |= 1u << s;
            base[s] = (unsigned)b;
         }
         else
            video_screen_prefs_auto(core, s, num_screens, base, &over, &out);
      }
   }

   /* The user's first, so the user's choice wins. One whose chain of
    * the user's insets has no root falls back to Auto; a loop's every
    * member does. */
   fall = 0;
   for (s = 0; s < num_screens; s++)
      if (     (user & (1u << s))
            && video_screen_prefs_root(base, user, s, num_screens) == s)
         fall |= 1u << s;
   user &= ~fall;
   for (s = 0; s < num_screens; s++)
      if (fall & (1u << s))
      {
         base[s] = s;
         video_screen_prefs_auto(s < count ? &screens->screens[s] : NULL,
               s, num_screens, base, &over, &out);
      }
   /* The core's with no root are SHOWN */
   fall = 0;
   for (s = 0; s < num_screens; s++)
      if (     (over & (1u << s))
            && video_screen_prefs_root(base, user | over, s, num_screens)
               == s)
         fall |= 1u << s;
   insets = (over & ~fall) | user;

   for (s = 0; s < num_screens; s++)
   {
      unsigned bit                     = 1u << s;
      const video_views_screen_t *core = s < count
         ? &screens->screens[s] : NULL;
      if (insets & bit)
      {
         /* One level; left out with any base on the way */
         unsigned t = s;
         for (i = 0; i < num_screens && (insets & (1u << t)); i++)
         {
            t = base[t];
            if (out & (1u << t))
               map->left_out |= bit;
         }
         map->base |= t << (s * 4);
         video_screen_prefs_inset(&resolved[s], defaults[s], core, s, map);
      }
      else if (core && core->size > 0.0f && core->base < num_screens)
      {
         map->base   |= core->base << (s * 4);
         map->size[s] = core->size;
      }
      if (out & bit)
         map->left_out |= bit;
      if (     core && core->state == RETRO_VIDEO_SCREEN_STATE_BLANK
            && !(map->left_out & bit))
         map->blank |= bit;
   }
   map->over = insets;
}

void video_screen_prefs_set(video_screen_prefs_t *prefs, unsigned screen,
      unsigned bits, const video_screen_values_t *values)
{
   unsigned p, was;
   bool changed;

   if (!prefs || screen >= RETRO_VIDEO_VIEWS_MAX)
      return;
   bits &= VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_COUNT) - 1;
   was   = prefs->set[screen];
   if (!values)
   {
      changed            = (was & bits) != 0;
      prefs->set[screen] = was & ~bits;
   }
   else
   {
      changed            = (bits & ~was) != 0;
      for (p = 0; p < VIDEO_SCREEN_PREF_COUNT; p++)
         if (   (bits & VIDEO_SCREEN_PREF_BIT(p))
             && video_screen_prefs_take(&prefs->values[screen], values, p))
            changed = true;
      prefs->set[screen] = was | bits;
   }
   if (changed)
      prefs->dirty = true;
}

size_t video_screen_prefs_key(unsigned screen, unsigned pref,
      char *s, size_t len)
{
   if (pref >= VIDEO_SCREEN_PREF_COUNT)
   {
      if (len)
         *s = '\0';
      return 0;
   }
   return (size_t)snprintf(s, len, "video_screen%u_%s", screen + 1,
         video_screen_prefs_names[pref]);
}
