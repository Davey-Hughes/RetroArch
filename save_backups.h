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

#ifndef __RARCH_SAVE_BACKUPS_H
#define __RARCH_SAVE_BACKUPS_H

#include <stddef.h>

#include <retro_common_api.h>
#include <boolean.h>

RETRO_BEGIN_DECLS

/* Safety copies of saves and states, kept in a backups folder beside
 * them, and recovery files for images that could not be written.
 * Callable from any thread; logs nothing. */

#define SAVE_BACKUPS_DIR             "backups"
#define SAVE_BACKUPS_RECOVERY_DIR    "recovery"
/* Kept free under the save root for save records. */
#define SAVE_BACKUPS_RECORDS_DIR     "records"
/* The system folder's copies go in <save root>/backups/system. */
#define SAVE_BACKUPS_SYSTEM_DIR      "system"
/* The core field of cloud sync's own backups. */
#define SAVE_BACKUPS_CORE_CLOUD_SYNC "cloud-sync"
/* Copies kept per file for each repeating reason. */
#define SAVE_BACKUPS_KEEP            10

enum save_backup_result
{
   SAVE_BACKUP_MADE = 0,
   SAVE_BACKUP_NO_FILE,
   SAVE_BACKUP_FAILED
};

/* The reason word in a copy's name. Fixed English, never translated. */
enum save_backup_reason
{
   SAVE_BACKUP_BEFORE_STATE_LOAD = 0,
   SAVE_BACKUP_UNREADABLE,
   SAVE_BACKUP_CUT,
   SAVE_BACKUP_CHANGED_ELSEWHERE,
   SAVE_BACKUP_NOT_LOADED,
   SAVE_BACKUP_CORE_CHANGED,
   SAVE_BACKUP_OTHER_CONTENT,
   SAVE_BACKUP_BLANKED,
   SAVE_BACKUP_UNSAVED,
   SAVE_BACKUP_REPLACED,
   SAVE_BACKUP_DELETED,
   SAVE_BACKUP_REASON_LAST
};

/* A parsed name "<Game> - <time>[-<n>] - <core> - <reason><ext>". */
struct save_backup_name
{
   size_t game_len; /* the game is the first game_len bytes */
   size_t core_off;
   size_t core_len;
   size_t ext_off;  /* to the end of the name */
   unsigned number; /* 1 when the name has none */
   enum save_backup_reason reason;
   char time[20];   /* "YYYY-MM-DD HH-MM-SS" */
};

/* Copies the folders copies are placed by and the core named in them.
 * An empty or NULL @savefile_dir is the content folder. Main thread,
 * while no copy is being made. NULLs throughout clear them. */
void save_backups_set_roots(const char *savefile_dir,
      const char *savestate_dir, const char *content_dir,
      const char *system_dir, const char *config_dir,
      const char *app_data_dir, const char *core);

/* The folder copies of the file at @path go to. Returns its length,
 * 0 when there is none. */
size_t save_backups_dir(char *s, size_t len, const char *path);

/* Reads a file name (no folder). False when it is not a copy's name. */
bool save_backups_parse(const char *name, struct save_backup_name *out);

/* @reason's word, as in a copy's name. */
const char *save_backups_reason_name(enum save_backup_reason reason);

/* Writes @data, or with NULL the file at @path as it is, into @dir as
 * a copy of @path, named with @core (NULL: the roots' core). Nothing
 * is written when the newest copy of @path there holds the same bytes.
 * NO_FILE: @data is NULL and there is no file at @path.
 * @sync: the write reaches the disk before it replaces anything (off
 * the frame loop); false inside a frame. */
enum save_backup_result save_backups_copy_to(const char *dir,
      const char *path, const char *core, enum save_backup_reason reason,
      const void *data, size_t len, bool sync);

/* Moves the file at @path into @dir under the name
 * save_backups_copy_to() would give its copy: a rename, or across
 * drives a synced copy and then the delete, undone when the original
 * stays. */
enum save_backup_result save_backups_move_to(const char *dir,
      const char *path, const char *core, enum save_backup_reason reason);

/* save_backups_copy_to() into save_backups_dir() of @path. */
enum save_backup_result save_backups_copy_image(const char *path,
      enum save_backup_reason reason, const void *data, size_t len,
      bool sync);
enum save_backup_result save_backups_copy_file(const char *path,
      enum save_backup_reason reason, bool sync);

/* Writes @data, meant for @path, as a new "unsaved" file: in the copy
 * folder, else <config>/recovery, else <app data>/recovery. When the
 * newest one, wherever it is, holds @data, that file serves instead.
 * Puts the path in @s and returns its length; 0 when none could be. */
size_t save_backups_recover(char *s, size_t len, const char *path,
      const void *data, size_t data_len, bool sync);

/* The newest "unsaved" file of @path changed after @mtime (INT64_MIN:
 * any), from where save_backups_recover() writes; its length, or 0. */
size_t save_backups_find_recovery(char *s, size_t len, const char *path,
      int64_t mtime);

#define SAVE_BACKUPS_FOLDERS_FILE "save_folders.json"

/* <config>/save_folders.json: adds the save and state roots as an
 * entry when they differ from the last one, else updates its last_seen
 * once a day; with @configured, also a note that @used stood in for
 * it. Rewritten only when something changed. */
bool save_backups_note_folders(const char *configured, const char *used,
      bool sync);

RETRO_END_DECLS

#endif
