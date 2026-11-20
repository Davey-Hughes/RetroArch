/* Copyright  (C) 2010-2026 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (retro_seqlatch.h).
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

#ifndef __LIBRETRO_SDK_SEQLATCH_H
#define __LIBRETRO_SDK_SEQLATCH_H

/* retro_seqlatch.h - a record one thread publishes and any thread reads,
 * neither ever waiting on the other.
 *
 * A latched seqlock: the record is kept as two copies of its words.  The
 * writer stamps odd and stores copy 0, then stamps even and stores copy
 * 1.  A reader reads the copy the stamp says is not being stored, so a
 * writer stopped part way holds no reader up; it starts over only when
 * the writer got through a whole copy during the read.
 *
 * Pick it over retro_seqlock.h when a reader must never wait on a
 * writer, or when every access must be atomic: race detectors stay quiet
 * here, where they flag retro_seqlock.h's plain record.
 *
 *   retro_atomic_int_t seq;
 *   retro_atomic_int_t words[2 * RETRO_SEQLATCH_WORDS(sizeof(status))];
 *
 *   writer:  retro_seqlatch_publish(&seq, words, &status, sizeof(status));
 *   reader:  retro_seqlatch_read(&seq, words, &copy, sizeof(copy));
 *
 * Rules:
 *   - One writer at a time.
 *   - words holds 2 * RETRO_SEQLATCH_WORDS(len) entries, len being the
 *     record's size, the same on every call.
 *
 * Header only.  Needs lock-free atomics (RETRO_ATOMIC_LOCK_FREE) to be
 * of use between threads.
 */

#include <stddef.h>
#include <string.h>

#include <retro_atomic.h>
#include <retro_inline.h>

#define RETRO_SEQLATCH_WORDS(size) (((size) + sizeof(int) - 1) / sizeof(int))

/* Writer: stores len bytes of src as the record. */
static INLINE void retro_seqlatch_publish(retro_atomic_int_t *seq,
      retro_atomic_int_t *words, const void *src, size_t len)
{
   size_t n = RETRO_SEQLATCH_WORDS(len);
   int s    = retro_atomic_load_relaxed_int(seq);
   unsigned c;

   /* Odd, then copy 0; even, then copy 1. */
   for (c = 1; c <= 2; c++)
   {
      size_t i;
      retro_atomic_int_t *copy = words + (c - 1) * n;
      retro_atomic_store_release_int(seq, (int)((unsigned)s + c));
      retro_atomic_thread_fence_release();
      for (i = 0; i < n; i++)
      {
         int w      = 0;
         size_t off = i * sizeof(int);
         memcpy(&w, (const char*)src + off,
               len - off < sizeof(int) ? len - off : sizeof(int));
         retro_atomic_store_relaxed_int(&copy[i], w);
      }
   }
}

/* Reader: copies the record into dst, which it may write more than once.
 * @return the stamp the copy was read under. */
static INLINE int retro_seqlatch_read(retro_atomic_int_t *seq,
      retro_atomic_int_t *words, void *dst, size_t len)
{
   size_t n = RETRO_SEQLATCH_WORDS(len);
   for (;;)
   {
      size_t i;
      int s                    = retro_atomic_load_acquire_int(seq);
      retro_atomic_int_t *copy = words + ((s & 1) ? n : 0);
      for (i = 0; i < n; i++)
      {
         int w      = retro_atomic_load_relaxed_int(&copy[i]);
         size_t off = i * sizeof(int);
         memcpy((char*)dst + off, &w,
               len - off < sizeof(int) ? len - off : sizeof(int));
      }
      retro_atomic_thread_fence_acquire();
      if (retro_atomic_load_relaxed_int(seq) == s)
         return s;
   }
}

#endif
