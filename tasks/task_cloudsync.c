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

#include <features/features_cpu.h>
#include <file/file_path.h>
#include <retro_dirent.h>
#include <formats/rjson.h>
#include <formats/rjson_stream.h>
#include <lists/dir_list.h>
#include <lists/file_list.h>
#include <lrc_hash.h>
#ifdef HAVE_THREADS
#include <rthreads/rthreads.h>
#endif
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <time/rtime.h>
#include <retro_atomic.h>
#include <retro_inline.h>

#include "../configuration.h"
#include "../content.h"
#include "../file_path_special.h"
#include "../msg_hash.h"
#include "../network/cloud_sync_driver.h"
#include "../paths.h"
#include "../runloop.h"
#include "../save_backups.h"
#include "../tasks/tasks_internal.h"
#include "../verbosity.h"
#include <compat/strl.h>

#include "task_cloudsync_path.h"

/* Completion callbacks hand manifest entries and downloads to the task
 * thread on lock-free lists where the atomics backend has pointer
 * operations, and under tcs_manifest_lock where it does not.
 * TASK_CLOUDSYNC_FORCE_LOCK takes the locked path on any backend, for
 * the tests that cover it on hosts whose backends all have them. */
#if defined(HAVE_THREADS) && defined(RETRO_ATOMIC_HAS_PTR) && !defined(TASK_CLOUDSYNC_FORCE_LOCK)
#define TCS_LOCK_FREE 1
#endif

/* A backup copied across drives is copied on a thread of its own. Its
 * hand-back needs a real exchange; without one the copy runs inline. */
#if defined(HAVE_THREADS) && defined(RETRO_ATOMIC_LOCK_FREE)
#define TCS_MOVE_WORKER 1
#endif

#define CSPFX "[CloudSync] "

#define MANIFEST_FILENAME_LOCAL  "manifest.local"
/* The folder each root was walked from, beside the local manifest */
#define ROOTS_FILENAME_LOCAL     "manifest.local.roots"
#define MANIFEST_FILENAME_SERVER CLOUD_SYNC_SERVER_MANIFEST

#define CS_FILE_HASH(item_file) ((char*)((item_file) ? ((item_file)->userdata) : (NULL)))
#define CS_FILE_KEY(item_file) ((item_file) ? ((item_file)->alt) : (NULL))

enum task_cloud_sync_phase
{
   CLOUD_SYNC_PHASE_BEGIN,
   CLOUD_SYNC_PHASE_FETCH_SERVER_MANIFEST,
   CLOUD_SYNC_PHASE_READ_LOCAL_MANIFEST,
   CLOUD_SYNC_PHASE_BUILD_CURRENT_MANIFEST,
   CLOUD_SYNC_PHASE_DIFF,
   CLOUD_SYNC_PHASE_UPDATE_MANIFESTS,
   CLOUD_SYNC_PHASE_END
};

/* An MD5 taken a chunk at a time across runs of the handler; see
 * task_cloud_sync_hash_step(). */
typedef struct
{
   MD5_CTX        md5;
   RFILE         *file;  /* NULL when idle */
} tcs_hash_t;

struct tcs_fetched;
struct tcs_move;

typedef struct
{
   /* Current phase.  Written by the task thread during dispatch AND
    * by driver completion callbacks (other threads) on phase
    * transitions; read by the task thread's poll while transfers are
    * still in flight.  The pre-atomic code wrote it unlocked in the
    * callbacks while the poll read it under the old lock, which was
    * a data race (the lock never covered the write side).  Atomic
    * with release stores / acquire loads via the accessors below. */
   retro_atomic_int_t phase;
   /* In-flight async transfer count.  Written by the task thread when
    * dispatching cloud_sync_* operations and by driver completion
    * callbacks, which a driver is free to invoke from another thread.
    * All accesses go through the task_cloud_sync_waiting_* helpers
    * below; see the comment there for the ordering contract. */
   retro_atomic_int_t waiting;
   /* Manifest present on the server (may be modified by other clients)*/
   file_list_t *server_manifest;
   size_t server_idx;
   /* Server entries under a directory this sync does not cover (its
    * toggle is off), counted once when the diff starts, and how many of
    * them the diff has stepped over so far.  Progress leaves both out:
    * they are carried into the updated manifest, not synced. */
   size_t server_out_of_scope;
   size_t server_skipped;
   /* Last-known state to compare agasint.*/
   file_list_t *local_manifest;
   size_t local_idx;
   file_list_t *current_manifest;
   size_t current_idx;
   file_list_t *updated_server_manifest;
   /* local manifest is sometimes different due to conflicts */
   file_list_t *updated_local_manifest;
#if defined(TCS_LOCK_FREE)
   /* Entries on their way into the two lists above; see
    * tcs_manifest_add_t. */
   retro_atomic_ptr_t manifest_adds;
#endif
   /* Hashes taken a chunk at a time between runs: the diff's current
    * file (which owns hash_cur.file) and the download being checked
    * (whose stream is its tcs_fetched record's). hash_buf is the read
    * buffer for both, allocated once per sync. */
   tcs_hash_t hash_cur;
   tcs_hash_t hash_fetched;
   uint8_t *hash_buf;
   /* Downloads waiting to be hashed: handed over by the fetch callback,
    * on whatever thread it runs (fetched_in), and taken by the task
    * thread, which works through them in order (fetched). */
#if defined(TCS_LOCK_FREE)
   retro_atomic_ptr_t fetched_in;
#else
   struct tcs_fetched *fetched_in;
#endif
   struct tcs_fetched *fetched;
#if defined(TCS_MOVE_WORKER)
   /* Backups being copied across drives, oldest first */
   struct tcs_move *moves;
#endif
   /* The walk of the synced directories that builds current_manifest,
    * a few entries per run: the directories open from the root of
    * dirlist[walk_root_idx] down, each with the length of its path in
    * walk_path; walk_bufs holds walk_root, walk_path and two scratch
    * paths, PATH_MAX_LENGTH each. */
   struct tcs_walk_dir *walk;
   size_t walk_depth;
   size_t walk_cap;
   size_t walk_root_idx;
   char *walk_bufs;
   bool need_manifest_uploaded;
   bool failures;
   bool conflicts;
   const char *fatal_error;
   /* Conflict resolution mode: 0=none, 1=keep_local, 2=keep_server,
    * 3=upload_all (files the server lost, changed here or not) */
   int conflict_resolution;
   uint32_t uploads;
   uint32_t downloads;
   retro_time_t start_time;
   /* Captured on the main thread when the task is pushed: the handler
    * runs on the threaded task queue's worker, and a worker takes what
    * it needs as arguments rather than reading the settings mid-sync. */
   bool destructive;
   char dir_core_assets[DIR_MAX_LENGTH];
   /* task_cloud_sync_server_id() */
   char server_id[33];
   /* The directory map for this sync, built at push. Per task rather
    * than a process-wide cache: a fresh list per sync is a handful of
    * strdups, and it cannot go stale against the sync toggles. An
    * element's attr.i holds its CS_ROOT_* flags. */
   struct string_list *dirlist;
   /* The roots file as the last sync left it: data is a root's name,
    * userdata the folder it was walked from. NULL when there is none. */
   struct string_list *roots;
   /* Keys this sync did not delete on the server (CS_ROOT_HELD), and
    * how many: the list falls short when out of memory. */
   struct string_list *held;
   unsigned held_count;
   /* Folders whose missing files are held, as "saves/Snes9x/" */
   struct string_list *held_dirs;
   /* files the server lost, not uploaded: unchanged here */
   unsigned unsent_count;
   /* Whoever pushed the sync, told when it is done. */
   retro_task_callback_t cb;
   void *cb_data;
} task_cloud_sync_state_t;

/* An entry for updated_server_manifest or updated_local_manifest, on
 * its way there.  Fetch/upload/delete completion callbacks can add
 * entries concurrently with each other and with the task thread's
 * dispatch-failure paths, so each pushes a record on a lock-free list
 * and the two manifests stay the task thread's alone: it folds the
 * records in once `waiting` has drained to zero, which is the first
 * time the manifests are read.  A manifest is sorted before it is
 * written, so the order the records arrive in does not matter.
 *
 * A backend with no pointer atomics appends under a lock instead. */
#ifdef HAVE_THREADS
#ifdef TCS_LOCK_FREE
typedef struct tcs_manifest_add
{
   struct tcs_manifest_add *next;
   char *key;     /* a copy, until the manifest has its own */
   char *hash;    /* a copy: the manifest's from here on */
   bool server;
} tcs_manifest_add_t;
#else
static slock_t *tcs_manifest_lock = NULL;
#endif
#endif

/* Ordering contract for `waiting`: a completion callback publishes
 * its side effects (phase, failures, need_manifest_uploaded, list
 * appends) *before* dropping the counter with a release/acq_rel
 * operation; the task handler's poll reads the counter with an
 * acquire load, so once it observes the drained count all of those
 * writes are visible to it.  This reproduces the release/acquire
 * chain the old lock provided on the decrement side while also
 * covering the increments that previously raced. */
static INLINE void task_cloud_sync_waiting_set(
      task_cloud_sync_state_t *sync_state, int v)
{
   retro_atomic_store_release_int(&sync_state->waiting, v);
}

static INLINE void task_cloud_sync_waiting_inc(
      task_cloud_sync_state_t *sync_state)
{
   retro_atomic_fetch_add_int(&sync_state->waiting, 1);
}

static INLINE void task_cloud_sync_waiting_dec(
      task_cloud_sync_state_t *sync_state)
{
   retro_atomic_fetch_sub_int(&sync_state->waiting, 1);
}

static INLINE int task_cloud_sync_waiting_get(
      task_cloud_sync_state_t *sync_state)
{
   return retro_atomic_load_acquire_int(&sync_state->waiting);
}

static INLINE void task_cloud_sync_phase_set(
      task_cloud_sync_state_t *sync_state,
      enum task_cloud_sync_phase phase)
{
   retro_atomic_store_release_int(&sync_state->phase, (int)phase);
}

static INLINE enum task_cloud_sync_phase task_cloud_sync_phase_get(
      task_cloud_sync_state_t *sync_state)
{
   return (enum task_cloud_sync_phase)
         retro_atomic_load_acquire_int(&sync_state->phase);
}

/* Forward declarations for conflict resolution */
static void task_cloud_sync_upload_current_file(task_cloud_sync_state_t *sync_state);
static void task_cloud_sync_fetch_server_file(task_cloud_sync_state_t *sync_state);
static bool task_cloud_sync_delete_current_file(task_cloud_sync_state_t *sync_state);
/* Defined below task_cloud_sync_write_updated_manifest(), used above it
 * by the server-manifest upload callback. */
static void task_cloud_sync_commit_local_manifest(task_cloud_sync_state_t *sync_state);

static void task_cloud_sync_begin_handler(void *user_data, const char *path, bool success, RFILE *file)
{
   retro_task_t            *task       = (retro_task_t *)user_data;
   task_cloud_sync_state_t *sync_state = NULL;

   if (!task)
      return;

   if (!(sync_state = (task_cloud_sync_state_t *)task->state))
      return;

   if (success)
   {
      RARCH_LOG(CSPFX "Begin succeeded.\n");
      task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_FETCH_SERVER_MANIFEST);
   }
   else
   {
      RARCH_WARN(CSPFX "Begin failed.\n");
      sync_state->failures = true;
      task_free_title(task);
      task_set_title(task, strdup("Cloud Sync failed"));
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
   }
   task_cloud_sync_waiting_set(sync_state, 0);
}

static bool tcs_object_member_handler(void *ctx, const char *s, size_t len)
{
   file_list_t      *list = (file_list_t *)ctx;
   struct item_file *item;

   if (!list->size)
      return false;

   item = &list->list[list->size - 1];
   if (string_is_equal(s, "path"))
      item->type = 1;
   else
      item->type = 0;
   return true;
}

static bool tcs_string_handler(void *ctx, const char *s, size_t len)
{
   file_list_t      *list = (file_list_t *)ctx;
   size_t            idx;
   struct item_file *item;

   if (!list->size)
      return false;

   idx  = list->size - 1;
   item = &list->list[idx];
   if (item->type)
      file_list_set_alt_at_offset(list, idx, s);
   else
      list->list[idx].userdata = strdup(s);
   return true;
}

static bool tcs_start_object_handler(void *ctx)
{
   file_list_t *list = (file_list_t *)ctx;
   return file_list_append(list, NULL, NULL, 0, 0, 0);
}

static bool tcs_end_object_handler(void *ctx)
{
   file_list_t      *list = (file_list_t *)ctx;
   struct item_file *item;

   if (!list->size)
      return false;

   item = &list->list[list->size - 1];
   if (!CS_FILE_KEY(item))
      file_list_pop(list, NULL);
   else
      item->type = 0;
   return true;
}

static file_list_t *task_cloud_sync_create_manifest(RFILE *file)
{
   file_list_t    *list         = NULL;
   rjson_t        *json         = NULL;
   enum rjson_type parse_result = RJSON_ERROR;

   if (!(list = (file_list_t *)calloc(1, sizeof(file_list_t))))
      return NULL;

   if (!(json = rjson_open_filestream(file)))
   {
      free(list);
      return NULL;
   }

   parse_result = rjson_parse(json, list,
               tcs_object_member_handler,
               tcs_string_handler,
               NULL,
               tcs_start_object_handler,
               tcs_end_object_handler,
               NULL,
               NULL,
               NULL,
               NULL);

   if (parse_result != RJSON_DONE)
   {
      const char *error = rjson_get_error(json);
      RARCH_ERR(CSPFX "Invalid manifest at line %u, column %u: %s\n",
            (unsigned)rjson_get_source_line(json),
            (unsigned)rjson_get_source_column(json),
            string_is_empty(error) ? "invalid structure" : error);
      rjson_free(json);
      file_list_free(list);
      return NULL;
   }

   rjson_free(json);

   file_list_sort_on_alt(list);

   RARCH_LOG(CSPFX "Created manifest with %u files.\n", list->size);

   return list;
}

static void task_cloud_sync_manifest_filename(char *s, size_t len, bool server,
      const char *dir_core_assets)
{
   fill_pathname_join_special(s, dir_core_assets,
         server ? MANIFEST_FILENAME_SERVER : MANIFEST_FILENAME_LOCAL,
         len);
}

static void task_cloud_sync_manifest_handler(void *user_data, const char *path,
      bool success, RFILE *file)
{
   task_cloud_sync_state_t *sync_state = (task_cloud_sync_state_t *)user_data;

   if (!sync_state)
      return;

   if (!success)
   {
      RARCH_WARN(CSPFX "Server manifest fetch failed.\n");
      sync_state->failures = true;
      task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_END);
      task_cloud_sync_waiting_set(sync_state, 0);
      return;
   }

   RARCH_LOG(CSPFX "Server manifest fetch succeeded.\n");
   /* An empty body is no manifest too: rjson rejects empty input,
    * and the drivers now hand back an empty file as an open RFILE. */
   if (file && filestream_get_size(file) == 0)
   {
      filestream_close(file);
      file = NULL;
   }
   /* it is valid for there not to be a server manifest */
   if (file)
   {
      sync_state->server_manifest = task_cloud_sync_create_manifest(file);
      filestream_close(file);
      if (!sync_state->server_manifest)
      {
         sync_state->failures    = true;
         sync_state->fatal_error = "Invalid server manifest";
         task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_END);
         task_cloud_sync_waiting_set(sync_state, 0);
         return;
      }
   }
   task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_READ_LOCAL_MANIFEST);
   task_cloud_sync_waiting_set(sync_state, 0);
}

static void task_cloud_sync_fetch_server_manifest(task_cloud_sync_state_t *sync_state)
{
   char        manifest_path[PATH_MAX_LENGTH];

   task_cloud_sync_manifest_filename(manifest_path, sizeof(manifest_path), true,
         sync_state->dir_core_assets);

   task_cloud_sync_waiting_set(sync_state, 1);
   if (!cloud_sync_read(MANIFEST_FILENAME_SERVER, manifest_path, task_cloud_sync_manifest_handler, sync_state))
   {
      RARCH_WARN(CSPFX "Could not read server manifest.\n");
      task_cloud_sync_waiting_set(sync_state, 0);
      task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_END);
   }
}

/* A root walked from another folder than the last sync's */
#define CS_ROOT_CHANGED 1
/* A root whose missing files are not deleted on the server */
#define CS_ROOT_HELD    2
/* More missing files than this, and more than half of a root's */
#define CS_HOLD_MIN     10
/* A folder with this many synced files or more, all missing */
#define CS_HOLD_DIR_MIN 3
/* Folder levels a key is checked at */
#define CS_HOLD_DEPTH   32

static void task_cloud_sync_roots_filename(char *s, size_t len,
      const char *dir_core_assets)
{
   fill_pathname_join_special(s, dir_core_assets, ROOTS_FILENAME_LOCAL, len);
}

/* The roots file in @out, NULL when there is none or it can't be
 * read, and then @unreadable; the server it names in @server (33
 * bytes). False when out of memory. */
static bool task_cloud_sync_read_roots(const char *dir_core_assets,
      struct string_list **out, char *server, bool *unreadable)
{
   char                path[PATH_MAX_LENGTH];
   void               *buf   = NULL;
   int64_t             len   = 0;
   char               *name  = NULL;
   rjson_t            *json  = NULL;
   struct string_list *roots = NULL;
   bool                ok    = true;
   enum rjson_type     type;

   *out = NULL;
   task_cloud_sync_roots_filename(path, sizeof(path), dir_core_assets);
   if (!path_is_valid(path))
      return true;
   if (!filestream_read_file(path, &buf, &len) || !len)
      goto error;
   if (     !(json  = rjson_open_buffer(buf, (size_t)len))
         || !(roots = string_list_new()))
      goto oom;

   /* the root members' strings; "held" is an array and skipped */
   while ((type = rjson_next(json)) != RJSON_DONE)
   {
      const char *str;
      if (type == RJSON_ERROR)
         goto error;
      if (     type != RJSON_STRING
            || rjson_get_context_type(json) != RJSON_OBJECT)
         continue;
      str = rjson_get_string(json, NULL);
      if (rjson_get_context_count(json) & 1)
      {
         free(name);
         if (!(name = strdup(str)))
            goto oom;
      }
      else if (name && string_is_equal(name, "server"))
         strlcpy(server, str, 33);
      else if (name)
      {
         union string_list_elem_attr attr;
         char *dir = strdup(str);
         attr.i    = 0;
         if (!dir || !string_list_append(roots, name, attr))
         {
            free(dir);
            goto oom;
         }
         roots->elems[roots->size - 1].userdata = dir;
      }
   }

   free(name);
   rjson_free(json);
   free(buf);
   *out = roots;
   return true;

oom:
   ok = false;
error:
   if (ok)
   {
      RARCH_WARN(CSPFX "Could not read \"%s\": syncing every folder as a first sync.\n", path);
      *unreadable = true;
   }
   free(name);
   if (json)
      rjson_free(json);
   string_list_free(roots);
   free(buf);
   return ok;
}

/* The two folders are the same, but for trailing separators */
static bool task_cloud_sync_same_dir(const char *a, const char *b)
{
   size_t la = strlen(a);
   size_t lb = strlen(b);
   while (la > 1 && (a[la - 1] == '/' || a[la - 1] == '\\'))
      la--;
   while (lb > 1 && (b[lb - 1] == '/' || b[lb - 1] == '\\'))
      lb--;
   return la == lb && !memcmp(a, b, la);
}

/* The dirlist root @key lies under, or -1 */
static int task_cloud_sync_key_root(const struct string_list *dirlist,
      const char *key)
{
   size_t i;
   for (i = 0; i < dirlist->size; i++)
   {
      size_t len = strlen(dirlist->elems[i].data);
      if (!strncmp(key, dirlist->elems[i].data, len) && key[len] == '/')
         return (int)i;
   }
   return -1;
}

/* A root walked from another folder than the last sync's leaves the
 * local manifest, so the diff runs first-sync rules for its keys:
 * fetch, upload or conflict, never a delete. False when out of
 * memory. */
static bool task_cloud_sync_drop_changed_roots(
      task_cloud_sync_state_t *sync_state)
{
   struct string_list *dirlist = sync_state->dirlist;
   file_list_t        *local   = sync_state->local_manifest;
   file_list_t        *kept;
   bool                changed = false;
   size_t              i;

   for (i = 0; i < dirlist->size; i++)
   {
      int at = string_list_find_elem(sync_state->roots,
            dirlist->elems[i].data);
      if (at && !task_cloud_sync_same_dir(
               (const char*)sync_state->roots->elems[at - 1].userdata,
               (const char*)dirlist->elems[i].userdata))
      {
         RARCH_WARN(CSPFX "\"%s\" is now \"%s\", was \"%s\": syncing it as a first sync.\n",
               dirlist->elems[i].data,
               (const char*)dirlist->elems[i].userdata,
               (const char*)sync_state->roots->elems[at - 1].userdata);
         dirlist->elems[i].attr.i |= CS_ROOT_CHANGED;
         changed = true;
      }
   }
   if (!changed || !local)
      return true;

   if (!(kept = (file_list_t*)calloc(1, sizeof(*kept))))
      return false;
   for (i = 0; i < local->size; i++)
   {
      const char *key = CS_FILE_KEY(&local->list[i]);
      int         at  = task_cloud_sync_key_root(dirlist, key);
      if (at >= 0 && (dirlist->elems[at].attr.i & CS_ROOT_CHANGED))
         continue;
      if (!file_list_append(kept, NULL, NULL, 0, 0, 0))
      {
         file_list_free(kept);
         return false;
      }
      file_list_set_alt_at_offset(kept, kept->size - 1, key);
      kept->list[kept->size - 1].userdata = local->list[i].userdata;
      local->list[i].userdata             = NULL;
   }
   file_list_free(local);
   sync_state->local_manifest = kept;
   return true;
}

static void task_cloud_sync_write_root(rjsonwriter_t *writer,
      const char *name, const char *dir, bool *first)
{
   if (!*first)
      rjsonwriter_raw(writer, ",\n", 2);
   *first = false;
   rjsonwriter_add_spaces(writer, 2);
   rjsonwriter_add_string(writer, name);
   rjsonwriter_raw(writer, ": ", 2);
   rjsonwriter_add_string(writer, dir);
}

/* A root with keys in the local manifest that this sync did not walk
 * and no roots file named: its folder is unknown, so whichever folder
 * it gets next is a change. */
static void task_cloud_sync_write_unknown_roots(rjsonwriter_t *writer,
      const task_cloud_sync_state_t *sync_state, bool *first)
{
   char               name[32];
   char               last[32];
   const file_list_t *local = sync_state->updated_local_manifest;
   size_t             i;

   last[0] = '\0';
   for (i = 0; local && i < local->size; i++)
   {
      const char *key   = CS_FILE_KEY(&local->list[i]);
      const char *slash = key ? strchr(key, '/') : NULL;
      size_t      len   = slash ? (size_t)(slash - key) : 0;
      if (!len || len >= sizeof(name))
         continue;
      memcpy(name, key, len);
      name[len] = '\0';
      /* sorted by key, so a root's keys are together */
      if (string_is_equal(name, last))
         continue;
      strlcpy(last, name, sizeof(last));
      if (     !string_list_find_elem(sync_state->dirlist, name)
            && !string_list_find_elem(sync_state->roots, name))
         task_cloud_sync_write_root(writer, name, "", first);
   }
}

/* Written after the local manifest it describes: the folder each root
 * was walked from, and the roots this sync did not cover as the last
 * one left them. */
static void task_cloud_sync_write_roots(task_cloud_sync_state_t *sync_state)
{
   char                path[PATH_MAX_LENGTH];
   struct string_list *dirlist = sync_state->dirlist;
   struct string_list *roots   = sync_state->roots;
   rjsonwriter_t      *writer  = rjsonwriter_open_memory();
   const char         *buf     = NULL;
   bool                first   = true;
   int                 len     = 0;
   size_t              i;

   task_cloud_sync_roots_filename(path, sizeof(path),
         sync_state->dir_core_assets);
   if (writer)
   {
      rjsonwriter_raw(writer, "{\n", 2);
      if (*sync_state->server_id)
         task_cloud_sync_write_root(writer, "server",
               sync_state->server_id, &first);
      for (i = 0; i < dirlist->size; i++)
         task_cloud_sync_write_root(writer, dirlist->elems[i].data,
               (const char*)dirlist->elems[i].userdata, &first);
      for (i = 0; roots && i < roots->size; i++)
         if (!string_list_find_elem(dirlist, roots->elems[i].data))
            task_cloud_sync_write_root(writer, roots->elems[i].data,
                  (const char*)roots->elems[i].userdata, &first);
      task_cloud_sync_write_unknown_roots(writer, sync_state, &first);
      if (!first)
         rjsonwriter_raw(writer, ",\n", 2);
      rjsonwriter_add_spaces(writer, 2);
      rjsonwriter_add_string(writer, "held");
      rjsonwriter_raw(writer, ": [", 3);
      for (i = 0; sync_state->held && i < sync_state->held->size; i++)
      {
         if (i)
            rjsonwriter_raw(writer, ", ", 2);
         rjsonwriter_add_string(writer, sync_state->held->elems[i].data);
      }
      rjsonwriter_raw(writer, "]\n}\n", 4);
      buf = rjsonwriter_get_memory_buffer(writer, &len);
   }
   if (!buf || !filestream_write_file_atomic(path, buf, len))
   {
      RARCH_ERR(CSPFX "Failed to write \"%s\".\n", path);
      sync_state->failures = true;
   }
   if (writer)
      rjsonwriter_free(writer);
}

static void task_cloud_sync_read_local_manifest(task_cloud_sync_state_t *sync_state)
{
   char manifest_path[PATH_MAX_LENGTH];
   char server[33];
   bool unreadable = false;

   task_cloud_sync_manifest_filename(manifest_path, sizeof(manifest_path), false,
         sync_state->dir_core_assets);

   /* it is valid for there not to be a local manifest, if we have never done a sync before */
   if (path_is_valid(manifest_path))
   {
      RFILE *rfile = filestream_open(manifest_path,
            RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
      if (!rfile)
      {
         RARCH_ERR(CSPFX "Could not open local manifest.\n");
         sync_state->failures    = true;
         sync_state->fatal_error = "Cannot read local manifest";
         task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_END);
         return;
      }
      else
      {
         RARCH_WARN(CSPFX "Opened local manifest.\n");
         sync_state->local_manifest = task_cloud_sync_create_manifest(rfile);
         filestream_close(rfile);
         /* The local manifest is rewritten in place, so an interrupted
          * write can leave it empty or truncated. Treating that as fatal
          * would block every later sync with no way out from the UI.
          * Drop it and continue as a first sync instead: files that
          * differ between local and server become conflicts rather
          * than being overwritten. */
         if (!sync_state->local_manifest)
         {
            RARCH_WARN(CSPFX "Discarding invalid local manifest.\n");
            filestream_delete(manifest_path);
         }
      }
   }

   server[0] = '\0';
   if (!task_cloud_sync_read_roots(sync_state->dir_core_assets,
            &sync_state->roots, server, &unreadable))
   {
      sync_state->failures = true;
      task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_END);
      return;
   }

   /* What the last sync left is another server's, or can't be told:
    * a first sync */
   if (     unreadable
         || (    *server && *sync_state->server_id
             && !string_is_equal(server, sync_state->server_id)))
   {
      if (!unreadable)
         RARCH_WARN(CSPFX "The server changed since the last sync: syncing as a first sync.\n");
      if (sync_state->local_manifest)
         file_list_free(sync_state->local_manifest);
      sync_state->local_manifest = NULL;
      string_list_free(sync_state->roots);
      sync_state->roots          = NULL;
   }

   /* A missing roots file, or a root it does not name, is unchanged */
   if (      sync_state->roots
         &&  sync_state->dirlist
         && !task_cloud_sync_drop_changed_roots(sync_state))
   {
      sync_state->failures = true;
      task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_END);
      return;
   }

   task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_BUILD_CURRENT_MANIFEST);
}

/* takes the filename in manifest format, e.g. "config/retroarch.cfg" */
static bool task_cloud_sync_should_ignore_file(const char *filename)
{
   const char *base;
   size_t      len;

   if (string_starts_with(filename, "config/"))
   {
      const char *path = filename + STRLEN_CONST("config/");

      /* need to exclude FILE_PATH_MAIN_CONFIG, those don't get sync'd */
      if (string_is_equal(path, FILE_PATH_MAIN_CONFIG))
         return true;

      /* ignore playlist files */
      if (string_starts_with(path, "content_") && string_ends_with(path, FILE_PATH_LPL_EXTENSION))
         return true;

      /* this device's recovery files (save_backups.c) */
      if (string_starts_with(path, SAVE_BACKUPS_RECOVERY_DIR "/"))
         return true;

      /* this device's save folders list (save_backups.c) */
      if (string_is_equal(path, SAVE_BACKUPS_FOLDERS_FILE))
         return true;
   }

   /* safety copies stay on the device that made them (save_backups.c) */
   if (     string_starts_with_case_insensitive(filename,
               "saves/" SAVE_BACKUPS_DIR "/")
         || string_starts_with_case_insensitive(filename,
               "states/" SAVE_BACKUPS_DIR "/"))
      return true;

   if (string_ends_with(filename, "/.DS_Store"))
       return true;

   /* other tools' files: folder metadata, Syncthing's old versions and
    * sync tools' conflicted copies */
   base = strrchr(filename, '/');
   base = base ? base + 1 : filename;
   if (     string_starts_with(base, "._")
         || string_is_equal_case_insensitive(base, "Thumbs.db")
         || string_is_equal_case_insensitive(base, "desktop.ini")
         || string_is_equal(base, ".nomedia")
         || strstr(filename, "/.stversions/")
         || strstr(base, "conflicted copy")
         || strstr(base, ".sync-conflict-"))
       return true;

   /* a download still being written, or left by one that never
    * finished (CS_FETCHING_SUFFIX) */
   if (string_ends_with(filename, ".rafetching"))
       return true;

   /* a save, state or disk index still being written, or left by one
    * that never finished (filestream_atomic_temp_name) */
   if (string_ends_with(filename, ".tmp"))
       return true;

   /* the old file an atomic write moved aside, or left by one that
    * never finished (filestream_finish_atomic) */
   if (string_ends_with(filename, ".prev"))
       return true;

   /* the same, left by a rename that moves its destination aside as
    * ".old", or ".old2" and so on, on some platforms
    * (retro_vfs_file_rename_impl) */
   len = strlen(filename);
   while (len && ISDIGIT(filename[len - 1]))
      len--;
   if (string_ends_with_size(filename, ".old", len, STRLEN_CONST(".old")))
       return true;

   return false;
}

/* A directory open in the walk, and the length of its path. */
struct tcs_walk_dir
{
   struct RDIR *dir;
   size_t       len;
};

#define CS_WALK_ROOT(s) ((s)->walk_bufs)
#define CS_WALK_PATH(s) ((s)->walk_bufs + PATH_MAX_LENGTH)
#define CS_WALK_REL(s)  ((s)->walk_bufs + 2 * PATH_MAX_LENGTH)
#define CS_WALK_ALT(s)  ((s)->walk_bufs + 3 * PATH_MAX_LENGTH)

static void task_cloud_sync_walk_free(task_cloud_sync_state_t *sync_state)
{
   while (sync_state->walk_depth)
      retro_closedir(sync_state->walk[--sync_state->walk_depth].dir);
   free(sync_state->walk);
   free(sync_state->walk_bufs);
   sync_state->walk      = NULL;
   sync_state->walk_cap  = 0;
   sync_state->walk_bufs = NULL;
}

/* Opens CS_WALK_PATH(sync_state), @len long, as the next level down. */
static bool task_cloud_sync_walk_push(task_cloud_sync_state_t *sync_state,
      size_t len)
{
   struct RDIR *dir;
   if (sync_state->walk_depth == sync_state->walk_cap)
   {
      size_t cap = sync_state->walk_cap ? sync_state->walk_cap * 2 : 8;
      struct tcs_walk_dir *walk = (struct tcs_walk_dir*)realloc(
            sync_state->walk, cap * sizeof(*walk));
      if (!walk)
         return false;
      sync_state->walk     = walk;
      sync_state->walk_cap = cap;
   }
   if (!(dir = retro_opendir_include_hidden(CS_WALK_PATH(sync_state), true)))
      return false;
   if (retro_dirent_error(dir))
   {
      retro_closedir(dir);
      return false;
   }
   sync_state->walk[sync_state->walk_depth].dir = dir;
   sync_state->walk[sync_state->walk_depth].len = len;
   sync_state->walk_depth++;
   return true;
}

/* Ends the sync when a synced folder can't be read to its end: its
 * files would look deleted, and the diff would delete the server's
 * copies of them. */
static void task_cloud_sync_walk_abort(task_cloud_sync_state_t *sync_state,
      const char *path)
{
   RARCH_ERR(CSPFX "Could not read \"%s\".\n", path);
   RARCH_ERR(CSPFX "Not syncing, the current state of the disk could not be established.\n");
   task_cloud_sync_walk_free(sync_state);
   sync_state->failures = true;
   task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_END);
}

/* One entry of the walk: the next name in the deepest open directory,
 * which adds a file to current_manifest or opens a subdirectory, or
 * the end of that directory. Lists what dir_list_new() lists for the
 * root, recursively, hidden entries included and directories left
 * out. False when a subdirectory can't be opened or read, which ends
 * the sync. */
static bool task_cloud_sync_walk_step(task_cloud_sync_state_t *sync_state)
{
   struct tcs_walk_dir *top  = &sync_state->walk[sync_state->walk_depth - 1];
   char                *path = CS_WALK_PATH(sync_state);
   const char          *name;
   size_t               len;
   size_t               idx;
   bool                 is_dir;

   if (!retro_readdir(top->dir))
   {
      if (retro_dirent_error(top->dir))
      {
         path[top->len] = '\0';
         task_cloud_sync_walk_abort(sync_state, path);
         return false;
      }
      retro_closedir(top->dir);
      sync_state->walk_depth--;
      return true;
   }
   name = retro_dirent_get_name(top->dir);
   if (     (name[0] == '.' || name[0] == '$')
         && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
      return true;

   len = top->len;
   if (len && path[len - 1] != '/' && path[len - 1] != '\\')
      path[len++] = '/';
   len += strlcpy(path + len, name, PATH_MAX_LENGTH - len);
   if (len >= PATH_MAX_LENGTH)
      return true;

   is_dir = retro_dirent_is_dir(top->dir, NULL);
#if TARGET_OS_IPHONE || TARGET_OS_OSX
   /* listed as a file, as dir_list_new() lists it */
   if (is_dir && len >= 10 && !memcmp(path + len - 10, ".framework", 10))
      is_dir = false;
#endif
   if (is_dir)
   {
      /* a folder the sync ignores is not walked */
      if (sync_state->walk_depth == 1)
      {
         snprintf(CS_WALK_ALT(sync_state), PATH_MAX_LENGTH, "%s/%s/",
               sync_state->dirlist->elems[sync_state->walk_root_idx].data,
               name);
         if (task_cloud_sync_should_ignore_file(CS_WALK_ALT(sync_state)))
            return true;
      }
      if (!task_cloud_sync_walk_push(sync_state, len))
      {
         task_cloud_sync_walk_abort(sync_state, path);
         return false;
      }
      return true;
   }

   path_relative_to(CS_WALK_REL(sync_state), path, CS_WALK_ROOT(sync_state),
         PATH_MAX_LENGTH);
   fill_pathname_join_special(CS_WALK_ALT(sync_state),
         sync_state->dirlist->elems[sync_state->walk_root_idx].data,
         CS_WALK_REL(sync_state), PATH_MAX_LENGTH);
   if (task_cloud_sync_should_ignore_file(CS_WALK_ALT(sync_state)))
      return true;

   /* The "alt" refers to the relative path of whatever we're syncing relative to the retroarch folder
    * whereas the full_path is the absolute disk path of the file. When building the manifest, adhere
    * to a portable standard, but use that as the portable representation of paths. While the actual
    * "manifest" is comprised of full, local-style paths associated with "alt"s which are portable. */
   pathname_make_slashes_portable(CS_WALK_ALT(sync_state));
   idx = sync_state->current_manifest->size;
   file_list_append(sync_state->current_manifest, path, NULL, 0, 0, 0);
   file_list_set_alt_at_offset(sync_state->current_manifest, idx,
         CS_WALK_ALT(sync_state));
   return true;
}

/**
 * task_cloud_sync_directory_map_new:
 *
 * Builds the string_list of folders this sync covers, on the main
 * thread when the task is pushed; the handler reads it from the task's
 * state. The caller owns the list.
 */
/* One entry of the map, transactionally: the path copy is made
 * first, the append second, and the copy is attached only once both
 * exist, so a failed append never indexes size - 1 of a list it did
 * not grow and a failed copy never leaves an entry whose userdata -
 * the directory the manifest walks - is NULL. */
static bool task_cloud_sync_directory_map_add(struct string_list *list,
      const char *name, size_t name_len, const char *dir)
{
   union string_list_elem_attr attr = {0};
   char *path;
   /* an unset folder is left out: not walked, its server keys kept */
   if (string_is_empty(dir))
      return true;
   if (!(path = strdup(dir)))
      return false;
   if (!string_list_append_n(list, name, name_len, attr))
   {
      free(path);
      return false;
   }
   list->elems[list->size - 1].userdata = path;
   return true;
}

static struct string_list *task_cloud_sync_directory_map_new(settings_t *settings)
{
   char  dir[DIR_MAX_LENGTH];
   struct string_list *list = string_list_new();

   if (!list)
      return NULL;

   if (settings->bools.cloud_sync_sync_configs)
   {
      fill_pathname_application_special(dir,
            sizeof(dir), APPLICATION_SPECIAL_DIRECTORY_CONFIG);
      if (!task_cloud_sync_directory_map_add(list,
               "config", STRLEN_CONST("config"), dir))
         goto error;
   }

   if (settings->bools.cloud_sync_sync_saves)
   {
      /* the main config's, not an override's or the command line's */
      if (!task_cloud_sync_directory_map_add(list,
               "saves", STRLEN_CONST("saves"),
               dir_get_ptr(RARCH_DIR_SAVEFILE_BASE)))
         goto error;
      if (!task_cloud_sync_directory_map_add(list,
               "states", STRLEN_CONST("states"),
               dir_get_ptr(RARCH_DIR_SAVESTATE_BASE)))
         goto error;
   }

   if (settings->bools.cloud_sync_sync_thumbs)
   {
      if (!task_cloud_sync_directory_map_add(list,
               "thumbnails", STRLEN_CONST("thumbnails"),
               settings->paths.directory_thumbnails))
         goto error;
   }

   if (settings->bools.cloud_sync_sync_system)
   {
      if (!task_cloud_sync_directory_map_add(list,
               "system", STRLEN_CONST("system"),
               settings->paths.directory_system))
         goto error;
   }

   return list;

error:
   /* string_list_free releases every attached userdata. */
   string_list_free(list);
   return NULL;
}

/* True when @key lies under one of the directories this sync covers. */
static bool task_cloud_sync_key_in_scope(const struct string_list *dirlist,
      const char *key)
{
   size_t i;
   for (i = 0; i < dirlist->size; i++)
      if (string_starts_with(key, dirlist->elems[i].data))
         return true;
   return false;
}

static size_t task_cloud_sync_count_out_of_scope(
      const task_cloud_sync_state_t *sync_state)
{
   const struct string_list *dirlist = sync_state->dirlist;
   const file_list_t        *server  = sync_state->server_manifest;
   size_t                    count   = 0;
   size_t                    i;

   if (!dirlist || !server)
      return 0;

   for (i = 0; i < server->size; i++)
      if (!task_cloud_sync_key_in_scope(dirlist,
               CS_FILE_KEY(&server->list[i])))
         count++;

   return count;
}

/* Files missing here that the server still has, changed there or not,
 * more than CS_HOLD_MIN of them and more than half of a root's: more
 * likely gone missing (an unmounted card, an emptied folder) than
 * deleted, so the diff deletes none of them on the server. The lists
 * are in file_list_sort_on_alt() order. */
static void task_cloud_sync_mark_held_roots(task_cloud_sync_state_t *sync_state)
{
   struct string_list *dirlist = sync_state->dirlist;
   file_list_t        *local   = sync_state->local_manifest;
   file_list_t        *current = sync_state->current_manifest;
   file_list_t        *server  = sync_state->server_manifest;
   size_t              r;

   if (!local || !server)
      return;

   for (r = 0; r < dirlist->size; r++)
   {
      size_t total   = 0;
      size_t missing = 0;
      size_t c       = 0;
      size_t s       = 0;
      size_t i;

      for (i = 0; i < local->size; i++)
      {
         struct item_file *local_file = &local->list[i];
         const char       *key        = CS_FILE_KEY(local_file);
         int               cmp        = 1;

         if (     string_is_empty(CS_FILE_HASH(local_file))
               || task_cloud_sync_key_root(dirlist, key) != (int)r)
            continue;
         total++;
         while (c < current->size
               && (cmp = strcasecmp(current->list[c].alt, key)) < 0)
            c++;
         if (c < current->size && !cmp)
            continue;
         cmp = 1;
         while (s < server->size
               && (cmp = strcasecmp(server->list[s].alt, key)) < 0)
            s++;
         if (     s < server->size && !cmp
               && !string_is_empty(CS_FILE_HASH(&server->list[s])))
            missing++;
      }

      if (missing > CS_HOLD_MIN && missing * 2 > total)
      {
         RARCH_WARN(CSPFX "%u of %u files under \"%s\" are missing here, not deleting them on the server.\n",
               (unsigned)missing, (unsigned)total, dirlist->elems[r].data);
         dirlist->elems[r].attr.i |= CS_ROOT_HELD;
      }
   }
}

/* A folder open in task_cloud_sync_mark_held_dirs(): its key prefix
 * (the first @len of @key) and its synced files. */
struct tcs_hold_dir
{
   const char *key;
   size_t      len;
   unsigned total;
   unsigned missing;
};

static void task_cloud_sync_hold_dir(task_cloud_sync_state_t *sync_state,
      const struct tcs_hold_dir *d)
{
   union string_list_elem_attr attr;
   int root;
   attr.i = 0;
   RARCH_WARN(CSPFX "All %u files under \"%.*s\" are missing here, not deleting them on the server.\n",
         d->total, (int)d->len, d->key);
   if (!sync_state->held_dirs)
      sync_state->held_dirs = string_list_new();
   if (     sync_state->held_dirs
         && string_list_append_n(sync_state->held_dirs, d->key, d->len, attr))
      return;
   /* out of memory: hold its root instead */
   if ((root = task_cloud_sync_key_root(sync_state->dirlist, d->key)) >= 0)
      sync_state->dirlist->elems[root].attr.i |= CS_ROOT_HELD;
}

/* Every synced file of a root or folder missing here that the server
 * still has, CS_HOLD_DIR_MIN or more of them: a folder more likely
 * lost than emptied, so the diff deletes none of them on the server.
 * The lists are in file_list_sort_on_alt() order, so a folder's keys
 * are together. */
static void task_cloud_sync_mark_held_dirs(task_cloud_sync_state_t *sync_state)
{
   struct tcs_hold_dir dirs[CS_HOLD_DEPTH];
   file_list_t        *local   = sync_state->local_manifest;
   file_list_t        *current = sync_state->current_manifest;
   file_list_t        *server  = sync_state->server_manifest;
   size_t              depth   = 0;
   size_t              c       = 0;
   size_t              s       = 0;
   size_t              i;

   if (!local || !server)
      return;

   for (i = 0; i <= local->size; i++)
   {
      const char *key   = i < local->size ? CS_FILE_KEY(&local->list[i]) : "";
      const char *slash;
      bool        missing;
      int         cmp;
      size_t      d;

      /* the folders this key is not under are complete */
      while (depth && strncasecmp(key, dirs[depth - 1].key,
               dirs[depth - 1].len))
      {
         depth--;
         if (     dirs[depth].total >= CS_HOLD_DIR_MIN
               && dirs[depth].missing == dirs[depth].total)
            task_cloud_sync_hold_dir(sync_state, &dirs[depth]);
      }
      if (     i == local->size
            || string_is_empty(CS_FILE_HASH(&local->list[i]))
            || task_cloud_sync_key_root(sync_state->dirlist, key) < 0)
         continue;

      cmp = 1;
      while (c < current->size
            && (cmp = strcasecmp(current->list[c].alt, key)) < 0)
         c++;
      missing = !(c < current->size && !cmp);
      if (missing)
      {
         cmp = 1;
         while (s < server->size
               && (cmp = strcasecmp(server->list[s].alt, key)) < 0)
            s++;
         missing = s < server->size && !cmp
               && !string_is_empty(CS_FILE_HASH(&server->list[s]));
      }

      for (slash = strchr(key + (depth ? dirs[depth - 1].len : 0), '/');
            slash && depth < CS_HOLD_DEPTH; slash = strchr(slash + 1, '/'))
      {
         dirs[depth].key     = key;
         dirs[depth].len     = (size_t)(slash + 1 - key);
         dirs[depth].total   = 0;
         dirs[depth].missing = 0;
         depth++;
      }
      for (d = 0; d < depth; d++)
      {
         dirs[d].total++;
         if (missing)
            dirs[d].missing++;
      }
   }
}

/* Whether a file missing here under @root is held on the server */
static bool task_cloud_sync_key_held(
      const task_cloud_sync_state_t *sync_state, int root, const char *key)
{
   const struct string_list *dirs = sync_state->held_dirs;
   size_t                    i;
   if (root < 0)
      return false;
   if (sync_state->dirlist->elems[root].attr.i & CS_ROOT_HELD)
      return true;
   for (i = 0; dirs && i < dirs->size; i++)
      if (!strncasecmp(key, dirs->elems[i].data, strlen(dirs->elems[i].data)))
         return true;
   return false;
}

/**
 * task_cloud_sync_build_current_manifest:
 * @sync_state       : pointer to the current sync state
 *
 * Create an in-memory manifest of actual, current disk data
 */
/* Builds current_manifest from the synced directories, as many entries
 * per run as @b allows and at least one, so a large directory does not
 * hold up one run of the handler - the frame loop, with Threaded Tasks
 * off. */
static void task_cloud_sync_build_current_manifest(
      task_cloud_sync_state_t *sync_state, nbio_budget_t *b)
{
   struct string_list *dirlist = sync_state->dirlist;

   if (!dirlist)
   {
      task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_END);
      return;
   }

   if (!sync_state->current_manifest)
   {
      if (     !(sync_state->current_manifest        = (file_list_t *)calloc(1, sizeof(file_list_t)))
            || !(sync_state->updated_server_manifest = (file_list_t *)calloc(1, sizeof(file_list_t)))
            || !(sync_state->updated_local_manifest  = (file_list_t *)calloc(1, sizeof(file_list_t)))
            || !(sync_state->walk_bufs               = (char*)malloc(4 * PATH_MAX_LENGTH)))
      {
         task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_END);
         return;
      }
      sync_state->walk_root_idx = 0;
   }

   while (task_nbio_slice_within_budget(b, 0, 0))
   {
      if (sync_state->walk_depth)
      {
         if (!task_cloud_sync_walk_step(sync_state))
            return;
         if (!sync_state->walk_depth)
            sync_state->walk_root_idx++;
         continue;
      }
      if (sync_state->walk_root_idx >= dirlist->size)
         break;

      /* The userdata of the elements is actually the full path to the directory, while data is the name of the folder itself */
      /* The paths iterated here are not portable, because they are still used for iterating later on */
      strlcpy(CS_WALK_ROOT(sync_state),
            (const char*)dirlist->elems[sync_state->walk_root_idx].userdata,
            PATH_MAX_LENGTH);
      fill_pathname_slash(CS_WALK_ROOT(sync_state), PATH_MAX_LENGTH);
      strlcpy(CS_WALK_PATH(sync_state), CS_WALK_ROOT(sync_state), PATH_MAX_LENGTH);
      /* A root that cannot be opened is not the same as an empty
       * directory: an empty one says every file under it is gone, which
       * is a statement the diff acts on by deleting the server's copies.
       * A directory that could not be read says nothing at all, and half
       * a picture of local state is worse than none, so the sync stops
       * rather than letting the manifest claim a deletion that never
       * happened. */
      if (!task_cloud_sync_walk_push(sync_state, strlen(CS_WALK_PATH(sync_state))))
      {
         task_cloud_sync_walk_abort(sync_state, CS_WALK_ROOT(sync_state));
         return;
      }
   }
   if (sync_state->walk_depth || sync_state->walk_root_idx < dirlist->size)
      return;

   task_cloud_sync_walk_free(sync_state);
   file_list_sort_on_alt(sync_state->current_manifest);
   sync_state->server_out_of_scope =
      task_cloud_sync_count_out_of_scope(sync_state);
   task_cloud_sync_mark_held_roots(sync_state);
   task_cloud_sync_mark_held_dirs(sync_state);
   task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_DIFF);
   RARCH_LOG(CSPFX "Created in-memory manifest of current disk state with %d files.\n", sync_state->current_manifest->size);
}

/**
 * task_cloud_sync_update_progress:
 * @task       : pointer to the retro_task executing us
 *
 * Updates the percentage of the current task's progress based on
 * current sync progress. It results in a percentage which is
 * computed based on the total number of files to deal with and
 * the current cumulative count that's been dealt with across
 * the three types of manifests (server, old local, current local)
 */
static void task_cloud_sync_update_progress(retro_task_t *task)
{
   task_cloud_sync_state_t *sync_state = NULL;
   unsigned long            val   = 0;
   unsigned long            count = 0;

   if (!task)
      return;

   if (!(sync_state = (task_cloud_sync_state_t *)task->state))
      return;

   /* Entries of a directory whose sync toggle is off are stepped over
    * in one go, so counting them would leave the bar short of 100 when
    * they sort last - and stuck below the in-scope share otherwise. */
   val = (unsigned long)((sync_state->server_idx - sync_state->server_skipped)
       + sync_state->current_idx);

   if (sync_state->server_manifest)
      count += sync_state->server_manifest->size
             - sync_state->server_out_of_scope;
   if (sync_state->current_manifest)
      count += sync_state->current_manifest->size;

   if (count != 0)
	   task_set_progress(task, (val * 100) / count);
   else
	   task_set_progress(task, 100);
}

#if defined(TCS_LOCK_FREE)
static void task_cloud_sync_add_to_updated_manifest(task_cloud_sync_state_t *sync_state, const char *key, const char *hash, bool server)
{
   void *head;
   tcs_manifest_add_t *add = (tcs_manifest_add_t*)malloc(sizeof(*add));

   if (add)
   {
      add->key  = key  ? strdup(key)  : NULL;
      add->hash = hash ? strdup(hash) : NULL;
      if ((key && !add->key) || (hash && !add->hash))
      {
         free(add->key);
         free(add->hash);
         free(add);
         add = NULL;
      }
   }
   if (!add)
   {
      /* The entry is lost: the manifests no longer say what was done */
      sync_state->failures = true;
      return;
   }
   add->server = server;
   do
   {
      head      = retro_atomic_load_acquire_ptr(&sync_state->manifest_adds);
      add->next = (tcs_manifest_add_t*)head;
   } while (!retro_atomic_cas_ptr(&sync_state->manifest_adds, head, add));
}

/* Fold the entries added so far into the manifests.  The task thread,
 * with `waiting` at zero, or the last holder of the state. */
static void task_cloud_sync_fold_manifest_adds(task_cloud_sync_state_t *sync_state)
{
   tcs_manifest_add_t *add = (tcs_manifest_add_t*)
      retro_atomic_exchange_ptr(&sync_state->manifest_adds, NULL);

   while (add)
   {
      tcs_manifest_add_t *next = add->next;
      file_list_t *list        = add->server
         ? sync_state->updated_server_manifest
         : sync_state->updated_local_manifest;

      if (list && file_list_append(list, NULL, NULL, 0, 0, 0))
      {
         size_t idx = list->size - 1;
         file_list_set_alt_at_offset(list, idx, add->key);
         list->list[idx].userdata = add->hash;
      }
      else
         sync_state->failures = true;
      free(add->key);
      free(add);
      add = next;
   }
}
#else
static void task_cloud_sync_add_to_updated_manifest(task_cloud_sync_state_t *sync_state, const char *key, const char *hash, bool server)
{
   file_list_t *list;
   size_t       idx;
#ifdef HAVE_THREADS
   slock_lock(tcs_manifest_lock);
#endif
   list = server ? sync_state->updated_server_manifest : sync_state->updated_local_manifest;
   idx = list->size;
   file_list_append(list, NULL, NULL, 0, 0, 0);
   file_list_set_alt_at_offset(list, idx, key);
   list->list[idx].userdata = hash ? strdup(hash) : NULL;
#ifdef HAVE_THREADS
   slock_unlock(tcs_manifest_lock);
#endif
}

#define task_cloud_sync_fold_manifest_adds(sync_state) ((void)0)
#endif

static INLINE int task_cloud_sync_key_cmp(struct item_file *left, struct item_file *right)
{
   char *left_key  = CS_FILE_KEY(left);
   char *right_key = CS_FILE_KEY(right);

   if (!left_key && !right_key)
      return 0;
   else if (!left_key)
      return 1;
   else if (!right_key)
      return -1;
   else
      return strcasecmp(left_key, right_key);
}

/* @md5's digest as 32 hex digits into @hash (33 bytes). */
static void task_cloud_sync_md5_hex(char *hash, MD5_CTX *md5)
{
   static const char hex[] = "0123456789abcdef";
   unsigned char     digest[16];
   unsigned          i;
   MD5_Final(digest, md5);
   for (i = 0; i < 16; i++)
   {
      hash[i * 2]     = hex[digest[i] >> 4];
      hash[i * 2 + 1] = hex[digest[i] & 15];
   }
   hash[32] = '\0';
}

/* Octets per chunk of a file's hash. */
#define CS_HASH_CHUNK (256 * 1024)

static char *task_cloud_sync_md5_rfile(RFILE *file)
{
   MD5_CTX        md5;
   unsigned char *buf  = NULL;
   char          *hash = (char*)malloc(33);

   if (!hash)
      return NULL;

   MD5_Init(&md5);

   /* Hash the whole file, from the start, whichever path is taken.
    * The mapped branch below always covered the entire file while the
    * read branch started wherever the stream happened to be, and one
    * caller hands this a stream it got from a fetch callback rather
    * than one it just opened - so on a platform with mappings and a
    * non-zero position the two branches hashed different bytes and
    * produced different manifest entries for the same content.  A
    * partial hash is not a useful answer to "what is this file", so
    * the read path is squared up with the mapped one rather than the
    * other way round. */
   filestream_seek(file, 0, RETRO_VFS_SEEK_POSITION_START);

   /* Hashed where it lies when the stream can lend it - the files are
    * opened with HINT_FREQUENT_ACCESS, so where the platform maps them
    * nothing is copied - and read into a buffer made on first need
    * otherwise. 256 KB at a time either way, same reasoning as
    * intfstream_get_crc: at 4 KB a call it is call overhead, not
    * hashing. */
   for (;;)
   {
      const uint8_t *ptr = NULL;
      int64_t        n   = filestream_view(file, CS_HASH_CHUNK, &ptr);

      if (n < 0)
      {
         if (!buf && !(buf = (unsigned char*)malloc(CS_HASH_CHUNK)))
         {
            free(hash);
            return NULL;
         }
         if ((n = filestream_read(file, buf, CS_HASH_CHUNK)) < 0)
         {
            free(buf);
            free(hash);
            return NULL;
         }
         ptr = buf;
      }
      if (!n)
         break;
      MD5_Update(&md5, ptr, (unsigned long)n);
   }
   free(buf);

   task_cloud_sync_md5_hex(hash, &md5);
   return hash;
}

/* Starts @h on @file, from its start. */
static void task_cloud_sync_hash_begin(tcs_hash_t *h, RFILE *file)
{
   MD5_Init(&h->md5);
   filestream_seek(file, 0, RETRO_VFS_SEEK_POSITION_START);
   h->file = file;
}

/* Hashes @h's file a chunk at a time while @b allows. True once it is
 * done, with the digest in @hash (33 bytes), or an empty @hash when a
 * read failed; the stream is left to its owner. False when the budget
 * ran out first: the next call carries on. Reads and hashes the same
 * bytes as task_cloud_sync_md5_rfile(). */
static bool task_cloud_sync_hash_step(tcs_hash_t *h, uint8_t *buf,
      nbio_budget_t *b, char *hash)
{
   for (;;)
   {
      const uint8_t *ptr = NULL;
      int64_t        n;
      if (!task_nbio_slice_within_budget(b, 0, 0))
         return false;
      if ((n = filestream_view(h->file, CS_HASH_CHUNK, &ptr)) < 0)
      {
         n   = filestream_read(h->file, buf, CS_HASH_CHUNK);
         ptr = buf;
      }
      if (n < 0)
      {
         hash[0] = '\0';
         h->file = NULL;
         return true;
      }
      if (!n)
         break;
      MD5_Update(&h->md5, ptr, (unsigned long)n);
   }
   task_cloud_sync_md5_hex(hash, &h->md5);
   h->file = NULL;
   return true;
}

/* The diff's current file hashed inside @b, so a large one is spread
 * over several runs of the handler instead of holding up one - the
 * frame loop's, with Threaded Tasks off. True once the diff step can
 * run: the hash is in ->userdata, or there is no current file, or it
 * could not be opened or read and the step that needs it will find that
 * out itself. False when the budget ran out first. */
static bool task_cloud_sync_hash_current(task_cloud_sync_state_t *sync_state,
      nbio_budget_t *b)
{
   struct item_file *item;
   RFILE            *file;
   char              hash[33];

   if (     !sync_state->current_manifest
         || sync_state->current_idx >= sync_state->current_manifest->size)
      return true;
   item = &sync_state->current_manifest->list[sync_state->current_idx];
   if (item->userdata)
      return true;

   if (!(file = sync_state->hash_cur.file))
   {
      if (     !sync_state->hash_buf
            && !(sync_state->hash_buf = (uint8_t*)malloc(CS_HASH_CHUNK)))
         return true;
      if (!(file = filestream_open(item->path, RETRO_VFS_FILE_ACCESS_READ,
                  RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS)))
         return true;
      task_cloud_sync_hash_begin(&sync_state->hash_cur, file);
   }
   if (!task_cloud_sync_hash_step(&sync_state->hash_cur,
            sync_state->hash_buf, b, hash))
      return false;
   filestream_close(file);
   if (*hash)
      item->userdata = strdup(hash);
   return true;
}

/* Where this sync's backups go: under the synced save folder's
 * backups/cloud-sync, which no sync walks; without one (false), under
 * <core assets>/cloud_backups. */
static bool task_cloud_sync_backup_root(task_cloud_sync_state_t *sync_state,
      char *s, size_t len)
{
   size_t      i;
   const char *saves = NULL;

   for (i = 0; sync_state->dirlist && i < sync_state->dirlist->size; i++)
      if (string_is_equal(sync_state->dirlist->elems[i].data, "saves"))
         saves = (const char*)sync_state->dirlist->elems[i].userdata;

   if (string_is_empty(saves))
   {
      fill_pathname_join_special(s, sync_state->dir_core_assets,
            "cloud_backups", len);
      return false;
   }
   fill_pathname_join_special(s, saves,
         SAVE_BACKUPS_DIR "/" SAVE_BACKUPS_CORE_CLOUD_SYNC, len);
   return true;
}

/* don't pass a server/local item_file to this, only current has ->path set */
/* Anything but MADE leaves the caller holding a file it must not
 * replace or remove: non-destructive mode promises a local copy of
 * anything it displaces, and without the backup there is nothing to
 * fall back on. With a move worker, COPY_OWED: the file is still in
 * place and its copy is owed at *@owed (the caller frees it). */
static enum save_backup_result task_cloud_sync_backup_file(
      task_cloud_sync_state_t *sync_state, struct item_file *file,
      enum save_backup_reason reason, char **owed)
{
   enum save_backup_result result = SAVE_BACKUP_FAILED;
   /* one heap block for the two paths: the task runs on small stacks */
   char *new_dir  = (char*)malloc(DIR_MAX_LENGTH + PATH_MAX_LENGTH);
   char *new_path = new_dir + DIR_MAX_LENGTH;

   *owed = NULL;
   if (!new_dir)
      return SAVE_BACKUP_FAILED;

   task_cloud_sync_backup_root(sync_state, new_dir, DIR_MAX_LENGTH);
   fill_pathname_join_special(new_path, new_dir, CS_FILE_KEY(file),
         PATH_MAX_LENGTH);
   pathname_conform_slashes_to_os(new_path);
   fill_pathname_basedir(new_dir, new_path, DIR_MAX_LENGTH);

#if defined(TCS_MOVE_WORKER)
   result = save_backups_move_begin(new_dir, file->path,
         SAVE_BACKUPS_CORE_CLOUD_SYNC, reason, new_path, PATH_MAX_LENGTH);
   if (     result == SAVE_BACKUP_COPY_OWED
         && !(*owed = strdup(new_path)))
      result = SAVE_BACKUP_FAILED;
#else
   result = save_backups_move_to(new_dir, file->path,
         SAVE_BACKUPS_CORE_CLOUD_SYNC, reason);
#endif
   if (result != SAVE_BACKUP_MADE && result != SAVE_BACKUP_COPY_OWED)
      RARCH_ERR(CSPFX "Could not back \"%s\" up to \"%s\".\n",
            file->path, new_dir);

   free(new_dir);
   return result;
}

/**
 * task_cloud_sync_carry_manifests_forward:
 * @sync_state       : sync state
 * @key              : portable manifest key for the file
 * @server_hash      : hash the server manifest holds for @key
 *
 * Records what both manifests already hold for @key, for the cases where
 * this sync leaves the local file exactly as it found it. The server side
 * keeps the hash the server reported and the local side keeps the hash the
 * last sync recorded, so the next sync sees the same difference between the
 * two and acts on it rather than treating the skipped work as done.
 **/
static void task_cloud_sync_carry_manifests_forward(
      task_cloud_sync_state_t *sync_state,
      const char *key,
      const char *server_hash)
{
   size_t idx;

   task_cloud_sync_add_to_updated_manifest(sync_state, key, server_hash, true);

   if (     file_list_search(sync_state->local_manifest, key, &idx)
         && string_is_equal_noncase(
            CS_FILE_KEY(&sync_state->local_manifest->list[idx]), key))
      task_cloud_sync_add_to_updated_manifest(sync_state, key,
            CS_FILE_HASH(&sync_state->local_manifest->list[idx]), false);
}

/* The local manifest keeps what the last sync left in it for @key.
 * file_list_search() also matches a longer key sharing @key's start. */
static void task_cloud_sync_keep_local_record(
      task_cloud_sync_state_t *sync_state, const char *key)
{
   size_t idx;

   if (     file_list_search(sync_state->local_manifest, key, &idx)
         && string_is_equal_noncase(
            CS_FILE_KEY(&sync_state->local_manifest->list[idx]), key))
      task_cloud_sync_add_to_updated_manifest(sync_state, key,
            CS_FILE_HASH(&sync_state->local_manifest->list[idx]), false);
}

/* Work on @key that did not happen: both manifests keep what the last
 * sync left in them, so the next sync meets the same difference. */
static void task_cloud_sync_keep_records(task_cloud_sync_state_t *sync_state,
      const char *key)
{
   size_t idx;

   task_cloud_sync_keep_local_record(sync_state, key);
   if (     file_list_search(sync_state->server_manifest, key, &idx)
         && string_is_equal_noncase(
            CS_FILE_KEY(&sync_state->server_manifest->list[idx]), key))
      task_cloud_sync_add_to_updated_manifest(sync_state, key,
            CS_FILE_HASH(&sync_state->server_manifest->list[idx]), true);
}

/**
 * task_cloud_sync_defer_live_savefile:
 * @sync_state       : sync state
 * @key              : portable manifest key for the file
 * @local_path       : absolute path the operation would write or remove
 * @server_hash      : hash the server manifest holds for @key
 *
 * A loaded core holds its save RAM in memory and writes it back over
 * @local_path when the content closes, so a file this sync puts there in
 * the meantime is overwritten from that memory and then uploaded in place
 * of the server's copy. Where @local_path belongs to the running core,
 * leave the file alone. The next sync runs once the core has written its
 * save RAM out, sees the real difference and resolves it, raising a
 * conflict where both sides moved.
 *
 * Returns: true when the operation was deferred.
 **/
static bool task_cloud_sync_defer_live_savefile(
      task_cloud_sync_state_t *sync_state,
      const char *key,
      const char *local_path,
      char *server_hash)
{
   if (!content_savefile_is_live(local_path))
      return false;

   RARCH_LOG(CSPFX "Deferring \"%s\", the running core owns it.\n", key);
   task_cloud_sync_carry_manifests_forward(sync_state, key, server_hash);

   return true;
}

typedef struct
{
   task_cloud_sync_state_t *sync_state;
   struct item_file        *server_file;
   /* Where the download goes once it is complete; the driver writes it
    * beside that, under CS_FETCHING_SUFFIX. */
   char                     local[PATH_MAX_LENGTH];
   /* the last sync left the file here */
   bool                     had_file;
} task_cloud_sync_fetch_state_t;

/* A download is written here first and moved into place only once it
 * is whole, so a transfer that fails part-way leaves the local file as
 * it was, and a file a core took meanwhile is never replaced. The walk
 * skips names ending in it. */
#define CS_FETCHING_SUFFIX ".rafetching"

/* @local's download, under CS_FETCHING_SUFFIX, into @s. False when the
 * name does not fit. */
static bool task_cloud_sync_fetching_path(char *s, size_t len,
      const char *local)
{
   size_t _len = strlcpy(s, local, len);
   return    _len < len
          && strlcpy(s + _len, CS_FETCHING_SUFFIX, len - _len)
               < len - _len;
}

/* What a fetch that never completed left beside its file. Kept while
 * the file the last sync left here is gone: it may be in the backups
 * already, and the leftover keeps the next sync from taking it for
 * deleted here. */
static void task_cloud_sync_fetch_discard(
      task_cloud_sync_fetch_state_t *fetch_state)
{
   char tmp[PATH_MAX_LENGTH];
   if (     (!fetch_state->had_file || path_is_valid(fetch_state->local))
         && task_cloud_sync_fetching_path(tmp, sizeof(tmp),
               fetch_state->local))
      filestream_delete(tmp);
}

/* Moves @tmp, a whole download, into place. False when it did not
 * land. */
static bool task_cloud_sync_fetch_place(
      task_cloud_sync_fetch_state_t *fetch_state, const char *key,
      const char *tmp)
{
   task_cloud_sync_state_t *sync_state = fetch_state->sync_state;
   if (filestream_rename(tmp, fetch_state->local) != 0)
   {
      RARCH_WARN(CSPFX "Could not move the download of \"%s\" into place.\n", key);
      task_cloud_sync_fetch_discard(fetch_state);
      task_cloud_sync_add_to_updated_manifest(sync_state, key,
            CS_FILE_HASH(fetch_state->server_file), true);
      sync_state->failures = true;
      return false;
   }
   /* the state limit's check across a save (command.c) */
   if (string_starts_with_case_insensitive(key, "states/"))
      content_state_fetched();
   return true;
}

enum tcs_commit
{
   TCS_COMMIT_LEFT = 0,
   TCS_COMMIT_PLACED,
   /* waits for its backup's copy, which then finishes the fetch */
   TCS_COMMIT_OWED
};

#if defined(TCS_MOVE_WORKER)
static bool task_cloud_sync_move_push(task_cloud_sync_state_t *sync_state,
      task_cloud_sync_fetch_state_t *fetch_state,
      struct item_file *server_file, const char *key, const char *path,
      const char *to, const char *fetching, const char *hash);
#endif

/* Moves a whole download, of hash @hash, into place. A core that has
 * taken the file since the fetch began keeps it, and the download goes,
 * as at the start of the fetch. LEFT when the download did not land. */
static enum tcs_commit task_cloud_sync_fetch_commit(
      task_cloud_sync_fetch_state_t *fetch_state, const char *key,
      const char *hash)
{
   char tmp[PATH_MAX_LENGTH];
   task_cloud_sync_state_t *sync_state = fetch_state->sync_state;
   const char *server_hash = CS_FILE_HASH(fetch_state->server_file);

   if (!task_cloud_sync_fetching_path(tmp, sizeof(tmp), fetch_state->local))
      return TCS_COMMIT_LEFT;
   if (content_savefile_is_live(fetch_state->local))
   {
      RARCH_LOG(CSPFX "Deferring \"%s\", the running core took it during the fetch.\n", key);
      task_cloud_sync_fetch_discard(fetch_state);
      task_cloud_sync_carry_manifests_forward(sync_state, key, server_hash);
      return TCS_COMMIT_LEFT;
   }
   /* A body the server manifest does not list, or an empty one over a
    * save, leaves the local copy: a conflict, recorded as any other.
    * Keep Server takes the first, never the second. */
   if (path_is_valid(fetch_state->local))
   {
      bool emptied = path_get_size(tmp) == 0
            && path_get_size(fetch_state->local) > 0;
      if (     emptied
            || (   !string_is_equal(hash, server_hash)
                && sync_state->conflict_resolution != 2))
      {
         RARCH_WARN(CSPFX "Not replacing \"%s\": the download is %s.\n",
               key, emptied ? "empty" : "not what the server manifest lists");
         filestream_delete(tmp);
         task_cloud_sync_add_to_updated_manifest(sync_state, key,
               server_hash, true);
         sync_state->conflicts = true;
         return TCS_COMMIT_LEFT;
      }
   }
   if (!sync_state->destructive && path_is_valid(fetch_state->local))
   {
      struct item_file local_file = {0};
      enum save_backup_result made;
      char *owed      = NULL;
      local_file.alt  = (char*)key;
      local_file.path = fetch_state->local;
      made = task_cloud_sync_backup_file(sync_state, &local_file,
            SAVE_BACKUP_REPLACED, &owed);
#if defined(TCS_MOVE_WORKER)
      if (made == SAVE_BACKUP_COPY_OWED)
      {
         bool queued = task_cloud_sync_move_push(sync_state, fetch_state,
               fetch_state->server_file, key, fetch_state->local, owed,
               tmp, hash);
         free(owed);
         if (queued)
            return TCS_COMMIT_OWED;
      }
#else
      (void)hash;
#endif
      if (made != SAVE_BACKUP_MADE)
      {
         filestream_delete(tmp);
         task_cloud_sync_carry_manifests_forward(sync_state, key, server_hash);
         sync_state->failures = true;
         return TCS_COMMIT_LEFT;
      }
   }
   return task_cloud_sync_fetch_place(fetch_state, key, tmp)
      ? TCS_COMMIT_PLACED : TCS_COMMIT_LEFT;
}

/* A download waiting to be hashed by the task thread. */
typedef struct tcs_fetched
{
   struct tcs_fetched            *next;
   task_cloud_sync_fetch_state_t *fetch_state;
   RFILE                         *file;
   char                          *path;  /* in the same block */
} tcs_fetched_t;

/* What a complete download leaves in the manifests. */
static void task_cloud_sync_fetched(task_cloud_sync_state_t *sync_state,
      struct item_file *server_file, const char *path, const char *hash)
{
   RARCH_LOG(CSPFX "Successfully fetched \"%s\".\n", path);
   task_cloud_sync_add_to_updated_manifest(sync_state, path, hash, false);
   task_cloud_sync_add_to_updated_manifest(sync_state, path, hash, true);
   /* if the file fetched from the server does not match its own hash, the manifest needs to be updated */
   if (!string_is_equal(hash, CS_FILE_HASH(server_file)))
      sync_state->need_manifest_uploaded = true;
   sync_state->downloads++;
}

/* A file's name ends in this while the atomic writer writes it. */
#define CS_TEMP_SUFFIX ".tmp"

#if defined(TCS_MOVE_WORKER)
enum
{
   TCS_MOVE_QUEUED = 0,
   TCS_MOVE_RUNNING,
   TCS_MOVE_DONE,
   TCS_MOVE_ABANDONED
};

/* A backup's copy across drives, on a detached thread. The task thread
 * fills it before the thread starts; the thread then reads only path
 * and to and writes only ok, and its exchange of state hands it back.
 * A copy nobody polls for (ABANDONED) is the thread's to dispose of.
 * Everything else is the task thread's. */
typedef struct tcs_move
{
   struct tcs_move               *next;
   retro_atomic_int_t             state;
   bool                           ok;
   unsigned                       lifetime;
   unsigned                       writes;
   int64_t                        size;
   int64_t                        mtime;
   char                          *path;
   char                          *to;
   char                          *key;
   /* A fetch's download, placed once the file is moved; NULL for the
    * server's deletion */
   char                          *fetching;
   task_cloud_sync_fetch_state_t *fetch_state;
   struct item_file              *server_file;
   char                           hash[33];
} tcs_move_t;

/* Copies in flight stay under both; a bigger copy runs alone. */
#define TCS_MOVE_MAX     4
#define TCS_MOVE_BUDGET  (16 * 1024 * 1024)

static void task_cloud_sync_move_worker(void *data)
{
   tcs_move_t *mv = (tcs_move_t*)data;
   mv->ok         = save_backups_copy_synced(mv->path, mv->to);
   if (retro_atomic_exchange_int(&mv->state, TCS_MOVE_DONE)
         == TCS_MOVE_ABANDONED)
   {
      /* nobody deletes the original now */
      if (mv->ok)
         filestream_delete(mv->to);
      free(mv);
   }
}

static void task_cloud_sync_move_start(tcs_move_t *mv)
{
   sthread_t *thread;
   retro_atomic_store_release_int(&mv->state, TCS_MOVE_RUNNING);
   if ((thread = sthread_create(task_cloud_sync_move_worker, mv)))
   {
      sthread_detach(thread);
      return;
   }
   mv->ok = save_backups_copy_synced(mv->path, mv->to);
   retro_atomic_store_release_int(&mv->state, TCS_MOVE_DONE);
}

/* Starts queued copies in order while the budget allows. */
static void task_cloud_sync_moves_start(task_cloud_sync_state_t *sync_state)
{
   tcs_move_t *mv;
   unsigned    running = 0;
   int64_t     bytes   = 0;

   for (mv = sync_state->moves; mv; mv = mv->next)
   {
      if (retro_atomic_load_acquire_int(&mv->state) == TCS_MOVE_QUEUED)
      {
         if (     running
               && (   running + 1 >= TCS_MOVE_MAX
                   || bytes + mv->size >= TCS_MOVE_BUDGET))
            return;
         task_cloud_sync_move_start(mv);
      }
      running++;
      bytes += mv->size;
   }
}

/* Queues the copy of @path to @to that its backup owes; until it is
 * done the key is in flight (`waiting`). A fetch's own unit carries
 * over. */
static bool task_cloud_sync_move_push(task_cloud_sync_state_t *sync_state,
      task_cloud_sync_fetch_state_t *fetch_state,
      struct item_file *server_file, const char *key, const char *path,
      const char *to, const char *fetching, const char *hash)
{
   tcs_move_t **tail;
   RFILE *reserve;
   size_t path_len     = strlen(path) + 1;
   size_t to_len       = strlen(to) + 1;
   size_t key_len      = strlen(key) + 1;
   size_t fetching_len = fetching ? strlen(fetching) + 1 : 0;
   tcs_move_t *mv      = (tcs_move_t*)calloc(1,
         sizeof(*mv) + path_len + to_len + key_len + fetching_len);

   /* The copy's temporary holds its name until the copy runs: a cut
    * name may be another file's too */
   if (!mv || !(reserve = filestream_open_atomic(to)))
   {
      free(mv);
      return false;
   }
   filestream_close(reserve);
   mv->path = (char*)(mv + 1);
   memcpy(mv->path, path, path_len);
   mv->to   = mv->path + path_len;
   memcpy(mv->to, to, to_len);
   mv->key  = mv->to + to_len;
   memcpy(mv->key, key, key_len);
   if (fetching)
   {
      mv->fetching = mv->key + key_len;
      memcpy(mv->fetching, fetching, fetching_len);
   }
   if (hash)
      strlcpy(mv->hash, hash, sizeof(mv->hash));
   mv->fetch_state = fetch_state;
   mv->server_file = server_file;
   mv->lifetime    = content_savefile_lifetime();
   mv->writes      = content_save_writes();
   if ((mv->size = path_get_size(path)) < 0)
      mv->size = TCS_MOVE_BUDGET;
   if (!path_get_mtime(path, &mv->mtime))
      mv->mtime = 0;
   retro_atomic_int_init(&mv->state, TCS_MOVE_QUEUED);

   for (tail = &sync_state->moves; *tail; tail = &(*tail)->next);
   *tail = mv;
   if (!fetch_state)
      task_cloud_sync_waiting_inc(sync_state);
   task_cloud_sync_moves_start(sync_state);
   return true;
}

/* The file may have been written after the copy read it: by content
 * loaded or closed meanwhile, by a save or state write of the frontend
 * (one still in flight may land yet), or by the core itself, which only
 * its size and mtime tell. */
static bool task_cloud_sync_move_touched(const tcs_move_t *mv)
{
   int64_t mtime = 0;
   if (     content_savefile_is_live(mv->path)
         || content_savefile_lifetime() != mv->lifetime
         || content_save_writes()       != mv->writes
         || content_save_state_in_progress(NULL)
         || path_get_size(mv->path)     != mv->size)
      return true;
   if (!path_get_mtime(mv->path, &mtime))
      mtime = 0;
   return mtime != mv->mtime;
}

/* The original goes once its copy is on the disk. A file written during
 * the copy stays, and the copy goes, for the next sync to see; the copy
 * of a file gone meanwhile stays. */
static void task_cloud_sync_move_finish(task_cloud_sync_state_t *sync_state,
      tcs_move_t *mv)
{
   task_cloud_sync_fetch_state_t *fetch_state = mv->fetch_state;
   const char *server_hash = CS_FILE_HASH(mv->server_file);

   if (task_cloud_sync_move_touched(mv))
   {
      RARCH_LOG(CSPFX "Deferring \"%s\", it may have been written during its backup.\n",
            mv->key);
      if (mv->ok && path_is_valid(mv->path))
         filestream_delete(mv->to);
      if (mv->fetching)
         filestream_delete(mv->fetching);
      task_cloud_sync_carry_manifests_forward(sync_state, mv->key,
            server_hash);
   }
   else if (!mv->ok || !save_backups_move_finish(mv->path, mv->to))
   {
      RARCH_ERR(CSPFX "Could not back \"%s\" up to \"%s\".\n",
            mv->path, mv->to);
      if (mv->fetching)
         filestream_delete(mv->fetching);
      else
         RARCH_ERR(CSPFX "Keeping \"%s\", the local file could not be removed.\n",
               mv->key);
      task_cloud_sync_carry_manifests_forward(sync_state, mv->key,
            server_hash);
      sync_state->failures = true;
   }
   else if (!fetch_state)
   {
      task_cloud_sync_add_to_updated_manifest(sync_state, mv->key,
            NULL, true);
      task_cloud_sync_add_to_updated_manifest(sync_state, mv->key,
            server_hash, false);
   }
   else if (task_cloud_sync_fetch_place(fetch_state, mv->key, mv->fetching))
      task_cloud_sync_fetched(sync_state, fetch_state->server_file,
            mv->key, mv->hash);

   free(fetch_state);
   free(mv);
   task_cloud_sync_waiting_dec(sync_state);
}

/* Finishes the copies that are done and starts what the budget then
 * allows. Never waits. */
static void task_cloud_sync_moves_run(task_cloud_sync_state_t *sync_state)
{
   tcs_move_t **link = &sync_state->moves;
   while (*link)
   {
      tcs_move_t *mv = *link;
      if (retro_atomic_load_acquire_int(&mv->state) != TCS_MOVE_DONE)
      {
         link = &mv->next;
         continue;
      }
      *link = mv->next;
      task_cloud_sync_move_finish(sync_state, mv);
   }
   task_cloud_sync_moves_start(sync_state);
}

/* A sync released with copies in flight: each copy is undone and its
 * original kept, by whichever side lets go of it last. */
static void task_cloud_sync_moves_abandon(task_cloud_sync_state_t *sync_state)
{
   while (sync_state->moves)
   {
      tcs_move_t *mv    = sync_state->moves;
      sync_state->moves = mv->next;
      if (mv->fetching)
         filestream_delete(mv->fetching);
      free(mv->fetch_state);
      switch (retro_atomic_exchange_int(&mv->state, TCS_MOVE_ABANDONED))
      {
         case TCS_MOVE_RUNNING:
            break;
         case TCS_MOVE_DONE:
            if (mv->ok)
               filestream_delete(mv->to);
            free(mv);
            break;
         default:
            /* never started: its temporary only held the name */
            filestream_finish_atomic(mv->to, false);
            free(mv);
            break;
      }
   }
}
#endif

/* Deletes the temporary files of backup copies in backups/cloud-sync
 * that never finished, as the atomic writer names them. Only at the
 * first sync of a process: each is from a process that ended mid-copy,
 * and none of this one's copies can be in flight before that sync's
 * diff. */
static void task_cloud_sync_sweep_temps(task_cloud_sync_state_t *sync_state)
{
   static bool swept = false;
   size_t i;
   struct string_list *list = NULL;
   char *dir;

   if (swept)
      return;
   swept = true;
   if (!(dir = (char*)malloc(DIR_MAX_LENGTH)))
      return;
   if (task_cloud_sync_backup_root(sync_state, dir, DIR_MAX_LENGTH))
      list = dir_list_new(dir, "tmp", false, true, false, true);
   free(dir);
   for (i = 0; list && i < list->size; i++)
   {
      struct save_backup_name parsed;
      char        name[256];
      const char *base = path_basename_nocompression(list->elems[i].data);
      size_t      _len = strlen(base);

      if (     _len <= STRLEN_CONST(CS_TEMP_SUFFIX)
            || !string_is_equal(base + _len - STRLEN_CONST(CS_TEMP_SUFFIX),
                  CS_TEMP_SUFFIX))
         continue;
      _len -= STRLEN_CONST(CS_TEMP_SUFFIX);
      if (_len >= sizeof(name))
         continue;
      memcpy(name, base, _len);
      name[_len] = '\0';
      if (     !save_backups_parse(name, &parsed)
            || parsed.core_len != STRLEN_CONST(SAVE_BACKUPS_CORE_CLOUD_SYNC)
            || strncmp(name + parsed.core_off, SAVE_BACKUPS_CORE_CLOUD_SYNC,
                  parsed.core_len))
         continue;
      filestream_delete(list->elems[i].data);
   }
   string_list_free(list);
}

/* Hands a download to the task thread to hash a chunk at a time. The
 * fetch stays in flight (`waiting`) until then. False when it could
 * not be handed over: nothing was taken. */
static bool task_cloud_sync_fetched_push(
      task_cloud_sync_fetch_state_t *fetch_state, const char *path,
      RFILE *file)
{
   task_cloud_sync_state_t *sync_state = fetch_state->sync_state;
   size_t                   len        = strlen(path) + 1;
   tcs_fetched_t           *rec        = (tcs_fetched_t*)malloc(sizeof(*rec) + len);
#if defined(TCS_LOCK_FREE)
   void                    *head;
#endif

   if (!rec)
      return false;
   rec->fetch_state = fetch_state;
   rec->file        = file;
   rec->path        = (char*)(rec + 1);
   memcpy(rec->path, path, len);
#if defined(TCS_LOCK_FREE)
   do
   {
      head      = retro_atomic_load_acquire_ptr(&sync_state->fetched_in);
      rec->next = (tcs_fetched_t*)head;
   } while (!retro_atomic_cas_ptr(&sync_state->fetched_in, head, rec));
#else
#ifdef HAVE_THREADS
   slock_lock(tcs_manifest_lock);
#endif
   rec->next              = sync_state->fetched_in;
   sync_state->fetched_in = rec;
#ifdef HAVE_THREADS
   slock_unlock(tcs_manifest_lock);
#endif
#endif
   return true;
}

/* Moves what was handed over to the end of the task thread's queue,
 * oldest first. */
static void task_cloud_sync_fetched_take(task_cloud_sync_state_t *sync_state)
{
   tcs_fetched_t *in, *rev = NULL, **tail;
#if defined(TCS_LOCK_FREE)
   in = (tcs_fetched_t*)retro_atomic_exchange_ptr(&sync_state->fetched_in, NULL);
#else
#ifdef HAVE_THREADS
   slock_lock(tcs_manifest_lock);
#endif
   in                     = sync_state->fetched_in;
   sync_state->fetched_in = NULL;
#ifdef HAVE_THREADS
   slock_unlock(tcs_manifest_lock);
#endif
#endif
   while (in)
   {
      tcs_fetched_t *next = in->next;
      in->next = rev;
      rev      = in;
      in       = next;
   }
   for (tail = &sync_state->fetched; *tail; tail = &(*tail)->next);
   *tail = rev;
}

/* A download comes back on the stream it was written through, which
 * cannot lend its bytes, so hashing it would read the whole file back
 * into the hash buffer.  Opened again to read with the mapping hint it
 * lends them where they lie, out of the page cache the write just
 * filled.  Under one hash chunk the file is a single read, cheaper
 * than another open, so it stays as it is; where the platform has no
 * mapping the new stream reads as the old one did. */
static void task_cloud_sync_fetched_lend(tcs_fetched_t *rec)
{
   char   tmp[PATH_MAX_LENGTH];
   RFILE *lent;

   if (     filestream_get_size(rec->file) < CS_HASH_CHUNK
         || !task_cloud_sync_fetching_path(tmp, sizeof(tmp),
               rec->fetch_state->local)
         || filestream_flush(rec->file) != 0)
      return;
   if (!(lent = filestream_open(tmp, RETRO_VFS_FILE_ACCESS_READ,
               RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS)))
      return;
   filestream_close(rec->file);
   rec->file = lent;
}

/* Hashes the downloads handed over, inside @b, finishing each fetch
 * once its file is hashed. */
static void task_cloud_sync_fetched_run(task_cloud_sync_state_t *sync_state,
      nbio_budget_t *b)
{
   task_cloud_sync_fetched_take(sync_state);
   while (sync_state->fetched)
   {
      tcs_fetched_t  *rec = sync_state->fetched;
      enum tcs_commit commit;
      char            hash[33];

      if (!sync_state->hash_fetched.file)
      {
         task_cloud_sync_fetched_lend(rec);
         if (     !sync_state->hash_buf
               && !(sync_state->hash_buf = (uint8_t*)malloc(CS_HASH_CHUNK)))
            hash[0] = '\0';
         else
            task_cloud_sync_hash_begin(&sync_state->hash_fetched, rec->file);
      }
      if (sync_state->hash_fetched.file)
      {
         if (!task_cloud_sync_hash_step(&sync_state->hash_fetched,
                  sync_state->hash_buf, b, hash))
            return;
      }
      else
      {
         /* no buffer: the whole file now, as before */
         char *h = task_cloud_sync_md5_rfile(rec->file);
         strlcpy(hash, h ? h : "", sizeof(hash));
         free(h);
      }
      filestream_close(rec->file);
      if (!*hash)
      {
         /* the local file is untouched, as on a failed download */
         RARCH_WARN(CSPFX "Could not hash the download of \"%s\".\n",
               rec->path);
         task_cloud_sync_fetch_discard(rec->fetch_state);
         task_cloud_sync_keep_records(sync_state, rec->path);
         sync_state->failures = true;
         commit = TCS_COMMIT_LEFT;
      }
      else
         commit = task_cloud_sync_fetch_commit(rec->fetch_state,
               rec->path, hash);
      if (commit == TCS_COMMIT_PLACED)
         task_cloud_sync_fetched(sync_state, rec->fetch_state->server_file,
               rec->path, hash);
      sync_state->fetched = rec->next;
      if (commit != TCS_COMMIT_OWED)
      {
         free(rec->fetch_state);
         task_cloud_sync_waiting_dec(sync_state);
      }
      free(rec);
   }
}

static void task_cloud_sync_fetch_cb(void *user_data, const char *path, bool success, RFILE *file)
{
   task_cloud_sync_fetch_state_t *fetch_state = (task_cloud_sync_fetch_state_t *)user_data;
   task_cloud_sync_state_t       *sync_state;
   struct item_file              *server_file;

   if (!fetch_state)
      return;

   sync_state = fetch_state->sync_state;
   server_file = fetch_state->server_file;

   if (success && file)
   {
      /* hashed by the task thread, a chunk at a time; the fetch stays
       * in flight until then */
      if (task_cloud_sync_fetched_push(fetch_state, path, file))
         return;
      filestream_close(file);
      task_cloud_sync_fetch_discard(fetch_state);
      task_cloud_sync_carry_manifests_forward(sync_state, path,
            CS_FILE_HASH(server_file));
      sync_state->failures = true;
      RARCH_WARN(CSPFX "Could not queue the download of \"%s\".\n", path);
   }
   else if (success)
   {
      /* The drivers report success with no file only when the server
       * does not have it.  Keeping it in the server manifest would fail
       * the same fetch on every sync, so drop it there; the local copy
       * and its record stay, so the next sync uploads it only if it
       * changed here. */
      RARCH_WARN(CSPFX "\"%s\" is in the server manifest but missing on the server, removing it from the manifest.\n", path);
      task_cloud_sync_keep_local_record(sync_state, path);
      sync_state->need_manifest_uploaded = true;
      sync_state->failures               = true;
   }
   else
   {
      /* the local file is untouched: both records stay, so the next
       * sync fetches it again */
      RARCH_WARN(CSPFX "Failed to fetch \"%s\".\n", path);
      if (file)
         filestream_close(file);
      task_cloud_sync_fetch_discard(fetch_state);
      task_cloud_sync_keep_records(sync_state, path);
      sync_state->failures = true;
   }

   task_cloud_sync_waiting_dec(sync_state);

   free(fetch_state);
}

static void task_cloud_sync_fetch_server_file(task_cloud_sync_state_t *sync_state)
{
   size_t                         i, idx;
   char                           filename[PATH_MAX_LENGTH];
   char                           fetching[PATH_MAX_LENGTH];
   char                           directory[DIR_MAX_LENGTH];
   struct string_list            *dirlist     = sync_state->dirlist;
   struct item_file              *server_file = &sync_state->server_manifest->list[sync_state->server_idx];
   const char                    *key         = CS_FILE_KEY(server_file);
   /* The key from the server file is in "portable" format, use '/'.
    * Server-supplied; treat as untrusted. cloud_sync_manifest_key_path()
    * returns the relative path portion, or NULL for a malformed/traversal
    * key (NULL, no '/', empty, absolute, or containing "..") that would
    * otherwise let a hostile manifest write outside the cloud-sync base
    * directory via fill_pathname_join_special. Regression coverage:
    * samples/tasks/cloudsync/cloudsync_path_safety_test.c. */
   const char                    *path        = cloud_sync_manifest_key_path(key);
   task_cloud_sync_fetch_state_t *fetch_state;

   if (!path)
   {
      RARCH_WARN(CSPFX "Refusing malformed/traversal key from server: %s\n",
            key ? key : "(null)");
      task_cloud_sync_add_to_updated_manifest(sync_state, key, CS_FILE_HASH(server_file), true);
      sync_state->failures = true;
      return;
   }

   /* there is a weird thing that can happen, where the server file changes but
    * the manifest does not have the updated hash. in that case when the file is
    * downloaded we have an opportunity to check that the hash matches and
    * update the server manifest, and correct the issue (over a local copy
    * only under Keep Server: otherwise a conflict, see
    * task_cloud_sync_fetch_commit()). to do this the server
    * manifest cannot be updated early; only on error or after the download is
    * successful. on error just add to the updated manifest without marking
    * need_manifest_uplaod. */

   if (task_cloud_sync_should_ignore_file(key))
   {
      /* don't fetch a file we're supposed to ignore, even if the server has it */
      RARCH_LOG(CSPFX "Ignoring %s.\n", key);
      task_cloud_sync_add_to_updated_manifest(sync_state, key, CS_FILE_HASH(server_file), true);
      return;
   }
   RARCH_LOG(CSPFX "Fetching %s.\n", key);

   filename[0] = '\0';
   for (i = 0; dirlist && i < dirlist->size; i++)
   {
      if (!string_starts_with(key, dirlist->elems[i].data))
         continue;
      fill_pathname_join_special(filename,
            (const char*)dirlist->elems[i].userdata,
            path, sizeof(filename));
      pathname_conform_slashes_to_os(filename);
      break;
   }
   if (!*filename)
   {
      /* how did this end up here? we don't know where to put it... */
      RARCH_WARN(CSPFX "Don't know where to put %s!\n", key);
      task_cloud_sync_add_to_updated_manifest(sync_state,
            key, CS_FILE_HASH(server_file), true);
      return;
   }

   if (task_cloud_sync_defer_live_savefile(sync_state, key, filename,
            CS_FILE_HASH(server_file)))
      return;

   fill_pathname_basedir(directory, filename, sizeof(directory));
   path_mkdir(directory);
   fetch_state = (task_cloud_sync_fetch_state_t*)malloc(sizeof(task_cloud_sync_fetch_state_t));
   if (!fetch_state)
   {
      RARCH_WARN(CSPFX "Wanted to fetch %s but failed to malloc.\n", key);
      task_cloud_sync_keep_records(sync_state, key);
      sync_state->failures = true;
      return;
   }
   fetch_state->sync_state  = sync_state;
   fetch_state->server_file = server_file;
   fetch_state->had_file    =
            file_list_search(sync_state->local_manifest, key, &idx)
         && string_is_equal_noncase(
            CS_FILE_KEY(&sync_state->local_manifest->list[idx]), key)
         && !string_is_empty(
            CS_FILE_HASH(&sync_state->local_manifest->list[idx]));
   strlcpy(fetch_state->local, filename, sizeof(fetch_state->local));
   if (     task_cloud_sync_fetching_path(fetching, sizeof(fetching), filename)
         && cloud_sync_read(key, fetching, task_cloud_sync_fetch_cb, fetch_state))
      task_cloud_sync_waiting_inc(sync_state);
   else
   {
      RARCH_WARN(CSPFX "Wanted to fetch %s but failed.\n", key);
      task_cloud_sync_keep_records(sync_state, key);
      sync_state->failures = true;
      free(fetch_state);
   }
}

static void task_cloud_sync_resolve_conflict(task_cloud_sync_state_t *sync_state)
{
   struct item_file *server_file = &sync_state->server_manifest->list[sync_state->server_idx];

   if (sync_state->conflict_resolution == 1)
   {
      /* Keep local: upload local file to server */
      RARCH_LOG(CSPFX "Conflict on %s, keeping local.\n", CS_FILE_KEY(server_file));
      task_cloud_sync_upload_current_file(sync_state);
   }
   else if (sync_state->conflict_resolution == 2)
   {
      /* Keep server: download server file, or take its delete */
      RARCH_LOG(CSPFX "Conflict on %s, keeping server.\n", CS_FILE_KEY(server_file));
      if (!string_is_empty(CS_FILE_HASH(server_file)))
         task_cloud_sync_fetch_server_file(sync_state);
      else if (task_cloud_sync_delete_current_file(sync_state))
      {
         task_cloud_sync_add_to_updated_manifest(sync_state,
               CS_FILE_KEY(server_file), NULL, true);
         task_cloud_sync_add_to_updated_manifest(sync_state,
               CS_FILE_KEY(server_file), CS_FILE_HASH(server_file), false);
      }
   }
   else
   {
      /* Default: ignore the conflict, keep file out of local manifest */
      RARCH_WARN(CSPFX "Conflicting change of %s.\n", CS_FILE_KEY(server_file));
      task_cloud_sync_add_to_updated_manifest(sync_state, CS_FILE_KEY(server_file), CS_FILE_HASH(server_file), true);
      /* no need to mark need_manifest_uploaded, nothing changed */
      sync_state->conflicts = true;
   }
}

static void task_cloud_sync_upload_cb(void *user_data, const char *path, bool success, RFILE *file)
{
   task_cloud_sync_state_t *sync_state = (task_cloud_sync_state_t *)user_data;
   size_t                   idx;

   if (file)
      filestream_close(file);

   if (!sync_state)
      return;

   if (success)
   {
      /* need to update server manifest as well */
      if (file_list_search(sync_state->current_manifest, path, &idx))
      {
         struct item_file *current_file = &sync_state->current_manifest->list[idx];
         task_cloud_sync_add_to_updated_manifest(sync_state, path, CS_FILE_HASH(current_file), true);
         task_cloud_sync_add_to_updated_manifest(sync_state, path, CS_FILE_HASH(current_file), false);
         sync_state->need_manifest_uploaded = true;
      }
      RARCH_LOG(CSPFX "Uploading \"%s\" succeeded.\n", path);
      sync_state->uploads++;
   }
   else
   {
      task_cloud_sync_keep_records(sync_state, path);
      RARCH_WARN(CSPFX "Uploading \"%s\" failed.\n", path);
      sync_state->failures = true;
   }

   task_cloud_sync_waiting_dec(sync_state);
}

/**
 * task_cloud_sync_upload_current_file:
 * @sync_state 	: pointer to the current sync task
 *
 * Uploads the current file to the cloud. The current file is defined
 * as whatever file is indicated by the current manifest (that is, the
 * local, actually true manifest) combined with whatever the iteration
 * variable indicates (the one specific to the current manifest)
 */
static void task_cloud_sync_upload_current_file(task_cloud_sync_state_t *sync_state)
{
   struct item_file *item     = &sync_state->current_manifest->list[sync_state->current_idx];
   const char       *path     = CS_FILE_KEY(item);
   const char       *filename = item->path;
   RFILE            *file;

   if (task_cloud_sync_should_ignore_file(path))
   {
      RARCH_LOG(CSPFX "Ignoring \"%s\", not uploading.\n", path);
      return;
   }

   file = filestream_open(filename,
         RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
   if (!file)
   {
      task_cloud_sync_keep_records(sync_state, path);
      RARCH_WARN(CSPFX "Could not read \"%s\".\n", filename);
      sync_state->failures = true;
      return;
   }

   if (!item->userdata && !(item->userdata = task_cloud_sync_md5_rfile(file)))
   {
      task_cloud_sync_keep_records(sync_state, path);
      filestream_close(file);
      RARCH_WARN(CSPFX "Could not hash \"%s\".\n", filename);
      sync_state->failures = true;
      return;
   }

   RARCH_LOG(CSPFX "Uploading \"%s\".\n", path);

   filestream_seek(file, 0, SEEK_SET);
   task_cloud_sync_waiting_inc(sync_state);
   if (!cloud_sync_update(path, file, task_cloud_sync_upload_cb, sync_state))
   {
      task_cloud_sync_keep_records(sync_state, path);
      filestream_close(file);
      task_cloud_sync_waiting_dec(sync_state);
      sync_state->failures = true;
      RARCH_WARN(CSPFX "Uploading \"%s\" failed.\n", path);
   }
}

/* Returns false when the local file was left in place, in which case the
 * caller must not record the delete as sync'd, or when its backup's copy
 * records it once done. */
static bool task_cloud_sync_delete_current_file(task_cloud_sync_state_t *sync_state)
{
   bool removed;
   struct item_file *item        = &sync_state->current_manifest->list[sync_state->current_idx];
   struct item_file *server_file = &sync_state->server_manifest->list[sync_state->server_idx];
   bool cloud_sync_destructive   = sync_state->destructive;

   if (task_cloud_sync_defer_live_savefile(sync_state, CS_FILE_KEY(item),
            item->path, CS_FILE_HASH(server_file)))
      return false;

   RARCH_WARN(CSPFX "Server has deleted \"%s\", so shall we.\n", CS_FILE_KEY(item));

   if (cloud_sync_destructive)
      removed = filestream_delete(item->path) == 0;
   else
   {
      char *owed = NULL;
      enum save_backup_result made = task_cloud_sync_backup_file(
            sync_state, item, SAVE_BACKUP_DELETED, &owed);
#if defined(TCS_MOVE_WORKER)
      if (made == SAVE_BACKUP_COPY_OWED)
      {
         bool queued = task_cloud_sync_move_push(sync_state, NULL,
               server_file, CS_FILE_KEY(server_file), item->path, owed,
               NULL, NULL);
         free(owed);
         if (queued)
            return false;
      }
#endif
      removed = made == SAVE_BACKUP_MADE;
   }

   if (!removed)
   {
      RARCH_ERR(CSPFX "Keeping \"%s\", the local file could not be removed.\n",
            CS_FILE_KEY(item));
      task_cloud_sync_carry_manifests_forward(sync_state, CS_FILE_KEY(item),
            CS_FILE_HASH(server_file));
      sync_state->failures = true;
      return false;
   }

   return true;
}

static void task_cloud_sync_check_server_current(task_cloud_sync_state_t *sync_state, bool include_local)
{
   bool server_changed, current_changed;
   struct item_file *server_file  = &sync_state->server_manifest->list[sync_state->server_idx];
   struct item_file *local_file   = NULL;
   struct item_file *current_file = &sync_state->current_manifest->list[sync_state->current_idx];
   const char       *filename     = current_file->path;
   RFILE            *file;

   if (task_cloud_sync_should_ignore_file(CS_FILE_KEY(server_file)))
   {
      RARCH_LOG(CSPFX "Ignoring \"%s\" (despite possible conflict).\n", CS_FILE_KEY(server_file));
      return;
   }

   /* normally hashed already, a chunk at a time, before this step */
   if (!current_file->userdata)
   {
      file = filestream_open(filename,
            RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
      if (!file)
      {
         task_cloud_sync_keep_records(sync_state, CS_FILE_KEY(server_file));
         RARCH_WARN(CSPFX "Could not read \"%s\".\n", filename);
         sync_state->failures = true;
         return;
      }
      current_file->userdata = task_cloud_sync_md5_rfile(file);
      filestream_close(file);
      if (!current_file->userdata)
      {
         task_cloud_sync_keep_records(sync_state, CS_FILE_KEY(server_file));
         RARCH_WARN(CSPFX "Could not hash \"%s\".\n", filename);
         sync_state->failures = true;
         return;
      }
   }

   if (string_is_equal(CS_FILE_HASH(server_file), CS_FILE_HASH(current_file)))
   {
      task_cloud_sync_add_to_updated_manifest(sync_state, CS_FILE_KEY(current_file), CS_FILE_HASH(current_file), true);
      task_cloud_sync_add_to_updated_manifest(sync_state, CS_FILE_KEY(current_file), CS_FILE_HASH(current_file), false);
      /* No need to mark need_manifest_uploaded, nothing changed */
      return;
   }

   if (!include_local)
   {
      task_cloud_sync_resolve_conflict(sync_state);
      return;
   }

   local_file      = &sync_state->local_manifest->list[sync_state->local_idx];
   server_changed  = !string_is_equal(CS_FILE_HASH(local_file), CS_FILE_HASH(server_file));
   current_changed = !string_is_equal(CS_FILE_HASH(local_file), CS_FILE_HASH(current_file));

   if (server_changed && current_changed)
      task_cloud_sync_resolve_conflict(sync_state);
   else if (current_changed)
      task_cloud_sync_upload_current_file(sync_state);
   else if (!string_is_empty(CS_FILE_HASH(server_file)))
      task_cloud_sync_fetch_server_file(sync_state);
   else if (task_cloud_sync_delete_current_file(sync_state))
   {
      task_cloud_sync_add_to_updated_manifest(sync_state, CS_FILE_KEY(server_file), NULL, true);
      task_cloud_sync_add_to_updated_manifest(sync_state, CS_FILE_KEY(server_file), CS_FILE_HASH(server_file), false);
   }
}

static void task_cloud_sync_delete_cb(void *user_data, const char *path, bool success, RFILE *file)
{
   task_cloud_sync_state_t *sync_state = (task_cloud_sync_state_t *)user_data;

   if (!sync_state)
      return;

   if (!success)
   {
      /* the next sync tries it again */
      task_cloud_sync_keep_records(sync_state, path);
      RARCH_WARN(CSPFX "Deleting \"%s\" failed.\n", path);
      sync_state->failures = true;
      task_cloud_sync_waiting_dec(sync_state);
      return;
   }

   RARCH_LOG(CSPFX "Deleting \"%s\" succeeded.\n", path);
   /* need to update server manifest. we don't set the hash as that indicates a
    * deleted file. need to update the local manifest to indicate we sync'd that
    * it is deleted */
   task_cloud_sync_add_to_updated_manifest(sync_state, path, NULL, true);
   task_cloud_sync_add_to_updated_manifest(sync_state, path, NULL, false);
   sync_state->need_manifest_uploaded = true;
   task_cloud_sync_waiting_dec(sync_state);
}

static void task_cloud_sync_delete_server_file(task_cloud_sync_state_t *sync_state)
{
   struct item_file *server_file = &sync_state->server_manifest->list[sync_state->server_idx];
   const char       *key = CS_FILE_KEY(server_file);

   if (task_cloud_sync_should_ignore_file(key))
   {
      RARCH_LOG(CSPFX "Ignoring \"%s\", instead of removing from server.\n", key);
      return;
   }

   RARCH_LOG(CSPFX "Deleting \"%s\".\n", key);

   task_cloud_sync_waiting_inc(sync_state);
   if (!cloud_sync_free(key, task_cloud_sync_delete_cb, sync_state))
   {
      /* if the delete fails, resurrect the hash from the last sync */
      size_t idx;
      if (file_list_search(sync_state->local_manifest, key, &idx))
      {
         struct item_file *local_file = &sync_state->local_manifest->list[idx];
         task_cloud_sync_add_to_updated_manifest(sync_state, key, CS_FILE_HASH(local_file), false);
      }
      task_cloud_sync_add_to_updated_manifest(sync_state, key, CS_FILE_HASH(server_file), true);
      /* we don't mark need_manifest_uploaded here, nothing has changed */
      task_cloud_sync_waiting_dec(sync_state);
   }
}

static void task_cloud_sync_maybe_ignore(task_cloud_sync_state_t *sync_state)
{
   struct string_list *dirlist = sync_state->dirlist;
   bool found;

   if (!dirlist)
      return;

   if (sync_state->local_manifest)
   {
      for (found = false; !found && sync_state->local_idx < sync_state->local_manifest->size; )
      {
         struct item_file *local_file  = &sync_state->local_manifest->list[sync_state->local_idx];
         const char *key = CS_FILE_KEY(local_file);
         found = task_cloud_sync_key_in_scope(dirlist, key);
         /* we have a record of doing a sync for this file but now no longer
          * wish to sync it. keep the record? might as well, in case the option
          * gets turned back on later. */
         if (!found)
         {
            task_cloud_sync_add_to_updated_manifest(sync_state, key, CS_FILE_HASH(local_file), false);
            sync_state->local_idx++;
         }
      }
   }

   if (sync_state->server_manifest)
   {
      for (found = false; !found && sync_state->server_idx < sync_state->server_manifest->size; )
      {
         struct item_file *server_file  = &sync_state->server_manifest->list[sync_state->server_idx];
         const char *key = CS_FILE_KEY(server_file);
         found = task_cloud_sync_key_in_scope(dirlist, key);
         /* must keep the server's manifest complete */
         if (!found)
         {
            task_cloud_sync_add_to_updated_manifest(sync_state, key, CS_FILE_HASH(server_file), true);
            sync_state->server_idx++;
            sync_state->server_skipped++;
         }
      }
   }
}

/* Kept on the server and left out of the local manifest, so the next
 * sync fetches it back. */
static void task_cloud_sync_hold_delete(task_cloud_sync_state_t *sync_state)
{
   struct item_file *server_file = &sync_state->server_manifest->list[sync_state->server_idx];
   union string_list_elem_attr attr;

   attr.i = 0;
   task_cloud_sync_add_to_updated_manifest(sync_state,
         CS_FILE_KEY(server_file), CS_FILE_HASH(server_file), true);
   sync_state->held_count++;
   if (!sync_state->held)
      sync_state->held = string_list_new();
   if (     !sync_state->held
         || !string_list_append(sync_state->held, CS_FILE_KEY(server_file), attr))
      sync_state->failures = true;
}

/* Whether @current_file differs from what the last sync left here
 * (@local_file). Unchanged, its record stays for the next sync. */
static bool task_cloud_sync_changed_here(task_cloud_sync_state_t *sync_state,
      struct item_file *current_file, struct item_file *local_file)
{
   const char *key = CS_FILE_KEY(current_file);

   if (!current_file->userdata)
   {
      RFILE *file = filestream_open(current_file->path,
            RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
      if (file)
      {
         current_file->userdata = task_cloud_sync_md5_rfile(file);
         filestream_close(file);
      }
      if (!current_file->userdata)
      {
         task_cloud_sync_keep_records(sync_state, key);
         RARCH_WARN(CSPFX "Could not hash \"%s\".\n", current_file->path);
         sync_state->failures = true;
         return false;
      }
   }
   if (!string_is_equal(CS_FILE_HASH(current_file), CS_FILE_HASH(local_file)))
      return true;
   RARCH_LOG(CSPFX "\"%s\" is not on the server and has not changed here, not uploading.\n", key);
   task_cloud_sync_add_to_updated_manifest(sync_state, key,
         CS_FILE_HASH(local_file), false);
   sync_state->unsent_count++;
   return false;
}

/* @key's file under @root into @s. False when it has none. */
static bool task_cloud_sync_root_path(
      const task_cloud_sync_state_t *sync_state, int root, const char *key,
      char *s, size_t len)
{
   const char *path = cloud_sync_manifest_key_path(key);

   if (root < 0 || !path)
      return false;
   fill_pathname_join_special(s,
         (const char*)sync_state->dirlist->elems[root].userdata,
         path, len);
   pathname_conform_slashes_to_os(s);
   return true;
}

/* A download of @key cut off before it was moved in, its local copy
 * maybe already in the backups: not a delete here. */
static bool task_cloud_sync_fetch_left(
      const task_cloud_sync_state_t *sync_state, int root, const char *key)
{
   char   local[PATH_MAX_LENGTH];
   size_t _len;
   if (!task_cloud_sync_root_path(sync_state, root, key,
            local, sizeof(local)))
      return false;
   _len = strlen(local);
   return    strlcpy(local + _len, CS_FETCHING_SUFFIX, sizeof(local) - _len)
               < sizeof(local) - _len
          && path_is_valid(local);
}

/* Whether @key's file is here after all. A save renames a state aside
 * and back, and the walk can land in between: the file is back by now,
 * still held (save_backups_held()), or in a writer's temp. */
static bool task_cloud_sync_still_here(
      const task_cloud_sync_state_t *sync_state, int root, const char *key)
{
   /* A state task's, the RAM state's, and for an ".auto" alone the
    * synchronous auto save's: "G.state.auto.tmp" is not slot 0's */
   static const char *const temps[] = { ".tmp", ".ram.tmp", ".auto.tmp" };
   char        local[PATH_MAX_LENGTH];
   size_t      _len;
   size_t      i, n;

   if (!task_cloud_sync_root_path(sync_state, root, key,
            local, sizeof(local)))
      return false;
   if (path_is_valid(local) || save_backups_held(local))
      return true;
   _len = strlen(local);
   n    = string_ends_with(local, ".auto") ? 3 : 2;
   for (i = 0; i < n; i++)
      if (     strlcpy(local + _len, temps[i], sizeof(local) - _len)
                  < sizeof(local) - _len
            && path_is_valid(local))
         return true;
   return false;
}

static void task_cloud_sync_held_notice(task_cloud_sync_state_t *sync_state)
{
   char msg[128];
   snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_CLOUD_SYNC_DELETIONS_HELD),
         sync_state->held_count);
   RARCH_WARN(CSPFX "%s\n", msg);
   runloop_msg_queue_push(msg, strlen(msg), 1, 180, false, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_WARNING);
}

static void task_cloud_sync_unsent_notice(task_cloud_sync_state_t *sync_state)
{
   char msg[192];
   snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_CLOUD_SYNC_UNCHANGED_NOT_SENT),
         sync_state->unsent_count);
   RARCH_WARN(CSPFX "%s\n", msg);
   runloop_msg_queue_push(msg, strlen(msg), 1, 180, false, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_WARNING);
}

static void task_cloud_sync_diff_next(task_cloud_sync_state_t *sync_state)
{
   int server_local_key_cmp;
   int server_current_key_cmp;
   int current_local_key_cmp;
   struct item_file *server_file  = NULL;
   struct item_file *local_file   = NULL;
   struct item_file *current_file = NULL;

   /* Move past files that are deliberately not being sync'd. There would not be
    * any in the current state, but there may be in the server or local state.
    * If there are in the server state, make sure to leave them in the server's
    * manifest. */
   task_cloud_sync_maybe_ignore(sync_state);

   if (   sync_state->server_manifest
       && sync_state->server_idx < sync_state->server_manifest->size)
      server_file = &sync_state->server_manifest->list[sync_state->server_idx];
   if (   sync_state->local_manifest
       && sync_state->local_idx < sync_state->local_manifest->size)
      local_file = &sync_state->local_manifest->list[sync_state->local_idx];
   if (   sync_state->current_manifest
       && sync_state->current_idx < sync_state->current_manifest->size)
      current_file = &sync_state->current_manifest->list[sync_state->current_idx];

   if (!server_file && !local_file && !current_file)
   {
      RARCH_LOG(CSPFX "Finished processing manifests.\n");
      if (sync_state->held_count)
         task_cloud_sync_held_notice(sync_state);
      if (sync_state->unsent_count)
         task_cloud_sync_unsent_notice(sync_state);
      task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_UPDATE_MANIFESTS);
      return;
   }

   /* Doing a three-way diff of sorted lists of files. grab the first one from
    * each, resolve any difference, move on. */
   server_local_key_cmp = task_cloud_sync_key_cmp(server_file, local_file);

   if (server_local_key_cmp < 0)
   {
      /* server has a file not in the last sync'd manifest */
      server_current_key_cmp = task_cloud_sync_key_cmp(server_file, current_file);
      if (server_current_key_cmp < 0)
      {
         /* the server has a file we don't have, we check the hash */
         if (!string_is_empty(CS_FILE_HASH(server_file)))
         {
            /* not over a file the walk missed: a conflict next time */
            if (task_cloud_sync_still_here(sync_state,
                     task_cloud_sync_key_root(sync_state->dirlist,
                        CS_FILE_KEY(server_file)),
                     CS_FILE_KEY(server_file)))
               task_cloud_sync_carry_manifests_forward(sync_state,
                     CS_FILE_KEY(server_file), CS_FILE_HASH(server_file));
            else
               task_cloud_sync_fetch_server_file(sync_state);
         }
         else
         {
            /* it's deleted on the server, remember that and mark the sync of the delete */
            task_cloud_sync_add_to_updated_manifest(sync_state, CS_FILE_KEY(server_file), NULL, true);
            task_cloud_sync_add_to_updated_manifest(sync_state, CS_FILE_KEY(server_file), NULL, false);
            /* we don't mark need_manifest_uploaded here, nothing has changed */
         }
         sync_state->server_idx++;
      }
      else if (server_current_key_cmp == 0)
      {
         /* the server has a file that we also have locally but haven't fetched from the server previously */
         task_cloud_sync_check_server_current(sync_state, false);
         sync_state->server_idx++;
         sync_state->current_idx++;
      }
      else
      {
         /* we have a file locally that the server doesn't have */
         task_cloud_sync_upload_current_file(sync_state);
         sync_state->current_idx++;
      }
   }
   else if (server_local_key_cmp == 0)
   {
      /* we've seen this file from the server before */
      current_local_key_cmp = task_cloud_sync_key_cmp(current_file, local_file);
      if (current_local_key_cmp < 0)
      {
         /* we have a file locally that the server doesn't have */
         task_cloud_sync_upload_current_file(sync_state);
         sync_state->current_idx++;
      }
      else if (current_local_key_cmp == 0)
      {
         /* we're all looking at the same file */
         task_cloud_sync_check_server_current(sync_state, true);
         sync_state->current_idx++;
         sync_state->local_idx++;
         sync_state->server_idx++;
      }
      else
      {
         /* the file has been deleted locally */
         if (!string_is_empty(CS_FILE_HASH(server_file)))
         {
            int root = task_cloud_sync_key_root(sync_state->dirlist,
                  CS_FILE_KEY(server_file));
            if (string_is_empty(CS_FILE_HASH(local_file)))
            {
               /* previously saw the delete, now it's resurrected; not
                * over a file the walk missed: a conflict next time */
               if (task_cloud_sync_still_here(sync_state, root,
                        CS_FILE_KEY(server_file)))
                  task_cloud_sync_carry_manifests_forward(sync_state,
                        CS_FILE_KEY(server_file), CS_FILE_HASH(server_file));
               else
                  task_cloud_sync_fetch_server_file(sync_state);
            }
            else if (   string_is_equal(CS_FILE_HASH(server_file), CS_FILE_HASH(local_file))
                     || sync_state->conflict_resolution == 1)
            {
               /* server didn't change, or Keep Local keeps the
                * delete: delete from the server, not over a cut fetch */
               if (     task_cloud_sync_still_here(sync_state, root,
                           CS_FILE_KEY(server_file))
                     || task_cloud_sync_fetch_left(sync_state, root,
                           CS_FILE_KEY(server_file)))
                  task_cloud_sync_carry_manifests_forward(sync_state,
                        CS_FILE_KEY(server_file), CS_FILE_HASH(server_file));
               else if (task_cloud_sync_key_held(sync_state, root,
                        CS_FILE_KEY(server_file)))
                  task_cloud_sync_hold_delete(sync_state);
               else
                  task_cloud_sync_delete_server_file(sync_state);
            }
            else if (task_cloud_sync_fetch_left(sync_state, root,
                        CS_FILE_KEY(server_file)))
               /* not deleted: its fetch was cut off, finish it */
               task_cloud_sync_fetch_server_file(sync_state);
            else
            {
               /* the server changed and local deleted, that's a conflict;
                * unresolved, the local manifest keeps it too, so the
                * next sync meets it again */
               if (sync_state->conflict_resolution != 2)
                  task_cloud_sync_add_to_updated_manifest(sync_state,
                        CS_FILE_KEY(local_file), CS_FILE_HASH(local_file), false);
               task_cloud_sync_resolve_conflict(sync_state);
            }
         }
         else
         {
            /* already deleted, oh well */
            task_cloud_sync_add_to_updated_manifest(sync_state, CS_FILE_KEY(server_file), NULL, true);
            task_cloud_sync_add_to_updated_manifest(sync_state, CS_FILE_KEY(server_file), NULL, false);
            /* we don't mark need_manifest_uploaded here, nothing has changed */
         }
         sync_state->local_idx++;
         sync_state->server_idx++;
      }
   }
   else
   {
      /* the server is missing a file that we've sync'd before? should have at
       * least had a deleted record? assume the server state got reset and treat
       * as a missing file on the server. A copy unchanged since the last
       * sync may be older than the one the server lost: only Upload
       * Local sends it. */
      current_local_key_cmp = task_cloud_sync_key_cmp(current_file, local_file);
      if (current_local_key_cmp < 0)
      {
         task_cloud_sync_upload_current_file(sync_state);
         sync_state->current_idx++;
      }
      else if (current_local_key_cmp == 0)
      {
         if (     sync_state->conflict_resolution == 3
               || task_cloud_sync_changed_here(sync_state,
                     current_file, local_file))
            task_cloud_sync_upload_current_file(sync_state);
         sync_state->current_idx++;
         sync_state->local_idx++;
      }
      else
      {
         /* this is odd, it exists in the last sync manifest but not on the
          * server and not on disk? wtf? */
         RARCH_WARN(CSPFX "%s only exists in previous manifest?\n", CS_FILE_KEY(local_file));
         sync_state->local_idx++;
      }
   }
}

static void task_cloud_sync_update_manifest_cb(void *user_data, const char *path, bool success, RFILE *file)
{
   task_cloud_sync_state_t *sync_state = (task_cloud_sync_state_t *)user_data;

   if (file)
      filestream_close(file);

   if (!sync_state)
      return;

   /* `success` used to be ignored here: a failed server-manifest upload
    * was logged as a success and never set ->failures, so the task
    * finished titled "Cloud Sync finished" while the server's record of
    * what this device holds was left behind.  Every other completion
    * callback in this file honours it. */
   if (success)
   {
      RARCH_LOG(CSPFX "Uploading updated manifest succeeded.\n");
      /* Only now is the local manifest true.  See
       * task_cloud_sync_commit_local_manifest(). */
      task_cloud_sync_commit_local_manifest(sync_state);
   }
   else
   {
      RARCH_ERR(CSPFX "Uploading updated manifest failed.\n");
      sync_state->failures = true;
   }

   task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_END);
   task_cloud_sync_waiting_set(sync_state, 0);
}

static RFILE *task_cloud_sync_write_updated_manifest(file_list_t *manifest, char *path)
{
   rjsonwriter_t *writer = NULL;
   size_t         idx    = 0;
   RFILE *file           = filestream_open(path,
         RETRO_VFS_FILE_ACCESS_READ_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!file)
      return NULL;

   if (!(writer = rjsonwriter_open_filestream(file)))
   {
      filestream_close(file);
      return NULL;
   }

   /* since we may be transferring files at the same time,
    * the newly created manifest might be out of order */
   file_list_sort_on_alt(manifest);

   rjsonwriter_raw(writer, "[\n", 2);

   for (; idx < manifest->size; idx++)
   {
      struct item_file *item = &manifest->list[idx];

      if (idx)
         rjsonwriter_raw(writer, ",\n", 2);

      rjsonwriter_add_spaces(writer, 2);
      rjsonwriter_raw(writer, "{\n", 2);

      rjsonwriter_add_spaces(writer, 4);
      rjsonwriter_add_string(writer, "path");
      rjsonwriter_raw(writer, ": ", 2);
      rjsonwriter_add_string(writer, CS_FILE_KEY(item));
      rjsonwriter_raw(writer, ",\n", 2);

      rjsonwriter_add_spaces(writer, 4);
      rjsonwriter_add_string(writer, "hash");
      rjsonwriter_raw(writer, ": ", 2);
      rjsonwriter_add_string(writer, CS_FILE_HASH(item));
      rjsonwriter_raw(writer, "\n", 1);

      rjsonwriter_add_spaces(writer, 2);
      rjsonwriter_raw(writer, "}", 1);
   }

   rjsonwriter_raw(writer, "\n]\n", 3);

   /* rjsonwriter_free() performs the final flush, so its result is what
    * says whether the manifest was written completely.  Without it a
    * short write - a full disk, an I/O error - was reported as a
    * successful manifest, and the caller went on to upload it. */
   if (!rjsonwriter_free(writer))
   {
      RARCH_ERR(CSPFX "Failed to write \"%s\".\n", path);
      filestream_close(file);
      return NULL;
   }

   RARCH_LOG(CSPFX "Wrote \"%s\".\n", path);

   return file;
}

/**
 * task_cloud_sync_commit_local_manifest:
 *
 * Writes the "what this device had at the end of the last sync" record.
 *
 * Deliberately not called before the server manifest upload has
 * completed.  It used to be written first, unconditionally, and a
 * failed server upload then left the local manifest asserting hashes
 * the server had never been told about.  The next sync reads that as
 * local == current and local != server, concludes the server changed,
 * and fetches - so a transient upload failure could pull an older
 * server copy over a newer local file.  Leaving the local manifest
 * behind instead means the next sync re-examines the same files and
 * re-uploads, which is wasteful and safe rather than quiet and lossy.
 */
static void task_cloud_sync_commit_local_manifest(task_cloud_sync_state_t *sync_state)
{
   /* both paths in one heap block: the task runs on small stacks */
   char  *manifest_path = (char*)malloc(2 * PATH_MAX_LENGTH);
   char  *tmp_path      = manifest_path + PATH_MAX_LENGTH;
   RFILE *file;

   if (!manifest_path)
   {
      sync_state->failures = true;
      return;
   }

   task_cloud_sync_manifest_filename(manifest_path, PATH_MAX_LENGTH, false,
         sync_state->dir_core_assets);

   /* Write the new manifest beside the old one and rename it into
    * place.  Written in place, a crash, full disk or power cut part
    * way through left an empty or truncated manifest.local, which the
    * next sync can only throw away (see
    * task_cloud_sync_read_local_manifest()).  With the rename, the old
    * manifest stays intact until the new one is complete, and keeping
    * the old one is the safe outcome described above. */
   if (strlcpy(tmp_path, manifest_path, PATH_MAX_LENGTH) >= PATH_MAX_LENGTH
         || strlcat(tmp_path, ".tmp", PATH_MAX_LENGTH) >= PATH_MAX_LENGTH)
   {
      RARCH_ERR(CSPFX "Local manifest path too long.\n");
      sync_state->failures = true;
      free(manifest_path);
      return;
   }

   if ((file = task_cloud_sync_write_updated_manifest(
               sync_state->updated_local_manifest, tmp_path)))
   {
      filestream_close(file);
      if (filestream_rename(tmp_path, manifest_path) == 0)
      {
         task_cloud_sync_write_roots(sync_state);
         free(manifest_path);
         return;
      }
      RARCH_ERR(CSPFX "Failed to replace \"%s\" with \"%s\".\n",
            manifest_path, tmp_path);
   }
   else
      RARCH_ERR(CSPFX "Failed to write local manifest to \"%s\".\n", tmp_path);

   filestream_delete(tmp_path);
   sync_state->failures = true;
   free(manifest_path);
}

static void task_cloud_sync_update_manifests(task_cloud_sync_state_t *sync_state)
{
   char   manifest_path[PATH_MAX_LENGTH];
   RFILE *file   = NULL;

   /* The diff is over and nothing is in flight: the manifests are
    * read from here on, so they take what was added to them. */
   task_cloud_sync_fold_manifest_adds(sync_state);

   if (sync_state->need_manifest_uploaded)
   {
      RARCH_LOG(CSPFX "Uploading updated manifest to server...\n");
      task_cloud_sync_manifest_filename(manifest_path, sizeof(manifest_path), true,
            sync_state->dir_core_assets);
      file = task_cloud_sync_write_updated_manifest(sync_state->updated_server_manifest, manifest_path);
      /* task_cloud_sync_write_updated_manifest() returns NULL when the
       * manifest cannot be opened for writing or the json writer cannot
       * be attached to it. filestream_seek() dereferences the stream
       * without a NULL check, so bail out the same way a failed upload
       * does instead of crashing the task thread. */
      if (!file)
      {
         RARCH_ERR(CSPFX "Failed to write updated manifest to \"%s\".\n",
               manifest_path);
         sync_state->failures = true;
         task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_END);
         return;
      }
      filestream_seek(file, 0, SEEK_SET);
      task_cloud_sync_waiting_set(sync_state, 1);
      if (!cloud_sync_update(MANIFEST_FILENAME_SERVER, file, task_cloud_sync_update_manifest_cb, sync_state))
      {
         RARCH_ERR(CSPFX "Uploading updated manifest failed.\n");
         filestream_close(file);
         task_cloud_sync_waiting_set(sync_state, 0);
         sync_state->failures = true;
         /* Dispatch failed, so the callback will not run and the local
          * manifest is deliberately left unwritten. */
         task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_END);
      }
      return;
   }

   /* Nothing to tell the server, so nothing to wait for: the local
    * manifest is already true and can be written here. */
   task_cloud_sync_commit_local_manifest(sync_state);
   task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_END);
}

static void task_cloud_sync_end_handler(void *user_data, const char *path, bool success, RFILE *file)
{
   retro_task_t            *task       = (retro_task_t *)user_data;
   task_cloud_sync_state_t *sync_state = NULL;
   retro_time_t             end_time   = cpu_features_get_time_usec();

   if (!task)
      return;

   if ((sync_state = (task_cloud_sync_state_t *)task->state))
   {
      char title[128];
      if (sync_state->fatal_error)
      {
         strlcpy(title, sync_state->fatal_error, sizeof(title));
         task_set_error(task, strdup(sync_state->fatal_error));
      }
      else
      {
         size_t _len = strlcpy_lit(title, "Cloud Sync finished", sizeof(title));
         if (sync_state->failures || sync_state->conflicts)
            _len += strlcpy_lit(title + _len, " with ", sizeof(title) - _len);
         if (sync_state->failures)
            _len += strlcpy_lit(title + _len, "failures", sizeof(title) - _len);
         if (sync_state->failures && sync_state->conflicts)
            _len += strlcpy_lit(title + _len, " and ", sizeof(title) - _len);
         if (sync_state->conflicts)
            strlcpy_lit(title + _len, "conflicts", sizeof(title) - _len);
      }
      task_free_title(task);
      task_set_title(task, strdup(title));

      /* Inside the guard: every field below belongs to sync_state, and
       * the branch above exists precisely because task->state can be
       * NULL here. */
      RARCH_LOG(CSPFX "Finished after %lld.%06lld seconds, %d files uploaded, %d files downloaded.\n",
            (end_time - sync_state->start_time) / 1000 / 1000,
            (end_time - sync_state->start_time) % (1000 * 1000),
            sync_state->uploads, sync_state->downloads);
   }

   task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
}

static void task_cloud_sync_task_step(retro_task_t *task,
      task_cloud_sync_state_t *sync_state, nbio_budget_t *b)
{

   /* We can transfer more than one file at a time.  Both loads are
    * acquire: completion callbacks may transition `phase` and drop
    * `waiting` concurrently with this poll, and everything they
    * published beforehand is visible once the loads observe it. */
   if (task_cloud_sync_waiting_get(sync_state) >
         ((task_cloud_sync_phase_get(sync_state) == CLOUD_SYNC_PHASE_DIFF) ? 4 : 0))
   {
      task->when = cpu_features_get_time_usec() + 17 * 1000; /* 17ms */
      return;
   }

   if (task_get_flags(task) & RETRO_TASK_FLG_FINISHED)
       goto task_finished;

   switch (task_cloud_sync_phase_get(sync_state))
   {
      case CLOUD_SYNC_PHASE_BEGIN:
         task_cloud_sync_sweep_temps(sync_state);
         task_cloud_sync_waiting_set(sync_state, 1);
         if (!cloud_sync_begin(task_cloud_sync_begin_handler, task))
         {
            RARCH_WARN(CSPFX "Could not begin.\n");
            sync_state->failures = true;
            task_free_title(task);
            task_set_title(task, strdup("Cloud Sync failed"));
            goto task_finished;
         }
         break;
      case CLOUD_SYNC_PHASE_FETCH_SERVER_MANIFEST:
         task_cloud_sync_fetch_server_manifest(sync_state);
         break;
      case CLOUD_SYNC_PHASE_READ_LOCAL_MANIFEST:
         task_cloud_sync_read_local_manifest(sync_state);
         break;
      case CLOUD_SYNC_PHASE_BUILD_CURRENT_MANIFEST:
         task_cloud_sync_build_current_manifest(sync_state, b);
         break;
      case CLOUD_SYNC_PHASE_DIFF:
         {
            /* As many steps as the shared I/O window allows; a step
             * waits until its file is hashed, and no more than four
             * transfers are in flight. Each chunk and each step is one
             * unit of the window. */
            while (     task_cloud_sync_phase_get(sync_state) == CLOUD_SYNC_PHASE_DIFF
                     && task_cloud_sync_waiting_get(sync_state) <= 4
                     && task_cloud_sync_hash_current(sync_state, b)
                     && task_nbio_slice_within_budget(b, 0, 0))
               task_cloud_sync_diff_next(sync_state);
         }
         /* After the steps, not before: the step that finishes the diff
          * moves on to UPDATE_MANIFESTS, and a figure taken ahead of it
          * would be the last one the bar ever shows. */
         task_cloud_sync_update_progress(task);
         break;
      case CLOUD_SYNC_PHASE_UPDATE_MANIFESTS:
         task_cloud_sync_update_manifests(sync_state);
         break;
      case CLOUD_SYNC_PHASE_END:
         task_cloud_sync_waiting_set(sync_state, 1);
         if (!cloud_sync_end(task_cloud_sync_end_handler, task))
         {
            RARCH_WARN(CSPFX "Could not end?!\n");
            sync_state->failures = true;
            goto task_finished;
         }
         break;
   }

   return;

task_finished:
   task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
}

static void task_cloud_sync_task_handler(retro_task_t *task)
{
   task_cloud_sync_state_t *sync_state;
   nbio_budget_t            b;

   if (!task)
      return;
   if (!(sync_state = (task_cloud_sync_state_t *)task->state))
   {
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
      return;
   }

   /* One window per run, shared by the results of calls a blocking
    * driver finished on its worker, the downloads being hashed and the
    * diff; every run makes at least one unit of progress. The results'
    * handlers run here, on this thread, before the in-flight count is
    * read. */
   task_nbio_slice_open(&b);
   cloud_sync_poll(task_nbio_slice_within_budget, &b);
   task_cloud_sync_fetched_run(sync_state, &b);
#if defined(TCS_MOVE_WORKER)
   task_cloud_sync_moves_run(sync_state);
#endif
   task_cloud_sync_task_step(task, sync_state, &b);
   task_nbio_slice_close(&b);
}

/* Runs on the main thread before the state is released. A sync that
 * failed, or finished with failures or conflicts, reports its title -
 * the sync's own summary - as the error. */
/* A sync asked for while one ran, without a pusher waiting on it -
 * Sync Now, Resolve: Keep Local, Resolve: Keep Server, or a content
 * load's or close's sync: run once that one is through, with the
 * conflict resolution asked for last, a resolution winning over a
 * plain sync. -1: none. Turned away, the menu's choices did nothing
 * while an automatic sync ran. Frame thread only: every push and this
 * task's callback run there. */
static int tcs_followup_mode = -1;

/* Syncs set up that have not called back yet. A task finder cannot
 * answer this from inside another task's handler on the unthreaded
 * queue, where a content load runs. Frame thread only. */
static unsigned tcs_in_flight = 0;

static bool task_push_cloud_sync_with_mode(int conflict_resolution,
      retro_task_callback_t cb, void *cb_data);

static void task_cloud_sync_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   task_cloud_sync_state_t *sync_state = (task_cloud_sync_state_t *)task->state;

   if (sync_state && sync_state->cb)
   {
      if (!error && (sync_state->failures || sync_state->conflicts))
         error = task->title ? task->title : "Cloud Sync failed";
      sync_state->cb(task, NULL, sync_state->cb_data, error);
   }

   /* Whatever the result: the content's saves load from what is here,
    * once no other sync can change it. */
   if (tcs_in_flight)
      tcs_in_flight--;
   if (!tcs_in_flight)
      runloop_save_load_release(SAVE_LOAD_HOLD_CLOUD_SYNC);

   if (tcs_followup_mode >= 0)
   {
      int mode          = tcs_followup_mode;
      tcs_followup_mode = -1;
      task_push_cloud_sync_with_mode(mode, NULL, NULL);
   }
}

/* Releases the sync's state when the task retires. Every manifest owns
 * its entries' hashes: an entry added to the updated manifests is a
 * copy of the hash it was given. */
static void task_cloud_sync_cleanup(retro_task_t *task)
{
   task_cloud_sync_state_t *sync_state = (task_cloud_sync_state_t *)task->state;

   if (!sync_state)
      return;
   task->state = NULL;

   /* A sync that ended before its manifests were read: what was added
    * goes into them all the same, to be freed with them. */
   task_cloud_sync_fold_manifest_adds(sync_state);

   if (sync_state->server_manifest)
      file_list_free(sync_state->server_manifest);
   if (sync_state->local_manifest)
      file_list_free(sync_state->local_manifest);
   if (sync_state->current_manifest)
      file_list_free(sync_state->current_manifest);
   if (sync_state->updated_server_manifest)
      file_list_free(sync_state->updated_server_manifest);
   if (sync_state->updated_local_manifest)
      file_list_free(sync_state->updated_local_manifest);
   /* string_list_free() releases elems[i].userdata as well as
    * elems[i].data, so the strdup'd directories go with it. */
   if (sync_state->dirlist)
      string_list_free(sync_state->dirlist);
   string_list_free(sync_state->roots);
   string_list_free(sync_state->held);
   string_list_free(sync_state->held_dirs);
   task_cloud_sync_walk_free(sync_state);
   if (sync_state->hash_cur.file)
      filestream_close(sync_state->hash_cur.file);
   task_cloud_sync_fetched_take(sync_state);
   while (sync_state->fetched)
   {
      tcs_fetched_t *rec  = sync_state->fetched;
      sync_state->fetched = rec->next;
      filestream_close(rec->file);
      task_cloud_sync_fetch_discard(rec->fetch_state);
      free(rec->fetch_state);
      free(rec);
   }
#if defined(TCS_MOVE_WORKER)
   task_cloud_sync_moves_abandon(sync_state);
#endif
   free(sync_state->hash_buf);

   free(sync_state);
}

/* What a sync task runs and releases: the one place that says so. */
static void task_cloud_sync_task_setup(retro_task_t *task,
      task_cloud_sync_state_t *sync_state, const char *title)
{
   task->state       = sync_state;
   task->title       = strdup(title);
   task->handler     = task_cloud_sync_task_handler;
   task->callback    = task_cloud_sync_cb;
   task->cleanup     = task_cloud_sync_cleanup;
   task->progress_cb = task_window_progress_cb;
   /* Save ownership and local mutations share the content thread. */
   task->flags     |= RETRO_TASK_FLG_MAIN_THREAD;
   tcs_in_flight++;
}

static bool task_cloud_sync_task_finder(retro_task_t *task, void *user_data)
{
   if (!task)
      return false;

   /* there can be only one; a finished one, waiting for its callback,
    * is no longer running and starts the follow-up from there */
   return     task->handler == task_cloud_sync_task_handler
          && !(task_get_flags(task) & RETRO_TASK_FLG_FINISHED);
}

#if defined(HAVE_SMBCLIENT) || defined(HAVE_NFSCLIENT)
/* @s into @md5 with its separators one way and none at either end,
 * as a driver joins it into its paths */
static void task_cloud_sync_id_path(MD5_CTX *md5, const char *s)
{
   size_t len;
   size_t i;
   while (*s == '/' || *s == '\\')
      s++;
   len = strlen(s);
   while (len && (s[len - 1] == '/' || s[len - 1] == '\\'))
      len--;
   for (i = 0; i < len; i++)
      MD5_Update(md5, (s[i] == '\\') ? "/" : s + i, 1);
   MD5_Update(md5, "", 1);
}
#endif

/* @url into @md5 as network/cloud_sync/webdav.c takes it: http:// when
 * it names no scheme, and no trailing slash */
static void task_cloud_sync_id_url(MD5_CTX *md5, const char *url)
{
   size_t len = strlen(url);
   if (!strstr(url, "://"))
      MD5_Update(md5, "http://", STRLEN_CONST("http://"));
   while (len && url[len - 1] == '/')
      len--;
   MD5_Update(md5, url, (unsigned long)len);
   MD5_Update(md5, "", 1);
}

/* The server and account the sync talks to, as its driver addresses
 * them, hashed into @s (33 bytes): another one's files are not what
 * the last sync left. A username names the account; passwords, keys
 * and tokens are left out, so a new key or a new sign-in is the same
 * server. Google Drive and iCloud keep no account id to tell another
 * account by: the driver alone. */
static void task_cloud_sync_server_id(char *s, const settings_t *settings)
{
   const char *drv = settings->arrays.cloud_sync_driver;
   MD5_CTX     md5;

   MD5_Init(&md5);
   MD5_Update(&md5, drv, (unsigned long)strlen(drv) + 1);
   if (string_is_equal(drv, "webdav"))
   {
      task_cloud_sync_id_url(&md5, settings->arrays.webdav_url);
      MD5_Update(&md5, settings->arrays.webdav_username,
            (unsigned long)strlen(settings->arrays.webdav_username) + 1);
   }
#ifdef HAVE_S3
   else if (string_is_equal(drv, "s3"))
      task_cloud_sync_id_url(&md5, settings->arrays.s3_url);
#endif
#ifdef HAVE_SMBCLIENT
   else if (string_is_equal(drv, "smb"))
   {
      task_cloud_sync_id_path(&md5, settings->arrays.smb_client_server_address);
      task_cloud_sync_id_path(&md5, settings->arrays.smb_client_share);
      task_cloud_sync_id_path(&md5, settings->arrays.smb_client_subdir);
      MD5_Update(&md5, settings->arrays.smb_client_username,
            (unsigned long)strlen(settings->arrays.smb_client_username) + 1);
   }
#endif
#ifdef HAVE_NFSCLIENT
   else if (string_is_equal(drv, "nfs"))
   {
      task_cloud_sync_id_path(&md5, settings->arrays.nfs_server);
      task_cloud_sync_id_path(&md5, settings->arrays.nfs_export);
      /* as network/cloud_sync/nfs.c picks it */
      task_cloud_sync_id_path(&md5,
            string_is_empty(settings->arrays.cloud_sync_nfs_subdir)
            ? settings->arrays.nfs_subdir
            : settings->arrays.cloud_sync_nfs_subdir);
   }
#endif
   task_cloud_sync_md5_hex(s, &md5);
}

/* Set while a main config save or state folder is missing (main thread) */
static bool tcs_paused = false;

void task_cloud_sync_set_paused(bool paused)
{
   tcs_paused = paused;
}

static bool task_push_cloud_sync_with_mode(int conflict_resolution,
      retro_task_callback_t cb, void *cb_data)
{
   char task_title[128];
   task_finder_data_t       find_data;
   task_cloud_sync_state_t *sync_state = NULL;
   retro_task_t            *task       = NULL;
   bool cloud_sync_enable              = config_get_ptr()->bools.cloud_sync_enable;

   if (!cloud_sync_enable)
      return false;

   if (tcs_paused)
   {
      RARCH_WARN(CSPFX "Paused: the save or state folder is missing.\n");
      return false;
   }

#if defined(HAVE_THREADS) && !defined(TCS_LOCK_FREE)
   if (!tcs_manifest_lock)
      tcs_manifest_lock = slock_new();
#endif

   find_data.func = task_cloud_sync_task_finder;
   if (task_queue_find(&find_data))
   {
      /* A pusher waiting on its own sync is told it was not run */
      if (cb)
      {
         RARCH_LOG(CSPFX "Already in progress.\n");
         return false;
      }
      if (conflict_resolution || tcs_followup_mode < 0)
         tcs_followup_mode = conflict_resolution;
      RARCH_LOG(CSPFX "In progress: another sync follows it.\n");
      return true;
   }

   sync_state = (task_cloud_sync_state_t *)calloc(1, sizeof(task_cloud_sync_state_t));
   if (!sync_state)
      return false;

   /* Keep this sync's settings across subsequent frames. */
   {
      settings_t *settings    = config_get_ptr();
      sync_state->destructive = settings->bools.cloud_sync_destructive;
      strlcpy(sync_state->dir_core_assets,
            settings->paths.directory_core_assets,
            sizeof(sync_state->dir_core_assets));
      task_cloud_sync_server_id(sync_state->server_id, settings);
      sync_state->dirlist     = task_cloud_sync_directory_map_new(settings);
   }
   cloud_sync_capture();
   if (!sync_state->dirlist)
   {
      free(sync_state);
      return false;
   }

   if (!(task = task_init()))
   {
      string_list_free(sync_state->dirlist);
      free(sync_state);
      return false;
   }

   /* calloc zero-fill is not a portable initializer for an atomic
    * (illegal formally under C11 stdatomic, and a class type on the
    * C++11 backend) - initialize it explicitly. */
   retro_atomic_int_init(&sync_state->waiting, 0);
   retro_atomic_int_init(&sync_state->phase, (int)CLOUD_SYNC_PHASE_BEGIN);

   task_cloud_sync_phase_set(sync_state, CLOUD_SYNC_PHASE_BEGIN);
   sync_state->start_time          = cpu_features_get_time_usec();
   sync_state->conflict_resolution = conflict_resolution;
   sync_state->cb                  = cb;
   sync_state->cb_data             = cb_data;

   strlcpy_lit(task_title, "Cloud Sync in progress", sizeof(task_title));

   task_cloud_sync_task_setup(task, sync_state, task_title);

   task_queue_push(task);
   return true;
}

bool task_cloud_sync_in_progress(void)
{
   return tcs_in_flight > 0;
}

bool task_push_cloud_sync(retro_task_callback_t cb, void *cb_data)
{
   return task_push_cloud_sync_with_mode(0, cb, cb_data);
}

void task_push_cloud_sync_update_driver(void)
{
   char manifest_path[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();

   cloud_sync_find_driver(settings->arrays.cloud_sync_driver,
         "cloud sync driver", verbosity_is_enabled());

   /* The sync does a three-way diff: current local <- last sync -> current server.
    * When the server changes it becomes a four way diff, which can lead to odd
    * conflicts or data loss. The easiest way to resolve it is to reset the last sync
    */
   task_cloud_sync_manifest_filename(manifest_path, sizeof(manifest_path), false,
         settings->paths.directory_core_assets);
   filestream_delete(manifest_path);
}

void task_push_cloud_sync_resolve_keep_local(void)
{
   RARCH_LOG(CSPFX "Starting sync with conflict resolution: keep local.\n");
   task_push_cloud_sync_with_mode(1, NULL, NULL);
}

void task_push_cloud_sync_resolve_keep_server(void)
{
   RARCH_LOG(CSPFX "Starting sync with conflict resolution: keep server.\n");
   task_push_cloud_sync_with_mode(2, NULL, NULL);
}

void task_push_cloud_sync_upload_all(void)
{
   RARCH_LOG(CSPFX "Starting sync uploading every file the server lost.\n");
   task_push_cloud_sync_with_mode(3, NULL, NULL);
}
