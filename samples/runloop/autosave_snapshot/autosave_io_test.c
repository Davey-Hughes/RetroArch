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

void RARCH_LOG(const char *fmt, ...) { (void)fmt; }
void RARCH_WARN(const char *fmt, ...) { (void)fmt; }
void RARCH_ERR(const char *fmt, ...) { (void)fmt; }

const char *msg_hash_to_str(enum msg_hash_enums msg)
{
   (void)msg;
   return "";
}

/* No recovery file: those are A9's. */
bool fill_pathname_application_data(char *s, size_t len)
{
   (void)s;
   (void)len;
   return false;
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

/* The test VFS: the built-in one, counting opens for writing and
 * cutting each written file at cut_at bytes, or refusing renames. */
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

static struct retro_vfs_file_handle *RETRO_CALLCONV test_open(
      const char *path, unsigned mode, unsigned hints)
{
   unsigned i;
   struct retro_vfs_file_handle *h =
      retro_vfs_file_open_impl(path, mode, hints);
   if (!h || !(mode & RETRO_VFS_FILE_ACCESS_WRITE))
      return h;
   VFS_LOCK();
   write_opens++;
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
   VFS_LOCK();
   refuse = refuse_rename;
   VFS_UNLOCK();
   return refuse ? -1 : retro_vfs_file_rename_impl(old_path, new_path);
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

static void clean(void)
{
   remove(SAVE);
   remove(WORKER_TEMP);
   remove(CLOSE_TEMP);
}

/* A full disk at close: the old file stays and no temporary is left. */
static void lane_close_full_disk(void)
{
   static unsigned char disk[BIG], ram[BIG];
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
      event_save_files(true, c != 0, true, NULL);
      set_vfs(0, false);
      use_test_vfs(false);
      CHECK(holds(SAVE, disk, BIG), "close_full_disk: the save changed");
      CHECK(!exists(CLOSE_TEMP), "close_full_disk: a temporary was left");
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
   event_save_files(true, true, true, NULL);
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
      event_save_files(true, c != 0, false, NULL);
      before = get_write_opens();
      event_save_files(true, c != 0, false, NULL);
      CHECK(get_write_opens() == before,
            "close_sync: an unsynced request rewrote matching bytes");
      synced = synced_writes;
      event_save_files(true, c != 0, true, NULL);
      CHECK(get_write_opens() > before,
            "close_sync: a synced request skipped unsynced bytes");
      CHECK(synced_writes > synced, "close_sync: the rewrite was not synced");
      before = get_write_opens();
      event_save_files(true, c != 0, true, NULL);
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
   event_save_files(true, compress, true, NULL);
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
   if (held)
      retro_atomic_store_release_int(&gate_parked, 1);
   retro_eventcount_commit_wait(ec, key);
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

static void start(void *ram, size_t len, bool compress)
{
   core_data = ram;
   core_size = len;
   slots[0]  = autosave_new(SAVE, ram, len, 0, compress,
         RETRO_MEMORY_SAVE_RAM, false);
   CHECK(slots[0] != NULL, "autosave_new failed");
   autosave_state.list = slots;
   autosave_state.num  = 1;
   wait_requested();
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
   event_save_files(true, false, true, NULL);
   CHECK(!exists(SAVE), "erased: memory with no size made a save");
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
         RETRO_MEMORY_SAVE_RAM, false);
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

int main(void)
{
#ifdef HAVE_THREADS
   if (!(vfs_lock = slock_new()))
      return 2;
#endif
   lane_close_full_disk();
   lane_close_unchanged();
   lane_close_sync();
   lane_close_erased();
#ifdef HAVE_THREADS
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
#endif
   if (failures)
   {
      printf("autosave io: %d failure(s)\n", failures);
      return 1;
   }
   puts("autosave io tests passed");
   return 0;
}
