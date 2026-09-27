/* Copyright  (C) 2010-2026 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (wayland_beam_test.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* The presentation timing in gfx/common/wayland_beam.c: nothing
 * before the first frame or after a reset, a frame reads back whole,
 * and a reader racing the writer never sees half of one. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "../../../gfx/common/wayland_beam.h"

#define ROUNDS 400000

static int fails;
static retro_atomic_int_t writer_done;

static void check(const char *what, int ok)
{
   printf("[%s] %s\n", ok ? "pass" : "FAIL", what);
   if (!ok)
      fails++;
}

/* Every field of round r encodes r, both halves of the 64-bit ones
 * included, so a mixed read shows */
static void fill(wl_beam_t *t, uint32_t r)
{
   memset(t, 0, sizeof(*t));
   t->line0_ns   = ((uint64_t)r << 32) | r;
   t->refresh_ns = (uint64_t)r * 1000 + 7;
   t->flags      = r;
   t->clock_id   = (int)r;
   snprintf(t->output, sizeof(t->output), "DP-%u", r);
}

static int consistent(const wl_beam_t *t)
{
   char name[32];
   uint32_t r = t->flags;
   snprintf(name, sizeof(name), "DP-%u", r);
   return t->line0_ns   == (((uint64_t)r << 32) | r)
       && t->refresh_ns == (uint64_t)r * 1000 + 7
       && t->clock_id   == (int)r
       && !strcmp(t->output, name);
}

/* The presenting thread is the only writer */
static void writer(void *arg)
{
   uint32_t r;
   wl_beam_t t;
   (void)arg;
   for (r = 1; r <= ROUNDS; r++)
   {
      fill(&t, r);
      wl_beam_publish(&t);
   }
   retro_atomic_inc_int(&writer_done);
}

int main(void)
{
   wl_beam_t t;
   sthread_t *th;
   unsigned reads = 0;
   unsigned torn  = 0;

   check("nothing before the first frame", !wl_beam_get(&t));

   fill(&t, 5);
   wl_beam_publish(&t);
   memset(&t, 0, sizeof(t));
   check("a published frame reads back whole",
         wl_beam_get(&t) && consistent(&t) && t.flags == 5);

   memset(&t, 0, sizeof(t));
   t.line0_ns   = 1;
   t.refresh_ns = 1;
   memset(t.output, 'x', sizeof(t.output));
   wl_beam_publish(&t);
   check("an unterminated name comes back terminated",
         wl_beam_get(&t)
         && strlen(t.output) == sizeof(t.output) - 1);

   wl_beam_reset();
   check("nothing after a reset", !wl_beam_get(&t));

   if (!(th = sthread_create(writer, NULL)))
   {
      puts("FAIL: no writer thread");
      return 1;
   }
   while (!retro_atomic_load_acquire_int(&writer_done))
   {
      if (wl_beam_get(&t))
      {
         reads++;
         if (!consistent(&t))
            torn++;
      }
   }
   sthread_join(th);
   printf("%u reads while the writer ran, %u torn\n", reads, torn);
   check("no read saw half a frame", reads > 0 && torn == 0);

   puts(fails ? "FAILED" : "ALL OK");
   return fails != 0;
}
