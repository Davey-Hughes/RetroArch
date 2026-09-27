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

#include <retro_atomic.h>

#include "wayland_beam.h"

/* The payload as int-sized atomics: valid, line 0 and refresh as
 * low/high halves, flags, clock, then the name */
#define WL_BEAM_VALID   0
#define WL_BEAM_LINE0   1
#define WL_BEAM_REFRESH 3
#define WL_BEAM_FLAGS   5
#define WL_BEAM_CLOCK   6
#define WL_BEAM_OUTPUT  7
#define WL_BEAM_WORDS   (WL_BEAM_OUTPUT + 8)

static retro_atomic_int_t wl_beam_seq;
static retro_atomic_int_t wl_beam_words[WL_BEAM_WORDS];

static void wl_beam_store(const uint32_t *u)
{
   int i;
   /* Unsigned: the count wraps after months of frames */
   unsigned seq = (unsigned)retro_atomic_load_relaxed_int(&wl_beam_seq);

   retro_atomic_store_relaxed_int(&wl_beam_seq, (int)(seq + 1));
   retro_atomic_thread_fence_release();
   for (i = 0; i < WL_BEAM_WORDS; i++)
   {
      int v;
      memcpy(&v, &u[i], sizeof(v));
      retro_atomic_store_relaxed_int(&wl_beam_words[i], v);
   }
   retro_atomic_store_release_int(&wl_beam_seq, (int)(seq + 2));
}

void wl_beam_publish(const wl_beam_t *t)
{
   uint32_t u[WL_BEAM_WORDS];

   u[WL_BEAM_VALID]       = 1;
   u[WL_BEAM_LINE0]       = (uint32_t)t->line0_ns;
   u[WL_BEAM_LINE0 + 1]   = (uint32_t)(t->line0_ns >> 32);
   u[WL_BEAM_REFRESH]     = (uint32_t)t->refresh_ns;
   u[WL_BEAM_REFRESH + 1] = (uint32_t)(t->refresh_ns >> 32);
   u[WL_BEAM_FLAGS]       = t->flags;
   u[WL_BEAM_CLOCK]       = (uint32_t)t->clock_id;
   memcpy(&u[WL_BEAM_OUTPUT], t->output, sizeof(t->output));
   wl_beam_store(u);
}

void wl_beam_reset(void)
{
   uint32_t u[WL_BEAM_WORDS];
   memset(u, 0, sizeof(u));
   wl_beam_store(u);
}

bool wl_beam_get(wl_beam_t *out)
{
   uint32_t u[WL_BEAM_WORDS];
   int i;

   for (;;)
   {
      int s1 = retro_atomic_load_acquire_int(&wl_beam_seq);
      if (s1 & 1)
         continue;
      for (i = 0; i < WL_BEAM_WORDS; i++)
      {
         int v = retro_atomic_load_relaxed_int(&wl_beam_words[i]);
         memcpy(&u[i], &v, sizeof(v));
      }
      retro_atomic_thread_fence_acquire();
      if (retro_atomic_load_relaxed_int(&wl_beam_seq) == s1)
         break;
   }

   if (!u[WL_BEAM_VALID])
      return false;

   out->line0_ns   = (uint64_t)u[WL_BEAM_LINE0]
                   | ((uint64_t)u[WL_BEAM_LINE0 + 1] << 32);
   out->refresh_ns = (uint64_t)u[WL_BEAM_REFRESH]
                   | ((uint64_t)u[WL_BEAM_REFRESH + 1] << 32);
   out->flags      = u[WL_BEAM_FLAGS];
   out->clock_id   = (int)u[WL_BEAM_CLOCK];
   memcpy(out->output, &u[WL_BEAM_OUTPUT], sizeof(out->output));
   out->output[sizeof(out->output) - 1] = '\0';
   return true;
}
