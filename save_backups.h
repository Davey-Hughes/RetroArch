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
#include <lists/string_list.h>

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
/* Held names a not-loaded automatic state has beside itself. */
#define SAVE_BACKUPS_HELD_MAX        9

enum save_backup_result
{
   SAVE_BACKUP_MADE = 0,
   SAVE_BACKUP_NO_FILE,
   SAVE_BACKUP_FAILED,
   /* save_backups_move_begin(): the rename failed, the copy is owed */
   SAVE_BACKUP_COPY_OWED
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

/* A parsed name "<Game> - <time> - <seq>-<device> - <core> - <reason><ext>",
 * or from before numbering "<Game> - <time>[-<n>] - <core> - <reason><ext>". */
struct save_backup_name
{
   size_t game_len; /* the game is the first game_len bytes */
   size_t core_off;
   size_t core_len;
   size_t ext_off;  /* to the end of the name */
   unsigned number; /* 1 when the name has none */
   unsigned seq;    /* 0 when the name has none */
   enum save_backup_reason reason;
   char time[20];   /* "YYYY-MM-DD HH-MM-SS" */
   char device[5];  /* "" when the name has none */
   /* The time copies are ordered by: the parse sets it to time; among
    * a folder's copies, the latest of its device's numbered up to it */
   char eff[20];
};

/* Copies the folders copies are placed by and the core named in them.
 * An empty or NULL @savefile_dir is the content folder. Main thread,
 * while no copy is being made. NULLs throughout clear them. Until a
 * save_folders.json gave this device's tag, reads it from
 * @config_dir's. */
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
 * is written when the newest copy of @path there is @core's, made on
 * this device, and holds the same bytes.
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

/* save_backups_move_to() up to the copy: renames @path into @dir, or
 * when that fails returns COPY_OWED with the copy's path in @s and
 * nothing written. */
enum save_backup_result save_backups_move_begin(const char *dir,
      const char *path, const char *core, enum save_backup_reason reason,
      char *s, size_t len);

/* Copies @path to @to a piece at a time, synced and renamed into place
 * as an atomic synced write is. Its temporary is opened first and goes
 * on any later failure; one made beforehand to hold @to's name that
 * can't be opened stays for the next sync's sweep. Reads no roots:
 * safe off the main thread. */
bool save_backups_copy_synced(const char *path, const char *to);

/* Held name @n (1 to SAVE_BACKUPS_HELD_MAX) of @path, in its own
 * folder: "<path>.not-loaded.prev", then "<path>.not-loaded.<n>.prev".
 * A not-loaded automatic state that cannot move into its backups waits
 * there, on its own volume, until its copy is made. Returns its
 * length, 0 when it does not fit. */
size_t save_backups_held_name(char *s, size_t len, const char *path,
      unsigned n);

/* Whether a ".state.auto" @path has a held name in use. */
bool save_backups_held(const char *path);

/* The name save_backups_copy_to() would give a copy of @path into @s.
 * Writes no file. */
enum save_backup_result save_backups_name_copy(char *s, size_t len,
      const char *dir, const char *path, const char *core,
      enum save_backup_reason reason);

/* Deletes the temporary files of @path's @reason copies in @dir: copies
 * that never finished. Returns the copies themselves, newest first,
 * in a new list; NULL on failure. */
struct string_list *save_backups_delete_temps(const char *dir,
      const char *path, const char *core, enum save_backup_reason reason);

/* Retention for a copy of @path at @made, put there by its own copy:
 * as save_backups_copy_to() runs it, @made counted and kept. */
void save_backups_retain_made(const char *dir, const char *path,
      const char *core, enum save_backup_reason reason, const char *made);

/* Deletes @path once its copy at @to is made. A delete that fails
 * deletes the copy and returns false, unless the original went all the
 * same (a network drive's lost reply). */
bool save_backups_move_finish(const char *path, const char *to);

/* save_backups_copy_to() into save_backups_dir() of @path, or when no
 * name fits there, into @path's own folder where save_backups_recover()
 * writes. With @seq: in, a copy retention keeps (0: none); out, the
 * copy that holds the data (0: none), with 0 in never a copy from
 * before numbering. A copy is held by its number; at the last number,
 * which copies share, by a value above every number made from its
 * time. */
enum save_backup_result save_backups_copy_image(const char *path,
      enum save_backup_reason reason, const void *data, size_t len,
      bool sync, unsigned *seq);
enum save_backup_result save_backups_copy_file(const char *path,
      enum save_backup_reason reason, bool sync, unsigned *seq);

/* @path's own folder where save_backups_recover() writes, under
 * <config>, or with @app_data under <app data>, into @s ("" and false
 * when that root is not set). Builds the name only. */
bool save_backups_recovery_folder(char *s, size_t len, const char *path,
      bool app_data);

/* When no name of a copy of @path for @reason, named with @core,
 * fits in the folder @dir: the first of @config_rec and @app_rec
 * (save_backups_recovery_folder(); "" skipped) that can be made and
 * one fits in, into @dir. False, @dir as it was, otherwise. Lists the
 * folders; reads no roots when @core is set. */
bool save_backups_recovery_pick(char *dir, size_t len,
      const char *config_rec, const char *app_rec, const char *path,
      const char *core, enum save_backup_reason reason);

/* save_backups_recovery_pick() from save_backups_dir() of @path, into
 * @s. */
bool save_backups_recovery_dir(char *s, size_t len, const char *path,
      const char *core, enum save_backup_reason reason);

/* Writes @data, meant for @path, as a new "unsaved" file: in the copy
 * folder, else <config>/recovery, else <app data>/recovery. When the
 * newest one, wherever it is, holds @data, that file serves instead.
 * Puts the path in @s and returns its length; 0 when none could be. */
size_t save_backups_recover(char *s, size_t len, const char *path,
      const void *data, size_t data_len, bool sync);

/* As save_backups_recover(), never in the copy folder: for a save whose
 * folder is missing. */
size_t save_backups_recover_away(char *s, size_t len, const char *path,
      const void *data, size_t data_len, bool sync);

/* The newest "unsaved" file of @path changed after @mtime (INT64_MIN:
 * any), from where save_backups_recover() writes; its length, or 0. */
size_t save_backups_find_recovery(char *s, size_t len, const char *path,
      int64_t mtime);

#define SAVE_BACKUPS_FOLDERS_FILE "save_folders.json"

/* <config>/save_folders.json: adds the save and state roots as an
 * entry when they differ from the last one, else updates its last_seen
 * once a day; with @configured, also a note that @used stood in for
 * it; and this device's tag when it holds none, else takes the tag
 * from it. Rewritten only when something changed. */
bool save_backups_note_folders(const char *configured, const char *used,
      bool sync);

RETRO_END_DECLS

#endif
