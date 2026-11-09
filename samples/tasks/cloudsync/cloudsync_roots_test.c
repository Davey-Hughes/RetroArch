/* Whole syncs of the real cloud-sync task over a folder standing in
 * for the server, threaded tasks on and off: what the sync deletes on
 * the server when a folder under a root can't be read, when a folder
 * is unset, when a root moved, when most of a root's files are
 * missing, when Keep Local resolves a file deleted here, and when a
 * state is only renamed aside for a moment; what the manifests keep
 * of a delete the server refuses, of one taken from it and of an
 * upload or a download that does not happen; that a save which can't
 * be read or hashed fails the sync; and which save folder it walks.
 *
 *   ./cloudsync_roots_test [lane...]   (default: every lane) */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <retro_dirent.h>
#include <retro_timers.h>
#include <streams/file_stream.h>
#include <queues/message_queue.h>
#include <lists/string_list.h>
#include "../../../msg_hash.h"

static struct RDIR *test_opendir(const char *name, bool include_hidden);
static int test_readdir(struct RDIR *rdir);
static bool test_dirent_error(struct RDIR *rdir);
static char *test_strdup(const char *s);
static RFILE *test_filestream_open(const char *path, unsigned mode,
      unsigned hints);
static bool test_string_list_append(struct string_list *list,
      const char *elem, union string_list_elem_attr attr);
static void *test_malloc(size_t size);
static int test_filestream_rename(const char *old_path,
      const char *new_path);
static int64_t test_filestream_view(RFILE *stream, int64_t len,
      const uint8_t **ptr);
static int64_t test_filestream_read(RFILE *stream, void *data,
      int64_t len);
#define retro_opendir_include_hidden test_opendir
#define retro_readdir test_readdir
#define retro_dirent_error test_dirent_error
#define strdup test_strdup
#define filestream_open test_filestream_open
#define string_list_append test_string_list_append
#define malloc test_malloc
#define filestream_rename test_filestream_rename
#define filestream_view test_filestream_view
#define filestream_read test_filestream_read
#include "../../../tasks/task_cloudsync.c"
#undef retro_opendir_include_hidden
#undef retro_readdir
#undef retro_dirent_error
#undef strdup
#undef filestream_open
#undef string_list_append
#undef malloc
#undef filestream_rename
#undef filestream_view
#undef filestream_read

/* A copy of this string runs out of memory */
static const char *fail_strdup;
static char *test_strdup(const char *s)
{
   if (fail_strdup && !strcmp(s, fail_strdup))
      return NULL;
   return strdup(s);
}

/* The task's opens of a file whose path ends in this fail, after the
 * first fail_fopen_after */
static const char *fail_fopen;
static unsigned fail_fopen_after;
static RFILE *test_filestream_open(const char *path, unsigned mode,
      unsigned hints)
{
   if (fail_fopen && string_ends_with(path, fail_fopen))
   {
      if (!fail_fopen_after)
         return NULL;
      fail_fopen_after--;
   }
   return filestream_open(path, mode, hints);
}

/* Faults the hooks below made */
static unsigned faults;

/* The task's next allocation of this size fails, and every one while
 * fail_alloc_all */
static size_t fail_alloc;
static bool fail_alloc_all;
static void *test_malloc(size_t size)
{
   if (fail_alloc && size == fail_alloc)
   {
      if (!fail_alloc_all)
         fail_alloc = 0;
      faults++;
      return NULL;
   }
   return malloc(size);
}

/* With no_view the task's files can't be mapped, as on targets without
 * mappings; its reads of a file whose path holds fail_fread fail */
static bool no_view;
static const char *fail_fread;
static int64_t test_filestream_view(RFILE *stream, int64_t len,
      const uint8_t **ptr)
{
   if (no_view)
      return -1;
   return filestream_view(stream, len, ptr);
}

static int64_t test_filestream_read(RFILE *stream, void *data,
      int64_t len)
{
   const char *path = filestream_get_path(stream);
   if (fail_fread && path && strstr(path, fail_fread))
   {
      faults++;
      return -1;
   }
   return filestream_read(stream, data, len);
}

/* The task's renames onto a path ending in this fail */
static const char *fail_rename;
static int test_filestream_rename(const char *old_path,
      const char *new_path)
{
   if (fail_rename && string_ends_with(new_path, fail_rename))
      return -1;
   return filestream_rename(old_path, new_path);
}

/* Adding this string to a list runs out of memory */
static const char *fail_append;
static bool test_string_list_append(struct string_list *list,
      const char *elem, union string_list_elem_attr attr)
{
   if (fail_append && !strcmp(elem, fail_append))
      return false;
   return string_list_append(list, elem, attr);
}

static const char *base = "cloudsync_roots_test_files";
static char srv[256], assets[256];
static int failures;
static unsigned deletes, uploads, fetches, notices, notice_count;
static int sync_done, sync_failed;
static int conflict_mode;

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

void RARCH_LOG(const char *fmt, ...) { (void)fmt; }
/* Warnings that name this */
static const char *warn_about;
static unsigned warned;
void RARCH_WARN(const char *fmt, ...)
{
   char    s[512];
   va_list ap;
   if (!warn_about)
      return;
   va_start(ap, fmt);
   vsnprintf(s, sizeof(s), fmt, ap);
   va_end(ap);
   if (strstr(s, warn_about))
      warned++;
}
void RARCH_ERR(const char *fmt, ...) { (void)fmt; }
bool verbosity_is_enabled(void) { return false; }
settings_t *config_get_ptr(void) { return NULL; }
/* For a lane that lists its folders as the frontend does: the main
 * config's save and state folders and the ones in use. */
static char main_saves[256], live_saves[256];
static char main_states[256], live_states[256];
char *dir_get_ptr(enum rarch_dir_type type)
{
   static char unset[1];
   switch (type)
   {
      case RARCH_DIR_SAVEFILE_BASE:
         return main_saves;
      case RARCH_DIR_SAVEFILE:
         return live_saves;
      case RARCH_DIR_SAVESTATE_BASE:
         return main_states;
      case RARCH_DIR_SAVESTATE:
         return live_states;
      default:
         break;
   }
   return unset;
}
/* A save whose path ends in this belongs to the running core */
static const char *live_save;
bool content_savefile_is_live(const char *path)
{ return live_save && string_ends_with(path, live_save); }
unsigned content_savefile_lifetime(void) { return 0; }
unsigned content_save_writes(void) { return 0; }
static unsigned state_fetches;
void content_state_fetched(void) { state_fetches++; }
bool content_save_state_in_progress(void *data) { (void)data; return false; }
void runloop_save_load_release(uint8_t bit) { (void)bit; }
void task_window_progress_cb(retro_task_t *task) { (void)task; }
size_t fill_pathname_application_special(char *s, size_t len,
      enum application_special_type type)
{ (void)s; (void)len; (void)type; return 0; }
const char *msg_hash_to_str(enum msg_hash_enums msg)
{ (void)msg; return "%u"; }
void runloop_msg_queue_push(const char *msg, size_t len,
      unsigned prio, unsigned duration, bool flush,
      char *title, enum message_queue_icon icon,
      enum message_queue_category category)
{
   (void)len; (void)prio; (void)duration; (void)flush; (void)title;
   (void)icon; (void)category;
   notices++;
   notice_count = (unsigned)strtoul(msg, NULL, 10);
}

/* A run of the handler gets a bounded share of the I/O window. */
static unsigned units;
void task_nbio_slice_open(nbio_budget_t *b) { b->floor = 1; units = 0; }
void task_nbio_slice_close(nbio_budget_t *b) { (void)b; }
bool task_nbio_slice_within_budget(void *ud, size_t avail, size_t len)
{ (void)ud; (void)avail; (void)len; return ++units <= 64; }

/* Faults in the walk: a folder that can't be opened, and one whose
 * first read fails. A folder outside the test's own is never opened,
 * only counted. */
static const char *fail_open, *fail_read;
static struct RDIR *read_dir;
static bool read_failed;
static unsigned outside_opens;
/* Runs once the walk has read this folder (with its slash) to its end */
static const char *walked_at;
static void (*walked)(void);
static struct RDIR *walked_dir;

static struct RDIR *test_opendir(const char *name, bool include_hidden)
{
   struct RDIR *d;
   if (strncmp(name, base, strlen(base)))
   {
      outside_opens++;
      return NULL;
   }
   if (fail_open && strstr(name, fail_open))
      return NULL;
   d = retro_opendir_include_hidden(name, include_hidden);
   if (d && fail_read && strstr(name, fail_read))
   {
      read_dir    = d;
      read_failed = false;
   }
   if (     d && walked_at && string_starts_with(name, walked_at)
         && strlen(name) == strlen(walked_at) + 1)
      walked_dir = d;
   return d;
}

static int test_readdir(struct RDIR *rdir)
{
   int more;
   if (rdir == read_dir)
   {
      read_failed = true;
      return 0;
   }
   more = retro_readdir(rdir);
   if (!more && rdir == walked_dir)
   {
      walked_dir = NULL;
      walked();
   }
   return more;
}

static bool test_dirent_error(struct RDIR *rdir)
{
   if (rdir == read_dir && read_failed)
      return true;
   return retro_dirent_error(rdir);
}

/* The server: a key's file is <srv>/<key>. */
static void srv_path(char *s, size_t len, const char *key)
{
   fill_pathname_join_special(s, srv, key, len);
   pathname_conform_slashes_to_os(s);
}

static const char *srv_key(const char *key)
{
   static char s[PATH_MAX_LENGTH];
   srv_path(s, sizeof(s), key);
   return s;
}

static bool copy_file(const char *from, const char *to)
{
   char    dir[PATH_MAX_LENGTH];
   void   *buf = NULL;
   int64_t len = 0;
   bool    ok;
   if (!filestream_read_file(from, &buf, &len))
      return false;
   fill_pathname_basedir(dir, to, sizeof(dir));
   path_mkdir(dir);
   ok = filestream_write_file(to, buf, len);
   free(buf);
   return ok;
}

void cloud_sync_find_driver(const char *drv, const char *prefix, bool verbose)
{ (void)drv; (void)prefix; (void)verbose; }
void cloud_sync_capture(void) { }
void cloud_sync_poll(cloud_sync_poll_budget_t within, void *budget)
{ (void)within; (void)budget; }
bool cloud_sync_begin(cloud_sync_complete_handler_t cb, void *ud)
{ cb(ud, NULL, true, NULL); return true; }
bool cloud_sync_end(cloud_sync_complete_handler_t cb, void *ud)
{ cb(ud, NULL, true, NULL); return true; }

/* Downloads of this key fail, at the call with fail_fetch_call */
static const char *fail_fetch;
static bool fail_fetch_call;

bool cloud_sync_read(const char *path, const char *file,
      cloud_sync_complete_handler_t cb, void *ud)
{
   char   s[PATH_MAX_LENGTH];
   RFILE *f = NULL;
   if (fail_fetch && string_is_equal(path, fail_fetch))
   {
      if (fail_fetch_call)
         return false;
      cb(ud, path, false, NULL);
      return true;
   }
   srv_path(s, sizeof(s), path);
   if (!path_is_valid(s))
   {
      cb(ud, path, true, NULL);
      return true;
   }
   if (copy_file(s, file))
      f = filestream_open(file, RETRO_VFS_FILE_ACCESS_READ,
            RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (f && !string_is_equal(path, MANIFEST_FILENAME_SERVER))
      fetches++;
   cb(ud, path, f != NULL, f);
   return true;
}

/* Uploads of this key fail, at the call with fail_update_call */
static const char *fail_update;
static bool fail_update_call;

bool cloud_sync_update(const char *path, RFILE *file,
      cloud_sync_complete_handler_t cb, void *ud)
{
   char s[PATH_MAX_LENGTH];
   bool ok;
   if (fail_update && string_is_equal(path, fail_update))
   {
      if (fail_update_call)
         return false;
      cb(ud, path, false, file);
      return true;
   }
   srv_path(s, sizeof(s), path);
   ok = copy_file(filestream_get_path(file), s);
   if (ok && !string_is_equal(path, MANIFEST_FILENAME_SERVER))
      uploads++;
   cb(ud, path, ok, file);
   return true;
}

/* The server refuses to delete the keys starting with this */
static const char *fail_free;

bool cloud_sync_free(const char *path, cloud_sync_complete_handler_t cb,
      void *ud)
{
   char s[PATH_MAX_LENGTH];
   srv_path(s, sizeof(s), path);
   deletes++;
   if (fail_free && !strncmp(path, fail_free, strlen(fail_free)))
   {
      cb(ud, path, false, NULL);
      return true;
   }
   cb(ud, path, filestream_delete(s) == 0, NULL);
   return true;
}

static void remove_tree(const char *dir)
{
   struct string_list *l = dir_list_new(dir, NULL, true, true, false, true);
   unsigned pass;
   size_t i;
   if (!l)
      return;
   for (i = 0; i < l->size; i++)
      if (!path_is_directory(l->elems[i].data))
         filestream_delete(l->elems[i].data);
   for (pass = 0; pass < 8; pass++)
      for (i = 0; i < l->size; i++)
         if (path_is_directory(l->elems[i].data))
            filestream_delete(l->elems[i].data);
   string_list_free(l);
   filestream_delete(dir);
}

static void put(const char *root, const char *rel, const char *text)
{
   char path[PATH_MAX_LENGTH], dir[PATH_MAX_LENGTH];
   fill_pathname_join_special(path, root, rel, sizeof(path));
   pathname_conform_slashes_to_os(path);
   fill_pathname_basedir(dir, path, sizeof(dir));
   path_mkdir(dir);
   CHECK(filestream_write_file(path, text, strlen(text)));
}

static void drop(const char *root, const char *rel)
{
   char path[PATH_MAX_LENGTH];
   fill_pathname_join_special(path, root, rel, sizeof(path));
   pathname_conform_slashes_to_os(path);
   CHECK(filestream_delete(path) == 0);
}

static bool holds(const char *root, const char *rel, const char *text)
{
   char    path[PATH_MAX_LENGTH];
   void   *buf = NULL;
   int64_t len = 0;
   bool    same;
   fill_pathname_join_special(path, root, rel, sizeof(path));
   pathname_conform_slashes_to_os(path);
   if (!filestream_read_file(path, &buf, &len))
      return false;
   same = len == (int64_t)strlen(text) && !memcmp(buf, text, (size_t)len);
   free(buf);
   return same;
}

static void roots_path(char *s, size_t len)
{
   fill_pathname_join_special(s, assets, "manifest.local.roots", len);
}

/* The roots file a sync wrote names @root. */
static bool roots_name(const char *root)
{
   char    path[PATH_MAX_LENGTH];
   void   *buf = NULL;
   int64_t len = 0;
   bool    found;
   roots_path(path, sizeof(path));
   if (!filestream_read_file(path, &buf, &len))
      return false;
   found = strstr((const char*)buf, root) != NULL;
   free(buf);
   return found;
}

static void on_done(retro_task_t *task, void *data, void *ud,
      const char *error)
{
   (void)task; (void)data; (void)ud;
   sync_done   = 1;
   sync_failed = error != NULL;
}

/* One sync of the folders in @dirlist, which it takes; true when it
 * finished clean. */
static bool sync_list(bool threaded, struct string_list *dirlist)
{
   retro_task_t            *task = task_init();
   task_cloud_sync_state_t *st   = (task_cloud_sync_state_t*)calloc(1, sizeof(*st));
   unsigned i;
   st->dirlist = dirlist;
   st->conflict_resolution = conflict_mode;
   strlcpy(st->dir_core_assets, assets, sizeof(st->dir_core_assets));
   retro_atomic_int_init(&st->waiting, 0);
   retro_atomic_int_init(&st->phase, CLOUD_SYNC_PHASE_BEGIN);
   st->cb      = on_done;
   task_cloud_sync_task_setup(task, st, "Cloud Sync");
   sync_done   = 0;
   sync_failed = 0;
   deletes = uploads = fetches = notices = notice_count = outside_opens = 0;
   warned  = 0;
   task_queue_init(threaded, NULL);
   task_queue_push(task);
   for (i = 0; i < 5000 && !sync_done; i++)
   {
      task_queue_check();
      retro_sleep(1);
   }
   task_queue_deinit();
   CHECK(sync_done);
   return sync_done && !sync_failed;
}

/* One sync of "saves" from @saves_root. */
static bool sync_saves(bool threaded, const char *saves_root)
{
   struct string_list *l = string_list_new();
   union string_list_elem_attr attr;
   attr.i = 0;
   string_list_append(l, "saves", attr);
   l->elems[0].userdata = strdup(saves_root);
   return sync_list(threaded, l);
}

/* A fresh server, core assets folder and local root for a lane. */
static void lane_dirs(const char *lane, bool threaded, char *root,
      size_t len)
{
   char dir[192];
   snprintf(dir, sizeof(dir), "%s/%s-%d", base, lane, threaded ? 1 : 0);
   remove_tree(dir);
   snprintf(srv, sizeof(srv), "%s/server", dir);
   snprintf(assets, sizeof(assets), "%s/assets", dir);
   snprintf(root, len, "%s/saves", dir);
   CHECK(path_mkdir(srv));
   CHECK(path_mkdir(assets));
   CHECK(path_mkdir(root));
}

static const char *snes[] = { "Snes9x/a.srm", "Snes9x/b.srm", "Snes9x/c.srm" };

/* A folder under the root that can't be opened, or read to its
 * end, says nothing about its files: the sync stops. */
static void unreadable_lane(bool threaded, bool open_fails)
{
   char     root[256];
   unsigned i;
   lane_dirs(open_fails ? "unreadable" : "readdir", threaded, root,
         sizeof(root));
   for (i = 0; i < 3; i++)
      put(root, snes[i], "s");
   put(root, "top1.srm", "t");
   put(root, "top2.srm", "t");
   CHECK(sync_saves(threaded, root));
   CHECK(uploads == 5);
   if (open_fails)
      fail_open = "Snes9x";
   else
      fail_read = "Snes9x";
   CHECK(!sync_saves(threaded, root));
   fail_open = fail_read = NULL;
   read_dir  = NULL;
   CHECK(deletes == 0);
   CHECK(uploads == 0);
}

/* An unset folder is left out of the sync: nothing outside the
 * test's folder is opened, its keys on the server stay and the other
 * folders still sync. */
static void empty_root_lane(bool threaded)
{
   char        root[256], sys[256], kept[PATH_MAX_LENGTH];
   settings_t *settings = (settings_t*)calloc(1, sizeof(*settings));
   lane_dirs("empty", threaded, root, sizeof(root));
   snprintf(sys, sizeof(sys), "%s/system-%d", base, threaded ? 1 : 0);
   strlcpy(main_saves, root, sizeof(main_saves));
   put(root, "a.srm", "s");
   put(sys, "bios.bin", "b");
   settings->bools.cloud_sync_sync_saves  = true;
   settings->bools.cloud_sync_sync_system = true;
   strlcpy(settings->paths.directory_system, sys,
         sizeof(settings->paths.directory_system));
   CHECK(sync_list(threaded, task_cloud_sync_directory_map_new(settings)));
   CHECK(uploads == 2);

   settings->paths.directory_system[0] = '\0';
   put(root, "b.srm", "s");
   CHECK(sync_list(threaded, task_cloud_sync_directory_map_new(settings)));
   CHECK(outside_opens == 0);
   CHECK(uploads == 1);
   CHECK(deletes == 0);
   srv_path(kept, sizeof(kept), "system/bios.bin");
   CHECK(path_is_valid(kept));

   /* set again, to another folder: a first sync of it */
   snprintf(sys, sizeof(sys), "%s/system-b-%d", base, threaded ? 1 : 0);
   CHECK(path_mkdir(sys));
   strlcpy(settings->paths.directory_system, sys,
         sizeof(settings->paths.directory_system));
   CHECK(sync_list(threaded, task_cloud_sync_directory_map_new(settings)));
   CHECK(deletes == 0);
   CHECK(fetches == 1);
   main_saves[0] = '\0';
   free(settings);
}

/* The save folder moved to an empty folder holding one
 * different save. Nothing is deleted on the server; what it has comes
 * down; the different save is a conflict. */
static void root_changed(bool threaded)
{
   char root_a[256], root_b[264], roots[PATH_MAX_LENGTH], json[300];
   lane_dirs("root_changed", threaded, root_a, sizeof(root_a));
   snprintf(root_b, sizeof(root_b), "%s-b", root_a);
   remove_tree(root_b);
   CHECK(path_mkdir(root_b));
   put(root_a, "a.srm", "1");
   put(root_a, "b.srm", "2");
   put(root_a, "c.srm", "3");
   CHECK(sync_saves(threaded, root_a));
   CHECK(uploads == 3);
   CHECK(roots_name(root_a));
   /* as a sync that walked A writes it */
   roots_path(roots, sizeof(roots));
   snprintf(json, sizeof(json), "{\"saves\": \"%s\"}\n", root_a);
   CHECK(filestream_write_file(roots, json, strlen(json)));
   put(root_b, "a.srm", "X");
   CHECK(!sync_saves(threaded, root_b));
   CHECK(deletes == 0);
   CHECK(uploads == 0);
   CHECK(fetches == 2);
   CHECK(holds(root_b, "a.srm", "X"));
   CHECK(holds(root_b, "b.srm", "2"));
   CHECK(holds(root_b, "c.srm", "3"));
   CHECK(holds(srv, "saves/a.srm", "1"));
   CHECK(roots_name(root_b));
   remove_tree(root_b);
}

/* A manifest from before the roots file: the sync runs as it always
 * did, and writes the roots file, which the next sync reads. */
static void no_roots_file(bool threaded)
{
   char root[256], root_b[264], roots[PATH_MAX_LENGTH];
   lane_dirs("no_roots", threaded, root, sizeof(root));
   put(root, "a.srm", "1");
   put(root, "b.srm", "2");
   put(root, "c.srm", "3");
   put(root, "d.srm", "4");
   CHECK(sync_saves(threaded, root));
   roots_path(roots, sizeof(roots));
   filestream_delete(roots);
   drop(root, "a.srm");
   put(root, "b.srm", "22");
   CHECK(sync_saves(threaded, root));
   CHECK(deletes == 1);
   CHECK(uploads == 1);
   CHECK(fetches == 0);
   CHECK(!path_is_valid(srv_key("saves/a.srm")));
   CHECK(holds(srv, "saves/b.srm", "22"));
   CHECK(roots_name(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   remove_tree(root_b);
   CHECK(path_mkdir(root_b));
   CHECK(sync_saves(threaded, root_b));
   CHECK(deletes == 0);
   CHECK(fetches == 3);
   remove_tree(root_b);
}

/* A folder unset at the first sync that writes the roots file has no
 * entry to carry: set again, to another folder, it is a first sync of
 * it. */
static void unset_no_entry(bool threaded)
{
   char        root[256], sys[272], roots[PATH_MAX_LENGTH];
   settings_t *settings = (settings_t*)calloc(1, sizeof(*settings));
   lane_dirs("unset_no_entry", threaded, root, sizeof(root));
   snprintf(sys, sizeof(sys), "%s-system", root);
   strlcpy(main_saves, root, sizeof(main_saves));
   put(root, "a.srm", "s");
   put(sys, "bios.bin", "b");
   settings->bools.cloud_sync_sync_saves  = true;
   settings->bools.cloud_sync_sync_system = true;
   strlcpy(settings->paths.directory_system, sys,
         sizeof(settings->paths.directory_system));
   CHECK(sync_list(threaded, task_cloud_sync_directory_map_new(settings)));
   CHECK(uploads == 2);
   /* as before the roots file */
   roots_path(roots, sizeof(roots));
   CHECK(filestream_delete(roots) == 0);

   settings->paths.directory_system[0] = '\0';
   CHECK(sync_list(threaded, task_cloud_sync_directory_map_new(settings)));
   CHECK(deletes == 0);

   snprintf(sys, sizeof(sys), "%s-system-b", root);
   CHECK(path_mkdir(sys));
   strlcpy(settings->paths.directory_system, sys,
         sizeof(settings->paths.directory_system));
   CHECK(sync_list(threaded, task_cloud_sync_directory_map_new(settings)));
   CHECK(deletes == 0);
   CHECK(fetches == 1);
   CHECK(holds(srv, "system/bios.bin", "b"));
   main_saves[0] = '\0';
   free(settings);
}

/* Out of memory reading the roots file: the sync stops rather than
 * take a moved folder as unchanged. */
static void roots_no_memory(bool threaded)
{
   char root_a[256], root_b[264];
   lane_dirs("roots_no_memory", threaded, root_a, sizeof(root_a));
   snprintf(root_b, sizeof(root_b), "%s-b", root_a);
   CHECK(path_mkdir(root_b));
   put(root_a, "a.srm", "1");
   put(root_a, "b.srm", "2");
   CHECK(sync_saves(threaded, root_a));
   fail_strdup = root_a;
   CHECK(!sync_saves(threaded, root_b));
   fail_strdup = NULL;
   CHECK(deletes == 0);
   CHECK(fetches == 0);
   CHECK(holds(srv, "saves/a.srm", "1"));
   CHECK(roots_name(root_a));
}

/* Sync walks the main config's save and state folders, not the ones
 * in use (an override's or the command line's): other folders in use
 * are no root change. */
static void main_folder_only(bool threaded)
{
   char        root[256], live_a[264], live_b[264];
   char        states[264], states_a[264], states_b[264];
   settings_t *settings = (settings_t*)calloc(1, sizeof(*settings));
   lane_dirs("main_only", threaded, root, sizeof(root));
   snprintf(live_a, sizeof(live_a), "%s-a", root);
   snprintf(live_b, sizeof(live_b), "%s-b", root);
   snprintf(states, sizeof(states), "%s-st", root);
   snprintf(states_a, sizeof(states_a), "%s-st-a", root);
   snprintf(states_b, sizeof(states_b), "%s-st-b", root);
   put(root, "a.srm", "1");
   put(live_a, "o.srm", "2");
   put(live_b, "p.srm", "3");
   put(states, "a.state", "4");
   put(states_a, "o.state", "5");
   put(states_b, "p.state", "6");
   strlcpy(main_saves, root, sizeof(main_saves));
   strlcpy(live_saves, live_a, sizeof(live_saves));
   strlcpy(main_states, states, sizeof(main_states));
   strlcpy(live_states, states_a, sizeof(live_states));
   settings->bools.cloud_sync_sync_saves = true;
   CHECK(sync_list(threaded, task_cloud_sync_directory_map_new(settings)));
   CHECK(uploads == 2);
   CHECK(holds(srv, "saves/a.srm", "1"));
   CHECK(!path_is_valid(srv_key("saves/o.srm")));
   CHECK(holds(srv, "states/a.state", "4"));
   CHECK(!path_is_valid(srv_key("states/o.state")));
   CHECK(roots_name(root));
   CHECK(!roots_name(live_a));
   CHECK(roots_name(states));
   CHECK(!roots_name(states_a));

   strlcpy(live_saves, live_b, sizeof(live_saves));
   strlcpy(live_states, states_b, sizeof(live_states));
   CHECK(sync_list(threaded, task_cloud_sync_directory_map_new(settings)));
   CHECK(uploads == 0);
   CHECK(fetches == 0);
   CHECK(deletes == 0);
   CHECK(!path_is_valid(srv_key("saves/p.srm")));
   CHECK(holds(live_b, "p.srm", "3"));
   CHECK(!roots_name(live_b));
   CHECK(!path_is_valid(srv_key("states/p.state")));
   CHECK(holds(states_b, "p.state", "6"));
   CHECK(!roots_name(states_b));
   main_saves[0] = live_saves[0] = '\0';
   main_states[0] = live_states[0] = '\0';
   free(settings);
}

static void name_file(char *s, size_t len, unsigned i)
{
   snprintf(s, len, "f%02u.srm", i);
}

/* Of @n files under one root, @gone are missing at the next sync: more
 * than 10 and more than half are held on the server, and come back
 * down at the sync after. */
static void mass_lane(bool threaded, unsigned n, unsigned gone, bool held)
{
   char     root[256], name[32], key[48];
   unsigned i;
   lane_dirs("mass", threaded, root, sizeof(root));
   for (i = 0; i < n; i++)
   {
      name_file(name, sizeof(name), i);
      put(root, name, name);
   }
   CHECK(sync_saves(threaded, root));
   CHECK(uploads == n);
   for (i = 0; i < gone; i++)
   {
      name_file(name, sizeof(name), i);
      drop(root, name);
   }
   /* an upload rewrites the server manifest in the same sync */
   put(root, "zz.srm", "zz");
   CHECK(sync_saves(threaded, root));
   if (!held)
   {
      CHECK(deletes == gone);
      CHECK(notices == 0);
      return;
   }
   CHECK(deletes == 0);
   CHECK(notices == 1);
   CHECK(notice_count == gone);
   CHECK(roots_name("\"held\""));
   CHECK(roots_name("saves/f00.srm"));
   for (i = 0; i < gone; i++)
   {
      snprintf(key, sizeof(key), "saves/f%02u.srm", i);
      CHECK(path_is_valid(srv_key(key)));
   }
   /* not applied: the next sync fetches them back */
   CHECK(sync_saves(threaded, root));
   CHECK(deletes == 0);
   CHECK(fetches == gone);
   CHECK(notices == 0);
   CHECK(!roots_name("saves/f00.srm"));
   for (i = 0; i < gone; i++)
   {
      name_file(name, sizeof(name), i);
      CHECK(holds(root, name, name));
   }
}

static bool sync_two(bool threaded, const char *saves, const char *states)
{
   struct string_list *l = string_list_new();
   union string_list_elem_attr attr;
   attr.i = 0;
   string_list_append(l, "saves", attr);
   l->elems[0].userdata = strdup(saves);
   string_list_append(l, "states", attr);
   l->elems[1].userdata = strdup(states);
   return sync_list(threaded, l);
}

/* Counted per root: saves is held, states is not. */
static void mass_two_roots(bool threaded)
{
   char     saves[256], states[256], name[32];
   unsigned i;
   lane_dirs("mass2", threaded, saves, sizeof(saves));
   fill_pathname_basedir(states, saves, sizeof(states));
   strlcat(states, "states", sizeof(states));
   CHECK(path_mkdir(states));
   for (i = 0; i < 30; i++)
   {
      name_file(name, sizeof(name), i);
      put(saves, name, name);
      if (i < 5)
         put(states, name, name);
   }
   CHECK(sync_two(threaded, saves, states));
   CHECK(uploads == 35);
   for (i = 0; i < 20; i++)
   {
      name_file(name, sizeof(name), i);
      drop(saves, name);
      if (i < 3)
         drop(states, name);
   }
   CHECK(sync_two(threaded, saves, states));
   CHECK(deletes == 3);
   CHECK(notices == 1);
   CHECK(notice_count == 20);
   CHECK(path_is_valid(srv_key("saves/f00.srm")));
   CHECK(!path_is_valid(srv_key("states/f00.srm")));
}

static void mass_held(bool threaded)
{
   mass_lane(threaded, 30, 20, true);
   mass_lane(threaded, 20, 11, true);
   mass_lane(threaded, 30, 10, false);
   mass_lane(threaded, 15, 10, false);
   /* exactly half is not more than half */
   mass_lane(threaded, 22, 11, false);
   mass_two_roots(threaded);
}

/* Out of memory listing a held key: still held, still counted. */
static void held_no_memory(bool threaded)
{
   char     root[256], name[32];
   unsigned i;
   lane_dirs("heldmem", threaded, root, sizeof(root));
   for (i = 0; i < 30; i++)
   {
      name_file(name, sizeof(name), i);
      put(root, name, name);
   }
   CHECK(sync_saves(threaded, root));
   for (i = 0; i < 20; i++)
   {
      name_file(name, sizeof(name), i);
      drop(root, name);
   }
   fail_append = "saves/f05.srm";
   CHECK(!sync_saves(threaded, root));
   fail_append = NULL;
   CHECK(deletes == 0);
   CHECK(notices == 1);
   CHECK(notice_count == 20);
   CHECK(path_is_valid(srv_key("saves/f05.srm")));
}

/* In the manifest at @path: 0 when @key is absent, 1 when it is
 * recorded deleted, 2 when it has a hash. */
static int manifest_entry(const char *path, const char *key)
{
   char        want[PATH_MAX_LENGTH];
   void       *buf = NULL;
   int64_t     len = 0;
   const char *at;
   int         entry = 0;
   if (!filestream_read_file(path, &buf, &len))
      return 0;
   snprintf(want, sizeof(want), "\"path\": \"%s\",", key);
   if ((at = strstr((const char*)buf, want)))
   {
      at    = strstr(at, "\"hash\": ");
      entry = at && !strncmp(at + 8, "\"\"", 2) ? 1 : 2;
   }
   free(buf);
   return entry;
}

static int server_entry(const char *key)
{
   return manifest_entry(srv_key(MANIFEST_FILENAME_SERVER), key);
}

static int local_entry(const char *key)
{
   char path[PATH_MAX_LENGTH];
   fill_pathname_join_special(path, assets, MANIFEST_FILENAME_LOCAL,
         sizeof(path));
   return manifest_entry(path, key);
}

/* Switches to a second device on the same server, with its own core
 * assets folder, and back. */
static void other_device(bool on)
{
   static char own[256];
   if (on)
   {
      strlcpy(own, assets, sizeof(own));
      strlcat(assets, "-b", sizeof(assets));
      path_mkdir(assets);
   }
   else
      strlcpy(assets, own, sizeof(assets));
}

/* As other_device(), for a third device. */
static void third_device(bool on)
{
   static char own[256];
   if (on)
   {
      strlcpy(own, assets, sizeof(own));
      strlcat(assets, "-c", sizeof(assets));
      path_mkdir(assets);
   }
   else
      strlcpy(assets, own, sizeof(assets));
}

/* Another device changes "a.srm" on the server, and this one has
 * deleted it: Keep Local deletes it on the server as any delete here
 * is, both manifests record the delete and nothing else is uploaded;
 * Keep Server fetches it back. @other: a later file here. */
static void keep_deleted_lane(bool threaded, int mode, bool other)
{
   char root[256], root_b[264], path[PATH_MAX_LENGTH];
   bool ok;
   lane_dirs(other ? "keep_other" : "keep", threaded, root, sizeof(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   CHECK(path_mkdir(root_b));
   put(root, "a.srm", "1");
   if (other)
      put(root, "b.srm", "b");
   CHECK(sync_saves(threaded, root));
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   put(root_b, "a.srm", "22");
   CHECK(sync_saves(threaded, root_b));
   CHECK(uploads == 1);
   other_device(false);

   drop(root, "a.srm");
   conflict_mode = mode;
   ok = sync_saves(threaded, root);
   conflict_mode = 0;
   CHECK(ok);
   CHECK(uploads == 0);
   if (other)
   {
      CHECK(holds(srv, "saves/b.srm", "b"));
      CHECK(server_entry("saves/b.srm") == 2);
      CHECK(local_entry("saves/b.srm") == 2);
   }
   if (mode == 2)
   {
      CHECK(deletes == 0);
      CHECK(fetches == 1);
      CHECK(holds(root, "a.srm", "22"));
      CHECK(holds(srv, "saves/a.srm", "22"));
      CHECK(server_entry("saves/a.srm") == 2);
      CHECK(local_entry("saves/a.srm") == 2);
      return;
   }
   CHECK(deletes == 1);
   CHECK(fetches == 0);
   CHECK(!path_is_valid(srv_key("saves/a.srm")));
   CHECK(server_entry("saves/a.srm") == 1);
   CHECK(local_entry("saves/a.srm") == 1);

   /* settled: nothing more here, and the other device deletes it */
   CHECK(sync_saves(threaded, root));
   CHECK(deletes == 0);
   CHECK(uploads == 0);
   CHECK(fetches == 0);
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   CHECK(uploads == 0);
   CHECK(!path_is_valid(srv_key("saves/a.srm")));
   fill_pathname_join_special(path, root_b, "a.srm", sizeof(path));
   CHECK(!path_is_valid(path));
   other_device(false);
}

/* A Keep Local delete counts toward the mass-deletion hold: 12 of 22
 * files gone here, one of them changed on the server, are held. */
static void keep_local_held(bool threaded)
{
   char     root[256], root_b[264], name[32], key[48];
   unsigned i;
   bool     ok;
   lane_dirs("keep_held", threaded, root, sizeof(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   CHECK(path_mkdir(root_b));
   for (i = 0; i < 22; i++)
   {
      name_file(name, sizeof(name), i);
      put(root, name, name);
   }
   CHECK(sync_saves(threaded, root));
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   put(root_b, "f00.srm", "changed");
   CHECK(sync_saves(threaded, root_b));
   other_device(false);
   for (i = 0; i < 12; i++)
   {
      name_file(name, sizeof(name), i);
      drop(root, name);
   }
   conflict_mode = 1;
   ok = sync_saves(threaded, root);
   conflict_mode = 0;
   CHECK(ok);
   CHECK(deletes == 0);
   CHECK(uploads == 0);
   CHECK(notices == 1);
   CHECK(notice_count == 12);
   CHECK(roots_name("saves/f00.srm"));
   CHECK(holds(srv, "saves/f00.srm", "changed"));
   for (i = 1; i < 12; i++)
   {
      snprintf(key, sizeof(key), "saves/f%02u.srm", i);
      CHECK(path_is_valid(srv_key(key)));
   }
   CHECK(sync_saves(threaded, root));
   CHECK(fetches == 12);
   CHECK(holds(root, "f00.srm", "changed"));
}

/* A backup under @root's backups/cloud-sync holds @text. */
static bool backup_holds(const char *root, const char *text)
{
   char                dir[PATH_MAX_LENGTH];
   struct string_list *l;
   bool                found = false;
   size_t              i;
   fill_pathname_join_special(dir, root, "backups/cloud-sync", sizeof(dir));
   pathname_conform_slashes_to_os(dir);
   if (!(l = dir_list_new(dir, NULL, false, true, false, true)))
      return false;
   for (i = 0; i < l->size && !found; i++)
   {
      void   *buf = NULL;
      int64_t len = 0;
      if (!filestream_read_file(l->elems[i].data, &buf, &len))
         continue;
      found = len == (int64_t)strlen(text) && !memcmp(buf, text, (size_t)len);
      free(buf);
   }
   string_list_free(l);
   return found;
}

/* The usual flow: @autos automatic syncs meet the conflict first and
 * keep it, until the user picks @mode; the sync after that is quiet.
 * @here_deleted: deleted here and changed on the other device, else
 * changed here and deleted there. */
static void keep_flow_lane(bool threaded, int mode, bool here_deleted,
      unsigned autos)
{
   char     root[256], root_b[264], path[PATH_MAX_LENGTH];
   unsigned i;
   bool     ok;
   lane_dirs(here_deleted ? "flow_del" : "flow_chg", threaded, root,
         sizeof(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   fill_pathname_join_special(path, root, "a.srm", sizeof(path));
   CHECK(path_mkdir(root_b));
   put(root, "a.srm", "1");
   put(root, "b.srm", "b");
   CHECK(sync_saves(threaded, root));
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   if (here_deleted)
      put(root_b, "a.srm", "22");
   else
      drop(root_b, "a.srm");
   CHECK(sync_saves(threaded, root_b));
   other_device(false);
   if (here_deleted)
      drop(root, "a.srm");
   else
      put(root, "a.srm", "33");

   for (i = 0; i < autos; i++)
   {
      CHECK(!sync_saves(threaded, root));
      CHECK(fetches == 0);
      CHECK(uploads == 0);
      CHECK(deletes == 0);
      CHECK(here_deleted ? !path_is_valid(path) : holds(root, "a.srm", "33"));
      CHECK(here_deleted ? holds(srv, "saves/a.srm", "22")
            : !path_is_valid(srv_key("saves/a.srm")));
   }

   conflict_mode = mode;
   ok = sync_saves(threaded, root);
   conflict_mode = 0;
   CHECK(ok);
   CHECK(holds(srv, "saves/b.srm", "b"));
   if (here_deleted && mode == 1)
   {
      CHECK(deletes == 1);
      CHECK(fetches == 0);
      CHECK(uploads == 0);
      CHECK(!path_is_valid(srv_key("saves/a.srm")));
      CHECK(server_entry("saves/a.srm") == 1);
      CHECK(local_entry("saves/a.srm") == 1);
   }
   else if (here_deleted)
   {
      CHECK(deletes == 0);
      CHECK(fetches == 1);
      CHECK(uploads == 0);
      CHECK(holds(root, "a.srm", "22"));
      CHECK(server_entry("saves/a.srm") == 2);
      CHECK(local_entry("saves/a.srm") == 2);
   }
   else if (mode == 1)
   {
      CHECK(uploads == 1);
      CHECK(fetches == 0);
      CHECK(holds(srv, "saves/a.srm", "33"));
      CHECK(server_entry("saves/a.srm") == 2);
      CHECK(local_entry("saves/a.srm") == 2);
   }
   else
   {
      CHECK(uploads == 0);
      CHECK(fetches == 0);
      CHECK(!path_is_valid(path));
      CHECK(backup_holds(root, "33"));
      CHECK(!path_is_valid(srv_key("saves/a.srm")));
      CHECK(server_entry("saves/a.srm") == 1);
      CHECK(local_entry("saves/a.srm") == 1);
   }

   CHECK(sync_saves(threaded, root));
   CHECK(fetches == 0);
   CHECK(uploads == 0);
   CHECK(deletes == 0);
}

static void keep_local_flow(bool threaded)
{
   keep_flow_lane(threaded, 1, true, 2);
   keep_flow_lane(threaded, 1, false, 2);
   keep_flow_lane(threaded, 1, false, 0);
}

static void keep_server_flow(bool threaded)
{
   keep_flow_lane(threaded, 2, true, 2);
   keep_flow_lane(threaded, 2, false, 2);
   keep_flow_lane(threaded, 2, false, 0);
}

/* Files deleted on both sides are not counted toward the hold: 10 of
 * 20 deleted here only, 2 more on both sides, are not held. */
static void keep_local_both_deleted(bool threaded)
{
   char     root[256], root_b[264], name[32];
   unsigned i;
   bool     ok;
   lane_dirs("keep_both", threaded, root, sizeof(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   CHECK(path_mkdir(root_b));
   for (i = 0; i < 20; i++)
   {
      name_file(name, sizeof(name), i);
      put(root, name, name);
   }
   CHECK(sync_saves(threaded, root));
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   drop(root_b, "f00.srm");
   drop(root_b, "f01.srm");
   CHECK(sync_saves(threaded, root_b));
   CHECK(deletes == 2);
   other_device(false);
   for (i = 0; i < 12; i++)
   {
      name_file(name, sizeof(name), i);
      drop(root, name);
   }
   conflict_mode = 1;
   ok = sync_saves(threaded, root);
   conflict_mode = 0;
   CHECK(ok);
   CHECK(deletes == 10);
   CHECK(notices == 0);
   CHECK(!path_is_valid(srv_key("saves/f02.srm")));
}

/* A file missing here counts toward the hold whether or not it changed
 * on the server, in every mode: 12 of 22 gone here, 2 of them changed
 * on the other device, are held. */
static void held_conflicts_lane(bool threaded, int mode)
{
   char     root[256], root_b[264], name[32], key[48];
   unsigned i;
   bool     ok;
   lane_dirs("held_conflicts", threaded, root, sizeof(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   CHECK(path_mkdir(root_b));
   for (i = 0; i < 22; i++)
   {
      name_file(name, sizeof(name), i);
      put(root, name, name);
   }
   CHECK(sync_saves(threaded, root));
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   put(root_b, "f00.srm", "changed");
   put(root_b, "f01.srm", "changed");
   CHECK(sync_saves(threaded, root_b));
   CHECK(uploads == 2);
   other_device(false);
   for (i = 0; i < 12; i++)
   {
      name_file(name, sizeof(name), i);
      drop(root, name);
   }
   conflict_mode = mode;
   ok = sync_saves(threaded, root);
   conflict_mode = 0;
   /* unpicked conflicts fail the sync */
   CHECK(ok == (mode != 0));
   CHECK(deletes == 0);
   CHECK(uploads == 0);
   CHECK(fetches == (mode == 2 ? 2u : 0u));
   CHECK(notices == 1);
   CHECK(notice_count == (mode == 1 ? 12u : 10u));
   for (i = 0; i < 12; i++)
   {
      snprintf(key, sizeof(key), "saves/f%02u.srm", i);
      CHECK(path_is_valid(srv_key(key)));
   }
   CHECK(holds(srv, "saves/f00.srm", "changed"));
   CHECK(holds(srv, "saves/f01.srm", "changed"));

   ok = sync_saves(threaded, root);
   CHECK(ok == (mode != 0));
   CHECK(deletes == 0);
   CHECK(fetches == (mode == 1 ? 12u : 10u));
   for (i = 2; i < 12; i++)
   {
      name_file(name, sizeof(name), i);
      CHECK(holds(root, name, name));
   }
   CHECK(holds(root, "f00.srm", "changed") == (mode != 0));
}

static void held_conflicts(bool threaded)
{
   held_conflicts_lane(threaded, 0);
   held_conflicts_lane(threaded, 1);
   held_conflicts_lane(threaded, 2);
}

/* A delete the server refuses, with a change sent in the same sync,
 * stays in both manifests: the next sync deletes it again, and the
 * other device takes the delete rather than uploading it again. */
static void delete_failed(bool threaded)
{
   char root[256], root_b[264], path[PATH_MAX_LENGTH];
   lane_dirs("delete_failed", threaded, root, sizeof(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   CHECK(path_mkdir(root_b));
   put(root, "a.srm", "1");
   put(root, "b.srm", "b");
   CHECK(sync_saves(threaded, root));
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   other_device(false);

   drop(root, "a.srm");
   put(root, "b.srm", "bb");
   fail_free = "saves/a.srm";
   CHECK(!sync_saves(threaded, root));
   fail_free = NULL;
   CHECK(deletes == 1);
   CHECK(uploads == 1);
   CHECK(holds(srv, "saves/a.srm", "1"));
   CHECK(server_entry("saves/a.srm") == 2);
   CHECK(local_entry("saves/a.srm") == 2);

   CHECK(sync_saves(threaded, root));
   CHECK(deletes == 1);
   CHECK(!path_is_valid(srv_key("saves/a.srm")));
   CHECK(server_entry("saves/a.srm") == 1);
   CHECK(local_entry("saves/a.srm") == 1);

   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   CHECK(uploads == 0);
   CHECK(fetches == 1);
   fill_pathname_join_special(path, root_b, "a.srm", sizeof(path));
   CHECK(!path_is_valid(path));
   other_device(false);
   CHECK(sync_saves(threaded, root));
   CHECK(fetches == 0);
   CHECK(!path_is_valid(srv_key("saves/a.srm")));
}

/* Deletes the server refused count toward the hold when tried again:
 * of 22 files, 10 refused and 2 more gone here are held. */
static void delete_failed_held(bool threaded)
{
   char     root[256], name[32], key[48];
   unsigned i;
   lane_dirs("delete_failed_held", threaded, root, sizeof(root));
   for (i = 0; i < 22; i++)
   {
      name_file(name, sizeof(name), i);
      put(root, name, name);
   }
   CHECK(sync_saves(threaded, root));
   for (i = 0; i < 10; i++)
   {
      name_file(name, sizeof(name), i);
      drop(root, name);
   }
   put(root, "f21.srm", "changed");
   fail_free = "saves/f0";
   CHECK(!sync_saves(threaded, root));
   fail_free = NULL;
   CHECK(deletes == 10);
   CHECK(notices == 0);

   drop(root, "f10.srm");
   drop(root, "f11.srm");
   CHECK(sync_saves(threaded, root));
   CHECK(deletes == 0);
   CHECK(notices == 1);
   CHECK(notice_count == 12);
   for (i = 0; i < 12; i++)
   {
      snprintf(key, sizeof(key), "saves/f%02u.srm", i);
      CHECK(path_is_valid(srv_key(key)));
   }
}

/* An upload that does not happen keeps both records. @how: 0 it
 * fails, 1 it can't be sent, 2 the file can't be read, 3 it can't be
 * read again to upload. "a.srm", deleted on the other device and
 * changed here, is kept by Keep Local while "b.srm" goes up: the
 * server keeps its delete, so a third device that still has "a.srm"
 * takes it rather than uploading its copy again, and the next sync
 * here uploads it. */
static void upload_failed_lane(bool threaded, int how)
{
   char root[256], root_b[264], root_c[264], path[PATH_MAX_LENGTH];
   char lane[32];
   bool ok;
   snprintf(lane, sizeof(lane), "upload_failed%d", how);
   lane_dirs(lane, threaded, root, sizeof(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   snprintf(root_c, sizeof(root_c), "%s-c", root);
   CHECK(path_mkdir(root_b));
   CHECK(path_mkdir(root_c));
   put(root, "a.srm", "1");
   put(root, "b.srm", "b");
   CHECK(sync_saves(threaded, root));
   third_device(true);
   CHECK(sync_saves(threaded, root_c));
   third_device(false);
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   drop(root_b, "a.srm");
   CHECK(sync_saves(threaded, root_b));
   CHECK(deletes == 1);
   other_device(false);

   put(root, "a.srm", "33");
   put(root, "b.srm", "bb");
   if (how < 2)
   {
      fail_update      = "saves/a.srm";
      fail_update_call = how == 1;
   }
   else
   {
      fail_fopen       = "a.srm";
      fail_fopen_after = how == 3 ? 1 : 0;
   }
   conflict_mode = 1;
   ok = sync_saves(threaded, root);
   conflict_mode    = 0;
   fail_update      = NULL;
   fail_update_call = false;
   fail_fopen       = NULL;
   CHECK(!ok);
   CHECK(uploads == 1);
   CHECK(holds(root, "a.srm", "33"));
   CHECK(!path_is_valid(srv_key("saves/a.srm")));
   CHECK(server_entry("saves/a.srm") == 1);
   CHECK(local_entry("saves/a.srm") == 2);

   third_device(true);
   CHECK(sync_saves(threaded, root_c));
   CHECK(uploads == 0);
   fill_pathname_join_special(path, root_c, "a.srm", sizeof(path));
   CHECK(!path_is_valid(path));
   third_device(false);

   conflict_mode = 1;
   ok = sync_saves(threaded, root);
   conflict_mode = 0;
   CHECK(ok);
   CHECK(uploads == 1);
   CHECK(holds(srv, "saves/a.srm", "33"));
}

static void upload_failed(bool threaded)
{
   int how;
   for (how = 0; how < 4; how++)
      upload_failed_lane(threaded, how);
}

/* A new file whose upload fails is recorded nowhere, not under the
 * entry of a longer key sharing its start, and goes up next sync. */
static void upload_failed_new(bool threaded)
{
   char root[256];
   lane_dirs("upload_failed_new", threaded, root, sizeof(root));
   put(root, "e.srm.x", "x");
   CHECK(sync_saves(threaded, root));
   put(root, "e.srm", "e");
   put(root, "f.srm", "f");
   fail_update = "saves/e.srm";
   CHECK(!sync_saves(threaded, root));
   fail_update = NULL;
   CHECK(uploads == 1);
   CHECK(server_entry("saves/e.srm") == 0);
   CHECK(local_entry("saves/e.srm") == 0);
   CHECK(sync_saves(threaded, root));
   CHECK(uploads == 1);
   CHECK(holds(srv, "saves/e.srm", "e"));
}

/* The other device changes "a.srm" and this one's download of it does
 * not happen. @how: 0 it fails, 1 it can't be sent, 2 no memory. The
 * copy here keeps its record, so the next sync fetches the change
 * rather than meeting a conflict, and a Keep Local after that has
 * nothing to put over it. */
static void fetch_failed_lane(bool threaded, int how)
{
   char root[256], root_b[264], lane[32];
   bool ok;
   snprintf(lane, sizeof(lane), "fetch_failed%d", how);
   lane_dirs(lane, threaded, root, sizeof(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   CHECK(path_mkdir(root_b));
   put(root, "a.srm", "1");
   CHECK(sync_saves(threaded, root));
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   put(root_b, "a.srm", "2");
   CHECK(sync_saves(threaded, root_b));
   CHECK(uploads == 1);
   other_device(false);

   if (how < 2)
   {
      fail_fetch      = "saves/a.srm";
      fail_fetch_call = how == 1;
   }
   else
      fail_alloc = sizeof(task_cloud_sync_fetch_state_t);
   ok = sync_saves(threaded, root);
   CHECK(!fail_alloc);
   fail_fetch      = NULL;
   fail_fetch_call = false;
   fail_alloc      = 0;
   CHECK(!ok);
   CHECK(fetches == 0);
   CHECK(holds(root, "a.srm", "1"));
   CHECK(local_entry("saves/a.srm") == 2);

   CHECK(sync_saves(threaded, root));
   CHECK(fetches == 1);
   CHECK(uploads == 0);
   CHECK(holds(root, "a.srm", "2"));

   conflict_mode = 1;
   ok = sync_saves(threaded, root);
   conflict_mode = 0;
   CHECK(ok);
   CHECK(uploads == 0);
   CHECK(holds(srv, "saves/a.srm", "2"));
}

static void fetch_failed(bool threaded)
{
   int how;
   for (how = 0; how < 3; how++)
      fetch_failed_lane(threaded, how);
}

/* A save that can't be opened, to hash it or to upload it, fails the
 * sync and is named in a warning. Its records stay as they were, and
 * it goes up once it can be read. */
static void save_unreadable(bool threaded)
{
   char root[256];
   lane_dirs("save_unreadable", threaded, root, sizeof(root));
   put(root, "a.srm", "1");
   CHECK(sync_saves(threaded, root));

   put(root, "a.srm", "11");
   fail_fopen_after = 0;
   fail_fopen = warn_about = "a.srm";
   CHECK(!sync_saves(threaded, root));
   CHECK(warned == 1);
   CHECK(uploads == 0);
   CHECK(holds(srv, "saves/a.srm", "1"));
   CHECK(server_entry("saves/a.srm") == 2);
   CHECK(local_entry("saves/a.srm") == 2);

   put(root, "c.srm", "c");
   fail_fopen = warn_about = "c.srm";
   CHECK(!sync_saves(threaded, root));
   CHECK(warned == 1);
   CHECK(uploads == 1);
   CHECK(holds(srv, "saves/a.srm", "11"));
   CHECK(server_entry("saves/c.srm") == 0);
   CHECK(local_entry("saves/c.srm") == 0);
   fail_fopen = warn_about = NULL;

   CHECK(sync_saves(threaded, root));
   CHECK(uploads == 1);
   CHECK(holds(srv, "saves/c.srm", "c"));
   CHECK(deletes == 0);
}

/* Work skipped on a key the local manifest does not list records no
 * local entry for it, not the entry of a longer key starting with it:
 * the auto state's download, deferred while the core owns it, arrives
 * at the next sync beside its thumbnail. */
static void carry_exact_key(bool threaded)
{
   char root[256], root_b[264];
   lane_dirs("carry_exact_key", threaded, root, sizeof(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   CHECK(path_mkdir(root_b));
   put(root, "Game.state.auto.png", "png");
   CHECK(sync_saves(threaded, root));
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   put(root_b, "Game.state.auto", "B");
   CHECK(sync_saves(threaded, root_b));
   CHECK(uploads == 1);
   other_device(false);

   live_save = "Game.state.auto";
   CHECK(sync_saves(threaded, root));
   live_save = NULL;
   CHECK(fetches == 0);
   CHECK(local_entry("saves/Game.state.auto") == 0);
   CHECK(local_entry("saves/Game.state.auto.png") == 2);

   CHECK(sync_saves(threaded, root));
   CHECK(fetches == 1);
   CHECK(holds(root, "Game.state.auto", "B"));

   conflict_mode = 1;
   CHECK(sync_saves(threaded, root));
   conflict_mode = 0;
   CHECK(deletes == 0);
   CHECK(uploads == 0);
   CHECK(holds(srv, "saves/Game.state.auto", "B"));
}

/* A download that can't be moved into place, after the old copy went
 * to the backups, drops the local record: the next sync fetches it
 * again, with no conflict. */
static void rename_failed(bool threaded)
{
   char root[256], root_b[264];
   lane_dirs("rename_failed", threaded, root, sizeof(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   CHECK(path_mkdir(root_b));
   put(root, "a.srm", "1");
   CHECK(sync_saves(threaded, root));
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   put(root_b, "a.srm", "2");
   CHECK(sync_saves(threaded, root_b));
   CHECK(uploads == 1);
   other_device(false);

   fail_rename = "a.srm";
   CHECK(!sync_saves(threaded, root));
   fail_rename = NULL;
   CHECK(fetches == 1);
   CHECK(!holds(root, "a.srm", "1"));
   CHECK(local_entry("saves/a.srm") == 0);
   CHECK(server_entry("saves/a.srm") == 2);

   CHECK(sync_saves(threaded, root));
   CHECK(fetches == 1);
   CHECK(holds(root, "a.srm", "2"));

   conflict_mode = 1;
   CHECK(sync_saves(threaded, root));
   conflict_mode = 0;
   CHECK(uploads == 0);
   CHECK(deletes == 0);
   CHECK(holds(srv, "saves/a.srm", "2"));
}

/* Hashing fails for the files named @file, on a read after a good open
 * or (!@read_error) out of memory, on a target that can't map files. */
static void hash_fault(bool read_error, const char *file)
{
   no_view    = true;
   warn_about = file;
   faults     = 0;
   if (read_error)
      fail_fread = file;
   else
   {
      fail_alloc     = CS_HASH_CHUNK;
      fail_alloc_all = true;
   }
}

static void hash_fault_off(void)
{
   CHECK(faults);
   no_view        = false;
   fail_fread     = warn_about = NULL;
   fail_alloc     = 0;
   fail_alloc_all = false;
}

/* A save whose hash can't be made fails the sync and is named in a
 * warning: nothing goes up or is placed, both records stay, and no
 * device takes it for a deletion. Changed here, new on another device,
 * changed on both, and new here. */
static void hash_failed_lane(bool threaded, bool read_error)
{
   char root[256], root_b[264], lane[32];
   snprintf(lane, sizeof(lane), "hash_failed%d", read_error ? 1 : 0);
   lane_dirs(lane, threaded, root, sizeof(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   CHECK(path_mkdir(root_b));
   put(root, "a.srm", "1");
   put(root, "b.srm", "b");
   CHECK(sync_saves(threaded, root));
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   other_device(false);

   put(root, "a.srm", "11");
   hash_fault(read_error, "a.srm");
   CHECK(!sync_saves(threaded, root));
   CHECK(warned == 1);
   hash_fault_off();
   CHECK(uploads == 0);
   CHECK(holds(srv, "saves/a.srm", "1"));
   CHECK(server_entry("saves/a.srm") == 2);
   CHECK(local_entry("saves/a.srm") == 2);
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   CHECK(holds(root_b, "a.srm", "1"));
   other_device(false);
   put(root, "a.srm", "1");

   other_device(true);
   put(root_b, "e.srm", "e");
   CHECK(sync_saves(threaded, root_b));
   other_device(false);
   hash_fault(read_error, "e.srm");
   CHECK(!sync_saves(threaded, root));
   CHECK(warned == 1);
   hash_fault_off();
   CHECK(fetches == 1);
   CHECK(!holds(root, "e.srm", "e"));
   CHECK(!holds(root, "e.srm" CS_FETCHING_SUFFIX, "e"));
   CHECK(server_entry("saves/e.srm") == 2);
   CHECK(local_entry("saves/e.srm") == 0);
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   CHECK(holds(root_b, "e.srm", "e"));
   other_device(false);
   CHECK(sync_saves(threaded, root));
   CHECK(fetches == 1);
   CHECK(holds(root, "e.srm", "e"));

   other_device(true);
   put(root_b, "b.srm", "b2");
   CHECK(sync_saves(threaded, root_b));
   other_device(false);
   put(root, "b.srm", "bA");
   hash_fault(read_error, "b.srm");
   CHECK(!sync_saves(threaded, root));
   CHECK(warned == 1);
   hash_fault_off();
   CHECK(uploads == 0);
   CHECK(fetches == 0);
   CHECK(holds(root, "b.srm", "bA"));
   CHECK(server_entry("saves/b.srm") == 2);
   CHECK(local_entry("saves/b.srm") == 2);

   put(root, "c.srm", "c");
   hash_fault(read_error, "c.srm");
   CHECK(!sync_saves(threaded, root));
   CHECK(warned == 1);
   hash_fault_off();
   CHECK(uploads == 0);
   CHECK(server_entry("saves/c.srm") == 0);
   CHECK(local_entry("saves/c.srm") == 0);

   conflict_mode = 1;
   CHECK(sync_saves(threaded, root));
   conflict_mode = 0;
   CHECK(uploads == 2);
   CHECK(deletes == 0);
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   CHECK(fetches == 2);
   CHECK(holds(root_b, "a.srm", "1"));
   CHECK(holds(root_b, "b.srm", "bA"));
   CHECK(holds(root_b, "c.srm", "c"));
   CHECK(holds(root_b, "e.srm", "e"));
   other_device(false);
}

static void hash_no_memory(bool threaded)  { hash_failed_lane(threaded, false); }
static void hash_read_error(bool threaded) { hash_failed_lane(threaded, true); }

/* A download of a save both devices hold whose hash fails keeps both
 * records: the next sync fetches it, with no conflict. */
static void fetch_hash_held(bool threaded)
{
   char root[256], root_b[264];
   lane_dirs("fetch_hash_held", threaded, root, sizeof(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   CHECK(path_mkdir(root_b));
   put(root, "a.srm", "1");
   CHECK(sync_saves(threaded, root));
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   put(root_b, "a.srm", "2");
   CHECK(sync_saves(threaded, root_b));
   CHECK(uploads == 1);
   other_device(false);

   hash_fault(true, "a.srm");
   fail_fread = "a.srm" CS_FETCHING_SUFFIX;
   CHECK(!sync_saves(threaded, root));
   CHECK(warned == 1);
   hash_fault_off();
   CHECK(fetches == 1);
   CHECK(holds(root, "a.srm", "1"));
   CHECK(local_entry("saves/a.srm") == 2);
   CHECK(server_entry("saves/a.srm") == 2);

   CHECK(sync_saves(threaded, root));
   CHECK(fetches == 1);
   CHECK(holds(root, "a.srm", "2"));

   conflict_mode = 1;
   CHECK(sync_saves(threaded, root));
   conflict_mode = 0;
   CHECK(uploads == 0);
   CHECK(deletes == 0);
   CHECK(holds(srv, "saves/a.srm", "2"));
}

/* A new save whose hash fails fails the sync on its own. */
static void new_hash_fails(bool threaded)
{
   char root[256];
   lane_dirs("new_hash_fails", threaded, root, sizeof(root));
   put(root, "a.srm", "1");
   CHECK(sync_saves(threaded, root));

   put(root, "c.srm", "c");
   hash_fault(true, "c.srm");
   CHECK(!sync_saves(threaded, root));
   CHECK(warned == 1);
   hash_fault_off();
   CHECK(uploads == 0);
   CHECK(local_entry("saves/c.srm") == 0);

   CHECK(sync_saves(threaded, root));
   CHECK(uploads == 1);
   CHECK(holds(srv, "saves/c.srm", "c"));
}

/* A delete already in both records stays in the server's when this
 * device sends a change: after it took the delete, and on a device
 * that never had the file. A third device that still has it then
 * takes the delete. */
static void tombstone_kept(bool threaded)
{
   char root[256], root_b[264], root_c[264], path[PATH_MAX_LENGTH];
   lane_dirs("tombstone_kept", threaded, root, sizeof(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   snprintf(root_c, sizeof(root_c), "%s-c", root);
   CHECK(path_mkdir(root_b));
   CHECK(path_mkdir(root_c));
   put(root, "a.srm", "1");
   put(root, "b.srm", "b");
   CHECK(sync_saves(threaded, root));
   third_device(true);
   CHECK(sync_saves(threaded, root_c));
   third_device(false);
   drop(root, "a.srm");
   CHECK(sync_saves(threaded, root));
   CHECK(deletes == 1);

   put(root, "b.srm", "bb");
   CHECK(sync_saves(threaded, root));
   CHECK(uploads == 1);
   CHECK(server_entry("saves/a.srm") == 1);

   other_device(true);
   put(root_b, "d.srm", "d");
   CHECK(sync_saves(threaded, root_b));
   CHECK(uploads == 1);
   CHECK(server_entry("saves/a.srm") == 1);
   CHECK(local_entry("saves/a.srm") == 1);
   other_device(false);

   third_device(true);
   CHECK(sync_saves(threaded, root_c));
   CHECK(uploads == 0);
   fill_pathname_join_special(path, root_c, "a.srm", sizeof(path));
   CHECK(!path_is_valid(path));
   third_device(false);
}

/* The other device deletes "a.srm"; this one takes the delete and
 * sends a change of "b.srm" in the same sync. @conflict: "a.srm" was
 * changed here too, and Keep Server takes the delete. A third device
 * that still has "a.srm" takes the delete rather than uploading it
 * again. */
static void taken_delete_lane(bool threaded, bool conflict)
{
   char root[256], root_b[264], root_c[264], path[PATH_MAX_LENGTH];
   bool ok;
   lane_dirs(conflict ? "taken_conflict" : "taken", threaded, root,
         sizeof(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   snprintf(root_c, sizeof(root_c), "%s-c", root);
   fill_pathname_join_special(path, root, "a.srm", sizeof(path));
   CHECK(path_mkdir(root_b));
   CHECK(path_mkdir(root_c));
   put(root, "a.srm", "1");
   put(root, "b.srm", "b");
   CHECK(sync_saves(threaded, root));
   third_device(true);
   CHECK(sync_saves(threaded, root_c));
   CHECK(fetches == 2);
   third_device(false);
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   drop(root_b, "a.srm");
   CHECK(sync_saves(threaded, root_b));
   CHECK(deletes == 1);
   other_device(false);

   if (conflict)
      put(root, "a.srm", "33");
   put(root, "b.srm", "bb");
   conflict_mode = conflict ? 2 : 0;
   ok = sync_saves(threaded, root);
   conflict_mode = 0;
   CHECK(ok);
   CHECK(uploads == 1);
   CHECK(!path_is_valid(path));
   CHECK(backup_holds(root, conflict ? "33" : "1"));
   CHECK(server_entry("saves/a.srm") == 1);
   CHECK(local_entry("saves/a.srm") == 1);

   third_device(true);
   CHECK(sync_saves(threaded, root_c));
   CHECK(uploads == 0);
   CHECK(fetches == 1);
   fill_pathname_join_special(path, root_c, "a.srm", sizeof(path));
   CHECK(!path_is_valid(path));
   third_device(false);
   CHECK(!path_is_valid(srv_key("saves/a.srm")));
   CHECK(sync_saves(threaded, root));
   CHECK(fetches == 0);
   CHECK(uploads == 0);
}

static void taken_delete(bool threaded)             { taken_delete_lane(threaded, false); }
static void keep_server_taken_delete(bool threaded) { taken_delete_lane(threaded, true); }

/* Files the server manifest no longer lists are not counted toward
 * the hold: of 22 files, 11 the server lost and 1 deleted here, the
 * one is deleted with no notice. */
static void server_lost(bool threaded)
{
   char     root[256], root_b[264], name[32];
   unsigned i;
   lane_dirs("server_lost", threaded, root, sizeof(root));
   snprintf(root_b, sizeof(root_b), "%s-b", root);
   CHECK(path_mkdir(root_b));
   for (i = 0; i < 22; i++)
   {
      name_file(name, sizeof(name), i);
      put(root, name, name);
      if (i >= 11)
         put(root_b, name, name);
   }
   CHECK(sync_saves(threaded, root));
   /* the server starts over, from another device's last 11 */
   remove_tree(srv);
   CHECK(path_mkdir(srv));
   other_device(true);
   CHECK(sync_saves(threaded, root_b));
   CHECK(uploads == 11);
   other_device(false);
   for (i = 0; i < 12; i++)
   {
      name_file(name, sizeof(name), i);
      drop(root, name);
   }
   CHECK(sync_saves(threaded, root));
   CHECK(deletes == 1);
   CHECK(notices == 0);
   CHECK(!path_is_valid(srv_key("saves/f11.srm")));
}

static void keep_local_deleted(bool threaded) { keep_deleted_lane(threaded, 1, false); }
static void keep_local_other(bool threaded)   { keep_deleted_lane(threaded, 1, true); }

static void keep_server_deleted(bool threaded)
{
   keep_deleted_lane(threaded, 2, false);
   keep_deleted_lane(threaded, 2, true);
}

/* A save moves "G.state.auto" aside for a moment, into backups/ or to
 * its held name, and the walk lands in between. @left: the held name
 * (1), the auto save's temp (2) or a state task's (3) stays and the
 * file is not back; otherwise the file is back by the diff. Nothing is
 * deleted on the server, the manifests keep it and the held name never
 * uploads. */
static char aside_states[264];
static void aside_back(void)
{
   put(aside_states, "G.state.auto", "2");
}

static void auto_aside_lane(bool threaded, int left)
{
   char saves[256];
   const char *name = left == 1 ? "G.state.auto.not-loaded.prev"
                    : left == 2 ? "G.state.auto.auto.tmp"
                                : "G.state.auto.tmp";
   lane_dirs(left == 1 ? "aside_held" : left == 2 ? "aside_temp"
         : left ? "aside_task_temp" : "aside_back",
         threaded, saves, sizeof(saves));
   snprintf(aside_states, sizeof(aside_states), "%s-st", saves);
   remove_tree(aside_states);
   put(saves, "a.srm", "a");
   put(aside_states, "G.state.auto", "1");
   CHECK(sync_two(threaded, saves, aside_states));
   CHECK(uploads == 2);

   if (left)
   {
      drop(aside_states, "G.state.auto");
      put(aside_states, name, left == 1 ? "1" : "2");
   }
   else
   {
      drop(aside_states, "G.state.auto");
      put(aside_states, "backups/G - not-loaded.state.auto", "1");
      walked_at = aside_states;
      walked    = aside_back;
   }
   /* an upload rewrites the server manifest in the same sync */
   put(saves, "b.srm", "b");
   CHECK(sync_two(threaded, saves, aside_states));
   walked_at = NULL;
   CHECK(deletes == 0);
   CHECK(uploads == 1);
   CHECK(holds(srv, "states/G.state.auto", "1"));
   CHECK(server_entry("states/G.state.auto") == 2);
   CHECK(local_entry("states/G.state.auto") == 2);
   CHECK(server_entry("states/G.state.auto.not-loaded.prev") == 0);
   CHECK(!path_is_valid(srv_key("states/G.state.auto.not-loaded.prev")));

   CHECK(sync_two(threaded, saves, aside_states));
   CHECK(deletes == 0);
   if (!left)
   {
      /* the new state goes up as a change */
      CHECK(uploads == 1);
      CHECK(holds(srv, "states/G.state.auto", "2"));
      CHECK(local_entry("states/G.state.auto") == 2);
      remove_tree(aside_states);
      return;
   }
   CHECK(uploads == 0);
   CHECK(holds(srv, "states/G.state.auto", "1"));

   /* gone for good: deleted */
   drop(aside_states, name);
   CHECK(sync_two(threaded, saves, aside_states));
   CHECK(deletes == 1);
   CHECK(server_entry("states/G.state.auto") == 1);
   CHECK(local_entry("states/G.state.auto") == 1);
   remove_tree(aside_states);
}

static void auto_aside_back(bool threaded) { auto_aside_lane(threaded, 0); }
static void auto_aside_held(bool threaded) { auto_aside_lane(threaded, 1); }
static void auto_aside_temp(bool threaded) { auto_aside_lane(threaded, 2); }
static void auto_aside_task_temp(bool threaded) { auto_aside_lane(threaded, 3); }

/* Another device's "G.state.auto" is on the server, and the walk here
 * misses this one's: by the diff it is back, or still in @name, the
 * RAM state's temp, moved in before the next sync. It is not fetched
 * over; the next sync meets it as a conflict, which by default moves
 * nothing.
 * @resurrected: this device synced its delete before; otherwise it
 * never had the file. */
static char back_states[264];
static const char *back_name;
static void auto_back(void)
{
   put(back_states, back_name, "2");
}

static void auto_back_lane(bool threaded, bool resurrected,
      const char *name)
{
   char saves[256], saves_b[264], states_b[264];
   lane_dirs(resurrected ? "back_resurrected"
         : string_is_equal(name, "G.state.auto") ? "back_new" : "back_ram",
         threaded, saves, sizeof(saves));
   snprintf(back_states, sizeof(back_states), "%s-st", saves);
   snprintf(saves_b, sizeof(saves_b), "%s-b", saves);
   snprintf(states_b, sizeof(states_b), "%s-st-b", saves);
   remove_tree(back_states);
   remove_tree(saves_b);
   remove_tree(states_b);
   CHECK(path_mkdir(back_states));
   CHECK(path_mkdir(saves_b));
   CHECK(path_mkdir(states_b));
   put(saves, "a.srm", "a");
   if (resurrected)
   {
      put(back_states, "G.state.auto", "1");
      CHECK(sync_two(threaded, saves, back_states));
      drop(back_states, "G.state.auto");
      CHECK(sync_two(threaded, saves, back_states));
      CHECK(deletes == 1);
      CHECK(local_entry("states/G.state.auto") == 1);
   }
   else
      CHECK(sync_two(threaded, saves, back_states));

   other_device(true);
   put(states_b, "G.state.auto", "3");
   conflict_mode = 1;
   CHECK(sync_two(threaded, saves_b, states_b));
   conflict_mode = 0;
   other_device(false);
   CHECK(holds(srv, "states/G.state.auto", "3"));

   /* an upload rewrites the server manifest in the same sync */
   put(saves, "b.srm", "b");
   back_name = name;
   walked_at = back_states;
   walked    = auto_back;
   CHECK(sync_two(threaded, saves, back_states));
   walked_at = NULL;
   CHECK(fetches == 0 && uploads == 1);
   CHECK(holds(back_states, name, "2"));
   CHECK(server_entry("states/G.state.auto") == 2);
   CHECK(local_entry("states/G.state.auto") == (resurrected ? 1 : 0));
   if (!string_is_equal(name, "G.state.auto"))
   {
      drop(back_states, name);
      put(back_states, "G.state.auto", "2");
   }

   sync_two(threaded, saves, back_states);
   CHECK(fetches == 0 && uploads == 0 && deletes == 0);
   CHECK(holds(back_states, "G.state.auto", "2"));
   CHECK(holds(srv, "states/G.state.auto", "3"));
   remove_tree(back_states);
   remove_tree(saves_b);
   remove_tree(states_b);
}

static void auto_back_resurrected(bool threaded)
{
   auto_back_lane(threaded, true, "G.state.auto");
}
static void auto_back_new(bool threaded)
{
   auto_back_lane(threaded, false, "G.state.auto");
}
static void auto_back_ram_temp(bool threaded)
{
   auto_back_lane(threaded, false, "G.state.auto.ram.tmp");
}

/* Slot 0's "G.state" is missing here and another device's is on the
 * server. "G.state.auto.tmp", the Auto slot's state task temp, is not
 * slot 0's: slot 0 comes down. */
static void slot0_auto_task_temp(bool threaded)
{
   char saves[256], states[264], saves_b[264], states_b[264];
   lane_dirs("slot0_auto_temp", threaded, saves, sizeof(saves));
   snprintf(states, sizeof(states), "%s-st", saves);
   snprintf(saves_b, sizeof(saves_b), "%s-b", saves);
   snprintf(states_b, sizeof(states_b), "%s-st-b", saves);
   remove_tree(states);
   remove_tree(saves_b);
   remove_tree(states_b);
   CHECK(path_mkdir(states));
   CHECK(path_mkdir(saves_b));
   CHECK(path_mkdir(states_b));
   put(saves, "a.srm", "a");
   CHECK(sync_two(threaded, saves, states));

   other_device(true);
   put(states_b, "G.state", "3");
   conflict_mode = 1;
   CHECK(sync_two(threaded, saves_b, states_b));
   conflict_mode = 0;
   other_device(false);
   CHECK(holds(srv, "states/G.state", "3"));

   put(states, "G.state.auto.tmp", "x");
   CHECK(sync_two(threaded, saves, states));
   CHECK(fetches == 1);
   CHECK(holds(states, "G.state", "3"));
   CHECK(holds(states, "G.state.auto.tmp", "x"));
   remove_tree(states);
   remove_tree(saves_b);
   remove_tree(states_b);
}

struct lane
{
   const char *name;
   void (*run)(bool threaded);
};

static void unreadable_subfolder(bool threaded) { unreadable_lane(threaded, true); }
static void readdir_error(bool threaded)        { unreadable_lane(threaded, false); }

/* A state that comes down is counted for the state limit's check
 * across a save; a save that comes down is not. */
static void fetch_state_written(bool threaded)
{
   char saves[256], states[264], saves_b[264], states_b[264];
   char roots[PATH_MAX_LENGTH], json[1200];
   lane_dirs("fetch_written", threaded, saves, sizeof(saves));
   snprintf(states, sizeof(states), "%s-st", saves);
   snprintf(saves_b, sizeof(saves_b), "%s-b", saves);
   snprintf(states_b, sizeof(states_b), "%s-st-b", saves);
   remove_tree(states);
   remove_tree(saves_b);
   remove_tree(states_b);
   CHECK(path_mkdir(saves_b));
   CHECK(path_mkdir(states_b));
   put(saves, "a.srm", "1");
   put(states, "a.state", "2");
   CHECK(sync_two(threaded, saves, states));
   CHECK(uploads == 2);
   roots_path(roots, sizeof(roots));
   snprintf(json, sizeof(json), "{\"saves\": \"%s\", \"states\": \"%s\"}\n",
         saves, states);
   CHECK(filestream_write_file(roots, json, strlen(json)));
   state_fetches = 0;
   CHECK(sync_two(threaded, saves_b, states_b));
   CHECK(fetches == 2);
   CHECK(holds(saves_b, "a.srm", "1"));
   CHECK(holds(states_b, "a.state", "2"));
   CHECK(state_fetches == 1);
   remove_tree(states);
   remove_tree(saves_b);
   remove_tree(states_b);
}

static const struct lane lanes[] = {
   { "sync_unreadable_subfolder", unreadable_subfolder },
   { "sync_readdir_error",        readdir_error },
   { "sync_empty_root",           empty_root_lane },
   { "sync_root_changed",         root_changed },
   { "sync_no_roots_file",        no_roots_file },
   { "sync_unset_no_entry",       unset_no_entry },
   { "sync_roots_no_memory",      roots_no_memory },
   { "sync_main_folder_only",     main_folder_only },
   { "sync_mass_held",            mass_held },
   { "sync_held_no_memory",       held_no_memory },
   { "sync_keep_local_deleted",   keep_local_deleted },
   { "sync_keep_local_other",     keep_local_other },
   { "sync_keep_local_held",      keep_local_held },
   { "sync_keep_server_deleted",  keep_server_deleted },
   { "sync_keep_local_flow",      keep_local_flow },
   { "sync_keep_server_flow",     keep_server_flow },
   { "sync_keep_local_both_deleted", keep_local_both_deleted },
   { "sync_held_conflicts",       held_conflicts },
   { "sync_delete_failed",        delete_failed },
   { "sync_delete_failed_held",   delete_failed_held },
   { "sync_taken_delete",         taken_delete },
   { "sync_keep_server_taken_delete", keep_server_taken_delete },
   { "sync_server_lost",          server_lost },
   { "sync_upload_failed",        upload_failed },
   { "sync_upload_failed_new",    upload_failed_new },
   { "sync_tombstone_kept",       tombstone_kept },
   { "sync_fetch_failed",         fetch_failed },
   { "sync_save_unreadable",      save_unreadable },
   { "sync_carry_exact_key",      carry_exact_key },
   { "sync_rename_failed",        rename_failed },
   { "sync_hash_no_memory",       hash_no_memory },
   { "sync_hash_read_error",      hash_read_error },
   { "sync_fetch_hash_held",      fetch_hash_held },
   { "sync_new_hash_fails",       new_hash_fails },
   { "sync_fetch_state_written",  fetch_state_written },
   { "sync_auto_aside_back",      auto_aside_back },
   { "sync_auto_aside_held",      auto_aside_held },
   { "sync_auto_aside_temp",      auto_aside_temp },
   { "sync_auto_aside_task_temp", auto_aside_task_temp },
   { "sync_auto_back_resurrected", auto_back_resurrected },
   { "sync_auto_back_new",        auto_back_new },
   { "sync_auto_back_ram_temp",   auto_back_ram_temp },
   { "sync_slot0_auto_task_temp", slot0_auto_task_temp },
};

int main(int argc, char **argv)
{
   size_t   i;
   unsigned t;
#if defined(HAVE_THREADS) && !defined(TCS_LOCK_FREE)
   tcs_manifest_lock = slock_new();
#endif
   remove_tree(base);
   CHECK(path_mkdir(base));
   for (i = 0; i < sizeof(lanes) / sizeof(lanes[0]); i++)
   {
      int a, wanted = argc < 2;
      for (a = 1; a < argc; a++)
         if (string_is_equal(argv[a], lanes[i].name))
            wanted = 1;
      if (!wanted)
         continue;
      for (t = 0; t < 2; t++)
      {
         int before = failures;
         lanes[i].run(t != 0);
         printf("%s %s (%s)\n", failures == before ? "PASS" : "FAIL",
               lanes[i].name, t ? "threaded" : "no threads");
      }
   }
   remove_tree(base);
   puts(failures ? "FAIL cloudsync_roots_test" : "PASS cloudsync_roots_test");
   return failures ? 1 : 0;
}
