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

#ifndef __WAYLAND_BEAM_H
#define __WAYLAND_BEAM_H

#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* The latest wp_presentation_feedback.presented of the context's
 * surface, for the display server's beam estimate */
typedef struct wl_beam
{
   uint64_t line0_ns;   /* when line 0 of the presented frame started */
   uint64_t refresh_ns; /* 0: the compositor did not say */
   uint32_t flags;      /* wp_presentation_feedback_kind bits */
   int      clock_id;
   char     output[32]; /* the synced output's connector, "" unknown */
} wl_beam_t;

/* One writer at a time: the thread that presents frames, the only one
 * that dispatches presentation feedback, and whichever thread tears
 * the context down after it. Read lock-free from the frame path
 * through a seqlock. */
void wl_beam_publish(const wl_beam_t *t);
void wl_beam_reset(void);
/* False until a frame has been presented, and after a reset */
bool wl_beam_get(wl_beam_t *out);

RETRO_END_DECLS

#endif
