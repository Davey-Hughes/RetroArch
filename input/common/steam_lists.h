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

#ifndef _STEAM_LISTS_H
#define _STEAM_LISTS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <retro_inline.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

/* Under Steam, Steam Input mirrors each controller it serves as a
 * virtual pad and lists the real ones in the environment for SDL to
 * hide. These are SDL's rules for those variables
 * (SDL_LoadVIDPIDListFromHint, SDL_ShouldIgnoreGamepad), for joypad
 * drivers that do not go through SDL.
 *
 * Kept free of libudev and evdev types, so that samples/input/steam_lists
 * can build it on its own. */

/* Each lower-case "0x" starts a number; a vid and the next number make
 * an entry, and what lies between is not read. Returns the entries. */
static INLINE unsigned steam_list_scan_string(const char *s,
      uint16_t vid, uint16_t pid, bool *found)
{
   unsigned entries = 0;

   *found = false;
   while (s && (s = strstr(s, "0x")))
   {
      char *end;
      uint16_t v = (uint16_t)strtol(s, &end, 0);
      uint16_t p;

      if (!(s = strstr(end, "0x")))
         break;
      p = (uint16_t)strtol(s, &end, 0);
      s = end;
      entries++;
      if (v == vid && p == pid)
         *found = true;
   }
   return entries;
}

/* A variable's value: "@path" names a file that holds the list. */
static INLINE unsigned steam_list_scan(const char *value,
      uint16_t vid, uint16_t pid, bool *found)
{
   unsigned entries;
   void *buf = NULL;

   if (!value || *value != '@')
      return steam_list_scan_string(value, vid, pid, found);
   *found = false;
   if (!filestream_read_file(value + 1, &buf, NULL))
      return 0;
   entries = steam_list_scan_string((const char*)buf, vid, pid, found);
   free(buf);
   return entries;
}

/* NULL to keep the pad, else the variable that hides it. */
static INLINE const char *steam_lists_skip(uint16_t vid, uint16_t pid,
      const char *allow, const char *except, const char *ignore)
{
   bool found;

   /* Steam's own pad, live only where Steam launched the program;
    * ALLOW read as SDL_GetStringBoolean reads it */
   if (vid == 0x28de && pid == 0x11ff)
   {
      if (     string_is_empty(allow)
            || *allow == '0'
            || string_is_equal_case_insensitive(allow, "false"))
         return "SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD";
      return NULL;
   }
   if (steam_list_scan(except, vid, pid, &found))
      return found ? NULL : "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT";
   steam_list_scan(ignore, vid, pid, &found);
   return found ? "SDL_GAMECONTROLLER_IGNORE_DEVICES" : NULL;
}

static INLINE const char *steam_lists_skip_env(uint16_t vid, uint16_t pid)
{
   return steam_lists_skip(vid, pid,
         getenv("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD"),
         getenv("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"),
         getenv("SDL_GAMECONTROLLER_IGNORE_DEVICES"));
}

#endif
