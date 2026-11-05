/* save_backups.c: where safety copies go, how they are named and read
 * back, same-second numbers, the identical-copy skip, retention and
 * the recovery-file chain. Included with its clock, its time format
 * and its two writes set here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <file/file_path.h>
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <retro_miscellaneous.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <time/rtime.h>

static struct tm test_tm;

static struct tm *test_localtime(const time_t *timep, struct tm *result)
{
   (void)timep;
   *result = test_tm;
   return result;
}

static bool strftime_fails;

static size_t test_strftime(char *s, size_t max, const char *format,
      const struct tm *tm)
{
   if (!strftime_fails)
      return strftime(s, max, format, tm);
   *s = '\0';
   return 0;
}

static unsigned plain_writes, synced_writes;

static bool test_write(const char *path, const void *data, int64_t len)
{
   plain_writes++;
   return filestream_write_file_atomic(path, data, len);
}

static bool test_write_synced(const char *path, const void *data,
      int64_t len)
{
   synced_writes++;
   return filestream_write_file_atomic_synced(path, data, len);
}

#define rtime_localtime test_localtime
#define strftime test_strftime
#define filestream_write_file_atomic test_write
#define filestream_write_file_atomic_synced test_write_synced
#include "../../../save_backups.c"
#undef rtime_localtime
#undef strftime
#undef filestream_write_file_atomic
#undef filestream_write_file_atomic_synced

#define ROOT "save_backups_test_files"

static int failures;

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void set_time(int hour, int min, int sec)
{
   memset(&test_tm, 0, sizeof(test_tm));
   test_tm.tm_year = 126;
   test_tm.tm_mon  = 10;
   test_tm.tm_mday = 3;
   test_tm.tm_hour = hour;
   test_tm.tm_min  = min;
   test_tm.tm_sec  = sec;
}

/* Equal but for which slash separates folders. */
static bool same_path(const char *a, const char *b)
{
   for (; *a && *b; a++, b++)
      if (*a != *b && !(PATH_CHAR_IS_SLASH(*a) && PATH_CHAR_IS_SLASH(*b)))
         return false;
   return !*a && !*b;
}

static void rm_tree(const char *dir)
{
   size_t i;
   struct string_list *l = dir_list_new(dir, NULL, true, true, false, false);
   if (l)
   {
      for (i = 0; i < l->size; i++)
      {
         if (path_is_directory(l->elems[i].data))
            rm_tree(l->elems[i].data);
         else
            filestream_delete(l->elems[i].data);
      }
      string_list_free(l);
   }
   path_rmdir(dir);
}

static size_t count_files(const char *dir)
{
   size_t n;
   struct string_list *l = dir_list_new(dir, NULL, false, true, false, false);
   n = l ? l->size : 0;
   if (l)
      string_list_free(l);
   return n;
}

static bool holds(const char *path, const char *text)
{
   return filestream_matches_buf(path, text, strlen(text));
}

static void put(const char *path, const char *text)
{
   char dir[PATH_MAX_LENGTH];
   fill_pathname_basedir(dir, path, sizeof(dir));
   path_mkdir(dir);
   CHECK(filestream_write_file(path, text, (int64_t)strlen(text)));
}

static void roots(void)
{
   save_backups_set_roots(ROOT "/saves", ROOT "/states", ROOT "/roms",
         ROOT "/system", ROOT "/config", ROOT "/appdata", "Snes9x");
}

static void backup_folders(void)
{
   char s[PATH_MAX_LENGTH];
   roots();
   CHECK(save_backups_dir(s, sizeof(s), ROOT "/saves/Snes9x/Game.srm"));
   CHECK(same_path(s, ROOT "/saves/backups/Snes9x"));
   save_backups_dir(s, sizeof(s), ROOT "/saves/Game.srm");
   CHECK(same_path(s, ROOT "/saves/backups"));
   save_backups_dir(s, sizeof(s), ROOT "/states/Snes9x/Game.state.auto");
   CHECK(same_path(s, ROOT "/states/backups/Snes9x"));
   save_backups_dir(s, sizeof(s), ROOT "/roms/Game.srm");
   CHECK(same_path(s, ROOT "/roms/backups"));
   save_backups_dir(s, sizeof(s), ROOT "/system/pcsx2/memcards/Mcd001.ps2");
   CHECK(same_path(s, ROOT "/saves/backups/system/pcsx2/memcards"));
   save_backups_dir(s, sizeof(s), ROOT "/override/x/Game.srm");
   CHECK(same_path(s, ROOT "/override/x/backups"));
   /* a name that only starts like a root is not under it */
   save_backups_dir(s, sizeof(s), ROOT "/saves2/Game.srm");
   CHECK(same_path(s, ROOT "/saves2/backups"));
   CHECK(!save_backups_dir(s, sizeof(s), ""));

   /* the most specific root wins */
   save_backups_set_roots(ROOT "/saves", ROOT "/saves/states", ROOT "/roms",
         ROOT "/system", NULL, NULL, "Snes9x");
   save_backups_dir(s, sizeof(s), ROOT "/saves/states/Game.state");
   CHECK(same_path(s, ROOT "/saves/states/backups"));

   /* no save folder: saves are in the content folder */
   save_backups_set_roots("", ROOT "/states", ROOT "/roms",
         ROOT "/system", NULL, NULL, "Snes9x");
   save_backups_dir(s, sizeof(s), ROOT "/roms/Game.srm");
   CHECK(same_path(s, ROOT "/roms/backups"));
   save_backups_dir(s, sizeof(s), ROOT "/system/x.bin");
   CHECK(same_path(s, ROOT "/roms/backups/system"));
}

/* A system file with no save folder: its copies stay beside it. */
static void backup_system_fallback(void)
{
   save_backups_set_roots(NULL, ROOT "/fstates", NULL, ROOT "/fsystem",
         NULL, NULL, "Snes9x");
   set_time(18, 0, 0);
   put(ROOT "/fsystem/pcsx2/Mcd001.ps2", "card");
   CHECK(save_backups_copy_file(ROOT "/fsystem/pcsx2/Mcd001.ps2",
         SAVE_BACKUP_CUT, true) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/fsystem/pcsx2/backups/"
         "Mcd001 - 2026-11-03 18-00-00 - Snes9x - cut.ps2", "card"));
   CHECK(count_files(ROOT "/fsystem/pcsx2/backups") == 1);
}

static void backup_names(void)
{
   size_t i, n;
   struct save_backup_name p;
   char game[400];
   static char path[PATH_MAX_LENGTH], dir[PATH_MAX_LENGTH];
   struct string_list *l;
   const char *zelda =
         "Zelda - Part 2 - 2026-11-03 10-00-00 - Snes9x - core-changed.mcd";
   roots();
   set_time(10, 0, 0);

   CHECK(save_backups_copy_image(ROOT "/saves/Snes9x/Game.srm",
         SAVE_BACKUP_UNREADABLE, "a", 1, true) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/saves/backups/Snes9x/"
         "Game - 2026-11-03 10-00-00 - Snes9x - unreadable.srm", "a"));
   CHECK(save_backups_copy_image(ROOT "/states/Game.state.auto",
         SAVE_BACKUP_NOT_LOADED, "b", 1, true) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/states/backups/"
         "Game - 2026-11-03 10-00-00 - Snes9x - not-loaded.state.auto", "b"));
   CHECK(save_backups_copy_image(ROOT "/states/Game.state3.auto",
         SAVE_BACKUP_NOT_LOADED, "c", 1, true) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/states/backups/"
         "Game - 2026-11-03 10-00-00 - Snes9x - not-loaded.state3.auto", "c"));
   CHECK(save_backups_copy_image(ROOT "/saves/Zelda - Part 2.mcd",
         SAVE_BACKUP_CORE_CHANGED, "d", 1, true) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/saves/backups/"
         "Zelda - Part 2 - 2026-11-03 10-00-00 - Snes9x - core-changed.mcd", "d"));
   CHECK(save_backups_copy_to(ROOT "/saves/backups", ROOT "/saves/Game.srm",
         "- Mega: Drive - Plus", SAVE_BACKUP_CUT, "e", 1, true) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/saves/backups/"
         "Game - 2026-11-03 10-00-00 - _ Mega_ Drive-Plus - cut.srm", "e"));
   save_backups_set_roots(ROOT "/saves", NULL, NULL, NULL, NULL, NULL, NULL);
   CHECK(save_backups_copy_image(ROOT "/saves/Game.srm",
         SAVE_BACKUP_BLANKED, "f", 1, true) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/saves/backups/"
         "Game - 2026-11-03 10-00-00-2 - unknown - blanked.srm", "f"));
   roots();

   /* every name read back */
   l = dir_list_new(ROOT "/saves/backups", NULL, false, true, false, false);
   CHECK(l && l->size == 3);
   for (i = 0; l && i < l->size; i++)
      CHECK(save_backups_parse(path_basename(l->elems[i].data), &p));
   if (l)
      string_list_free(l);
   CHECK(save_backups_parse(zelda, &p));
   CHECK(p.game_len == 14 && p.number == 1);
   CHECK(!strcmp(p.time, "2026-11-03 10-00-00"));
   CHECK(p.core_len == 6 && !strncmp(zelda + p.core_off, "Snes9x", 6));
   CHECK(p.reason == SAVE_BACKUP_CORE_CHANGED);
   CHECK(!strcmp(zelda + p.ext_off, ".mcd"));
   CHECK(save_backups_parse(
         "Game - 2026-11-03 10-00-00-12 - Snes9x - deleted", &p));
   CHECK(p.number == 12 && p.reason == SAVE_BACKUP_DELETED && p.ext_off == 48);
   CHECK(save_backups_parse(
         "G - 2026-11-03 10-00-00 - cloud-sync - replaced.state.auto", &p));
   CHECK(p.reason == SAVE_BACKUP_REPLACED);
   CHECK(!save_backups_parse("Game.srm", &p));
   CHECK(!save_backups_parse("Game - 2026-11-03 10-00-00 - Snes9x - cut.srm.tmp", &p));
   CHECK(!save_backups_parse("Game - 2026-11-03 10-00-00 - Snes9x - cut.srm.prev", &p));
   CHECK(!save_backups_parse("Game - 2026-11-03 10-00-00 - Snes9x - Cut.srm", &p));
   CHECK(!save_backups_parse("Game - 2026-11-03 10-00-00 - Snes9x - cuts.srm", &p));
   CHECK(!save_backups_parse("Game - 2026-11-3 10-00-00 - Snes9x - cut.srm", &p));
   CHECK(!save_backups_parse("Game - 2026-11-03 10-00-00-1 - Snes9x - cut.srm", &p));
   CHECK(!save_backups_parse("Game - 2026-11-03 10-00-00-02 - Snes9x - cut.srm", &p));
   CHECK(!save_backups_parse("Game - 2026-11-03 10-00-00x - Snes9x - cut.srm", &p));
   CHECK(!save_backups_parse(" - Snes9x - cut.srm", &p));

   /* a long game is cut at a character, and only the game */
   game[0] = 'a';
   for (i = 0; i < 149; i++)
   {
      game[1 + 2 * i] = (char)0xC3;
      game[2 + 2 * i] = (char)0xA9;
   }
   strcpy(game + 299, "z.srm");
   fill_pathname_join(path, ROOT "/saves", game, sizeof(path));
   CHECK(save_backups_copy_image(path, SAVE_BACKUP_UNREADABLE, "g", 1, true)
         == SAVE_BACKUP_MADE);
   set_time(10, 0, 1);
   CHECK(save_backups_copy_image(path, SAVE_BACKUP_BEFORE_STATE_LOAD, "h", 1, true)
         == SAVE_BACKUP_MADE);
   l = dir_list_new(ROOT "/saves/backups", NULL, false, true, false, false);
   n = 0;
   for (i = 0; l && i < l->size; i++)
   {
      const char *name = path_basename(l->elems[i].data);
      if (name[0] != 'a')
         continue;
      n++;
      CHECK(strlen(name) < 200);
      CHECK(save_backups_parse(name, &p));
      CHECK(p.game_len == 135);
      CHECK(!memcmp(name, game, p.game_len));
      CHECK(((unsigned char)name[p.game_len - 1] & 0xC0) == 0x80);
      CHECK(!strcmp(name + p.ext_off, ".srm"));
   }
   CHECK(n == 2);
   if (l)
      string_list_free(l);

   /* the cut keeps trailing spaces and dots */
   memset(game, 'b', 134);
   memcpy(game + 134, " .", 2);
   memset(game + 136, 'c', 100);
   strcpy(game + 236, ".srm");
   fill_pathname_join(path, ROOT "/override", game, sizeof(path));
   CHECK(save_backups_copy_image(path, SAVE_BACKUP_CUT, "i", 1, true)
         == SAVE_BACKUP_MADE);
   game[136] = '\0';
   snprintf(dir, sizeof(dir), ROOT "/override/backups/%s"
         " - 2026-11-03 10-00-01 - Snes9x - cut.srm", game);
   CHECK(holds(dir, "i"));

   /* and steps back at most 3 bytes, so a cut game is within 3 bytes
    * of its limit (136 here) */
   memset(game, 'y', 132);
   strcpy(game + 132, " \xF0\x9F\x98\x80 tail");
   CHECK(save_backups_game_len(game, strlen(game), 6, 4) == 133);
   memset(game, 'd', 130);
   memset(game + 130, 0x80, 10);
   CHECK(save_backups_game_len(game, 140, 6, 4) == 133);
}

/* Every character the core field changes. */
static void backup_core_chars(void)
{
   set_time(17, 0, 0);
   CHECK(save_backups_copy_to(ROOT "/chars", ROOT "/chars/Game.srm",
         "a<b>c\"d/e\\f|g?h*i\001j\037k", SAVE_BACKUP_CUT, "1", 1, true)
         == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/chars/"
         "Game - 2026-11-03 17-00-00 - a_b_c_d_e_f_g_h_i_j_k - cut.srm", "1"));
   CHECK(save_backups_copy_to(ROOT "/chars", ROOT "/chars/Game.srm",
         "", SAVE_BACKUP_CUT, "2", 1, true) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/chars/"
         "Game - 2026-11-03 17-00-00-2 - unknown - cut.srm", "2"));
   CHECK(count_files(ROOT "/chars") == 2);
}

static void backup_collisions(void)
{
   char dir[] = ROOT "/collide/backups";
   save_backups_set_roots(ROOT "/collide", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   set_time(11, 0, 0);
   CHECK(save_backups_copy_image(ROOT "/collide/Game.srm",
         SAVE_BACKUP_CUT, "1", 1, true) == SAVE_BACKUP_MADE);
   CHECK(save_backups_copy_image(ROOT "/collide/Game.srm",
         SAVE_BACKUP_CUT, "2", 1, true) == SAVE_BACKUP_MADE);
   CHECK(save_backups_copy_image(ROOT "/collide/Game.srm",
         SAVE_BACKUP_CUT, "3", 1, true) == SAVE_BACKUP_MADE);
   CHECK(count_files(dir) == 3);
   CHECK(holds(ROOT "/collide/backups/Game - 2026-11-03 11-00-00 - Snes9x - cut.srm", "1"));
   CHECK(holds(ROOT "/collide/backups/Game - 2026-11-03 11-00-00-2 - Snes9x - cut.srm", "2"));
   CHECK(holds(ROOT "/collide/backups/Game - 2026-11-03 11-00-00-3 - Snes9x - cut.srm", "3"));
   /* one count per file and second, whatever the reason */
   CHECK(save_backups_copy_image(ROOT "/collide/Game.srm",
         SAVE_BACKUP_UNREADABLE, "4", 1, true) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/collide/backups/Game - 2026-11-03 11-00-00-4 - Snes9x - unreadable.srm", "4"));
   /* a name of another case, one file where case is not told apart,
    * is not written over */
   put(ROOT "/collide/backups/game - 2026-11-03 11-00-00-5 - Snes9x - cut.srm", "x");
   CHECK(save_backups_copy_image(ROOT "/collide/Game.srm",
         SAVE_BACKUP_CUT, "5", 1, true) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/collide/backups/game - 2026-11-03 11-00-00-5 - Snes9x - cut.srm", "x"));
   CHECK(   holds(ROOT "/collide/backups/Game - 2026-11-03 11-00-00-5 - Snes9x - cut.srm", "5")
         || holds(ROOT "/collide/backups/Game - 2026-11-03 11-00-00-6 - Snes9x - cut.srm", "5"));
   CHECK(count_files(dir) == 6);
}

static void backup_identical_skip(void)
{
   char dir[] = ROOT "/same/backups";
   save_backups_set_roots(ROOT "/same", NULL, NULL, NULL, NULL, NULL, "Snes9x");
   set_time(12, 0, 0);
   CHECK(save_backups_copy_image(ROOT "/same/Game.srm",
         SAVE_BACKUP_CUT, "AAA", 3, true) == SAVE_BACKUP_MADE);
   set_time(12, 0, 1);
   CHECK(save_backups_copy_image(ROOT "/same/Game.srm",
         SAVE_BACKUP_UNREADABLE, "AAA", 3, true) == SAVE_BACKUP_MADE);
   CHECK(count_files(dir) == 1);
   CHECK(save_backups_copy_image(ROOT "/same/Game.srm",
         SAVE_BACKUP_UNREADABLE, "BBB", 3, true) == SAVE_BACKUP_MADE);
   CHECK(count_files(dir) == 2);
   /* only the newest copy counts */
   set_time(12, 0, 2);
   CHECK(save_backups_copy_image(ROOT "/same/Game.srm",
         SAVE_BACKUP_UNREADABLE, "AAA", 3, true) == SAVE_BACKUP_MADE);
   CHECK(count_files(dir) == 3);

   /* the file's own bytes */
   put(ROOT "/same/File.srm", "file");
   CHECK(save_backups_copy_file(ROOT "/same/File.srm", SAVE_BACKUP_CUT, true)
         == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/same/backups/File - 2026-11-03 12-00-02 - Snes9x - cut.srm", "file"));
   CHECK(save_backups_copy_file(ROOT "/same/File.srm", SAVE_BACKUP_CUT, true)
         == SAVE_BACKUP_MADE);
   CHECK(count_files(dir) == 4);
   CHECK(save_backups_copy_file(ROOT "/same/Missing.srm", SAVE_BACKUP_CUT, true)
         == SAVE_BACKUP_NO_FILE);
   CHECK(count_files(dir) == 4);
}

static void backup_retention(void)
{
   unsigned k;
   char text[8];
   char dir[] = ROOT "/keep/backups";
   const char *never[] = {
      ROOT "/keep/backups/Game - 2000-01-01 00-00-00 - Snes9x - unreadable.srm",
      ROOT "/keep/backups/Game - 2000-01-01 00-00-00 - Snes9x.srm",
      ROOT "/keep/backups/Game - notes.srm",
      ROOT "/keep/backups/Other - 2000-01-01 00-00-00 - Snes9x - before-state-load.srm",
      ROOT "/keep/backups/game - 2000-01-01 00-00-00 - Snes9x - before-state-load.srm",
      ROOT "/keep/backups/Game - 2000-01-01 00-00-00 - Snes9x - before-state-load.sav",
      ROOT "/keep/backups/Game - 2000-01-01 00-00-00 - Snes9x - before-state-load.srm.tmp",
      ROOT "/keep/backups/Game - 2000-01-01 00-00-00 - Snes9x - unsaved.srm"
   };
   save_backups_set_roots(ROOT "/keep", NULL, NULL, NULL, NULL, NULL, "Snes9x");
   for (k = 0; k < sizeof(never) / sizeof(never[0]); k++)
      put(never[k], "never");
   for (k = 0; k < 12; k++)
   {
      set_time(13, 0, (int)k);
      snprintf(text, sizeof(text), "b%u", k);
      CHECK(save_backups_copy_image(ROOT "/keep/Game.srm",
            SAVE_BACKUP_BEFORE_STATE_LOAD, text, strlen(text), true)
            == SAVE_BACKUP_MADE);
      snprintf(text, sizeof(text), "c%u", k);
      CHECK(save_backups_copy_image(ROOT "/keep/Game.srm",
            SAVE_BACKUP_CHANGED_ELSEWHERE, text, strlen(text), true)
            == SAVE_BACKUP_MADE);
   }
   for (k = 0; k < 3; k++)
   {
      set_time(13, 1, (int)k);
      snprintf(text, sizeof(text), "u%u", k);
      CHECK(save_backups_copy_image(ROOT "/keep/Game.srm",
            SAVE_BACKUP_UNREADABLE, text, strlen(text), true) == SAVE_BACKUP_MADE);
   }
   CHECK(count_files(dir) == 10 + 10 + 3 + 8);
   for (k = 0; k < sizeof(never) / sizeof(never[0]); k++)
      CHECK(holds(never[k], "never"));
   CHECK(!path_is_valid(ROOT "/keep/backups/Game - 2026-11-03 13-00-00 - Snes9x - before-state-load.srm"));
   CHECK(!path_is_valid(ROOT "/keep/backups/Game - 2026-11-03 13-00-01 - Snes9x - before-state-load.srm"));
   CHECK(holds(ROOT "/keep/backups/Game - 2026-11-03 13-00-02 - Snes9x - before-state-load.srm", "b2"));
   CHECK(!path_is_valid(ROOT "/keep/backups/Game - 2026-11-03 13-00-00-2 - Snes9x - changed-elsewhere.srm"));
   CHECK(!path_is_valid(ROOT "/keep/backups/Game - 2026-11-03 13-00-01-2 - Snes9x - changed-elsewhere.srm"));
   CHECK(holds(ROOT "/keep/backups/Game - 2026-11-03 13-00-02-2 - Snes9x - changed-elsewhere.srm", "c2"));

   /* the copy just made is kept even when its clock reads older */
   set_time(9, 0, 0);
   CHECK(save_backups_copy_image(ROOT "/keep/Game.srm",
         SAVE_BACKUP_BEFORE_STATE_LOAD, "old", 3, true) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/keep/backups/Game - 2026-11-03 09-00-00 - Snes9x - before-state-load.srm", "old"));
   CHECK(!path_is_valid(ROOT "/keep/backups/Game - 2026-11-03 13-00-02 - Snes9x - before-state-load.srm"));
   CHECK(count_files(dir) == 10 + 10 + 3 + 8);
}

/* Two names cut to one: a copy may be either file's, so neither's are
 * pruned. */
static void backup_retention_cut(void)
{
   unsigned k;
   char text[8], game[160], path[256], name[320];
   save_backups_set_roots(ROOT "/cut", NULL, NULL, NULL, NULL, NULL, "Snes9x");
   memset(game, 'x', 150);
   strcpy(game + 150, "A.srm");
   snprintf(path, sizeof(path), ROOT "/cut/%s", game);
   for (k = 0; k < 3; k++)
   {
      set_time(16, 0, (int)k);
      snprintf(text, sizeof(text), "a%u", k);
      CHECK(save_backups_copy_image(path, SAVE_BACKUP_BEFORE_STATE_LOAD,
            text, strlen(text), true) == SAVE_BACKUP_MADE);
   }
   game[150] = 'B';
   snprintf(path, sizeof(path), ROOT "/cut/%s", game);
   for (k = 0; k < 12; k++)
   {
      set_time(16, 1, (int)k);
      snprintf(text, sizeof(text), "b%u", k);
      CHECK(save_backups_copy_image(path, SAVE_BACKUP_BEFORE_STATE_LOAD,
            text, strlen(text), true) == SAVE_BACKUP_MADE);
   }
   CHECK(count_files(ROOT "/cut/backups") == 3 + 12);
   game[136] = '\0';
   for (k = 0; k < 3; k++)
   {
      snprintf(name, sizeof(name), ROOT "/cut/backups/%s"
            " - 2026-11-03 16-00-%02u - Snes9x - before-state-load.srm",
            game, k);
      snprintf(text, sizeof(text), "a%u", k);
      CHECK(holds(name, text));
   }
}

/* How many files in @dir hold text starting with @tag. */
static size_t count_tagged(const char *dir, char tag)
{
   size_t i, n = 0;
   struct string_list *l = dir_list_new(dir, NULL, false, true, false, false);
   for (i = 0; l && i < l->size; i++)
   {
      void *buf   = NULL;
      int64_t len = 0;
      if (     filestream_read_file(l->elems[i].data, &buf, &len)
            && len && *(const char*)buf == tag)
         n++;
      free(buf);
   }
   if (l)
      string_list_free(l);
   return n;
}

/* @count before-state-load copies of @name into @dir, made at minute
 * @min, holding "<tag><k>". */
static void make_copies(const char *dir, const char *name, const char *core,
      char tag, unsigned count, int min)
{
   unsigned k;
   char text[16], path[256];
   snprintf(path, sizeof(path), "%s/%s", dir, name);
   for (k = 0; k < count; k++)
   {
      set_time(20, min, (int)k);
      snprintf(text, sizeof(text), "%c%u", tag, k);
      CHECK(save_backups_copy_to(dir, path, core,
            SAVE_BACKUP_BEFORE_STATE_LOAD, text, strlen(text), true)
            == SAVE_BACKUP_MADE);
   }
}

/* A long file's copies are never pruned by a shorter file whose name
 * its cut starts with; pruning needs a game more than 3 bytes under
 * its copy's own limit. */
static void backup_retention_long(void)
{
   char a[200], c[200], core[48];

   strcpy(a, "Mario");
   memset(a + 5, ' ', 140);
   strcpy(a + 145, "x.srm");
   make_copies(ROOT "/long1", a, "Snes9x", 'a', 3, 0);
   make_copies(ROOT "/long1", "Mario.srm", "Snes9x", 'c', 12, 1);
   CHECK(count_tagged(ROOT "/long1", 'a') == 3);
   CHECK(count_tagged(ROOT "/long1", 'c') == 10);

   memset(a, 'y', 132);
   strcpy(a + 132, " \xF0\x9F\x98\x80 tail.srm");
   memset(c, 'y', 132);
   strcpy(c + 132, ".srm");
   make_copies(ROOT "/long2", a, "Snes9x", 'a', 3, 0);
   make_copies(ROOT "/long2", c, "Snes9x", 'c', 12, 1);
   CHECK(count_tagged(ROOT "/long2", 'a') == 3);
   CHECK(count_tagged(ROOT "/long2", 'c') == 10);

   /* 133 bytes is what a cut can leave: never pruned */
   memset(c, 'w', 133);
   strcpy(c + 133, ".srm");
   make_copies(ROOT "/long3", c, "Snes9x", 'c', 12, 1);
   CHECK(count_tagged(ROOT "/long3", 'c') == 12);

   /* a 40-byte core cuts at 102 */
   memset(core, 'k', 40);
   core[40] = '\0';
   memset(a, 'v', 120);
   strcpy(a + 120, ".srm");
   memset(c, 'v', 102);
   strcpy(c + 102, ".srm");
   make_copies(ROOT "/long4", a, core, 'a', 3, 0);
   make_copies(ROOT "/long4", c, "Snes9x", 'c', 12, 1);
   CHECK(count_tagged(ROOT "/long4", 'a') == 3);
   CHECK(count_tagged(ROOT "/long4", 'c') == 10);
}

static void recovery_chain(void)
{
   char s[PATH_MAX_LENGTH];
   save_backups_set_roots(ROOT "/rsaves", NULL, NULL, NULL,
         ROOT "/rconfig", ROOT "/rapp", "Snes9x");
   set_time(14, 0, 0);
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves/Game.srm", "r1", 2, true));
   CHECK(same_path(s, ROOT "/rsaves/backups/Game - 2026-11-03 14-00-00 - Snes9x - unsaved.srm"));
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves/Game.srm", "r1", 2, true));
   CHECK(same_path(s, ROOT "/rsaves/backups/Game - 2026-11-03 14-00-00-2 - Snes9x - unsaved.srm"));
   CHECK(holds(ROOT "/rsaves/backups/Game - 2026-11-03 14-00-00 - Snes9x - unsaved.srm", "r1"));
   CHECK(count_files(ROOT "/rsaves/backups") == 2);

   /* the save folder can't be written: a file where its folder goes */
   put(ROOT "/rsaves2/backups", "file");
   save_backups_set_roots(ROOT "/rsaves2", NULL, NULL, NULL,
         ROOT "/rconfig", ROOT "/rapp", "Snes9x");
   CHECK(save_backups_copy_image(ROOT "/rsaves2/Game.srm",
         SAVE_BACKUP_CUT, "c", 1, true) == SAVE_BACKUP_FAILED);
   CHECK(count_files(ROOT "/rsaves2") == 1);
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves2/Game.srm", "r2", 2, true));
   CHECK(same_path(s, ROOT "/rconfig/recovery/Game - 2026-11-03 14-00-00 - Snes9x - unsaved.srm"));
   CHECK(holds(s, "r2"));

   /* nor the config folder */
   put(ROOT "/rconfig2/recovery", "file");
   save_backups_set_roots(ROOT "/rsaves2", NULL, NULL, NULL,
         ROOT "/rconfig2", ROOT "/rapp", "Snes9x");
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves2/Game.srm", "r3", 2, true));
   CHECK(same_path(s, ROOT "/rapp/recovery/Game - 2026-11-03 14-00-00 - Snes9x - unsaved.srm"));
   CHECK(holds(s, "r3"));

   /* and no app data folder (consoles) */
   save_backups_set_roots(ROOT "/rsaves2", NULL, NULL, NULL,
         ROOT "/rconfig2", NULL, "Snes9x");
   CHECK(!save_backups_recover(s, sizeof(s), ROOT "/rsaves2/Game.srm", "r4", 2, true));
   CHECK(!*s);
   CHECK(count_files(ROOT "/rsaves2") == 1);
   CHECK(count_files(ROOT "/rconfig2") == 1);
   CHECK(count_files(ROOT "/rapp/recovery") == 1);
}

/* Each writer's sync flag reaches the write it asks for. */
static void backup_sync(void)
{
   char s[PATH_MAX_LENGTH];
   save_backups_set_roots(ROOT "/sync", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   set_time(15, 0, 0);
   plain_writes = synced_writes = 0;
   CHECK(save_backups_copy_image(ROOT "/sync/Game.srm", SAVE_BACKUP_CUT,
         "1", 1, true) == SAVE_BACKUP_MADE);
   CHECK(synced_writes == 1 && plain_writes == 0);
   CHECK(save_backups_copy_image(ROOT "/sync/Game.srm", SAVE_BACKUP_CUT,
         "2", 1, false) == SAVE_BACKUP_MADE);
   CHECK(synced_writes == 1 && plain_writes == 1);
   put(ROOT "/sync/File.srm", "f1");
   CHECK(save_backups_copy_file(ROOT "/sync/File.srm", SAVE_BACKUP_CUT,
         true) == SAVE_BACKUP_MADE);
   CHECK(synced_writes == 2 && plain_writes == 1);
   put(ROOT "/sync/File.srm", "f2");
   CHECK(save_backups_copy_file(ROOT "/sync/File.srm", SAVE_BACKUP_CUT,
         false) == SAVE_BACKUP_MADE);
   CHECK(synced_writes == 2 && plain_writes == 2);
   CHECK(save_backups_copy_to(ROOT "/sync/backups/cloud-sync",
         ROOT "/sync/Game.srm", SAVE_BACKUPS_CORE_CLOUD_SYNC,
         SAVE_BACKUP_REPLACED, "3", 1, true) == SAVE_BACKUP_MADE);
   CHECK(synced_writes == 3 && plain_writes == 2);
   CHECK(save_backups_copy_to(ROOT "/sync/backups/cloud-sync",
         ROOT "/sync/Game.srm", SAVE_BACKUPS_CORE_CLOUD_SYNC,
         SAVE_BACKUP_DELETED, "4", 1, false) == SAVE_BACKUP_MADE);
   CHECK(synced_writes == 3 && plain_writes == 3);
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/sync/Game.srm",
         "r", 1, true));
   CHECK(synced_writes == 4 && plain_writes == 3);
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/sync/Game.srm",
         "r", 1, false));
   CHECK(synced_writes == 4 && plain_writes == 4);
   /* nothing written for a copy the newest one already holds */
   CHECK(save_backups_copy_image(ROOT "/sync/Game.srm", SAVE_BACKUP_CUT,
         "r", 1, true) == SAVE_BACKUP_MADE);
   CHECK(synced_writes == 4 && plain_writes == 4);
}

/* No copy without its time in its name. */
static void backup_no_time(void)
{
   char s[PATH_MAX_LENGTH];
   save_backups_set_roots(ROOT "/notime", NULL, NULL, NULL,
         ROOT "/notime_config", NULL, "Snes9x");
   set_time(19, 0, 0);
   strftime_fails = true;
   CHECK(save_backups_copy_image(ROOT "/notime/Game.srm", SAVE_BACKUP_CUT,
         "1", 1, true) == SAVE_BACKUP_FAILED);
   CHECK(!save_backups_recover(s, sizeof(s), ROOT "/notime/Game.srm",
         "r", 1, true));
   CHECK(!*s);
   strftime_fails = false;
   CHECK(!path_is_valid(ROOT "/notime"));
   CHECK(!path_is_valid(ROOT "/notime_config"));
}

int main(void)
{
   rm_tree(ROOT);
   CHECK(path_mkdir(ROOT));
   backup_folders();
   backup_system_fallback();
   backup_names();
   backup_core_chars();
   backup_collisions();
   backup_identical_skip();
   backup_retention();
   backup_retention_cut();
   backup_retention_long();
   recovery_chain();
   backup_sync();
   backup_no_time();
   save_backups_set_roots(NULL, NULL, NULL, NULL, NULL, NULL, NULL);
   rm_tree(ROOT);
   puts(failures ? "FAIL save_backups_test" : "PASS save_backups_test");
   return failures ? 1 : 0;
}
