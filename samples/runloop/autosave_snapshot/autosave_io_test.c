/* save.c's autosave worker and close-time write over real files, the
 * worker's through a test VFS that can cut a write short or refuse a
 * rename.  Build with function sections and --gc-sections: only what
 * the lanes reach of save.c is linked. */

/* The *_impl functions and the VFS callbacks share a handle type only
 * under VFS_FRONTEND. */
#define VFS_FRONTEND

#include <stdio.h>
#include <stdarg.h>
#include <retro_timers.h>
#include <lists/dir_list.h>
#include <vfs/vfs_implementation.h>

#define filestream_write_file_atomic_synced test_write_atomic_synced
#ifdef HAVE_THREADS
#define sthread_create test_sthread_create
#define sthread_join test_sthread_join
#define cpu_features_get_time_usec test_time_usec
#define retro_eventcount_commit_wait test_commit_wait
#endif
#include "../../../save.c"
#undef filestream_write_file_atomic_synced
bool filestream_write_file_atomic_synced(const char *path, const void *data,
      int64_t size);
#ifdef HAVE_THREADS
#undef sthread_create
#undef sthread_join
#undef cpu_features_get_time_usec
#undef retro_eventcount_commit_wait
sthread_t *sthread_create(void (*thread_func)(void*), void *userdata);
void sthread_join(sthread_t *thread);
void retro_eventcount_commit_wait(retro_eventcount_t *ec, int key);
#endif

#define SAVE        "game.srm"
#define WORKER_TEMP SAVE ".auto.tmp"
#define CLOSE_TEMP  SAVE ".tmp"
#define BIG         8192

static int failures;

#define CHECK(cond, msg) \
   do { \
      if (!(cond)) \
      { \
         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
         failures++; \
      } \
   } while (0)

/* What the core reports for RETRO_MEMORY_SAVE_RAM; RTC has none. */
static void  *core_data;
static size_t core_size;

bool core_get_memory(retro_ctx_memory_info_t *info)
{
   info->data = info->id == RETRO_MEMORY_SAVE_RAM ? core_data : NULL;
   info->size = info->id == RETRO_MEMORY_SAVE_RAM ? core_size : 0;
   return true;
}

/* Log lines, each led by its level: I, W or E.  Workers log too. */
#define MAX_LOG 128
static char log_lines[MAX_LOG][320];
static unsigned log_num;
#ifdef HAVE_THREADS
static slock_t *log_lock;
#define LOG_LOCK()   slock_lock(log_lock)
#define LOG_UNLOCK() slock_unlock(log_lock)
#else
#define LOG_LOCK()
#define LOG_UNLOCK()
#endif

static void log_line(char level, const char *fmt, va_list ap)
{
   LOG_LOCK();
   if (log_num < MAX_LOG)
   {
      log_lines[log_num][0] = level;
      vsnprintf(log_lines[log_num] + 1, sizeof(log_lines[0]) - 1, fmt, ap);
      log_num++;
   }
   LOG_UNLOCK();
}

void RARCH_LOG(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   log_line('I', fmt, ap);
   va_end(ap);
}

void RARCH_WARN(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   log_line('W', fmt, ap);
   va_end(ap);
}

void RARCH_ERR(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   log_line('E', fmt, ap);
   va_end(ap);
}

/* Lines logged at @level that hold @text. */
static unsigned logged(char level, const char *text)
{
   unsigned i, n = 0;
   LOG_LOCK();
   CHECK(log_num < MAX_LOG, "the log filled up");
   for (i = 0; i < log_num; i++)
      if (log_lines[i][0] == level && strstr(log_lines[i] + 1, text))
         n++;
   LOG_UNLOCK();
   return n;
}

/* Each message is "msg<n> %s", so a notice tells which it was. */
const char *msg_hash_to_str(enum msg_hash_enums msg)
{
   static char ring[8][32];
   static unsigned next;
   char *s = ring[next++ & 7];
   snprintf(s, sizeof(ring[0]), "msg%d %%s", (int)msg);
   return s;
}

#define MAX_NOTICES 32
static int notice_ids[MAX_NOTICES];
static unsigned notice_num;

void runloop_msg_queue_push(const char *msg, size_t len,
      unsigned prio, unsigned duration, bool flush, char *title,
      enum message_queue_icon icon, enum message_queue_category category)
{
   (void)len;
   (void)prio;
   (void)duration;
   (void)flush;
   (void)title;
   (void)icon;
   (void)category;
   if (notice_num < MAX_NOTICES)
      notice_ids[notice_num++] = atoi(msg + 3);
}

static unsigned notices(enum msg_hash_enums msg)
{
   unsigned i, n = 0;
   for (i = 0; i < notice_num; i++)
      if (notice_ids[i] == (int)msg)
         n++;
   return n;
}

#ifdef HAVE_THREADS
/* Lines logged as a warning that are the notice @msg. */
static unsigned logged_msg(enum msg_hash_enums msg)
{
   char text[32];
   snprintf(text, sizeof(text), "msg%d ", (int)msg);
   return logged('W', text);
}
#endif

static runloop_state_t runloop_st;

runloop_state_t *runloop_state_get_ptr(void)
{
   return &runloop_st;
}

static unsigned synced_writes;

bool content_finish_tmp(const char *path, bool ok, bool sync)
{
   if (sync)
      synced_writes++;
   return (sync
         ? filestream_finish_atomic_synced(path, ok)
         : filestream_finish_atomic(path, ok)) == 0;
}

bool test_write_atomic_synced(const char *path, const void *data,
      int64_t size)
{
   synced_writes++;
   return filestream_write_file_atomic_synced(path, data, size);
}

/* The test VFS: the built-in one, counting opens of the save's own
 * files for writing and cutting each at cut_at bytes, or refusing
 * their renames. Copies and recovery files are written as usual. */
#define MAX_OPEN 8
#ifdef HAVE_THREADS
static slock_t *vfs_lock;
#define VFS_LOCK()   slock_lock(vfs_lock)
#define VFS_UNLOCK() slock_unlock(vfs_lock)
#else
#define VFS_LOCK()
#define VFS_UNLOCK()
#endif
static struct retro_vfs_file_handle *open_files[MAX_OPEN];
static int64_t open_written[MAX_OPEN];
static int64_t cut_at;
static bool refuse_rename;
static unsigned write_opens;
/* What reached the save, in order: each image's first byte. */
static char put_log[16];
static unsigned put_num;
#ifdef HAVE_THREADS
static uintptr_t main_thread;
static unsigned main_write_opens;
#endif

static bool save_own(const char *path)
{
   return !strncmp(path_basename(path), SAVE, strlen(SAVE));
}

static struct retro_vfs_file_handle *RETRO_CALLCONV test_open(
      const char *path, unsigned mode, unsigned hints)
{
   unsigned i;
   struct retro_vfs_file_handle *h =
      retro_vfs_file_open_impl(path, mode, hints);
   if (!h || !(mode & RETRO_VFS_FILE_ACCESS_WRITE) || !save_own(path))
      return h;
   VFS_LOCK();
   write_opens++;
#ifdef HAVE_THREADS
   if (sthread_get_current_thread_id() == main_thread)
      main_write_opens++;
#endif
   for (i = 0; i < MAX_OPEN; i++)
      if (!open_files[i])
      {
         open_files[i]   = h;
         open_written[i] = 0;
         break;
      }
   VFS_UNLOCK();
   return h;
}

static int RETRO_CALLCONV test_close(struct retro_vfs_file_handle *h)
{
   unsigned i;
   VFS_LOCK();
   for (i = 0; i < MAX_OPEN; i++)
      if (open_files[i] == h)
         open_files[i] = NULL;
   VFS_UNLOCK();
   return retro_vfs_file_close_impl(h);
}

static int64_t RETRO_CALLCONV test_write(struct retro_vfs_file_handle *h,
      const void *s, uint64_t len)
{
   unsigned i;
   int64_t room = -1;
   VFS_LOCK();
   for (i = 0; i < MAX_OPEN; i++)
      if (open_files[i] == h)
      {
         if (cut_at)
         {
            room = cut_at - open_written[i];
            if (room > (int64_t)len)
               room = (int64_t)len;
            if (room < 0)
               room = 0;
         }
         open_written[i] += cut_at ? room : (int64_t)len;
         break;
      }
   VFS_UNLOCK();
   if (room < 0)
      return retro_vfs_file_write_impl(
            h, s, len);
   /* A full disk: what fits is written, then the write fails. */
   if (room)
      retro_vfs_file_write_impl(h,
            s, (uint64_t)room);
   return room == (int64_t)len ? room : -1;
}

static int RETRO_CALLCONV test_rename(const char *old_path,
      const char *new_path)
{
   bool refuse;
   int ret;
   int first = EOF;
   FILE *f   = NULL;
   VFS_LOCK();
   refuse = refuse_rename;
   VFS_UNLOCK();
   if (!save_own(new_path))
      return retro_vfs_file_rename_impl(old_path, new_path);
   if (refuse)
      return -1;
   if ((f = fopen(old_path, "rb")))
   {
      first = fgetc(f);
      fclose(f);
   }
   ret = retro_vfs_file_rename_impl(old_path, new_path);
   VFS_LOCK();
   if (!ret && first != EOF && put_num < sizeof(put_log) - 1)
      put_log[put_num++] = (char)first;
   VFS_UNLOCK();
   return ret;
}

static struct retro_vfs_interface test_iface;

static void use_test_vfs(bool on)
{
   struct retro_vfs_interface_info info;
   memset(&test_iface, 0, sizeof(test_iface));
   test_iface.get_path = retro_vfs_file_get_path_impl;
   test_iface.open     = test_open;
   test_iface.close    = test_close;
   test_iface.size     = retro_vfs_file_size_impl;
   test_iface.tell     = retro_vfs_file_tell_impl;
   test_iface.seek     = retro_vfs_file_seek_impl;
   test_iface.read     = retro_vfs_file_read_impl;
   test_iface.write    = test_write;
   test_iface.flush    = retro_vfs_file_flush_impl;
   test_iface.remove   = retro_vfs_file_remove_impl;
   test_iface.rename   = test_rename;
   test_iface.truncate = retro_vfs_file_truncate_impl;
   info.required_interface_version = FILESTREAM_REQUIRED_VFS_VERSION;
   info.iface                      = on ? &test_iface : NULL;
   filestream_vfs_init(&info);
}

static void set_vfs(int64_t cut, bool refuse)
{
   VFS_LOCK();
   cut_at        = cut;
   refuse_rename = refuse;
   VFS_UNLOCK();
}

static unsigned get_write_opens(void)
{
   unsigned n;
   VFS_LOCK();
   n = write_opens;
   VFS_UNLOCK();
   return n;
}

#ifdef HAVE_THREADS
static bool written_in_order(const char *order)
{
   bool ok;
   VFS_LOCK();
   put_log[put_num] = '\0';
   ok = !strcmp(put_log, order);
   VFS_UNLOCK();
   return ok;
}
#endif

static void fill(unsigned char *p, size_t len, unsigned seed)
{
   size_t i;
   for (i = 0; i < len; i++)
   {
      seed = seed * 1103515245u + 12345u;
      p[i] = (unsigned char)(seed >> 16);
   }
}

static bool exists(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return false;
   fclose(f);
   return true;
}

static void put(const char *path, const void *data, size_t len)
{
   FILE *f = fopen(path, "wb");
   if (f)
   {
      fwrite(data, 1, len, f);
      fclose(f);
   }
}

/* The file's raw bytes are exactly data. */
static bool holds(const char *path, const void *data, size_t len)
{
   static unsigned char buf[BIG * 2];
   size_t got;
   FILE *f = fopen(path, "rb");
   if (!f)
      return false;
   got = fread(buf, 1, sizeof(buf), f);
   fclose(f);
   return got == len && !memcmp(buf, data, len);
}

#ifdef HAVE_THREADS
/* The file reads back (raw or rzip) as data. */
static bool reads_as(const char *path, const void *data, size_t len)
{
   void *buf = NULL;
   int64_t got = 0;
   bool ok;
#if defined(HAVE_COMPRESSION)
   if (!rzipstream_read_file(path, &buf, &got))
#else
   if (!filestream_read_file(path, &buf, &got))
#endif
      return false;
   ok = got == (int64_t)len && !memcmp(buf, data, len);
   free(buf);
   return ok;
}
#endif

/* The save holds no data: 0 raw bytes, or an rzip header for 0. */
static bool emptied(const char *path)
{
   static const unsigned char zero[8];
   unsigned char buf[32];
   size_t got;
   FILE *f = fopen(path, "rb");
   if (!f)
      return false;
   got = fread(buf, 1, sizeof(buf), f);
   fclose(f);
   return got == 0 || (got == 20 && !memcmp(buf, "#RZIPv", 6)
         && !memcmp(buf + 12, zero, sizeof(zero)));
}

static void clear_dir(const char *dir)
{
   size_t i;
   struct string_list *list = dir_list_new(dir, NULL, false, true,
         false, false);
   for (i = 0; list && i < list->size; i++)
      filestream_delete(list->elems[i].data);
   if (list)
      string_list_free(list);
}

static void clean(void)
{
   remove(SAVE);
   remove(WORKER_TEMP);
   remove(CLOSE_TEMP);
   remove("game.rtc");
   clear_dir("backups");
   clear_dir("blk");
   clear_dir("cfg/recovery");
   save_backups_set_roots(NULL, NULL, NULL, NULL, "cfg", NULL, "as_core");
   notice_num = 0;
   VFS_LOCK();
   put_num = 0;
   VFS_UNLOCK();
   LOG_LOCK();
   log_num = 0;
   LOG_UNLOCK();
}

/* Copies in @dir with @reason (or any, with SAVE_BACKUP_REASON_LAST);
 * the newest one's path in @newest. */
static unsigned copies(const char *dir, enum save_backup_reason reason,
      char *newest, size_t len)
{
   size_t i;
   unsigned n = 0;
   struct save_backup_name best;
   struct string_list *list = dir_list_new(dir, NULL, false, true,
         false, false);
   if (newest)
      newest[0] = '\0';
   for (i = 0; list && i < list->size; i++)
   {
      struct save_backup_name name;
      if (     !save_backups_parse(path_basename(list->elems[i].data), &name)
            || (reason != SAVE_BACKUP_REASON_LAST && name.reason != reason))
         continue;
      if (newest && (!n || memcmp(name.time, best.time, sizeof(best.time)) > 0
               || (!memcmp(name.time, best.time, sizeof(best.time))
                  && name.number > best.number)))
      {
         strlcpy(newest, list->elems[i].data, len);
         best = name;
      }
      n++;
   }
   if (list)
      string_list_free(list);
   return n;
}

/* An rzip save of @data with its last 100 bytes cut off */
static void put_cut_rzip(const char *path, const void *data, size_t len,
      void *out, int64_t *out_len)
{
   void *buf = NULL;
   int64_t n = 0;
   rzipstream_write_file(path, data, (int64_t)len);
   filestream_read_file(path, &buf, &n);
   *out_len = n > 100 ? n - 100 : 0;
   memcpy(out, buf, (size_t)*out_len);
   free(buf);
   put(path, out, (size_t)*out_len);
}

/* With no worker: the flush before a state load writes the
 * save here, after the copy it owes; the next write after the load
 * backs up what the flush wrote, once per load. */
static void lane_state_load_copy_sync(void)
{
   static unsigned char cut[BIG * 2], ram[BIG], p1[BIG], l1[BIG], p2[BIG];
   char newest[PATH_MAX_LENGTH];
   int64_t cut_len = 0;
   unsigned opens;
   const int bsl   = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   clean();
   fill(p1, BIG, 42);
   fill(l1, BIG, 43);
   fill(p2, BIG, 44);
   put_cut_rzip(SAVE, l1, BIG, cut, &cut_len);
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   CHECK(!event_load_save_files(false), "state_load_sync: a cut rzip loaded");
   use_test_vfs(true);

   memcpy(ram, p1, BIG);
   event_flush_save_files(true, false, bsl);
   CHECK(copies("backups", SAVE_BACKUP_UNREADABLE, newest, sizeof(newest)) == 1
         && holds(newest, cut, (size_t)cut_len),
         "state_load_sync: the flush did not make the owed copy");
   CHECK(holds(SAVE, p1, BIG), "state_load_sync: the flush did not write");
   CHECK(!copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, NULL, 0),
         "state_load_sync: a copy before the load");

   memcpy(ram, l1, BIG);
   event_save_files(true, false, true, true, NULL);
   CHECK(copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, newest,
            sizeof(newest)) == 1 && holds(newest, p1, BIG),
         "state_load_sync: no copy of the pre-load save");
   CHECK(holds(SAVE, l1, BIG), "state_load_sync: not written after the copy");

   memcpy(ram, p2, BIG);
   event_flush_save_files(true, false, bsl);
   memcpy(ram, l1, BIG);
   event_save_files(true, false, true, true, NULL);
   CHECK(copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, newest,
            sizeof(newest)) == 2 && holds(newest, p2, BIG),
         "state_load_sync: no copy for the second load");

   /* Unchanged: nothing written. */
   opens = get_write_opens();
   event_flush_save_files(true, false, 0);
   CHECK(get_write_opens() == opens, "state_load_sync: unchanged, written");
   use_test_vfs(false);
   path_deinit_savefile();
   clean();
}

#ifdef HAVE_THREADS
/* An image for the put log: @id is its first byte. */
static void fill_id(unsigned char *p, size_t len, unsigned seed, char id)
{
   fill(p, len, seed);
   p[0] = (unsigned char)id;
}

/* No recovery file, notice or warning. */
static bool quiet(void)
{
   return   !notice_num
         && !copies("cfg/recovery", SAVE_BACKUP_REASON_LAST, NULL, 0)
         && !copies("backups", SAVE_BACKUP_UNSAVED, NULL, 0)
         && !logged('W', "") && !logged('E', "");
}
#endif

/* With no worker, a flush whose owed copy cannot be made: the save
 * stays and the image, which the load replaces, goes to a recovery
 * file, one for each and shown each time. */
static void lane_flush_copy_fails_sync(void)
{
   static unsigned char cut[BIG * 2], ram[BIG], p1[BIG], p2[BIG], l1[BIG];
   char newest[PATH_MAX_LENGTH];
   int64_t cut_len  = 0;
   const char *save = "blk/game.srm";
   const int bsl    = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   clean();
   path_mkdir("blk");
   put("blk/backups", "x", 1);
   fill(ram, BIG, 60);
   fill(p1, BIG, 61);
   fill(p2, BIG, 62);
   fill(l1, BIG, 63);
   put_cut_rzip(save, ram, BIG, cut, &cut_len);
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(save);
   CHECK(!event_load_save_files(false), "flush_copy_sync: a cut rzip loaded");
   notice_num = 0;

   memcpy(ram, p1, BIG);
   event_flush_save_files(true, false, bsl);
   CHECK(holds(save, cut, (size_t)cut_len),
         "flush_copy_sync: written without its copy");
   CHECK(copies("cfg/recovery", SAVE_BACKUP_UNSAVED, newest,
            sizeof(newest)) == 1 && holds(newest, p1, BIG),
         "flush_copy_sync: the image is not in one recovery file");
   CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 1,
         "flush_copy_sync: not one recovery notice");
   CHECK(notice_num == 1, "flush_copy_sync: another notice");
   CHECK(logged('W', "Unsaved progress") == 1,
         "flush_copy_sync: the recovery file not logged");

   memcpy(ram, p2, BIG);
   event_flush_save_files(true, false, bsl);
   CHECK(copies("cfg/recovery", SAVE_BACKUP_UNSAVED, newest,
            sizeof(newest)) == 2 && holds(newest, p2, BIG),
         "flush_copy_sync: the second image is not in a recovery file");
   CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 2,
         "flush_copy_sync: the second not shown");

   memcpy(ram, l1, BIG);
   event_save_files(true, false, true, true, NULL);
   CHECK(holds(save, cut, (size_t)cut_len),
         "flush_copy_sync: written at close");
   CHECK(copies("cfg/recovery", SAVE_BACKUP_UNSAVED, newest,
            sizeof(newest)) == 3 && holds(newest, l1, BIG),
         "flush_copy_sync: the close's image is not in a recovery file");
   CHECK(notice_num == 3, "flush_copy_sync: not one notice each");
   CHECK(logged('W', "Unsaved progress") == 3,
         "flush_copy_sync: not one log line each");
   path_deinit_savefile();
   clean();
}

/* With no worker, the same memory flushed again into a failing save,
 * and at the close: one recovery file, in whichever folder the chain
 * reached, told and logged each time. Only the newest is compared. */
static void lane_flush_same_image_sync(void)
{
   static unsigned char cut[BIG * 2], ram[BIG], p1[BIG], p2[BIG];
   char newest[PATH_MAX_LENGTH];
   int64_t cut_len = 0;
   unsigned c;
   const int bsl   = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   fill(p1, BIG, 67);
   fill(p2, BIG, 68);
   for (c = 0; c < 2; c++)
   {
      const char *dir = c ? "cfg/recovery" : "backups";
      clean();
      fill(ram, BIG, 69);
      if (c)
      {
         path_mkdir("blk");
         put("blk/backups", "x", 1);
         put_cut_rzip("blk/game.srm", ram, BIG, cut, &cut_len);
      }
      else
         put(SAVE, ram, BIG);
      core_data = ram;
      core_size = BIG;
      path_init_savefile_new();
      path_init_savefile_rtc(c ? "blk/game.srm" : SAVE);
      event_load_save_files(false);
      notice_num = 0;
      if (!c)
      {
         use_test_vfs(true);
         set_vfs(4096, false);
      }

      memcpy(ram, p1, BIG);
      event_flush_save_files(true, false, bsl);
      event_flush_save_files(true, false, bsl);
      CHECK(copies(dir, SAVE_BACKUP_UNSAVED, newest, sizeof(newest)) == 1
            && holds(newest, p1, BIG),
            "same_sync: the same image is not in one recovery file");
      CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 2 && notice_num == 2,
            "same_sync: not told each time");
      CHECK(logged('W', "Unsaved progress") == 2
            && logged('W', path_basename(newest)) == 2,
            "same_sync: its path not logged each time");

      /* A close with the image held: that file again. */
      event_save_files(true, false, true, true, NULL);
      CHECK(copies(dir, SAVE_BACKUP_UNSAVED, newest, sizeof(newest)) == 1,
            "same_sync: the close made another");
      CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 3 && notice_num == 3
            && logged('W', path_basename(newest)) == 3,
            "same_sync: the close not told and logged");

      /* An older one holding it does not count. */
      memcpy(ram, p2, BIG);
      event_flush_save_files(true, false, bsl);
      memcpy(ram, p1, BIG);
      event_flush_save_files(true, false, bsl);
      CHECK(copies(dir, SAVE_BACKUP_UNSAVED, newest, sizeof(newest)) == 3
            && holds(newest, p1, BIG),
            "same_sync: compared with more than the newest");
      CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 5 && notice_num == 5,
            "same_sync: not one notice each");
      set_vfs(0, false);
      use_test_vfs(false);
      path_deinit_savefile();
   }
   clean();
}

/* With no worker, a flush whose write fails: its image goes to a
 * recovery file and the copy stays owed, of the save still there. */
static void lane_flush_write_fails_sync(void)
{
   static unsigned char disk[BIG], ram[BIG], p1[BIG], l1[BIG];
   char newest[PATH_MAX_LENGTH];
   const int bsl = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   clean();
   fill(disk, BIG, 64);
   fill(p1, BIG, 65);
   fill(l1, BIG, 66);
   put(SAVE, disk, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   CHECK(event_load_save_files(false), "flush_write_sync: no load");
   use_test_vfs(true);
   set_vfs(4096, false);

   memcpy(ram, p1, BIG);
   event_flush_save_files(true, false, bsl);
   CHECK(holds(SAVE, disk, BIG), "flush_write_sync: the save changed");
   CHECK(!exists(CLOSE_TEMP), "flush_write_sync: the temporary was left");
   CHECK(copies("backups", SAVE_BACKUP_UNSAVED, newest, sizeof(newest)) == 1
         && holds(newest, p1, BIG),
         "flush_write_sync: the image is not in one recovery file");
   CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 1 && notice_num == 1,
         "flush_write_sync: not one recovery notice");

   set_vfs(0, false);
   memcpy(ram, l1, BIG);
   event_save_files(true, false, true, true, NULL);
   CHECK(holds(SAVE, l1, BIG), "flush_write_sync: not written after");
   CHECK(copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, newest,
            sizeof(newest)) == 1 && holds(newest, disk, BIG),
         "flush_write_sync: the old save is not in one copy");
   CHECK(copies("backups", SAVE_BACKUP_UNSAVED, NULL, 0) == 1
         && copies("backups", SAVE_BACKUP_REASON_LAST, NULL, 0) == 2,
         "flush_write_sync: another recovery file or copy");
   CHECK(notice_num == 1, "flush_write_sync: another notice");
   use_test_vfs(false);
   path_deinit_savefile();
   clean();
}

/* A core that reports no save RAM at load: the file is copied
 * before the close writes over it; no notice. */
static void lane_late_core(void)
{
   static unsigned char disk[BIG], ram[BIG];
   char newest[PATH_MAX_LENGTH];

   clean();
   fill(disk, BIG, 20);
   put(SAVE, disk, BIG);
   core_data = NULL;
   core_size = 0;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   CHECK(!event_load_save_files(false), "late_core: loaded without memory");
   CHECK(notice_num == 0, "late_core: a notice for a late core");
   fill(ram, BIG, 21);
   core_data = ram;
   core_size = BIG;
   event_save_files(true, false, true, true, NULL);
   CHECK(copies("backups", SAVE_BACKUP_NOT_LOADED, newest, sizeof(newest)) == 1,
         "late_core: no not-loaded copy");
   CHECK(holds(newest, disk, BIG), "late_core: the copy is not the old save");
   CHECK(holds(SAVE, ram, BIG), "late_core: the close did not write");
   path_deinit_savefile();
   clean();
}

/* A full disk at close: the old file stays, no temporary is left and
 * the image goes to a recovery file. */
static void lane_close_full_disk(void)
{
   static unsigned char disk[BIG], ram[BIG];
   char newest[PATH_MAX_LENGTH];
   unsigned c;

   for (c = 0; c < 2; c++)
   {
      clean();
      fill(disk, BIG, 1);
      fill(ram, BIG, 2);
      put(SAVE, disk, BIG);
      core_data = ram;
      core_size = BIG;
      path_init_savefile_new();
      path_init_savefile_rtc(SAVE);
      use_test_vfs(true);
      set_vfs(4096, false);
      event_save_files(true, c != 0, true, true, NULL);
      set_vfs(0, false);
      use_test_vfs(false);
      CHECK(holds(SAVE, disk, BIG), "close_full_disk: the save changed");
      CHECK(!exists(CLOSE_TEMP), "close_full_disk: a temporary was left");
      CHECK(copies("backups", SAVE_BACKUP_UNSAVED, newest,
               sizeof(newest)) == 1, "close_full_disk: no recovery file");
      CHECK(holds(newest, ram, BIG),
            "close_full_disk: the recovery is not the image");
      CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 1,
            "close_full_disk: not one recovery notice");
      path_deinit_savefile();
   }
   clean();
}

/* An unchanged raw save, compression on: the close write leaves it. */
static void lane_close_unchanged(void)
{
   static unsigned char disk[BIG], ram[BIG];
   unsigned before;

   clean();
   fill(disk, BIG, 3);
   memset(ram, 0, BIG);
   put(SAVE, disk, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   use_test_vfs(true);
   CHECK(event_load_save_files(false), "close_unchanged: no load");
   CHECK(!memcmp(ram, disk, BIG), "close_unchanged: load differs");
   before = get_write_opens();
   event_save_files(true, true, true, true, NULL);
   CHECK(get_write_opens() == before, "close_unchanged: the save was written");
   use_test_vfs(false);
   CHECK(holds(SAVE, disk, BIG), "close_unchanged: raw bytes changed");
   path_deinit_savefile();
   clean();
}

/* Bytes an unsynced write left are written again by a synced request,
 * then skipped; an unsynced request skips them. */
static void lane_close_sync(void)
{
   static unsigned char disk[BIG], ram[BIG];
   unsigned c, before, synced;

   for (c = 0; c < 2; c++)
   {
      clean();
      fill(disk, BIG, 8);
      fill(ram, BIG, 9);
      put(SAVE, disk, BIG);
      core_data = ram;
      core_size = BIG;
      path_init_savefile_new();
      path_init_savefile_rtc(SAVE);
      use_test_vfs(true);
      event_save_files(true, c != 0, false, false, NULL);
      before = get_write_opens();
      event_save_files(true, c != 0, false, false, NULL);
      CHECK(get_write_opens() == before,
            "close_sync: an unsynced request rewrote matching bytes");
      synced = synced_writes;
      event_save_files(true, c != 0, true, true, NULL);
      CHECK(get_write_opens() > before,
            "close_sync: a synced request skipped unsynced bytes");
      CHECK(synced_writes > synced, "close_sync: the rewrite was not synced");
      before = get_write_opens();
      event_save_files(true, c != 0, true, true, NULL);
      CHECK(get_write_opens() == before,
            "close_sync: synced bytes were written again");
      use_test_vfs(false);
      path_deinit_savefile();
   }
   clean();
}

static unsigned char erased_disk[BIG];

/* A save if @file, and save RAM that reports @load_size bytes at load
 * and 0 at close, its pointer kept if @pointer. */
static void close_at_zero(bool file, size_t load_size, bool pointer,
      bool compress)
{
   static unsigned char ram[BIG];

   clean();
   fill(erased_disk, BIG, 11);
   if (file)
      put(SAVE, erased_disk, BIG);
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = load_size;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   event_load_save_files(false);
   core_data = pointer ? ram : NULL;
   core_size = 0;
   event_save_files(true, compress, true, true, NULL);
   path_deinit_savefile();
}

/* Save RAM erased to 0 bytes, its pointer kept: the close write
 * empties the save and makes none.  Memory with no size all session,
 * or no pointer, leaves the save alone. */
static void lane_close_erased(void)
{
   unsigned c;

   for (c = 0; c < 2; c++)
   {
      close_at_zero(true, BIG, true, c != 0);
      CHECK(c ? emptied(SAVE) : holds(SAVE, erased_disk, 0),
            "close_erased: the save was not emptied");
      CHECK(!exists(CLOSE_TEMP), "close_erased: a temporary was left");
      close_at_zero(false, BIG, true, c != 0);
      CHECK(!exists(SAVE) && !exists(CLOSE_TEMP),
            "close_erased: an erase made a save");
   }
   close_at_zero(true, 0, true, false);
   CHECK(holds(SAVE, erased_disk, BIG),
         "close_erased: memory with no size changed the save");
   close_at_zero(false, 0, true, false);
   CHECK(!exists(SAVE), "close_erased: memory with no size made a save");
   close_at_zero(true, BIG, false, false);
   CHECK(holds(SAVE, erased_disk, BIG),
         "close_erased: vanished memory changed the save");
   /* An erase whose write fails leaves nothing to recover. */
   use_test_vfs(true);
   set_vfs(0, true);
   close_at_zero(true, BIG, true, false);
   set_vfs(0, false);
   use_test_vfs(false);
   CHECK(holds(SAVE, erased_disk, BIG),
         "close_erased: a failed erase changed the save");
   CHECK(copies("backups", SAVE_BACKUP_UNSAVED, NULL, 0) == 0
         && copies("cfg/recovery", SAVE_BACKUP_REASON_LAST, NULL, 0) == 0,
         "close_erased: a recovery file for an erase");
   CHECK(notices(MSG_SAVE_WRITE_FAILED) == 1
         && notices(MSG_SAVE_WRITE_RECOVERED) == 0,
         "close_erased: a failed erase not shown once");
   clean();
}

#ifdef HAVE_THREADS
static unsigned joins;
static unsigned creates;
static bool fail_create;
static retro_time_t now_usec;

/* Each worker's handle, so a join can tell whether the worker has
 * ended; only stop() and autosave_deinit() join running ones. */
#define MAX_WORKERS 64
static sthread_t *worker_threads[MAX_WORKERS];
static autosave_t *worker_handles[MAX_WORKERS];
static bool joining_all;

/* The next worker started is held after a wake from its wait for a
 * copy until the gate opens.  gate_ec is set while no worker runs. */
static bool gate_next;
static retro_eventcount_t *gate_ec;
static retro_atomic_int_t gate_parked;
static retro_atomic_int_t gate_open;
/* Entries to and returns from a worker's wait for a copy. */
static retro_atomic_int_t wait_entries;
static retro_atomic_int_t wait_leaves;

sthread_t *test_sthread_create(void (*thread_func)(void*), void *userdata)
{
   unsigned i;
   sthread_t *t;
   creates++;
   if (fail_create)
      return NULL;
   if (gate_next)
   {
      gate_next = false;
      gate_ec   = &((autosave_t*)userdata)->ec;
   }
   t = sthread_create(thread_func, userdata);
   for (i = 0; t && i < MAX_WORKERS; i++)
      if (!worker_threads[i])
      {
         worker_threads[i] = t;
         worker_handles[i] = (autosave_t*)userdata;
         break;
      }
   return t;
}

void test_sthread_join(sthread_t *thread)
{
   unsigned i;
   joins++;
   for (i = 0; i < MAX_WORKERS; i++)
      if (worker_threads[i] == thread)
      {
         if (!retro_atomic_load_acquire_int(&worker_handles[i]->exited))
         {
            CHECK(joining_all, "a check joined a worker still running");
            /* Or the join waits for a held worker forever. */
            retro_atomic_store_release_int(&gate_open, 1);
         }
         worker_threads[i] = NULL;
      }
   sthread_join(thread);
}

void test_commit_wait(retro_eventcount_t *ec, int key)
{
   bool held = ec == gate_ec;
   retro_atomic_fetch_add_int(&wait_entries, 1);
   if (held)
      retro_atomic_store_release_int(&gate_parked, 1);
   retro_eventcount_commit_wait(ec, key);
   retro_atomic_fetch_add_int(&wait_leaves, 1);
   while (held && !retro_atomic_load_acquire_int(&gate_open))
      retro_sleep(1);
}

retro_time_t test_time_usec(void)
{
   return now_usec;
}

static autosave_t *slots[1];

static void wait_requested(void)
{
   unsigned i;
   for (i = 0; i < 5000; i++)
   {
      if (     slots[0]
            && retro_atomic_load_acquire_int(&slots[0]->snapshot_requested))
         return;
      retro_sleep(1);
   }
   CHECK(0, "snapshot request timed out");
}

/* One check: the worker copies, writes if it differs, and asks again. */
static void capture_and_wait(void)
{
   autosave_lock();
   autosave_unlock();
   wait_requested();
}

/* A worker for @path; it has slot 0's session state when the save
 * file list is built. */
static void start_at(const char *path, void *ram, size_t len,
      bool compress)
{
   core_data = ram;
   core_size = len;
   slots[0]  = autosave_new(path, ram, len, 0, compress,
         RETRO_MEMORY_SAVE_RAM, false, save_file_get(0));
   CHECK(slots[0] != NULL, "autosave_new failed");
   autosave_state.list = slots;
   autosave_state.num  = 1;
   wait_requested();
}

static void start(void *ram, size_t len, bool compress)
{
   start_at(SAVE, ram, len, compress);
}

static void stop(void)
{
   joining_all         = true;
   autosave_free_chain(slots[0]);
   joining_all         = false;
   slots[0]            = NULL;
   autosave_state.list = NULL;
   autosave_state.num  = 0;
}

/* @n images have reached the save. Polls no fopen of it, which can
 * refuse the worker's rename on Windows. */
static bool wait_puts(unsigned n)
{
   unsigned i;
   for (i = 0; i < 5000; i++)
   {
      unsigned done;
      VFS_LOCK();
      done = put_num;
      VFS_UNLOCK();
      if (done >= n)
         return true;
      retro_sleep(1);
   }
   return false;
}

/* The flushes out are written, or failed, and given back, and the
 * worker has gone on to wait again: it owes and asks nothing more. */
static void wait_flushed(void)
{
   unsigned i;
   for (i = 0; i < 5000; i++)
   {
      if (!autosave_flush_waiting(slots[0]))
      {
         int left = retro_atomic_load_acquire_int(&wait_leaves);
         if (retro_atomic_load_acquire_int(&wait_entries) > left)
            return;
      }
      retro_sleep(1);
   }
   CHECK(0, "flush timed out");
}

/* With a worker resting a long interval: the flush before a
 * state load wakes it and it writes the save; the main thread only
 * copies. The next write after the load backs that up first. */
static void lane_state_load_copy_worker(void)
{
   static unsigned char disk[BIG], ram[BIG], p1[BIG], l1[BIG], p2[BIG];
   char newest[PATH_MAX_LENGTH];
   unsigned opens;
   const int bsl = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   clean();
   fill(disk, BIG, 41);
   fill(p1, BIG, 42);
   fill(l1, BIG, 43);
   fill(p2, BIG, 44);
   put(SAVE, disk, BIG);
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   CHECK(event_load_save_files(false), "state_load: no load");
   use_test_vfs(true);
   main_thread      = sthread_get_current_thread_id();
   main_write_opens = 0;
   slots[0] = autosave_new(SAVE, ram, BIG, 100, false,
         RETRO_MEMORY_SAVE_RAM, false, save_file_get(0));
   CHECK(slots[0] != NULL, "state_load: autosave_new failed");
   autosave_state.list = slots;
   autosave_state.num  = 1;
   wait_requested();
   /* Unchanged: it rests 100 s without writing. */
   autosave_check();

   memcpy(ram, p1, BIG);
   event_flush_save_files(true, false, bsl);
   wait_flushed();
   CHECK(holds(SAVE, p1, BIG), "state_load: the flush was not written");
   CHECK(!copies("backups", SAVE_BACKUP_REASON_LAST, NULL, 0),
         "state_load: a copy before the load");

   memcpy(ram, l1, BIG);
   wait_requested();
   autosave_check();
   CHECK(wait_puts(2) && holds(SAVE, l1, BIG),
         "state_load: the load not written");
   CHECK(copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, newest,
            sizeof(newest)) == 1 && holds(newest, p1, BIG),
         "state_load: no copy of the pre-load save");

   memcpy(ram, p2, BIG);
   event_flush_save_files(true, false, bsl);
   wait_flushed();
   CHECK(holds(SAVE, p2, BIG), "state_load: second flush not written");
   memcpy(ram, l1, BIG);
   wait_requested();
   autosave_check();
   CHECK(wait_puts(4) && holds(SAVE, l1, BIG),
         "state_load: second load not written");
   CHECK(copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, newest,
            sizeof(newest)) == 2 && holds(newest, p2, BIG),
         "state_load: no copy for the second load");

   /* Unchanged: the flush wakes it from its rest; nothing written. */
   opens = get_write_opens();
   event_flush_save_files(true, false, 0);
   wait_requested();
   CHECK(get_write_opens() == opens, "state_load: unchanged, written");
   CHECK(main_write_opens == 0, "state_load: the main thread wrote");
   stop();
   use_test_vfs(false);
   path_deinit_savefile();
   clean();
}

/* A loaded save with its session state; the test VFS on. */
static void flush_lane_begin(const void *disk, void *ram)
{
   clean();
   put(SAVE, disk, BIG);
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   CHECK(event_load_save_files(false), "flush lane: no load");
   use_test_vfs(true);
   main_thread      = sthread_get_current_thread_id();
   main_write_opens = 0;
}

static void flush_lane_end(void)
{
   stop();
   gate_next = false;
   gate_ec   = NULL;
   set_vfs(0, false);
   use_test_vfs(false);
   path_deinit_savefile();
   clean();
}

/* A worker parked in its wait for a copy; it writes nothing after its
 * next wake until the gate opens. */
static void start_parked_at(const char *path, void *ram)
{
   unsigned i;
   retro_atomic_int_init(&gate_parked, 0);
   retro_atomic_int_init(&gate_open, 0);
   gate_next = true;
   start_at(path, ram, BIG, false);
   for (i = 0; i < 5000 && !retro_atomic_load_acquire_int(&gate_parked); i++)
      retro_sleep(1);
   CHECK(retro_atomic_load_acquire_int(&gate_parked),
         "parked: the worker did not wait");
}

static void start_parked(void *ram)
{
   start_parked_at(SAVE, ram);
}

/* A flush and a filled snapshot pending together, whichever came
 * first: the flush is written and the snapshot, which the next check
 * makes again, is not. */
static void lane_flush_with_snapshot(void)
{
   static unsigned char disk[BIG], ram[BIG], s1[BIG], p1[BIG], l1[BIG];
   char newest[PATH_MAX_LENGTH];
   unsigned c;
   const int bsl = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   fill_id(disk, BIG, 70, 'D');
   fill_id(s1, BIG, 71, 'S');
   fill_id(p1, BIG, 72, 'P');
   fill_id(l1, BIG, 73, 'L');
   for (c = 0; c < 2; c++)
   {
      flush_lane_begin(disk, ram);
      start_parked(ram);
      if (c)
      {
         memcpy(ram, s1, BIG);
         autosave_check();
      }
      memcpy(ram, p1, BIG);
      event_flush_save_files(true, false, bsl);
      memcpy(ram, l1, BIG);
      if (!c)
         autosave_check();
      CHECK(  !retro_atomic_load_acquire_int(&slots[0]->snapshot_requested)
            && autosave_flush_waiting(slots[0]),
            "with_snapshot: not both pending");
      CHECK(holds(SAVE, disk, BIG), "with_snapshot: written while held");

      retro_atomic_store_release_int(&gate_open, 1);
      wait_flushed();
      wait_requested();
      CHECK(holds(SAVE, p1, BIG), "with_snapshot: the flush not written");
      CHECK(written_in_order("P"), "with_snapshot: a snapshot was written");
      CHECK(!copies("backups", SAVE_BACKUP_REASON_LAST, NULL, 0),
            "with_snapshot: a copy before the next write");

      capture_and_wait();
      CHECK(holds(SAVE, l1, BIG), "with_snapshot: the load not written");
      CHECK(written_in_order("PL"), "with_snapshot: written out of order");
      CHECK(copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, newest,
               sizeof(newest)) == 1 && holds(newest, p1, BIG),
            "with_snapshot: the pre-load save is not in one copy");
      CHECK(copies("backups", SAVE_BACKUP_REASON_LAST, NULL, 0) == 1,
            "with_snapshot: another copy");
      CHECK(logged('I', "is backed up before its next write") == 1
            && logged('I', "Backed up") == 1,
            "with_snapshot: not one log line per load and per copy");
      CHECK(quiet(), "with_snapshot: a recovery file, notice or warning");
      CHECK(main_write_opens == 0, "with_snapshot: the main thread wrote");
      flush_lane_end();
   }
}

/* A second flush queues behind the first and both are written in
 * order, each owing its own copy. A third, with both out, adds only
 * what it owes to the waiting one: its image is written nowhere. */
static void lane_flush_queue(void)
{
   static unsigned char disk[BIG], ram[BIG], p1[BIG], p2[BIG], p3[BIG],
                        l1[BIG], p4[BIG], p5[BIG], p6[BIG];
   char newest[PATH_MAX_LENGTH];
   const int bsl = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   fill_id(disk, BIG, 74, 'D');
   fill_id(p1, BIG, 75, 'P');
   fill_id(p2, BIG, 76, 'Q');
   fill_id(p3, BIG, 77, 'R');
   fill_id(l1, BIG, 78, 'L');
   fill_id(p4, BIG, 79, 'T');
   fill_id(p5, BIG, 80, 'U');
   fill_id(p6, BIG, 73, 'V');
   flush_lane_begin(disk, ram);
   start_parked(ram);

   memcpy(ram, p1, BIG);
   event_flush_save_files(true, false, bsl);
   memcpy(ram, p2, BIG);
   event_flush_save_files(true, false, 0);
   memcpy(ram, p3, BIG);
   event_flush_save_files(true, false, bsl);
   memcpy(ram, l1, BIG);
   CHECK(holds(SAVE, disk, BIG), "queue: written while held");

   retro_atomic_store_release_int(&gate_open, 1);
   wait_flushed();
   wait_requested();
   CHECK(holds(SAVE, p2, BIG), "queue: the second image not written");
   CHECK(written_in_order("PQ"), "queue: not the two images in order");
   CHECK(copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, newest,
            sizeof(newest)) == 1 && holds(newest, p1, BIG),
         "queue: the first image is not in one copy");

   /* The second image owed nothing; the third load's copy is owed now. */
   capture_and_wait();
   CHECK(holds(SAVE, l1, BIG), "queue: the load not written");
   CHECK(written_in_order("PQL"), "queue: the load out of order");
   CHECK(copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, newest,
            sizeof(newest)) == 2 && holds(newest, p2, BIG),
         "queue: the third load's copy was not made");

   /* Slot 1 holds the older image now: it still goes first. */
   memcpy(ram, p4, BIG);
   event_flush_save_files(true, false, 0);
   wait_flushed();
   wait_requested();
   retro_atomic_store_release_int(&gate_open, 0);
   memcpy(ram, p5, BIG);
   event_flush_save_files(true, false, 0);
   memcpy(ram, p6, BIG);
   event_flush_save_files(true, false, 0);
   retro_atomic_store_release_int(&gate_open, 1);
   wait_flushed();
   wait_requested();
   CHECK(written_in_order("PQLTUV"), "queue: a later flush out of order");
   CHECK(holds(SAVE, p6, BIG), "queue: a later flush not written");
   CHECK(copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, NULL, 0) == 2,
         "queue: a copy for a flush that owed none");
   CHECK(copies("backups", SAVE_BACKUP_REASON_LAST, NULL, 0) == 2,
         "queue: another copy");
   CHECK(logged('I', "is backed up before its next write") == 2
         && logged('I', "Backed up") == 2,
         "queue: not one log line per load and per copy");
   CHECK(quiet(), "queue: a recovery file, notice or warning");
   CHECK(main_write_opens == 0, "queue: the main thread wrote");
   flush_lane_end();
}

/* A slot whose memory changed size is not retired while a flush is
 * out; once it is written, it is, and the new worker backs it up. */
static void lane_flush_retire_guard(void)
{
   static unsigned char disk[BIG], ram[BIG], p1[BIG], big[BIG + 64];
   char newest[PATH_MAX_LENGTH];
   unsigned i, before;
   autosave_t *held;
   const int bsl = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   fill_id(disk, BIG, 81, 'D');
   fill_id(p1, BIG, 82, 'P');
   fill_id(big, sizeof(big), 83, 'B');
   flush_lane_begin(disk, ram);
   start_parked(ram);
   held = slots[0];

   memcpy(ram, p1, BIG);
   event_flush_save_files(true, false, bsl);
   core_data = big;
   core_size = sizeof(big);
   before    = creates;
   for (i = 0; i < 3; i++)
      autosave_check();
   CHECK(slots[0] == held && !autosave_quitting(held) && creates == before,
         "retire_guard: retired with a flush out");
   CHECK(autosave_flush_waiting(held), "retire_guard: the flush not out");

   retro_atomic_store_release_int(&gate_open, 1);
   wait_flushed();
   CHECK(holds(SAVE, p1, BIG), "retire_guard: the flush not written");
   wait_requested();
   for (i = 0; i < 5000 && slots[0] == held; i++)
   {
      autosave_check();
      retro_sleep(1);
   }
   CHECK(slots[0] != held && creates == before + 1,
         "retire_guard: not retired once written");
   wait_requested();
   capture_and_wait();
   CHECK(holds(SAVE, big, sizeof(big)), "retire_guard: new size not written");
   CHECK(written_in_order("PB"), "retire_guard: written out of order");
   CHECK(copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, newest,
            sizeof(newest)) == 1 && holds(newest, p1, BIG),
         "retire_guard: the flush is not in one copy");
   CHECK(copies("backups", SAVE_BACKUP_REASON_LAST, NULL, 0) == 1,
         "retire_guard: another copy");
   CHECK(logged('I', "is backed up before its next write") == 1
         && logged('I', "Backed up") == 1,
         "retire_guard: not one log line per load and per copy");
   CHECK(quiet(), "retire_guard: a recovery file, notice or warning");
   CHECK(main_write_opens == 0, "retire_guard: the main thread wrote");
   flush_lane_end();
}

/* Quit with one flush out, and with two: all are written, in order, a
 * filled snapshot is dropped, and the close pays what the last owes. */
static void lane_flush_at_quit(void)
{
   static unsigned char disk[BIG], ram[BIG], p1[BIG], p2[BIG], l1[BIG];
   char newest[PATH_MAX_LENGTH];
   unsigned c;
   const int bsl = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   fill_id(disk, BIG, 84, 'D');
   fill_id(p1, BIG, 85, 'P');
   fill_id(p2, BIG, 86, 'Q');
   fill_id(l1, BIG, 87, 'L');
   for (c = 0; c < 2; c++)
   {
      flush_lane_begin(disk, ram);
      start_parked(ram);
      memcpy(ram, p1, BIG);
      event_flush_save_files(true, false, bsl);
      if (c)
      {
         memcpy(ram, p2, BIG);
         event_flush_save_files(true, false, bsl);
      }
      memcpy(ram, l1, BIG);
      autosave_check();

      stop();
      CHECK(holds(SAVE, c ? p2 : p1, BIG), "at_quit: a flush not written");
      CHECK(written_in_order(c ? "PQ" : "P"), "at_quit: not those in order");
      CHECK(copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, newest,
               sizeof(newest)) == c && (!c || holds(newest, p1, BIG)),
            "at_quit: the first flush is not in a copy");
      CHECK(main_write_opens == 0, "at_quit: the main thread wrote");

      event_save_files(true, false, true, true, NULL);
      CHECK(holds(SAVE, l1, BIG), "at_quit: the close did not write");
      CHECK(written_in_order(c ? "PQL" : "PL"), "at_quit: close out of order");
      CHECK(copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, newest,
               sizeof(newest)) == c + 1 && holds(newest, c ? p2 : p1, BIG),
            "at_quit: the close did not copy the last flush");
      CHECK(copies("backups", SAVE_BACKUP_REASON_LAST, NULL, 0) == c + 1,
            "at_quit: another copy");
      CHECK(logged('I', "is backed up before its next write") == c + 1
            && logged('I', "Backed up") == c + 1,
            "at_quit: not one log line per load and per copy");
      CHECK(quiet(), "at_quit: a recovery file, notice or warning");
      gate_next = false;
      gate_ec   = NULL;
      use_test_vfs(false);
      path_deinit_savefile();
      clean();
   }
}

/* A live worker's memory changed size: the flush is written on the
 * main thread, then the copy is owed; the next check retires the
 * worker and the new one pays it. */
static void lane_flush_size_changed(void)
{
   static unsigned char disk[BIG], ram[BIG], n1[BIG + 64], l1[BIG + 64];
   char newest[PATH_MAX_LENGTH];
   autosave_t *old;
   const int bsl = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   fill_id(disk, BIG, 88, 'D');
   fill_id(n1, sizeof(n1), 89, 'N');
   fill_id(l1, sizeof(l1), 90, 'L');
   flush_lane_begin(disk, ram);
   start(ram, BIG, false);
   old = slots[0];

   core_data = n1;
   core_size = sizeof(n1);
   event_flush_save_files(true, false, bsl);
   CHECK(holds(SAVE, n1, sizeof(n1)), "size_changed: not written");
   CHECK(main_write_opens == 1, "size_changed: not on the main thread");
   CHECK(!copies("backups", SAVE_BACKUP_REASON_LAST, NULL, 0),
         "size_changed: a copy before the next write");
   CHECK(slots[0] == old && !autosave_quitting(old),
         "size_changed: retired by the flush");

   core_data = l1;
   autosave_check();
   CHECK(slots[0] != old, "size_changed: not retired at the next check");
   wait_requested();
   capture_and_wait();
   CHECK(holds(SAVE, l1, sizeof(l1)), "size_changed: the load not written");
   CHECK(written_in_order("NL"), "size_changed: written out of order");
   CHECK(copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, newest,
            sizeof(newest)) == 1 && holds(newest, n1, sizeof(n1)),
         "size_changed: the flush is not in one copy");
   CHECK(copies("backups", SAVE_BACKUP_REASON_LAST, NULL, 0) == 1,
         "size_changed: another copy");
   CHECK(logged('I', "is backed up before its next write") == 1
         && logged('I', "Backed up") == 1,
         "size_changed: not one log line per load and per copy");
   CHECK(quiet(), "size_changed: a recovery file, notice or warning");
   CHECK(main_write_opens == 1, "size_changed: the main thread wrote again");
   flush_lane_end();
}

/* A flush whose write fails, cut short or its rename refused: the save
 * stays, the image goes to one recovery file, one notice and no line
 * in the log from the worker; the next write backs up the old save. */
static void lane_flush_write_fails(void)
{
   static unsigned char disk[BIG], ram[BIG], p1[BIG], l1[BIG];
   char newest[PATH_MAX_LENGTH];
   unsigned c;
   const int bsl = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   fill_id(disk, BIG, 91, 'D');
   fill_id(p1, BIG, 92, 'P');
   fill_id(l1, BIG, 93, 'L');
   for (c = 0; c < 2; c++)
   {
      flush_lane_begin(disk, ram);
      start(ram, BIG, false);
      set_vfs(c ? 0 : 4096, c != 0);
      memcpy(ram, p1, BIG);
      event_flush_save_files(true, false, bsl);
      wait_flushed();
      set_vfs(0, false);
      memcpy(ram, l1, BIG);
      wait_requested();

      CHECK(holds(SAVE, disk, BIG), "flush_write: the save changed");
      CHECK(!exists(WORKER_TEMP), "flush_write: the temporary was left");
      CHECK(copies("backups", SAVE_BACKUP_UNSAVED, newest,
               sizeof(newest)) == 1 && holds(newest, p1, BIG),
            "flush_write: the image is not in one recovery file");
      CHECK(!copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, NULL, 0),
            "flush_write: a copy before the next write");

      capture_and_wait();
      CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 1 && notice_num == 1,
            "flush_write: not one recovery notice");
      CHECK(!logged('W', "") && !logged('E', ""),
            "flush_write: the worker logged");
      CHECK(holds(SAVE, l1, BIG), "flush_write: the load not written");
      CHECK(copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, newest,
               sizeof(newest)) == 1 && holds(newest, disk, BIG),
            "flush_write: the old save is not in one copy");
      CHECK(copies("backups", SAVE_BACKUP_UNSAVED, NULL, 0) == 1
            && copies("backups", SAVE_BACKUP_REASON_LAST, NULL, 0) == 2,
            "flush_write: another recovery file or copy");
      capture_and_wait();
      CHECK(notice_num == 1, "flush_write: another notice");
      CHECK(main_write_opens == 0, "flush_write: the main thread wrote");
      flush_lane_end();
   }
}

/* Flushes the image, which cannot be written, and shows it. */
static void flush_failing(void *ram, const void *image)
{
   memcpy(ram, image, BIG);
   event_flush_save_files(true, false, 1 << SAVE_BACKUP_BEFORE_STATE_LOAD);
   wait_flushed();
   capture_and_wait();
}

/* The same memory flushed again into a failing save: one recovery file,
 * in whichever folder the chain reached, each flush told. Only the
 * newest recovery file is compared, not a newer copy nor an older one. */
static void lane_flush_same_image(void)
{
   static unsigned char cut[BIG * 2], disk[BIG], ram[BIG], p1[BIG], p2[BIG];
   char newest[PATH_MAX_LENGTH];
   int64_t cut_len = 0;
   unsigned c;

   fill_id(disk, BIG, 103, 'D');
   fill_id(p1, BIG, 104, 'P');
   fill_id(p2, BIG, 105, 'Q');
   for (c = 0; c < 2; c++)
   {
      const char *save = c ? "blk/game.srm" : SAVE;
      const char *dir  = c ? "cfg/recovery" : "backups";
      clean();
      if (c)
      {
         path_mkdir("blk");
         put("blk/backups", "x", 1);
         put_cut_rzip(save, disk, BIG, cut, &cut_len);
      }
      else
         put(save, disk, BIG);
      memset(ram, 0, BIG);
      core_data = ram;
      core_size = BIG;
      path_init_savefile_new();
      path_init_savefile_rtc(save);
      event_load_save_files(false);
      notice_num = 0;
      LOG_LOCK();
      log_num = 0;
      LOG_UNLOCK();
      use_test_vfs(true);
      set_vfs(4096, false);
      start_at(save, ram, BIG, false);

      /* Besides one notice per flush: once a session, the copy that
       * cannot be made, or else the autosave write that failed. */
      flush_failing(ram, p1);
      CHECK(copies(dir, SAVE_BACKUP_UNSAVED, newest, sizeof(newest)) == 1
            && holds(newest, p1, BIG),
            "same: the image is not in one recovery file");
      CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 1
            && notice_num == 1 + c, "same: not told");
      if (!c)
         CHECK(copies(dir, SAVE_BACKUP_BEFORE_STATE_LOAD, NULL, 0) == 1,
               "same: no newer copy to skip over");

      flush_failing(ram, p1);
      CHECK(copies(dir, SAVE_BACKUP_UNSAVED, newest, sizeof(newest)) == 1
            && holds(newest, p1, BIG),
            "same: the same image made another file");
      CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 2
            && notice_num == 3, "same: the same image not told");

      flush_failing(ram, p2);
      flush_failing(ram, p1);
      CHECK(copies(dir, SAVE_BACKUP_UNSAVED, newest, sizeof(newest)) == 3
            && holds(newest, p1, BIG),
            "same: compared with more than the newest");
      flush_failing(ram, p1);
      CHECK(copies(dir, SAVE_BACKUP_UNSAVED, NULL, 0) == 3,
            "same: the newest not compared");
      CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 5
            && notice_num == 6, "same: not one notice each");

      /* The chain's first folder works again: the newest is still in
       * the fallback, and is the file. */
      if (c)
      {
         filestream_delete("blk/backups");
         flush_failing(ram, p1);
         CHECK(!copies("blk/backups", SAVE_BACKUP_UNSAVED, NULL, 0)
               && copies(dir, SAVE_BACKUP_UNSAVED, NULL, 0) == 3,
               "same: the newest in another folder not compared");
         CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 6 && notice_num == 7,
               "same: the other folder's not told");
      }
      capture_and_wait();
      CHECK(holds(save, c ? cut : disk, c ? (size_t)cut_len : BIG),
            "same: the save changed");
      CHECK(!logged('W', "") && !logged('E', ""), "same: the worker logged");
      stop();
      set_vfs(0, false);
      use_test_vfs(false);
      path_deinit_savefile();
      clear_dir("blk/backups");
      path_rmdir("blk/backups");
   }
   clean();
}

/* A flush whose owed copy cannot be made: the save stays and the image
 * goes to one recovery file; both notices, each once. Later checks
 * only retry; the close's image is a recovery file of its own. */
static void lane_flush_copy_fails(void)
{
   static unsigned char cut[BIG * 2], ram[BIG], p1[BIG], l1[BIG];
   char newest[PATH_MAX_LENGTH];
   int64_t cut_len  = 0;
   const char *save = "blk/game.srm";
   unsigned i;
   const int bsl    = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   clean();
   path_mkdir("blk");
   put("blk/backups", "x", 1);
   fill(ram, BIG, 94);
   fill(p1, BIG, 95);
   fill(l1, BIG, 96);
   put_cut_rzip(save, ram, BIG, cut, &cut_len);
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(save);
   CHECK(!event_load_save_files(false), "flush_copy: a cut rzip loaded");
   notice_num = 0;
   start_at(save, ram, BIG, false);

   memcpy(ram, p1, BIG);
   event_flush_save_files(true, false, bsl);
   wait_flushed();
   memcpy(ram, l1, BIG);
   wait_requested();
   CHECK(holds(save, cut, (size_t)cut_len),
         "flush_copy: written without its copy");
   CHECK(copies("cfg/recovery", SAVE_BACKUP_UNSAVED, newest,
            sizeof(newest)) == 1 && holds(newest, p1, BIG),
         "flush_copy: the image is not in one recovery file");

   for (i = 0; i < 3; i++)
      capture_and_wait();
   CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 1,
         "flush_copy: not one recovery notice");
   CHECK(notices(MSG_SAVE_BACKUP_FAILED) == 1,
         "flush_copy: not one backup notice");
   CHECK(notice_num == 2, "flush_copy: another notice");
   CHECK(!logged('W', "Unsaved progress"), "flush_copy: the worker logged");
   CHECK(copies("cfg/recovery", SAVE_BACKUP_UNSAVED, NULL, 0) == 1,
         "flush_copy: a check made a recovery file");
   CHECK(holds(save, cut, (size_t)cut_len), "flush_copy: written after");
   stop();

   event_save_files(true, false, true, true, NULL);
   CHECK(holds(save, cut, (size_t)cut_len), "flush_copy: written at close");
   CHECK(copies("cfg/recovery", SAVE_BACKUP_UNSAVED, newest,
            sizeof(newest)) == 2 && holds(newest, l1, BIG),
         "flush_copy: the close's image is not in a recovery file");
   CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 2,
         "flush_copy: the close not shown");
   CHECK(logged('W', "Unsaved progress") == 1,
         "flush_copy: the close not logged");
   path_deinit_savefile();
   clean();
}

/* A flush with nowhere for its image: each is shown as not saved. */
static void lane_flush_unrecoverable(void)
{
   static unsigned char disk[BIG], ram[BIG], p1[BIG], p2[BIG], p3[BIG];
   const char *save = "blk/game.srm";
   const int bsl    = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   clean();
   save_backups_set_roots(NULL, NULL, NULL, NULL, NULL, NULL, "as_core");
   path_mkdir("blk");
   put("blk/backups", "x", 1);
   fill(disk, BIG, 97);
   fill(p1, BIG, 98);
   fill(p2, BIG, 99);
   fill(p3, BIG, 106);
   put(save, disk, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(save);
   CHECK(event_load_save_files(false), "unrecoverable: no load");
   use_test_vfs(true);
   set_vfs(4096, false);
   start_at(save, ram, BIG, false);

   memcpy(ram, p1, BIG);
   event_flush_save_files(true, false, bsl);
   wait_flushed();
   autosave_check();
   CHECK(notices(MSG_SAVE_WRITE_FAILED) == 1, "unrecoverable: not shown");
   memcpy(ram, p2, BIG);
   event_flush_save_files(true, false, bsl);
   wait_flushed();
   autosave_check();
   CHECK(notices(MSG_SAVE_WRITE_FAILED) == 2,
         "unrecoverable: the second not shown");
   CHECK(!notices(MSG_SAVE_WRITE_RECOVERED),
         "unrecoverable: told it was recovered");
   CHECK(holds(save, disk, BIG), "unrecoverable: the save changed");
   CHECK(!exists("blk/game.srm.auto.tmp"),
         "unrecoverable: the temporary was left");
   CHECK(!copies("cfg/recovery", SAVE_BACKUP_REASON_LAST, NULL, 0),
         "unrecoverable: a recovery file");
   CHECK(!logged('W', "Unsaved progress") && !logged('W', "msg"),
         "unrecoverable: the worker logged");

   /* Lost at the exit: no later session can tell. */
   memcpy(ram, p3, BIG);
   event_flush_save_files(true, false, bsl);
   stop();
   CHECK(notices(MSG_SAVE_WRITE_FAILED) == 3, "unrecoverable: not shown at exit");
   CHECK(logged_msg(MSG_SAVE_WRITE_FAILED) == 1 && logged('W', "") == 1,
         "unrecoverable: not one log line at exit");
   set_vfs(0, false);
   use_test_vfs(false);
   path_deinit_savefile();
   clean();
}

/* A flush that fails at quit: no check runs after it, and the image is
 * in a recovery file and shown all the same. */
static void lane_flush_fails_at_quit(void)
{
   static unsigned char cut[BIG * 2], disk[BIG], ram[BIG], p1[BIG], l1[BIG];
   char newest[PATH_MAX_LENGTH];
   int64_t cut_len  = 0;
   const char *save = "blk/game.srm";
   const int bsl    = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   fill_id(disk, BIG, 100, 'D');
   fill_id(p1, BIG, 101, 'P');
   fill_id(l1, BIG, 102, 'L');
   flush_lane_begin(disk, ram);
   start_parked(ram);
   set_vfs(4096, false);
   memcpy(ram, p1, BIG);
   event_flush_save_files(true, false, bsl);
   memcpy(ram, l1, BIG);
   stop();
   set_vfs(0, false);
   CHECK(holds(SAVE, disk, BIG), "fails_at_quit: the save changed");
   CHECK(copies("backups", SAVE_BACKUP_UNSAVED, newest, sizeof(newest)) == 1
         && holds(newest, p1, BIG),
         "fails_at_quit: the image is not in one recovery file");
   CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 1 && notice_num == 1,
         "fails_at_quit: not one recovery notice");
   CHECK(logged_msg(MSG_SAVE_WRITE_RECOVERED) == 1 && logged('W', "") == 1,
         "fails_at_quit: not one log line");

   event_save_files(true, false, true, true, NULL);
   CHECK(holds(SAVE, l1, BIG), "fails_at_quit: the close did not write");
   CHECK(copies("backups", SAVE_BACKUP_BEFORE_STATE_LOAD, newest,
            sizeof(newest)) == 1 && holds(newest, disk, BIG),
         "fails_at_quit: the old save is not in one copy");
   CHECK(copies("backups", SAVE_BACKUP_UNSAVED, NULL, 0) == 1
         && copies("backups", SAVE_BACKUP_REASON_LAST, NULL, 0) == 2,
         "fails_at_quit: another recovery file or copy");
   CHECK(logged('W', "") == 1, "fails_at_quit: the close logged a warning");
   gate_next = false;
   gate_ec   = NULL;
   use_test_vfs(false);
   path_deinit_savefile();
   clean();

   /* Its copy fails too: that notice is not logged here. */
   path_mkdir("blk");
   put("blk/backups", "x", 1);
   fill_id(disk, BIG, 107, 'D');
   put_cut_rzip(save, disk, BIG, cut, &cut_len);
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(save);
   event_load_save_files(false);
   notice_num = 0;
   LOG_LOCK();
   log_num = 0;
   LOG_UNLOCK();
   start_parked_at(save, ram);
   memcpy(ram, p1, BIG);
   event_flush_save_files(true, false, bsl);
   stop();
   CHECK(holds(save, cut, (size_t)cut_len),
         "fails_at_quit: written without its copy");
   CHECK(copies("cfg/recovery", SAVE_BACKUP_UNSAVED, newest,
            sizeof(newest)) == 1 && holds(newest, p1, BIG),
         "fails_at_quit: the image is not in one recovery file, copy failed");
   CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 1
         && notices(MSG_SAVE_BACKUP_FAILED) == 1 && notice_num == 2,
         "fails_at_quit: not one notice each, copy failed");
   CHECK(logged_msg(MSG_SAVE_WRITE_RECOVERED) == 1 && logged('W', "") == 1,
         "fails_at_quit: not one log line, copy failed");
   gate_next = false;
   gate_ec   = NULL;
   path_deinit_savefile();

   /* Through autosave_deinit() itself. */
   flush_lane_begin(disk, ram);
   CHECK(autosave_init(false, 1), "fails_at_quit: autosave_init failed");
   set_vfs(4096, false);
   memcpy(ram, p1, BIG);
   event_flush_save_files(true, false, bsl);
   joining_all = true;
   autosave_deinit();
   joining_all = false;
   CHECK(holds(SAVE, disk, BIG), "fails_at_quit: deinit, the save changed");
   CHECK(copies("backups", SAVE_BACKUP_UNSAVED, newest, sizeof(newest)) == 1
         && holds(newest, p1, BIG),
         "fails_at_quit: deinit, the image is not in one recovery file");
   CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 1 && notice_num == 1,
         "fails_at_quit: deinit, not one recovery notice");
   CHECK(logged_msg(MSG_SAVE_WRITE_RECOVERED) == 1 && logged('W', "") == 1,
         "fails_at_quit: deinit, not one log line");
   flush_lane_end();
}

/* A retired worker is reaped inside a check, which does not log: what
 * it raised is only shown. */
static void lane_flush_reap_no_log(void)
{
   static unsigned char disk[BIG], ram[BIG], big[BIG + 64];
   unsigned i;
   autosave_t *held;

   fill_id(disk, BIG, 108, 'D');
   fill_id(big, sizeof(big), 109, 'B');
   flush_lane_begin(disk, ram);
   start(ram, BIG, false);
   held      = slots[0];
   core_data = big;
   core_size = sizeof(big);
   autosave_check();
   CHECK(slots[0] != held && slots[0]->prev == held,
         "reap: not retired");
   for (i = 0; i < 5000 && !retro_atomic_load_acquire_int(&held->exited); i++)
      retro_sleep(1);
   CHECK(retro_atomic_load_acquire_int(&held->exited),
         "reap: the retired worker did not end");
   retro_atomic_fetch_or_int(&held->file->flags, SAVE_FILE_FLUSH_RECOVERED);
   autosave_reap(slots[0]);
   CHECK(!slots[0]->prev, "reap: not joined");
   CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 1 && notice_num == 1,
         "reap: not shown");
   CHECK(!logged('W', ""), "reap: logged inside a check");
   flush_lane_end();
}

/* Erased memory's flush that fails leaves nothing to recover: told as
 * not saved, and no empty recovery file. */
static void lane_flush_zero_bytes(void)
{
   static unsigned char disk[BIG], ram[BIG];

   fill_id(disk, BIG, 110, 'D');
   flush_lane_begin(disk, ram);
   core_size = 0;
   slots[0]  = autosave_new(SAVE, ram, 0, 0, false, RETRO_MEMORY_SAVE_RAM,
         true, save_file_get(0));
   CHECK(slots[0] != NULL, "zero_bytes: autosave_new failed");
   autosave_state.list = slots;
   autosave_state.num  = 1;
   wait_requested();
   set_vfs(0, true);
   event_flush_save_files(true, false, 0);
   wait_flushed();
   stop();
   set_vfs(0, false);
   CHECK(holds(SAVE, disk, BIG), "zero_bytes: the save changed");
   CHECK(!copies("backups", SAVE_BACKUP_UNSAVED, NULL, 0)
         && !copies("cfg/recovery", SAVE_BACKUP_REASON_LAST, NULL, 0),
         "zero_bytes: an empty recovery file");
   CHECK(notices(MSG_SAVE_WRITE_FAILED) == 1 && notice_num == 1,
         "zero_bytes: not told as not saved");
   flush_lane_end();
}

/* A full disk or a refused rename keeps the old save whole and
 * leaves no temporary; the image is written at the next check. */
static void lane_autosave_full_disk(void)
{
   static unsigned char disk[BIG], ram[BIG];
   unsigned c;

   for (c = 0; c < 2; c++)
   {
      clean();
      fill(disk, BIG, 4);
      memcpy(ram, disk, BIG);
      put(SAVE, disk, BIG);
      use_test_vfs(true);
      start(ram, BIG, c != 0);

      set_vfs(4096, false);
      fill(ram, BIG, 5);
      capture_and_wait();
      CHECK(holds(SAVE, disk, BIG), "full_disk: the save was cut");
      CHECK(!exists(WORKER_TEMP), "full_disk: the temporary was left");
      CHECK(memcmp(slots[0]->buffer, ram, BIG) != 0,
            "full_disk: a failed image counted as written");
      set_vfs(0, false);
      capture_and_wait();
      CHECK(reads_as(SAVE, ram, BIG), "full_disk: no retry after the failure");

      memcpy(disk, ram, BIG);
      set_vfs(0, true);
      fill(ram, BIG, 6);
      capture_and_wait();
      CHECK(reads_as(SAVE, disk, BIG), "rename refused: the save changed");
      CHECK(!exists(WORKER_TEMP), "rename refused: the temporary was left");
      set_vfs(0, false);
      capture_and_wait();
      CHECK(reads_as(SAVE, ram, BIG), "rename refused: no retry");
      CHECK(!exists(WORKER_TEMP), "full_disk: a temporary was left");

      stop();
      use_test_vfs(false);
   }
   clean();
}

/* Started after the load, the worker writes nothing for an
 * unchanged raw save, and compresses it only once it changes. */
static void lane_no_rewrite_at_launch(void)
{
   static unsigned char disk[BIG], ram[BIG];
   unsigned i, before;
   unsigned char head[6];
   FILE *f;

   clean();
   fill(disk, BIG, 7);
   memset(ram, 0, BIG);
   put(SAVE, disk, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   use_test_vfs(true);
   CHECK(event_load_save_files(false), "no_rewrite: no load");
   CHECK(autosave_init(true, 1), "no_rewrite: autosave_init failed");
   slots[0] = autosave_state.list ? autosave_state.list[0] : NULL;
   CHECK(slots[0] != NULL, "no_rewrite: no worker for save RAM");
   wait_requested();
   before = get_write_opens();
   for (i = 0; slots[0] && i < 3; i++)
      capture_and_wait();
   CHECK(get_write_opens() == before, "no_rewrite: an unchanged save was written");
   CHECK(holds(SAVE, disk, BIG), "no_rewrite: raw bytes changed");

   if (slots[0])
   {
      ram[0] ^= 0xFF;
      capture_and_wait();
   }
   joining_all = true;
   autosave_deinit();
   joining_all = false;
   slots[0] = NULL;
   CHECK(reads_as(SAVE, ram, BIG), "no_rewrite: a change was not written");
   memset(head, 0, sizeof(head));
   if ((f = fopen(SAVE, "rb")))
   {
      CHECK(fread(head, 1, sizeof(head), f) == sizeof(head), "short save");
      fclose(f);
   }
   CHECK(!memcmp(head, "#RZIPv", 6), "no_rewrite: a change was not compressed");
   use_test_vfs(false);
   path_deinit_savefile();
   clean();
}

/* A moved buffer is followed; a resized or vanished one retires
 * its worker without a join and a new one carries on. */
static void lane_pointer_requery(void)
{
   static unsigned char a[64], b[64], c[128];
   autosave_t *first, *second;

   clean();
   memset(a, 0, sizeof(a));
   joins = 0;
   start(a, sizeof(a), false);
   first = slots[0];

   memset(a, 1, sizeof(a));
   capture_and_wait();
   CHECK(holds(SAVE, a, sizeof(a)), "requery: first write");

   memset(b, 2, sizeof(b));
   core_data = b;
   capture_and_wait();
   CHECK(slots[0] == first, "requery: a moved buffer replaced the worker");
   CHECK(holds(SAVE, b, sizeof(b)), "requery: the moved buffer was not followed");

   memset(c, 3, sizeof(c));
   memcpy(c, b, sizeof(b));
   core_data = c;
   core_size = sizeof(c);
   autosave_lock();
   autosave_unlock();
   CHECK(slots[0] != first && slots[0] && slots[0]->prev == first,
         "requery: a resize did not replace the worker");
   CHECK(joins == 0, "requery: autosave_check joined a thread");
   wait_requested();
   capture_and_wait();
   CHECK(holds(SAVE, c, sizeof(c)), "requery: a resize was not written");
   memset(c, 4, sizeof(c));
   capture_and_wait();
   CHECK(holds(SAVE, c, sizeof(c)), "requery: the new size was not written");
   second = slots[0];

   core_data = NULL;
   core_size = 0;
   autosave_lock();
   autosave_unlock();
   CHECK(slots[0] == second && autosave_quitting(second),
         "requery: vanished memory did not retire the worker");
   autosave_lock();
   autosave_unlock();
   CHECK(slots[0] == second, "requery: replaced while memory is gone");

   core_data = c;
   core_size = sizeof(c);
   autosave_lock();
   autosave_unlock();
   CHECK(slots[0] != second && slots[0] && slots[0]->prev == second,
         "requery: returned memory did not get a worker");
   wait_requested();
   memset(c, 5, sizeof(c));
   capture_and_wait();
   CHECK(holds(SAVE, c, sizeof(c)), "requery: the returned memory was not written");

   stop();
   CHECK(joins == 3, "requery: retired workers were not joined at the end");
   clean();
}

/* What a retired worker had not written, a change or a
 * refused write, is written by the new one. */
static void lane_resize_unwritten(void)
{
   static unsigned char a[64], c[128], d[192], e[256];

   clean();
   memset(a, 1, sizeof(a));
   put(SAVE, a, sizeof(a));
   use_test_vfs(true);
   start(a, sizeof(a), false);

   memset(c, 6, sizeof(c));
   core_data = c;
   core_size = sizeof(c);
   autosave_lock();
   autosave_unlock();
   wait_requested();
   capture_and_wait();
   CHECK(holds(SAVE, c, sizeof(c)), "resize: a pending change was not written");

   set_vfs(0, true);
   memset(d, 7, sizeof(d));
   core_data = d;
   core_size = sizeof(d);
   autosave_lock();
   autosave_unlock();
   wait_requested();
   capture_and_wait();
   CHECK(holds(SAVE, c, sizeof(c)), "resize: a refused write changed the save");

   memset(e, 7, sizeof(e));
   core_data = e;
   core_size = sizeof(e);
   set_vfs(0, false);
   autosave_lock();
   autosave_unlock();
   wait_requested();
   capture_and_wait();
   CHECK(holds(SAVE, e, sizeof(e)), "resize: a retry was dropped");

   stop();
   use_test_vfs(false);
   clean();
}

/* A core that reports the extent in use grows it on an append and
 * shrinks it on an erase, the bytes before it unchanged: both are
 * written.  Memory back at its old size and bytes is not, unless its
 * last image was never written. */
static void lane_resize_extent(void)
{
   static unsigned char ram[256];
   unsigned before;

   clean();
   memset(ram, 0xFF, sizeof(ram));
   fill(ram, 128, 8);
   put(SAVE, ram, 128);
   use_test_vfs(true);
   start(ram, 128, false);

   fill(ram + 128, 64, 9);
   core_size = 192;
   autosave_lock();
   autosave_unlock();
   wait_requested();
   capture_and_wait();
   CHECK(holds(SAVE, ram, 192), "extent: a grow was not written");

   memset(ram + 64, 0xFF, 128);
   core_size = 64;
   autosave_lock();
   autosave_unlock();
   wait_requested();
   capture_and_wait();
   CHECK(holds(SAVE, ram, 64), "extent: a shrink was not written");

   core_data = NULL;
   core_size = 0;
   autosave_lock();
   autosave_unlock();
   fill(ram + 64, 32, 10);
   core_data = ram;
   core_size = 96;
   autosave_lock();
   autosave_unlock();
   wait_requested();
   capture_and_wait();
   CHECK(holds(SAVE, ram, 96), "extent: a new size after a gap was not written");

   core_data = NULL;
   core_size = 0;
   autosave_lock();
   autosave_unlock();
   core_data = ram;
   core_size = 96;
   before    = get_write_opens();
   autosave_lock();
   autosave_unlock();
   wait_requested();
   capture_and_wait();
   CHECK(get_write_opens() == before, "extent: an unchanged return was written");
   CHECK(holds(SAVE, ram, 96), "extent: an unchanged return changed the save");

   core_data = NULL;
   core_size = 0;
   autosave_lock();
   autosave_unlock();
   ram[10]  ^= 0x5A;
   core_data = ram;
   core_size = 96;
   autosave_lock();
   autosave_unlock();
   wait_requested();
   capture_and_wait();
   CHECK(holds(SAVE, ram, 96), "extent: a change while gone was not written");

   /* The new handle's first write is refused: its image is pending
    * although it matches memory. */
   set_vfs(0, true);
   core_data = NULL;
   core_size = 0;
   autosave_lock();
   autosave_unlock();
   ram[20]  ^= 0x5A;
   core_data = ram;
   core_size = 96;
   autosave_lock();
   autosave_unlock();
   wait_requested();
   capture_and_wait();
   core_data = NULL;
   core_size = 0;
   autosave_lock();
   autosave_unlock();
   set_vfs(0, false);
   core_data = ram;
   core_size = 96;
   autosave_lock();
   autosave_unlock();
   wait_requested();
   capture_and_wait();
   CHECK(holds(SAVE, ram, 96), "extent: a refused write was dropped on return");

   stop();
   use_test_vfs(false);
   clean();
}

/* Save RAM erased to 0 bytes, its pointer kept: the worker empties the
 * save and makes none, and writes the size when it comes back.  Memory
 * that vanishes writes nothing and its return is compared as before;
 * memory with no size all session gets no worker and no save. */
static void lane_resize_erased(void)
{
   static unsigned char ram[128];
   unsigned c, before;

   for (c = 0; c < 2; c++)
   {
      clean();
      fill(ram, 128, 12);
      put(SAVE, ram, 128);
      start(ram, 128, c != 0);
      core_size = 0;
      autosave_lock();
      autosave_unlock();
      wait_requested();
      capture_and_wait();
      CHECK(c ? emptied(SAVE) : holds(SAVE, ram, 0),
            "erased: the save was not emptied");
      core_size = 128;
      autosave_lock();
      autosave_unlock();
      wait_requested();
      capture_and_wait();
      CHECK(reads_as(SAVE, ram, 128), "erased: the returned size was not written");
      stop();
   }

   clean();
   start(ram, 128, false);
   core_size = 0;
   autosave_lock();
   autosave_unlock();
   wait_requested();
   capture_and_wait();
   CHECK(!exists(SAVE) && !exists(WORKER_TEMP), "erased: an erase made a save");
   stop();

   clean();
   put(SAVE, ram, 128);
   use_test_vfs(true);
   start(ram, 128, false);
   before    = get_write_opens();
   core_data = NULL;
   autosave_lock();
   autosave_unlock();
   autosave_lock();
   autosave_unlock();
   core_data = ram;
   autosave_lock();
   autosave_unlock();
   wait_requested();
   capture_and_wait();
   CHECK(get_write_opens() == before, "erased: vanished memory was written");
   CHECK(holds(SAVE, ram, 128), "erased: vanished memory changed the save");
   stop();
   use_test_vfs(false);

   clean();
   core_data = ram;
   core_size = 0;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   event_load_save_files(false);
   CHECK(autosave_init(false, 1), "erased: autosave_init failed");
   CHECK(!autosave_state.list[0], "erased: memory with no size got a worker");
   autosave_lock();
   autosave_unlock();
   autosave_deinit();
   event_save_files(true, false, true, true, NULL);
   CHECK(!exists(SAVE), "erased: memory with no size made a save");
   path_deinit_savefile();

   /* No size at load, a size mid-session, 0 at close: emptied. */
   clean();
   core_data = ram;
   core_size = 0;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   event_load_save_files(false);
   start(ram, 128, false);
   fill(ram, 128, 13);
   capture_and_wait();
   CHECK(holds(SAVE, ram, 128), "erased: the mid-session size not written");
   stop();
   core_size = 0;
   event_save_files(true, false, true, true, NULL);
   CHECK(holds(SAVE, ram, 0), "erased: a size had mid-session not emptied");
   path_deinit_savefile();
   clean();
}

/* Resizes again and again leave no retired handles behind once their
 * workers have ended. */
static void lane_retire_bounded(void)
{
   static unsigned char a[64], c[128];
   unsigned i, n;
   autosave_t *h;

   clean();
   memset(a, 1, sizeof(a));
   memset(c, 1, sizeof(c));
   joins = 0;
   start(a, sizeof(a), false);
   for (i = 0; i < 20; i++)
   {
      core_data = (i & 1) ? a : c;
      core_size = (i & 1) ? sizeof(a) : sizeof(c);
      autosave_lock();
      autosave_unlock();
      /* Again at once, while the retired worker is likely still
       * ending: it must not be joined yet. */
      autosave_lock();
      autosave_unlock();
      wait_requested();
   }
   for (i = 0; i < 5000 && slots[0]->prev; i++)
   {
      retro_sleep(1);
      autosave_lock();
      autosave_unlock();
   }
   for (n = 0, h = slots[0]->prev; h; h = h->prev)
      n++;
   CHECK(n == 0, "bounded: retired handles were kept");
   CHECK(joins == 20, "bounded: retired workers were not joined");
   stop();
   CHECK(joins == 21, "bounded: a worker was not joined at the end");
   clean();
}

/* A retired worker that has not ended is not joined: held after its
 * wake, it stays on the chain until it ends, then is joined. */
static void lane_no_join_running(void)
{
   static unsigned char a[64], c[128];
   unsigned i, before;
   autosave_t *held;

   clean();
   memset(a, 1, sizeof(a));
   memset(c, 1, sizeof(c));
   retro_atomic_int_init(&gate_parked, 0);
   retro_atomic_int_init(&gate_open, 0);
   gate_next = true;
   start(a, sizeof(a), false);
   held = slots[0];
   for (i = 0; i < 5000 && !retro_atomic_load_acquire_int(&gate_parked); i++)
      retro_sleep(1);
   CHECK(retro_atomic_load_acquire_int(&gate_parked),
         "running: the worker did not wait");

   before    = joins;
   core_data = c;
   core_size = sizeof(c);
   for (i = 0; i < 20; i++)
   {
      autosave_lock();
      autosave_unlock();
   }
   CHECK(joins == before, "running: a worker still running was joined");
   CHECK(slots[0] && slots[0]->prev == held && !slots[0]->prev->prev,
         "running: the retired handle is not alone on the chain");

   retro_atomic_store_release_int(&gate_open, 1);
   for (i = 0; i < 5000 && slots[0] && slots[0]->prev; i++)
   {
      retro_sleep(1);
      autosave_lock();
      autosave_unlock();
   }
   CHECK(slots[0] && !slots[0]->prev, "running: the ended worker was kept");
   CHECK(joins == before + 1, "running: the ended worker was not joined");
   stop();
   gate_next = false;
   gate_ec   = NULL;
   clean();
}

/* A replacement whose worker cannot start is tried again once per
 * interval, not at every check. */
static void lane_create_retry(void)
{
   static unsigned char a[64], c[128];
   unsigned i, before;

   clean();
   memset(a, 1, sizeof(a));
   memset(c, 1, sizeof(c));
   core_data = a;
   core_size = sizeof(a);
   now_usec  = 1000000;
   slots[0]  = autosave_new(SAVE, a, sizeof(a), 1, false,
         RETRO_MEMORY_SAVE_RAM, false, NULL);
   CHECK(slots[0] != NULL, "create_retry: autosave_new failed");
   autosave_state.list = slots;
   autosave_state.num  = 1;
   wait_requested();

   core_data   = c;
   core_size   = sizeof(c);
   fail_create = true;
   before      = creates;
   for (i = 0; i < 10; i++)
   {
      autosave_lock();
      autosave_unlock();
   }
   CHECK(creates == before + 1, "create_retry: tried again at every check");
   now_usec += 999999;
   autosave_lock();
   autosave_unlock();
   CHECK(creates == before + 1, "create_retry: tried again within the interval");
   now_usec += 1;
   for (i = 0; i < 10; i++)
   {
      autosave_lock();
      autosave_unlock();
   }
   CHECK(creates == before + 2, "create_retry: not tried once per interval");

   fail_create = false;
   now_usec   += 1000000;
   autosave_lock();
   autosave_unlock();
   CHECK(creates == before + 3 && slots[0] && !autosave_quitting(slots[0]),
         "create_retry: no worker once it could start");
   wait_requested();
   stop();
   clean();
}
#endif

/* A save that is there but does not read is copied as it is on
 * disk before the first write over it; one notice at load. */
static void lane_unreadable_backed_up(void)
{
   static unsigned char cut[BIG * 2], ram[BIG];
   char newest[PATH_MAX_LENGTH];
   int64_t cut_len = 0;

   clean();
   fill(ram, BIG, 22);
   put_cut_rzip(SAVE, ram, BIG, cut, &cut_len);
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   CHECK(!event_load_save_files(false), "unreadable: a cut rzip loaded");
   CHECK(notices(MSG_SAVE_FILE_UNREADABLE) == 1, "unreadable: no notice");
   CHECK(copies("backups", SAVE_BACKUP_REASON_LAST, NULL, 0) == 0,
         "unreadable: copied at load");
#ifdef HAVE_THREADS
   start(ram, BIG, false);
   fill(ram, BIG, 23);
   capture_and_wait();
   stop();
#else
   fill(ram, BIG, 23);
   event_save_files(true, false, true, true, NULL);
#endif
   CHECK(copies("backups", SAVE_BACKUP_UNREADABLE, newest, sizeof(newest)) == 1,
         "unreadable: no unreadable copy");
   CHECK(holds(newest, cut, (size_t)cut_len),
         "unreadable: the copy is not the file's bytes");
   CHECK(holds(SAVE, ram, BIG), "unreadable: not written after the copy");
   CHECK(notices(MSG_SAVE_FILE_UNREADABLE) == 1, "unreadable: notices");
   path_deinit_savefile();
   clean();
}

/* One write of the current image: by the worker, or at close. */
static void write_once(void *ram, size_t len)
{
#ifdef HAVE_THREADS
   if (slots[0])
   {
      capture_and_wait();
      return;
   }
#endif
   (void)ram;
   (void)len;
   event_save_files(true, false, true, true, NULL);
}

/* A non-blank save written over with all 0xFF, then all 0x00: one
 * blanked copy, before the first. */
static void lane_blanked_backed_up(void)
{
   static unsigned char disk[BIG], ram[BIG], ones[BIG];
   char newest[PATH_MAX_LENGTH];

   clean();
   fill(disk, BIG, 24);
   put(SAVE, disk, BIG);
   memset(ram, 0, BIG);
   memset(ones, 0xFF, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   CHECK(event_load_save_files(false), "blanked: no load");
#ifdef HAVE_THREADS
   start(ram, BIG, false);
#endif
   memset(ram, 0xFF, BIG);
   write_once(ram, BIG);
   CHECK(copies("backups", SAVE_BACKUP_BLANKED, newest, sizeof(newest)) == 1,
         "blanked: no blanked copy");
   CHECK(holds(newest, disk, BIG), "blanked: the copy is not the old save");
   CHECK(holds(SAVE, ones, BIG), "blanked: 0xFF not written");
   memset(ram, 0x00, BIG);
   write_once(ram, BIG);
   CHECK(copies("backups", SAVE_BACKUP_REASON_LAST, NULL, 0) == 1,
         "blanked: a second copy in one session");
   CHECK(holds(SAVE, ram, BIG), "blanked: 0x00 not written");
#ifdef HAVE_THREADS
   stop();
#endif
   path_deinit_savefile();
   clean();
}

/* A copy that cannot be made (a file where its folder goes) keeps the
 * save as it was all session, with one notice and a try at each
 * check; at close the image goes to a recovery file. */
static void lane_owed_copy_fails(void)
{
   static unsigned char cut[BIG * 2], ram[BIG];
   char newest[PATH_MAX_LENGTH];
   int64_t cut_len = 0;
   const char *save = "blk/game.srm";

   clean();
   path_mkdir("blk");
   put("blk/backups", "x", 1);
   fill(ram, BIG, 25);
   put_cut_rzip(save, ram, BIG, cut, &cut_len);
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(save);
   CHECK(!event_load_save_files(false), "owed_fails: a cut rzip loaded");
#ifdef HAVE_THREADS
   {
      unsigned i;
      start_at(save, ram, BIG, false);
      for (i = 0; i < 3; i++)
      {
         ram[0] = (unsigned char)(i + 1);
         capture_and_wait();
         CHECK(retro_atomic_load_acquire_int(&save_file_get(0)->flags)
               & SAVE_FILE_BACKUP_FAILED, "owed_fails: no copy tried");
         CHECK(holds(save, cut, (size_t)cut_len),
               "owed_fails: written without its copy");
      }
      capture_and_wait();
      CHECK(notices(MSG_SAVE_BACKUP_FAILED) == 1,
            "owed_fails: not one backup notice");
      stop();
   }
#endif
   fill(ram, BIG, 26);
   event_save_files(true, false, true, true, NULL);
   CHECK(holds(save, cut, (size_t)cut_len), "owed_fails: written at close");
   CHECK(copies("cfg/recovery", SAVE_BACKUP_UNSAVED, newest,
            sizeof(newest)) == 1, "owed_fails: no recovery file");
   CHECK(holds(newest, ram, BIG), "owed_fails: the recovery is not the image");
   CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 1,
         "owed_fails: not one recovery notice");
   path_deinit_savefile();
   clean();
}

/* An autosave that keeps failing is shown once a session; each
 * close or flush write that fails is shown. No recovery file can be
 * made here (a file where its folder goes, no config folder). */
static void lane_write_failed_notice(void)
{
   static unsigned char disk[BIG], ram[BIG];
   const char *save = "blk/game.srm";
   unsigned expect  = 0;

   clean();
   save_backups_set_roots(NULL, NULL, NULL, NULL, NULL, NULL, "as_core");
   path_mkdir("blk");
   put("blk/backups", "x", 1);
   fill(disk, BIG, 27);
   put(save, disk, BIG);
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(save);
   CHECK(event_load_save_files(false), "write_failed: no load");
   use_test_vfs(true);
   set_vfs(4096, false);
#ifdef HAVE_THREADS
   {
      unsigned i;
      char line[64];
      start_at(save, ram, BIG, false);
      for (i = 0; i < 3; i++)
      {
         ram[0] = (unsigned char)(i + 1);
         capture_and_wait();
      }
      capture_and_wait();
      CHECK(notices(MSG_SAVE_WRITE_FAILED) == 1,
            "write_failed: not one autosave notice");
      stop();
      snprintf(line, sizeof(line), "msg%d %s", (int)MSG_SAVE_WRITE_FAILED,
            save);
      CHECK(logged('W', line) == 0,
            "write_failed: the autosave notice logged");
      expect = 1;
   }
#endif
   fill(ram, BIG, 28);
   event_save_files(true, false, true, true, NULL);
   event_save_files(true, false, false, false, NULL);
   CHECK(notices(MSG_SAVE_WRITE_FAILED) == expect + 2,
         "write_failed: a close or flush failure not shown");
   set_vfs(0, false);
   use_test_vfs(false);
   CHECK(holds(save, disk, BIG), "write_failed: the save changed");
   path_deinit_savefile();
   clean();
}

/* Unsaved progress newer than the save is announced once at load, also
 * with noload-save, and not after the save was written again. */
static void lane_recovery_announced(void)
{
   static unsigned char disk[BIG], ram[BIG];
   char rec[PATH_MAX_LENGTH];
   int64_t mtime = 0;

   clean();
   fill(disk, BIG, 29);
   put(SAVE, disk, BIG);
   CHECK(save_backups_recover(rec, sizeof(rec), SAVE, disk, BIG, false),
         "recovery_announced: no recovery file made");
   CHECK(path_get_mtime(SAVE, &mtime)
         && path_set_mtime(SAVE, mtime - 60),
         "recovery_announced: cannot age the save");
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   event_load_save_files(false);
   CHECK(notices(MSG_SAVE_RECOVERY_NEWER) == 1,
         "recovery_announced: not announced");
   event_load_save_files(false);
   CHECK(notices(MSG_SAVE_RECOVERY_NEWER) == 1,
         "recovery_announced: twice in one session");
   path_deinit_savefile();

   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   event_load_save_files(true);
   CHECK(notices(MSG_SAVE_RECOVERY_NEWER) == 2,
         "recovery_announced: not announced with noload-save");
   fill(ram, BIG, 30);
   event_save_files(true, false, true, true, NULL);
   path_deinit_savefile();

   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   event_load_save_files(false);
   CHECK(notices(MSG_SAVE_RECOVERY_NEWER) == 2,
         "recovery_announced: announced after the save was written");
   path_deinit_savefile();
   clean();
}

/* A recovery file used again for the same image is announced, though
 * the save was written after the file was first made. */
static void lane_recovery_reused_announced(void)
{
   static unsigned char disk[BIG], ram[BIG];
   char rec[PATH_MAX_LENGTH], again[PATH_MAX_LENGTH];
   int64_t mtime = 0;

   clean();
   fill(disk, BIG, 35);
   put(SAVE, disk, BIG);
   CHECK(   save_backups_recover(rec, sizeof(rec), SAVE, disk, BIG, false)
         && path_get_mtime(SAVE, &mtime)
         && path_set_mtime(rec, mtime - 120)
         && path_set_mtime(SAVE, mtime - 60),
         "recovery_reused: cannot age the recovery file and the save");
   CHECK(   save_backups_recover(again, sizeof(again), SAVE, disk, BIG,
               false)
         && copies("backups", SAVE_BACKUP_UNSAVED, NULL, 0) == 1,
         "recovery_reused: the recovery file not used again");
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   event_load_save_files(false);
   CHECK(notices(MSG_SAVE_RECOVERY_NEWER) == 1,
         "recovery_reused: not announced");
   path_deinit_savefile();
   clean();
}

/* A recovery that only <config>/recovery or <app data>/recovery could
 * take is announced from there. */
static void lane_recovery_elsewhere(void)
{
   static unsigned char disk[BIG], ram[BIG];
   static const char *const dirs[] = { "cfg/recovery",
      "cfg/appdata/recovery" };
   const char *save = "blk/game.srm";
   char rec[PATH_MAX_LENGTH];
   int64_t mtime = 0;
   unsigned i;

   for (i = 0; i < 2; i++)
   {
      clean();
      clear_dir(dirs[1]);
      save_backups_set_roots(NULL, NULL, NULL, NULL, i ? NULL : "cfg",
            "cfg/appdata", "as_core");
      path_mkdir("blk");
      put("blk/backups", "x", 1);
      fill(disk, BIG, 33);
      put(save, disk, BIG);
      CHECK(save_backups_recover(rec, sizeof(rec), save, disk, BIG, false)
            && copies(dirs[i], SAVE_BACKUP_UNSAVED, NULL, 0) == 1,
            "recovery_elsewhere: not made in the shared folder");
      remove("blk/backups");
      CHECK(path_get_mtime(save, &mtime)
            && path_set_mtime(save, mtime - 60),
            "recovery_elsewhere: cannot age the save");
      memset(ram, 0, BIG);
      core_data = ram;
      core_size = BIG;
      path_init_savefile_new();
      path_init_savefile_rtc(save);
      event_load_save_files(false);
      CHECK(notices(MSG_SAVE_RECOVERY_NEWER) == 1, i
            ? "recovery_elsewhere: not announced from app data"
            : "recovery_elsewhere: not announced from config");
      path_deinit_savefile();
   }
   clear_dir(dirs[1]);
   clean();
}

static int RETRO_CALLCONV test_get_mtime(const char *path, int64_t *mtime)
{
   return save_own(path) ? -1 : retro_vfs_get_mtime_impl(path, mtime);
}

/* Path calls through a backend with no mtime for the save's files. */
static void use_no_mtime(bool on)
{
   static struct retro_vfs_interface iface;
   struct retro_vfs_interface_info info;
   iface.stat         = retro_vfs_stat_impl;
   iface.mkdir        = retro_vfs_mkdir_impl;
   iface.stat_64      = retro_vfs_stat_64_impl;
   iface.set_readonly = retro_vfs_set_readonly_impl;
   iface.get_mtime    = test_get_mtime;
   iface.set_mtime    = retro_vfs_set_mtime_impl;
   info.required_interface_version = METADATA_REQUIRED_VFS_VERSION;
   info.iface                      = on ? &iface : NULL;
   path_vfs_init(&info);
}

/* A save with no mtime: any recovery with an mtime is announced, even
 * one older than the save. */
static void lane_recovery_no_mtime(void)
{
   static unsigned char disk[BIG], ram[BIG];
   char rec[PATH_MAX_LENGTH];
   int64_t mtime = 0;
   unsigned i;

   clean();
   fill(disk, BIG, 34);
   put(SAVE, disk, BIG);
   CHECK(save_backups_recover(rec, sizeof(rec), SAVE, disk, BIG, false)
         && path_get_mtime(SAVE, &mtime)
         && path_set_mtime(rec, mtime - 60),
         "no_mtime: no older recovery file made");
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = BIG;
   for (i = 0; i < 2; i++)
   {
      use_no_mtime(i == 1);
      path_init_savefile_new();
      path_init_savefile_rtc(SAVE);
      event_load_save_files(false);
      path_deinit_savefile();
      use_no_mtime(false);
      CHECK(notices(MSG_SAVE_RECOVERY_NEWER) == i, i
            ? "no_mtime: not announced"
            : "no_mtime: an older recovery announced");
   }
   clean();
}

/* --sram-mode noload-save: the save is not loaded, so it is copied
 * before the first write over it. */
static void lane_noload_save(void)
{
   static unsigned char disk[BIG], ram[BIG];
   char newest[PATH_MAX_LENGTH];

   clean();
   fill(disk, BIG, 31);
   put(SAVE, disk, BIG);
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   CHECK(!event_load_save_files(true), "noload_save: loaded");
   CHECK(notice_num == 0, "noload_save: a notice");
#ifdef HAVE_THREADS
   start(ram, BIG, false);
   fill(ram, BIG, 32);
   capture_and_wait();
   stop();
#else
   fill(ram, BIG, 32);
   event_save_files(true, false, true, true, NULL);
#endif
   CHECK(copies("backups", SAVE_BACKUP_NOT_LOADED, newest, sizeof(newest)) == 1,
         "noload_save: no not-loaded copy");
   CHECK(holds(newest, disk, BIG), "noload_save: the copy is not the old save");
   CHECK(holds(SAVE, ram, BIG), "noload_save: not written after the copy");
   path_deinit_savefile();
   clean();
}

/* A write inside a frame leaves a save that owes a copy; an explicit
 * request (not synced) makes the copy, then writes. */
static void lane_explicit_request(void)
{
   static unsigned char cut[BIG * 2], ram[BIG];
   char newest[PATH_MAX_LENGTH];
   int64_t cut_len = 0;

   clean();
   fill(ram, BIG, 33);
   put_cut_rzip(SAVE, ram, BIG, cut, &cut_len);
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   CHECK(!event_load_save_files(false), "explicit: a cut rzip loaded");
   fill(ram, BIG, 34);
   CHECK(!content_save_ram_file(0, false, false, false, false, false),
         "explicit: a frame's write went ahead");
   CHECK(holds(SAVE, cut, (size_t)cut_len), "explicit: a frame's write wrote");
   /* What CMD_EVENT_SAVE_FILES does for a NULL request */
   CHECK(content_save_ram_file(0, false, false, false, true, false),
         "explicit: the request failed");
   CHECK(copies("backups", SAVE_BACKUP_REASON_LAST, newest, sizeof(newest)) == 1,
         "explicit: not one copy");
   CHECK(holds(newest, cut, (size_t)cut_len),
         "explicit: the copy is not the file's bytes");
   CHECK(holds(SAVE, ram, BIG), "explicit: not written after the copy");
   path_deinit_savefile();
   clean();
}

/* A write shorter than the save as loaded (an old core's partial
 * load, an erase to 0 bytes) copies the save first, once a session. */
static void lane_cut_copy_old_core(void)
{
   static unsigned char disk[BIG], ram[BIG];
   char newest[PATH_MAX_LENGTH];
   unsigned i;

   clean();
   fill(disk, BIG, 35);
   put(SAVE, disk, BIG);
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = 2048;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   CHECK(event_load_save_files(false), "cut: no partial load");
#ifdef HAVE_THREADS
   start(ram, 2048, false);
#endif
   for (i = 0; i < 2; i++)
   {
      fill(ram, 2048, 36 + i);
      write_once(ram, 2048);
   }
#ifdef HAVE_THREADS
   stop();
#endif
   event_save_files(true, false, true, true, NULL);
   CHECK(copies("backups", SAVE_BACKUP_CUT, newest, sizeof(newest)) == 1,
         "cut: not one cut copy");
   CHECK(holds(newest, disk, BIG), "cut: the copy is not the old save");
   CHECK(copies("backups", SAVE_BACKUP_REASON_LAST, NULL, 0) == 1,
         "cut: another copy");
   CHECK(holds(SAVE, ram, 2048), "cut: not written after the copy");
   path_deinit_savefile();

   /* Erased to 0 bytes with the pointer kept (Genesis Plus GX) */
   clean();
   put(SAVE, disk, BIG);
   fill(ram, BIG, 38);
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   CHECK(event_load_save_files(false), "cut: no load");
#ifdef HAVE_THREADS
   start(ram, BIG, false);
   core_size = 0;
   autosave_lock();
   autosave_unlock();
   wait_requested();
   capture_and_wait();
   CHECK(holds(SAVE, ram, 0), "cut: the worker did not empty the save");
   stop();
#endif
   core_size = 0;
   event_save_files(true, false, true, true, NULL);
   CHECK(copies("backups", SAVE_BACKUP_CUT, newest, sizeof(newest)) == 1,
         "cut: not one copy before the erase");
   CHECK(holds(newest, disk, BIG), "cut: the erase's copy is not the old save");
   CHECK(holds(SAVE, ram, 0), "cut: the erase did not empty the save");
   path_deinit_savefile();
   clean();
}

/* An erase whose cut copy cannot be made at close: the save stays,
 * no recovery file, and it is shown as not saved. */
static void lane_erase_copy_fails(void)
{
   static unsigned char disk[BIG], ram[BIG];
   const char *save = "blk/game.srm";

   clean();
   path_mkdir("blk");
   put("blk/backups", "x", 1);
   fill(disk, BIG, 40);
   put(save, disk, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(save);
   CHECK(event_load_save_files(false), "erase_fails: no load");
   core_size = 0;
   event_save_files(true, false, true, true, NULL);
   CHECK(holds(save, disk, BIG), "erase_fails: the save changed");
   CHECK(copies("cfg/recovery", SAVE_BACKUP_REASON_LAST, NULL, 0) == 0,
         "erase_fails: a recovery file");
   CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 0,
         "erase_fails: a recovery notice");
   CHECK(notices(MSG_SAVE_WRITE_FAILED) == 1,
         "erase_fails: not one write-failed notice");
   CHECK(logged('W', "\"blk/game.srm\" not emptied: it could not be "
            "backed up") == 1, "erase_fails: not one not-emptied line");
   CHECK(logged('E', "Couldn't back up or keep") == 0,
         "erase_fails: logged as lost");
   path_deinit_savefile();
   clean();
}

/* A synced request whose owed copy fails has not synced the earlier
 * unsynced write: the next synced request writes it again, synced,
 * though memory matches the disk. */
static void lane_rewrite_after_failed_copy(void)
{
   static unsigned char disk[BIG], ram[BIG], image[BIG];
   const char *save = "blk/game.srm";

   clean();
   path_mkdir("blk");
   fill(disk, BIG, 41);
   put(save, disk, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(save);
   CHECK(event_load_save_files(false), "rewrite: no load");
   fill(image, BIG, 42);
   memcpy(ram, image, BIG);
   event_save_files(true, false, false, true, NULL);
   CHECK(holds(save, image, BIG), "rewrite: the unsynced request did not write");
   /* All 0x00 owes a blanked copy; it cannot be made. */
   put("blk/backups", "x", 1);
   memset(ram, 0, BIG);
   event_save_files(true, false, true, true, NULL);
   CHECK(holds(save, image, BIG), "rewrite: written without its copy");
   remove("blk/backups");
   memcpy(ram, image, BIG);
   synced_writes = 0;
   event_save_files(true, false, true, true, NULL);
   CHECK(synced_writes == 1, "rewrite: the unsynced write was not synced");
   CHECK(holds(save, image, BIG), "rewrite: the save changed");
   clear_dir("blk/backups");
   path_rmdir("blk/backups");
   path_deinit_savefile();
   clean();
}

/* An explicit request (not synced) whose owed copy fails, twice: the
 * save stays, one notice on screen and in the log, no recovery file. */
static void lane_explicit_copy_fails(void)
{
   static unsigned char cut[BIG * 2], ram[BIG];
   char line[64];
   int64_t cut_len = 0;
   const char *save = "blk/game.srm";

   clean();
   path_mkdir("blk");
   put("blk/backups", "x", 1);
   fill(ram, BIG, 43);
   put_cut_rzip(save, ram, BIG, cut, &cut_len);
   memset(ram, 0, BIG);
   core_data = ram;
   core_size = BIG;
   path_init_savefile_new();
   path_init_savefile_rtc(save);
   CHECK(!event_load_save_files(false), "explicit_fails: a cut rzip loaded");
   fill(ram, BIG, 44);
   event_save_files(true, false, false, true, NULL);
   event_save_files(true, false, false, true, NULL);
   CHECK(holds(save, cut, (size_t)cut_len),
         "explicit_fails: written without its copy");
   CHECK(notices(MSG_SAVE_BACKUP_FAILED) == 1,
         "explicit_fails: not one notice");
   snprintf(line, sizeof(line), "msg%d %s", (int)MSG_SAVE_BACKUP_FAILED,
         save);
   CHECK(logged('W', line) == 1, "explicit_fails: not logged once");
   CHECK(copies("cfg/recovery", SAVE_BACKUP_REASON_LAST, NULL, 0) == 0,
         "explicit_fails: a recovery file");
   CHECK(notices(MSG_SAVE_WRITE_RECOVERED) == 0,
         "explicit_fails: a recovery notice");
   path_deinit_savefile();
   clean();
}

/* A late core's copy is logged once with its reason; a slot whose
 * memory is never reported is logged at load only. */
static void lane_backup_logged(void)
{
   static unsigned char disk[BIG], ram[BIG];

   clean();
   fill(disk, BIG, 45);
   put(SAVE, disk, BIG);
   put("game.rtc", disk, 16);
   core_data = NULL;
   core_size = 0;
   path_init_savefile_new();
   path_init_savefile_rtc(SAVE);
   CHECK(!event_load_save_files(false), "logged: loaded without memory");
   CHECK(logged('I', "reported no memory for \"game.srm\" at load; "
            "left unread.") == 1, "logged: no load line for the save");
   CHECK(logged('I', "reported no memory for \"game.rtc\" at load; "
            "left unread.") == 1, "logged: no load line for the rtc");
   fill(ram, BIG, 46);
#ifdef HAVE_THREADS
   start(ram, BIG, false);
   capture_and_wait();
   fill(ram, BIG, 47);
   capture_and_wait();
   stop();
#else
   core_data = ram;
   core_size = BIG;
   event_save_files(true, false, true, true, NULL);
   fill(ram, BIG, 47);
#endif
   event_save_files(true, false, true, true, NULL);
   CHECK(holds(SAVE, ram, BIG), "logged: not written");
   CHECK(logged('I', "Backed up \"game.srm\" (not-loaded) before "
            "writing it.") == 1, "logged: the copy not logged once");
   CHECK(logged('I', "Backed up \"game.rtc\"") == 0,
         "logged: a copy of a slot never reported");
   CHECK(holds("game.rtc", disk, 16), "logged: the rtc changed");
   path_deinit_savefile();
   clean();
}

int main(void)
{
#ifdef HAVE_THREADS
   if (!(vfs_lock = slock_new()) || !(log_lock = slock_new()))
      return 2;
#endif
   lane_close_full_disk();
   lane_close_unchanged();
   lane_close_sync();
   lane_close_erased();
   lane_late_core();
   lane_unreadable_backed_up();
   lane_blanked_backed_up();
   lane_owed_copy_fails();
   lane_write_failed_notice();
   lane_recovery_announced();
   lane_recovery_reused_announced();
   lane_recovery_elsewhere();
   lane_recovery_no_mtime();
   lane_noload_save();
   lane_explicit_request();
   lane_cut_copy_old_core();
   lane_erase_copy_fails();
   lane_rewrite_after_failed_copy();
   lane_explicit_copy_fails();
   lane_backup_logged();
   lane_state_load_copy_sync();
   lane_flush_copy_fails_sync();
   lane_flush_write_fails_sync();
   lane_flush_same_image_sync();
#ifdef HAVE_THREADS
   retro_atomic_int_init(&wait_entries, 0);
   retro_atomic_int_init(&wait_leaves, 0);
   lane_state_load_copy_worker();
   lane_flush_with_snapshot();
   lane_flush_queue();
   lane_flush_retire_guard();
   lane_flush_at_quit();
   lane_flush_size_changed();
   lane_flush_write_fails();
   lane_flush_copy_fails();
   lane_flush_unrecoverable();
   lane_flush_fails_at_quit();
   lane_flush_reap_no_log();
   lane_flush_zero_bytes();
   lane_flush_same_image();
   lane_autosave_full_disk();
   lane_no_rewrite_at_launch();
   lane_pointer_requery();
   lane_resize_unwritten();
   lane_resize_extent();
   lane_resize_erased();
   lane_retire_bounded();
   lane_no_join_running();
   lane_create_retry();
   slock_free(vfs_lock);
   slock_free(log_lock);
#endif
   if (failures)
   {
      printf("autosave io: %d failure(s)\n", failures);
      return 1;
   }
   puts("autosave io tests passed");
   return 0;
}
