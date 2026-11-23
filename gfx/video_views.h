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

#ifndef __VIDEO_VIEWS_H
#define __VIDEO_VIEWS_H

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <retro_inline.h>
#include <retro_atomic.h>
#include <rthreads/retro_seqlatch.h>
#include <retro_common_api.h>
#include <libretro.h>

RETRO_BEGIN_DECLS

enum video_stereo_mode
{
   VIDEO_STEREO_MODE_2D = 0,
   VIDEO_STEREO_MODE_SBS_HALF,
   VIDEO_STEREO_MODE_SBS_FULL,
   VIDEO_STEREO_MODE_TOP_BOTTOM,
   VIDEO_STEREO_MODE_ANAGLYPH,
   VIDEO_STEREO_MODE_INTERLACED,
   VIDEO_STEREO_MODE_LAST
};

enum video_screen_layout
{
   VIDEO_SCREEN_LAYOUT_VERTICAL = 0,
   VIDEO_SCREEN_LAYOUT_HORIZONTAL,
   VIDEO_SCREEN_LAYOUT_LAST
};

/* Headset input settings (video_openxr_controllers, video_openxr_laser,
 * video_openxr_laser_beam, video_openxr_touch_hands,
 * video_openxr_touch_button). */
enum video_openxr_controllers
{
   VIDEO_OPENXR_CONTROLLERS_COMBINED = 0,
   VIDEO_OPENXR_CONTROLLERS_SEPARATE,
   VIDEO_OPENXR_CONTROLLERS_LAST
};

enum video_openxr_laser
{
   VIDEO_OPENXR_LASER_AUTO = 0,
   VIDEO_OPENXR_LASER_ALWAYS,
   VIDEO_OPENXR_LASER_OFF,
   VIDEO_OPENXR_LASER_LAST
};

enum video_openxr_touch_hands
{
   VIDEO_OPENXR_TOUCH_HANDS_BOTH = 0,
   VIDEO_OPENXR_TOUCH_HANDS_LEFT,
   VIDEO_OPENXR_TOUCH_HANDS_RIGHT,
   VIDEO_OPENXR_TOUCH_HANDS_NONE,
   VIDEO_OPENXR_TOUCH_HANDS_LAST
};

enum video_openxr_touch_button
{
   VIDEO_OPENXR_TOUCH_BUTTON_TRIGGER = 0,
   VIDEO_OPENXR_TOUCH_BUTTON_GRIP,
   VIDEO_OPENXR_TOUCH_BUTTON_STICK,
   VIDEO_OPENXR_TOUCH_BUTTON_BUMPER,
   VIDEO_OPENXR_TOUCH_BUTTON_A,
   VIDEO_OPENXR_TOUCH_BUTTON_B,
   VIDEO_OPENXR_TOUCH_BUTTON_X,
   VIDEO_OPENXR_TOUCH_BUTTON_Y,
   VIDEO_OPENXR_TOUCH_BUTTON_LAST
};

/* What a core is told the headset shows, cycled by the Headset View
 * hotkey and never saved: IMMERSIVE reports what the headset can do,
 * STEREOSCOPIC withholds RETRO_VIDEO_VIEWS_STATUS_HMD, FLAT also
 * RETRO_VIDEO_VIEWS_STATUS_STEREO. */
enum video_headset_view
{
   VIDEO_HEADSET_VIEW_IMMERSIVE = 0,
   VIDEO_HEADSET_VIEW_STEREOSCOPIC,
   VIDEO_HEADSET_VIEW_FLAT,
   VIDEO_HEADSET_VIEW_LAST
};

enum video_openxr_laser_beam
{
   VIDEO_OPENXR_LASER_BEAM_OFF = 0,
   VIDEO_OPENXR_LASER_BEAM_EDIT,
   VIDEO_OPENXR_LASER_BEAM_ALWAYS,
   VIDEO_OPENXR_LASER_BEAM_LAST
};

/* Snap to Angles: the step edit mode turns and tilts a held item in
 * (video_openxr_edit_snap_angles). */
enum video_openxr_edit_snap_angles
{
   VIDEO_OPENXR_EDIT_SNAP_ANGLES_OFF = 0,
   VIDEO_OPENXR_EDIT_SNAP_ANGLES_15,
   VIDEO_OPENXR_EDIT_SNAP_ANGLES_30,
   VIDEO_OPENXR_EDIT_SNAP_ANGLES_45,
   VIDEO_OPENXR_EDIT_SNAP_ANGLES_90,
   VIDEO_OPENXR_EDIT_SNAP_ANGLES_LAST
};

/* Hinge Gap: the gap edit mode leaves between screens whose edges
 * meet (video_openxr_edit_hinge_gap). */
enum video_openxr_edit_hinge_gap
{
   VIDEO_OPENXR_EDIT_HINGE_GAP_0 = 0,
   VIDEO_OPENXR_EDIT_HINGE_GAP_1,
   VIDEO_OPENXR_EDIT_HINGE_GAP_2,
   VIDEO_OPENXR_EDIT_HINGE_GAP_3,
   VIDEO_OPENXR_EDIT_HINGE_GAP_5,
   VIDEO_OPENXR_EDIT_HINGE_GAP_10,
   VIDEO_OPENXR_EDIT_HINGE_GAP_LAST
};

/* Edit Smoothing: how much edit mode steadies the controllers' aim
 * (video_openxr_edit_smoothing). */
enum video_openxr_edit_smoothing
{
   VIDEO_OPENXR_EDIT_SMOOTHING_OFF = 0,
   VIDEO_OPENXR_EDIT_SMOOTHING_LOW,
   VIDEO_OPENXR_EDIT_SMOOTHING_MEDIUM,
   VIDEO_OPENXR_EDIT_SMOOTHING_HIGH,
   VIDEO_OPENXR_EDIT_SMOOTHING_LAST
};

/* video_openxr_placement: where the headset keeps the screens. */
enum video_openxr_placement
{
   VIDEO_OPENXR_PLACEMENT_FIXED = 0,
   VIDEO_OPENXR_PLACEMENT_FOLLOW,
   VIDEO_OPENXR_PLACEMENT_LAZY,
   VIDEO_OPENXR_PLACEMENT_LAST
};

/* video_openxr_refresh_rate: one of these, or a rate in Hz. */
enum video_openxr_refresh
{
   VIDEO_OPENXR_REFRESH_AUTO = 0,
   VIDEO_OPENXR_REFRESH_HEADSET
};

/* The most refresh rates read from a headset's list. */
#define VIDEO_HEADSET_MAX_RATES 16

/* An origin in VIDEO_POS_PACK's layout and a size in
 * VIDEO_SCALE_PACK's, as video_viewport_t keeps them. */
typedef struct video_views_rect
{
   unsigned pos;
   unsigned dims;
} video_views_rect_t;

/* A validated copy of a core's view map. */
typedef struct video_views_map
{
   struct retro_video_view views[RETRO_VIDEO_VIEWS_MAX];
   unsigned num_views;
   unsigned num_screens;
   /* Bit s is screen s. */
   unsigned no_input;   /* RETRO_VIDEO_SCREEN_FLAG_NO_INPUT */
   unsigned viewless;   /* below num_screens with no view */
   /* The frontend's, set per frame; validation leaves them 0. */
   unsigned left_out;   /* ABSENT, or its Display resolved to Off: no place */
   unsigned blank;      /* BLANK and not left out: a place, drawn black */
   /* The frontend's arrangement, set with the masks; validation leaves
    * it 0. */
   unsigned over;       /* screens drawn as insets this frame */
   uint32_t base;       /* 4 bits a screen: an inset's base, a sized screen's */
   uint32_t anchor;     /* 4 bits a screen: an inset's anchor, 1-8 */
   uint8_t opacity[RETRO_VIDEO_VIEWS_MAX];  /* an inset's; 255 solid */
   float size[RETRO_VIDEO_VIEWS_MAX];       /* of its base's height; 0 none */
   /* Set by validation. */
   unsigned image;                              /* screens drawn from an image */
   unsigned image_dims[RETRO_VIDEO_VIEWS_MAX];  /* VIDEO_SCALE_PACK */
   float image_aspect[RETRO_VIDEO_VIEWS_MAX];   /* display w / h */
   /* The frontend's, set per frame; validation leaves them 0. */
   uintptr_t image_tex[RETRO_VIDEO_VIEWS_MAX];  /* 0 until a picture landed */
   uint8_t image_gen[RETRO_VIDEO_VIEWS_MAX];    /* moves when one lands */
   unsigned nearest;                            /* image screens sampled nearest */
} video_views_map_t;

#define VIDEO_VIEWS_NIBBLE(bits, s) (((bits) >> ((s) * 4)) & 0xfu)

typedef struct video_views_frect
{
   double x;
   double y;
   double w;
   double h;
} video_views_frect_t;

#define VIDEO_VIEWS_NAME_SIZE 32   /* bytes kept of a screen's name, with its NUL */
#define VIDEO_VIEWS_IMAGE_MAX 512  /* largest picture side taken */

/* One validated retro_video_screen. */
typedef struct video_views_screen
{
   unsigned flags;
   unsigned state;
   unsigned show;
   unsigned base;
   unsigned anchor;
   unsigned order;
   unsigned owners;
   unsigned swap_with;
   unsigned group;
   float size;
   float opacity;
   char name[VIDEO_VIEWS_NAME_SIZE];   /* "" when the core gave none */
} video_views_screen_t;

typedef struct video_views_screens
{
   video_views_screen_t screens[RETRO_VIDEO_SCREENS_MAX];
   unsigned count;                     /* 0: the map describes no screens */
} video_views_screens_t;

/* Screen names crossing from the main thread, their one writer, to any
 * thread (rthreads/retro_seqlatch.h). Zeroed: every name "", stamp 0. */
typedef struct video_views_names
{
   char name[RETRO_VIDEO_SCREENS_MAX][VIDEO_VIEWS_NAME_SIZE];
} video_views_names_t;

#define VIDEO_VIEWS_NAMES_WORDS \
   RETRO_SEQLATCH_WORDS(sizeof(video_views_names_t))

typedef struct video_views_names_box
{
   retro_atomic_int_t seq;
   retro_atomic_int_t words[2 * VIDEO_VIEWS_NAMES_WORDS];
} video_views_names_box_t;

void video_views_names_publish(video_views_names_box_t *box,
      const video_views_names_t *names);

/* Returns the stamp it read at. */
int video_views_names_read(video_views_names_box_t *box,
      video_views_names_t *out);

typedef struct video_screen_xr
{
   float x;
   float y;
   float z;
   float yaw;
   float pitch;
   float roll;
   float scale;
   bool visible;
} video_screen_xr_t;

void video_views_xr_reset(video_screen_xr_t *screens, unsigned count);
void video_views_xr_normalize(video_screen_xr_t *screen);

bool video_views_validate(const struct retro_video_view *views,
      unsigned num_views, video_views_map_t *out);

/* A whole SET_VIDEO_VIEWS argument: its views, and its screens when it
 * sets RETRO_VIDEO_VIEWS_FLAG_SCREENS. Writes @out and @screens only
 * when it returns true. */
bool video_views_validate_map(const struct retro_video_views *in,
      video_views_map_t *out, video_views_screens_t *screens);

bool video_views_fit_frame(const video_views_map_t *map,
      unsigned frame_dims);

/* Rescale every rectangle for a CPU filter's output size. Widths scale
 * alone so the two eyes of a screen stay the same size. A view keeps
 * its display aspect ratio. */
void video_views_scale(video_views_map_t *map,
      unsigned in_dims, unsigned out_dims);

/* A view's display aspect ratio: its own, or width / height. */
double video_views_aspect(const struct retro_video_view *v);

/* Screen @screen's display size in source units: its EYE_NONE (or
 * left) view's height times its aspect; on an odd @rotation, the view's
 * width tall at the inverse aspect. A screen that is not an inset and
 * has a size is size times its base's extent tall, at its own (turned)
 * aspect. False without a view. */
bool video_views_extent(const video_views_map_t *map, unsigned screen,
      unsigned rotation, double *w, double *h);

/* An inset's rectangle in @base's units: at @anchor inside @base,
 * @size of its height tall at @aspect (w / h), shrunk to fit inside
 * @base with the aspect kept. */
void video_views_inset(const video_views_frect_t *base, unsigned anchor,
      double size, double aspect, video_views_frect_t *out);

/* Index of the view showing @screen to @eye: its EYE_NONE view if it has
 * one, else the matching eye, and for EYE_NONE the left. -1 if none. */
int video_views_find(const video_views_map_t *map,
      unsigned screen, unsigned eye);

/* A tracked map's eyes: true when the map holds exactly one LEFT and
 * one RIGHT view, neither on a screen left out, their indices in
 * eyes[0] and eyes[1]; else -1. */
bool video_views_eye_pair(const video_views_map_t *map, int eyes[2]);

/* The views of @roots and their insets as a map of their own in @out,
 * their screens counted from 0 in @map's order, bases renumbered with
 * them; @index[i] is @out's view i's index in @map. For a driver that
 * shows some screens on one display and the rest on another. False,
 * with @out emptied, when none of those screens has a view. */
bool video_views_select_screens(const video_views_map_t *map,
      unsigned roots, video_views_map_t *out, unsigned *index);

/* The map for one frame: a copy scaled from the core's frame size to the
 * size the driver receives. False, with out emptied, when there is no map
 * or it does not fit the frame. */
bool video_views_snapshot(const video_views_map_t *map,
      unsigned frame_dims, unsigned out_dims,
      video_views_map_t *out);

#define VIDEO_VIEWS_MAX_PLACEMENTS (RETRO_VIDEO_VIEWS_MAX * 2)

#define VIDEO_VIEWS_PLACEMENT_BLANK (1u << 0)   /* drawn black */
#define VIDEO_VIEWS_PLACEMENT_INSET (1u << 1)   /* drawn over its base's */
/* Bits 8-15 hold 255 minus the opacity, so flags 0 is solid. */
#define VIDEO_VIEWS_PLACEMENT_ALPHA_BITS(a) ((255u - (a)) << 8)
#define VIDEO_VIEWS_PLACEMENT_ALPHA(flags)  (255u - (((flags) >> 8) & 0xffu))

/* One view drawn into one eye's area. */
typedef struct video_views_placement
{
   video_views_rect_t dst;   /* canvas pixels, top-left origin */
   unsigned view;            /* index into the map's views */
   unsigned area;            /* 0: left eye or the only area, 1: right eye */
   unsigned flags;           /* VIDEO_VIEWS_PLACEMENT_* */
} video_views_placement_t;

typedef struct video_views_layout
{
   video_views_placement_t placements[VIDEO_VIEWS_MAX_PLACEMENTS];
   video_views_rect_t areas[2];
   unsigned num_placements;
   unsigned num_areas;
   /* What the placements are drawn into: the window, or for anaglyph and
    * interlaced an offscreen image holding both eyes side by side, which
    * a final pass blends into the window. */
   unsigned canvas_dims;
   /* The size the menu and widgets lay out at. */
   unsigned ui_dims;
   /* The params' stereo mode, so drivers pick the blend without reading
    * settings. */
   unsigned stereo_mode;
   bool offscreen;
   /* The UI is drawn once and placed in each eye's area. */
   bool ui_per_eye;
} video_views_layout_t;

/* What the settings give every layout, filled once a frame on the main
 * thread */
typedef struct video_views_layout_settings
{
   video_views_rect_t custom_vp;   /* window pixels; dims 0: none */
   float single_aspect;
   unsigned stereo_mode;
   unsigned screen_layout;
   unsigned rotation;
   bool stretch;
   bool swap_eyes;
   bool scale_integer;
} video_views_layout_settings_t;

typedef struct video_views_layout_params
{
   const video_views_map_t *map;
   /* One root only, insets aside. custom_vp is the custom viewport in
    * window pixels, or NULL; single_aspect is the aspect ratio setting's
    * value, or 0 for the view's own; stretch fills the area instead. */
   const video_views_rect_t *custom_vp;
   float single_aspect;
   bool stretch;
   unsigned dims;
   unsigned stereo_mode;
   unsigned screen_layout;
   unsigned exclude_screens;
   unsigned rotation;
   bool swap_eyes;
   bool scale_integer;
} video_views_layout_params_t;

void video_views_layout(const video_views_layout_params_t *params,
      video_views_layout_t *out);

/* Whether @a and @b draw the same views in the same places with the
 * same flags. */
bool video_views_layout_same(const video_views_layout_t *a,
      const video_views_layout_t *b);

/* Screens below num_screens with a place of their own: not viewless,
 * left out or an inset. */
unsigned video_views_roots(const video_views_map_t *map);

/* @roots and the insets over them. */
unsigned video_views_with_insets(const video_views_map_t *map,
      unsigned roots);

/* The size the menu and widgets lay out at: one eye's UI size when
 * @layout draws the UI per eye, else @dims. NULL is a frame drawn
 * whole. */
unsigned video_views_ui_dims(const video_views_layout_t *layout,
      unsigned dims);

/* The part of @r inside a target of size @dims, in @out; false when
 * nothing is. */
bool video_views_clip(const video_views_rect_t *r,
      unsigned dims, video_views_rect_t *out);

/* The part of placement @p drawn into a target of size @dims, in @out:
 * what is inside its area and the target. False when nothing is. A
 * custom viewport can be larger than the window, and under threaded
 * video a frame can carry placements made for the window's previous
 * size. */
bool video_views_visible(const video_views_layout_t *layout,
      const video_views_placement_t *p, unsigned dims,
      video_views_rect_t *out);

/* The part of view @v that @cut shows, @cut being part of the rectangle
 * @dst the whole view is scaled to: for a blitter that scales a source
 * rectangle into a destination one, as video_views_visible cut it. At
 * least one pixel each way. */
void video_views_cut_source(const struct retro_video_view *v,
      const video_views_rect_t *dst, const video_views_rect_t *cut,
      video_views_rect_t *out);

/* The rectangle of @view's first placement drawn into a target of
 * size @dims, which its shader passes are sized for; all_areas false
 * draws the first area only. False when no placement of the view
 * shows, a blank one showing nothing of it. */
bool video_views_first_drawn(const video_views_layout_t *layout,
      unsigned view, bool all_areas, unsigned dims,
      video_views_rect_t *out);

/* Window point to packed-frame pointer coordinates, through @layout,
 * VIDEO_POS_PACK'd into @res_pos. @map and @frame_dims are the core's
 * own. A point on any copy of a screen lands in that screen's EYE_NONE
 * or left-eye rectangle. NO_INPUT screens are not there: with only
 * those placed, every point is off every view. */
bool video_views_map_point(const video_views_layout_t *layout,
      const video_views_map_t *map, unsigned frame_dims,
      int x, int y, bool report_oob, uint32_t *res_pos);

/* A packed-frame point, in frame pixels, that lies in a view of a
 * NO_INPUT screen and in no view of a screen that takes input. */
bool video_views_point_no_input(const video_views_map_t *map, int x, int y);

/* Window point to the coordinates the menu and widgets lay out in,
 * through @layout. A per-eye UI: through the area the point is in, or
 * the nearer one, so a point outside both stays outside the UI. NULL,
 * or a UI drawn once, leaves the point as it is. */
void video_views_ui_point(const video_views_layout_t *layout,
      int x, int y, int *res_x, int *res_y);

/* Where touches on a display of a console's own land in the core's
 * packed frame: the views it shows, written by the video driver and read
 * by the input driver without a lock, behind a sequence count that is
 * odd while they change. Zeroed, it holds none. */
typedef struct video_views_touch
{
   video_views_rect_t dst[RETRO_VIDEO_VIEWS_MAX];
   struct retro_video_view src[RETRO_VIDEO_VIEWS_MAX];
   unsigned count;
   unsigned no_input;   /* bit i: entry i takes no input */
   unsigned dims;
   unsigned frame_dims;
   retro_atomic_int_t seq;
} video_views_touch_t;

/* @layout's placements, of @map's views through @index (the selection's
 * to the map's), on a display of size @dims; @frame_dims the packed
 * frame's. A NULL @layout holds none. */
void video_views_touch_publish(video_views_touch_t *t,
      const video_views_map_t *map, const video_views_layout_t *layout,
      const unsigned *index, unsigned dims, unsigned frame_dims);

/* A touch, -0x7fff to 0x7fff across the display, to the core's pointer
 * in its packed frame, -0x8000 off the views that take input. False,
 * with @res_x and @res_y as they were, when the display shows no views. */
bool video_views_touch_point(video_views_touch_t *t, int x, int y,
      int16_t *res_x, int16_t *res_y);

/* A canvas's two halves, each of size @dims, into @out for the CPU
 * drivers, XRGB8888 with strides in pixels: anaglyph by Dubois'
 * least-squares red/cyan matrices, or interlaced, even rows from the
 * left half and odd from the right. */
void video_views_blend_xrgb8888(const uint32_t *canvas,
      size_t canvas_stride, uint32_t *out, size_t out_stride,
      unsigned dims, unsigned stereo_mode);

/* ---- The compositor: what drivers that draw views share ----
 * A driver keeps the GPU objects; the decisions are written once, in
 * gfx/video_views_draw.h, which each driver instantiates with its own
 * types and calls - no function pointers, nothing a driver did by hand
 * costs more. */

/* Per-view shader chains, the driver's, as video_views_draw.h's
 * pick_chains takes them: one set for the preset, one for the stock
 * shader. count is the views the driver last made room for. A chain
 * that could not be made is remembered, with or without a map, until
 * the preset changes or the chains are made again. */
typedef struct video_views_chains
{
   void *preset[RETRO_VIDEO_VIEWS_MAX];
   void *stock[RETRO_VIDEO_VIEWS_MAX];
   unsigned count;
   bool stock_failed;
   bool preset_failed;
} video_views_chains_t;

/* Whether @map has views past @set's count, which pick_chains can't
 * draw: a frame queued before the count shrank, or a dupe laid out
 * from the map before it. Such a frame keeps the last FALLBACK. */
static INLINE bool video_views_chains_behind(
      const video_views_chains_t *set, const video_views_map_t *map)
{
   return map->num_views > set->count;
}

/* A render target the views draw into: the size it was made at, kept
 * when making it failed so it isn't retried every frame. */
typedef struct video_views_target
{
   unsigned dims;
   bool made;
} video_views_target_t;

/* The dims the menu and widgets lay out at and the fonts size from,
 * while the UI layer is drawn. */
typedef struct video_views_ui_dims
{
   unsigned dims;
   unsigned video_dims;
} video_views_ui_dims_t;

/* The UI drawn once and placed in each eye's area: a map with a layout
 * that asks for it, on a driver that draws views (@allowed). */
static INLINE bool video_views_ui_per_eye(const video_views_map_t *map,
      const video_views_layout_t *layout, bool allowed)
{
   return map->num_views && layout->ui_per_eye && allowed;
}

/* The UI layer is drawn when there is UI to draw. A macro, so what is
 * asked last is asked only when nothing before it answered. */
#define VIDEO_VIEWS_UI_WANTED(layer, menu, msg, statistics, widgets) \
   ((layer) && ((menu) || ((msg) && *(msg)) || (statistics) || (widgets)))

/* @dims and @video_dims moved to the UI layer's size, and back. */
static INLINE void video_views_ui_enter(video_views_ui_dims_t *saved,
      unsigned *dims, unsigned *video_dims, unsigned ui_dims)
{
   saved->dims       = *dims;
   saved->video_dims = *video_dims;
   *dims             = ui_dims;
   *video_dims       = ui_dims;
}

static INLINE void video_views_ui_leave(const video_views_ui_dims_t *saved,
      unsigned *dims, unsigned *video_dims)
{
   *dims       = saved->dims;
   *video_dims = saved->video_dims;
}

RETRO_END_DECLS

#endif
