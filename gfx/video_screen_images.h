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

#ifndef __VIDEO_SCREEN_IMAGES_H
#define __VIDEO_SCREEN_IMAGES_H

#include <boolean.h>
#include <retro_common_api.h>
#include <libretro.h>

#include "video_views.h"

RETRO_BEGIN_DECLS

/* The pictures of image screens, a store per core instance. Main thread
 * only. */

enum video_screen_images_owner
{
   VIDEO_SCREEN_IMAGES_MAIN = 0,
   VIDEO_SCREEN_IMAGES_SECOND,   /* run-ahead's second instance */
   VIDEO_SCREEN_IMAGES_OWNERS
};

#define VIDEO_SCREEN_IMAGES_SLOTS 3   /* per screen per instance */

/* The core instance whose environment calls run now: MAIN unless the
 * run-ahead hook says otherwise. */
void video_screen_images_set_caller(unsigned owner);

/* runahead_secondary_core_destroy: @owner's store emptied and its
 * lends ended. */
void video_screen_images_free_owner(unsigned owner);

/* GET_VIDEO_SCREEN_BUFFER for the caller's store. @images: whether
 * STATUS_IMAGES is set now. False leaves @buf as it was. */
bool video_screen_images_lend(struct retro_video_screen_buffer *buf,
      bool images);

/* SET_VIDEO_VIEWS's state rules and pixels for @map, already validated,
 * against the caller's store, all or nothing: false changes nothing and
 * ends no lend. True also makes the caller's store the one whose
 * pictures are drawn. */
bool video_screen_images_set(const struct retro_video_views *in,
      const video_views_map_t *map);

/* Main thread, a shown frame, after the arrange step, never in the
 * redraw: uploads the shown store's newest picture of each screen of
 * @draw, and writes @map's image_tex and image_gen and the image bits
 * of @nearest into map->nearest. */
void video_screen_images_frame(video_views_map_t *map, unsigned draw,
      unsigned nearest);

/* driver_uninit, after video_thread_wait_idle, before the driver
 * goes: every surface freed and every pin cleared. Slots and pictures
 * stay for the next driver. */
void video_screen_images_driver_free(void);

/* Content unload: both stores emptied, their textures freed, every lend
 * ended. */
void video_screen_images_clear(void);

RETRO_END_DECLS

#endif
