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
   "xr_visible"
};

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
   unsigned set = 0;

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
   if (      !(set & VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_DISPLAY))
         && screens
         && screen < screens->count
         && (   screens->screens[screen].show == RETRO_VIDEO_SCREEN_SHOW_HIDDEN
             || screens->screens[screen].show == RETRO_VIDEO_SCREEN_SHOW_OVER))
   {
      memcpy(out->display, VIDEO_WINDOW_DISPLAY_OFF,
            sizeof(VIDEO_WINDOW_DISPLAY_OFF));
      return VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_DISPLAY);
   }
   return 0;
}

void video_screen_prefs_masks(const video_screen_values_t *resolved,
      const video_views_screens_t *screens, unsigned num_screens,
      unsigned *left_out, unsigned *blank)
{
   unsigned s;
   unsigned count = screens ? screens->count : 0;

   *left_out = 0;
   *blank    = 0;
   if (num_screens > RETRO_VIDEO_VIEWS_MAX)
      num_screens = RETRO_VIDEO_VIEWS_MAX;
   for (s = 0; s < num_screens; s++)
   {
      /* Entries at or past count are stale */
      unsigned state = s < count
         ? screens->screens[s].state : RETRO_VIDEO_SCREEN_STATE_ACTIVE;
      if (   state == RETRO_VIDEO_SCREEN_STATE_ABSENT
          || !strcmp(resolved[s].display, VIDEO_WINDOW_DISPLAY_OFF))
         *left_out |= 1u << s;
      else if (state == RETRO_VIDEO_SCREEN_STATE_BLANK)
         *blank    |= 1u << s;
   }
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
