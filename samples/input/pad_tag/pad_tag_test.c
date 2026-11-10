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

/* input/input_pad_tag.h: the list must stay exactly as it is (a pad
 * keeps its word only while it does), no two words may look alike, and
 * the hash is pinned to FNV-1a's published values and to the
 * identities samples/menu/pad_tag_rows expects. */

#include <stdio.h>
#include <string.h>

#include "input_pad_tag.h"

static unsigned failures;

static void check(int ok, const char *what, const char *a, const char *b)
{
   if (ok)
      return;
   fprintf(stderr, "FAIL: %s%s%s%s%s\n", what,
         a ? ": " : "", a ? a : "", b ? " " : "", b ? b : "");
   failures++;
}

static char low(char c)
{
   return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int same_from(const char *a, const char *b)
{
   for (; *a && *b; a++, b++)
      if (low(*a) != low(*b))
         return 0;
   return !*a && !*b;
}

/* Whether @a is @b, or becomes it by one letter replaced, added or
 * taken away, or two side by side swapped. */
static int one_edit(const char *a, const char *b)
{
   size_t i  = 0;
   size_t la = strlen(a);
   size_t lb = strlen(b);
   if (la < lb)
   {
      const char *t = a;
      size_t n      = la;
      a             = b;
      b             = t;
      la            = lb;
      lb            = n;
   }
   if (la - lb > 1)
      return 0;
   while (i < lb && low(a[i]) == low(b[i]))
      i++;
   if (i == lb)
      return 1;
   if (la > lb)
      return same_from(a + i + 1, b + i);
   if (same_from(a + i + 1, b + i + 1))
      return 1;
   return i + 1 < la
       && low(a[i]) == low(b[i + 1])
       && low(a[i + 1]) == low(b[i])
       && same_from(a + i + 2, b + i + 2);
}

static void check_words(void)
{
   size_t i, j, k;
   uint32_t h = 0x811c9dc5UL;
   check(sizeof(input_pad_tag_words) / sizeof(input_pad_tag_words[0])
         == 256, "the list does not have 256 words", NULL, NULL);
   for (i = 0; i < 256; i++)
   {
      const char *w = input_pad_tag_words[i];
      size_t n      = strlen(w);
      int ok        = n >= 3 && n <= 6 && w[0] >= 'A' && w[0] <= 'Z';
      for (k = 1; k < n; k++)
         if (w[k] < 'a' || w[k] > 'z')
            ok = 0;
      check(ok, "not 3-6 ASCII letters, capital first", w, NULL);
      for (j = i + 1; j < 256; j++)
         check(!one_edit(w, input_pad_tag_words[j]),
               "the same or one letter apart", w, input_pad_tag_words[j]);
      h = input_pad_tag_fnv1a(h, (const unsigned char*)w, n + 1);
   }
   /* every word and its order: a change here changes pads' tags */
   check(h == 0x5e65f367UL, "the list changed (checksum)", NULL, NULL);
}

static void check_hash(void)
{
   static const unsigned char foobar[] = "foobar";
   check(input_pad_tag_fnv1a(0x811c9dc5UL, foobar, 0) == 0x811c9dc5UL,
         "FNV-1a of nothing", NULL, NULL);
   check(input_pad_tag_fnv1a(0x811c9dc5UL, (const unsigned char*)"a", 1)
         == 0xe40c292cUL, "FNV-1a of \"a\"", NULL, NULL);
   check(input_pad_tag_fnv1a(0x811c9dc5UL, foobar, 6) == 0xbf9cf968UL,
         "FNV-1a of \"foobar\"", NULL, NULL);
}

static void check_ident(unsigned vid, unsigned pid, const char *uniq,
      const char *phys, unsigned long want, const char *word)
{
   uint32_t got = input_pad_tag_ident(vid, pid, uniq, phys);
   check(got == want, "identity", uniq, phys);
   if (word)
      check(!strcmp(input_pad_tag_word(got), word), "word", word,
            input_pad_tag_word(got));
}

int main(void)
{
   check_words();
   check_hash();
   /* the port path when the pad has no serial */
   check_ident(0x045e, 0x028e, "", "port-1",   0x8c6c6606UL, "Lizard");
   check_ident(0x045e, 0x028e, "", "port-2",   0x8b6c6473UL, "Wagon");
   /* a serial wins, whichever port */
   check_ident(0x045e, 0x028e, "serial-9", "port-1", 0xa1e19035UL, "Tree");
   check_ident(0x045e, 0x028e, "serial-9", "port-2", 0xa1e19035UL, "Tree");
   /* two identities, one word: the menu numbers them */
   check_ident(0x045e, 0x028e, "", "port-11",  0x45a4e895UL, "Parcel");
   check_ident(0x045e, 0x028e, "", "port-22",  0xbca1d253UL, "Parcel");
   /* as udev reports a wired pad and a Bluetooth one */
   check_ident(0x045e, 0x028e, "", "usb-0000:00:14.0-1/input0",
         0x82d1f4b8UL, "Cabin");
   check_ident(0x054c, 0x0ce6, "a0:5a:5c:12:34:56", "f4:4e:fc:00:00:01",
         0xb57c2e0eUL, "Tuna");
   /* no identity */
   check_ident(0x045e, 0x028e, NULL, "port-1", 0, NULL);
   check_ident(0x045e, 0x028e, "", NULL,       0, NULL);
   check_ident(0x045e, 0x028e, "", "",         0, NULL);

   if (failures)
   {
      fprintf(stderr, "pad_tag_test: %u failure(s)\n", failures);
      return 1;
   }
   printf("pad_tag_test: 256 words, none alike; identities pinned\n");
   return 0;
}
