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

#ifndef __VIDEO_SCREEN_PREFS_H
#define __VIDEO_SCREEN_PREFS_H

#include <stddef.h>

#include <boolean.h>
#include <retro_common_api.h>
#include <retro_miscellaneous.h>
#include <libretro.h>

#include "video_views.h"
#include "video_windows.h"

RETRO_BEGIN_DECLS

/* The per-screen settings, one bit each in a store's set[] */
enum video_screen_pref
{
   VIDEO_SCREEN_PREF_DISPLAY = 0,
   VIDEO_SCREEN_PREF_WINDOW_FULLSCREEN,
   VIDEO_SCREEN_PREF_WINDOW_X,
   VIDEO_SCREEN_PREF_WINDOW_Y,
   VIDEO_SCREEN_PREF_WINDOW_WIDTH,
   VIDEO_SCREEN_PREF_WINDOW_HEIGHT,
   VIDEO_SCREEN_PREF_WINDOW_POS_SET,
   VIDEO_SCREEN_PREF_XR_X,
   VIDEO_SCREEN_PREF_XR_Y,
   VIDEO_SCREEN_PREF_XR_Z,
   VIDEO_SCREEN_PREF_XR_YAW,
   VIDEO_SCREEN_PREF_XR_PITCH,
   VIDEO_SCREEN_PREF_XR_ROLL,
   VIDEO_SCREEN_PREF_XR_SCALE,
   VIDEO_SCREEN_PREF_XR_VISIBLE,
   VIDEO_SCREEN_PREF_CORNER,
   VIDEO_SCREEN_PREF_SIZE,
   VIDEO_SCREEN_PREF_OPACITY,
   VIDEO_SCREEN_PREF_COUNT
};

#define VIDEO_SCREEN_PREF_BIT(p)   (1u << (p))
/* The bits of XR_X to XR_SCALE */
#define VIDEO_SCREEN_PREF_XR_PLACE \
   (  VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_XR_SCALE + 1) \
    - VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_XR_X))
#define VIDEO_SCREEN_PREF_XR_ALL \
   (  VIDEO_SCREEN_PREF_XR_PLACE \
    | VIDEO_SCREEN_PREF_BIT(VIDEO_SCREEN_PREF_XR_VISIBLE))

#define VIDEO_SCREEN_SIZE_DEFAULT    25
#define VIDEO_SCREEN_OPACITY_DEFAULT 100

/* One screen's per-screen settings, in the forms settings_t keeps */
typedef struct video_screen_values
{
   unsigned window_pos;    /* VIDEO_POS_PACK */
   unsigned window_dims;   /* VIDEO_SCALE_PACK; 0 automatic */
   unsigned corner;        /* 0 Default, else RETRO_VIDEO_SCREEN_ANCHOR_ 1-8 */
   unsigned size;          /* percent of the base's shown height; 0 Default */
   unsigned opacity;       /* percent; 0 Default */
   video_screen_xr_t xr;   /* headset placement and visible */
   char display[VIDEO_WINDOW_DISPLAY_NAME_SIZE]; /* "" Auto, "off", "main", a key */
   bool window_fullscreen;
   bool window_pos_set;
} video_screen_values_t;

/* The running core's choices: only what the user set */
typedef struct video_screen_prefs
{
   video_screen_values_t values[RETRO_VIDEO_VIEWS_MAX];
   unsigned set[RETRO_VIDEO_VIEWS_MAX];  /* VIDEO_SCREEN_PREF_BIT()s */
   char library_name[NAME_MAX_LENGTH];   /* the core loaded, whose file it is */
   bool core_loaded;  /* a core, not the dummy, is loaded: the store is its */
   bool in_use;       /* it uses views: it sent a map since loading, or had a file */
   bool dirty;        /* changed since loaded or saved */
} video_screen_prefs_t;

/* Pure. @screen's values: each from @prefs where set, else the core's
 * default from @screens, else @global. The core's defaults: Display
 * Off for show HIDDEN and the Over value of its base for OVER; Corner,
 * Size and Opacity its non-zero anchor, size and opacity, the last two
 * as rounded percents. A LAYER's Display is "" unless it is Off, and
 * its show gives no default. Returns the VIDEO_SCREEN_PREF_BIT()s whose
 * value is the core's default. @prefs and @screens may be NULL. */
unsigned video_screen_prefs_resolve(const video_screen_prefs_t *prefs,
      const video_views_screens_t *screens,
      const video_screen_values_t *global, unsigned screen,
      video_screen_values_t *out);

/* Pure. The base an Over value names, from 0; -1 for any other string. */
int video_screen_prefs_over_base(const char *display);

/* Pure. The Over value naming @base into @s. */
size_t video_screen_prefs_over_value(unsigned base, char *s, size_t len);

/* Pure. @map's left_out, blank, over, base, anchor, opacity and size for
 * its num_screens screens, from each screen's @resolved values,
 * @defaults (resolve()'s return per screen) and the core's @screens
 * (NULL: none). Reads @map's num_screens, viewless, image, and
 * rules_out and layer, whose screens are left out, as ABSENT.
 * @images: STATUS_IMAGES this frame. Without it every image screen is
 * left out, as ABSENT.
 * @turned: the core's rotation is not 0 this frame: upright is left out
 * then, as ABSENT, besides rules_out and layer. */
void video_screen_prefs_arrange(const video_screen_values_t *resolved,
      const unsigned *defaults, const video_views_screens_t *screens,
      bool images, bool turned, video_views_map_t *map);

/* Takes @bits of @values for @screen, marking them set; NULL @values
 * clears those entries. dirty only when something changed. */
void video_screen_prefs_set(video_screen_prefs_t *prefs, unsigned screen,
      unsigned bits, const video_screen_values_t *values);

/* @pref's key for @screen, as the global config names it:
 * "video_screen%u_display", "video_screen%u_window_x", "video_screen%u_xr_scale", ... */
size_t video_screen_prefs_key(unsigned screen, unsigned pref,
      char *s, size_t len);

RETRO_END_DECLS

#endif
