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
#include <file/file_path.h>
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <retro_miscellaneous.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <time/rtime.h>

#include "save_backups.h"

/* A whole name stays under this many bytes. */
#define SAVE_BACKUPS_NAME_MAX    200
/* Room kept in every name for the longest reason word and a "-n", so
 * the game is cut the same way for all of one file's copies. */
#define SAVE_BACKUPS_REASON_ROOM 17
#define SAVE_BACKUPS_NUMBER_ROOM 8
/* Most a cut steps back from the limit, to a UTF-8 boundary. */
#define SAVE_BACKUPS_CUT_ROOM    3
#define SAVE_BACKUPS_TIME_LEN    19

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

/* Every file this helper writes goes through here. */
static bool save_backups_write_file(const char *path,
      const void *data, int64_t len, bool sync)
{
   if (sync)
      return filestream_write_file_atomic_synced(path, data, len);
   return filestream_write_file_atomic(path, data, len);
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

bool save_backups_parse(const char *name, struct save_backup_name *out)
{
   unsigned i, reason;
   size_t n;
   const char *sep3, *sep2, *sep1, *rest, *t;
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

   t = sep1 + 3;
   n = (size_t)(sep2 - t);
   if (n < SAVE_BACKUPS_TIME_LEN)
      return false;
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
   out->number = 1;
   if (n > SAVE_BACKUPS_TIME_LEN)
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
   size_t fixed = 3 + SAVE_BACKUPS_TIME_LEN + SAVE_BACKUPS_NUMBER_ROOM
      + 3 + core_len + 3 + SAVE_BACKUPS_REASON_ROOM + ext_len;
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

static int save_backups_cmp(const struct save_backup_name *a,
      const struct save_backup_name *b)
{
   int c = memcmp(a->time, b->time, SAVE_BACKUPS_TIME_LEN);
   if (c)
      return c;
   return a->number < b->number ? -1 : a->number > b->number;
}

/* Deletes the oldest copies of one file for @reason in @list, all but
 * SAVE_BACKUPS_KEEP counting the one just made, which is not in it. */
static void save_backups_retain(struct string_list *list,
      struct save_backup_name *names, const bool *mine,
      enum save_backup_reason reason, size_t ext_len)
{
   size_t i;
   size_t count = 0;
   for (i = 0; i < list->size; i++)
   {
      if (!mine[i] || names[i].reason != reason)
         continue;
      /* Near its limit, the game may be a longer name's cut: never
       * pruned */
      if (names[i].game_len + SAVE_BACKUPS_CUT_ROOM
            < save_backups_game_max(names[i].core_len, ext_len))
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

/* Writes @data as a copy of @path into @dir, its path in @s. With
 * @skip_same, nothing is written when the newest copy holds it. */
static enum save_backup_result save_backups_write_copy(char *s, size_t len,
      const char *dir, const char *path, const char *core,
      enum save_backup_reason reason, const void *data, size_t data_len,
      bool skip_same, bool sync)
{
   size_t i, _len, base_len, game_len;
   char stamp[SAVE_BACKUPS_TIME_LEN + 1];
   time_t now;
   struct tm tm_;
   unsigned number                 = 1;
   size_t newest                   = (size_t)-1;
   struct string_list *list        = NULL;
   struct save_backup_name *names  = NULL;
   bool *mine                      = NULL;
   enum save_backup_result result  = SAVE_BACKUP_FAILED;
   const char *base                = path_basename_nocompression(path);
   const char *ext                 = save_backups_ext(base);
   char *field                     = save_backups_core_field(
         core ? core : save_backups_roots[SAVE_BACKUPS_ROOT_CORE]);

   s[0] = '\0';
   if (!field || string_is_empty(dir))
      goto end;
   base_len = (size_t)(ext - base);
   game_len = save_backups_game_len(base, base_len, strlen(field),
         strlen(ext));

   /* rtime_localtime leaves tm_ unwritten when localtime() fails */
   memset(&tm_, 0, sizeof(tm_));
   time(&now);
   rtime_localtime(&now, &tm_);
   if (!strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &tm_))
      goto end;

   if ((list = dir_list_new(dir, NULL, false, true, false, false))
         && list->size)
   {
      names = (struct save_backup_name*)malloc(
            list->size * sizeof(*names));
      mine  = (bool*)calloc(list->size, sizeof(*mine));
      if (!names || !mine)
         goto end;
   }
   for (i = 0; names && i < list->size; i++)
   {
      const char *name = path_basename_nocompression(list->elems[i].data);
      if (     !save_backups_parse(name, &names[i])
            || names[i].game_len != game_len
            || memcmp(name, base, game_len)
            || !string_is_equal(name + names[i].ext_off, ext))
         continue;
      mine[i] = true;
      if (newest == (size_t)-1
            || save_backups_cmp(&names[i], &names[newest]) > 0)
         newest = i;
      if (     !memcmp(names[i].time, stamp, SAVE_BACKUPS_TIME_LEN)
            && names[i].number >= number)
         number = names[i].number + 1;
   }

   if (     skip_same
         && newest != (size_t)-1
         && filestream_matches_buf(list->elems[newest].data, data, data_len))
   {
      strlcpy(s, list->elems[newest].data, len);
      result = SAVE_BACKUP_MADE;
      goto end;
   }

   if (!path_mkdir(dir))
      goto end;
   _len = fill_pathname_join(s, dir, "", len);
   if (_len + game_len >= len)
      goto end;
   memcpy(s + _len, base, game_len);
   _len += game_len;
   s[_len] = '\0';
   /* Never over another file, whatever its name's case. */
   for (;; number++)
   {
      if (number > 1)
         snprintf(s + _len, len - _len, " - %s-%u - %s - %s%s", stamp,
               number, field, save_backups_reasons[reason], ext);
      else
         snprintf(s + _len, len - _len, " - %s - %s - %s%s", stamp,
               field, save_backups_reasons[reason], ext);
      if (strlen(s) + 1 >= len)
         goto end;
      if (!path_is_valid(s))
         break;
   }

   if (!save_backups_write_file(s, data, (int64_t)data_len, sync))
   {
      s[0] = '\0';
      goto end;
   }
   result = SAVE_BACKUP_MADE;

   /* A cut name may be another file's too */
   if (     names
         && game_len == base_len
         && (  reason == SAVE_BACKUP_BEFORE_STATE_LOAD
            || reason == SAVE_BACKUP_CHANGED_ELSEWHERE))
      save_backups_retain(list, names, mine, reason, strlen(ext));

end:
   if (result != SAVE_BACKUP_MADE)
      s[0] = '\0';
   free(names);
   free(mine);
   if (list)
      string_list_free(list);
   free(field);
   return result;
}

enum save_backup_result save_backups_copy_to(const char *dir,
      const char *path, const char *core, enum save_backup_reason reason,
      const void *data, size_t len, bool sync)
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
         core, reason, data, len, true, sync);
   free(buf);
   free(made);
   return result;
}

enum save_backup_result save_backups_copy_image(const char *path,
      enum save_backup_reason reason, const void *data, size_t len,
      bool sync)
{
   enum save_backup_result result = SAVE_BACKUP_FAILED;
   char *dir = (char*)malloc(PATH_MAX_LENGTH);
   if (dir && save_backups_dir(dir, PATH_MAX_LENGTH, path))
      result = save_backups_copy_to(dir, path, NULL, reason,
            data ? data : "", data ? len : 0, sync);
   free(dir);
   return result;
}

enum save_backup_result save_backups_copy_file(const char *path,
      enum save_backup_reason reason, bool sync)
{
   enum save_backup_result result = SAVE_BACKUP_FAILED;
   char *dir = (char*)malloc(PATH_MAX_LENGTH);
   if (dir && save_backups_dir(dir, PATH_MAX_LENGTH, path))
      result = save_backups_copy_to(dir, path, NULL, reason, NULL, 0, sync);
   free(dir);
   return result;
}

size_t save_backups_recover(char *s, size_t len, const char *path,
      const void *data, size_t data_len, bool sync)
{
   unsigned i;
   bool made = false;
   char *dir = (char*)malloc(PATH_MAX_LENGTH);
   s[0]      = '\0';
   if (!dir)
      return 0;
   if (save_backups_dir(dir, PATH_MAX_LENGTH, path))
      made = save_backups_write_copy(s, len, dir, path, NULL,
            SAVE_BACKUP_UNSAVED, data, data_len, false, sync)
         == SAVE_BACKUP_MADE;
   for (i = SAVE_BACKUPS_ROOT_CONFIG;
         !made && i <= SAVE_BACKUPS_ROOT_APP_DATA; i++)
   {
      if (!save_backups_roots[i])
         continue;
      fill_pathname_join(dir, save_backups_roots[i],
            SAVE_BACKUPS_RECOVERY_DIR, PATH_MAX_LENGTH);
      made = save_backups_write_copy(s, len, dir, path, NULL,
            SAVE_BACKUP_UNSAVED, data, data_len, false, sync)
         == SAVE_BACKUP_MADE;
   }
   free(dir);
   return made ? strlen(s) : 0;
}
