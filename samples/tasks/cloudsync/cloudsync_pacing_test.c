/* Cloud sync's diff is paced by the shared per-frame I/O window.
 *
 * Every sync hashes every local file it covers. The diff used to take
 * one file per run of the task handler and hash it whole there, so a
 * large file - a big savestate, a memory card, a BIOS under "sync
 * system" - held one run of the handler, which with Threaded Tasks off
 * is the frame loop, for as long as reading and hashing it took; and a
 * thousand small files took a thousand frames.
 *
 * The diff now runs as many steps as task_nbio_slice_within_budget()
 * grants, and hashes the current file a 256 KiB chunk at a time between
 * steps, carrying a file over to the next run when the window closes.
 * Here the window is a stub granting a fixed number of work units per
 * run, and the real tasks/task_cloudsync.c handler is driven through a
 * diff of files whose hashes the server already has:
 *
 *  - a 9 MiB file is hashed over many runs, never more than the grant
 *    per run, and its hash matches a one-shot MD5 of the file;
 *  - small files go several to a run instead of one per run;
 *  - with a window that is always spent, every run still makes
 *    progress (the floor) and the diff finishes;
 *  - the result is the same as before: every file found unchanged,
 *    nothing transferred;
 *  - a 9 MiB download, reported from another thread, is hashed by the
 *    task thread over many runs within the grant, and its hash goes
 *    into both updated manifests (the fetch callback used to hash the
 *    whole file itself, on the thread it ran on);
 *  - the directory walk that lists the local files runs as many entries
 *    per run as the grant allows, at least one, instead of reading every
 *    synced directory in one run, and lists what dir_list_new() listed:
 *    nested and hidden files, ignored names left out, two roots; a root
 *    that cannot be read still stops the sync;
 *  - a core's save items, in the real registry, sync as save RAM does:
 *    walked and uploaded from the save directory, their write temps
 *    left out; a newer server copy or a server deletion of a file the
 *    running core owns, or one an item that left still owes a write,
 *    is deferred; an item in the system directory is walked only with
 *    Sync: System Files on;
 *  - pushed on the task queue and run to its end, the sync releases
 *    everything it held (LeakSanitizer): the state used to be left
 *    behind by every sync, because it was freed from a callback that
 *    was handed task_data, which the task never set.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

#include <rthreads/rthreads.h>
#include <retro_timers.h>

#include "../../../tasks/task_cloudsync.c"
#include "../../../save_items.h"

/* The locked hand-over (TASK_CLOUDSYNC_FORCE_LOCK, or a backend with no
 * pointer atomics) appends under the lock task_push_cloud_sync() makes;
 * these tests make it themselves. */
static void tcs_test_lock(void)
{
#if defined(HAVE_THREADS) && !defined(TCS_LOCK_FREE)
   if (!tcs_manifest_lock)
      tcs_manifest_lock = slock_new();
#endif
}

static unsigned transfers;

/* --- what the task links against and this test never reaches ---------- */

void RARCH_LOG(const char *fmt, ...)  { (void)fmt; }
void RARCH_WARN(const char *fmt, ...) { (void)fmt; }
void RARCH_ERR(const char *fmt, ...)  { (void)fmt; }
bool verbosity_is_enabled(void)       { return false; }
/* NULL but for the follow-up lane, which pushes real syncs */
static settings_t *test_settings;
settings_t *config_get_ptr(void)      { return test_settings; }
/* The save and state directories a sync covers. */
static char save_dir[96], state_dir[96];
char *dir_get_ptr(enum rarch_dir_type type)
{
   if (type == RARCH_DIR_SAVEFILE)
      return save_dir;
   return type == RARCH_DIR_SAVESTATE ? state_dir : NULL;
}
/* As save.c answers: the save RAM of a running core, if any, the save
 * items it registered (the real registry), and a path an item that left
 * still owes its last image. */
static char live_path[256];
static char pending_path[256];
bool content_savefile_is_live(const char *path)
{
   if (!path)
      return false;
   if (     (*live_path    && !strcmp(path, live_path))
         || (*pending_path && !strcmp(path, pending_path)))
      return true;
   return save_items_path_is_live(path);
}
void task_window_progress_cb(retro_task_t *task) { (void)task; }
size_t fill_pathname_application_special(char *s, size_t len,
      enum application_special_type type)
{ (void)s; (void)len; (void)type; return 0; }
void cloud_sync_find_driver(const char *drv, const char *prefix,
      bool verbosity_enabled)
{ (void)drv; (void)prefix; (void)verbosity_enabled; }
void cloud_sync_capture(void) { }
static unsigned begins;
bool cloud_sync_begin(cloud_sync_complete_handler_t cb, void *user_data)
{ (void)cb; (void)user_data; begins++; return false; }
/* Nonzero: the driver ends the sync, and reports it at once. */
static int end_ok;
bool cloud_sync_end(cloud_sync_complete_handler_t cb, void *user_data)
{
   if (!end_ok)
      return false;
   cb(user_data, NULL, true, NULL);
   return true;
}
/* A fetch writes @fetch_size bytes to the local file and reports it from
 * another thread, as a WebDAV or S3 transfer reports from the main
 * thread while the task runs on the queue's worker. Unset, a fetch is
 * an unexpected transfer. */
static size_t fetch_size;
static retro_atomic_int_t fetches_reported;

typedef struct
{
   cloud_sync_complete_handler_t cb;
   void *ud;
   char  key[64];
   char  file[128];
} fetch_job_t;

static void write_file(const char *path, size_t size, unsigned seed);

/* The keys a sync asked the driver to fetch ([0]) and to upload ([1]). */
static char     asked[2][16][64];
static unsigned num_asked[2];

static void ask(unsigned what, const char *key)
{
   if (num_asked[what] < 16)
      strlcpy(asked[what][num_asked[what]], key, sizeof(asked[0][0]));
   num_asked[what]++;
}

/* Nonzero: a fetch writes half its bytes and then reports failure, as
 * a transfer cut off part-way. */
static int fetch_fail;

static void fetch_thread(void *data)
{
   fetch_job_t job = *(fetch_job_t*)data;
   RFILE      *f;
   free(data);
   if (fetch_fail)
   {
      write_file(job.file, fetch_size / 2, 9);
      job.cb(job.ud, job.key, false, NULL);
      retro_atomic_inc_int(&fetches_reported);
      return;
   }
   write_file(job.file, fetch_size, 9);
   f = filestream_open(job.file, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   job.cb(job.ud, job.key, f != NULL, f);
   retro_atomic_inc_int(&fetches_reported);
}

bool cloud_sync_read(const char *path, const char *file,
      cloud_sync_complete_handler_t cb, void *user_data)
{
   fetch_job_t *job;
   sthread_t   *t;
   ask(0, path);
   if (!fetch_size || !(job = (fetch_job_t*)calloc(1, sizeof(*job))))
   {
      transfers++;
      return false;
   }
   job->cb = cb;
   job->ud = user_data;
   strlcpy(job->key,  path, sizeof(job->key));
   strlcpy(job->file, file, sizeof(job->file));
   if (!(t = sthread_create(fetch_thread, job)))
   {
      free(job);
      return false;
   }
   sthread_detach(t);
   return true;
}
bool cloud_sync_update(const char *path, RFILE *file,
      cloud_sync_complete_handler_t cb, void *user_data)
{ (void)file; (void)cb; (void)user_data; ask(1, path); transfers++; return false; }
bool cloud_sync_free(const char *path, cloud_sync_complete_handler_t cb,
      void *user_data)
{ (void)path; (void)cb; (void)user_data; transfers++; return false; }
void cloud_sync_poll(cloud_sync_poll_budget_t within, void *budget) { (void)within; (void)budget; }

/* --- the I/O window: @grant units per run on top of the floor ------- */

static unsigned grant;
static unsigned granted;      /* units handed out in the open run */
static unsigned max_granted;  /* most units any run used */
static unsigned runs;

void task_nbio_slice_open(nbio_budget_t *b)
{
   b->floor = 1;
   granted  = 0;
   runs++;
}

void task_nbio_slice_close(nbio_budget_t *b)
{
   (void)b;
   if (granted > max_granted)
      max_granted = granted;
}

bool task_nbio_slice_within_budget(void *ud, size_t avail, size_t len)
{
   nbio_budget_t *b = (nbio_budget_t*)ud;
   (void)avail; (void)len;
   if (b->floor)
   {
      b->floor = 0;
      granted++;
      return true;
   }
   if (granted < grant + 1)
   {
      granted++;
      return true;
   }
   return false;
}

/* --- the test ----------------------------------------------------------- */

static int fails;

static void check(const char *what, int ok)
{
   printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
   if (!ok)
      fails++;
}

static char dir[64];

static void write_file(const char *path, size_t size, unsigned seed)
{
   FILE   *f = fopen(path, "wb");
   size_t  i;
   for (i = 0; f && i < size; i++)
      fputc((int)((i * 131 + seed * 7 + (i >> 9)) & 0xff), f);
   if (f)
      fclose(f);
}

static char *hash_of(const char *path)
{
   RFILE *f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   char  *h = f ? task_cloud_sync_md5_rfile(f) : NULL;
   if (f)
      filestream_close(f);
   return h;
}

static void list_add(file_list_t *list, const char *key, const char *path,
      char *hash)
{
   size_t idx = list->size;
   file_list_append(list, path, NULL, 0, 0, 0);
   file_list_set_alt_at_offset(list, idx, key);
   list->list[idx].userdata = hash;
}

/* A sync at the diff with @n files of @size bytes, all unchanged on the
 * server since the last sync. Runs the handler until the diff ends and
 * returns the number of runs. */
static unsigned run_diff(unsigned n, size_t size, unsigned grant_units,
      int *hashes_ok)
{
   task_cloud_sync_state_t *st   = (task_cloud_sync_state_t*)calloc(1, sizeof(*st));
   retro_task_t            *task = task_init();
   struct string_list      *dl   = string_list_new();
   union string_list_elem_attr attr;
   unsigned                 i;

   attr.i = 0;
   string_list_append(dl, "saves", attr);
   dl->elems[0].userdata = strdup(dir);
   st->dirlist                 = dl;
   st->server_manifest         = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->local_manifest          = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->current_manifest        = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->updated_server_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->updated_local_manifest  = (file_list_t*)calloc(1, sizeof(file_list_t));
   retro_atomic_int_init(&st->waiting, 0);
   retro_atomic_int_init(&st->phase, (int)CLOUD_SYNC_PHASE_DIFF);

   for (i = 0; i < n; i++)
   {
      char key[64], path[128];
      snprintf(key,  sizeof(key),  "saves/f%04u.srm", i);
      snprintf(path, sizeof(path), "%s/f%04u.srm", dir, i);
      write_file(path, size, i);
      list_add(st->server_manifest,  key, NULL, hash_of(path));
      list_add(st->local_manifest,   key, NULL, hash_of(path));
      list_add(st->current_manifest, key, path, NULL);
   }

   task->state   = st;
   task->handler = task_cloud_sync_task_handler;
   grant = grant_units; runs = 0; max_granted = 0; transfers = 0;
   for (i = 0; i < 100000
         && task_cloud_sync_phase_get(st) == CLOUD_SYNC_PHASE_DIFF; i++)
      task_cloud_sync_task_handler(task);

   *hashes_ok = task_cloud_sync_phase_get(st) != CLOUD_SYNC_PHASE_DIFF;
   for (i = 0; i < n; i++)
   {
      const char *a = (const char*)st->current_manifest->list[i].userdata;
      const char *b = (const char*)st->server_manifest->list[i].userdata;
      if (!a || !b || strcmp(a, b))
         *hashes_ok = 0;
   }
   task_cloud_sync_fold_manifest_adds(st);
   if (     st->updated_server_manifest->size != n
         || st->updated_local_manifest->size  != n)
      *hashes_ok = 0;

   for (i = 0; i < n; i++)
   {
      char path[128];
      snprintf(path, sizeof(path), "%s/f%04u.srm", dir, i);
      unlink(path);
   }
   task_cloud_sync_cleanup(task);
   free(task->title);
   free(task);
   return runs;
}

/* One file on the server only: fetched, then hashed by the task. */
static void fetch_lane(void)
{
   task_cloud_sync_state_t *st   = (task_cloud_sync_state_t*)calloc(1, sizeof(*st));
   retro_task_t            *task = task_init();
   struct string_list      *dl   = string_list_new();
   union string_list_elem_attr attr;
   char     path[128];
   char    *want;
   unsigned i;
   int      ok_server = 0, ok_local = 0;

   attr.i = 0;
   string_list_append(dl, "saves", attr);
   dl->elems[0].userdata = strdup(dir);
   st->dirlist                 = dl;
   st->server_manifest         = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->local_manifest          = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->current_manifest        = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->updated_server_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->updated_local_manifest  = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->destructive             = true;
   strlcpy(st->dir_core_assets, dir, sizeof(st->dir_core_assets));
   retro_atomic_int_init(&st->waiting, 0);
   retro_atomic_int_init(&st->phase, (int)CLOUD_SYNC_PHASE_DIFF);
   retro_atomic_int_init(&fetches_reported, 0);

   /* what the download will hold, hashed beforehand */
   snprintf(path, sizeof(path), "%s/dl.srm", dir);
   fetch_size = 9 * 1024 * 1024;
   write_file(path, fetch_size, 9);
   want = hash_of(path);
   unlink(path);
   list_add(st->server_manifest, "saves/dl.srm", NULL, strdup(want));

   task_cloud_sync_task_setup(task, st, "Cloud Sync in progress");
   grant = 3; runs = 0; max_granted = 0; transfers = 0;
   /* start the fetch and wait for its report */
   for (i = 0; i < 5000 && !task_cloud_sync_waiting_get(st)
         && !retro_atomic_load_acquire_int(&fetches_reported); i++)
      task_cloud_sync_task_handler(task);
   /* Leave completed downloads queued until the fixture is ready. */
   for (i = 0; i < 5000 && !retro_atomic_load_acquire_int(&fetches_reported); i++)
      retro_sleep(1);
   check("fetch: callback returned before processing",
         retro_atomic_load_acquire_int(&fetches_reported) != 0);
   /* the callback has returned: the file is not hashed yet, and one run
    * of the handler hashes only what its grant allows */
   task_cloud_sync_task_handler(task);
   check("download: not hashed by the callback nor in one run",
         task_cloud_sync_waiting_get(st) == 1 && st->downloads == 0);
   for (i = 0; i < 200000 && (task_cloud_sync_phase_get(st) == CLOUD_SYNC_PHASE_DIFF
            || task_cloud_sync_waiting_get(st) > 0); i++)
      task_cloud_sync_task_handler(task);
   check("download: fetched and hashed, nothing in flight",
         task_cloud_sync_waiting_get(st) == 0 && st->downloads == 1);
   check("download: hashed over many runs", runs >= 36 / 4);
   check("download: no run past its grant", max_granted <= 4);
   task_cloud_sync_fold_manifest_adds(st);
   for (i = 0; i < st->updated_server_manifest->size; i++)
      if (!strcmp(st->updated_server_manifest->list[i].alt, "saves/dl.srm"))
         ok_server = st->updated_server_manifest->list[i].userdata
            && !strcmp((const char*)st->updated_server_manifest->list[i].userdata, want);
   for (i = 0; i < st->updated_local_manifest->size; i++)
      if (!strcmp(st->updated_local_manifest->list[i].alt, "saves/dl.srm"))
         ok_local = st->updated_local_manifest->list[i].userdata
            && !strcmp((const char*)st->updated_local_manifest->list[i].userdata, want);
   check("download: its hash in both updated manifests", ok_server && ok_local);
   check("download: matches the server, no manifest upload needed",
         !st->need_manifest_uploaded);
   {
      char  tmp[160];
      char *got = hash_of(path);
      snprintf(tmp, sizeof(tmp), "%s.rafetching", path);
      check("download: lands under its own name, nothing left beside it",
            got && !strcmp(got, want) && access(tmp, F_OK) != 0);
      free(got);
   }

   fetch_size = 0;
   free(want);
   unlink(path);
   task_cloud_sync_cleanup(task);
   free(task->title);
   free(task);
}

/* A local save the server has a newer copy of; the fetch is under way
 * when @what happens. The local file must come through as it was. */
static void fetch_keep_lane(int live, const char *what_keep, const char *what_rest)
{
   task_cloud_sync_state_t *st   = (task_cloud_sync_state_t*)calloc(1, sizeof(*st));
   retro_task_t            *task = task_init();
   struct string_list      *dl   = string_list_new();
   union string_list_elem_attr attr;
   char     path[128], tmp[160];
   char    *old, *srv, *got;
   unsigned i;
   int      carried = 0;

   attr.i = 0;
   string_list_append(dl, "saves", attr);
   dl->elems[0].userdata = strdup(dir);
   st->dirlist                 = dl;
   st->server_manifest         = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->local_manifest          = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->current_manifest        = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->updated_server_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->updated_local_manifest  = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->destructive             = true;
   strlcpy(st->dir_core_assets, dir, sizeof(st->dir_core_assets));
   retro_atomic_int_init(&st->waiting, 0);
   retro_atomic_int_init(&st->phase, (int)CLOUD_SYNC_PHASE_DIFF);
   retro_atomic_int_init(&fetches_reported, 0);

   /* the server's copy, hashed, then the local one in its place */
   snprintf(path, sizeof(path), "%s/keep.srm", dir);
   snprintf(tmp,  sizeof(tmp),  "%s.rafetching", path);
   fetch_size = 64 * 1024;
   write_file(path, fetch_size, 9);
   srv = hash_of(path);
   write_file(path, 4096, 4);
   old = hash_of(path);
   list_add(st->server_manifest, "saves/keep.srm", NULL, strdup(srv));
   fetch_fail = !live;

   task_cloud_sync_task_setup(task, st, "Cloud Sync in progress");
   grant = 100;
   for (i = 0; i < 5000 && !task_cloud_sync_waiting_get(st)
         && !retro_atomic_load_acquire_int(&fetches_reported); i++)
      task_cloud_sync_task_handler(task);
   /* Leave completed downloads queued until the fixture is ready. */
   for (i = 0; i < 5000 && !retro_atomic_load_acquire_int(&fetches_reported); i++)
      retro_sleep(1);
   check("fetch: callback returned before processing",
         retro_atomic_load_acquire_int(&fetches_reported) != 0);
   /* the download is in, not yet in place: a core takes the file now */
   if (live)
   {
      strlcpy(live_path, path, sizeof(live_path));
      pathname_conform_slashes_to_os(live_path);
   }
   for (i = 0; i < 200000 && (task_cloud_sync_phase_get(st) == CLOUD_SYNC_PHASE_DIFF
            || task_cloud_sync_waiting_get(st) > 0); i++)
      task_cloud_sync_task_handler(task);

   got = hash_of(path);
   check(what_keep, got && !strcmp(got, old) && access(tmp, F_OK) != 0);
   task_cloud_sync_fold_manifest_adds(st);
   for (i = 0; i < st->updated_server_manifest->size; i++)
      if (!strcmp(st->updated_server_manifest->list[i].alt, "saves/keep.srm"))
         carried = st->updated_server_manifest->list[i].userdata
            && !strcmp((const char*)st->updated_server_manifest->list[i].userdata, srv);
   check(what_rest, st->downloads == 0 && carried
         && (live ? !st->failures : st->failures));

   *live_path = '\0';
   fetch_fail = 0;
   fetch_size = 0;
   free(got);
   free(old);
   free(srv);
   unlink(path);
   unlink(tmp);
   task_cloud_sync_cleanup(task);
   free(task->title);
   free(task);
}

static int key_cmp(const void *a, const void *b)
{
   return strcmp(*(const char*const*)a, *(const char*const*)b);
}

/* The keys dir_list_new() gives for @root under @name, as the manifest
 * build made them before the walk. */
static size_t reference_keys(const char *root, const char *name,
      char **keys, size_t at)
{
   char slash[256], rel[256], alt[256];
   struct string_list *l;
   size_t i;
   strlcpy(slash, root, sizeof(slash));
   fill_pathname_slash(slash, sizeof(slash));
   if (!(l = dir_list_new(slash, NULL, false, true, true, true)))
      return at;
   for (i = 0; i < l->size; i++)
   {
      path_relative_to(rel, l->elems[i].data, slash, sizeof(rel));
      fill_pathname_join_special(alt, name, rel, sizeof(alt));
      if (task_cloud_sync_should_ignore_file(alt))
         continue;
      pathname_make_slashes_portable(alt);
      keys[at++] = strdup(alt);
   }
   string_list_free(l);
   return at;
}

static task_cloud_sync_state_t *walk_state(const char *root_a, const char *root_b)
{
   task_cloud_sync_state_t *st = (task_cloud_sync_state_t*)calloc(1, sizeof(*st));
   struct string_list *dl      = string_list_new();
   union string_list_elem_attr attr;
   attr.i = 0;
   string_list_append(dl, "saves", attr);
   dl->elems[0].userdata = strdup(root_a);
   if (root_b)
   {
      string_list_append(dl, "states", attr);
      dl->elems[1].userdata = strdup(root_b);
   }
   st->dirlist = dl;
   retro_atomic_int_init(&st->waiting, 0);
   retro_atomic_int_init(&st->phase, (int)CLOUD_SYNC_PHASE_BUILD_CURRENT_MANIFEST);
   return st;
}

static unsigned walk_run(task_cloud_sync_state_t *st, retro_task_t *task)
{
   unsigned i;
   task_cloud_sync_task_setup(task, st, "Cloud Sync in progress");
   runs = 0; max_granted = 0;
   for (i = 0; i < 200000
         && task_cloud_sync_phase_get(st) == CLOUD_SYNC_PHASE_BUILD_CURRENT_MANIFEST; i++)
      task_cloud_sync_task_handler(task);
   return runs;
}

static void walk_lane(void)
{
   char      a[160], b[160], path[256], cmd[800];
   char    **keys = (char**)calloc(4000, sizeof(char*));
   char    **got  = (char**)calloc(4000, sizeof(char*));
   size_t    nkeys = 0, i;
   unsigned  r, k;
   int       same;
   task_cloud_sync_state_t *st;
   retro_task_t *task;

   snprintf(a, sizeof(a), "%s/walk_saves", dir);
   snprintf(b, sizeof(b), "%s/walk_states", dir);
   snprintf(cmd, sizeof(cmd), "mkdir -p %s/core1/sub %s/core2 %s/.hidden %s/x", a, a, a, b);
   if (system(cmd) != 0)
      return;
   for (k = 0; k < 2400; k++)
   {
      const char *sub = (k % 4 == 0) ? "core1" : (k % 4 == 1) ? "core1/sub" : (k % 4 == 2) ? "core2" : "";
      snprintf(path, sizeof(path), "%s/%s%sf%04u.srm", a, sub, *sub ? "/" : "", k);
      write_file(path, 16, k);
   }
   for (k = 0; k < 600; k++)
   {
      snprintf(path, sizeof(path), "%s/x/s%04u.state", b, k);
      write_file(path, 16, k);
   }
   snprintf(path, sizeof(path), "%s/.hidden/h.srm", a);      write_file(path, 4, 1);
   snprintf(path, sizeof(path), "%s/.dot.srm", a);           write_file(path, 4, 1);
   snprintf(path, sizeof(path), "%s/core2/p.srm.rafetching", a); write_file(path, 4, 1);
   snprintf(path, sizeof(path), "%s/core2/p.srm.tmp", a);        write_file(path, 4, 1);
   snprintf(path, sizeof(path), "%s/.DS_Store", b);          write_file(path, 4, 1);

   nkeys = reference_keys(a, "saves", keys, 0);
   nkeys = reference_keys(b, "states", keys, nkeys);
   qsort(keys, nkeys, sizeof(*keys), key_cmp);

   st   = walk_state(a, b);
   task = task_init();
   grant = 20;
   r = walk_run(st, task);
   check("walk: reaches the diff", task_cloud_sync_phase_get(st) == CLOUD_SYNC_PHASE_DIFF);
   check("walk: spread over runs, not one", r >= (unsigned)(nkeys / 21));
   check("walk: no run past its grant", max_granted <= 21);
   same = st->current_manifest && st->current_manifest->size == nkeys && nkeys >= 3002;
   for (i = 0; same && i < nkeys; i++)
      got[i] = st->current_manifest->list[i].alt;
   if (same)
      qsort(got, nkeys, sizeof(*got), key_cmp);
   for (i = 0; same && i < nkeys; i++)
      if (strcmp(got[i], keys[i]))
         same = 0;
   check("walk: the same files dir_list_new() lists, hidden ones too", same);
   for (i = 0; same && i < nkeys; i++)
      if (     strstr(keys[i], ".rafetching") || strstr(keys[i], ".DS_Store")
            || strstr(keys[i], ".tmp"))
         same = 0;
   check("walk: ignored names left out", same);
   task_cloud_sync_cleanup(task);
   free(task->title);
   free(task);

   /* a spent window: one entry per run, and it still ends */
   st   = walk_state(a, NULL);
   task = task_init();
   grant = 0;
   r = walk_run(st, task);
   check("walk: one entry per run with a spent window, and it ends",
         task_cloud_sync_phase_get(st) == CLOUD_SYNC_PHASE_DIFF && max_granted == 1);
   task_cloud_sync_cleanup(task);
   free(task->title);
   free(task);

   /* a root that cannot be read stops the sync */
   snprintf(path, sizeof(path), "%s/no_such_dir", dir);
   st   = walk_state(a, path);
   task = task_init();
   grant = 20;
   walk_run(st, task);
   check("walk: an unreadable root stops the sync",
         task_cloud_sync_phase_get(st) == CLOUD_SYNC_PHASE_END && st->failures);
   task_cloud_sync_cleanup(task);
   free(task->title);
   free(task);

   for (i = 0; i < nkeys; i++)
      free(keys[i]);
   free(keys);
   free(got);
   snprintf(cmd, sizeof(cmd), "rm -rf %s %s", a, b);
   if (system(cmd) != 0) { }
}

/* A game whose core registers save items: Game.eep beside its save RAM,
 * card.mcd, the *.mcd files of the cards folder, and vmu.bin in the
 * system directory. The write temps beside them never sync. */
static char system_dir[96];
static const char *const save_keys[] = {
   "saves/Game.srm", "saves/Game.eep", "saves/card.mcd",
   "saves/cards/a.mcd", "saves/cards/b.mcd", "saves/cards/notes.txt",
   "saves/Other.srm", NULL };
static const char *const temp_keys[] = {
   "saves/Game.eep.tmp", "saves/Game.eep.auto.tmp", "saves/card.mcd.tmp",
   NULL };
static const char *const system_keys[] = {
   "system/vmu.bin", "system/bios.bin", NULL };
/* Server copies newer than the local files: of what the running core
 * owns (cards/c.mcd is on the server only), of what it does not, and a
 * file the server deleted. */
static const char *const owned_keys[] = {
   "saves/Game.srm", "saves/Game.eep", "saves/card.mcd",
   "saves/cards/a.mcd", "saves/cards/c.mcd", NULL };
static const char *const unowned_keys[] = {
   "saves/cards/notes.txt", "saves/Other.srm", NULL };
static const char *const deleted_keys[] = { "saves/cards/b.mcd", NULL };

static unsigned char eep[64];
static struct retro_save_segment eep_seg = { eep, sizeof(eep) };
static const char *const mcd_files[] = { "*.mcd", NULL };

static void key_path(char *s, size_t len, const char *key)
{
   if (!strncmp(key, "system/", 7))
      snprintf(s, len, "%s/%s", system_dir, key + 7);
   else
      snprintf(s, len, "%s/%s", save_dir, key + 6);
}

static void write_keys(const char *const *keys)
{
   char path[256];
   for (; *keys; keys++)
   {
      key_path(path, sizeof(path), *keys);
      write_file(path, 64, (unsigned)strlen(*keys));
   }
}

/* The files as each lane starts. */
static void items_files(void)
{
   write_keys(save_keys);
   write_keys(system_keys);
}

static void unlink_keys(const char *const *keys)
{
   char path[256];
   for (; *keys; keys++)
   {
      key_path(path, sizeof(path), *keys);
      unlink(path);
   }
}

/* @keys into @list with the file's hash ('c'), a newer hash ('n'), or
 * none, deleted on the server ('d'). */
static void add_keys(file_list_t *list, const char *const *keys, char how)
{
   char path[256];
   for (; *keys; keys++)
   {
      key_path(path, sizeof(path), *keys);
      list_add(list, *keys, NULL, how == 'c' ? hash_of(path)
            : how == 'n' ? strdup("0123456789abcdef0123456789abcdef")
            : NULL);
   }
}

/* The game starts and its core registers its items, or it closes. */
static void items_running(bool running)
{
   struct retro_save_item      items[4];
   struct retro_save_interface iface;
   struct save_items_paths     paths;
   unsigned i;

   save_items_clear();
   *live_path = '\0';
   if (!running)
      return;
   memset(items, 0, sizeof(items));
   for (i = 0; i < 4; i++)
   {
      items[i].storage       = RETRO_SAVE_STORAGE_CORE_FILE;
      items[i].root          = RETRO_SAVE_ROOT_SAVE_DIR;
      items[i].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   }
   items[0].id           = "eep";
   items[0].path         = "eep";
   items[0].storage      = RETRO_SAVE_STORAGE_FRONTEND_BUFFER;
   items[0].root         = RETRO_SAVE_ROOT_FRONTEND;
   items[0].segments     = &eep_seg;
   items[0].num_segments = 1;
   items[1].id           = "card";
   items[1].path         = "card.mcd";
   items[2].id           = "cards";
   items[2].path         = "cards";
   items[2].storage      = RETRO_SAVE_STORAGE_CORE_FOLDER;
   items[2].include      = mcd_files;
   items[3].id           = "vmu";
   items[3].path         = "vmu.bin";
   items[3].root         = RETRO_SAVE_ROOT_SYSTEM_DIR;
   memset(&iface, 0, sizeof(iface));
   iface.interface_version = 1;
   iface.num_items         = 4;
   iface.item_size         = sizeof(items[0]);
   iface.items             = items;
   key_path(live_path, sizeof(live_path), "saves/Game.srm");
   memset(&paths, 0, sizeof(paths));
   paths.core                = "Test";
   paths.savefile            = live_path;
   paths.savefile_dir        = save_dir;
   paths.system_dir          = system_dir;
   paths.savefile_dir_is_dir = true;
   check("items: the core registers its save items",
         save_items_register(&iface, &paths));
   save_items_drop_previous();
}

/* A sync of the fixture with Sync: System Files @system_files, the last
 * sync's record @local and the server's manifest @server, through the
 * walk and the diff. */
static task_cloud_sync_state_t *items_sync(retro_task_t *task,
      bool system_files, file_list_t *local, file_list_t *server)
{
   task_cloud_sync_state_t *st       = (task_cloud_sync_state_t*)calloc(1, sizeof(*st));
   settings_t              *settings = (settings_t*)calloc(1, sizeof(*settings));
   unsigned i;

   settings->bools.cloud_sync_sync_saves  = true;
   settings->bools.cloud_sync_sync_system = system_files;
   strlcpy(settings->paths.directory_system, system_dir,
         sizeof(settings->paths.directory_system));
   st->dirlist = task_cloud_sync_directory_map_new(settings);
   free(settings);
   file_list_sort_on_alt(local);
   file_list_sort_on_alt(server);
   st->local_manifest  = local;
   st->server_manifest = server;
   st->destructive     = true;
   strlcpy(st->dir_core_assets, dir, sizeof(st->dir_core_assets));
   retro_atomic_int_init(&st->waiting, 0);
   retro_atomic_int_init(&st->phase, (int)CLOUD_SYNC_PHASE_BUILD_CURRENT_MANIFEST);
   task_cloud_sync_task_setup(task, st, "Cloud Sync in progress");
   grant        = 100;
   num_asked[0] = num_asked[1] = 0;
   for (i = 0; i < 100000
         && (  task_cloud_sync_phase_get(st) == CLOUD_SYNC_PHASE_BUILD_CURRENT_MANIFEST
            || task_cloud_sync_phase_get(st) == CLOUD_SYNC_PHASE_DIFF); i++)
      task_cloud_sync_task_handler(task);
   task_cloud_sync_fold_manifest_adds(st);
   return st;
}

static void items_done(retro_task_t *task)
{
   task_cloud_sync_cleanup(task);
   free(task->title);
   free(task);
}

static bool asked_for(unsigned what, const char *key)
{
   unsigned i;
   for (i = 0; i < num_asked[what] && i < 16; i++)
      if (!strcmp(asked[what][i], key))
         return true;
   return false;
}

static unsigned asked_count(unsigned what, const char *const *keys)
{
   unsigned n = 0;
   for (; *keys; keys++)
      n += asked_for(what, *keys);
   return n;
}

/* How many of @keys the walk listed. */
static unsigned walked(const task_cloud_sync_state_t *st,
      const char *const *keys)
{
   unsigned n = 0;
   size_t   idx;
   for (; *keys; keys++)
      n += file_list_search(st->current_manifest, *keys, &idx);
   return n;
}

/* How many of @keys the sync left alone: neither fetched nor removed,
 * and the server's copy kept for the next sync. */
static unsigned left_alone(const task_cloud_sync_state_t *st,
      const char *const *keys)
{
   unsigned n = 0;
   size_t   i, idx;
   char     path[256];
   for (; *keys; keys++)
   {
      const char *want;
      if (     asked_for(0, *keys)
            || !file_list_search(st->server_manifest, *keys, &idx))
         continue;
      if (!(want = CS_FILE_HASH(&st->server_manifest->list[idx])))
      {
         key_path(path, sizeof(path), *keys);
         n += access(path, F_OK) == 0;
         continue;
      }
      for (i = 0; i < st->updated_server_manifest->size; i++)
         if (     !strcmp(st->updated_server_manifest->list[i].alt, *keys)
               && st->updated_server_manifest->list[i].userdata
               && !strcmp((const char*)st->updated_server_manifest->list[i].userdata, want))
         {
            n++;
            break;
         }
   }
   return n;
}

static file_list_t *new_list(void)
{
   return (file_list_t*)calloc(1, sizeof(file_list_t));
}

/* The newer server copies, over a record of the files as they are. */
static task_cloud_sync_state_t *items_newer_sync(retro_task_t *task)
{
   file_list_t *local  = new_list();
   file_list_t *server = new_list();
   items_files();
   add_keys(local,  save_keys,    'c');
   add_keys(server, owned_keys,   'n');
   add_keys(server, unowned_keys, 'n');
   add_keys(server, deleted_keys, 'd');
   return items_sync(task, false, local, server);
}

/* Newer server copies of the system directory's files. */
static task_cloud_sync_state_t *items_system_sync(retro_task_t *task,
      bool system_files)
{
   file_list_t *local  = new_list();
   file_list_t *server = new_list();
   items_files();
   add_keys(local,  save_keys,   'c');
   add_keys(local,  system_keys, 'c');
   add_keys(server, save_keys,   'c');
   add_keys(server, system_keys, 'n');
   return items_sync(task, system_files, local, server);
}

static void items_lane(void)
{
   char path[256];
   task_cloud_sync_state_t *st;
   retro_task_t *task;

   snprintf(save_dir,   sizeof(save_dir),   "%s/item_saves",  dir);
   snprintf(state_dir,  sizeof(state_dir),  "%s/item_states", dir);
   snprintf(system_dir, sizeof(system_dir), "%s/item_system", dir);
   snprintf(path, sizeof(path), "%s/cards", save_dir);
   path_mkdir(path);
   path_mkdir(state_dir);
   path_mkdir(system_dir);
   items_files();
   write_keys(temp_keys);

   /* no core running: item files sync as save RAM does */
   items_running(false);
   task = task_init();
   st   = items_sync(task, false, new_list(), new_list());
   check("items: their files walked with the save RAM, 7 of 7",
         st->current_manifest->size == 7 && walked(st, save_keys) == 7);
   check("items: new files uploaded, 7 of 7, nothing fetched",
         num_asked[1] == 7 && asked_count(1, save_keys) == 7
         && num_asked[0] == 0);
   check("items: write temps neither walked nor uploaded, 0 of 3",
         walked(st, temp_keys) == 0 && asked_count(1, temp_keys) == 0);
   items_done(task);

   /* the game runs: what its core owns is deferred */
   items_running(true);
   task = task_init();
   st   = items_newer_sync(task);
   check("items: a running core's 5 files (one server-only) not fetched",
         left_alone(st, owned_keys) == 5);
   check("items: ... nor its file the server deleted removed, 1 of 1",
         left_alone(st, deleted_keys) == 1);
   check("items: the 2 files no core owns fetched, nothing else",
         num_asked[0] == 2 && asked_count(0, unowned_keys) == 2
         && num_asked[1] == 0);
   items_done(task);

   /* an item in the system directory */
   task = task_init();
   st   = items_system_sync(task, false);
   check("items: System Files off, the system directory not walked",
         st->dirlist->size == 2 && st->current_manifest->size == 7
         && walked(st, system_keys) == 0);
   check("items: ... nor its 2 newer server copies fetched",
         num_asked[0] == 0 && num_asked[1] == 0 && st->server_skipped == 2);
   items_done(task);
   task = task_init();
   st   = items_system_sync(task, true);
   check("items: System Files on, the item there walked, 9 files",
         st->dirlist->size == 3 && st->current_manifest->size == 9
         && walked(st, system_keys) == 2);
   check("items: ... deferred while the core runs, the BIOS fetched",
         left_alone(st, system_keys) == 1
         && num_asked[0] == 1 && asked_for(0, "system/bios.bin")
         && num_asked[1] == 0);
   items_done(task);

   /* the game closed; Game.eep's last image is still being written */
   items_running(false);
   key_path(pending_path, sizeof(pending_path), "saves/Game.eep");
   task = task_init();
   st   = items_newer_sync(task);
   check("items: a pending write deferred, the 6 other copies fetched",
         left_alone(st, owned_keys) == 1 && !asked_for(0, "saves/Game.eep")
         && num_asked[0] == 6);
   key_path(path, sizeof(path), "saves/cards/b.mcd");
   check("items: ... and the file the server deleted removed",
         left_alone(st, deleted_keys) == 0 && access(path, F_OK) != 0);
   items_done(task);
   *pending_path = '\0';

   unlink_keys(save_keys);
   unlink_keys(owned_keys);
   unlink_keys(temp_keys);
   unlink_keys(system_keys);
   snprintf(path, sizeof(path), "%s/cards", save_dir);
   rmdir(path);
   rmdir(save_dir);
   rmdir(state_dir);
   rmdir(system_dir);
   *save_dir = *state_dir = '\0';
}

/* What a sync's pusher is told when it is done. */
static unsigned done_calls;
static int      done_failed;
static int      done_had_state;

static void on_sync_done(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   (void)task_data;
   done_calls++;
   done_failed    = err != NULL;
   done_had_state = task->state != NULL && user_data == (void*)&done_calls;
}

/* A whole sync's tail through the real task queue: the diff, the local
 * manifest and the end, then retirement. Run under LeakSanitizer in CI.
 * @ends: whether the driver ends the sync. */
static void queue_run(int ends)
{
   task_cloud_sync_state_t *st   = (task_cloud_sync_state_t*)calloc(1, sizeof(*st));
   retro_task_t            *task = task_init();
   struct string_list      *dl   = string_list_new();
   union string_list_elem_attr attr;
   char     path[128];
   unsigned i;

   attr.i = 0;
   string_list_append(dl, "saves", attr);
   dl->elems[0].userdata = strdup(dir);
   st->dirlist                 = dl;
   st->server_manifest         = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->local_manifest          = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->current_manifest        = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->updated_server_manifest = (file_list_t*)calloc(1, sizeof(file_list_t));
   st->updated_local_manifest  = (file_list_t*)calloc(1, sizeof(file_list_t));
   strlcpy(st->dir_core_assets, dir, sizeof(st->dir_core_assets));
   retro_atomic_int_init(&st->waiting, 0);
   retro_atomic_int_init(&st->phase, (int)CLOUD_SYNC_PHASE_DIFF);
   st->cb      = on_sync_done;
   st->cb_data = &done_calls;
   snprintf(path, sizeof(path), "%s/q.srm", dir);
   write_file(path, 300 * 1024, 3);
   list_add(st->server_manifest,  "saves/q.srm", NULL, hash_of(path));
   list_add(st->local_manifest,   "saves/q.srm", NULL, hash_of(path));
   list_add(st->current_manifest, "saves/q.srm", path, NULL);

   /* set up as task_push_cloud_sync() sets up every sync */
   task_cloud_sync_task_setup(task, st, "Cloud Sync in progress");
   grant      = 2;
   end_ok     = ends;
   done_calls = 0;

   task_queue_init(false, NULL);
   task_queue_push(task);
   for (i = 0; i < 10000; i++)
      task_queue_check();
   task_queue_deinit();
   end_ok = 0;

   unlink(path);
   snprintf(path, sizeof(path), "%s/manifest.local", dir);
   unlink(path);
   snprintf(path, sizeof(path), "%s/manifest.local.tmp", dir);
   unlink(path);
}

static void queue_lane(void)
{
   queue_run(1);
   check("queue: the sync ran to its end and retired", 1);
   check("queue: a clean sync tells its pusher once, with no error, before its state goes",
         done_calls == 1 && !done_failed && done_had_state);

   queue_run(0);
   check("queue: a sync the driver could not end tells its pusher it failed",
         done_calls == 1 && done_failed);
}

/* A sync whose driver cannot begin: its pusher hears it failed. */
static void begin_lane(void)
{
   task_cloud_sync_state_t *st   = (task_cloud_sync_state_t*)calloc(1, sizeof(*st));
   retro_task_t            *task = task_init();
   unsigned i;

   retro_atomic_int_init(&st->waiting, 0);
   retro_atomic_int_init(&st->phase, (int)CLOUD_SYNC_PHASE_BEGIN);
   st->cb      = on_sync_done;
   st->cb_data = &done_calls;
   task_cloud_sync_task_setup(task, st, "Cloud Sync in progress");
   done_calls = 0;

   task_queue_init(false, NULL);
   task_queue_push(task);
   for (i = 0; i < 100; i++)
      task_queue_check();
   task_queue_deinit();
   check("begin: a sync that could not begin tells its pusher it failed",
         done_calls == 1 && done_failed);
}

/* Resolve: Keep Local chosen while an automatic sync runs. It was
 * turned away with a log line and nothing on screen; it runs once that
 * sync is through now, keeping its resolution over a plain Sync Now
 * asked for after it. */
static void followup_lane(void)
{
   static settings_t s;
   unsigned i;

   memset(&s, 0, sizeof(s));
   s.bools.cloud_sync_enable = true;
   strlcpy(s.paths.directory_core_assets, dir,
         sizeof(s.paths.directory_core_assets));
   test_settings     = &s;
   begins            = 0;
   tcs_followup_mode = -1;

   task_queue_init(false, NULL);
   check("follow-up: the automatic sync is pushed",
         task_push_cloud_sync(NULL, NULL));
   task_push_cloud_sync_resolve_keep_local();
   check("follow-up: Sync Now during it is taken, not refused",
         task_push_cloud_sync(NULL, NULL));
   check("follow-up: and keeps the resolution asked for before it",
         tcs_followup_mode == 1);
   for (i = 0; i < 1000; i++)
      task_queue_check();
   check("follow-up: one more sync runs once the first is through",
         begins == 2);
   check("follow-up: and none is left owed", tcs_followup_mode == -1);
   task_queue_deinit();
   test_settings = NULL;
}

int main(void)
{
   unsigned r;
   int      ok;

   tcs_test_lock();
   strcpy(dir, "cs_pacing_XXXXXX");
   if (!mkdtemp(dir))
      return 2;

   printf("cloudsync_pacing_test\n");

   /* one 9 MiB file: 36 chunks; 3 units per run */
   r = run_diff(1, 9 * 1024 * 1024, 3, &ok);
   check("large file: hash matches a one-shot MD5, manifests complete", ok);
   check("large file: spread over runs, not hashed in one", r >= 36 / 4);
   check("large file: no run past its grant", max_granted <= 4);
   check("large file: nothing transferred", transfers == 0);

   /* 40 small files, 10 units per run */
   r = run_diff(40, 1000, 10, &ok);
   check("small files: hashes match, manifests complete", ok);
   check("small files: several per run", r <= 40 / 3);
   check("small files: no run past its grant", max_granted <= 11);

   /* a window that is always spent: the floor alone carries it */
   r = run_diff(5, 600 * 1024, 0, &ok);
   check("spent window: every run progresses, the diff ends", ok && r < 100000);
   check("spent window: one unit per run", max_granted == 1);

   fetch_lane();
   fetch_keep_lane(1,
         "fetch: a save a core took during the download is left as it was",
         "fetch: ... and the server's copy waits for the next sync");
   fetch_keep_lane(0,
         "fetch: a download cut off part-way leaves the save as it was",
         "fetch: ... and is fetched again next sync");
   walk_lane();
   items_lane();
   queue_lane();
   begin_lane();
   followup_lane();

   rmdir(dir);
   printf("cloudsync_pacing_test: %s\n", fails ? "FAIL" : "PASS");
   return fails ? 1 : 0;
}
