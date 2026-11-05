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

#ifndef __RARCH_SAVE_ITEMS_H
#define __RARCH_SAVE_ITEMS_H

#include <stddef.h>

#include <boolean.h>
#include <libretro.h>
#include <lists/string_list.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define SAVE_ITEMS_INTERFACE_VERSION 1

/* No such item: save_item_t::prev and ::next. */
#define SAVE_ITEMS_NONE 0xffffffffu

/* What GET_SAVE_FRONTEND_INTERFACE leaves set in each mask. */
#define SAVE_ITEMS_STORAGE_KINDS ( \
      RETRO_SAVE_STORAGE_BIT(RETRO_SAVE_STORAGE_CORE_FILE) \
    | RETRO_SAVE_STORAGE_BIT(RETRO_SAVE_STORAGE_CORE_FOLDER) \
    | RETRO_SAVE_STORAGE_BIT(RETRO_SAVE_STORAGE_FRONTEND_BUFFER) \
    | RETRO_SAVE_STORAGE_BIT(RETRO_SAVE_STORAGE_FRONTEND_BLOB) \
    | RETRO_SAVE_STORAGE_BIT(RETRO_SAVE_STORAGE_IN_CONTENT) \
    | RETRO_SAVE_STORAGE_BIT(RETRO_SAVE_STORAGE_EXTERNAL) \
    | RETRO_SAVE_STORAGE_BIT(RETRO_SAVE_STORAGE_NOT_PERSISTED))
#define SAVE_ITEMS_ITEM_FLAGS ( \
      RETRO_SAVE_FLAG_CONTAINS_CONTENT \
    | RETRO_SAVE_FLAG_DIFF_OF_CONTENT \
    | RETRO_SAVE_FLAG_RELOAD_NEEDS_RESET \
    | RETRO_SAVE_FLAG_CORE_REWRITES \
    | RETRO_SAVE_FLAG_LIVE_ELSEWHERE \
    | RETRO_SAVE_FLAG_HOST_CLOCK \
    | RETRO_SAVE_FLAG_SENSITIVE \
    | RETRO_SAVE_FLAG_FRONTEND_MAY_TRANSFORM \
    | RETRO_SAVE_FLAG_HARDWARE_SHARED \
    | RETRO_SAVE_FLAG_EXACT_SIZE)
#define SAVE_ITEMS_FEATURES RETRO_SAVE_FEATURE_LEGACY_LOAD

/* The smallest retro_save_interface::item_size accepted. */
#define SAVE_ITEMS_ITEM_MIN_SIZE sizeof(struct retro_save_item)
/* Beyond these, item_size and num_items are taken as garbage. */
#define SAVE_ITEMS_ITEM_MAX_SIZE 4096
#define SAVE_ITEMS_MAX_ITEMS 4096

typedef struct save_item
{
   /* Every string, list and the segment array belong to the set;
    * segment data stays the core's. */
   struct retro_save_item item;
   /* Resolved path; NULL when the item has none. */
   char *path;
   /* Resolved legacy_paths, NULL-terminated; NULL when none. */
   char **legacy;
   /* What a longer file held past a buffer item's segments, written
    * back after them. */
   void *tail;
   size_t tail_size;
   /* Index of the same id in the set this one replaced. */
   unsigned prev;
   /* Index of the same id in the set that replaced this one. */
   unsigned next;
   /* New to the set or moved, at a file another item of the replaced
    * set used with another format or size: a safety copy is owed
    * before its first write. */
   bool type_changed;
   /* Left out for a finding: described only, with no path, and it
    * retires no legacy memory. */
   bool left_out;
   /* Bit 1 << memory type: at that memory's .srm or .rtc file, or a
    * folder holding it, which the core did not report at the call. */
   unsigned char legacy_unreported;
} save_item_t;

typedef struct save_items_set
{
   /* items is NULL here: see save_items_set::items. entries points
    * at save_items_set::entries or is NULL. */
   struct retro_save_interface iface;
   struct retro_save_entry_interface entries;
   save_item_t *items;
   unsigned num_items;
   /* The .srm and .rtc names, two PATH_MAX_LENGTH halves, while an item
    * has legacy_unreported bits; NULL otherwise. */
   char *legacy_names;
} save_items_set_t;

typedef struct save_items_state
{
   /* RETRO_SAVE_FEATURE_* the primary instance negotiated. */
   uint64_t features;
   save_items_set_t *current;
   /* What the last registration replaced, or ended when refused, kept
    * until save_items_drop_previous() so its buffers can be read
    * during that SET_SAVE_INTERFACE call. */
   save_items_set_t *previous;
   /* Legacy memories of sets a refusal ended, or of items a later set
    * left out: retired until the content unloads. */
   unsigned *retired;
   unsigned num_retired;
   /* The last registration's findings, for get_issues, and the slots
    * kept after them for the first fill's. */
   struct retro_save_issue *issues;
   unsigned num_issues;
   unsigned issues_room;
   /* Findings logged, and ids of left-out items told, since the
    * content loaded: each is said once. */
   char **logged;
   unsigned num_logged;
   char **told;
   unsigned num_told;
   /* The last registration was refused. */
   bool refused;
   /* The last registration was refused and ended previous. */
   bool dropped;
   /* A refusal was shown since the content loaded. */
   bool refusal_shown;
   /* The core called GET_SAVE_FRONTEND_INTERFACE as the primary. */
   bool asked;
   /* A SET without that GET, or off the core's thread, was logged. */
   bool unasked_told;
   bool thread_told;
   /* The core's name, for the lines save.c logs. */
   char *core;
   /* The blob_size, blob_save, get_generation or can_snapshot call in
    * progress, by name; NULL when none. */
   const char *callback;
} save_items_state_t;

/* What names an item's file, as runloop sees it when asked. */
struct save_items_paths
{
   /* The core's name, for logs. */
   const char *core;
   /* Content paths of a subsystem session; NULL outside one. */
   const struct string_list *subsystem;
   /* Content 0's .srm outside a subsystem session. */
   const char *savefile;
   /* GET_SAVE_DIRECTORY. */
   const char *savefile_dir;
   /* GET_SYSTEM_DIRECTORY. */
   const char *system_dir;
   /* The content path outside a subsystem session. */
   const char *content;
   /* Slot 0's savestate; other slots add their number, the auto state
    * ".auto", and a thumbnail ".png". */
   const char *savestate;
   /* Replays: auto as is, slots add their number. */
   const char *replay;
   /* The disk control index; NULL when the frontend keeps none. */
   const char *disk_index;
   bool savefile_dir_is_dir;
   /* By memory type: the core reports RETRO_MEMORY_SAVE_RAM or
    * RETRO_MEMORY_RTC, so the frontend writes its file unless retired. */
   bool legacy_live[2];
};

save_items_state_t *save_items_state_get_ptr(void);

/* RETRO_ENVIRONMENT_GET_SAVE_FRONTEND_INTERFACE for one instance
 * (enum retro_save_instance). */
bool save_items_get_frontend_interface(
      struct retro_save_frontend_interface *fi, unsigned instance);

/* RETRO_ENVIRONMENT_SET_SAVE_INTERFACE. The old set becomes
 * save_items_state::previous either way. On failure nothing is
 * current, and save_items_state::dropped tells whether a set ended. */
bool save_items_register(const struct retro_save_interface *iface,
      const struct save_items_paths *paths);

/* At its first fill, item @i of the current set is checked again
 * with the legacy memories the core reports now (@live, by memory
 * type), and left out as at the call. True when it was. */
bool save_items_check_reported(unsigned i, const bool *live);
/* Names every item of the current set again. */
void save_items_resolve(const struct save_items_paths *paths);

void save_items_drop_previous(void);

/* Whether a registered item names this memory in legacy_memory. */
bool save_items_legacy_retired(unsigned memory_type);

/* Whether the frontend loads and writes this item's file. */
bool save_items_frontend_owned(const save_item_t *it);

/* Whether the core negotiated RETRO_SAVE_FEATURE_LEGACY_LOAD and
 * registered legacy_load(). */
bool save_items_can_legacy_load(void);

/* True once per content load, after a registration was refused: the
 * caller tells the user. */
bool save_items_take_refusal(void);

/* The refusals the primary would meet for @iface, logged as for it;
 * nothing is kept. True when it would be accepted. Run-ahead's second
 * instance gets this answer. */
bool save_items_check(const struct retro_save_interface *iface,
      const char *core);

/* Logs, once per content load, a SET_SAVE_INTERFACE before
 * GET_SAVE_FRONTEND_INTERFACE or from @other_thread. */
void save_items_check_caller(const char *core, bool other_thread);

/* "<core>: save item <i> "<id>" ("<label>")", for a line about item
 * @i of @set. */
void save_items_who(char *s, size_t len, const save_items_set_t *set,
      unsigned i);

/* The label, else the id, of a left-out item of the current set that
 * the frontend would write, not told of since the content loaded, once
 * each; NULL when none. */
const char *save_items_take_left_out(void);

/* False, logged, inside the core's blob_size, blob_save,
 * get_generation or can_snapshot: a registration there frees the set
 * they read. */
bool save_items_can_register(void);

void save_items_clear(void);

/* Bytes item @i of @set holds now: its segments and kept tail, or
 * blob_size() when within max_size. 0 when there is nothing to copy. A
 * blob is asked through the core, so @set must be the current one, or
 * the replaced one during its SET_SAVE_INTERFACE. */
size_t save_items_size(const save_items_set_t *set, unsigned i);

/* The core's blob_size(), with no max_size applied. */
size_t save_items_blob_size(const save_items_set_t *set, unsigned i);

/* The core's get_generation(); 0 without one. */
uint64_t save_items_generation(const save_items_set_t *set, unsigned i);

/* The core's can_snapshot(); true without one. */
bool save_items_can_snapshot(const save_items_set_t *set, unsigned i);

/* Copies item @i into @buf, which holds save_items_size() bytes: the
 * segments in order, then the tail, or blob_save(). */
bool save_items_copy_out(const save_items_set_t *set, unsigned i,
      void *buf, size_t len);

/* The segments alone: @len is save_items_size() less the tail. */
bool save_items_copy_out_head(const save_items_set_t *set, unsigned i,
      void *buf, size_t len);

/* Loads @len bytes into item @i: the leading segments of a buffer (a
 * short image leaves the rest as it is, a long one is cut), or the
 * whole image through blob_load(), which may refuse. */
bool save_items_copy_in(const save_items_set_t *set, unsigned i,
      const void *buf, size_t len);

/* Keeps what @len bytes read for current buffer item @i hold past its
 * segments as its tail. */
void save_items_keep_tail(unsigned i, const void *buf, size_t len);

void save_items_drop_tail(unsigned i);

/* Drops @len bytes of current item @i's tail from offset @at. */
void save_items_cut_tail(unsigned i, size_t at, size_t len);

/* Moves the front of current item @i's tail into the segment bytes
 * from offset @from on, which a re-registration added. Returns the
 * bytes moved. */
size_t save_items_grow_from_tail(unsigned i, size_t from);

/* Whether @path is the file of a registered item: a frontend-owned
 * file, a core file, or a core folder or a file in it that its
 * patterns select. */
bool save_items_path_is_live(const char *path);

/* <core_assets>/state_undo/<library_name>/<content name>: where the
 * safety copies and state loads keep one copy of a file per basename.
 * @content is the content path without its extension. 0 when a part
 * is empty. */
size_t save_items_state_undo_dir(char *s, size_t len,
      const char *core_assets, const char *library_name,
      const char *content);

RETRO_END_DECLS

#endif
