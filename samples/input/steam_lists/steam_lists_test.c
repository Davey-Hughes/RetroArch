/* steam_lists_test.c -- the controllers Steam asks SDL to hide.
 *
 * Under Steam, Steam Input mirrors each controller it serves as a
 * virtual pad (28de:11ff) and lists the real ones in
 * SDL_GAMECONTROLLER_IGNORE_DEVICES, so that SDL programs see each
 * controller once. A joypad driver that reads evdev itself sees both,
 * and one controller drives two players.
 *
 * The claims, as SDL2's SDL_ShouldIgnoreGameController and SDL3's
 * SDL_ShouldIgnoreGamepad decide:
 *
 *   1. A pad on IGNORE is hidden; one not on it is kept.
 *   2. The lists are SDL's parse: hex digits in either case, any
 *      separator, unpadded numbers; only a lower-case "0x" starts a
 *      number; a vid with no pid after it is no entry.
 *   3. EXCEPT with an entry hides every pad it does not name, and
 *      IGNORE is not read. EXCEPT with no entry is no list.
 *   4. "@path" names a file that holds the list. A file that cannot be
 *      read is a list with no entry.
 *   5. Steam's own pad is kept only when ALLOW is true as SDL reads it
 *      (unset, empty, "0..." and "false" in any case are false), and
 *      neither list touches it.
 *
 * The decision is the driver's own, from steam_lists.h. */

#include <stdio.h>
#include <string.h>

#include <boolean.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "steam_lists.h"

static unsigned failures = 0;

#define CHECK(cond, msg) \
   do { \
      if (!(cond)) \
      { \
         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
         failures++; \
      } \
   } while (0)

#define FILES        "steam_lists_test_files"
#define EXCEPT_FILE  FILES "/except.txt"
#define IGNORE_FILE  FILES "/ignore.txt"
#define EMPTY_FILE   FILES "/empty.txt"
#define MISSING_FILE FILES "/missing.txt"

#define ALW "SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD"
#define EXC "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"
#define IGN "SDL_GAMECONTROLLER_IGNORE_DEVICES"

static const char except_text[] = "0x045e/0x028e\n0x28de/0x11ff\n";
static const char ignore_text[] = "0x045e/0x028e\r\n";

struct verdict
{
   const char *what;
   unsigned vid;
   unsigned pid;
   const char *allow;
   const char *except;
   const char *ignore;
   const char *want;   /* the variable that hides the pad, or NULL */
};

static const struct verdict verdicts[] = {
   /* 1 */
   { "on IGNORE",              0x045e, 0x028e, NULL, NULL, "0x045e/0x028e", IGN },
   { "not on IGNORE",          0x054c, 0x09cc, NULL, NULL, "0x045e/0x028e", NULL },
   { "on a list of several",   0x054c, 0x09cc, NULL, NULL,
      "0x045e/0x028e,0x054c/0x09cc,0x057e/0x2009",                          IGN },
   { "nothing set",            0x045e, 0x028e, NULL, NULL, NULL,            NULL },
   { "IGNORE empty",           0x045e, 0x028e, NULL, NULL, "",              NULL },
   /* 2 */
   { "hex digits in upper case", 0x2dc8, 0x6003, NULL, NULL, "0x2DC8/0x6003", IGN },
   { "any separator",          0x054c, 0x09cc, NULL, NULL,
      "0x045e:0x028e; 0x054c 0x09cc",                                       IGN },
   { "unpadded numbers",       0x045e, 0x028e, NULL, NULL, "0x45e/0x28e",   IGN },
   { "\"0X\" is not a number", 0x045e, 0x028e, NULL, NULL, "0X045e/0X028e", NULL },
   { "a vid with no pid",      0x045e, 0x0000, NULL, NULL,
      "0x054c/0x09cc,0x045e",                                               NULL },
   /* 3 */
   { "EXCEPT wins over IGNORE", 0x045e, 0x028e, NULL, "0x045e/0x028e",
      "0x045e/0x028e",                                                      NULL },
   { "not on EXCEPT",          0x054c, 0x09cc, NULL, "0x045e/0x028e", NULL, EXC },
   { "EXCEPT of Steam's pad only", 0x045e, 0x028e, NULL, "0x28de/0x11ff",
      NULL,                                                                 EXC },
   { "EXCEPT with no entry",   0x045e, 0x028e, NULL, "none", "0x045e/0x028e", IGN },
   { "EXCEPT empty",           0x045e, 0x028e, NULL, "", "0x045e/0x028e",   IGN },
   /* 4 */
   { "EXCEPT from a file",     0x054c, 0x09cc, NULL, "@" EXCEPT_FILE, NULL, EXC },
   { "on EXCEPT from a file",  0x045e, 0x028e, NULL, "@" EXCEPT_FILE,
      "0x045e/0x028e",                                                      NULL },
   { "IGNORE from a file",     0x045e, 0x028e, NULL, NULL, "@" IGNORE_FILE, IGN },
   { "EXCEPT file unreadable", 0x045e, 0x028e, NULL, "@" MISSING_FILE,
      "0x045e/0x028e",                                                      IGN },
   { "EXCEPT file empty",      0x045e, 0x028e, NULL, "@" EMPTY_FILE,
      "0x045e/0x028e",                                                      IGN },
   { "IGNORE file unreadable", 0x045e, 0x028e, NULL, NULL, "@" MISSING_FILE, NULL },
   /* 5 */
   { "ALLOW unset",            0x28de, 0x11ff, NULL,    NULL, NULL, ALW },
   { "ALLOW empty",            0x28de, 0x11ff, "",      NULL, NULL, ALW },
   { "ALLOW 0",                0x28de, 0x11ff, "0",     NULL, NULL, ALW },
   { "ALLOW 00",               0x28de, 0x11ff, "00",    NULL, NULL, ALW },
   { "ALLOW false",            0x28de, 0x11ff, "false", NULL, NULL, ALW },
   { "ALLOW FALSE",            0x28de, 0x11ff, "FALSE", NULL, NULL, ALW },
   { "ALLOW 1",                0x28de, 0x11ff, "1",     NULL, NULL, NULL },
   { "ALLOW true",             0x28de, 0x11ff, "true",  NULL, NULL, NULL },
   { "ALLOW TRUE",             0x28de, 0x11ff, "TRUE",  NULL, NULL, NULL },
   { "ALLOW yes",              0x28de, 0x11ff, "yes",   NULL, NULL, NULL },
   { "Steam's pad on EXCEPT, ALLOW unset", 0x28de, 0x11ff, NULL,
      "0x28de/0x11ff", NULL,                                                ALW },
   { "Steam's pad on IGNORE, ALLOW true", 0x28de, 0x11ff, "1", NULL,
      "0x28de/0x11ff",                                                      NULL },
   { "Steam's pad not on EXCEPT, ALLOW true", 0x28de, 0x11ff, "1",
      "0x045e/0x028e", NULL,                                                NULL },
   /* only 11ff is Steam's own: the Deck's pad is an ordinary one */
   { "Steam Deck pad on IGNORE", 0x28de, 0x1205, "1", NULL,
      "0x28de/0x1205",                                                      IGN }
};

int main(void)
{
   size_t i;
   bool found = true;

   /* the files "@path" names, in a directory of the test's own */
   CHECK(path_mkdir(FILES) || path_is_directory(FILES),
         "fixture: cannot make " FILES);
   CHECK(filestream_write_file(EXCEPT_FILE, except_text,
            (int64_t)strlen(except_text)), "fixture: " EXCEPT_FILE);
   CHECK(filestream_write_file(IGNORE_FILE, ignore_text,
            (int64_t)strlen(ignore_text)), "fixture: " IGNORE_FILE);
   CHECK(filestream_write_file(EMPTY_FILE, "", 0), "fixture: " EMPTY_FILE);
   filestream_delete(MISSING_FILE);

   /* the parser's entry count */
   CHECK(steam_list_scan_string("0x054c/0x09cc,0x045e", 0x045e, 0,
            &found) == 1 && !found, "a vid with no pid made an entry");
   CHECK(steam_list_scan_string("0X045e/0X028e", 0x045e, 0x028e,
            &found) == 0 && !found, "\"0X\" started a number");
   CHECK(steam_list_scan_string("0x45e/0x28e", 0x045e, 0x028e,
            &found) == 1 && found, "an unpadded pair was not read");
   CHECK(steam_list_scan_string(NULL, 0, 0, &found) == 0 && !found,
         "an unset list has entries");
   CHECK(steam_list_scan("@" EXCEPT_FILE, 0x28de, 0x11ff, &found) == 2
         && found, "the file's two entries were not read");
   CHECK(steam_list_scan("@" MISSING_FILE, 0, 0, &found) == 0 && !found,
         "an unreadable file has entries");

   for (i = 0; i < sizeof(verdicts) / sizeof(verdicts[0]); i++)
   {
      const struct verdict *v = &verdicts[i];
      const char *got         = steam_lists_skip((uint16_t)v->vid,
            (uint16_t)v->pid, v->allow, v->except, v->ignore);

      if (v->want ? !got || !string_is_equal(got, v->want) : got != NULL)
      {
         fprintf(stderr, "FAIL: %s (%04x:%04x): %s, want %s\n", v->what,
               v->vid, v->pid, got ? got : "kept",
               v->want ? v->want : "kept");
         failures++;
      }
   }

   filestream_delete(EXCEPT_FILE);
   filestream_delete(IGNORE_FILE);
   filestream_delete(EMPTY_FILE);
   CHECK(path_rmdir(FILES), "fixture: cannot remove " FILES);

   if (failures)
   {
      fprintf(stderr, "FAIL steam_lists_test: %u failures\n", failures);
      return 1;
   }
   fprintf(stderr, "PASS steam_lists_test (%u cases)\n",
         (unsigned)(sizeof(verdicts) / sizeof(verdicts[0])));
   return 0;
}
