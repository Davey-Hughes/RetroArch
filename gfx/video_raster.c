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

#include <string.h>

#include <libretro.h>

#include "video_raster.h"
#include "../verbosity.h"

void video_raster_reset(video_raster_t *raster)
{
   memset(raster, 0, sizeof(*raster));
}

enum video_raster_violation video_raster_poll(video_raster_t *raster,
      const void *data, unsigned width, unsigned height, unsigned row)
{
   if (raster->flags & VIDEO_RASTER_FLAG_BROKEN)
      return VIDEO_RASTER_OK;

   if (!data || data == RETRO_HW_FRAME_BUFFER_VALID)
   {
      if (!(raster->flags & VIDEO_RASTER_FLAG_WARNED_DATA))
         RARCH_WARN("[Video] Raster poll without a software frame.\n");
      raster->flags |= VIDEO_RASTER_FLAG_WARNED_DATA
                     | VIDEO_RASTER_FLAG_BROKEN;
      return VIDEO_RASTER_BAD_DATA;
   }

   if (row >= height)
   {
      if (!(raster->flags & VIDEO_RASTER_FLAG_WARNED_ROW))
         RARCH_WARN("[Video] Raster poll for row %u of a frame %u rows tall.\n",
               row, height);
      raster->flags |= VIDEO_RASTER_FLAG_WARNED_ROW
                     | VIDEO_RASTER_FLAG_BROKEN;
      return VIDEO_RASTER_BAD_ROW;
   }

   if (raster->calls)
   {
      if (width != raster->width || height != raster->height)
      {
         if (!(raster->flags & VIDEO_RASTER_FLAG_WARNED_SIZE))
            RARCH_WARN("[Video] Raster poll for %ux%u within a %ux%u frame.\n",
                  width, height, raster->width, raster->height);
         raster->flags |= VIDEO_RASTER_FLAG_WARNED_SIZE
                        | VIDEO_RASTER_FLAG_BROKEN;
         return VIDEO_RASTER_BAD_SIZE;
      }

      if (row <= raster->last_row)
      {
         if (!(raster->flags & VIDEO_RASTER_FLAG_WARNED_ORDER))
            RARCH_WARN("[Video] Raster poll for row %u after row %u.\n",
                  row, raster->last_row);
         raster->flags |= VIDEO_RASTER_FLAG_WARNED_ORDER
                        | VIDEO_RASTER_FLAG_BROKEN;
         return VIDEO_RASTER_BAD_ORDER;
      }
   }
   else
   {
      raster->width     = width;
      raster->height    = height;
      raster->first_row = row;
   }

   raster->last_row = row;
   raster->calls++;
   return VIDEO_RASTER_OK;
}

enum video_raster_violation video_raster_frame_end(video_raster_t *raster,
      const void *data, unsigned width, unsigned height)
{
   enum video_raster_violation violation = VIDEO_RASTER_OK;

   /* A dupe (NULL) ends the frame without judging it */
   if (     raster->calls
         && data
         && !(raster->flags & VIDEO_RASTER_FLAG_BROKEN))
   {
      if (data == RETRO_HW_FRAME_BUFFER_VALID)
      {
         if (!(raster->flags & VIDEO_RASTER_FLAG_WARNED_DATA))
            RARCH_WARN("[Video] Raster poll without a software frame.\n");
         raster->flags |= VIDEO_RASTER_FLAG_WARNED_DATA;
         violation      = VIDEO_RASTER_BAD_DATA;
      }
      else if (width != raster->width || height != raster->height)
      {
         if (!(raster->flags & VIDEO_RASTER_FLAG_WARNED_SIZE))
            RARCH_WARN("[Video] Raster polls for %ux%u, frame presented at %ux%u.\n",
                  raster->width, raster->height, width, height);
         raster->flags |= VIDEO_RASTER_FLAG_WARNED_SIZE;
         violation      = VIDEO_RASTER_BAD_SIZE;
      }
      else if (!(raster->flags & VIDEO_RASTER_FLAG_LOGGED))
      {
         RARCH_LOG("[Video] Core polls the raster: %u calls for %ux%u, rows %u-%u.\n",
               raster->calls, width, height,
               raster->first_row, raster->last_row);
         raster->flags |= VIDEO_RASTER_FLAG_LOGGED;
      }
   }

   raster->width     = 0;
   raster->height    = 0;
   raster->calls     = 0;
   raster->first_row = 0;
   raster->last_row  = 0;
   raster->flags    &= ~VIDEO_RASTER_FLAG_BROKEN;
   return violation;
}
