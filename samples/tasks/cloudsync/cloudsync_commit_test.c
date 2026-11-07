/* The real cloud-sync task, queued with threaded tasks on and off.
 * Downloads arrive on another thread. Save ownership and file commits
 * must stay on the content thread, including a failed hand-over.
 * A backup's copy across drives runs on a thread of its own, and the
 * sync never waits for it. The walk leaves out the safety copies and
 * recovery files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rthreads/rthreads.h>
#include <retro_timers.h>
#include <vfs/vfs_implementation.h>

static int fail_alloc;
static void *test_malloc(size_t size)
{
   if (fail_alloc)
   {
      fail_alloc = 0;
      return NULL;
   }
   return malloc(size);
}

#define malloc test_malloc
#include "../../../tasks/task_cloudsync.c"
#undef malloc

static const char *directory = "cloudsync_commit_test_files";
static int hold_mode;
static char local_path[128], fetching_path[160];
static retro_atomic_int_t off_main, ownership_checks;
static int live, retired, failures, expect_old, expect_failure;
static void *pending_fetch;
/* 1: moving the save into its backups fails, as across drives;
 * 2: deleting it fails too; 3: the delete removes it but reports a
 * failure, as a network drive can; 4: the copy fails, a full card */
static retro_atomic_int_t refuse_move, refused;
/* syncs of a backup, and how many came before the save was deleted */
static retro_atomic_int_t backup_syncs, syncs_at_delete;
/* syncs of a backup on the main thread; copies held at their sync
 * while gate is set; the largest read off the main thread */
static retro_atomic_int_t main_syncs, gate, at_gate, max_piece;
/* deletes of a backup off the main thread */
static retro_atomic_int_t worker_deletes;
static unsigned lifetime, writes, polls;
static bool state_writing;
/* lane(): 1 the copy is held at its sync; 2 and content loads
 * meanwhile; 3 the sync is released meanwhile; 4 released once the
 * copy is done, before the sync sees it; 5 held, with a second key;
 * 6 held while the frontend writes a state; 7 while the core rewrites
 * the save, the same size; 8 a state write still in flight; 9 the core
 * rewrites it at another size, its mtime put back */
static char z_path[128];
/* lane(): the save is deleted once its copy has read it; vanishing is
 * 1 until then, 2 after */
static bool vanish;
static retro_atomic_int_t vanishing;
/* what the last sync recorded for the save */
static char synced_server[33], synced_local[33];

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d (hold %d): %s\n", __LINE__, hold_mode, #c); \
   failures++; } } while (0)

void RARCH_LOG(const char *fmt, ...) { (void)fmt; }
void RARCH_WARN(const char *fmt, ...) { (void)fmt; }
void RARCH_ERR(const char *fmt, ...) { (void)fmt; }
bool verbosity_is_enabled(void) { return false; }
const char *msg_hash_to_str(enum msg_hash_enums msg) { (void)msg; return ""; }
void runloop_msg_queue_push(const char *msg, size_t len, unsigned prio,
      unsigned duration, bool flush, char *title,
      enum message_queue_icon icon, enum message_queue_category category)
{
   (void)msg; (void)len; (void)prio; (void)duration; (void)flush;
   (void)title; (void)icon; (void)category;
}
settings_t *config_get_ptr(void) { return NULL; }
char *dir_get_ptr(enum rarch_dir_type type) { (void)type; return NULL; }
int __real_filestream_rename(const char *old_path, const char *new_path);
int __wrap_filestream_rename(const char *old_path, const char *new_path)
{
   if (string_is_equal(old_path, local_path) && !task_is_on_main_thread())
      retro_atomic_inc_int(&off_main);
   if (     retro_atomic_load_relaxed_int(&refuse_move)
         && string_is_equal(old_path, local_path))
   {
      retro_atomic_inc_int(&refused);
      return -1;
   }
   return __real_filestream_rename(old_path, new_path);
}
int __real_filestream_delete(const char *path);
int __wrap_filestream_delete(const char *path)
{
   if (string_is_equal(path, local_path) && !task_is_on_main_thread())
      retro_atomic_inc_int(&off_main);
   if (     strstr(path, SAVE_BACKUPS_CORE_CLOUD_SYNC)
         && !task_is_on_main_thread())
      retro_atomic_inc_int(&worker_deletes);
   if (     string_is_equal(path, local_path)
         && retro_atomic_load_relaxed_int(&syncs_at_delete) < 0)
      retro_atomic_store_relaxed_int(&syncs_at_delete,
            retro_atomic_load_relaxed_int(&backup_syncs));
   if (     (   retro_atomic_load_relaxed_int(&refuse_move) == 2
             || retro_atomic_load_relaxed_int(&refuse_move) == 3)
         && string_is_equal(path, local_path))
   {
      if (retro_atomic_load_relaxed_int(&refuse_move) == 3)
         __real_filestream_delete(path);
      return -1;
   }
   return __real_filestream_delete(path);
}
int __real_retro_vfs_file_sync_impl(libretro_vfs_implementation_file *stream);
int __wrap_retro_vfs_file_sync_impl(libretro_vfs_implementation_file *stream)
{
   if (     stream && stream->orig_path
         && strstr(stream->orig_path, SAVE_BACKUPS_CORE_CLOUD_SYNC))
   {
      retro_atomic_inc_int(&backup_syncs);
      if (task_is_on_main_thread())
         retro_atomic_inc_int(&main_syncs);
      else
      {
         /* a slow card */
         retro_atomic_inc_int(&at_gate);
         while (retro_atomic_load_acquire_int(&gate))
            retro_sleep(1);
         retro_atomic_dec_int(&at_gate);
      }
      if (retro_atomic_load_relaxed_int(&refuse_move) == 4)
         return -1;
   }
   return __real_retro_vfs_file_sync_impl(stream);
}
int __real_filestream_close(RFILE *stream);
int __wrap_filestream_close(RFILE *stream)
{
   bool save = string_is_equal(filestream_get_path(stream), local_path);
   int ret   = __real_filestream_close(stream);
   if (save && retro_atomic_load_acquire_int(&vanishing) == 1)
   {
      __real_filestream_delete(local_path);
      retro_atomic_store_release_int(&vanishing, 2);
   }
   return ret;
}
int64_t __real_filestream_read(RFILE *stream, void *data, int64_t len);
int64_t __wrap_filestream_read(RFILE *stream, void *data, int64_t len)
{
   if (     !task_is_on_main_thread()
         && len > retro_atomic_load_relaxed_int(&max_piece))
      retro_atomic_store_relaxed_int(&max_piece, (int)len);
   return __real_filestream_read(stream, data, len);
}
bool content_savefile_is_live(const char *path)
{
   retro_atomic_inc_int(&ownership_checks);
   if (!task_is_on_main_thread())
      retro_atomic_inc_int(&off_main);
   return live && !strcmp(path, local_path);
}
unsigned content_savefile_lifetime(void) { return lifetime; }
unsigned content_save_writes(void) { return writes; }
bool content_save_state_in_progress(void *data)
{ (void)data; return state_writing; }
void task_window_progress_cb(retro_task_t *task) { (void)task; }
size_t fill_pathname_application_special(char *s, size_t len,
      enum application_special_type type)
{ (void)s; (void)len; (void)type; return 0; }
void cloud_sync_find_driver(const char *drv, const char *prefix, bool verbose)
{ (void)drv; (void)prefix; (void)verbose; }
void cloud_sync_capture(void) { }
bool cloud_sync_begin(cloud_sync_complete_handler_t cb, void *ud)
{ (void)cb; (void)ud; return false; }
bool cloud_sync_end(cloud_sync_complete_handler_t cb, void *ud)
{ cb(ud, NULL, true, NULL); return true; }
bool cloud_sync_read(const char *path, const char *file,
      cloud_sync_complete_handler_t cb, void *ud)
{ (void)path; (void)file; (void)cb; pending_fetch = ud; return true; }
bool cloud_sync_update(const char *path, RFILE *file,
      cloud_sync_complete_handler_t cb, void *ud)
{ (void)path; (void)file; (void)cb; (void)ud; return false; }
bool cloud_sync_free(const char *path, cloud_sync_complete_handler_t cb, void *ud)
{ (void)path; (void)cb; (void)ud; return false; }
void cloud_sync_poll(cloud_sync_poll_budget_t within, void *budget)
{ (void)within; (void)budget; polls++; }
void task_nbio_slice_open(nbio_budget_t *b) { b->floor = 1; }
void task_nbio_slice_close(nbio_budget_t *b) { (void)b; }
bool task_nbio_slice_within_budget(void *ud, size_t avail, size_t len)
{ (void)ud; (void)avail; (void)len; return true; }

static void add_key(file_list_t *list, const char *path, const char *hash,
      const char *key)
{
   size_t at = list->size;
   file_list_append(list, path, NULL, 0, 0, 0);
   file_list_set_alt_at_offset(list, at, key);
   list->list[at].userdata = hash ? strdup(hash) : NULL;
}

static void add(file_list_t *list, const char *path, const char *hash)
{
   add_key(list, path, hash, "saves/save.srm");
}

static void synced_hash(file_list_t *list, char *s, size_t len)
{
   size_t at;
   *s = '\0';
   if (     file_list_search(list, "saves/save.srm", &at)
         && CS_FILE_HASH(&list->list[at]))
      strlcpy(s, CS_FILE_HASH(&list->list[at]), len);
}

static bool holds(const char *path, const char *text)
{
   char buf[16];
   RFILE *f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   int64_t n = f ? filestream_read(f, buf, sizeof(buf)) : -1;
   if (f)
      filestream_close(f);
   return n == (int64_t)strlen(text) && !memcmp(buf, text, strlen(text));
}

static void done(retro_task_t *task, void *data, void *ud, const char *error)
{
   task_cloud_sync_state_t *st = (task_cloud_sync_state_t*)task->state;
   size_t at;
   (void)data; (void)ud;
   CHECK(task_is_on_main_thread());
   CHECK((error != NULL) == expect_failure);
   task_cloud_sync_fold_manifest_adds(st);
   synced_hash(st->updated_server_manifest, synced_server,
         sizeof(synced_server));
   synced_hash(st->updated_local_manifest, synced_local,
         sizeof(synced_local));
   if (expect_old)
   {
      CHECK(file_list_search(st->updated_local_manifest, "saves/save.srm", &at));
      if (file_list_search(st->updated_local_manifest, "saves/save.srm", &at))
         CHECK(string_is_equal(CS_FILE_HASH(&st->updated_local_manifest->list[at]), "old"));
   }
   retired++;
}

static void complete_fetch(void *data)
{
   task_cloud_sync_fetch_state_t *fetch = (task_cloud_sync_fetch_state_t*)data;
   RFILE *f = filestream_open(fetching_path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   CHECK(f != NULL);
   task_cloud_sync_fetch_cb(fetch, "saves/save.srm", true, f);
}

/* After a save vanished during its copy. Found gone by the copy's own
 * thread, the server's change is a conflict at the next sync, the key
 * left out of the local record; the sync after that, or one keeping
 * the server's, fetches it. The inline move just took it. */
static void vanished_next_sync(void)
{
#if defined(TCS_MOVE_WORKER)
   int n;
   CHECK(string_is_equal(synced_server, "new"));
   CHECK(string_is_equal(synced_local, "old"));
   for (n = 0; n < 3; n++)
   {
      retro_task_t *task = task_init();
      task_cloud_sync_state_t *st = (task_cloud_sync_state_t*)calloc(1, sizeof(*st));
      union string_list_elem_attr attr;
      size_t at;
      attr.i = 0;
      st->server_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
      st->local_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
      st->current_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
      st->updated_server_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
      st->updated_local_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
      st->dirlist = string_list_new();
      string_list_append(st->dirlist, "saves", attr);
      st->dirlist->elems[0].userdata = strdup(directory);
      st->conflict_resolution = n == 2 ? 2 : 0;
      add(st->server_manifest, NULL, synced_server);
      if (n != 1)
         add(st->local_manifest, NULL, synced_local);
      retro_atomic_int_init(&st->waiting, 0);
      retro_atomic_int_init(&st->phase, CLOUD_SYNC_PHASE_DIFF);
      task_cloud_sync_task_setup(task, st, "Cloud Sync");
      pending_fetch = NULL;
      task_cloud_sync_diff_next(st);
      task_cloud_sync_fold_manifest_adds(st);
      CHECK(st->conflicts == (n == 0));
      CHECK((pending_fetch != NULL) == (n != 0));
      CHECK(n || (file_list_search(st->updated_server_manifest,
                  "saves/save.srm", &at)
            && string_is_equal(CS_FILE_HASH(
                  &st->updated_server_manifest->list[at]), "new")));
      CHECK(!file_list_search(st->updated_local_manifest, "saves/save.srm",
               &at));
      free(pending_fetch);
      pending_fetch = NULL;
      task_cloud_sync_cleanup(task);
      free(task->title);
      free(task);
   }
#else
   /* both record the download's own hash */
   CHECK(!string_is_empty(synced_local));
   CHECK(!string_is_equal(synced_local, "old"));
   CHECK(string_is_equal(synced_server, synced_local));
#endif
}

static void lane(bool threaded, bool deletion, bool owned, bool no_memory,
      bool destructive)
{
   retro_task_t *task = task_init();
   task_cloud_sync_state_t *st = (task_cloud_sync_state_t*)calloc(1, sizeof(*st));
   unsigned i;
   char manifest[160], backup_dir[160];
   struct string_list *backups;
   union string_list_elem_attr attr;
   int mode = retro_atomic_load_relaxed_int(&refuse_move);
   /* a save that can't be deleted, or copied, is kept */
   bool keep = mode == 2 || mode == 4;
   /* the copy undone, the save kept, and no failure */
   bool undone = hold_mode >= 2 && hold_mode != 5;
   unsigned polls_at_hold = 0;
#if defined(TCS_MOVE_WORKER)
   /* gone by the copy's end, the save is not moved */
   undone = undone || vanish;
#endif
   retired = 0;
   live = 0;
   expect_old = owned || no_memory || keep || undone;
   expect_failure = no_memory || keep;
   retro_atomic_store_relaxed_int(&main_syncs, 0);
   retro_atomic_store_release_int(&gate, hold_mode != 0);
   retro_atomic_store_relaxed_int(&off_main, 0);
   retro_atomic_store_relaxed_int(&ownership_checks, 0);
   retro_atomic_store_relaxed_int(&backup_syncs, 0);
   retro_atomic_store_relaxed_int(&syncs_at_delete, -1);
   retro_atomic_store_relaxed_int(&worker_deletes, 0);
   state_writing = false;
   CHECK(filestream_write_file(local_path, "old", 3));
   if (hold_mode == 7 || hold_mode == 9)
      CHECK(path_set_mtime(local_path, 1000000000));
   st->server_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->local_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->current_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->updated_server_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->updated_local_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->destructive = destructive;
   st->dirlist = string_list_new();
   attr.i = 0;
   string_list_append(st->dirlist, "saves", attr);
   st->dirlist->elems[0].userdata = strdup(directory);
   snprintf(st->dir_core_assets, sizeof(st->dir_core_assets), "%s/assets",
         directory);
   CHECK(path_mkdir(st->dir_core_assets));
   add(st->server_manifest, NULL, deletion ? NULL : "new");
   add(st->local_manifest, NULL, "old");
   add(st->current_manifest, local_path, "old");
   if (hold_mode == 5)
   {
      /* after the first key, which waits on its copy meanwhile */
      CHECK(filestream_write_file(z_path, "zzz", 3));
      add_key(st->server_manifest, NULL, NULL, "saves/z.srm");
      add_key(st->local_manifest, NULL, "zzz", "saves/z.srm");
      add_key(st->current_manifest, z_path, "zzz", "saves/z.srm");
   }
   retro_atomic_int_init(&st->waiting, 0);
   retro_atomic_int_init(&st->phase, deletion ? CLOUD_SYNC_PHASE_DIFF : CLOUD_SYNC_PHASE_END);
   st->cb = done;
   task_cloud_sync_task_setup(task, st, "Cloud Sync");
   task_queue_init(threaded, NULL);
   if (!deletion)
   {
      sthread_t *worker;
      pending_fetch = NULL;
      task_cloud_sync_fetch_server_file(st);
      CHECK(pending_fetch != NULL);
      CHECK(holds(local_path, "old"));
      CHECK(filestream_write_file(fetching_path, "new", 3));
      fail_alloc = no_memory;
#ifdef HAVE_THREADS
      worker = sthread_create(complete_fetch, pending_fetch);
      CHECK(worker != NULL);
      if (worker)
         sthread_join(worker);
#else
      (void)worker;
      complete_fetch(pending_fetch);
#endif
      CHECK(holds(local_path, "old"));
   }
   /* A core takes the save after the transfer, before its commit. */
   live = owned;
   retro_atomic_store_release_int(&vanishing, vanish);
   snprintf(manifest, sizeof(manifest), "%s/assets/manifest.local", directory);
#if defined(TCS_MOVE_WORKER)
   if (hold_mode == 3 || hold_mode == 4)
   {
      /* run here, not queued: the sync is released under the copy */
      for (i = 0; i < 2000 && !retro_atomic_load_acquire_int(&at_gate); i++)
      {
         task_cloud_sync_task_handler(task);
         retro_sleep(1);
      }
      CHECK(retro_atomic_load_acquire_int(&at_gate) == 1);
      if (hold_mode == 4 && st->moves)
      {
         tcs_move_t *mv = st->moves;
         retro_atomic_store_release_int(&gate, 0);
         for (i = 0; i < 2000
               && retro_atomic_load_acquire_int(&mv->state) != TCS_MOVE_DONE; i++)
            retro_sleep(1);
         CHECK(retro_atomic_load_acquire_int(&mv->state) == TCS_MOVE_DONE);
      }
      /* returns at once, the copy still held */
      task_cloud_sync_cleanup(task);
      CHECK(holds(local_path, "old"));
      retro_atomic_store_release_int(&gate, 0);
      /* hold 3: the copy's own thread deletes it */
      for (i = 0; hold_mode == 3 && i < 2000
            && !retro_atomic_load_acquire_int(&worker_deletes); i++)
         retro_sleep(1);
      CHECK(retro_atomic_load_acquire_int(&worker_deletes) == (hold_mode == 3));
      free(task->title);
      free(task);
      CHECK(retired == 0);
   }
   else
#endif
   {
      task_queue_push(task);
      for (i = 0; i < 2000 && !retired; i++)
      {
         task_queue_check();
         if (     hold_mode
               && retro_atomic_load_acquire_int(&gate)
               && retro_atomic_load_acquire_int(&at_gate))
         {
            if (!polls_at_hold)
               polls_at_hold = polls;
            else if (polls - polls_at_hold >= 3)
            {
               /* the sync went on, never waiting for the copy */
               CHECK(holds(local_path, "old"));
               CHECK(retired == 0);
               CHECK(deletion || path_is_valid(fetching_path));
               CHECK(!path_is_valid(manifest));
               if (hold_mode == 5)
                  CHECK(!path_is_valid(z_path));
               if (hold_mode == 2)
                  lifetime++;
               else if (hold_mode == 6)
                  writes++;
               else if (hold_mode == 7)
                  CHECK(filestream_write_file(local_path, "cor", 3));
               else if (hold_mode == 8)
                  state_writing = true;
               else if (hold_mode == 9)
               {
                  CHECK(filestream_write_file(local_path, "core", 4));
                  CHECK(path_set_mtime(local_path, 1000000000));
               }
               retro_atomic_store_release_int(&gate, 0);
            }
         }
         retro_sleep(1);
      }
      CHECK(retired == 1);
      CHECK(!hold_mode || (polls_at_hold && polls - polls_at_hold >= 3));
   }
   CHECK(retro_atomic_load_acquire_int(&vanishing) == (vanish ? 2 : 0));
   retro_atomic_store_relaxed_int(&vanishing, 0);
   CHECK(retro_atomic_load_relaxed_int(&off_main) == 0);
   if (!no_memory)
      CHECK(retro_atomic_load_relaxed_int(&ownership_checks) > 0);
   /* a rename is not synced; a copy is, before the save goes, and off
    * the main thread where it can be */
   if (!mode)
      CHECK(retro_atomic_load_relaxed_int(&backup_syncs) == 0);
   else if (undone || mode == 4)
      CHECK(retro_atomic_load_relaxed_int(&syncs_at_delete) < 0);
   else
      CHECK(retro_atomic_load_relaxed_int(&syncs_at_delete) > 0);
#if defined(TCS_MOVE_WORKER)
   CHECK(retro_atomic_load_relaxed_int(&main_syncs) == 0);
#else
   CHECK(!mode || retro_atomic_load_relaxed_int(&main_syncs) > 0);
#endif
   if (expect_old && vanish)
      CHECK(!path_is_valid(local_path));
   else if (expect_old)
      CHECK(holds(local_path, hold_mode == 7 ? "cor"
               : hold_mode == 9 ? "core" : "old"));
   else if (!deletion)
      CHECK(holds(local_path, "new"));
   else
      CHECK(!path_is_valid(local_path));
   CHECK(!path_is_valid(fetching_path));
   snprintf(backup_dir, sizeof(backup_dir), "%s/backups/cloud-sync/saves", directory);
   backups = dir_list_new(backup_dir, NULL, false, false, true, true);
   if (!destructive && (!expect_old || vanish))
   {
      CHECK(backups && backups->size == (hold_mode == 5 ? 2u : 1u));
      if (backups && backups->size)
      {
         struct save_backup_name name;
         /* the save's before z's */
         dir_list_sort(backups, true);
         CHECK(holds(backups->elems[0].data, "old"));
         CHECK(save_backups_parse(path_basename(backups->elems[0].data), &name)
               && name.reason == (deletion
                  ? SAVE_BACKUP_DELETED : SAVE_BACKUP_REPLACED));
      }
   }
   else
      CHECK(!backups || backups->size == 0);
   snprintf(backup_dir, sizeof(backup_dir), "%s/assets/cloud_backups", directory);
   CHECK(!path_is_valid(backup_dir));
   if (backups)
   {
      /* left in place, copies would pile up over the mode's syncs */
      if (!keep)
         for (i = 0; i < backups->size; i++)
         {
            CHECK(!string_ends_with(backups->elems[i].data, CS_TEMP_SUFFIX));
            filestream_delete(backups->elems[i].data);
         }
      string_list_free(backups);
   }
   if (vanish && !deletion)
      vanished_next_sync();
   task_queue_deinit();
   filestream_delete(local_path);
   filestream_delete(z_path);
   filestream_delete(manifest);
   snprintf(manifest, sizeof(manifest), "%s/assets/manifest.local.roots",
         directory);
   filestream_delete(manifest);
}

/* Two backups of one key in one second are told apart by number; with
 * no synced save folder they go under the core assets' cloud_backups. */
static void backup_location_lane(void)
{
   task_cloud_sync_state_t *st = (task_cloud_sync_state_t*)calloc(1, sizeof(*st));
   struct item_file item = {0};
   union string_list_elem_attr attr;
   struct string_list *list = NULL;
   struct save_backup_name a, b;
   char dir[256];
   char *owed = NULL;
   unsigned i, tries;
   attr.i    = 0;
   item.alt  = (char*)"saves/Snes9x/save.srm";
   item.path = local_path;
   st->dirlist = string_list_new();
   string_list_append(st->dirlist, "saves", attr);
   st->dirlist->elems[0].userdata = strdup(directory);
   snprintf(st->dir_core_assets, sizeof(st->dir_core_assets), "%s/assets",
         directory);
   snprintf(dir, sizeof(dir), "%s/backups/cloud-sync/saves/Snes9x", directory);
   for (tries = 0; tries < 3; tries++)
   {
      time_t start = time(NULL);
      for (i = 0; i < 2; i++)
      {
         CHECK(filestream_write_file(local_path, i ? "two" : "one", 3));
         CHECK(task_cloud_sync_backup_file(st, &item, SAVE_BACKUP_REPLACED,
                  &owed) == SAVE_BACKUP_MADE);
         CHECK(!path_is_valid(local_path));
      }
      list = dir_list_new(dir, NULL, false, false, false, false);
      if (time(NULL) == start)
         break;
      /* the clock turned between the two: again */
      for (i = 0; list && i < list->size; i++)
         filestream_delete(list->elems[i].data);
      string_list_free(list);
      list = NULL;
   }
   CHECK(list && list->size == 2);
   if (list && list->size == 2)
   {
      dir_list_sort(list, true);
      CHECK(save_backups_parse(path_basename(list->elems[0].data), &a));
      CHECK(save_backups_parse(path_basename(list->elems[1].data), &b));
      CHECK(a.reason == SAVE_BACKUP_REPLACED && b.reason == SAVE_BACKUP_REPLACED);
      CHECK(!memcmp(a.time, b.time, sizeof(a.time)));
      CHECK(a.number == 1 && b.number == 2);
      CHECK(!strncmp(path_basename(list->elems[0].data) + a.core_off,
               "cloud-sync", a.core_len) && a.core_len == 10);
      CHECK(holds(list->elems[0].data, "one"));
      CHECK(holds(list->elems[1].data, "two"));
   }
   for (i = 0; list && i < list->size; i++)
      filestream_delete(list->elems[i].data);
   string_list_free(list);

   string_list_free(st->dirlist);
   st->dirlist = string_list_new();
   CHECK(filestream_write_file(local_path, "three", 5));
   CHECK(task_cloud_sync_backup_file(st, &item, SAVE_BACKUP_DELETED, &owed)
         == SAVE_BACKUP_MADE);
   snprintf(dir, sizeof(dir), "%s/assets/cloud_backups/saves/Snes9x", directory);
   list = dir_list_new(dir, NULL, false, false, false, false);
   CHECK(list && list->size == 1);
   if (list && list->size == 1)
   {
      CHECK(save_backups_parse(path_basename(list->elems[0].data), &a)
            && a.reason == SAVE_BACKUP_DELETED);
      CHECK(holds(list->elems[0].data, "three"));
      filestream_delete(list->elems[0].data);
   }
   string_list_free(list);
   path_rmdir(dir);
   snprintf(dir, sizeof(dir), "%s/assets/cloud_backups/saves", directory);
   path_rmdir(dir);
   snprintf(dir, sizeof(dir), "%s/assets/cloud_backups", directory);
   path_rmdir(dir);
   snprintf(dir, sizeof(dir), "%s/assets", directory);
   path_rmdir(dir);
   string_list_free(st->dirlist);
   free(st);
}

#if defined(TCS_MOVE_WORKER)
/* What the sync holds in flight never reaches the copies' budget, but
 * for one big copy alone. */
static void budget_check(task_cloud_sync_state_t *st, unsigned *most)
{
   tcs_move_t *mv;
   unsigned n    = 0;
   int64_t bytes = 0;
   bool big      = false;
   for (mv = st->moves; mv; mv = mv->next)
      if (retro_atomic_load_acquire_int(&mv->state) != TCS_MOVE_QUEUED)
      {
         n++;
         bytes += mv->size;
         big   |= mv->size >= TCS_MOVE_BUDGET;
      }
   CHECK(n < 4);
   CHECK(big ? n == 1 : bytes < 16 << 20);
   if (n > *most)
      *most = n;
}

/* Copies run side by side within the budget, each a piece at a time. */
static void budget_lane(const int64_t *sizes, unsigned count,
      unsigned held_most)
{
   retro_task_t *task = task_init();
   task_cloud_sync_state_t *st = (task_cloud_sync_state_t*)calloc(1, sizeof(*st));
   struct item_file server = {0};
   char path[160], to[200], dir[160];
   unsigned i, most = 0;
   uint8_t *buf = (uint8_t*)calloc(1, (size_t)sizes[0] > 1 ? 32 << 20 : 1);
   st->updated_server_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->updated_local_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
   retro_atomic_int_init(&st->waiting, 0);
   retro_atomic_int_init(&st->phase, CLOUD_SYNC_PHASE_DIFF);
   task_cloud_sync_task_setup(task, st, "Cloud Sync");
   server.alt = (char*)"saves/b.srm";
   snprintf(dir, sizeof(dir), "%s/backups/cloud-sync/budget", directory);
   CHECK(path_mkdir(dir));
   CHECK(buf != NULL);
   retro_atomic_store_relaxed_int(&max_piece, 0);
   retro_atomic_store_release_int(&gate, 1);
   for (i = 0; buf && i < count; i++)
   {
      snprintf(path, sizeof(path), "%s/b%u.srm", directory, i);
      snprintf(to, sizeof(to), "%s/b%u.srm", dir, i);
      CHECK(filestream_write_file(path, buf, sizes[i]));
      CHECK(task_cloud_sync_move_push(st, NULL, &server, "saves/b.srm",
               path, to, NULL, NULL));
      budget_check(st, &most);
   }
   CHECK(most == held_most);
   retro_atomic_store_release_int(&gate, 0);
   for (i = 0; i < 20000 && st->moves; i++)
   {
      task_cloud_sync_moves_run(st);
      budget_check(st, &most);
      retro_sleep(1);
   }
   CHECK(!st->moves);
   CHECK(task_cloud_sync_waiting_get(st) == 0);
   /* no file read whole, however big */
   CHECK(retro_atomic_load_relaxed_int(&max_piece) == 1024 * 1024);
   for (i = 0; i < count; i++)
   {
      snprintf(path, sizeof(path), "%s/b%u.srm", directory, i);
      snprintf(to, sizeof(to), "%s/b%u.srm", dir, i);
      CHECK(!path_is_valid(path));
      CHECK(path_get_size(to) == sizes[i]);
      filestream_delete(to);
   }
   path_rmdir(dir);
   task_cloud_sync_cleanup(task);
   free(task->title);
   free(task);
   free(buf);
}
#endif

/* The temporary files of copies that never finished are swept at the
 * first sync of the process, which this is, and only those. No later
 * sync sweeps, so none sweeps the temporary file of a copy in flight. */
static void sweep_lane(void)
{
   static const char *kept[] = {
      "save - 2026-11-07 10-00-00 - cloud-sync - replaced.srm",
      "notes.tmp",
      "save - 2026-11-07 10-00-00 - Snes9x - replaced.srm.tmp"
   };
   task_cloud_sync_state_t *st = (task_cloud_sync_state_t*)calloc(1, sizeof(*st));
   union string_list_elem_attr attr;
   char dir[160], stale[256], path[256];
   unsigned i;
#if defined(TCS_MOVE_WORKER)
   retro_task_t *task = task_init();
   struct item_file server = {0};
   char to[256], temp[260];
#endif
   attr.i = 0;
   st->dirlist = string_list_new();
   string_list_append(st->dirlist, "saves", attr);
   st->dirlist->elems[0].userdata = strdup(directory);
   snprintf(dir, sizeof(dir), "%s/backups/cloud-sync/saves", directory);
   CHECK(path_mkdir(dir));
   snprintf(stale, sizeof(stale),
         "%s/save - 2026-11-07 09-00-00-2 - cloud-sync - deleted.srm.tmp", dir);
   CHECK(filestream_write_file(stale, "x", 1));
   for (i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
   {
      snprintf(path, sizeof(path), "%s/%s", dir, kept[i]);
      CHECK(filestream_write_file(path, "x", 1));
   }
   task_cloud_sync_sweep_temps(st);
   CHECK(!path_is_valid(stale));
   for (i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
   {
      snprintf(path, sizeof(path), "%s/%s", dir, kept[i]);
      CHECK(path_is_valid(path));
      filestream_delete(path);
   }
   CHECK(filestream_write_file(stale, "x", 1));
#if defined(TCS_MOVE_WORKER)
   st->updated_server_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->updated_local_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
   retro_atomic_int_init(&st->waiting, 0);
   retro_atomic_int_init(&st->phase, CLOUD_SYNC_PHASE_DIFF);
   task_cloud_sync_task_setup(task, st, "Cloud Sync");
   server.alt = (char*)"saves/save.srm";
   snprintf(to, sizeof(to),
         "%s/save - 2026-11-07 10-00-01 - cloud-sync - deleted.srm", dir);
   snprintf(temp, sizeof(temp), "%s.tmp", to);
   CHECK(filestream_write_file(local_path, "old", 3));
   retro_atomic_store_release_int(&gate, 1);
   CHECK(task_cloud_sync_move_push(st, NULL, &server, "saves/save.srm",
            local_path, to, NULL, NULL));
   for (i = 0; i < 2000 && !retro_atomic_load_acquire_int(&at_gate); i++)
      retro_sleep(1);
   CHECK(path_is_valid(temp));
#endif
   task_cloud_sync_sweep_temps(st);
   CHECK(path_is_valid(stale));
   filestream_delete(stale);
#if defined(TCS_MOVE_WORKER)
   CHECK(path_is_valid(temp));
   retro_atomic_store_release_int(&gate, 0);
   for (i = 0; i < 2000 && st->moves; i++)
   {
      task_cloud_sync_moves_run(st);
      retro_sleep(1);
   }
   CHECK(holds(to, "old"));
   CHECK(!path_is_valid(local_path));
   filestream_delete(to);
   task_cloud_sync_cleanup(task);
   free(task->title);
   free(task);
#else
   string_list_free(st->dirlist);
   free(st);
#endif
}

static const char *walk_files[] = {
   "saves/ok.srm",
   "saves/Snes9x/backups/y.srm",
   "saves/backups/x.srm",
   "states/Backups/s.state",
   "config/remaps/r.rmp",
   "config/recovery/z - 2026-11-03 10-00-00 - Snes9x - unsaved.srm"
};

/* The folders those make, deepest first */
static const char *walk_dirs[] = {
   "saves/Snes9x/backups",
   "saves/Snes9x",
   "saves/backups",
   "saves",
   "states/Backups",
   "states",
   "config/remaps",
   "config/recovery",
   "config"
};

static bool walk_listed(const task_cloud_sync_state_t *st, const char *key)
{
   size_t i;
   for (i = 0; i < st->current_manifest->size; i++)
      if (string_is_equal(st->current_manifest->list[i].alt, key))
         return true;
   return false;
}

static void walk_lane(void)
{
   static const char *roots[] = { "saves", "states", "config" };
   retro_task_t *task = task_init();
   task_cloud_sync_state_t *st = (task_cloud_sync_state_t*)calloc(1, sizeof(*st));
   union string_list_elem_attr attr;
   char path[256];
   unsigned i;
   attr.i = 0;
   st->dirlist = string_list_new();
   for (i = 0; i < 3; i++)
   {
      snprintf(path, sizeof(path), "%s/walk/%s", directory, roots[i]);
      string_list_append(st->dirlist, roots[i], attr);
      st->dirlist->elems[i].userdata = strdup(path);
   }
   for (i = 0; i < sizeof(walk_files) / sizeof(walk_files[0]); i++)
   {
      char dir[256];
      snprintf(path, sizeof(path), "%s/walk/%s", directory, walk_files[i]);
      fill_pathname_basedir(dir, path, sizeof(dir));
      CHECK(path_mkdir(dir));
      CHECK(filestream_write_file(path, "x", 1));
   }
   retro_atomic_int_init(&st->waiting, 0);
   retro_atomic_int_init(&st->phase, CLOUD_SYNC_PHASE_BUILD_CURRENT_MANIFEST);
   task_cloud_sync_task_setup(task, st, "Cloud Sync");
   for (i = 0; i < 1000
         && task_cloud_sync_phase_get(st) == CLOUD_SYNC_PHASE_BUILD_CURRENT_MANIFEST; i++)
      task_cloud_sync_task_handler(task);
   CHECK(task_cloud_sync_phase_get(st) == CLOUD_SYNC_PHASE_DIFF);
   CHECK(st->current_manifest && st->current_manifest->size == 3);
   if (st->current_manifest)
   {
      CHECK(walk_listed(st, "saves/ok.srm"));
      CHECK(walk_listed(st, "saves/Snes9x/backups/y.srm"));
      CHECK(walk_listed(st, "config/remaps/r.rmp"));
   }
   /* nor fetched, uploaded or deleted from the server */
   CHECK(task_cloud_sync_should_ignore_file("saves/backups/x.srm"));
   CHECK(task_cloud_sync_should_ignore_file("states/BACKUPS/s.state"));
   CHECK(task_cloud_sync_should_ignore_file("config/recovery/z.srm"));
   CHECK(!task_cloud_sync_should_ignore_file("saves/Snes9x/backups/y.srm"));
   CHECK(!task_cloud_sync_should_ignore_file("saves/backupsx/a.srm"));
   CHECK(!task_cloud_sync_should_ignore_file("system/backups/a.bin"));
   task_cloud_sync_cleanup(task);
   free(task->title);
   free(task);
   for (i = 0; i < sizeof(walk_files) / sizeof(walk_files[0]); i++)
   {
      snprintf(path, sizeof(path), "%s/walk/%s", directory, walk_files[i]);
      filestream_delete(path);
   }
   for (i = 0; i < sizeof(walk_dirs) / sizeof(walk_dirs[0]); i++)
   {
      snprintf(path, sizeof(path), "%s/walk/%s", directory, walk_dirs[i]);
      CHECK(path_rmdir(path));
   }
   snprintf(path, sizeof(path), "%s/walk", directory);
   CHECK(path_rmdir(path));
}

int main(void)
{
   unsigned t;
   int m;
   retro_atomic_int_init(&off_main, 0);
   retro_atomic_int_init(&ownership_checks, 0);
   retro_atomic_int_init(&main_syncs, 0);
   retro_atomic_int_init(&gate, 0);
   retro_atomic_int_init(&at_gate, 0);
   retro_atomic_int_init(&max_piece, 0);
   retro_atomic_int_init(&vanishing, 0);
#if defined(HAVE_THREADS) && !defined(TCS_LOCK_FREE)
   tcs_manifest_lock = slock_new();
#endif
   CHECK(path_mkdir(directory));
   snprintf(local_path, sizeof(local_path), "%s/save.srm", directory);
   pathname_conform_slashes_to_os(local_path);
   snprintf(fetching_path, sizeof(fetching_path), "%s.rafetching", local_path);
   snprintf(z_path, sizeof(z_path), "%s/z.srm", directory);
   pathname_conform_slashes_to_os(z_path);
   for (t = 0; t < 2; t++)
   {
      lane(t != 0, false, false, false, true);
      lane(t != 0, false, true, false, true);
      lane(t != 0, false, false, true, true);
      lane(t != 0, true, true, false, true);
      lane(t != 0, true, false, false, true);
      lane(t != 0, false, false, false, false);
      lane(t != 0, false, true, false, false);
      lane(t != 0, false, false, true, false);
      lane(t != 0, true, true, false, false);
      lane(t != 0, true, false, false, false);
   }
   /* across drives the move is a copy, then the delete */
   for (m = 1; m <= 4; m++)
   {
      retro_atomic_store_relaxed_int(&refuse_move, m);
      for (t = 0; t < 2; t++)
      {
         lane(t != 0, false, false, false, false);
         lane(t != 0, true, false, false, false);
      }
   }
   retro_atomic_store_relaxed_int(&refuse_move, 0);
   CHECK(retro_atomic_load_relaxed_int(&refused) == 16);
   /* the save deleted as it is copied, by a core or another tool */
   retro_atomic_store_relaxed_int(&refuse_move, 1);
   vanish = true;
   for (t = 0; t < 2; t++)
   {
      lane(t != 0, false, false, false, false);
      lane(t != 0, true, false, false, false);
   }
   vanish = false;
   retro_atomic_store_relaxed_int(&refuse_move, 0);
#if defined(TCS_MOVE_WORKER)
   {
      static const int64_t small[] = { 1, 1, 1, 1, 1 };
      static const int64_t sized[] = { 6 << 20, 6 << 20, 6 << 20, 17 << 20, 1 };
      static const int64_t edge[]  = { 8 << 20, 8 << 20 };
      /* held at their syncs, a slow card: the copies, while the sync
       * goes on */
      retro_atomic_store_relaxed_int(&refuse_move, 1);
      for (hold_mode = 1; hold_mode <= 9; hold_mode++)
         for (t = 0; t < 2; t++)
         {
            if (hold_mode != 5)
               lane(t != 0, false, false, false, false);
            lane(t != 0, true, false, false, false);
         }
      hold_mode = 0;
      state_writing = false;
      retro_atomic_store_relaxed_int(&refuse_move, 0);
      budget_lane(small, 5, 3);
      budget_lane(sized, 5, 2);
      /* at the budget, not under it: the second waits */
      budget_lane(edge, 2, 1);
   }
#endif
   sweep_lane();
   backup_location_lane();
   walk_lane();
   puts(failures ? "FAIL cloudsync_commit_test" : "PASS cloudsync_commit_test");
   return failures ? 1 : 0;
}
