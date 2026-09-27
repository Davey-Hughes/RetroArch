/*  RetroArch - A frontend for libretro.
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

#ifndef __RARCH_VIDEO_RASTER_H
#define __RARCH_VIDEO_RASTER_H

#include <stdint.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* The rows a core reports finished through
 * RETRO_ENVIRONMENT_GET_RASTER_POLL_INTERFACE, checked against that
 * contract a frame at a time. */

enum video_raster_violation
{
   VIDEO_RASTER_OK = 0,
   VIDEO_RASTER_BAD_DATA,
   VIDEO_RASTER_BAD_ROW,
   VIDEO_RASTER_BAD_ORDER,
   VIDEO_RASTER_BAD_SIZE
};

enum video_raster_flags
{
   VIDEO_RASTER_FLAG_LOGGED       = (1 << 0),
   VIDEO_RASTER_FLAG_WARNED_DATA  = (1 << 1),
   VIDEO_RASTER_FLAG_WARNED_ROW   = (1 << 2),
   VIDEO_RASTER_FLAG_WARNED_ORDER = (1 << 3),
   VIDEO_RASTER_FLAG_WARNED_SIZE  = (1 << 4),
   /* The frame broke the contract; its later polls are ignored */
   VIDEO_RASTER_FLAG_BROKEN       = (1 << 5),
   /* A frontend replay of the cached frame, not the core's frame */
   VIDEO_RASTER_FLAG_REPLAY       = (1 << 6)
};

typedef struct video_raster
{
   unsigned width;
   unsigned height;
   unsigned calls;
   unsigned first_row;
   unsigned last_row;
   uint8_t  flags;
} video_raster_t;

/* A new core: forgets the frame and the once-only reports */
void video_raster_reset(video_raster_t *raster);

enum video_raster_violation video_raster_poll(video_raster_t *raster,
      const void *data, unsigned width, unsigned height, unsigned row);

/* The frame's video_refresh; a replay passes through untouched */
enum video_raster_violation video_raster_frame_end(video_raster_t *raster,
      const void *data, unsigned width, unsigned height);

RETRO_END_DECLS

#endif
