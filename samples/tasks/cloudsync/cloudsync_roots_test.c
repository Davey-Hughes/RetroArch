/* Whole syncs of the real cloud-sync task over a folder standing in
 * for the server, threaded tasks on and off: what the sync deletes on
 * the server when a folder under a root can't be read, when a folder
 * is unset, when a root moved, when most of a root's files are
 * missing, and which save folder it walks.
 *
 *   ./cloudsync_roots_test [lane...]   (default: every lane) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <retro_dirent.h>
#include <retro_timers.h>
#include <queues/message_queue.h>
#include <lists/string_list.h>
#include "../../../msg_hash.h"

static struct RDIR *test_opendir(const char *name, bool include_hidden);
static int test_readdir(struct RDIR *rdir);
static bool test_dirent_error(struct RDIR *rdir);
static char *test_strdup(const char *s);
static bool test_string_list_append(struct string_list *list,
      const char *elem, union string_list_elem_attr attr);
#define retro_opendir_include_hidden test_opendir
#define retro_readdir test_readdir
#define retro_dirent_error test_dirent_error
#define strdup test_strdup
#define string_list_append test_string_list_append
#include "../../../tasks/task_cloudsync.c"
#undef retro_opendir_include_hidden
#undef retro_readdir
#undef retro_dirent_error
#undef strdup
#undef string_list_append

/* A copy of this string runs out of memory */
static const char *fail_strdup;
static char *test_strdup(const char *s)
{
   if (fail_strdup && !strcmp(s, fail_strdup))
      return NULL;
   return strdup(s);
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

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

void RARCH_LOG(const char *fmt, ...) { (void)fmt; }
void RARCH_WARN(const char *fmt, ...) { (void)fmt; }
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
bool content_savefile_is_live(const char *path) { (void)path; return false; }
unsigned content_savefile_lifetime(void) { return 0; }
unsigned content_save_writes(void) { return 0; }
bool content_save_state_in_progress(void *data) { (void)data; return false; }
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
   return d;
}

static int test_readdir(struct RDIR *rdir)
{
   if (rdir == read_dir)
   {
      read_failed = true;
      return 0;
   }
   return retro_readdir(rdir);
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

bool cloud_sync_read(const char *path, const char *file,
      cloud_sync_complete_handler_t cb, void *ud)
{
   char   s[PATH_MAX_LENGTH];
   RFILE *f = NULL;
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

bool cloud_sync_update(const char *path, RFILE *file,
      cloud_sync_complete_handler_t cb, void *ud)
{
   char s[PATH_MAX_LENGTH];
   bool ok;
   srv_path(s, sizeof(s), path);
   ok = copy_file(filestream_get_path(file), s);
   if (ok && !string_is_equal(path, MANIFEST_FILENAME_SERVER))
      uploads++;
   cb(ud, path, ok, file);
   return true;
}

bool cloud_sync_free(const char *path, cloud_sync_complete_handler_t cb,
      void *ud)
{
   char s[PATH_MAX_LENGTH];
   srv_path(s, sizeof(s), path);
   deletes++;
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
   strlcpy(st->dir_core_assets, assets, sizeof(st->dir_core_assets));
   retro_atomic_int_init(&st->waiting, 0);
   retro_atomic_int_init(&st->phase, CLOUD_SYNC_PHASE_BEGIN);
   st->cb      = on_done;
   task_cloud_sync_task_setup(task, st, "Cloud Sync");
   sync_done   = 0;
   sync_failed = 0;
   deletes = uploads = fetches = notices = notice_count = outside_opens = 0;
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

struct lane
{
   const char *name;
   void (*run)(bool threaded);
};

static void unreadable_subfolder(bool threaded) { unreadable_lane(threaded, true); }
static void readdir_error(bool threaded)        { unreadable_lane(threaded, false); }

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
