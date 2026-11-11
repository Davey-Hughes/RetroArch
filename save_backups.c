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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <compat/strl.h>
#include <encodings/crc32.h>
#if defined(HAVE_CRYPTO)
#include <crypto/kdf.h>
#endif
#include <file/file_path.h>
#include <formats/rjson.h>
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <retro_atomic.h>
#include <retro_miscellaneous.h>
#include <streams/file_stream.h>
#if defined(HAVE_COMPRESSION)
#include <streams/rzip_stream.h>
#endif
#include <string/stdstring.h>
#include <time/rtime.h>
#include <vfs/vfs_implementation.h>

#include "save_backups.h"

/* A whole name stays under this many bytes. */
#define SAVE_BACKUPS_NAME_MAX    200
/* Room kept in every name for the longest reason word, so the game is
 * cut the same way for all of one file's copies. */
#define SAVE_BACKUPS_REASON_ROOM 17
/* And for a copy's number at its widest and its device's tag:
 * " - ", 9 digits, "-" and 4 letters */
#define SAVE_BACKUPS_SEQ_ROOM    17
#define SAVE_BACKUPS_TAG_LEN     4
/* Past it, a number is no copy's */
#define SAVE_BACKUPS_SEQ_MAX     999999999u
/* Numbers a copy tries past taken names before it fails */
#define SAVE_BACKUPS_SEQ_TRIES   99
/* Most a cut steps back from the limit, to a UTF-8 boundary. */
#define SAVE_BACKUPS_CUT_ROOM    3
#define SAVE_BACKUPS_TIME_LEN    19
/* A copy is written through "<path>.tmp" */
#define SAVE_BACKUPS_TEMP_LEN    4
/* A copy of a file is read and written this much at a time. */
#define SAVE_BACKUPS_COPY_PIECE  (1024 * 1024)
/* Ends a game cut to fit its path, so retention can tell, wherever the
 * copy is later. */
#define SAVE_BACKUPS_CUT_MARK    '~'

#ifndef SAVE_BACKUPS_PATH_MAX
#if defined(_WIN32) && !defined(_XBOX)
/* MAX_PATH less its NUL, in UTF-16 units: Win32 opens and UWP renames
 * take plain paths. */
#define SAVE_BACKUPS_PATH_MAX    259
#endif
#endif

enum save_backups_root
{
   SAVE_BACKUPS_ROOT_SAVEFILE = 0,
   SAVE_BACKUPS_ROOT_SAVESTATE,
   SAVE_BACKUPS_ROOT_CONTENT,
   SAVE_BACKUPS_ROOT_SYSTEM,
   SAVE_BACKUPS_ROOT_CONFIG,
   SAVE_BACKUPS_ROOT_APP_DATA,
   SAVE_BACKUPS_ROOT_CORE,
   SAVE_BACKUPS_ROOT_LAST
};

static const char *const save_backups_reasons[SAVE_BACKUP_REASON_LAST] =
{
   "before-state-load",
   "unreadable",
   "cut",
   "changed-elsewhere",
   "not-loaded",
   "core-changed",
   "other-content",
   "blanked",
   "unsaved",
   "replaced",
   "deleted"
};

static char *save_backups_roots[SAVE_BACKUPS_ROOT_LAST];

/* A device's tag: Crockford's base32 letters, lowercase, so it never
 * reads as a number nor changes on a case-blind drive */
static const char save_backups_tag_letters[] = "abcdefghjkmnpqrstvwxyz";
/* This device's tag, its letters packed; 0 until read or made */
static retro_atomic_int_t save_backups_device;
/* That tag is save_folders.json's */
static bool save_backups_device_kept;

static void save_backups_device_read(void);

/* @t starts with a device's tag */
static bool save_backups_is_tag(const char *t)
{
   unsigned i;
   for (i = 0; i < SAVE_BACKUPS_TAG_LEN; i++)
      if (!t[i] || !strchr(save_backups_tag_letters, t[i]))
         return false;
   return true;
}

static int save_backups_tag_pack(const char *t)
{
   return (int)t[0] | (int)t[1] << 8 | (int)t[2] << 16 | (int)t[3] << 24;
}

/* This device's tag into @s: as read, else one made now, which the
 * next save_backups_note_folders() keeps */
static void save_backups_device_get(char *s)
{
   unsigned i;
   int v = retro_atomic_load_acquire_int(&save_backups_device);
   if (!v)
   {
      unsigned char r[SAVE_BACKUPS_TAG_LEN];
      bool got = false;
      char t[SAVE_BACKUPS_TAG_LEN];
#if defined(HAVE_CRYPTO)
      got = crypto_random_bytes(r, sizeof(r)) == 0;
#endif
      if (!got)
      {
         unsigned x = (unsigned)time(NULL) ^ ((unsigned)clock() << 16)
            ^ (unsigned)(size_t)&v;
         for (i = 0; i < SAVE_BACKUPS_TAG_LEN; i++)
         {
            x    = x * 1103515245u + 12345u;
            r[i] = (unsigned char)(x >> 16);
         }
      }
      for (i = 0; i < SAVE_BACKUPS_TAG_LEN; i++)
         t[i] = save_backups_tag_letters[r[i]
            % (sizeof(save_backups_tag_letters) - 1)];
      /* Another thread's may be in first */
      v = save_backups_tag_pack(t);
      if (!retro_atomic_cas_int(&save_backups_device, 0, v))
         v = retro_atomic_load_acquire_int(&save_backups_device);
   }
   for (i = 0; i < SAVE_BACKUPS_TAG_LEN; i++)
      s[i] = (char)((unsigned)v >> (8 * i) & 0xFF);
   s[SAVE_BACKUPS_TAG_LEN] = '\0';
}

/* Every file this helper writes goes through here. */
static bool save_backups_write_file(const char *path,
      const void *data, int64_t len, bool sync)
{
   if (sync)
      return filestream_write_file_atomic_synced(path, data, len);
   return filestream_write_file_atomic(path, data, len);
}

bool save_backups_copy_synced(const char *path, const char *to)
{
   int ret;
   int64_t got  = 0;
   bool ok      = false;
   void *buf    = malloc(SAVE_BACKUPS_COPY_PIECE);
   /* First: it may hold the name already */
   RFILE *out   = filestream_open_atomic(to);
   RFILE *in    = buf && out ? filestream_open(path,
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_SEQUENTIAL_BULK) : NULL;
   if (out)
   {
      ok = in != NULL;
      while (ok && (got = filestream_read(in, buf,
                  SAVE_BACKUPS_COPY_PIECE)) > 0)
         ok = filestream_write(out, buf, got) == got;
      if (got < 0)
         ok = false;
      if (filestream_close(out) != 0)
         ok = false;
      /* Written but not renamed into place */
      if ((ret = filestream_finish_atomic_synced(to, ok)) == 1)
      {
         size_t _len = strlen(to) + SAVE_BACKUPS_TEMP_LEN + 1;
         char *temp  = (char*)malloc(_len);
         if (temp && filestream_atomic_temp_name(temp, _len, to))
            filestream_delete(temp);
         free(temp);
      }
      ok = ret == 0;
   }
   if (in)
      filestream_close(in);
   free(buf);
   return ok;
}

bool save_backups_move_finish(const char *path, const char *to)
{
   if (filestream_delete(path) != 0 && path_is_valid(path))
   {
      filestream_delete(to);
      return false;
   }
   return true;
}

size_t save_backups_held_name(char *s, size_t len, const char *path,
      unsigned n)
{
   size_t _len;
   char *base;
   if (n < 1 || n > SAVE_BACKUPS_HELD_MAX || !len
         || !(base = (char*)malloc(len)))
      return 0;
   _len = strlcpy(base, path, len);
   if (_len < len)
      _len += strlcpy(base + _len, ".not-loaded", len - _len);
   _len = _len < len ? fill_pathname_aside(s, base, n, len) : 0;
   free(base);
   return _len;
}

bool save_backups_held(const char *path)
{
   unsigned n;
   char *s;
   bool found = false;
   size_t _len = strlen(path);
   if (     _len < STRLEN_CONST(".state.auto")
         || !string_is_equal(path + _len - STRLEN_CONST(".state.auto"),
            ".state.auto")
         || !(s = (char*)malloc(PATH_MAX_LENGTH)))
      return false;
   for (n = 1; !found && n <= SAVE_BACKUPS_HELD_MAX; n++)
      found =    save_backups_held_name(s, PATH_MAX_LENGTH, path, n)
              && path_is_valid(s);
   free(s);
   return found;
}

void save_backups_set_roots(const char *savefile_dir,
      const char *savestate_dir, const char *content_dir,
      const char *system_dir, const char *config_dir,
      const char *app_data_dir, const char *core)
{
   unsigned i;
   const char *in[SAVE_BACKUPS_ROOT_LAST];
   in[SAVE_BACKUPS_ROOT_SAVEFILE]  = string_is_empty(savefile_dir)
      ? content_dir : savefile_dir;
   in[SAVE_BACKUPS_ROOT_SAVESTATE] = savestate_dir;
   in[SAVE_BACKUPS_ROOT_CONTENT]   = content_dir;
   in[SAVE_BACKUPS_ROOT_SYSTEM]    = system_dir;
   in[SAVE_BACKUPS_ROOT_CONFIG]    = config_dir;
   in[SAVE_BACKUPS_ROOT_APP_DATA]  = app_data_dir;
   in[SAVE_BACKUPS_ROOT_CORE]      = core;
   for (i = 0; i < SAVE_BACKUPS_ROOT_LAST; i++)
   {
      free(save_backups_roots[i]);
      save_backups_roots[i] = string_is_empty(in[i]) ? NULL : strdup(in[i]);
   }
   if (!save_backups_device_kept)
      save_backups_device_read();
}

/* The part of @path below @root, or NULL when it is not under it. */
static const char *save_backups_below(const char *root, const char *path)
{
   size_t i;
   if (!root)
      return NULL;
   for (i = 0; root[i]; i++)
      if (     root[i] != path[i]
            && !(PATH_CHAR_IS_SLASH(root[i]) && PATH_CHAR_IS_SLASH(path[i])))
         return NULL;
   if (!PATH_CHAR_IS_SLASH(root[i - 1]))
   {
      if (!PATH_CHAR_IS_SLASH(path[i]))
         return NULL;
      i++;
   }
   while (PATH_CHAR_IS_SLASH(path[i]))
      i++;
   return path + i;
}

size_t save_backups_dir(char *s, size_t len, const char *path)
{
   unsigned i;
   size_t _len;
   size_t root_len  = 0;
   int which        = -1;
   const char *rest = NULL;
   const char *base;

   s[0] = '\0';
   if (string_is_empty(path))
      return 0;
   for (i = SAVE_BACKUPS_ROOT_SAVEFILE; i <= SAVE_BACKUPS_ROOT_SYSTEM; i++)
   {
      const char *below = save_backups_below(save_backups_roots[i], path);
      if (below && strlen(save_backups_roots[i]) > root_len)
      {
         root_len = strlen(save_backups_roots[i]);
         rest     = below;
         which    = (int)i;
      }
   }
   if (which == SAVE_BACKUPS_ROOT_SYSTEM
         && !save_backups_roots[SAVE_BACKUPS_ROOT_SAVEFILE])
      which = -1;

   if (which < 0)
   {
      fill_pathname_basedir(s, path, len);
      _len = fill_pathname_join(s, s, SAVE_BACKUPS_DIR, len);
      return _len < len ? _len : 0;
   }

   if (which == SAVE_BACKUPS_ROOT_SYSTEM)
   {
      fill_pathname_join(s, save_backups_roots[SAVE_BACKUPS_ROOT_SAVEFILE],
            SAVE_BACKUPS_DIR, len);
      _len = fill_pathname_join(s, s, SAVE_BACKUPS_SYSTEM_DIR, len);
   }
   else
      _len = fill_pathname_join(s, save_backups_roots[which],
            SAVE_BACKUPS_DIR, len);

   /* The folders between the root and the file */
   base = path_basename_nocompression(rest);
   if (base > rest)
   {
      size_t n = (size_t)(base - rest);
      _len     = fill_pathname_slash(s, len);
      if (_len + n >= len)
         return 0;
      memcpy(s + _len, rest, n);
      _len    += n;
      while (_len && PATH_CHAR_IS_SLASH(s[_len - 1]))
         _len--;
      s[_len]  = '\0';
   }
   return _len < len ? _len : 0;
}

/* The extension of @name: from its last '.', and a final ".auto"
 * takes the one before it too. */
static const char *save_backups_ext(const char *name)
{
   const char *ext = strrchr(name, '.');
   const char *p;
   if (!ext)
      return name + strlen(name);
   if (ext == name || !string_is_equal(ext, ".auto"))
      return ext;
   p = ext;
   while (p > name && p[-1] != '.')
      p--;
   return p > name ? p - 1 : ext;
}

/* The last " - " that ends at or before @end, or NULL. */
static const char *save_backups_sep(const char *name, const char *end)
{
   const char *p;
   if (end - name < 3)
      return NULL;
   for (p = end - 3; ; p--)
   {
      if (p[0] == ' ' && p[1] == '-' && p[2] == ' ')
         return p;
      if (p == name)
         return NULL;
   }
}

const char *save_backups_reason_name(enum save_backup_reason reason)
{
   return (unsigned)reason < SAVE_BACKUP_REASON_LAST
      ? save_backups_reasons[reason] : "";
}

/* @t starts with a time "YYYY-MM-DD HH-MM-SS" */
static bool save_backups_time(const char *t)
{
   unsigned i;
   for (i = 0; i < SAVE_BACKUPS_TIME_LEN; i++)
   {
      char c = t[i];
      if (i == 4 || i == 7 || i == 13 || i == 16)
      {
         if (c != '-')
            return false;
      }
      else if (i == 10)
      {
         if (c != ' ')
            return false;
      }
      else if (c < '0' || c > '9')
         return false;
   }
   return true;
}

/* The @n digits at @t as a number from 1 to SAVE_BACKUPS_SEQ_MAX; 0
 * when they are not one */
static unsigned save_backups_seq(const char *t, size_t n)
{
   size_t i;
   unsigned v = 0;
   if (n > 9)
      return 0;
   for (i = 0; i < n; i++)
   {
      if (t[i] < '0' || t[i] > '9')
         return 0;
      v = v * 10 + (unsigned)(t[i] - '0');
   }
   return v;
}

bool save_backups_parse(const char *name, struct save_backup_name *out)
{
   unsigned i, reason;
   size_t n;
   const char *sep3, *sep2, *sep1, *sep0, *rest, *t;
   const char *end = name + strlen(name);

   if (!(sep3 = save_backups_sep(name, end)))
      return false;
   if (!(sep2 = save_backups_sep(name, sep3)))
      return false;
   if (!(sep1 = save_backups_sep(name, sep2)))
      return false;

   rest = sep3 + 3;
   for (reason = 0; reason < SAVE_BACKUP_REASON_LAST; reason++)
   {
      n = strlen(save_backups_reasons[reason]);
      if (!strncmp(rest, save_backups_reasons[reason], n)
            && (!rest[n] || rest[n] == '.'))
         break;
   }
   if (reason == SAVE_BACKUP_REASON_LAST)
      return false;
   rest += n;
   /* What the name rule makes, never a temp's ".srm.tmp" */
   if (*rest && save_backups_ext(rest) != rest)
      return false;

   if (sep3 == sep2 + 3)
      return false;

   t              = sep1 + 3;
   n              = (size_t)(sep2 - t);
   out->number    = 1;
   out->seq       = 0;
   out->device[0] = '\0';
   if (     n > SAVE_BACKUPS_TAG_LEN + 1
         && t[n - SAVE_BACKUPS_TAG_LEN - 1] == '-'
         && save_backups_is_tag(t + n - SAVE_BACKUPS_TAG_LEN)
         && (out->seq = save_backups_seq(t,
               n - SAVE_BACKUPS_TAG_LEN - 1)))
   {
      memcpy(out->device, t + n - SAVE_BACKUPS_TAG_LEN,
            SAVE_BACKUPS_TAG_LEN);
      out->device[SAVE_BACKUPS_TAG_LEN] = '\0';
      if (     !(sep0 = save_backups_sep(name, sep1))
            || sep1 - (sep0 + 3) != SAVE_BACKUPS_TIME_LEN
            || !save_backups_time(sep0 + 3))
         return false;
      t    = sep0 + 3;
      sep1 = sep0;
   }
   else if (n < SAVE_BACKUPS_TIME_LEN || !save_backups_time(t))
      return false;
   else if (n > SAVE_BACKUPS_TIME_LEN)
   {
      unsigned long num = 0;
      if (     n > SAVE_BACKUPS_TIME_LEN + 10
            || t[SAVE_BACKUPS_TIME_LEN] != '-'
            || t[SAVE_BACKUPS_TIME_LEN + 1] < '1'
            || t[SAVE_BACKUPS_TIME_LEN + 1] > '9')
         return false;
      for (i = SAVE_BACKUPS_TIME_LEN + 1; i < n; i++)
      {
         if (t[i] < '0' || t[i] > '9')
            return false;
         num = num * 10 + (unsigned long)(t[i] - '0');
      }
      if (num < 2)
         return false;
      out->number = (unsigned)num;
   }

   memcpy(out->time, t, SAVE_BACKUPS_TIME_LEN);
   out->time[SAVE_BACKUPS_TIME_LEN] = '\0';
   memcpy(out->eff, out->time, sizeof(out->eff));
   out->game_len = (size_t)(sep1 - name);
   out->core_off = (size_t)(sep2 + 3 - name);
   out->core_len = (size_t)(sep3 - (sep2 + 3));
   out->ext_off  = (size_t)(rest - name);
   out->reason   = (enum save_backup_reason)reason;
   return true;
}

/* @core as a name field: no characters a file system refuses and no
 * " - ", so the name still reads from the right. */
static char *save_backups_core_field(const char *core)
{
   size_t i, n = 0;
   char *s;
   if (string_is_empty(core))
      core = "unknown";
   if (!(s = (char*)malloc(strlen(core) + 1)))
      return NULL;
   for (i = 0; core[i]; i++)
   {
      unsigned char c = (unsigned char)core[i];
      /* " - " becomes "-", checked on the output so none is left */
      if (c == ' ' && n > 1 && s[n - 2] == ' ' && s[n - 1] == '-')
      {
         s[n - 2] = '-';
         n--;
      }
      else if (c < 0x20 || strchr("<>:\"/\\|?*", c))
         s[n++] = '_';
      else
         s[n++] = (char)c;
   }
   if (s[0] == '-')
      s[0] = '_';
   s[n] = '\0';
   return s;
}

/* The longest game a name with this core and extension keeps, so the
 * whole name stays under the limit. */
static size_t save_backups_game_max(size_t core_len, size_t ext_len)
{
   size_t fixed = 3 + SAVE_BACKUPS_TIME_LEN + SAVE_BACKUPS_SEQ_ROOM + 3
      + core_len + 3 + SAVE_BACKUPS_REASON_ROOM + ext_len;
   return fixed + 2 < SAVE_BACKUPS_NAME_MAX
      ? SAVE_BACKUPS_NAME_MAX - 1 - fixed : 1;
}

/* How much of @game a name keeps. A cut game is never more than
 * SAVE_BACKUPS_CUT_ROOM bytes under the limit. */
static size_t save_backups_game_len(const char *game, size_t game_len,
      size_t core_len, size_t ext_len)
{
   unsigned i;
   size_t max = save_backups_game_max(core_len, ext_len);
   if (game_len <= max)
      return game_len;
   for (i = 0; i < SAVE_BACKUPS_CUT_ROOM && max
         && ((unsigned char)game[max] & 0xC0) == 0x80; i++)
      max--;
   return max;
}

#ifdef SAVE_BACKUPS_PATH_MAX
/* UTF-16 units in the first @len bytes of @s */
static size_t save_backups_units(const char *s, size_t len)
{
   size_t i, n = 0;
   for (i = 0; i < len; i++)
   {
      unsigned char c = (unsigned char)s[i];
      if ((c & 0xC0) != 0x80)
         n += c >= 0xF0 ? 2 : 1;
   }
   return n;
}

/* The most UTF-16 units a game keeps in a name with this core and
 * extension, so the copy's temporary fits in a folder @dir_units long;
 * below 0 when not even an empty game fits. */
static long save_backups_game_room(size_t dir_units, const char *core,
      const char *ext)
{
   size_t fixed = dir_units + 3 + SAVE_BACKUPS_TIME_LEN
      + SAVE_BACKUPS_SEQ_ROOM + 3
      + save_backups_units(core, strlen(core))
      + 3 + SAVE_BACKUPS_REASON_ROOM + save_backups_units(ext, strlen(ext))
      + SAVE_BACKUPS_TEMP_LEN;
   return (long)SAVE_BACKUPS_PATH_MAX - (long)fixed;
}
#endif

/* Where a copy's path has a limit, the UTF-16 units of @dir's full
 * path and the slash before a copy's name; else -1. */
static long save_backups_dir_units(const char *dir)
{
#ifdef SAVE_BACKUPS_PATH_MAX
   size_t _len        = strlen(dir);
   long n             = _len && !PATH_CHAR_IS_SLASH(dir[_len - 1]);
   const char *scheme = strstr(dir, "://");
   bool whole         = path_is_absolute(dir);
   char *full         = NULL;
   /* A VFS URL, not a drive's "C://" */
   if (scheme && scheme - dir > 1)
      return -1;
#ifdef _WIN32
   /* "/dir" is on the working folder's drive */
   if (PATH_CHAR_IS_SLASH(dir[0]) && !PATH_CHAR_IS_SLASH(dir[1]))
      whole = false;
#endif
   if (!whole && (full = (char*)malloc(PATH_MAX_LENGTH)))
   {
      strlcpy(full, dir, PATH_MAX_LENGTH);
      if (path_resolve_realpath(full, PATH_MAX_LENGTH, false))
      {
         dir  = full;
         _len = strlen(full);
      }
   }
   n += (long)save_backups_units(dir, _len);
   free(full);
   return n;
#else
   return -1;
#endif
}

/* How much of @game a name keeps in a folder @dir_units long: as
 * save_backups_game_len(), and when that does not fit the path, what
 * fits beside a SAVE_BACKUPS_CUT_MARK, so possibly none; *@mark tells
 * which. */
static size_t save_backups_game_fit(long dir_units, const char *game,
      size_t game_len, const char *core, const char *ext, bool *mark)
{
   size_t n = save_backups_game_len(game, game_len, strlen(core),
         strlen(ext));
#ifdef SAVE_BACKUPS_PATH_MAX
   size_t i;
   long room;
#endif
   *mark    = false;
#ifdef SAVE_BACKUPS_PATH_MAX
   if (dir_units < 0)
      return n;
   room = save_backups_game_room((size_t)dir_units, core, ext);
   if (!n || (long)save_backups_units(game, n) <= room)
      return n;
   *mark = true;
   room--;
   for (i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)game[i];
      if ((c & 0xC0) != 0x80 && (room -= c >= 0xF0 ? 2 : 1) < 0)
         return i;
   }
#endif
   return n;
}

/* A name from before numbering is older. On one device by number, so
 * a clock set back keeps the order; between devices by time, each
 * device's held at its latest (eff), so the order never loops. Then by
 * number, device and time. */
static int save_backups_cmp(const struct save_backup_name *a,
      const struct save_backup_name *b)
{
   int c;
   if (!a->seq != !b->seq)
      return a->seq ? 1 : -1;
   if ((c = memcmp(a->eff, b->eff, SAVE_BACKUPS_TIME_LEN)))
      return c;
   if (a->seq != b->seq)
      return a->seq < b->seq ? -1 : 1;
   if ((c = strcmp(a->device, b->device)))
      return c;
   if ((c = memcmp(a->time, b->time, SAVE_BACKUPS_TIME_LEN)))
      return c;
   return a->number < b->number ? -1 : a->number > b->number;
}

/* What a caller holds a copy by: its number; at the last number,
 * which copies share, a value above every number from its time */
static unsigned save_backups_record(unsigned seq, const char *time)
{
   unsigned i, h = 0;
   if (seq < SAVE_BACKUPS_SEQ_MAX)
      return seq;
   for (i = 0; i < SAVE_BACKUPS_TIME_LEN; i++)
      h = h * 31 + (unsigned char)time[i];
   /* Up to INT_MAX, as callers keep it in an int */
   return SAVE_BACKUPS_SEQ_MAX + 1 + h % (0x7FFFFFFFu - SAVE_BACKUPS_SEQ_MAX);
}

/* Deletes the oldest copies of one file for @reason in @list, all but
 * SAVE_BACKUPS_KEEP counting the one just made, which is not in it.
 * The copy held by @keep, @device's, is neither counted nor deleted. */
static void save_backups_retain(struct string_list *list,
      struct save_backup_name *names, const bool *mine,
      enum save_backup_reason reason, size_t ext_len, unsigned keep,
      const char *device)
{
   size_t i;
   size_t count = 0;
   for (i = 0; i < list->size; i++)
   {
      const char *name;
      if (!mine[i] || names[i].reason != reason)
         continue;
      name = path_basename_nocompression(list->elems[i].data);
      /* Near its limit or marked, the game may be a longer name's cut:
       * never pruned */
      if (     keep
            && save_backups_record(names[i].seq, names[i].time) == keep
            && string_is_equal(names[i].device, device))
         names[i].reason = SAVE_BACKUP_REASON_LAST;
      else if (names[i].game_len + SAVE_BACKUPS_CUT_ROOM
               < save_backups_game_max(names[i].core_len, ext_len)
            && (  !names[i].game_len
               || name[names[i].game_len - 1] != SAVE_BACKUPS_CUT_MARK))
         count++;
      else
         names[i].reason = SAVE_BACKUP_REASON_LAST;
   }
   while (count + 1 > SAVE_BACKUPS_KEEP)
   {
      size_t oldest = list->size;
      for (i = 0; i < list->size; i++)
         if (     mine[i] && names[i].reason == reason
               && (oldest == list->size
                  || save_backups_cmp(&names[i], &names[oldest]) < 0))
            oldest = i;
      filestream_delete(list->elems[oldest].data);
      names[oldest].reason = SAVE_BACKUP_REASON_LAST;
      count--;
   }
}

/* @name (a basename) is a copy of the file named @base, whose game
 * field is its first @game_len bytes, then the cut mark when @mark, and
 * whose extension is @ext. */
static bool save_backups_of(const char *name, struct save_backup_name *out,
      const char *base, size_t game_len, bool mark, const char *ext)
{
   return save_backups_parse(name, out)
      && out->game_len == game_len + (mark ? 1 : 0)
      && !memcmp(name, base, game_len)
      && (!mark || name[game_len] == SAVE_BACKUPS_CUT_MARK)
      && string_is_equal(name + out->ext_off, ext);
}

/* The copies of a file in a folder, as a copy's name and retention
 * read them */
struct save_backups_copies
{
   struct string_list *list;
   struct save_backup_name *names;
   bool *mine;
   char *field;
   const char *base;
   const char *ext;
   size_t base_len;
   size_t game_len;
   size_t newest;   /* (size_t)-1 when none */
   long dir_units;
   unsigned seq;    /* the next number, past SAVE_BACKUPS_SEQ_MAX when
                     * none is left */
   bool mark;
   char stamp[SAVE_BACKUPS_TIME_LEN + 1];
   char device[SAVE_BACKUPS_TAG_LEN + 1];
};

/* Lists the copies of @path in @dir, named with @core (NULL: the
 * roots' core). save_backups_free_copies() after, whatever it returns. */
static bool save_backups_list_copies(struct save_backups_copies *c,
      const char *dir, const char *path, const char *core)
{
   size_t i, j;
   time_t now;
   struct tm tm_;

   memset(c, 0, sizeof(*c));
   c->newest = (size_t)-1;
   c->base   = path_basename_nocompression(path);
   c->ext    = save_backups_ext(c->base);
   c->field  = save_backups_core_field(
         core ? core : save_backups_roots[SAVE_BACKUPS_ROOT_CORE]);
   if (!c->field || string_is_empty(dir))
      return false;
   c->base_len  = (size_t)(c->ext - c->base);
   c->dir_units = save_backups_dir_units(dir);
   c->game_len  = save_backups_game_fit(c->dir_units, c->base, c->base_len,
         c->field, c->ext, &c->mark);

   /* rtime_localtime leaves tm_ unwritten when localtime() fails */
   memset(&tm_, 0, sizeof(tm_));
   time(&now);
   rtime_localtime(&now, &tm_);
   if (!strftime(c->stamp, sizeof(c->stamp), "%Y-%m-%d %H-%M-%S", &tm_))
      return false;
   save_backups_device_get(c->device);

   if ((c->list = dir_list_new(dir, NULL, false, true, false, false))
         && c->list->size)
   {
      c->names = (struct save_backup_name*)malloc(
            c->list->size * sizeof(*c->names));
      c->mine  = (bool*)calloc(c->list->size, sizeof(*c->mine));
      if (!c->names || !c->mine)
         return false;
   }
   /* Not listed, the copy's number would be a guess */
   else if (!c->list && path_is_directory(dir))
      return false;
   for (i = 0; c->names && i < c->list->size; i++)
   {
      const char *name = path_basename_nocompression(c->list->elems[i].data);
      if (!save_backups_of(name, &c->names[i], c->base, c->game_len,
               c->mark, c->ext))
         continue;
      c->mine[i] = true;
      if (c->names[i].seq > c->seq)
         c->seq = c->names[i].seq;
   }
   c->seq++;
   /* Numbers on one device never go down: a copy's time is held up to
    * its device's latest at a number no higher, all reasons' */
   for (i = 0; c->names && i < c->list->size; i++)
      for (j = 0; c->mine[i] && c->names[i].seq && j < c->list->size; j++)
         if (     c->mine[j]
               && c->names[j].seq
               && c->names[j].seq <= c->names[i].seq
               && string_is_equal(c->names[j].device, c->names[i].device)
               && memcmp(c->names[j].time, c->names[i].eff,
                  SAVE_BACKUPS_TIME_LEN) > 0)
            memcpy(c->names[i].eff, c->names[j].time,
                  SAVE_BACKUPS_TIME_LEN);
   for (i = 0; c->names && i < c->list->size; i++)
      if (     c->mine[i]
            && (c->newest == (size_t)-1
               || save_backups_cmp(&c->names[i], &c->names[c->newest]) > 0))
         c->newest = i;
   return true;
}

static void save_backups_free_copies(struct save_backups_copies *c)
{
   free(c->names);
   free(c->mine);
   if (c->list)
      string_list_free(c->list);
   free(c->field);
}

/* A free name in @dir for a copy for @reason into @s, the folder made,
 * its number in *@seq. With none left, the last again: its time orders
 * it among those. *@no_room: no name fits. */
static bool save_backups_free_name(char *s, size_t len, const char *dir,
      const struct save_backups_copies *c, enum save_backup_reason reason,
      unsigned *seq, bool *no_room)
{
   size_t _len, name_off;
   unsigned i;
   if (!path_mkdir(dir))
      return false;
   name_off = fill_pathname_join(s, dir, "", len);
   if (name_off + c->game_len + (c->mark ? 1 : 0) >= len)
      return false;
   memcpy(s + name_off, c->base, c->game_len);
   _len    = name_off + c->game_len;
   if (c->mark)
      s[_len++] = SAVE_BACKUPS_CUT_MARK;
   s[_len] = '\0';
   /* Never over another file, whatever its name's case, nor over a
    * copy still being written to that name. */
   for (i = 0; i < SAVE_BACKUPS_SEQ_TRIES; i++)
   {
      size_t name_len;
      unsigned number = c->seq + i;
      if (number > SAVE_BACKUPS_SEQ_MAX)
         number = SAVE_BACKUPS_SEQ_MAX;
      snprintf(s + _len, len - _len, " - %s - %04u-%s - %s - %s%s",
            c->stamp, number, c->device, c->field,
            save_backups_reasons[reason], c->ext);
      if ((name_len = strlen(s)) + SAVE_BACKUPS_TEMP_LEN + 1 >= len)
         break;
#ifdef SAVE_BACKUPS_PATH_MAX
      if (     c->dir_units >= 0
            &&   (size_t)c->dir_units + SAVE_BACKUPS_TEMP_LEN
               + save_backups_units(s + name_off, strlen(s + name_off))
               > SAVE_BACKUPS_PATH_MAX)
         break;
#endif
      if (!path_is_valid(s))
      {
         bool writing;
         memcpy(s + name_len, ".tmp", SAVE_BACKUPS_TEMP_LEN + 1);
         writing     = path_is_valid(s);
         s[name_len] = '\0';
         if (!writing)
         {
            *seq = number;
            return true;
         }
      }
      if (number == SAVE_BACKUPS_SEQ_MAX)
         return false;
   }
   if (no_room && i < SAVE_BACKUPS_SEQ_TRIES)
      *no_room = true;
   return false;
}

/* Whether @reason's copies are pruned to SAVE_BACKUPS_KEEP. A cut name
 * may be another file's too. */
static bool save_backups_retained(const struct save_backups_copies *c,
      enum save_backup_reason reason)
{
   return c->names
      && c->game_len == c->base_len
      && (  reason == SAVE_BACKUP_BEFORE_STATE_LOAD
         || reason == SAVE_BACKUP_CHANGED_ELSEWHERE
         || (  reason == SAVE_BACKUP_NOT_LOADED
            && string_is_equal(c->ext, ".state.auto")));
}

/* Whether copy @i in @c's list is named for @c's core */
static bool save_backups_core_is(const struct save_backups_copies *c,
      size_t i)
{
   const char *name = path_basename_nocompression(c->list->elems[i].data);
   return c->names[i].core_len == strlen(c->field)
      && !memcmp(name + c->names[i].core_off, c->field,
            c->names[i].core_len);
}

/* Whether the copy @copy holds @data, compressed or not: @data is the
 * file @file's bytes, or with NULL an image. */
static bool save_backups_holds(const char *copy, const void *data,
      size_t len, const char *file)
{
#if defined(HAVE_COMPRESSION)
   bool same;
   void *plain = NULL;
   int64_t n   = 0;
#endif
   if (filestream_matches_buf(copy, data, len))
      return true;
#if defined(HAVE_COMPRESSION)
   /* Compared decompressed: a copy from the image is raw */
   if (!file)
      return rzipstream_matches_buf(copy, data, len);
   if (!rzipstream_read_file(file, &plain, &n))
      return false;
   same = rzipstream_matches_buf(copy, plain, (size_t)n);
   free(plain);
   return same;
#else
   return false;
#endif
}

/* Writes @data as a copy of @path into @dir, its path in @s; with
 * @move, renames @path there instead, or names the copy owed when that
 * fails. With @skip_same, nothing is written when the newest copy is
 * this core's, made on this device, and holds it; @of_file: @data is
 * @path's bytes. @seq as in save_backups_copy_image(). */
static enum save_backup_result save_backups_write_copy(char *s, size_t len,
      const char *dir, const char *path, const char *core,
      enum save_backup_reason reason, const void *data, size_t data_len,
      bool move, bool skip_same, bool of_file, bool sync, unsigned *seq,
      bool *no_room)
{
   struct save_backups_copies c;
   enum save_backup_result result = SAVE_BACKUP_FAILED;
   unsigned keep                  = seq ? *seq : 0;
   unsigned made                  = 0;

   s[0] = '\0';
   if (!save_backups_list_copies(&c, dir, path, core))
      goto end;

   if (     skip_same
         && c.newest != (size_t)-1
         && (!seq || keep || c.names[c.newest].seq)
         && save_backups_core_is(&c, c.newest)
         && string_is_equal(c.names[c.newest].device, c.device)
         && save_backups_holds(c.list->elems[c.newest].data, data, data_len,
               of_file ? path : NULL))
   {
      strlcpy(s, c.list->elems[c.newest].data, len);
      made   = save_backups_record(c.names[c.newest].seq,
            c.names[c.newest].time);
      result = SAVE_BACKUP_MADE;
      goto end;
   }

   if (!save_backups_free_name(s, len, dir, &c, reason, &made, no_room))
      goto end;
   made = save_backups_record(made, c.stamp);

   if (move)
   {
      if (filestream_rename(path, s) != 0)
      {
         result = SAVE_BACKUP_COPY_OWED;
         goto end;
      }
   }
   else if (!save_backups_write_file(s, data, (int64_t)data_len, sync))
   {
      /* Written in place: the free name holds only our partial copy */
      if (filestream_atomic_in_place(s))
         filestream_delete(s);
      s[0] = '\0';
      goto end;
   }
   result = SAVE_BACKUP_MADE;

   if (save_backups_retained(&c, reason))
      save_backups_retain(c.list, c.names, c.mine, reason, strlen(c.ext),
            keep, c.device);

end:
   if (result != SAVE_BACKUP_MADE && result != SAVE_BACKUP_COPY_OWED)
      s[0] = '\0';
   else if (seq)
      *seq = made;
   save_backups_free_copies(&c);
   return result;
}

enum save_backup_result save_backups_name_copy(char *s, size_t len,
      const char *dir, const char *path, const char *core,
      enum save_backup_reason reason)
{
   unsigned seq;
   struct save_backups_copies c;
   enum save_backup_result result = SAVE_BACKUP_FAILED;
   if (     save_backups_list_copies(&c, dir, path, core)
         && save_backups_free_name(s, len, dir, &c, reason, &seq, NULL))
      result = SAVE_BACKUP_MADE;
   else
      s[0] = '\0';
   save_backups_free_copies(&c);
   return result;
}

struct string_list *save_backups_delete_temps(const char *dir,
      const char *path, const char *core, enum save_backup_reason reason)
{
   size_t i;
   struct save_backups_copies c;
   struct string_list *copies = NULL;
   if (     save_backups_list_copies(&c, dir, path, core)
         && (copies = string_list_new()))
   {
      union string_list_elem_attr attr;
      attr.i = 0;
      for (i = 0; c.list && i < c.list->size; i++)
      {
         struct save_backup_name parsed;
         char        name[256];
         const char *base = path_basename_nocompression(
               c.list->elems[i].data);
         size_t      _len = strlen(base);
         if (     _len <= SAVE_BACKUPS_TEMP_LEN
               || _len - SAVE_BACKUPS_TEMP_LEN >= sizeof(name)
               || !string_is_equal(base + _len - SAVE_BACKUPS_TEMP_LEN,
                  ".tmp"))
            continue;
         _len -= SAVE_BACKUPS_TEMP_LEN;
         memcpy(name, base, _len);
         name[_len] = '\0';
         if (     save_backups_of(name, &parsed, c.base, c.game_len, c.mark,
                  c.ext)
               && parsed.reason == reason)
            filestream_delete(c.list->elems[i].data);
      }
      /* Newest first */
      for (;;)
      {
         size_t newest = (size_t)-1;
         for (i = 0; c.names && i < c.list->size; i++)
            if (     c.mine[i] && c.names[i].reason == reason
                  && (newest == (size_t)-1
                     || save_backups_cmp(&c.names[i], &c.names[newest]) > 0))
               newest = i;
         if (newest == (size_t)-1)
            break;
         if (!string_list_append(copies, c.list->elems[newest].data, attr))
         {
            string_list_free(copies);
            copies = NULL;
            break;
         }
         c.mine[newest] = false;
      }
   }
   save_backups_free_copies(&c);
   return copies;
}

void save_backups_retain_made(const char *dir, const char *path,
      const char *core, enum save_backup_reason reason, const char *made)
{
   size_t i;
   struct save_backups_copies c;
   if (     save_backups_list_copies(&c, dir, path, core)
         && save_backups_retained(&c, reason))
   {
      const char *name = path_basename_nocompression(made);
      for (i = 0; i < c.list->size; i++)
         if (string_is_equal(
                  path_basename_nocompression(c.list->elems[i].data), name))
            c.mine[i] = false;
      save_backups_retain(c.list, c.names, c.mine, reason, strlen(c.ext),
            0, c.device);
   }
   save_backups_free_copies(&c);
}

static enum save_backup_result save_backups_copy_seq(const char *dir,
      const char *path, const char *core, enum save_backup_reason reason,
      const void *data, size_t len, bool sync, unsigned *seq, bool *no_room)
{
   enum save_backup_result result;
   void *buf = NULL;
   char *made;
   if (!(made = (char*)malloc(PATH_MAX_LENGTH)))
      return SAVE_BACKUP_FAILED;
   if (!data)
   {
      int64_t n = 0;
      if (!path_is_valid(path))
      {
         free(made);
         return SAVE_BACKUP_NO_FILE;
      }
      if (!filestream_read_file(path, &buf, &n))
      {
         free(made);
         return SAVE_BACKUP_FAILED;
      }
      data = buf;
      len  = (size_t)n;
   }
   result = save_backups_write_copy(made, PATH_MAX_LENGTH, dir, path,
         core, reason, data, len, false, true, buf != NULL, sync, seq,
         no_room);
   free(buf);
   free(made);
   return result;
}

enum save_backup_result save_backups_copy_to(const char *dir,
      const char *path, const char *core, enum save_backup_reason reason,
      const void *data, size_t len, bool sync)
{
   return save_backups_copy_seq(dir, path, core, reason, data, len, sync,
         NULL, NULL);
}

enum save_backup_result save_backups_move_begin(const char *dir,
      const char *path, const char *core, enum save_backup_reason reason,
      char *s, size_t len)
{
   if (!path_is_valid(path))
      return SAVE_BACKUP_NO_FILE;
   return save_backups_write_copy(s, len, dir, path, core, reason,
         NULL, 0, true, false, false, false, NULL, NULL);
}

enum save_backup_result save_backups_move_to(const char *dir,
      const char *path, const char *core, enum save_backup_reason reason)
{
   enum save_backup_result result;
   char *made;
   if (!path_is_valid(path))
      return SAVE_BACKUP_NO_FILE;
   if (!(made = (char*)malloc(PATH_MAX_LENGTH)))
      return SAVE_BACKUP_FAILED;
   result = save_backups_write_copy(made, PATH_MAX_LENGTH, dir, path,
         core, reason, NULL, 0, true, false, false, false, NULL, NULL);
   if (result == SAVE_BACKUP_COPY_OWED)
      result = (  save_backups_copy_synced(path, made)
               && save_backups_move_finish(path, made))
         ? SAVE_BACKUP_MADE : SAVE_BACKUP_FAILED;
   free(made);
   return result;
}

/* @path's own folder in root @i's recovery folder, named by its
 * CRC-32, into @s: saves of one name never share copies there. */
static bool save_backups_recovery_folder(char *s, size_t len, unsigned i,
      const char *path)
{
   char id[9];
   if (!save_backups_roots[i])
      return false;
   snprintf(id, sizeof(id), "%08lx", (unsigned long)encoding_crc32(0,
         (const uint8_t*)path, strlen(path)));
   fill_pathname_join(s, save_backups_roots[i], SAVE_BACKUPS_RECOVERY_DIR,
         len);
   fill_pathname_join(s, s, id, len);
   return true;
}

/* A copy no name fits for beside the save goes to its own folder where
 * recovery files do. */
static enum save_backup_result save_backups_copy_any(const char *path,
      enum save_backup_reason reason, const void *data, size_t len,
      bool sync, unsigned *seq)
{
   unsigned i;
   bool no_room                   = false;
   enum save_backup_result result = SAVE_BACKUP_FAILED;
   char *dir = (char*)malloc(PATH_MAX_LENGTH);
   if (!dir)
      return SAVE_BACKUP_FAILED;
   if (save_backups_dir(dir, PATH_MAX_LENGTH, path))
      result = save_backups_copy_seq(dir, path, NULL, reason, data, len,
            sync, seq, &no_room);
   /* Nor the folder's own name */
   else
      no_room = !string_is_empty(path);
   for (i = SAVE_BACKUPS_ROOT_CONFIG; no_room
         && result == SAVE_BACKUP_FAILED && i <= SAVE_BACKUPS_ROOT_APP_DATA;
         i++)
      if (save_backups_recovery_folder(dir, PATH_MAX_LENGTH, i, path))
         result = save_backups_copy_seq(dir, path, NULL, reason, data, len,
               sync, seq, NULL);
   free(dir);
   return result;
}

enum save_backup_result save_backups_copy_image(const char *path,
      enum save_backup_reason reason, const void *data, size_t len,
      bool sync, unsigned *seq)
{
   return save_backups_copy_any(path, reason, data ? data : "",
         data ? len : 0, sync, seq);
}

enum save_backup_result save_backups_copy_file(const char *path,
      enum save_backup_reason reason, bool sync, unsigned *seq)
{
   return save_backups_copy_any(path, reason, NULL, 0, sync, seq);
}

/* Whether the newest recovery file of @path, its path in @s, holds
 * @data. @sync: it may not have been synced when written, so it is now,
 * and false if that fails. */
static bool save_backups_recovered(char *s, size_t len, const char *path,
      const void *data, size_t data_len, bool sync)
{
   bool ok;
   char *dir;
   RFILE *file;
   if (     !save_backups_find_recovery(s, len, path, INT64_MIN)
         || !filestream_matches_buf(s, data, data_len))
      return false;
   /* Newer than the save again, so it is announced */
   path_set_mtime(s, (int64_t)time(NULL));
   if (!sync)
      return true;
   file = filestream_open(s,
         RETRO_VFS_FILE_ACCESS_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   ok   = file && filestream_sync(file) == 0;
   if (file && filestream_close(file) != 0)
      ok = false;
   /* Its name too, as a synced write's rename */
   if (ok && (dir = (char*)malloc(len)))
   {
      strlcpy(dir, s, len);
      path_basedir(dir);
      retro_vfs_dir_sync_impl(dir);
      free(dir);
   }
   return ok;
}

size_t save_backups_recover(char *s, size_t len, const char *path,
      const void *data, size_t data_len, bool sync)
{
   unsigned i;
   char *dir;
   bool made = false;
   if (save_backups_recovered(s, len, path, data, data_len, sync))
      return strlen(s);
   s[0] = '\0';
   if (!(dir = (char*)malloc(PATH_MAX_LENGTH)))
      return 0;
   if (save_backups_dir(dir, PATH_MAX_LENGTH, path))
      made = save_backups_write_copy(s, len, dir, path, NULL,
            SAVE_BACKUP_UNSAVED, data, data_len, false, false, false, sync,
            NULL, NULL)
         == SAVE_BACKUP_MADE;
   for (i = SAVE_BACKUPS_ROOT_CONFIG;
         !made && i <= SAVE_BACKUPS_ROOT_APP_DATA; i++)
   {
      if (!save_backups_roots[i])
         continue;
      fill_pathname_join(dir, save_backups_roots[i],
            SAVE_BACKUPS_RECOVERY_DIR, PATH_MAX_LENGTH);
      made = save_backups_write_copy(s, len, dir, path, NULL,
            SAVE_BACKUP_UNSAVED, data, data_len, false, false, false, sync,
            NULL, NULL)
         == SAVE_BACKUP_MADE;
   }
   free(dir);
   return made ? strlen(s) : 0;
}

/* The newest recovery file of @base in @dir changed after *@newest.
 * Times may be whole seconds: on a tie, the later name. */
static void save_backups_newest_unsaved(char *s, size_t len,
      const char *dir, const char *base, const char *field, const char *ext,
      int64_t *newest)
{
   size_t i;
   bool mark;
   size_t game_len          = save_backups_game_fit(
         save_backups_dir_units(dir), base, (size_t)(ext - base), field, ext,
         &mark);
   struct string_list *list = dir_list_new(dir, NULL, false, true,
         false, false);
   for (i = 0; list && i < list->size; i++)
   {
      struct save_backup_name name, best;
      int64_t t        = 0;
      const char *file = list->elems[i].data;
      if (     save_backups_of(path_basename_nocompression(file), &name,
                  base, game_len, mark, ext)
            && name.reason == SAVE_BACKUP_UNSAVED
            && path_get_mtime(file, &t)
            && (t > *newest
               || (  t == *newest && *s
                  && save_backups_parse(path_basename_nocompression(s), &best)
                  && save_backups_cmp(&name, &best) > 0)))
      {
         *newest = t;
         strlcpy(s, file, len);
      }
   }
   if (list)
      string_list_free(list);
}

size_t save_backups_find_recovery(char *s, size_t len, const char *path,
      int64_t mtime)
{
   unsigned i;
   char *dir        = (char*)malloc(PATH_MAX_LENGTH);
   const char *base = path_basename_nocompression(path);
   const char *ext  = save_backups_ext(base);
   char *field      = save_backups_core_field(
         save_backups_roots[SAVE_BACKUPS_ROOT_CORE]);
   s[0]             = '\0';
   if (dir && field)
   {
      if (save_backups_dir(dir, PATH_MAX_LENGTH, path))
         save_backups_newest_unsaved(s, len, dir, base, field, ext,
               &mtime);
      for (i = SAVE_BACKUPS_ROOT_CONFIG; i <= SAVE_BACKUPS_ROOT_APP_DATA; i++)
      {
         if (!save_backups_roots[i])
            continue;
         fill_pathname_join(dir, save_backups_roots[i],
               SAVE_BACKUPS_RECOVERY_DIR, PATH_MAX_LENGTH);
         save_backups_newest_unsaved(s, len, dir, base, field, ext,
               &mtime);
      }
   }
   free(dir);
   free(field);
   return strlen(s);
}

enum save_folders_key
{
   SAVE_FOLDERS_DEVICE = 0,
   SAVE_FOLDERS_SAVES,
   SAVE_FOLDERS_STATES,
   SAVE_FOLDERS_FIRST_SEEN,
   SAVE_FOLDERS_LAST_SEEN,
   SAVE_FOLDERS_CONFIGURED,
   SAVE_FOLDERS_USED,
   SAVE_FOLDERS_TIME,
   SAVE_FOLDERS_KEY_LAST
};

static const char *const save_folders_keys[SAVE_FOLDERS_KEY_LAST] =
{
   "device", "saves", "states", "first_seen", "last_seen", "configured",
   "used", "time"
};

struct save_folders_entry
{
   char *v[SAVE_FOLDERS_KEY_LAST];
};

struct save_folders_list
{
   struct save_folders_entry *e;
   size_t n;
   size_t cap;
};

static struct save_folders_entry *save_folders_add(
      struct save_folders_list *l)
{
   if (l->n == l->cap)
   {
      size_t cap = l->cap ? l->cap * 2 : 8;
      struct save_folders_entry *e = (struct save_folders_entry*)
         realloc(l->e, cap * sizeof(*e));
      if (!e)
         return NULL;
      l->e   = e;
      l->cap = cap;
   }
   memset(&l->e[l->n], 0, sizeof(l->e[l->n]));
   return &l->e[l->n++];
}

/* False when @path is there but not read whole */
static bool save_folders_read(struct save_folders_list *l,
      const char *path)
{
   void *buf     = NULL;
   int64_t len   = 0;
   rjson_t *json = NULL;
   int key       = -1;
   bool whole    = true;
   if (!filestream_read_file(path, &buf, &len))
      return !path_is_valid(path);
   if (!(json = rjson_open_buffer(buf, (size_t)len)))
   {
      free(buf);
      return false;
   }
   for (;;)
   {
      size_t slen;
      const char *str;
      enum rjson_type t = rjson_next(json);
      if (t == RJSON_DONE)
         break;
      if (t == RJSON_ERROR)
      {
         whole = false;
         break;
      }
      if (rjson_get_context_depth(json) != 2)
         continue;
      if (t == RJSON_OBJECT)
      {
         if (!save_folders_add(l))
         {
            whole = false;
            break;
         }
         continue;
      }
      if (t != RJSON_STRING || !l->n)
         continue;
      str = rjson_get_string(json, &slen);
      if (rjson_get_context_count(json) & 1)
      {
         for (key = 0; key < SAVE_FOLDERS_KEY_LAST; key++)
            if (string_is_equal(str, save_folders_keys[key]))
               break;
      }
      else if (key >= 0 && key < SAVE_FOLDERS_KEY_LAST
            && !l->e[l->n - 1].v[key]
            && !(l->e[l->n - 1].v[key] = strdup(str)))
         whole = false;
   }
   rjson_free(json);
   free(buf);
   return whole;
}

/* Moves @path to its first free aside name, put in @s */
static bool save_folders_set_aside(char *s, size_t len, const char *path)
{
   unsigned n;
   for (n = 1; fill_pathname_aside(s, path, n, len); n++)
      if (!path_is_valid(s))
         return filestream_rename(path, s) == 0;
   return false;
}

static bool save_folders_write(const struct save_folders_list *l,
      const char *path, bool sync)
{
   size_t i, k;
   int len;
   bool ok        = false;
   char *buf      = NULL;
   rjsonwriter_t *w = rjsonwriter_open_memory();
   if (!w)
      return false;
   rjsonwriter_raw(w, "[", 1);
   for (i = 0; i < l->n; i++)
   {
      bool first = true;
      rjsonwriter_raw(w, i ? ",\n  {" : "\n  {", i ? 5 : 4);
      for (k = 0; k < SAVE_FOLDERS_KEY_LAST; k++)
      {
         if (!l->e[i].v[k])
            continue;
         if (!first)
            rjsonwriter_raw(w, ", ", 2);
         rjsonwriter_add_string(w, save_folders_keys[k]);
         rjsonwriter_raw(w, ": ", 2);
         rjsonwriter_add_string(w, l->e[i].v[k]);
         first = false;
      }
      rjsonwriter_raw(w, "}", 1);
   }
   rjsonwriter_raw(w, "\n]\n", 3);
   if ((buf = rjsonwriter_get_memory_buffer(w, &len)))
      ok = save_backups_write_file(path, buf, len, sync);
   rjsonwriter_free(w);
   return ok;
}

static bool save_folders_same(const char *a, const char *b)
{
   return string_is_equal(a ? a : "", b ? b : "");
}

/* The device tag in @l, or NULL */
static const char *save_folders_device(const struct save_folders_list *l)
{
   size_t i;
   for (i = 0; i < l->n; i++)
   {
      const char *t = l->e[i].v[SAVE_FOLDERS_DEVICE];
      if (t && strlen(t) == SAVE_BACKUPS_TAG_LEN && save_backups_is_tag(t))
         return t;
   }
   return NULL;
}

static void save_folders_free(struct save_folders_list *l)
{
   size_t i, k;
   for (i = 0; i < l->n; i++)
      for (k = 0; k < SAVE_FOLDERS_KEY_LAST; k++)
         free(l->e[i].v[k]);
   free(l->e);
}

/* <config>/save_folders.json into @s; false with no config folder */
static bool save_folders_path(char *s, size_t len)
{
   if (!save_backups_roots[SAVE_BACKUPS_ROOT_CONFIG])
      return false;
   fill_pathname_join_special(s, save_backups_roots[SAVE_BACKUPS_ROOT_CONFIG],
         SAVE_BACKUPS_FOLDERS_FILE, len);
   return true;
}

static void save_backups_device_read(void)
{
   struct save_folders_list l;
   const char *t;
   char *path = (char*)malloc(PATH_MAX_LENGTH);
   memset(&l, 0, sizeof(l));
   if (path && save_folders_path(path, PATH_MAX_LENGTH))
   {
      save_folders_read(&l, path);
      if ((t = save_folders_device(&l)))
      {
         retro_atomic_store_release_int(&save_backups_device,
               save_backups_tag_pack(t));
         save_backups_device_kept = true;
      }
   }
   save_folders_free(&l);
   free(path);
}

bool save_backups_note_folders(const char *configured, const char *used,
      bool sync)
{
   size_t i;
   time_t now;
   struct tm tm_;
   char stamp[24];
   struct save_folders_list l;
   struct save_folders_entry *last = NULL;
   struct save_folders_entry *e    = NULL;
   const char *device              = NULL;
   bool tagged                     = false;
   bool changed                    = false;
   bool ok                         = true;
   bool whole;
   const char *saves  = save_backups_roots[SAVE_BACKUPS_ROOT_SAVEFILE];
   const char *states = save_backups_roots[SAVE_BACKUPS_ROOT_SAVESTATE];
   char *path         = NULL;

   if (     !(path = (char*)malloc(PATH_MAX_LENGTH))
         || !save_folders_path(path, PATH_MAX_LENGTH))
   {
      free(path);
      return false;
   }

   memset(&tm_, 0, sizeof(tm_));
   time(&now);
   rtime_localtime(&now, &tm_);
   strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm_);

   memset(&l, 0, sizeof(l));
   whole = save_folders_read(&l, path);
   /* This device's tag: the one kept, else the one copies use added */
   if ((device = save_folders_device(&l)))
      retro_atomic_store_release_int(&save_backups_device,
            save_backups_tag_pack(device));
   else if ((e = save_folders_add(&l)))
   {
      char t[SAVE_BACKUPS_TAG_LEN + 1];
      save_backups_device_get(t);
      e->v[SAVE_FOLDERS_DEVICE]     = strdup(t);
      e->v[SAVE_FOLDERS_FIRST_SEEN] = strdup(stamp);
      changed = tagged = true;
   }
   for (i = l.n; i-- > 0; )
      if (l.e[i].v[SAVE_FOLDERS_SAVES] || l.e[i].v[SAVE_FOLDERS_STATES])
      {
         last = &l.e[i];
         break;
      }

   if (     !last
         || !save_folders_same(last->v[SAVE_FOLDERS_SAVES], saves)
         || !save_folders_same(last->v[SAVE_FOLDERS_STATES], states))
   {
      if ((e = save_folders_add(&l)))
      {
         e->v[SAVE_FOLDERS_SAVES]      = strdup(saves ? saves : "");
         e->v[SAVE_FOLDERS_STATES]     = strdup(states ? states : "");
         e->v[SAVE_FOLDERS_FIRST_SEEN] = strdup(stamp);
         e->v[SAVE_FOLDERS_LAST_SEEN]  = strdup(stamp);
         changed = true;
      }
   }
   /* A day is "YYYY-MM-DD", the first 10 bytes */
   else if (!last->v[SAVE_FOLDERS_LAST_SEEN]
         || strncmp(last->v[SAVE_FOLDERS_LAST_SEEN], stamp, 10))
   {
      free(last->v[SAVE_FOLDERS_LAST_SEEN]);
      last->v[SAVE_FOLDERS_LAST_SEEN] = strdup(stamp);
      changed = true;
   }

   if (configured)
   {
      /* The same note is kept once a day */
      for (i = l.n; i-- > 0; )
         if (l.e[i].v[SAVE_FOLDERS_CONFIGURED])
            break;
      if (     i == (size_t)-1
            || !save_folders_same(l.e[i].v[SAVE_FOLDERS_CONFIGURED],
               configured)
            || !save_folders_same(l.e[i].v[SAVE_FOLDERS_USED], used)
            || !l.e[i].v[SAVE_FOLDERS_TIME]
            || strncmp(l.e[i].v[SAVE_FOLDERS_TIME], stamp, 10))
      {
         if ((e = save_folders_add(&l)))
         {
            e->v[SAVE_FOLDERS_CONFIGURED] = strdup(configured);
            e->v[SAVE_FOLDERS_USED]       = strdup(used ? used : "");
            e->v[SAVE_FOLDERS_TIME]       = strdup(stamp);
            changed = true;
         }
      }
   }

   /* What was not read stays, aside; back in place when not written */
   if (changed)
   {
      size_t size = strlen(path) + PATH_ASIDE_SUFFIX_SIZE;
      char *aside = whole ? NULL : (char*)malloc(size);
      ok = whole || (aside && save_folders_set_aside(aside, size, path));
      if (ok && !(ok = save_folders_write(&l, path, sync)) && aside)
         filestream_rename(aside, path);
      free(aside);
   }
   if (device || (tagged && ok))
      save_backups_device_kept = true;
   save_folders_free(&l);
   free(path);
   return ok;
}
