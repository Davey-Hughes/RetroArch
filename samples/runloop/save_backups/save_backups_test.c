/* save_backups.c: where safety copies go, how they are named and read
 * back, copy numbers, the identical-copy skip, retention and the
 * recovery-file chain. Included with its clock, its time format
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
#include <vfs/vfs_implementation.h>

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
static bool write_fails;

static bool test_write(const char *path, const void *data, int64_t len)
{
   plain_writes++;
   if (write_fails)
      return false;
   return filestream_write_file_atomic(path, data, len);
}

static bool test_write_synced(const char *path, const void *data,
      int64_t len)
{
   synced_writes++;
   return filestream_write_file_atomic_synced(path, data, len);
}

static bool rename_fails, read_fails;

static int test_rename(const char *old_path, const char *new_path)
{
   return rename_fails ? -1 : filestream_rename(old_path, new_path);
}

static int64_t test_read_file(const char *path, void **buf, int64_t *len)
{
   if (read_fails)
      return 0;
   return filestream_read_file(path, buf, len);
}

static unsigned file_syncs;
static bool sync_fails;

static int test_sync(RFILE *stream)
{
   file_syncs++;
   return sync_fails ? -1 : filestream_sync(stream);
}

static unsigned dir_syncs;

static int test_dir_sync(const char *dir)
{
   dir_syncs++;
   return retro_vfs_dir_sync_impl(dir);
}

static bool list_fails, list_reversed;

/* A folder listing that fails, as an opendir() refused or OOM; or
 * comes in the other order */
static struct string_list *test_dir_list_new(const char *dir,
      const char *ext, bool include_dirs, bool include_hidden,
      bool include_compressed, bool recursive)
{
   size_t i;
   struct string_list *l;
   if (list_fails)
      return NULL;
   l = dir_list_new(dir, ext, include_dirs, include_hidden,
         include_compressed, recursive);
   for (i = 0; list_reversed && l && i < l->size / 2; i++)
   {
      struct string_list_elem e    = l->elems[i];
      l->elems[i]                  = l->elems[l->size - 1 - i];
      l->elems[l->size - 1 - i]    = e;
   }
   return l;
}

#define rtime_localtime test_localtime
#define strftime test_strftime
#define filestream_write_file_atomic test_write
#define filestream_write_file_atomic_synced test_write_synced
#define filestream_rename test_rename
#define filestream_read_file test_read_file
#define filestream_sync test_sync
#define retro_vfs_dir_sync_impl test_dir_sync
#define dir_list_new test_dir_list_new
#if defined(SAVE_BACKUPS_PATH_MAX) && !defined(_WIN32)
/* Win32's limit as from a run folder no deeper than RUN_UNITS: main()
 * adds what this one has past that, so the lanes pass from any folder.
 * Windows itself keeps the real limit. */
#define RUN_UNITS 124
static long path_max = SAVE_BACKUPS_PATH_MAX;
#undef SAVE_BACKUPS_PATH_MAX
#define SAVE_BACKUPS_PATH_MAX path_max
#endif
#include "../../../save_backups.c"
#undef rtime_localtime
#undef strftime
#undef filestream_write_file_atomic
#undef filestream_write_file_atomic_synced
#undef filestream_rename
#undef filestream_read_file
#undef filestream_sync
#undef retro_vfs_dir_sync_impl
#undef dir_list_new

#define ROOT "save_backups_test_files"

static int failures;

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

/* This device's tag as @t, or with NULL none, as at start-up */
static void use_device(const char *t)
{
   retro_atomic_store_release_int(&save_backups_device,
         t ? save_backups_tag_pack(t) : 0);
   save_backups_device_kept = false;
}

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

#ifdef SAVE_BACKUPS_PATH_MAX
/* UTF-16 units in @s */
static long units(const char *s)
{
   long n = 0;
   for (; *s; s++)
      if (((unsigned char)*s & 0xC0) != 0x80)
         n += (unsigned char)*s >= 0xF0 ? 2 : 1;
   return n;
}

/* UTF-16 units in @path's full path, the run folder's included */
static long full_units(const char *path)
{
   static char s[PATH_MAX_LENGTH];
   strlcpy(s, path, sizeof(s));
   if (!path_is_absolute(s))
      CHECK(path_resolve_realpath(s, sizeof(s), false));
   return units(s);
}
#endif

/* The most bytes of an ASCII game a copy into @dir keeps with this core
 * and extension: the name's 199 less 62 for the separators, the time,
 * room for a number, and the longest reason, and where a copy's path has a limit,
 * no more than is left after the folder and ".tmp"; below 0 when not
 * even an empty game fits. */
static long game_limit(const char *dir, const char *core, const char *ext)
{
   long fixed = 62 + (long)(strlen(core) + strlen(ext));
   long n     = 199 - fixed;
#ifdef SAVE_BACKUPS_PATH_MAX
   long room  = SAVE_BACKUPS_PATH_MAX - 4 - (full_units(dir) + 1) - fixed;
   if (room < n)
      n = room;
#endif
   return n;
}

/* Whether a long ASCII game in @dir is cut by the path, so its cut
 * ends with the mark. */
static bool path_cuts(const char *dir, const char *core, const char *ext)
{
   return game_limit(dir, core, ext)
      < 137 - (long)(strlen(core) + strlen(ext));
}

/* Ends long ASCII @game where a copy into @dir cuts it. */
static void cut_game(char *game, const char *dir, const char *core,
      const char *ext)
{
   long n = game_limit(dir, core, ext);
   if (path_cuts(dir, core, ext))
      game[n - 1] = '~';
   game[n] = '\0';
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
         SAVE_BACKUP_CUT, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/fsystem/pcsx2/backups/"
         "Mcd001 - 2026-11-03 18-00-00 - 0001-kqmz - Snes9x - cut.ps2", "card"));
   CHECK(count_files(ROOT "/fsystem/pcsx2/backups") == 1);
}

static void backup_names(void)
{
   size_t i, n;
   long cut, keep;
   bool mark;
   struct save_backup_name p;
   char game[400];
   static char path[PATH_MAX_LENGTH], dir[PATH_MAX_LENGTH];
   struct string_list *l;
   const char *zelda =
         "Zelda - Part 2 - 2026-11-03 10-00-00 - Snes9x - core-changed.mcd";
   roots();
   set_time(10, 0, 0);

   CHECK(save_backups_copy_image(ROOT "/saves/Snes9x/Game.srm",
         SAVE_BACKUP_UNREADABLE, "a", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/saves/backups/Snes9x/"
         "Game - 2026-11-03 10-00-00 - 0001-kqmz - Snes9x - unreadable.srm", "a"));
   CHECK(save_backups_copy_image(ROOT "/states/Game.state.auto",
         SAVE_BACKUP_NOT_LOADED, "b", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/states/backups/"
         "Game - 2026-11-03 10-00-00 - 0001-kqmz - Snes9x - not-loaded.state.auto", "b"));
   CHECK(save_backups_copy_image(ROOT "/states/Game.state3.auto",
         SAVE_BACKUP_NOT_LOADED, "c", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/states/backups/"
         "Game - 2026-11-03 10-00-00 - 0001-kqmz - Snes9x - not-loaded.state3.auto", "c"));
   CHECK(save_backups_copy_image(ROOT "/saves/Zelda - Part 2.mcd",
         SAVE_BACKUP_CORE_CHANGED, "d", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/saves/backups/"
         "Zelda - Part 2 - 2026-11-03 10-00-00 - 0001-kqmz - Snes9x - core-changed.mcd", "d"));
   CHECK(save_backups_copy_to(ROOT "/saves/backups", ROOT "/saves/Game.srm",
         "- Mega: Drive - Plus", SAVE_BACKUP_CUT, "e", 1, true) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/saves/backups/"
         "Game - 2026-11-03 10-00-00 - 0001-kqmz - _ Mega_ Drive-Plus - cut.srm", "e"));
   save_backups_set_roots(ROOT "/saves", NULL, NULL, NULL, NULL, NULL, NULL);
   CHECK(save_backups_copy_image(ROOT "/saves/Game.srm",
         SAVE_BACKUP_BLANKED, "f", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/saves/backups/"
         "Game - 2026-11-03 10-00-00 - 0002-kqmz - unknown - blanked.srm", "f"));
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
   CHECK(save_backups_copy_image(path, SAVE_BACKUP_UNREADABLE, "g", 1, true, NULL)
         == SAVE_BACKUP_MADE);
   set_time(10, 0, 1);
   CHECK(save_backups_copy_image(path, SAVE_BACKUP_BEFORE_STATE_LOAD, "h", 1, true, NULL)
         == SAVE_BACKUP_MADE);
   /* 'a' and 63 two-byte characters, or what fits beside the mark */
   cut  = game_limit(ROOT "/saves/backups", "Snes9x", ".srm");
   mark = cut < 64;
   cut  = mark ? 2 * cut - 2 : 127;
   l   = dir_list_new(ROOT "/saves/backups", NULL, false, true, false, false);
   n   = 0;
   for (i = 0; l && i < l->size; i++)
   {
      const char *name = path_basename(l->elems[i].data);
      if (name[0] != 'a')
         continue;
      n++;
      CHECK(strlen(name) < 200);
      CHECK(save_backups_parse(name, &p));
      CHECK(p.game_len == (size_t)cut);
      CHECK(!memcmp(name, game, p.game_len - mark));
      CHECK(!mark || name[p.game_len - 1] == '~');
      CHECK(((unsigned char)name[p.game_len - 1 - mark] & 0xC0) == 0x80);
      CHECK(!strcmp(name + p.ext_off, ".srm"));
   }
   CHECK(n == 2);
   if (l)
      string_list_free(l);

   /* the cut keeps trailing spaces and dots, before any mark */
   keep = game_limit(ROOT "/override/backups", "Snes9x", ".srm")
      - path_cuts(ROOT "/override/backups", "Snes9x", ".srm");
   memset(game, 'b', (size_t)keep - 2);
   memcpy(game + keep - 2, " .", 2);
   memset(game + keep, 'c', 100);
   strcpy(game + keep + 100, ".srm");
   fill_pathname_join(path, ROOT "/override", game, sizeof(path));
   CHECK(save_backups_copy_image(path, SAVE_BACKUP_CUT, "i", 1, true, NULL)
         == SAVE_BACKUP_MADE);
   cut_game(game, ROOT "/override/backups", "Snes9x", ".srm");
   snprintf(dir, sizeof(dir), ROOT "/override/backups/%s"
         " - 2026-11-03 10-00-01 - 0001-kqmz - Snes9x - cut.srm", game);
   CHECK(holds(dir, "i"));

   /* and steps back at most 3 bytes, so a cut game is within 3 bytes
    * of its limit (127 here) */
   memset(game, 'y', 123);
   strcpy(game + 123, " \xF0\x9F\x98\x80 tail");
   CHECK(save_backups_game_len(game, strlen(game), 6, 4) == 124);
   memset(game, 'd', 121);
   memset(game + 121, 0x80, 10);
   CHECK(save_backups_game_len(game, 131, 6, 4) == 124);
}

/* Every character the core field changes. */
static void backup_core_chars(void)
{
   size_t i;
   struct save_backup_name p;
   struct string_list *l;
   set_time(17, 0, 0);
   CHECK(save_backups_copy_to(ROOT "/chars", ROOT "/chars/Game.srm",
         "a<b>c\"d/e\\f|g?h*i\001j\037k", SAVE_BACKUP_CUT, "1", 1, true)
         == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/chars/"
         "Game - 2026-11-03 17-00-00 - 0001-kqmz - a_b_c_d_e_f_g_h_i_j_k - cut.srm", "1"));
   CHECK(save_backups_copy_to(ROOT "/chars", ROOT "/chars/Game.srm",
         "", SAVE_BACKUP_CUT, "2", 1, true) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/chars/"
         "Game - 2026-11-03 17-00-00 - 0002-kqmz - unknown - cut.srm", "2"));
   /* no " - " left by the cleanup itself either */
   CHECK(save_backups_copy_to(ROOT "/chars", ROOT "/chars/Game.srm",
         "a  -  b", SAVE_BACKUP_CUT, "3", 1, true) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/chars/"
         "Game - 2026-11-03 17-00-00 - 0003-kqmz - a-b - cut.srm", "3"));
   CHECK(save_backups_copy_to(ROOT "/chars", ROOT "/chars/Game.srm",
         "c   -   d", SAVE_BACKUP_CUT, "4", 1, true) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/chars/"
         "Game - 2026-11-03 17-00-00 - 0004-kqmz - c-d - cut.srm", "4"));
   l = dir_list_new(ROOT "/chars", NULL, false, true, false, false);
   CHECK(l && l->size == 4);
   for (i = 0; l && i < l->size; i++)
      CHECK(save_backups_parse(path_basename(l->elems[i].data), &p));
   if (l)
      string_list_free(l);
}

static void backup_collisions(void)
{
   char dir[] = ROOT "/collide/backups";
   save_backups_set_roots(ROOT "/collide", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   set_time(11, 0, 0);
   CHECK(save_backups_copy_image(ROOT "/collide/Game.srm",
         SAVE_BACKUP_CUT, "1", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(save_backups_copy_image(ROOT "/collide/Game.srm",
         SAVE_BACKUP_CUT, "2", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(save_backups_copy_image(ROOT "/collide/Game.srm",
         SAVE_BACKUP_CUT, "3", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(count_files(dir) == 3);
   CHECK(holds(ROOT "/collide/backups/Game - 2026-11-03 11-00-00 - 0001-kqmz - Snes9x - cut.srm", "1"));
   CHECK(holds(ROOT "/collide/backups/Game - 2026-11-03 11-00-00 - 0002-kqmz - Snes9x - cut.srm", "2"));
   CHECK(holds(ROOT "/collide/backups/Game - 2026-11-03 11-00-00 - 0003-kqmz - Snes9x - cut.srm", "3"));
   /* one count per file, whatever the reason */
   CHECK(save_backups_copy_image(ROOT "/collide/Game.srm",
         SAVE_BACKUP_UNREADABLE, "4", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/collide/backups/Game - 2026-11-03 11-00-00 - 0004-kqmz - Snes9x - unreadable.srm", "4"));
   /* a name of another case, one file where case is not told apart,
    * is not written over */
   put(ROOT "/collide/backups/game - 2026-11-03 11-00-00 - 0005-kqmz - Snes9x - cut.srm", "x");
   CHECK(save_backups_copy_image(ROOT "/collide/Game.srm",
         SAVE_BACKUP_CUT, "5", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/collide/backups/game - 2026-11-03 11-00-00 - 0005-kqmz - Snes9x - cut.srm", "x"));
   CHECK(   holds(ROOT "/collide/backups/Game - 2026-11-03 11-00-00 - 0005-kqmz - Snes9x - cut.srm", "5")
         || holds(ROOT "/collide/backups/Game - 2026-11-03 11-00-00 - 0006-kqmz - Snes9x - cut.srm", "5"));
   CHECK(count_files(dir) == 6);
   /* nor the names of copies still being written (the next number is
    * 6 or 7, by how the case went) */
   set_time(11, 0, 1);
   put(ROOT "/collide/backups/Game - 2026-11-03 11-00-01 - 0006-kqmz - Snes9x - cut.srm.tmp", "w");
   put(ROOT "/collide/backups/Game - 2026-11-03 11-00-01 - 0007-kqmz - Snes9x - cut.srm.tmp", "w");
   CHECK(save_backups_copy_image(ROOT "/collide/Game.srm",
         SAVE_BACKUP_CUT, "7", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/collide/backups/Game - 2026-11-03 11-00-01 - 0008-kqmz - Snes9x - cut.srm", "7"));
   CHECK(holds(ROOT "/collide/backups/Game - 2026-11-03 11-00-01 - 0007-kqmz - Snes9x - cut.srm.tmp", "w"));
   CHECK(!path_is_valid(ROOT "/collide/backups/Game - 2026-11-03 11-00-01 - 0007-kqmz - Snes9x - cut.srm"));
}

static void backup_identical_skip(void)
{
   char dir[] = ROOT "/same/backups";
   save_backups_set_roots(ROOT "/same", NULL, NULL, NULL, NULL, NULL, "Snes9x");
   set_time(12, 0, 0);
   CHECK(save_backups_copy_image(ROOT "/same/Game.srm",
         SAVE_BACKUP_CUT, "AAA", 3, true, NULL) == SAVE_BACKUP_MADE);
   set_time(12, 0, 1);
   CHECK(save_backups_copy_image(ROOT "/same/Game.srm",
         SAVE_BACKUP_UNREADABLE, "AAA", 3, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(count_files(dir) == 1);
   CHECK(save_backups_copy_image(ROOT "/same/Game.srm",
         SAVE_BACKUP_UNREADABLE, "BBB", 3, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(count_files(dir) == 2);
   /* only the newest copy counts */
   set_time(12, 0, 2);
   CHECK(save_backups_copy_image(ROOT "/same/Game.srm",
         SAVE_BACKUP_UNREADABLE, "AAA", 3, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(count_files(dir) == 3);

   /* the file's own bytes */
   put(ROOT "/same/File.srm", "file");
   CHECK(save_backups_copy_file(ROOT "/same/File.srm", SAVE_BACKUP_CUT, true, NULL)
         == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/same/backups/File - 2026-11-03 12-00-02 - 0001-kqmz - Snes9x - cut.srm", "file"));
   CHECK(save_backups_copy_file(ROOT "/same/File.srm", SAVE_BACKUP_CUT, true, NULL)
         == SAVE_BACKUP_MADE);
   CHECK(count_files(dir) == 4);
   CHECK(save_backups_copy_file(ROOT "/same/Missing.srm", SAVE_BACKUP_CUT, true, NULL)
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
            SAVE_BACKUP_BEFORE_STATE_LOAD, text, strlen(text), true, NULL)
            == SAVE_BACKUP_MADE);
      snprintf(text, sizeof(text), "c%u", k);
      CHECK(save_backups_copy_image(ROOT "/keep/Game.srm",
            SAVE_BACKUP_CHANGED_ELSEWHERE, text, strlen(text), true, NULL)
            == SAVE_BACKUP_MADE);
   }
   for (k = 0; k < 3; k++)
   {
      set_time(13, 1, (int)k);
      snprintf(text, sizeof(text), "u%u", k);
      CHECK(save_backups_copy_image(ROOT "/keep/Game.srm",
            SAVE_BACKUP_UNREADABLE, text, strlen(text), true, NULL) == SAVE_BACKUP_MADE);
   }
   CHECK(count_files(dir) == 10 + 10 + 3 + 8);
   for (k = 0; k < sizeof(never) / sizeof(never[0]); k++)
      CHECK(holds(never[k], "never"));
   CHECK(!path_is_valid(ROOT "/keep/backups/Game - 2026-11-03 13-00-00 - 0001-kqmz - Snes9x - before-state-load.srm"));
   CHECK(!path_is_valid(ROOT "/keep/backups/Game - 2026-11-03 13-00-01 - 0003-kqmz - Snes9x - before-state-load.srm"));
   CHECK(holds(ROOT "/keep/backups/Game - 2026-11-03 13-00-02 - 0005-kqmz - Snes9x - before-state-load.srm", "b2"));
   CHECK(!path_is_valid(ROOT "/keep/backups/Game - 2026-11-03 13-00-00 - 0002-kqmz - Snes9x - changed-elsewhere.srm"));
   CHECK(!path_is_valid(ROOT "/keep/backups/Game - 2026-11-03 13-00-01 - 0004-kqmz - Snes9x - changed-elsewhere.srm"));
   CHECK(holds(ROOT "/keep/backups/Game - 2026-11-03 13-00-02 - 0006-kqmz - Snes9x - changed-elsewhere.srm", "c2"));

   /* the copy just made is kept even when its clock reads older */
   set_time(9, 0, 0);
   CHECK(save_backups_copy_image(ROOT "/keep/Game.srm",
         SAVE_BACKUP_BEFORE_STATE_LOAD, "old", 3, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/keep/backups/Game - 2026-11-03 09-00-00 - 0028-kqmz - Snes9x - before-state-load.srm", "old"));
   CHECK(!path_is_valid(ROOT "/keep/backups/Game - 2026-11-03 13-00-02 - 0005-kqmz - Snes9x - before-state-load.srm"));
   CHECK(count_files(dir) == 10 + 10 + 3 + 8);
}

/* not-loaded keeps the newest copies of an automatic state, every copy
 * of a save file */
static void backup_retention_not_loaded(void)
{
   unsigned k;
   char text[8];
   char dir[] = ROOT "/notloaded/backups";
   save_backups_set_roots(ROOT "/notloaded", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   for (k = 0; k < 12; k++)
   {
      set_time(14, 0, (int)k);
      snprintf(text, sizeof(text), "a%u", k);
      put(ROOT "/notloaded/Game.state.auto", text);
      CHECK(save_backups_move_to(dir, ROOT "/notloaded/Game.state.auto",
            NULL, SAVE_BACKUP_NOT_LOADED) == SAVE_BACKUP_MADE);
      snprintf(text, sizeof(text), "s%u", k);
      CHECK(save_backups_copy_to(dir, ROOT "/notloaded/Game.srm", NULL,
            SAVE_BACKUP_NOT_LOADED, text, strlen(text), false)
            == SAVE_BACKUP_MADE);
   }
   CHECK(count_files(dir) == 10 + 12);
   CHECK(!path_is_valid(ROOT "/notloaded/backups/Game - 2026-11-03 14-00-00 - 0001-kqmz - Snes9x - not-loaded.state.auto"));
   CHECK(!path_is_valid(ROOT "/notloaded/backups/Game - 2026-11-03 14-00-01 - 0002-kqmz - Snes9x - not-loaded.state.auto"));
   CHECK(holds(ROOT "/notloaded/backups/Game - 2026-11-03 14-00-02 - 0003-kqmz - Snes9x - not-loaded.state.auto", "a2"));
   CHECK(holds(ROOT "/notloaded/backups/Game - 2026-11-03 14-00-00 - 0001-kqmz - Snes9x - not-loaded.srm", "s0"));
}

/* A not-loaded automatic state held beside itself while a task copies
 * it: the held names, naming the copy, its retention, and the temps of
 * copies that never finished. */
static void backup_held(void)
{
   unsigned k;
   static char s[PATH_MAX_LENGTH], first[PATH_MAX_LENGTH];
   struct string_list *copies;
   char text[8];
   char dir[] = ROOT "/holding/backups";
   const char *autop = ROOT "/holding/Game.state.auto";
   const char *temps[] = {
      ROOT "/holding/backups/Game - 2026-11-03 16-00-00 - Snes9x - not-loaded.state.auto.tmp",
      ROOT "/holding/backups/Game - 2026-11-03 16-00-00 - 0002-kqmz - Other - not-loaded.state.auto.tmp"
   };
   const char *others[] = {
      ROOT "/holding/backups/Game - 2026-11-03 16-00-00 - Snes9x - before-state-load.state.auto.tmp",
      ROOT "/holding/backups/Other - 2026-11-03 16-00-00 - Snes9x - not-loaded.state.auto.tmp",
      ROOT "/holding/backups/Game - 2026-11-03 16-00-00 - Snes9x - not-loaded.srm.tmp",
      ROOT "/holding/backups/Game.state.auto.tmp"
   };

   CHECK(save_backups_held_name(s, sizeof(s), autop, 1)
         && same_path(s, ROOT "/holding/Game.state.auto.not-loaded.prev"));
   CHECK(save_backups_held_name(s, sizeof(s), autop, 2)
         && same_path(s, ROOT "/holding/Game.state.auto.not-loaded.2.prev"));
   CHECK(save_backups_held_name(s, sizeof(s), autop, 9)
         && same_path(s, ROOT "/holding/Game.state.auto.not-loaded.9.prev"));
   CHECK(!save_backups_held_name(s, sizeof(s), autop, 0));
   CHECK(!save_backups_held_name(s, sizeof(s), autop, 10));
   CHECK(!save_backups_held_name(s, strlen(autop) + 16, autop, 1));

   save_backups_set_roots(ROOT "/holding", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   CHECK(!save_backups_held(autop));
   put(ROOT "/holding/Game.state.auto.prev", "aside");
   CHECK(!save_backups_held(autop));
   put(ROOT "/holding/Game.state.auto.not-loaded.3.prev", "h3");
   CHECK(save_backups_held(autop));
   put(ROOT "/holding/Game.srm.not-loaded.prev", "srm");
   CHECK(!save_backups_held(ROOT "/holding/Game.srm"));

   /* naming writes nothing */
   set_time(15, 0, 0);
   CHECK(save_backups_name_copy(first, sizeof(first), dir, autop, "Snes9x", SAVE_BACKUP_NOT_LOADED) == SAVE_BACKUP_MADE);
   CHECK(same_path(first, ROOT "/holding/backups/Game - 2026-11-03 15-00-00 - 0001-kqmz - Snes9x - not-loaded.state.auto"));
   CHECK(count_files(dir) == 0);
   CHECK((copies = save_backups_delete_temps(dir, autop, "Snes9x",
         SAVE_BACKUP_NOT_LOADED)) && !copies->size);
   string_list_free(copies);
   put(first, "c0");
   CHECK(save_backups_name_copy(s, sizeof(s), dir, autop, "Snes9x", SAVE_BACKUP_NOT_LOADED) == SAVE_BACKUP_MADE);
   CHECK(same_path(s, ROOT "/holding/backups/Game - 2026-11-03 15-00-00 - 0002-kqmz - Snes9x - not-loaded.state.auto"));
   CHECK(count_files(dir) == 1);

   /* the copy made counts and stays, even when its clock reads older */
   for (k = 1; k < 10; k++)
   {
      set_time(15, 1, (int)k);
      snprintf(text, sizeof(text), "c%u", k);
      CHECK(save_backups_name_copy(s, sizeof(s), dir, autop, "Snes9x", SAVE_BACKUP_NOT_LOADED) == SAVE_BACKUP_MADE);
      put(s, text);
   }
   set_time(9, 0, 0);
   CHECK(save_backups_name_copy(s, sizeof(s), dir, autop, "Snes9x", SAVE_BACKUP_NOT_LOADED) == SAVE_BACKUP_MADE);
   put(s, "made");
   save_backups_retain_made(dir, autop, "Snes9x", SAVE_BACKUP_NOT_LOADED, s);
   CHECK(count_files(dir) == 10 && holds(s, "made"));
   CHECK(!path_is_valid(first));
   CHECK(holds(ROOT "/holding/backups/Game - 2026-11-03 15-01-01 - 0002-kqmz - Snes9x - not-loaded.state.auto", "c1"));
   /* listed newest first, by number, this reason only */
   put(ROOT "/holding/backups/Game - 2026-11-03 16-00-00 - Snes9x - before-state-load.state.auto", "b");
   CHECK((copies = save_backups_delete_temps(dir, autop, "Snes9x",
         SAVE_BACKUP_NOT_LOADED)) && copies->size == 10);
   CHECK(copies && same_path(copies->elems[0].data, s));
   for (k = 0; copies && k < 9; k++)
   {
      snprintf(text, sizeof(text), "c%u", 9 - k);
      CHECK(holds(copies->elems[k + 1].data, text));
   }
   string_list_free(copies);
   filestream_delete(ROOT "/holding/backups/Game - 2026-11-03 16-00-00 - Snes9x - before-state-load.state.auto");
   /* held names are no copies */
   CHECK(holds(ROOT "/holding/Game.state.auto.not-loaded.3.prev", "h3"));
   /* the only copy */
   CHECK(save_backups_name_copy(s, sizeof(s), dir, ROOT "/holding/Solo.state.auto", "Snes9x",
         SAVE_BACKUP_NOT_LOADED) == SAVE_BACKUP_MADE);
   put(s, "solo");
   save_backups_retain_made(dir, ROOT "/holding/Solo.state.auto", "Snes9x",
         SAVE_BACKUP_NOT_LOADED, s);
   CHECK(holds(s, "solo") && count_files(dir) == 11);

   /* this file's not-loaded temps go, of whichever core */
   for (k = 0; k < sizeof(temps) / sizeof(temps[0]); k++)
      put(temps[k], "t");
   for (k = 0; k < sizeof(others) / sizeof(others[0]); k++)
      put(others[k], "t");
   string_list_free(save_backups_delete_temps(dir, autop, "Snes9x",
         SAVE_BACKUP_NOT_LOADED));
   for (k = 0; k < sizeof(temps) / sizeof(temps[0]); k++)
      CHECK(!path_is_valid(temps[k]));
   for (k = 0; k < sizeof(others) / sizeof(others[0]); k++)
      CHECK(holds(others[k], "t"));
   CHECK(count_files(dir) == 11 + 4);

   /* nor the name of a copy in flight */
   set_time(15, 2, 0);
   put(ROOT "/holding/backups/Game - 2026-11-03 15-02-00 - 0012-kqmz - Snes9x - not-loaded.state.auto.tmp", "w");
   CHECK(save_backups_name_copy(s, sizeof(s), dir, autop, "Snes9x", SAVE_BACKUP_NOT_LOADED) == SAVE_BACKUP_MADE);
   CHECK(same_path(s, ROOT "/holding/backups/Game - 2026-11-03 15-02-00 - 0013-kqmz - Snes9x - not-loaded.state.auto"));
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
            text, strlen(text), true, NULL) == SAVE_BACKUP_MADE);
   }
   game[150] = 'B';
   snprintf(path, sizeof(path), ROOT "/cut/%s", game);
   for (k = 0; k < 12; k++)
   {
      set_time(16, 1, (int)k);
      snprintf(text, sizeof(text), "b%u", k);
      CHECK(save_backups_copy_image(path, SAVE_BACKUP_BEFORE_STATE_LOAD,
            text, strlen(text), true, NULL) == SAVE_BACKUP_MADE);
   }
   CHECK(count_files(ROOT "/cut/backups") == 3 + 12);
   cut_game(game, ROOT "/cut/backups", "Snes9x", ".srm");
   for (k = 0; k < 3; k++)
   {
      snprintf(name, sizeof(name), ROOT "/cut/backups/%s"
            " - 2026-11-03 16-00-%02u - %04u-kqmz - Snes9x - before-state-load.srm",
            game, k, k + 1);
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
   long n;

   strcpy(a, "Mario");
   memset(a + 5, ' ', 140);
   strcpy(a + 145, "x.srm");
   make_copies(ROOT "/long1", a, "Snes9x", 'a', 3, 0);
   make_copies(ROOT "/long1", "Mario.srm", "Snes9x", 'c', 12, 1);
   CHECK(count_tagged(ROOT "/long1", 'a') == 3);
   CHECK(count_tagged(ROOT "/long1", 'c') == 10);

   n = game_limit(ROOT "/long2", "Snes9x", ".srm");
   memset(a, 'y', (size_t)n - 4);
   strcpy(a + n - 4, " \xF0\x9F\x98\x80 tail.srm");
   memset(c, 'y', (size_t)n - 4);
   strcpy(c + n - 4, ".srm");
   make_copies(ROOT "/long2", a, "Snes9x", 'a', 3, 0);
   make_copies(ROOT "/long2", c, "Snes9x", 'c', 12, 1);
   CHECK(count_tagged(ROOT "/long2", 'a') == 3);
   CHECK(count_tagged(ROOT "/long2", 'c') == 10);

   /* 3 under the name's limit is what a cut can leave: never pruned.
    * Whole at the path's limit, it has no mark: pruned. */
   n = game_limit(ROOT "/long3", "Snes9x", ".srm");
   memset(c, 'w', (size_t)n - 3);
   strcpy(c + n - 3, ".srm");
   make_copies(ROOT "/long3", c, "Snes9x", 'c', 12, 1);
   CHECK(count_tagged(ROOT "/long3", 'c')
         == (path_cuts(ROOT "/long3", "Snes9x", ".srm") ? 10 : 12));

   /* a 40-byte core cuts shorter (at 89 by the name's limit) */
   memset(core, 'k', 40);
   core[40] = '\0';
   n = game_limit(ROOT "/long4", core, ".srm");
   memset(a, 'v', 120);
   strcpy(a + 120, ".srm");
   memset(c, 'v', (size_t)n);
   strcpy(c + n, ".srm");
   make_copies(ROOT "/long4", a, core, 'a', 3, 0);
   make_copies(ROOT "/long4", c, "Snes9x", 'c', 12, 1);
   CHECK(count_tagged(ROOT "/long4", 'a') == 3);
   CHECK(count_tagged(ROOT "/long4", 'c') == 10);
   /* nor the other way: a cut file prunes nothing */
   make_copies(ROOT "/long4", c, "Snes9x", 'c', 12, 2);
   make_copies(ROOT "/long4", a, core, 'a', 1, 3);
   CHECK(count_tagged(ROOT "/long4", 'a') == 4);
   CHECK(count_tagged(ROOT "/long4", 'c') == 10);
}

/* Copies cut to fit a folder elsewhere, then synced here: their marked
 * game never counts, even for a file named exactly that. */
static void backup_retention_mark(void)
{
   unsigned k;
   char name[128];
   for (k = 0; k < 12; k++)
   {
      snprintf(name, sizeof(name), ROOT "/mark/Mario~"
            " - 2000-01-01 00-00-%02u - Snes9x - before-state-load.srm", k);
      put(name, "a");
   }
   make_copies(ROOT "/mark", "Mario~.srm", "Snes9x", 'c', 12, 0);
   CHECK(count_tagged(ROOT "/mark", 'a') == 12);
   CHECK(count_tagged(ROOT "/mark", 'c') == 12);
}

/* A session's first before-state-load copy, its number held by the
 * caller, outlives retention; a copy from before numbering never
 * stands for it. */
static void retention_keeps_first_of_session(void)
{
   unsigned k, seq, first;
   char text[8];
   char dir[] = ROOT "/first/backups";
   const char *first_name = ROOT "/first/backups/"
         "Game - 2026-11-03 08-00-00 - 0001-kqmz - Snes9x - before-state-load.srm";
   save_backups_set_roots(ROOT "/first", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   put(ROOT "/first/backups/"
         "Game - 2000-01-01 00-00-00 - Snes9x - changed-elsewhere.srm",
         "first");
   set_time(8, 0, 0);
   seq = 0;
   CHECK(save_backups_copy_image(ROOT "/first/Game.srm",
         SAVE_BACKUP_BEFORE_STATE_LOAD, "first", 5, true, &seq)
         == SAVE_BACKUP_MADE);
   CHECK(seq == 1 && holds(first_name, "first"));
   first = seq;
   /* a numbered copy holds it: that one, nothing written */
   seq   = 0;
   CHECK(save_backups_copy_image(ROOT "/first/Game.srm",
         SAVE_BACKUP_BEFORE_STATE_LOAD, "first", 5, true, &seq)
         == SAVE_BACKUP_MADE);
   CHECK(seq == first && count_files(dir) == 2);
   for (k = 0; k < 12; k++)
   {
      set_time(8, 1, (int)k);
      snprintf(text, sizeof(text), "b%u", k);
      seq = first;
      CHECK(save_backups_copy_image(ROOT "/first/Game.srm",
            SAVE_BACKUP_BEFORE_STATE_LOAD, text, strlen(text), true, &seq)
            == SAVE_BACKUP_MADE);
      CHECK(seq == k + 2);
   }
   CHECK(holds(first_name, "first"));
   CHECK(count_tagged(dir, 'b') == 10);
   CHECK(!path_is_valid(ROOT "/first/backups/Game - 2026-11-03 08-01-01 - 0003-kqmz - Snes9x - before-state-load.srm"));
   CHECK(holds(ROOT "/first/backups/Game - 2026-11-03 08-01-02 - 0004-kqmz - Snes9x - before-state-load.srm", "b2"));
   /* held by no one, it is an ordinary copy */
   set_time(8, 2, 0);
   CHECK(save_backups_copy_image(ROOT "/first/Game.srm",
         SAVE_BACKUP_BEFORE_STATE_LOAD, "next", 4, true, NULL)
         == SAVE_BACKUP_MADE);
   CHECK(!path_is_valid(first_name));
   CHECK(count_tagged(dir, 'b') == 9);
   CHECK(count_files(dir) == 11);
}

/* A clock set back: the numbers keep the newest copies. */
static void retention_clock_back(void)
{
   unsigned k;
   char text[8];
   char dir[] = ROOT "/clock/backups";
   save_backups_set_roots(ROOT "/clock", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   for (k = 0; k < 5; k++)
   {
      set_time(13, 30, (int)k);
      snprintf(text, sizeof(text), "a%u", k);
      CHECK(save_backups_copy_image(ROOT "/clock/Game.srm",
            SAVE_BACKUP_BEFORE_STATE_LOAD, text, strlen(text), true, NULL)
            == SAVE_BACKUP_MADE);
   }
   for (k = 0; k < 10; k++)
   {
      set_time(7, 0, (int)k);
      snprintf(text, sizeof(text), "z%u", k);
      CHECK(save_backups_copy_image(ROOT "/clock/Game.srm",
            SAVE_BACKUP_BEFORE_STATE_LOAD, text, strlen(text), true, NULL)
            == SAVE_BACKUP_MADE);
   }
   CHECK(count_tagged(dir, 'a') == 0);
   CHECK(count_tagged(dir, 'z') == 10);
   CHECK(holds(ROOT "/clock/backups/Game - 2026-11-03 07-00-00 - 0006-kqmz - Snes9x - before-state-load.srm", "z0"));
}

/* Copies from before numbering are older than any numbered one, by
 * their time and number among themselves, so pruned first; numbering
 * starts at 1 beside them. */
static void retention_mixed_names(void)
{
   unsigned k;
   char text[8], name[160];
   char dir[] = ROOT "/mixed/backups";
   struct save_backup_name a, b;
   save_backups_set_roots(ROOT "/mixed", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   for (k = 0; k < 5; k++)
   {
      snprintf(name, sizeof(name), "%s/Game - 2026-11-03 23-00-0%u"
            " - Snes9x - before-state-load.srm", dir, k);
      snprintf(text, sizeof(text), "o%u", k);
      put(name, text);
   }
   put(ROOT "/mixed/backups/Game - 2026-11-03 23-00-00-2 - Snes9x - before-state-load.srm", "p");
   for (k = 0; k < 6; k++)
   {
      set_time(6, 0, (int)k);
      snprintf(text, sizeof(text), "n%u", k);
      CHECK(save_backups_copy_image(ROOT "/mixed/Game.srm",
            SAVE_BACKUP_BEFORE_STATE_LOAD, text, strlen(text), true, NULL)
            == SAVE_BACKUP_MADE);
   }
   CHECK(holds(ROOT "/mixed/backups/Game - 2026-11-03 06-00-00 - 0001-kqmz - Snes9x - before-state-load.srm", "n0"));
   CHECK(count_tagged(dir, 'n') == 6);
   CHECK(!path_is_valid(ROOT "/mixed/backups/Game - 2026-11-03 23-00-00 - Snes9x - before-state-load.srm"));
   CHECK(!path_is_valid(ROOT "/mixed/backups/Game - 2026-11-03 23-00-00-2 - Snes9x - before-state-load.srm"));
   CHECK(holds(ROOT "/mixed/backups/Game - 2026-11-03 23-00-01 - Snes9x - before-state-load.srm", "o1"));
   CHECK(count_files(dir) == 10);

   CHECK(save_backups_parse("G - 2026-11-03 23-00-00-2 - S - cut.srm", &a));
   CHECK(save_backups_parse("G - 2000-01-01 00-00-00 - 0001-kqmz - S - cut.srm", &b));
   CHECK(save_backups_cmp(&a, &b) < 0 && save_backups_cmp(&b, &a) > 0);
   /* one device: by number; two: by time, then number, then device */
   CHECK(save_backups_parse("G - 2026-11-03 23-00-00 - 0001-kqmz - S - cut.srm", &a));
   CHECK(save_backups_cmp(&b, &a) < 0 && save_backups_cmp(&a, &b) > 0);
   CHECK(save_backups_parse("G - 2000-01-01 00-00-00 - 0002-kqmz - S - cut.srm", &b));
   strcpy(b.eff, a.time);
   CHECK(save_backups_cmp(&a, &b) < 0);
   CHECK(save_backups_parse("G - 2000-01-01 00-00-00 - 0002-abcd - S - cut.srm", &b));
   CHECK(save_backups_cmp(&b, &a) < 0 && save_backups_cmp(&a, &b) > 0);
   CHECK(save_backups_parse("G - 2026-11-03 23-00-00 - 0001-abcd - S - cut.srm", &b));
   CHECK(save_backups_cmp(&b, &a) < 0 && save_backups_cmp(&a, &b) > 0);
}

/* Two devices' copies in one folder (a sync tool): one that was
 * offline numbered its newer copies lower; they are kept. */
static void retention_two_devices(void)
{
   unsigned k;
   char name[160];
   char dir[] = ROOT "/two/backups";
   save_backups_set_roots(ROOT "/two", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   for (k = 1; k <= 9; k++)
   {
      snprintf(name, sizeof(name), "%s/Game - 2026-11-03 10-00-0%u"
            " - %04u-abcd - Snes9x - before-state-load.srm", dir, k, k);
      put(name, "a");
   }
   for (k = 4; k <= 9; k++)
   {
      snprintf(name, sizeof(name), "%s/Game - 2026-11-03 11-00-0%u"
            " - %04u-kqmz - Snes9x - before-state-load.srm", dir, k, k);
      put(name, "b");
   }
   set_time(12, 0, 0);
   CHECK(save_backups_copy_image(ROOT "/two/Game.srm",
         SAVE_BACKUP_BEFORE_STATE_LOAD, "n", 1, true, NULL)
         == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/two/backups/Game - 2026-11-03 12-00-00"
         " - 0010-kqmz - Snes9x - before-state-load.srm", "n"));
   CHECK(count_tagged(dir, 'b') == 6 && count_tagged(dir, 'a') == 3);
   CHECK(holds(ROOT "/two/backups/Game - 2026-11-03 10-00-07"
         " - 0007-abcd - Snes9x - before-state-load.srm", "a"));
}

/* This device's clock set back between its #5 and #6, another's #7
 * made in between: by each device's latest time, #7 is the oldest,
 * in whatever order the folder lists them. */
static void retention_cycle(void)
{
   unsigned k, pass;
   char name[160], dir[64];
   for (pass = 0; pass < 2; pass++)
   {
      snprintf(dir, sizeof(dir), ROOT "/cycle%u/backups", pass);
      for (k = 1; k <= 7; k++)
      {
         snprintf(name, sizeof(name), "%s/Game - 2026-11-03 12-00-0%u"
               " - %04u-cccc - Snes9x - before-state-load.srm", dir, k, k);
         put(name, "c");
      }
      snprintf(name, sizeof(name), "%s/Game - 2026-11-03 10-00-00"
            " - 0005-kqmz - Snes9x - before-state-load.srm", dir);
      put(name, "a5");
      snprintf(name, sizeof(name), "%s/Game - 2026-11-03 09-10-00"
            " - 0006-kqmz - Snes9x - before-state-load.srm", dir);
      put(name, "a6");
      snprintf(name, sizeof(name), "%s/Game - 2026-11-03 09-30-00"
            " - 0007-abcd - Snes9x - before-state-load.srm", dir);
      put(name, "b7");
      snprintf(name, sizeof(name), ROOT "/cycle%u", pass);
      save_backups_set_roots(name, NULL, NULL, NULL, NULL, NULL, "Snes9x");
      snprintf(name, sizeof(name), ROOT "/cycle%u/Game.srm", pass);
      set_time(13, 0, 0);
      list_reversed = pass == 1;
      CHECK(save_backups_copy_image(name, SAVE_BACKUP_BEFORE_STATE_LOAD,
            "n", 1, true, NULL) == SAVE_BACKUP_MADE);
      list_reversed = false;
      CHECK(count_files(dir) == 10);
      CHECK(count_tagged(dir, 'b') == 0 && count_tagged(dir, 'c') == 7);
      snprintf(name, sizeof(name), "%s/Game - 2026-11-03 09-10-00"
            " - 0006-kqmz - Snes9x - before-state-load.srm", dir);
      CHECK(holds(name, "a6"));
      snprintf(name, sizeof(name), "%s/Game - 2026-11-03 13-00-00"
            " - 0008-kqmz - Snes9x - before-state-load.srm", dir);
      CHECK(holds(name, "n"));
   }
}

/* retention_cycle with this device's latest time in a copy of another
 * reason: its #6 is held up to its cut #5's time all the same, so the
 * other device's #7 is the oldest. */
static void retention_cycle_reasons(void)
{
   unsigned k;
   char name[160];
   char dir[] = ROOT "/cyclecut/backups";
   for (k = 1; k <= 8; k++)
   {
      snprintf(name, sizeof(name), "%s/Game - 2026-11-03 12-00-0%u"
            " - %04u-cccc - Snes9x - before-state-load.srm", dir, k, k);
      put(name, "c");
   }
   snprintf(name, sizeof(name), "%s/Game - 2026-11-03 10-00-00"
         " - 0005-kqmz - Snes9x - cut.srm", dir);
   put(name, "a5");
   snprintf(name, sizeof(name), "%s/Game - 2026-11-03 09-30-00"
         " - 0007-abcd - Snes9x - before-state-load.srm", dir);
   put(name, "b7");
   snprintf(name, sizeof(name), "%s/Game - 2026-11-03 09-10-00"
         " - 0006-kqmz - Snes9x - before-state-load.srm", dir);
   put(name, "a6");
   save_backups_set_roots(ROOT "/cyclecut", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   set_time(13, 0, 0);
   CHECK(save_backups_copy_image(ROOT "/cyclecut/Game.srm",
         SAVE_BACKUP_BEFORE_STATE_LOAD, "n", 1, true, NULL)
         == SAVE_BACKUP_MADE);
   CHECK(count_files(dir) == 11);
   CHECK(count_tagged(dir, 'b') == 0 && count_tagged(dir, 'c') == 8);
   CHECK(holds(name, "a6"));
   CHECK(holds(ROOT "/cyclecut/backups/Game - 2026-11-03 13-00-00"
         " - 0009-kqmz - Snes9x - before-state-load.srm", "n"));
}

/* Two devices may give a copy the same number: two names, and the
 * next copy numbers past both. */
static void same_number_two_devices(void)
{
   char dir[] = ROOT "/samenum/backups";
   save_backups_set_roots(ROOT "/samenum", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   put(ROOT "/samenum/backups/Game - 2026-11-03 14-00-00"
         " - 0001-abcd - Snes9x - cut.srm", "o");
   use_device("kqmz");
   set_time(14, 0, 0);
   CHECK(save_backups_copy_image(ROOT "/samenum/Game.srm", SAVE_BACKUP_CUT,
         "1", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/samenum/backups/Game - 2026-11-03 14-00-00"
         " - 0002-kqmz - Snes9x - cut.srm", "1"));
   put(ROOT "/samenum/backups/Game - 2026-11-03 14-00-00"
         " - 0002-abcd - Snes9x - cut.srm", "o");
   CHECK(count_files(dir) == 3);
   CHECK(save_backups_copy_image(ROOT "/samenum/Game.srm", SAVE_BACKUP_CUT,
         "2", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/samenum/backups/Game - 2026-11-03 14-00-00"
         " - 0003-kqmz - Snes9x - cut.srm", "2"));
}

/* A number whose name is taken, here by a copy in flight, goes to the
 * next; 99 taken in a row fail the copy, never written over a file. */
static void backup_taken_number(void)
{
   unsigned k;
   char name[160];
   char dir[] = ROOT "/taken/backups";
   save_backups_set_roots(ROOT "/taken", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   set_time(5, 0, 0);
   CHECK(save_backups_copy_image(ROOT "/taken/Game.srm", SAVE_BACKUP_CUT,
         "1", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/taken/backups/Game - 2026-11-03 05-00-00 - 0001-kqmz - Snes9x - cut.srm", "1"));
   for (k = 2; k < 100; k++)
   {
      snprintf(name, sizeof(name), "%s/Game - 2026-11-03 05-00-00"
            " - %04u-kqmz - Snes9x - cut.srm.tmp", dir, k);
      put(name, "w");
   }
   CHECK(save_backups_copy_image(ROOT "/taken/Game.srm", SAVE_BACKUP_CUT,
         "2", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(holds(ROOT "/taken/backups/Game - 2026-11-03 05-00-00 - 0100-kqmz - Snes9x - cut.srm", "2"));
   for (k = 101; k < 200; k++)
   {
      snprintf(name, sizeof(name), "%s/Game - 2026-11-03 05-00-00"
            " - %04u-kqmz - Snes9x - cut.srm.tmp", dir, k);
      put(name, "w");
   }
   CHECK(save_backups_copy_image(ROOT "/taken/Game.srm", SAVE_BACKUP_CUT,
         "3", 1, true, NULL) == SAVE_BACKUP_FAILED);
   CHECK(!path_is_valid(ROOT "/taken/backups/Game - 2026-11-03 05-00-00 - 0200-kqmz - Snes9x - cut.srm"));
   CHECK(count_files(dir) == 2 + 98 + 99);
   CHECK(count_tagged(dir, 'w') == 98 + 99);
}

/* Names from before numbering and numbered ones with a device's tag
 * read back, and what a copy is named reads as made. */
static void backup_parse_tagged(void)
{
   unsigned seq = 0;
   struct save_backup_name p;
   char s[PATH_MAX_LENGTH];
   const char *zelda =
         "Zelda - Part 2 - 2026-11-03 10-00-00 - 0042-kqmz - Snes9x - cut.mcd";
   CHECK(save_backups_parse(zelda, &p));
   CHECK(p.seq == 42 && p.number == 1 && p.game_len == 14);
   CHECK(!strcmp(p.time, "2026-11-03 10-00-00"));
   CHECK(!strcmp(p.eff, p.time) && !strcmp(p.device, "kqmz"));
   CHECK(p.core_len == 6 && !strncmp(zelda + p.core_off, "Snes9x", 6));
   CHECK(p.reason == SAVE_BACKUP_CUT && !strcmp(zelda + p.ext_off, ".mcd"));
   CHECK(save_backups_parse("G - 2026-11-03 10-00-00 - 1-abcd - 42 - cut", &p));
   CHECK(p.seq == 1 && p.core_len == 2 && p.game_len == 1);
   CHECK(!strcmp(p.device, "abcd"));
   CHECK(save_backups_parse("G - 2026-11-03 10-00-00 - 12345-zyxw - S - cut", &p));
   CHECK(p.seq == 12345 && !strcmp(p.device, "zyxw"));
   CHECK(save_backups_parse(
         "G - 2026-11-03 10-00-00 - 999999999-kqmz - S - cut", &p));
   CHECK(p.seq == 999999999u);
   CHECK(!save_backups_parse(
         "G - 2026-11-03 10-00-00 - 1000000000-kqmz - S - cut", &p));
   CHECK(!save_backups_parse(
         "G - 2026-11-03 10-00-00 - 4294967295-kqmz - S - cut", &p));
   /* old names, one whose core is digits */
   CHECK(save_backups_parse("G - 2026-11-03 10-00-00-3 - 42 - cut", &p));
   CHECK(!p.seq && p.number == 3 && p.core_len == 2 && p.game_len == 1);
   CHECK(!p.device[0] && !strcmp(p.eff, p.time));
   CHECK(save_backups_parse("G - 2026-11-03 10-00-00 - S - cut", &p));
   CHECK(!p.seq && p.number == 1 && !p.device[0]);
   /* numbered with no tag, or not a tag */
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00 - 0042 - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00 - 999999999 - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00 - 0042-kqm - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00 - 0042-kqmzz - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00 - 0042-KQMZ - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00 - 0042-kqmi - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00 - 0042-kq1z - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00 - 0042_kqmz - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00 - -kqmz - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00 - kqmz - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00-kqmz - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00 - 4294967296-kqmz - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00 - 12345678901-kqmz - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00 - 0000-kqmz - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00 - 00x1-kqmz - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-03 10-00-00-2 - 0001-kqmz - S - cut", &p));
   CHECK(!save_backups_parse("G - 2026-11-3 10-00-00 - 0001-kqmz - S - cut", &p));
   CHECK(!save_backups_parse("2026-11-03 10-00-00 - 0001-kqmz - S - cut", &p));
   CHECK(!save_backups_parse(" - 0001-kqmz - S - cut", &p));

   save_backups_set_roots(ROOT "/parse", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   set_time(10, 0, 0);
   CHECK(save_backups_copy_image(ROOT "/parse/Zelda - Part 2.mcd",
         SAVE_BACKUP_CORE_CHANGED, "z", 1, true, &seq) == SAVE_BACKUP_MADE);
   snprintf(s, sizeof(s), ROOT "/parse/backups/"
         "Zelda - Part 2 - 2026-11-03 10-00-00 - %04u-kqmz - Snes9x"
         " - core-changed.mcd", seq);
   CHECK(seq == 1 && holds(s, "z"));
   CHECK(save_backups_parse(path_basename(s), &p));
   CHECK(p.seq == seq && p.game_len == 14 && !strcmp(p.time, "2026-11-03 10-00-00"));
   CHECK(p.reason == SAVE_BACKUP_CORE_CHANGED && !strcmp(p.device, "kqmz"));
}

/* A long game keeps one cut whatever the number's width, under the
 * name's limit; past the last number, a copy takes it again. */
static void backup_long_numbered(void)
{
   size_t i, n = 0;
   unsigned seq = 0;
   struct save_backup_name p;
   char game[260], path[300], name[PATH_MAX_LENGTH];
   char dir[] = ROOT "/longseq/backups";
   struct string_list *l;
   save_backups_set_roots(ROOT "/longseq", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   memset(game, 'L', 210);
   strcpy(game + 210, ".srm");
   snprintf(path, sizeof(path), ROOT "/longseq/%s", game);
   set_time(4, 0, 0);
   CHECK(save_backups_copy_image(path, SAVE_BACKUP_CUT, "1", 1, true, &seq)
         == SAVE_BACKUP_MADE);
   CHECK(seq == 1);
   cut_game(game, dir, "Snes9x", ".srm");
   snprintf(name, sizeof(name), "%s/%s - 2026-11-03 04-00-00 - 99999999-kqmz"
         " - Snes9x - cut.srm", dir, game);
   put(name, "h");
   CHECK(save_backups_copy_image(path, SAVE_BACKUP_CUT, "2", 1, true, &seq)
         == SAVE_BACKUP_MADE);
   CHECK(seq == 100000000u);
   snprintf(name, sizeof(name), "%s/%s - 2026-11-03 04-00-00 - 999999998-kqmz"
         " - Snes9x - cut.srm", dir, game);
   put(name, "h");
   CHECK(save_backups_copy_image(path, SAVE_BACKUP_CUT, "3", 1, true, &seq)
         == SAVE_BACKUP_MADE);
   CHECK(seq > 999999999u);
   set_time(4, 0, 1);
   CHECK(save_backups_copy_image(path, SAVE_BACKUP_CUT, "4", 1, true, &seq)
         == SAVE_BACKUP_MADE);
   CHECK(seq > 999999999u);
   snprintf(name, sizeof(name), "%s/%s - 2026-11-03 04-00-01 - 999999999-kqmz"
         " - Snes9x - cut.srm", dir, game);
   CHECK(holds(name, "4"));
   l = dir_list_new(dir, NULL, false, true, false, false);
   for (i = 0; l && i < l->size; i++)
   {
      const char *base = path_basename(l->elems[i].data);
      n++;
      CHECK(strlen(base) < SAVE_BACKUPS_NAME_MAX);
      CHECK(save_backups_parse(base, &p));
      CHECK(p.game_len == strlen(game) && !memcmp(base, game, p.game_len));
   }
   CHECK(n == 6);
   if (l)
      string_list_free(l);
}

/* A number past the last is no copy's: never pruned, and copies
 * number on from the others. */
static void backup_number_cap(void)
{
   unsigned k, seq;
   char text[8];
   char dir[] = ROOT "/cap/backups";
   const char *odd = ROOT "/cap/backups/"
         "Game - 2026-11-03 03-00-00 - 4294967295-kqmz - Snes9x - before-state-load.srm";
   save_backups_set_roots(ROOT "/cap", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   put(odd, "x");
   for (k = 0; k < 12; k++)
   {
      set_time(3, 1, (int)k);
      snprintf(text, sizeof(text), "c%u", k);
      seq = 0;
      CHECK(save_backups_copy_image(ROOT "/cap/Game.srm",
            SAVE_BACKUP_BEFORE_STATE_LOAD, text, strlen(text), true, &seq)
            == SAVE_BACKUP_MADE);
      CHECK(seq == k + 1);
   }
   CHECK(holds(odd, "x"));
   CHECK(count_tagged(dir, 'c') == 10);
}

/* Past the last number, copies take it again and their time orders
 * them: the newest are kept, and of them only the session's first
 * beside those. */
static void retention_at_cap(void)
{
   unsigned k, seq, first;
   char text[8], name[160];
   char dir[] = ROOT "/capk/backups";
   save_backups_set_roots(ROOT "/capk", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   for (k = 1; k < 10; k++)
   {
      snprintf(name, sizeof(name), "%s/Game - 2026-11-03 01-00-0%u"
            " - %04u-kqmz - Snes9x - before-state-load.srm", dir, k, k);
      put(name, "o");
   }
   put(ROOT "/capk/backups/Game - 2026-11-03 01-00-30 - 999999999-kqmz"
         " - Snes9x - before-state-load.srm", "r");
   set_time(1, 1, 0);
   seq = 0;
   CHECK(save_backups_copy_image(ROOT "/capk/Game.srm",
         SAVE_BACKUP_BEFORE_STATE_LOAD, "f", 1, true, &seq)
         == SAVE_BACKUP_MADE);
   CHECK(seq > 999999999u && seq <= 0x7FFFFFFFu);
   first = seq;
   for (k = 0; k < 12; k++)
   {
      set_time(1, 2, (int)k);
      snprintf(text, sizeof(text), "n%u", k);
      seq = first;
      CHECK(save_backups_copy_image(ROOT "/capk/Game.srm",
            SAVE_BACKUP_BEFORE_STATE_LOAD, text, strlen(text), true, &seq)
            == SAVE_BACKUP_MADE);
      CHECK(seq > 999999999u && seq != first);
   }
   CHECK(count_files(dir) == 11 && count_tagged(dir, 'n') == 10);
   CHECK(holds(ROOT "/capk/backups/Game - 2026-11-03 01-01-00 - 999999999-kqmz"
         " - Snes9x - before-state-load.srm", "f"));
   CHECK(holds(ROOT "/capk/backups/Game - 2026-11-03 01-02-10 - 999999999-kqmz"
         " - Snes9x - before-state-load.srm", "n10"));
   CHECK(holds(ROOT "/capk/backups/Game - 2026-11-03 01-02-11 - 999999999-kqmz"
         " - Snes9x - before-state-load.srm", "n11"));
   /* held by no one: by time, the oldest at the last number go */
   set_time(1, 3, 0);
   CHECK(save_backups_copy_image(ROOT "/capk/Game.srm",
         SAVE_BACKUP_BEFORE_STATE_LOAD, "z", 1, true, NULL)
         == SAVE_BACKUP_MADE);
   CHECK(count_files(dir) == 10);
   CHECK(count_tagged(dir, 'o') == 0 && count_tagged(dir, 'n') == 9);
   CHECK(holds(ROOT "/capk/backups/Game - 2026-11-03 01-02-11 - 999999999-kqmz"
         " - Snes9x - before-state-load.srm", "n11"));
}

/* A folder there but not listed fails the copy: numbered first, it
 * would be pruned first. */
static void backup_list_failed(void)
{
   char dir[] = ROOT "/nolist/backups";
   save_backups_set_roots(ROOT "/nolist", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   set_time(9, 0, 0);
   CHECK(save_backups_copy_image(ROOT "/nolist/Game.srm",
         SAVE_BACKUP_BEFORE_STATE_LOAD, "a", 1, true, NULL)
         == SAVE_BACKUP_MADE);
   set_time(9, 0, 1);
   list_fails = true;
   CHECK(save_backups_copy_image(ROOT "/nolist/Game.srm",
         SAVE_BACKUP_BEFORE_STATE_LOAD, "b", 1, true, NULL)
         == SAVE_BACKUP_FAILED);
   /* no folder yet: nothing to list */
   CHECK(save_backups_copy_image(ROOT "/nolist/sub/Game.srm",
         SAVE_BACKUP_BEFORE_STATE_LOAD, "c", 1, true, NULL)
         == SAVE_BACKUP_MADE);
   list_fails = false;
   CHECK(count_files(dir) == 1);
   CHECK(holds(ROOT "/nolist/backups/sub/"
         "Game - 2026-11-03 09-00-01 - 0001-kqmz - Snes9x - before-state-load.srm",
         "c"));
}

/* A game as long as the limit leaves stays whole: the name keeps room
 * for the widest number and the longest reason, no more. */
static void backup_name_room(void)
{
   char game[200], path[300], name[PATH_MAX_LENGTH];
   long n = game_limit(ROOT "/room/backups", "Snes9x", ".srm");
   save_backups_set_roots(ROOT "/room", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   CHECK(n > 0 && n < (long)sizeof(game));
   if (n <= 0 || n >= (long)sizeof(game))
      return;
   memset(game, 'R', (size_t)n);
   game[n] = '\0';
   snprintf(path, sizeof(path), ROOT "/room/%s.srm", game);
   set_time(2, 0, 0);
   CHECK(save_backups_copy_image(path, SAVE_BACKUP_CUT, "r", 1, true, NULL)
         == SAVE_BACKUP_MADE);
   snprintf(name, sizeof(name), ROOT "/room/backups/%s - 2026-11-03 02-00-00"
         " - 0001-kqmz - Snes9x - cut.srm", game);
   CHECK(holds(name, "r"));
   CHECK(strlen(path_basename(name)) + 5 + 14 < SAVE_BACKUPS_NAME_MAX);
}

#ifdef SAVE_BACKUPS_PATH_MAX
/* ROOT "/<name>/dd..d", with the d that make the path of a file in it,
 * up to its name, @len UTF-16 units long; false when the run folder is
 * too long for that. */
static bool deep_dir(char *dir, size_t size, const char *name, long len)
{
   long k;
   size_t _len = (size_t)snprintf(dir, size, ROOT "/%s", name);
   k           = len - full_units(dir) - 2;
   if (k < 1 || _len + 1 + (size_t)k >= size)
      return false;
   dir[_len++] = '/';
   memset(dir + _len, 'd', (size_t)k);
   dir[_len + k] = '\0';
   return true;
}

/* In a deep folder only the game is cut, at a character, to what fits
 * beside the mark: the copy is made and reads back, and its recovery
 * file is found. A file whose whole name is the cut's prefix and
 * another character is not taken for it. */
static void backup_deep(void)
{
   struct save_backup_name p;
   char game[160];
   char *q, *r;
   static char dir[512], path[PATH_MAX_LENGTH];
   static char s[PATH_MAX_LENGTH], t[PATH_MAX_LENGTH], u[PATH_MAX_LENGTH];
   /* 24 units for the game */
   CHECK(deep_dir(dir, sizeof(dir), "deep",
         SAVE_BACKUPS_PATH_MAX - 4 - 72 - 24));
   CHECK(game_limit(dir, "Snes9x", ".srm") == 24);
   set_time(21, 0, 0);
   memcpy(game, "Deep", 4);
   memset(game + 4, 'x', 19);
   strcpy(game + 23, "Q.srm");
   fill_pathname_join(path, dir, game, sizeof(path));
   CHECK(save_backups_copy_to(dir, path, "Snes9x", SAVE_BACKUP_CUT,
         "d1", 2, true) == SAVE_BACKUP_MADE);
   game[24] = '\0';
   snprintf(s, sizeof(s), "%s/%s - 2026-11-03 21-00-00 - 0001-kqmz - Snes9x"
         " - cut.srm", dir, game);
   CHECK(holds(s, "d1"));
   memset(game + 4, 'x', 100);
   strcpy(game + 104, ".srm");
   fill_pathname_join(path, dir, game, sizeof(path));
   CHECK(save_backups_copy_to(dir, path, "Snes9x", SAVE_BACKUP_CUT,
         "d1", 2, true) == SAVE_BACKUP_MADE);
   CHECK(save_backups_copy_to(dir, path, "Snes9x",
         SAVE_BACKUP_BEFORE_STATE_LOAD, "d2", 2, true) == SAVE_BACKUP_MADE);
   strcpy(game + 23, "~");
   snprintf(s, sizeof(s), "%s/%s - 2026-11-03 21-00-00 - 0001-kqmz - Snes9x"
         " - cut.srm", dir, game);
   CHECK(holds(s, "d1"));
   snprintf(s, sizeof(s), "%s/%s - 2026-11-03 21-00-00 - 0002-kqmz - Snes9x"
         " - before-state-load.srm", dir, game);
   CHECK(holds(s, "d2"));
   CHECK(full_units(s) + 4 <= SAVE_BACKUPS_PATH_MAX);
   CHECK(save_backups_parse(path_basename(s), &p));
   CHECK(p.game_len == 24 && p.seq == 2);
   CHECK(p.reason == SAVE_BACKUP_BEFORE_STATE_LOAD);

   /* a 4-byte character is 2 units */
   memset(game, 'e', 22);
   strcpy(game + 22, "\xF0\x9F\x98\x80 tail.srm");
   fill_pathname_join(path, dir, game, sizeof(path));
   CHECK(save_backups_copy_to(dir, path, "Snes9x", SAVE_BACKUP_CUT,
         "d3", 2, true) == SAVE_BACKUP_MADE);
   strcpy(game + 22, "~");
   snprintf(s, sizeof(s), "%s/%s - 2026-11-03 21-00-00 - 0001-kqmz - Snes9x"
         " - cut.srm", dir, game);
   CHECK(holds(s, "d3"));
   memset(game, 'f', 22);
   strcpy(game + 22, "\xF0\x9F\x98\x80.srm");
   fill_pathname_join(path, dir, game, sizeof(path));
   CHECK(save_backups_copy_to(dir, path, "Snes9x", SAVE_BACKUP_CUT,
         "d4", 2, true) == SAVE_BACKUP_MADE);
   game[26] = '\0';
   snprintf(s, sizeof(s), "%s/%s - 2026-11-03 21-00-00 - 0001-kqmz - Snes9x"
         " - cut.srm", dir, game);
   CHECK(holds(s, "d4"));

   save_backups_set_roots(dir, NULL, NULL, NULL, NULL, NULL, "Snes9x");
   memcpy(game, "Deep", 4);
   memset(game + 4, 'r', 100);
   strcpy(game + 104, ".srm");
   fill_pathname_join(path, dir, game, sizeof(path));
   CHECK(save_backups_recover(s, sizeof(s), path, "r1", 2, true));
   CHECK(save_backups_find_recovery(t, sizeof(t), path, INT64_MIN));
   CHECK(*s && same_path(s, t));
   /* a newer one beside it, whole: the cut's prefix and Q */
   strlcpy(u, s, sizeof(u));
   q = strrchr(u, '~');
   r = strstr(u, " 21-00-00 ");
   CHECK(q && r);
   if (q && r)
   {
      *q   = 'Q';
      r[8] = '1';
      put(u, "r1");
      CHECK(save_backups_find_recovery(t, sizeof(t), path, INT64_MIN));
      CHECK(*s && same_path(s, t));
   }
}

/* The shortest cut game is the mark alone. A copy whose name does not
 * fit even so is not made, and its save's copy stays owed. */
static void backup_no_room(void)
{
   struct save_backup_name p;
   static char dir[512], path[PATH_MAX_LENGTH];
   static char s[PATH_MAX_LENGTH];
   set_time(22, 0, 0);
   CHECK(deep_dir(dir, sizeof(dir), "room0", SAVE_BACKUPS_PATH_MAX - 4 - 72));
   CHECK(game_limit(dir, "Snes9x", ".srm") == 0);
   fill_pathname_join(path, dir, "Game.srm", sizeof(path));
   CHECK(save_backups_copy_to(dir, path, "Snes9x",
         SAVE_BACKUP_BEFORE_STATE_LOAD, "n1", 2, true) == SAVE_BACKUP_MADE);
   snprintf(s, sizeof(s),
         "%s/~ - 2026-11-03 22-00-00 - 0001-kqmz - Snes9x - before-state-load.srm",
         dir);
   CHECK(holds(s, "n1"));
   CHECK(save_backups_parse(path_basename(s), &p) && p.game_len == 1);

   /* past the room kept for the longest reason and a 9-digit number, a
    * shorter one fits */
   CHECK(deep_dir(dir, sizeof(dir), "room16",
         SAVE_BACKUPS_PATH_MAX - 4 - 72 + 16));
   fill_pathname_join(path, dir, "Game.srm", sizeof(path));
   CHECK(save_backups_copy_to(dir, path, "Snes9x", SAVE_BACKUP_CUT,
         "n2", 2, true) == SAVE_BACKUP_MADE);
   snprintf(s, sizeof(s), "%s/~ - 2026-11-03 22-00-00 - 0001-kqmz - Snes9x"
         " - cut.srm", dir);
   CHECK(holds(s, "n2"));
   CHECK(save_backups_copy_to(dir, path, "Snes9x",
         SAVE_BACKUP_BEFORE_STATE_LOAD, "n3", 2, true) == SAVE_BACKUP_FAILED);
   CHECK(count_files(dir) == 1);

   CHECK(deep_dir(dir, sizeof(dir), "noroom", SAVE_BACKUPS_PATH_MAX - 29));
   fill_pathname_join(path, dir, "Game.srm", sizeof(path));
   CHECK(save_backups_copy_to(dir, path, "Snes9x", SAVE_BACKUP_CUT,
         "n4", 2, true) == SAVE_BACKUP_FAILED);
   CHECK(count_files(dir) == 0);

   /* a path and its ".tmp" fit in 259 units, not 260; that name is 54 */
   CHECK(deep_dir(dir, sizeof(dir), "edge", SAVE_BACKUPS_PATH_MAX - 4 - 54));
   fill_pathname_join(path, dir, "Game.srm", sizeof(path));
   CHECK(save_backups_copy_to(dir, path, "Snes9x", SAVE_BACKUP_CUT,
         "n5", 2, true) == SAVE_BACKUP_MADE);
   snprintf(s, sizeof(s), "%s/~ - 2026-11-03 22-00-00 - 0001-kqmz - Snes9x"
         " - cut.srm", dir);
   CHECK(holds(s, "n5"));
   CHECK(full_units(s) + 4 == SAVE_BACKUPS_PATH_MAX);
   CHECK(deep_dir(dir, sizeof(dir), "edge1", SAVE_BACKUPS_PATH_MAX - 4 - 53));
   fill_pathname_join(path, dir, "Game.srm", sizeof(path));
   CHECK(save_backups_copy_to(dir, path, "Snes9x", SAVE_BACKUP_CUT,
         "n6", 2, true) == SAVE_BACKUP_FAILED);
   CHECK(count_files(dir) == 0);
}

/* Copies cut in a deep folder, the folder moved somewhere roomier: a
 * file named exactly their cut game prunes none of them. */
static void backup_moved(void)
{
   size_t _len = 0;
   struct save_backup_name p;
   char game[64];
   static char dir[512];
   struct string_list *l;
   CHECK(deep_dir(dir, sizeof(dir), "moved",
         SAVE_BACKUPS_PATH_MAX - 4 - 72 - 24));
   memset(game, 'g', 40);
   strcpy(game + 40, ".srm");
   make_copies(dir, game, "Snes9x", 'a', 12, 0);
   l = dir_list_new(dir, NULL, false, true, false, false);
   if (     l && l->size
         && save_backups_parse(path_basename(l->elems[0].data), &p)
         && p.game_len < 40)
   {
      _len = p.game_len;
      memcpy(game, path_basename(l->elems[0].data), _len);
   }
   CHECK(_len);
   strcpy(game + _len, ".srm");
   if (l)
      string_list_free(l);
   CHECK(!rename(dir, ROOT "/moved2"));
   make_copies(ROOT "/moved2", game, "Snes9x", 'c', 12, 1);
   CHECK(count_tagged(ROOT "/moved2", 'a') == 12);
   CHECK(count_tagged(ROOT "/moved2", 'c') == 12);
}

/* A VFS URL has no path limit; on Windows "/dir" is on a drive. */
static void backup_dir_units(void)
{
   CHECK(save_backups_dir_units("smb://host/share/saves") < 0);
   CHECK(save_backups_dir_units("C://sbt") == 8);
#ifdef _WIN32
   {
      static char s[PATH_MAX_LENGTH];
      strlcpy(s, "/sbt", sizeof(s));
      CHECK(path_resolve_realpath(s, sizeof(s), false));
      CHECK(save_backups_dir_units("/sbt") == units(s) + 1);
   }
#endif
}
#endif

static void recovery_chain(void)
{
   char s[PATH_MAX_LENGTH];
   save_backups_set_roots(ROOT "/rsaves", NULL, NULL, NULL,
         ROOT "/rconfig", ROOT "/rapp", "Snes9x");
   set_time(14, 0, 0);
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves/Game.srm", "r1", 2, true));
   CHECK(same_path(s, ROOT "/rsaves/backups/Game - 2026-11-03 14-00-00 - 0001-kqmz - Snes9x - unsaved.srm"));
   /* the same image: that file, nothing written */
   plain_writes = synced_writes = 0;
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves/Game.srm", "r1", 2, true));
   CHECK(same_path(s, ROOT "/rsaves/backups/Game - 2026-11-03 14-00-00 - 0001-kqmz - Snes9x - unsaved.srm"));
   CHECK(!plain_writes && !synced_writes);
   CHECK(count_files(ROOT "/rsaves/backups") == 1);
   /* another */
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves/Game.srm", "r5", 2, true));
   CHECK(same_path(s, ROOT "/rsaves/backups/Game - 2026-11-03 14-00-00 - 0002-kqmz - Snes9x - unsaved.srm"));
   CHECK(holds(ROOT "/rsaves/backups/Game - 2026-11-03 14-00-00 - 0001-kqmz - Snes9x - unsaved.srm", "r1"));
   CHECK(count_files(ROOT "/rsaves/backups") == 2);
   /* only the newest counts */
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves/Game.srm", "r1", 2, true));
   CHECK(same_path(s, ROOT "/rsaves/backups/Game - 2026-11-03 14-00-00 - 0003-kqmz - Snes9x - unsaved.srm"));
   CHECK(count_files(ROOT "/rsaves/backups") == 3);
   /* of the recovery files: a newer copy is not one */
   CHECK(save_backups_copy_image(ROOT "/rsaves/Game.srm", SAVE_BACKUP_CUT,
         "c9", 2, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves/Game.srm", "r1", 2, true));
   CHECK(same_path(s, ROOT "/rsaves/backups/Game - 2026-11-03 14-00-00 - 0003-kqmz - Snes9x - unsaved.srm"));
   CHECK(count_files(ROOT "/rsaves/backups") == 4);
   /* nor does a newer copy with the bytes make one needless */
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves/Game.srm", "c9", 2, true));
   CHECK(holds(s, "c9"));
   CHECK(count_files(ROOT "/rsaves/backups") == 5);

   /* the save folder can't be written: a file where its folder goes */
   put(ROOT "/rsaves2/backups", "file");
   save_backups_set_roots(ROOT "/rsaves2", NULL, NULL, NULL,
         ROOT "/rconfig", ROOT "/rapp", "Snes9x");
   CHECK(save_backups_copy_image(ROOT "/rsaves2/Game.srm",
         SAVE_BACKUP_CUT, "c", 1, true, NULL) == SAVE_BACKUP_FAILED);
   CHECK(count_files(ROOT "/rsaves2") == 1);
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves2/Game.srm", "r2", 2, true));
   CHECK(same_path(s, ROOT "/rconfig/recovery/Game - 2026-11-03 14-00-00 - 0001-kqmz - Snes9x - unsaved.srm"));
   CHECK(holds(s, "r2"));
   /* the same rule in the fallback */
   plain_writes = synced_writes = 0;
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves2/Game.srm", "r2", 2, true));
   CHECK(same_path(s, ROOT "/rconfig/recovery/Game - 2026-11-03 14-00-00 - 0001-kqmz - Snes9x - unsaved.srm"));
   CHECK(!plain_writes && !synced_writes);
   CHECK(count_files(ROOT "/rconfig/recovery") == 1);

   /* nor the config folder */
   put(ROOT "/rconfig2/recovery", "file");
   save_backups_set_roots(ROOT "/rsaves2", NULL, NULL, NULL,
         ROOT "/rconfig2", ROOT "/rapp", "Snes9x");
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves2/Game.srm", "r3", 2, true));
   CHECK(same_path(s, ROOT "/rapp/recovery/Game - 2026-11-03 14-00-00 - 0001-kqmz - Snes9x - unsaved.srm"));
   CHECK(holds(s, "r3"));
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves2/Game.srm", "r3", 2, true));
   CHECK(same_path(s, ROOT "/rapp/recovery/Game - 2026-11-03 14-00-00 - 0001-kqmz - Snes9x - unsaved.srm"));
   CHECK(count_files(ROOT "/rapp/recovery") == 1);

   /* and no app data folder (consoles) */
   save_backups_set_roots(ROOT "/rsaves2", NULL, NULL, NULL,
         ROOT "/rconfig2", NULL, "Snes9x");
   CHECK(!save_backups_recover(s, sizeof(s), ROOT "/rsaves2/Game.srm", "r4", 2, true));
   CHECK(!*s);
   CHECK(count_files(ROOT "/rsaves2") == 1);
   CHECK(count_files(ROOT "/rconfig2") == 1);
   CHECK(count_files(ROOT "/rapp/recovery") == 1);

   /* the save folder works again: the newest is still the fallback's */
   put(ROOT "/rsaves3/backups", "file");
   save_backups_set_roots(ROOT "/rsaves3", NULL, NULL, NULL,
         ROOT "/rconfig3", NULL, "Snes9x");
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves3/Game.srm", "r6", 2, true));
   CHECK(same_path(s, ROOT "/rconfig3/recovery/Game - 2026-11-03 14-00-00 - 0001-kqmz - Snes9x - unsaved.srm"));
   CHECK(filestream_delete(ROOT "/rsaves3/backups") == 0);
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/rsaves3/Game.srm", "r6", 2, true));
   CHECK(same_path(s, ROOT "/rconfig3/recovery/Game - 2026-11-03 14-00-00 - 0001-kqmz - Snes9x - unsaved.srm"));
   CHECK(!path_is_valid(ROOT "/rsaves3/backups"));
}

/* Times in whole seconds tie: the numbers in the names decide. */
static void recovery_newest_tie(void)
{
   static char made[8][PATH_MAX_LENGTH];
   char s[PATH_MAX_LENGTH];
   char name[PATH_MAX_LENGTH];
   char image[2];
   int64_t t = 0;
   unsigned i;
   save_backups_set_roots(ROOT "/tie", NULL, NULL, NULL, NULL, NULL, "Snes9x");
   set_time(14, 30, 0);
   image[1] = '\0';
   for (i = 0; i < 8; i++)
   {
      image[0] = (char)('a' + i);
      CHECK(save_backups_recover(s, sizeof(s), ROOT "/tie/Game.srm",
            image, 1, false));
      if (!i)
         CHECK(path_get_mtime(s, &t));
      CHECK(path_set_mtime(s, t));
   }
   for (i = 8; i-- > 1;)
   {
      CHECK(save_backups_find_recovery(s, sizeof(s), ROOT "/tie/Game.srm",
            INT64_MIN));
      snprintf(name, sizeof(name),
            ROOT "/tie/backups/Game - 2026-11-03 14-30-00 - %04u-kqmz - Snes9x - unsaved.srm",
            i + 1);
      CHECK(same_path(s, name));
      CHECK(filestream_delete(s) == 0);
   }
   /* the times decide first, however the folder lists its files */
   save_backups_set_roots(ROOT "/tie2", NULL, NULL, NULL, NULL, NULL, "Snes9x");
   for (i = 0; i < 8; i++)
   {
      image[0] = (char)('a' + i);
      CHECK(save_backups_recover(made[i], sizeof(made[i]),
            ROOT "/tie2/Game.srm", image, 1, false));
   }
   for (i = 0; i < 8; i++)
   {
      CHECK(path_set_mtime(made[i], t + 100 + i));
      CHECK(save_backups_find_recovery(s, sizeof(s), ROOT "/tie2/Game.srm",
            INT64_MIN));
      CHECK(same_path(s, made[i]));
   }
   /* not newer than @mtime on a tie */
   CHECK(!save_backups_find_recovery(s, sizeof(s), ROOT "/tie2/Game.srm",
         t + 107));
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
         "1", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(synced_writes == 1 && plain_writes == 0);
   CHECK(save_backups_copy_image(ROOT "/sync/Game.srm", SAVE_BACKUP_CUT,
         "2", 1, false, NULL) == SAVE_BACKUP_MADE);
   CHECK(synced_writes == 1 && plain_writes == 1);
   put(ROOT "/sync/File.srm", "f1");
   CHECK(save_backups_copy_file(ROOT "/sync/File.srm", SAVE_BACKUP_CUT,
         true, NULL) == SAVE_BACKUP_MADE);
   CHECK(synced_writes == 2 && plain_writes == 1);
   put(ROOT "/sync/File.srm", "f2");
   CHECK(save_backups_copy_file(ROOT "/sync/File.srm", SAVE_BACKUP_CUT,
         false, NULL) == SAVE_BACKUP_MADE);
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
         "s", 1, false));
   CHECK(synced_writes == 4 && plain_writes == 4);
   /* nothing written for a copy the newest one already holds */
   CHECK(save_backups_copy_image(ROOT "/sync/Game.srm", SAVE_BACKUP_CUT,
         "s", 1, true, NULL) == SAVE_BACKUP_MADE);
   CHECK(synced_writes == 4 && plain_writes == 4);
}

/* A held image may have been written without a sync: a caller that
 * asks for one gets it, its folder too, and a new file when it fails. */
static void recovery_held_sync(void)
{
   char s[PATH_MAX_LENGTH];
   char first[PATH_MAX_LENGTH];
   save_backups_set_roots(ROOT "/held", NULL, NULL, NULL, NULL, NULL,
         "Snes9x");
   set_time(16, 0, 0);
   plain_writes = synced_writes = file_syncs = dir_syncs = 0;
   CHECK(save_backups_recover(first, sizeof(first), ROOT "/held/Game.srm",
         "h", 1, false));
   CHECK(plain_writes == 1 && !synced_writes && !file_syncs && !dir_syncs);
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/held/Game.srm",
         "h", 1, false));
   CHECK(same_path(s, first));
   CHECK(plain_writes == 1 && !synced_writes && !file_syncs && !dir_syncs);
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/held/Game.srm",
         "h", 1, true));
   CHECK(same_path(s, first));
   CHECK(plain_writes == 1 && !synced_writes && file_syncs == 1);
   CHECK(dir_syncs == 1);
   CHECK(holds(first, "h"));
   sync_fails = true;
   CHECK(save_backups_recover(s, sizeof(s), ROOT "/held/Game.srm",
         "h", 1, true));
   sync_fails = false;
   CHECK(!same_path(s, first));
   CHECK(synced_writes == 1 && file_syncs == 2 && dir_syncs == 1);
   CHECK(count_files(ROOT "/held/backups") == 2);
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
         "1", 1, true, NULL) == SAVE_BACKUP_FAILED);
   CHECK(!save_backups_recover(s, sizeof(s), ROOT "/notime/Game.srm",
         "r", 1, true));
   CHECK(!*s);
   strftime_fails = false;
   CHECK(!path_is_valid(ROOT "/notime"));
   CHECK(!path_is_valid(ROOT "/notime_config"));
}

static unsigned count_of(const char *path, const char *text)
{
   void *buf    = NULL;
   int64_t len  = 0;
   unsigned n   = 0;
   const char *p;
   if (!filestream_read_file(path, &buf, &len))
      return 0;
   for (p = (const char*)buf; (p = strstr(p, text)); p++)
      n++;
   free(buf);
   return n;
}

/* save_folders.json: one entry per set of roots, its last_seen moved
 * once a day, a note for each fallback (once a day), this device's tag,
 * and no write when nothing changed. */
static void folders_list(void)
{
   const char *list = ROOT "/config/" SAVE_BACKUPS_FOLDERS_FILE;
   CHECK(path_mkdir(ROOT "/config"));
   roots();
   set_time(10, 0, 0);
   plain_writes = synced_writes = 0;
   CHECK(save_backups_note_folders(NULL, NULL, false));
   CHECK(plain_writes == 1);
   CHECK(count_of(list, "\"saves\": ") == 1);
   CHECK(count_of(list, "{\"device\": \"kqmz\", \"first_seen\": ") == 1);
   CHECK(count_of(list, "\"first_seen\": \"2026-11-03T10:00:00\"") == 2);

   set_time(11, 0, 0);
   CHECK(save_backups_note_folders(NULL, NULL, false));
   CHECK(plain_writes == 1);

   test_tm.tm_mday = 4;
   CHECK(save_backups_note_folders(NULL, NULL, false));
   CHECK(plain_writes == 2);
   CHECK(count_of(list, "\"saves\": ") == 1);
   CHECK(count_of(list, "\"first_seen\": \"2026-11-03T10:00:00\"") == 2);
   CHECK(count_of(list, "\"last_seen\": \"2026-11-04T11:00:00\"") == 1);

   save_backups_set_roots(ROOT "/default", ROOT "/states", ROOT "/roms",
         ROOT "/system", ROOT "/config", ROOT "/appdata", "Snes9x");
   CHECK(save_backups_note_folders("/card/saves", ROOT "/default", true));
   CHECK(save_backups_note_folders("/card/saves", ROOT "/default", true));
   CHECK(plain_writes == 2);
   CHECK(synced_writes == 1);
   CHECK(count_of(list, "\"saves\": ") == 2);
   CHECK(count_of(list, "\"configured\": \"/card/saves\"") == 1);
   CHECK(count_of(list, "\"used\": ") == 1);
   roots();
}

/* A save_folders.json that does not read whole is moved aside, never
 * written over: its entries stay, a second one beside the first. */
static void save_folders_corrupt_kept(void)
{
   const char *list = ROOT "/config/" SAVE_BACKUPS_FOLDERS_FILE;
   const char *bad  = "[\n  {\"saves\": \"/old/saves\", "
         "\"states\": \"/old/states\"},\n  {\"saves\": ";
   char aside[PATH_MAX_LENGTH], aside2[PATH_MAX_LENGTH];
   fill_pathname_aside(aside, list, 1, sizeof(aside));
   fill_pathname_aside(aside2, list, 2, sizeof(aside2));
   roots();
   set_time(10, 0, 0);
   put(list, bad);
   CHECK(save_backups_note_folders(NULL, NULL, false));
   CHECK(holds(aside, bad));
   CHECK(count_of(list, "\"saves\": \"/old/saves\"") == 1);
   CHECK(count_of(list, "\"saves\": ") == 2);
   put(list, bad);
   CHECK(save_backups_note_folders(NULL, NULL, false));
   CHECK(holds(aside, bad) && holds(aside2, bad));
   CHECK(count_of(list, "\"saves\": ") == 2);
   filestream_delete(aside);
   filestream_delete(aside2);

   /* not moved aside, nothing written; not written, put back */
   put(list, bad);
   rename_fails = true;
   CHECK(!save_backups_note_folders(NULL, NULL, false));
   rename_fails = false;
   CHECK(holds(list, bad) && !path_is_valid(aside));
   write_fails = true;
   CHECK(!save_backups_note_folders(NULL, NULL, false));
   write_fails = false;
   CHECK(holds(list, bad) && !path_is_valid(aside));
   /* there but not read: kept aside as well */
   read_fails = true;
   CHECK(save_backups_note_folders(NULL, NULL, false));
   read_fails = false;
   CHECK(holds(aside, bad) && count_of(list, "\"saves\": ") == 1);
   filestream_delete(aside);
}

/* The newest copy's device in @dir */
static void copy_device(char *t, const char *dir)
{
   size_t i;
   unsigned best = 0;
   struct save_backup_name p;
   struct string_list *l = dir_list_new(dir, NULL, false, true, false,
         false);
   t[0] = '\0';
   for (i = 0; l && i < l->size; i++)
      if (     save_backups_parse(path_basename(l->elems[i].data), &p)
            && p.seq > best)
      {
         best = p.seq;
         strlcpy(t, p.device, 5);
      }
   if (l)
      string_list_free(l);
}

/* This device's tag: made at the first copy, kept in save_folders.json
 * and read back from it; its rewrites keep it; one there but not read
 * is set aside and another tag kept; one not written is tried again,
 * the copies' tag the same meanwhile. */
static void device_tag(void)
{
   char t[5], u[5], pat[32], aside[PATH_MAX_LENGTH];
   const char *list = ROOT "/dev1/" SAVE_BACKUPS_FOLDERS_FILE;
   const char *bad  = "[\n  {\"device\": ";
   fill_pathname_aside(aside, list, 1, sizeof(aside));
   use_device(NULL);
   save_backups_set_roots(ROOT "/devsaves", NULL, NULL, NULL, ROOT "/dev1",
         NULL, "Snes9x");
   CHECK(!retro_atomic_load_acquire_int(&save_backups_device));
   set_time(15, 0, 0);
   CHECK(save_backups_copy_image(ROOT "/devsaves/Game.srm", SAVE_BACKUP_CUT,
         "1", 1, true, NULL) == SAVE_BACKUP_MADE);
   copy_device(t, ROOT "/devsaves/backups");
   CHECK(strlen(t) == 4 && save_backups_is_tag(t));
   CHECK(path_mkdir(ROOT "/dev1"));
   CHECK(save_backups_note_folders(NULL, NULL, false));
   snprintf(pat, sizeof(pat), "\"device\": \"%s\"", t);
   CHECK(count_of(list, pat) == 1 && save_backups_device_kept);

   use_device(NULL);
   save_backups_set_roots(ROOT "/devsaves2", NULL, NULL, NULL, ROOT "/dev1",
         NULL, "Snes9x");
   save_backups_device_get(u);
   CHECK(!strcmp(u, t) && save_backups_device_kept);
   CHECK(save_backups_note_folders(NULL, NULL, false));
   CHECK(count_of(list, pat) == 1 && count_of(list, "\"saves\": ") == 2);

   put(list, bad);
   use_device(NULL);
   save_backups_set_roots(ROOT "/devsaves", NULL, NULL, NULL, ROOT "/dev1",
         NULL, "Snes9x");
   CHECK(!retro_atomic_load_acquire_int(&save_backups_device));
   CHECK(save_backups_note_folders(NULL, NULL, false));
   CHECK(holds(aside, bad));
   save_backups_device_get(u);
   snprintf(pat, sizeof(pat), "\"device\": \"%s\"", u);
   CHECK(count_of(list, pat) == 1 && save_backups_device_kept);
   filestream_delete(aside);

   filestream_delete(list);
   use_device(NULL);
   save_backups_set_roots(ROOT "/devsaves", NULL, NULL, NULL, ROOT "/dev1",
         NULL, "Snes9x");
   set_time(15, 0, 1);
   CHECK(save_backups_copy_image(ROOT "/devsaves/Game.srm", SAVE_BACKUP_CUT,
         "2", 1, true, NULL) == SAVE_BACKUP_MADE);
   copy_device(t, ROOT "/devsaves/backups");
   write_fails = true;
   CHECK(!save_backups_note_folders(NULL, NULL, false));
   write_fails = false;
   CHECK(!path_is_valid(list) && !save_backups_device_kept);
   save_backups_device_get(u);
   CHECK(!strcmp(u, t));
   CHECK(save_backups_note_folders(NULL, NULL, false));
   snprintf(pat, sizeof(pat), "\"device\": \"%s\"", t);
   CHECK(count_of(list, pat) == 1 && save_backups_device_kept);
   use_device("kqmz");
}

/* save_folders.json's tag, written by another instance, replaces the
 * one made in memory, whether set_roots() or note_folders() reads it:
 * copies carry the file's tag from then on, and the file keeps one. */
static void device_tag_adopted(void)
{
   char t[5];
   void *buf        = NULL;
   int64_t len      = 0;
   const char *list = ROOT "/dev2/" SAVE_BACKUPS_FOLDERS_FILE;
   const char *late = ROOT "/dev3/" SAVE_BACKUPS_FOLDERS_FILE;
   const char *pat  = "\"device\": \"wxyz\"";
   use_device("wxyz");
   save_backups_set_roots(ROOT "/adsaves", NULL, NULL, NULL, ROOT "/dev2",
         NULL, "Snes9x");
   CHECK(path_mkdir(ROOT "/dev2"));
   CHECK(save_backups_note_folders(NULL, NULL, false));
   CHECK(count_of(list, pat) == 1);

   use_device("abcd");
   save_backups_set_roots(ROOT "/adsaves", NULL, NULL, NULL, ROOT "/dev2",
         NULL, "Snes9x");
   set_time(16, 0, 0);
   CHECK(save_backups_copy_image(ROOT "/adsaves/Game.srm", SAVE_BACKUP_CUT,
         "1", 1, true, NULL) == SAVE_BACKUP_MADE);
   copy_device(t, ROOT "/adsaves/backups");
   CHECK(!strcmp(t, "wxyz"));

   /* Written after set_roots() found none */
   use_device("abcd");
   save_backups_set_roots(ROOT "/adsaves", NULL, NULL, NULL, ROOT "/dev3",
         NULL, "Snes9x");
   CHECK(filestream_read_file(list, &buf, &len));
   put(late, (const char*)buf);
   free(buf);
   CHECK(save_backups_note_folders(NULL, NULL, false));
   set_time(16, 0, 1);
   CHECK(save_backups_copy_image(ROOT "/adsaves/Game.srm", SAVE_BACKUP_CUT,
         "2", 1, true, NULL) == SAVE_BACKUP_MADE);
   copy_device(t, ROOT "/adsaves/backups");
   CHECK(!strcmp(t, "wxyz"));
   CHECK(count_of(late, pat) == 1 && count_of(late, "\"device\"") == 1);
   use_device("kqmz");
}

int main(void)
{
#ifdef SAVE_BACKUPS_PATH_MAX
   char core[41];
#ifdef RUN_UNITS
   if (full_units(ROOT) > RUN_UNITS)
      path_max += full_units(ROOT) - RUN_UNITS;
#endif
   memset(core, 'k', 40);
   core[40] = '\0';
   /* long4's game has the least room */
   if (game_limit(ROOT "/long4", core, ".srm") < 4)
   {
      puts("FAIL save_backups_test: run folder too long");
      return 1;
   }
#endif
   rm_tree(ROOT);
   CHECK(path_mkdir(ROOT));
   /* This device's tag in the names below */
   put(ROOT "/device/" SAVE_BACKUPS_FOLDERS_FILE,
         "[\n  {\"device\": \"kqmz\"}\n]\n");
   save_backups_set_roots(NULL, NULL, NULL, NULL, ROOT "/device", NULL, NULL);
   backup_folders();
   backup_system_fallback();
   backup_names();
   backup_core_chars();
   backup_collisions();
   backup_identical_skip();
   backup_retention();
   backup_retention_not_loaded();
   backup_held();
   backup_retention_cut();
   backup_retention_long();
   backup_retention_mark();
   retention_keeps_first_of_session();
   retention_clock_back();
   retention_mixed_names();
   retention_two_devices();
   retention_cycle();
   retention_cycle_reasons();
   same_number_two_devices();
   backup_taken_number();
   backup_parse_tagged();
   backup_long_numbered();
   backup_number_cap();
   retention_at_cap();
   backup_list_failed();
   backup_name_room();
#ifdef SAVE_BACKUPS_PATH_MAX
   backup_deep();
   backup_no_room();
   backup_moved();
   backup_dir_units();
#endif
   recovery_chain();
   recovery_newest_tie();
   backup_sync();
   recovery_held_sync();
   backup_no_time();
   folders_list();
   save_folders_corrupt_kept();
   device_tag();
   device_tag_adopted();
   save_backups_set_roots(NULL, NULL, NULL, NULL, NULL, NULL, NULL);
   rm_tree(ROOT);
   puts(failures ? "FAIL save_backups_test" : "PASS save_backups_test");
   return failures ? 1 : 0;
}
