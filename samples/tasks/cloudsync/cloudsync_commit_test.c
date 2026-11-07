/* The real cloud-sync task, queued with threaded tasks on and off.
 * Downloads arrive on another thread. Save ownership and file commits
 * must stay on the content thread, including a failed hand-over.
 * The walk leaves out the safety copies and recovery files. */
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
static char local_path[128], fetching_path[160];
static retro_atomic_int_t off_main, ownership_checks;
static int live, retired, failures, expect_old, expect_failure;
static void *pending_fetch;
/* 1: moving the save into its backups fails, as across drives;
 * 2: deleting it fails too; 3: the delete removes it but reports a
 * failure, as a network drive can */
static retro_atomic_int_t refuse_move, refused;
/* syncs of a backup, and how many came before the save was deleted */
static retro_atomic_int_t backup_syncs, syncs_at_delete;

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

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
   if (     string_is_equal(path, local_path)
         && retro_atomic_load_relaxed_int(&syncs_at_delete) < 0)
      retro_atomic_store_relaxed_int(&syncs_at_delete,
            retro_atomic_load_relaxed_int(&backup_syncs));
   if (     retro_atomic_load_relaxed_int(&refuse_move) >= 2
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
      retro_atomic_inc_int(&backup_syncs);
   return __real_retro_vfs_file_sync_impl(stream);
}
bool content_savefile_is_live(const char *path)
{
   retro_atomic_inc_int(&ownership_checks);
   if (!task_is_on_main_thread())
      retro_atomic_inc_int(&off_main);
   return live && !strcmp(path, local_path);
}
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
{ (void)within; (void)budget; }
void task_nbio_slice_open(nbio_budget_t *b) { b->floor = 1; }
void task_nbio_slice_close(nbio_budget_t *b) { (void)b; }
bool task_nbio_slice_within_budget(void *ud, size_t avail, size_t len)
{ (void)ud; (void)avail; (void)len; return true; }

static void add(file_list_t *list, const char *path, const char *hash)
{
   size_t at = list->size;
   file_list_append(list, path, NULL, 0, 0, 0);
   file_list_set_alt_at_offset(list, at, "saves/save.srm");
   list->list[at].userdata = hash ? strdup(hash) : NULL;
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
   /* a save that can't be deleted is kept, its copy undone */
   bool keep = mode == 2;
   retired = 0;
   live = 0;
   expect_old = owned || no_memory || keep;
   expect_failure = no_memory || keep;
   retro_atomic_store_relaxed_int(&off_main, 0);
   retro_atomic_store_relaxed_int(&ownership_checks, 0);
   retro_atomic_store_relaxed_int(&backup_syncs, 0);
   retro_atomic_store_relaxed_int(&syncs_at_delete, -1);
   CHECK(filestream_write_file(local_path, "old", 3));
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
      worker = sthread_create(complete_fetch, pending_fetch);
      CHECK(worker != NULL);
      if (worker)
         sthread_join(worker);
      CHECK(holds(local_path, "old"));
   }
   /* A core takes the save after the transfer, before its commit. */
   live = owned;
   task_queue_push(task);
   for (i = 0; i < 2000 && !retired; i++)
   {
      task_queue_check();
      retro_sleep(1);
   }
   CHECK(retired == 1);
   CHECK(retro_atomic_load_relaxed_int(&off_main) == 0);
   if (!no_memory)
      CHECK(retro_atomic_load_relaxed_int(&ownership_checks) > 0);
   /* a rename is not synced; a copy is, before the save goes */
   if (!mode)
      CHECK(retro_atomic_load_relaxed_int(&backup_syncs) == 0);
   else
      CHECK(retro_atomic_load_relaxed_int(&syncs_at_delete) > 0);
   if (expect_old)
      CHECK(holds(local_path, "old"));
   else if (!deletion)
      CHECK(holds(local_path, "new"));
   else
      CHECK(!path_is_valid(local_path));
   CHECK(!path_is_valid(fetching_path));
   snprintf(backup_dir, sizeof(backup_dir), "%s/backups/cloud-sync/saves", directory);
   backups = dir_list_new(backup_dir, NULL, false, false, true, true);
   if (!destructive && !expect_old)
   {
      CHECK(backups && backups->size == 1);
      if (backups && backups->size)
      {
         struct save_backup_name name;
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
            filestream_delete(backups->elems[i].data);
      string_list_free(backups);
   }
   task_queue_deinit();
   filestream_delete(local_path);
   snprintf(manifest, sizeof(manifest), "%s/assets/manifest.local", directory);
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
         CHECK(task_cloud_sync_backup_file(st, &item, SAVE_BACKUP_REPLACED));
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
   CHECK(task_cloud_sync_backup_file(st, &item, SAVE_BACKUP_DELETED));
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
#if defined(HAVE_THREADS) && !defined(TCS_LOCK_FREE)
   tcs_manifest_lock = slock_new();
#endif
   CHECK(path_mkdir(directory));
   snprintf(local_path, sizeof(local_path), "%s/save.srm", directory);
   pathname_conform_slashes_to_os(local_path);
   snprintf(fetching_path, sizeof(fetching_path), "%s.rafetching", local_path);
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
   for (m = 1; m <= 3; m++)
   {
      retro_atomic_store_relaxed_int(&refuse_move, m);
      for (t = 0; t < 2; t++)
      {
         lane(t != 0, false, false, false, false);
         lane(t != 0, true, false, false, false);
      }
   }
   retro_atomic_store_relaxed_int(&refuse_move, 0);
   CHECK(retro_atomic_load_relaxed_int(&refused) == 12);
   backup_location_lane();
   walk_lane();
   puts(failures ? "FAIL cloudsync_commit_test" : "PASS cloudsync_commit_test");
   return failures ? 1 : 0;
}
