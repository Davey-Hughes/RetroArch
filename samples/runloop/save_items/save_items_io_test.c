/* save.c's side of the save items: write-on-difference, raw unless
 * the core allows otherwise, blobs, legacy retirement, autosave from
 * segments through whole-file writes, get_generation, retirement
 * without a join, orphans, the fill of items registered after the
 * load, and ownership. save.c and save_items.c are included; save.c's
 * streams, the core's memory and the logger are stubbed with a disk in
 * memory. */
/* The checks are asserts: keep them in every build. */
#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <retro_atomic.h>
#include <retro_timers.h>
#include <features/features_cpu.h>
#include <rthreads/rthreads.h>

/* save.c's reallocs, and callocs, fail while these are set, and
 * save_items.c's mallocs of fail_malloc_size bytes, and save.c's of
 * fail_main_malloc_size on the main thread. */
static retro_atomic_int_t fail_reallocs;
static retro_atomic_int_t fail_callocs;
static retro_atomic_int_t fail_malloc_size;
static retro_atomic_int_t fail_main_malloc_size;
static uintptr_t main_thread;

static void *io_test_realloc(void *ptr, size_t len)
{
   if (retro_atomic_load_acquire_int(&fail_reallocs))
      return NULL;
   return realloc(ptr, len);
}

/* save_items.c's reallocs fail while this is set. */
static bool fail_items_reallocs;

static void *io_test_items_realloc(void *ptr, size_t len)
{
   if (fail_items_reallocs)
      return NULL;
   return realloc(ptr, len);
}

static void *io_test_calloc(size_t n, size_t len)
{
   if (retro_atomic_load_acquire_int(&fail_callocs))
      return NULL;
   return calloc(n, len);
}

static void *io_test_malloc(size_t len)
{
   int fail = retro_atomic_load_acquire_int(&fail_malloc_size);
   if (fail && (size_t)fail == len)
      return NULL;
   return malloc(len);
}

/* Called before save.c's mallocs of main_malloc_hook_size bytes on the
 * main thread. */
static void (*main_malloc_hook)(void);
static size_t main_malloc_hook_size;

static void *io_test_main_malloc(size_t len)
{
   int fail = retro_atomic_load_acquire_int(&fail_main_malloc_size);
   if (     sthread_get_current_thread_id() == main_thread
         && main_malloc_hook && len == main_malloc_hook_size)
      main_malloc_hook();
   if (     fail && (size_t)fail == len
         && sthread_get_current_thread_id() == main_thread)
      return NULL;
   return malloc(len);
}

#ifdef HAVE_THREADS
/* save.c's threads fail to start while this is set. */
static retro_atomic_int_t fail_thread_starts;

static sthread_t *io_test_sthread_create(void (*fn)(void*), void *userdata)
{
   if (retro_atomic_load_acquire_int(&fail_thread_starts))
      return NULL;
   return sthread_create(fn, userdata);
}
#endif

#define realloc io_test_realloc
#define calloc  io_test_calloc
#define malloc  io_test_main_malloc
#ifdef HAVE_THREADS
#define sthread_create io_test_sthread_create
#endif
#include "../../../save.c"
#undef realloc
#undef calloc
#undef malloc
#undef sthread_create
#define malloc  io_test_malloc
#define realloc io_test_items_realloc
#include "../../../save_items.c"
#undef malloc
#undef realloc

/* Room for a hundred safety copies of one file. */
#define DISK_FILES 128
#define DISK_SIZE  64

struct disk_file
{
   unsigned char data[DISK_SIZE];
   char path[96];
   size_t size;
   bool exists;
};

/* What a stubbed intfstream writes to. */
struct fake_stream
{
   char path[96];
};

static struct disk_file disk[DISK_FILES];
static slock_t *disk_lock;
static scond_t *disk_cond;
static unsigned raw_writes;
/* The safety copies among the writes. */
static unsigned undo_writes;
static unsigned rzip_writes;
static unsigned rzip_reads;
static unsigned worker_opens;
static unsigned worker_rzip_opens;
static unsigned worker_writes;
static unsigned recovery_writes;
/* Reads, and copies written, by the main thread; writes of /s/ files. */
static unsigned main_reads;
static unsigned main_undo_writes;
static unsigned save_writes;
/* Writes into /s/ made while copy_watch is not on disk. */
static const char *copy_watch;
static unsigned uncopied_writes;
/* The close has begun: a held copy may go on. */
static bool closing;
static unsigned memory_asks[2];
static bool hold_write;
static bool fail_writes;
/* Only the copies into /undo/ fail; or the next fail_undo_left of
 * them. */
static bool fail_undo;
static unsigned fail_undo_left;
/* A new recovery file's write stops halfway, as a kill leaves it. */
static bool cut_recovery;
/* Reads of this file fail, as an I/O error or a lock fails them. */
static const char *fail_read_path;
/* Only the writes into /s/ fail. */
static bool fail_save_writes;
/* The recovery folder; NULL for none. */
static const char *app_dir;
/* The core was called inside a registration. */
static bool in_set;
static unsigned core_calls_in_set;
#ifdef HAVE_THREADS
static bool retire_returned;
static bool retire_timed_out;
#endif
/* A worker is held after writing a temp (in /s/ too), before or after
 * a rename's move, after a move into /s/, before writing in /undo/, or
 * before writing a recovery file, while these are set; the main thread
 * never is. */
static bool hold_temp;
static bool hold_save_temp;
static bool hold_rename;
static bool hold_renamed;
static bool hold_save_renamed;
static bool hold_undo;
static bool hold_recovery;
static unsigned worker_holds;
static unsigned failed_renames;
static char renamed_from[96];
/* Paths match whatever their case, as on Windows or macOS. */
static bool disk_nocase;
static unsigned err_count;
static char err_last[256];

static unsigned char wram[8];
static unsigned char iram[4];
static unsigned char legacy_ram[4];
/* An old core's RTC, shorter than its file, while use_rtc is set. */
static unsigned char legacy_rtc[2];
static bool use_rtc;
/* Save RAM not reported. */
static bool no_ram;
/* The .srm set_items() names; NULL: none. */
static const char *set_savefile;
/* Left-out notices, and the last one's name. */
static unsigned notices;
static char notice_last[64];
static struct retro_save_segment segs[2];
static unsigned char blob_state[16];
static size_t blob_len;
static bool blob_refuse;
static unsigned loaded_calls;
static unsigned loaded_in_set;
static bool loaded_saw_file;
/* legacy_load(): offered, calls, what it was given, which names it
 * refuses, and a registration from inside it or loaded(). */
static bool use_legacy_load;
static unsigned legacy_calls;
static unsigned legacy_last_index;
static size_t legacy_last_len;
static unsigned legacy_refuse_below;
static bool legacy_reregister;
static bool loaded_reregister;
static bool blob_reregister;
static const struct retro_save_item *rereg_items;
static unsigned rereg_n;
/* The core registers rereg_items again from inside this callback, once. */
#define REREG_SIZE       1
#define REREG_SAVE       2
#define REREG_GENERATION 3
#define REREG_SNAPSHOT   4
static unsigned rereg_inside;
static unsigned rereg_refusals;
/* The file path_get_mtime() calls newer. */
static const char *newer_path;
static bool blob_fail_save;
/* blob_save() fills only this many bytes when nonzero. */
static size_t blob_short;
static unsigned blob_sizes;
static unsigned blob_saves;
static unsigned char card[6];
#ifdef HAVE_THREADS
static struct retro_save_segment card_seg;
#endif
static uint64_t generation;
static bool use_generation;
/* can_snapshot()'s answer, and its calls. */
static bool use_can_snapshot;
static bool snapshot_ready;
static unsigned snapshot_calls;
/* Log lines that say the game was still saving. */
static unsigned saving_logs;

/* The last log line that names a file in /undo/, and how many told a
 * copy kept. */
static char copy_log[256];
static unsigned copy_logs;

void RARCH_LOG(const char *fmt, ...)
{
   char s[256];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(s, sizeof(s), fmt, ap);
   va_end(ap);
   if (strstr(s, "still saving"))
      saving_logs++;
   if (strstr(s, "/undo/"))
   {
      slock_lock(disk_lock);
      strlcpy(copy_log, s, sizeof(copy_log));
      if (strstr(s, "Kept a copy"))
         copy_logs++;
      slock_unlock(disk_lock);
   }
}

/* Warnings since start(). */
static char warn_hist[4096];
void RARCH_WARN(const char *fmt, ...)
{
   char s[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(s, sizeof(s), fmt, ap);
   va_end(ap);
   if (strstr(s, "still saving"))
      saving_logs++;
   slock_lock(disk_lock);
   strlcat(warn_hist, s, sizeof(warn_hist));
   slock_unlock(disk_lock);
}

#ifdef HAVE_THREADS
/* Warnings since start() that hold @part. */
static unsigned warned(const char *part)
{
   unsigned n = 0;
   const char *at;
   slock_lock(disk_lock);
   for (at = warn_hist; (at = strstr(at, part)); at++)
      n++;
   slock_unlock(disk_lock);
   return n;
}
#endif

void RARCH_ERR(const char *fmt, ...)
{
   va_list ap;
   slock_lock(disk_lock);
   va_start(ap, fmt);
   vsnprintf(err_last, sizeof(err_last), fmt, ap);
   va_end(ap);
   err_count++;
   slock_unlock(disk_lock);
}

/* A message shows the files it names. */
const char *msg_hash_to_str(enum msg_hash_enums msg)
{
   (void)msg;
   return "%s|%s";
}

static unsigned osd_count;
static char osd_last[256];
/* Every message since start(), one per line. */
static char osd_hist[1024];

bool fill_pathname_application_data(char *s, size_t len)
{
   if (!app_dir)
      return false;
   strlcpy(s, app_dir, len);
   return true;
}

void runloop_msg_queue_push(const char *msg, size_t len, unsigned prio,
      unsigned duration, bool flush, char *title,
      enum message_queue_icon icon, enum message_queue_category category)
{
   (void)len;
   (void)prio;
   (void)duration;
   (void)flush;
   (void)title;
   (void)icon;
   (void)category;
   osd_count++;
   strlcpy(osd_last, msg, sizeof(osd_last));
   strlcat(osd_hist, msg, sizeof(osd_hist));
   strlcat(osd_hist, "\n", sizeof(osd_hist));
}

void runloop_save_items_notice(const char *name)
{
   notices++;
   strlcpy(notice_last, name ? name : "", sizeof(notice_last));
}

bool path_mkdir(const char *dir)
{
   (void)dir;
   return true;
}

bool core_get_memory(retro_ctx_memory_info_t *info)
{
   if (info->id < 2)
      memory_asks[info->id]++;
   info->data = NULL;
   info->size = 0;
   if (info->id == RETRO_MEMORY_SAVE_RAM && !no_ram)
   {
      info->data = legacy_ram;
      info->size = sizeof(legacy_ram);
   }
   else if (info->id == RETRO_MEMORY_RTC && use_rtc)
   {
      info->data = legacy_rtc;
      info->size = sizeof(legacy_rtc);
   }
   return true;
}

/* Call with disk_lock held. */
static struct disk_file *disk_find(const char *path, bool create)
{
   unsigned i;
   for (i = 0; i < DISK_FILES; i++)
      if (     disk[i].exists
            && (disk_nocase
               ? string_is_equal_noncase(disk[i].path, path)
               : !strcmp(disk[i].path, path)))
         return &disk[i];
   if (!create)
      return NULL;
   for (i = 0; i < DISK_FILES; i++)
      if (!disk[i].exists)
      {
         strlcpy(disk[i].path, path, sizeof(disk[i].path));
         disk[i].exists = true;
         disk[i].size   = 0;
         return &disk[i];
      }
   assert(!"disk full");
   return NULL;
}

static void disk_put(const char *path, const void *data, size_t len)
{
   struct disk_file *f;
   assert(len <= DISK_SIZE);
   slock_lock(disk_lock);
   f       = disk_find(path, true);
   memcpy(f->data, data, len);
   f->size = len;
   slock_unlock(disk_lock);
}

static bool disk_holds(const char *path, const void *data, size_t len)
{
   bool same;
   struct disk_file *f;
   slock_lock(disk_lock);
   f    = disk_find(path, false);
   same = f && f->size == len && !memcmp(f->data, data, len);
   slock_unlock(disk_lock);
   return same;
}

static bool disk_has(const char *path)
{
   bool has;
   slock_lock(disk_lock);
   has = disk_find(path, false) != NULL;
   slock_unlock(disk_lock);
   return has;
}

bool path_is_valid(const char *path)
{
   return disk_has(path);
}

static bool disk_read(const char *path, void **buf, int64_t *len)
{
   struct disk_file *f;
   slock_lock(disk_lock);
   if (sthread_get_current_thread_id() == main_thread)
      main_reads++;
   if (fail_read_path && !strcmp(path, fail_read_path))
      f = NULL;
   else if ((f = disk_find(path, false)))
   {
      *buf = malloc(f->size + 1);
      memcpy(*buf, f->data, f->size);
      *len = (int64_t)f->size;
   }
   slock_unlock(disk_lock);
   return f != NULL;
}

int64_t filestream_read_file(const char *path, void **buf, int64_t *len)
{
   return disk_read(path, buf, len) ? 1 : 0;
}

bool rzipstream_read_file(const char *path, void **buf, int64_t *len)
{
   rzip_reads++;
   return disk_read(path, buf, len);
}

bool filestream_matches_buf(const char *path, const void *data, size_t len)
{
   return disk_holds(path, data, len);
}

bool rzipstream_matches_buf(const char *path, const void *data, size_t len)
{
   return disk_holds(path, data, len);
}

static bool is_temp(const char *path)
{
   size_t len = strlen(path);
   return len > 4 && !strcmp(path + len - 4, ".tmp");
}

/* Call with disk_lock held. */
static void worker_hold(const bool *hold)
{
   if (!*hold || sthread_get_current_thread_id() == main_thread)
      return;
   worker_holds++;
   scond_broadcast(disk_cond);
   while (*hold)
      scond_wait(disk_cond, disk_lock);
}

/* Every write, from either thread: counted, held while hold_write is
 * set, refused while fail_writes is. Writing a name resets it, as
 * O_TRUNC does. */
static bool disk_write(const char *path, const void *data, size_t len,
      unsigned *counter)
{
   bool ok;
   struct disk_file *f;
   assert(len <= DISK_SIZE);
   slock_lock(disk_lock);
   (*counter)++;
   if (!strncmp(path, "/undo/", 6))
   {
      undo_writes++;
      if (sthread_get_current_thread_id() == main_thread)
         main_undo_writes++;
      worker_hold(&hold_undo);
   }
   else if (!strncmp(path, "/s/", 3))
   {
      save_writes++;
      if (copy_watch && !disk_find(copy_watch, false))
         uncopied_writes++;
   }
   scond_broadcast(disk_cond);
   while (hold_write)
      scond_wait(disk_cond, disk_lock);
   if (fail_undo_left && !strncmp(path, "/undo/", 6))
   {
      fail_undo_left--;
      ok = false;
   }
   else if ((ok =    !fail_writes
             && !(fail_undo && !strncmp(path, "/undo/", 6))
             && !(fail_save_writes && !strncmp(path, "/s/", 3))))
   {
      f       = disk_find(path, true);
      memcpy(f->data, data, len);
      f->size = len;
      if (is_temp(path))
      {
         worker_hold(&hold_temp);
         if (!strncmp(path, "/s/", 3))
            worker_hold(&hold_save_temp);
      }
   }
   slock_unlock(disk_lock);
   return ok;
}

/* A missing source fails, as ENOENT does. */
static int disk_rename(const char *from, const char *to)
{
   struct disk_file *src, *dst;
   slock_lock(disk_lock);
   worker_hold(&hold_rename);
   if (!(src = disk_find(from, false)))
   {
      failed_renames++;
      slock_unlock(disk_lock);
      return -1;
   }
   dst         = disk_find(to, true);
   memcpy(dst->data, src->data, src->size);
   dst->size   = src->size;
   src->exists = false;
   strlcpy(renamed_from, from, sizeof(renamed_from));
   worker_hold(&hold_renamed);
   if (!strncmp(to, "/s/", 3))
      worker_hold(&hold_save_renamed);
   slock_unlock(disk_lock);
   return 0;
}

int filestream_rename(const char *old_path, const char *new_path)
{
   return disk_rename(old_path, new_path);
}

int filestream_delete(const char *path)
{
   struct disk_file *f;
   slock_lock(disk_lock);
   if ((f = disk_find(path, false)))
      f->exists = false;
   slock_unlock(disk_lock);
   return f ? 0 : -1;
}

/* While cut_recovery is set, writes the first half of a new recovery
 * file at @path and fails. */
static bool recovery_cut(const char *path, const void *data,
      int64_t size)
{
   bool cut;
   slock_lock(disk_lock);
   cut = cut_recovery;
   slock_unlock(disk_lock);
   if (cut)
      disk_put(path, data, (size_t)size / 2);
   return cut;
}

/* Through <path>.tmp, as libretro-common's does. A new recovery file
 * counts among recovery_writes. */
bool filestream_write_file_atomic(const char *path, const void *data,
      int64_t size)
{
   char tmp[128];
   bool fresh =    app_dir && !strncmp(path, app_dir, strlen(app_dir))
                && !disk_has(path);
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   if (fresh)
   {
      slock_lock(disk_lock);
      worker_hold(&hold_recovery);
      slock_unlock(disk_lock);
   }
   if (fresh && recovery_cut(tmp, data, size))
      return false;
   if (!disk_write(tmp, data, (size_t)size,
            fresh ? &recovery_writes : &raw_writes))
      return false;
   if (disk_rename(tmp, path) == 0)
      return true;
   filestream_delete(tmp);
   return false;
}

/* A temp, or else a recovery file. */
bool filestream_write_file(const char *path, const void *data,
      int64_t size)
{
   if (is_temp(path))
      return disk_write(path, data, (size_t)size, &raw_writes);
   slock_lock(disk_lock);
   recovery_writes++;
   worker_hold(&hold_recovery);
   slock_unlock(disk_lock);
   if (recovery_cut(path, data, size))
      return false;
   disk_put(path, data, (size_t)size);
   return true;
}

bool filestream_atomic_temp_name(char *s, size_t len, const char *path)
{
   snprintf(s, len, "%s.tmp", path);
   return true;
}

bool rzipstream_write_file(const char *path, const void *data, int64_t len)
{
   return disk_write(path, data, (size_t)len, &rzip_writes);
}

bool content_finish_tmp(const char *path, bool ok)
{
   char tmp[128];
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   if (ok && disk_rename(tmp, path) == 0)
      return true;
   filestream_delete(tmp);
   return false;
}

intfstream_t *intfstream_open_file(const char *path, unsigned mode,
      unsigned hints)
{
   struct fake_stream *s = (struct fake_stream*)malloc(sizeof(*s));
   (void)mode;
   (void)hints;
   strlcpy(s->path, path, sizeof(s->path));
   slock_lock(disk_lock);
   worker_opens++;
   slock_unlock(disk_lock);
   return (intfstream_t*)s;
}

intfstream_t *intfstream_open_rzip_file(const char *path, unsigned mode)
{
   slock_lock(disk_lock);
   worker_rzip_opens++;
   slock_unlock(disk_lock);
   return intfstream_open_file(path, mode, 0);
}

/* Save RAM's worker; an item's worker never comes here. */
int64_t intfstream_write(intfstream_t *file, const void *data,
      uint64_t len)
{
   struct fake_stream *s = (struct fake_stream*)file;
   return disk_write(s->path, data, (size_t)len, &worker_writes)
      ? (int64_t)len : -1;
}

int intfstream_flush(intfstream_t *file)
{
   (void)file;
   return 0;
}

int intfstream_close(intfstream_t *file)
{
   (void)file;
   return 0;
}

static void register_items(const struct retro_save_item *items,
      unsigned n);
static bool set_items(const struct retro_save_item *items, unsigned n);

static void reregister_inside(unsigned which)
{
   if (rereg_inside != which)
      return;
   rereg_inside = 0;
   if (!set_items(rereg_items, rereg_n))
      rereg_refusals++;
}

static size_t RETRO_CALLCONV blob_size(unsigned item)
{
   (void)item;
   if (in_set)
      core_calls_in_set++;
   blob_sizes++;
   reregister_inside(REREG_SIZE);
   return blob_len;
}

static bool RETRO_CALLCONV blob_save(unsigned item, void *buf, size_t len)
{
   (void)item;
   if (in_set)
      core_calls_in_set++;
   blob_saves++;
   reregister_inside(REREG_SAVE);
   if (blob_fail_save)
      return false;
   memcpy(buf, blob_state, blob_short && blob_short < len ? blob_short : len);
   return true;
}

static bool RETRO_CALLCONV blob_load(unsigned item, const void *buf,
      size_t len)
{
   (void)item;
   if (in_set)
      core_calls_in_set++;
   if (blob_reregister)
   {
      blob_reregister = false;
      register_items(rereg_items, rereg_n);
   }
   if (blob_refuse || len > sizeof(blob_state))
      return false;
   memcpy(blob_state, buf, len);
   blob_len = len;
   return true;
}

static uint64_t RETRO_CALLCONV item_generation(unsigned item)
{
   (void)item;
   if (in_set)
      core_calls_in_set++;
   reregister_inside(REREG_GENERATION);
   return generation;
}

static bool RETRO_CALLCONV item_can_snapshot(unsigned item)
{
   (void)item;
   snapshot_calls++;
   reregister_inside(REREG_SNAPSHOT);
   return snapshot_ready;
}

/* What the core sees when told its buffer is filled. */
static void RETRO_CALLCONV item_loaded(unsigned item)
{
   (void)item;
   loaded_calls++;
   if (in_set)
      loaded_in_set++;
   loaded_saw_file = wram[0] == 0x5a && iram[3] == 0xa5;
   if (loaded_reregister)
   {
      loaded_reregister = false;
      register_items(rereg_items, rereg_n);
   }
}

/* Converts by filling wram with 0x33. */
static bool RETRO_CALLCONV item_legacy_load(unsigned item, unsigned index,
      const char *format, const void *buf, size_t len)
{
   (void)item;
   (void)format;
   (void)buf;
   legacy_calls++;
   legacy_last_index = index;
   legacy_last_len   = len;
   if (index < legacy_refuse_below)
      return false;
   memset(wram, 0x33, sizeof(wram));
   if (legacy_reregister)
   {
      legacy_reregister = false;
      register_items(rereg_items, rereg_n);
   }
   return true;
}

bool path_get_mtime(const char *path, int64_t *mtime)
{
   if (!disk_has(path))
      return false;
   *mtime = (newer_path && !strcmp(path, newer_path)) ? 2 : 1;
   return true;
}

static struct retro_save_item buffer_item(const char *id, const char *path)
{
   struct retro_save_item it;
   memset(&it, 0, sizeof(it));
   it.id            = id;
   it.path          = path;
   it.storage       = RETRO_SAVE_STORAGE_FRONTEND_BUFFER;
   it.root          = RETRO_SAVE_ROOT_ABSOLUTE;
   it.segments      = segs;
   it.num_segments  = 2;
   it.legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   return it;
}

#ifdef HAVE_THREADS
/* Its own six bytes, to tell a fill of it from one of the others. */
static struct retro_save_item card_item(const char *id, const char *path)
{
   struct retro_save_item it = buffer_item(id, path);
   it.segments               = &card_seg;
   it.num_segments           = 1;
   return it;
}

static struct retro_save_item blob_item(const char *id, const char *path)
{
   struct retro_save_item it;
   memset(&it, 0, sizeof(it));
   it.id            = id;
   it.path          = path;
   it.storage       = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   it.root          = RETRO_SAVE_ROOT_ABSOLUTE;
   it.legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   return it;
}
#endif

/* As runloop.c's SET_SAVE_INTERFACE case does it; false when refused
 * before anything changed. */
static bool set_items(const struct retro_save_item *items, unsigned n)
{
   struct retro_save_interface iface;
   struct retro_save_frontend_interface fi;
   struct save_items_paths paths;
   if (!save_items_can_register())
      return false;
   memset(&iface, 0, sizeof(iface));
   memset(&paths, 0, sizeof(paths));
   paths.savefile          = set_savefile;
   iface.interface_version = 1;
   iface.num_items         = n;
   iface.item_size         = sizeof(struct retro_save_item);
   iface.items             = items;
   iface.blob_size         = blob_size;
   iface.blob_save         = blob_save;
   iface.blob_load         = blob_load;
   iface.get_generation    = use_generation ? item_generation : NULL;
   iface.can_snapshot      = use_can_snapshot ? item_can_snapshot : NULL;
   iface.loaded            = item_loaded;
   iface.legacy_load       = use_legacy_load ? item_legacy_load : NULL;
   memset(&fi, 0, sizeof(fi));
   fi.interface_version    = 1;
   fi.features             = RETRO_SAVE_FEATURE_LEGACY_LOAD;
   assert(save_items_get_frontend_interface(&fi,
            RETRO_SAVE_INSTANCE_PRIMARY));
   in_set                  = true;
   assert(save_items_register(&iface, &paths));
   event_save_items_registered();
   save_items_drop_previous();
   in_set                  = false;
   return true;
}

static void register_items(const struct retro_save_item *items, unsigned n)
{
   assert(set_items(items, n));
}

#ifdef HAVE_THREADS
/* A refused registration, as runloop.c handles it. */
static void register_refused(const struct retro_save_item *items,
      unsigned n)
{
   struct retro_save_interface iface;
   struct save_items_paths paths;
   memset(&iface, 0, sizeof(iface));
   memset(&paths, 0, sizeof(paths));
   iface.interface_version = 1;
   iface.num_items         = n;
   iface.item_size         = sizeof(struct retro_save_item);
   iface.items             = items;
   assert(!save_items_register(&iface, &paths));
   if (save_items_state_get_ptr()->dropped)
      event_save_items_registered();
   save_items_drop_previous();
}
#endif

/* wram then iram, as the file holds them. */
static const unsigned char *image(void)
{
   static unsigned char buf[sizeof(wram) + sizeof(iram)];
   memcpy(buf, wram, sizeof(wram));
   memcpy(buf + sizeof(wram), iram, sizeof(iram));
   return buf;
}

static void start(void)
{
   event_save_items_deinit();
   save_items_clear();
   path_deinit_savefile();
   path_init_savefile_new();
   slock_lock(disk_lock);
   memset(disk, 0, sizeof(disk));
   raw_writes = undo_writes = rzip_writes = rzip_reads = 0;
   worker_opens = worker_rzip_opens = worker_writes = 0;
   recovery_writes = 0;
   main_reads = main_undo_writes = save_writes = 0;
   copy_watch      = NULL;
   uncopied_writes = 0;
   closing      = false;
   fail_writes  = false;
   fail_undo    = false;
   fail_undo_left = 0;
   cut_recovery = false;
   fail_save_writes = false;
   fail_read_path   = NULL;
   hold_temp    = false;
   hold_rename  = false;
   hold_renamed = false;
   hold_save_renamed = false;
   hold_undo    = false;
   hold_save_temp = false;
   hold_recovery  = false;
   worker_holds = failed_renames = 0;
   renamed_from[0] = '\0';
   disk_nocase  = false;
   err_count    = 0;
   err_last[0]  = '\0';
   warn_hist[0] = '\0';
   copy_log[0]  = '\0';
   copy_logs    = 0;
   slock_unlock(disk_lock);
   memset(memory_asks, 0, sizeof(memory_asks));
   memset(wram, 0, sizeof(wram));
   memset(iram, 0, sizeof(iram));
   memset(legacy_ram, 0, sizeof(legacy_ram));
   memset(legacy_rtc, 0, sizeof(legacy_rtc));
   use_rtc        = false;
   no_ram         = false;
   set_savefile   = NULL;
   notices        = 0;
   notice_last[0] = '\0';
   memset(card, 0, sizeof(card));
   blob_len       = 0;
   blob_refuse    = false;
   blob_fail_save = false;
   blob_short     = 0;
   blob_sizes     = 0;
   blob_saves     = 0;
   generation          = 0;
   use_generation      = false;
   use_can_snapshot    = false;
   snapshot_ready      = true;
   snapshot_calls      = 0;
   saving_logs         = 0;
   loaded_calls        = 0;
   loaded_in_set       = 0;
   loaded_saw_file     = false;
   use_legacy_load     = false;
   legacy_calls        = 0;
   legacy_refuse_below = 0;
   legacy_reregister   = false;
   loaded_reregister   = false;
   blob_reregister     = false;
   rereg_inside        = 0;
   rereg_refusals      = 0;
   newer_path          = NULL;
   app_dir        = NULL;
   osd_count      = 0;
   osd_last[0]    = '\0';
   osd_hist[0]    = '\0';
   core_calls_in_set = 0;
   event_save_items_set_undo_dir("/undo");
}

static unsigned files_on_disk(void)
{
   unsigned i;
   unsigned n = 0;
   slock_lock(disk_lock);
   for (i = 0; i < DISK_FILES; i++)
      if (disk[i].exists)
         n++;
   slock_unlock(disk_lock);
   return n;
}

/* A core stops registering the item in save RAM's place: the item's
 * last image lands, then save RAM is written at each save, as before
 * the interface. */
static void test_given_back_saved(void)
{
   static const unsigned char older[4] = { 1, 0, 0, 0 };
   struct retro_save_segment seg;
   struct retro_save_item items[1];
   unsigned k;

   start();
   app_dir = "/app";
   path_init_savefile_rtc("/s/game.srm");
   seg.data               = iram;
   seg.size               = sizeof(iram);
   items[0]               = buffer_item("srm", "/s/game.srm");
   items[0].segments      = &seg;
   items[0].num_segments  = 1;
   items[0].legacy_memory = RETRO_MEMORY_SAVE_RAM;
   register_items(items, 1);
   event_load_save_files(false);
   iram[0] = 1;
   event_save_files(true, false, NULL);
   assert(disk_holds("/s/game.srm", older, sizeof(older)));
   iram[0] = 2;
   register_items(NULL, 0);
   assert(save_item_files.num_orphans == 1);
   /* Killed here. */
   assert(files_on_disk() == 1);
   assert(disk_holds("/s/game.srm", older, sizeof(older)));
   for (k = 0; k < 3; k++)
   {
      memset(legacy_ram, 9 + k, sizeof(legacy_ram));
      event_save_files(true, false, NULL);
      assert(save_item_files.num_orphans == 0);
      assert(disk_holds("/s/game.srm", legacy_ram, sizeof(legacy_ram)));
      assert(files_on_disk() == 1);
   }
   assert(osd_count == 0 && err_count == 0);
}

#ifdef HAVE_THREADS
static void copies_wait(bool reap);
#endif

/* The same with an item longer than save RAM: its last image is copied
 * once, before save RAM's first write cuts the file. */
static void test_given_back_shorter(void)
{
   unsigned char older[12];
   unsigned char last[12];
   struct retro_save_item items[1];

   start();
   app_dir = "/app";
   path_init_savefile_rtc("/s/game.srm");
   items[0]               = buffer_item("srm", "/s/game.srm");
   items[0].legacy_memory = RETRO_MEMORY_SAVE_RAM;
   register_items(items, 1);
   event_load_save_files(false);
   wram[0] = 1;
   event_save_files(true, false, NULL);
   memcpy(older, image(), sizeof(older));
   assert(disk_holds("/s/game.srm", older, sizeof(older)));
   wram[0] = 2;
   memcpy(last, image(), sizeof(last));
   register_items(NULL, 0);
#ifdef HAVE_THREADS
   copies_wait(false);
#endif
   /* Killed here. */
   assert(files_on_disk() == 2);
   assert(disk_holds("/s/game.srm", older, sizeof(older)));
   assert(disk_holds("/undo/game.srm", last, sizeof(last)));
#ifdef HAVE_THREADS
   copies_wait(true);
#endif
   memset(legacy_ram, 9, sizeof(legacy_ram));
   event_save_files(true, false, NULL);
   assert(disk_holds("/s/game.srm", legacy_ram, sizeof(legacy_ram)));
   assert(disk_holds("/undo/game.srm", last, sizeof(last)));
   assert(files_on_disk() == 2);
   legacy_ram[0] = 10;
   event_save_files(true, false, NULL);
   assert(disk_holds("/s/game.srm", legacy_ram, sizeof(legacy_ram)));
   assert(files_on_disk() == 2);
   assert(copy_logs == 1 && strstr(copy_log, "/undo/game.srm"));
   assert(osd_count == 0 && err_count == 0);
}

#ifdef HAVE_THREADS
static void retired_join(bool jobs);
static autosave_t *start_autosave(bool compress);
static void capture(autosave_t *h);
static void wait_requested(autosave_t *h);
#endif

/* The same with save RAM reporting no size as the item leaves: the file
 * is copied once, when save RAM reports a size again, before its first
 * write cuts the file. Pass 1: save RAM's own autosave writes first. */
static void test_given_back_unsized(void)
{
   unsigned char last[12];
   struct retro_save_item items[1];
   unsigned pass;

#ifdef HAVE_THREADS
   for (pass = 0; pass < 2; pass++)
#else
   for (pass = 0; pass < 1; pass++)
#endif
   {
#ifdef HAVE_THREADS
      autosave_t *h = NULL;
#endif
      start();
      app_dir = "/app";
      path_init_savefile_rtc("/s/game.srm");
      items[0]               = buffer_item("srm", "/s/game.srm");
      items[0].legacy_memory = RETRO_MEMORY_SAVE_RAM;
      register_items(items, 1);
      event_load_save_files(false);
#ifdef HAVE_THREADS
      if (pass)
         h = start_autosave(false);
#endif
      wram[0] = 1;
      event_save_files(true, false, NULL);
      wram[0] = 2;
      memcpy(last, image(), sizeof(last));
      no_ram = true;
      register_items(NULL, 0);
#ifdef HAVE_THREADS
      if (pass)
         retired_join(false);
      else
#endif
         event_save_files(true, false, NULL);
      assert(disk_holds("/s/game.srm", last, sizeof(last)));
      assert(files_on_disk() == 1);
      no_ram     = false;
      copy_watch = "/undo/game.srm";
      memset(legacy_ram, 9, sizeof(legacy_ram));
#ifdef HAVE_THREADS
      if (pass)
         autosave_check();
      else
         event_save_files(true, false, NULL);
      copies_wait(false);
      /* Killed here. */
      assert(!pass || !autosave_state.list[0]);
      assert(disk_holds("/s/game.srm", last, sizeof(last)));
      assert(disk_holds("/undo/game.srm", last, sizeof(last)));
      assert(files_on_disk() == 2);
      copies_wait(true);
      if (pass)
      {
         autosave_check();
         assert((h = autosave_state.list[0]));
         h->interval = 0;
         wait_requested(h);
         capture(h);
      }
      else
#endif
         event_save_files(true, false, NULL);
      assert(uncopied_writes == 0);
      assert(disk_holds("/s/game.srm", legacy_ram, sizeof(legacy_ram)));
      assert(disk_holds("/undo/game.srm", last, sizeof(last)));
      assert(files_on_disk() == 2);
      legacy_ram[0] = 10;
      event_save_files(true, false, NULL);
      assert(disk_holds("/s/game.srm", legacy_ram, sizeof(legacy_ram)));
      assert(files_on_disk() == 2);
      assert(copy_logs == 1 && strstr(copy_log, "/undo/game.srm"));
      assert(osd_count == 0 && err_count == 0);
#ifdef HAVE_THREADS
      autosave_deinit();
#endif
   }
}

#ifdef HAVE_THREADS
/* Joins the retired handles' threads, copy jobs only when @jobs: they
 * have written their last. */
static void retired_join(bool jobs)
{
   unsigned i;
   for (i = 0; i < autosave_state.num_retired; i++)
   {
      autosave_t *h = autosave_state.retired[i];
      if (jobs && !h->job)
         continue;
      sthread_join(h->thread);
      h->thread = NULL;
   }
}

/* Until every copy job has written its last; with @reap, freed as the
 * frame start does. */
static void copies_wait(bool reap)
{
   retired_join(true);
   if (reap)
      autosave_reap(true);
}

/* SAVE_FILES once the copies started before it are done, and until an
 * image handed to one is written, as a synchronous copy would. */
static bool save_files(void)
{
   bool ret;
   copies_wait(false);
   ret = event_save_files(true, false, NULL);
   copies_wait(true);
   return ret;
}

/* save_files() up to the kill point: the copies it started are done,
 * and none is reaped until frame_start_reap(). */
static bool save_files_unreaped(void)
{
   bool ret;
   copies_wait(false);
   ret = event_save_files(true, false, NULL);
   copies_wait(false);
   return ret;
}

static void frame_start_reap(void)
{
   autosave_reap(true);
}

/* The load with no copy job started: a worker started next makes the
 * copy the fill found owed. */
static void load_for_worker(void)
{
   event_save_items_set_undo_dir(NULL);
   event_load_save_files(false);
   event_save_items_set_undo_dir("/undo");
}

static void wait_requested(autosave_t *h)
{
   unsigned i;
   for (i = 0; i < 5000; i++)
   {
      if (retro_atomic_load_acquire_int(&h->snapshot_requested))
         return;
      retro_sleep(1);
   }
   assert(!"snapshot request timed out");
}

/* One frame's publish, then the worker's next request. */
static void capture(autosave_t *h)
{
   autosave_lock();
   autosave_unlock();
   wait_requested(h);
}

static autosave_t *start_autosave(bool compress)
{
   autosave_t *h;
   /* A worker starts once a copy job of its file is reaped. */
   copies_wait(true);
   assert(autosave_init(compress, 1));
   assert((h = save_item_files.files[0].autosave));
   /* No rest between rounds. Set before the first snapshot, whose
    * publish orders it before the worker reads it. */
   h->interval = 0;
   wait_requested(h);
   return h;
}

static unsigned disk_count_named(const char *part);
static unsigned disk_named_holds(const char *part, const void *data,
      size_t len);
static void set_fail_save_writes(bool fail);

/* @undo files in /undo/, @recovered recovery files, and @told
 * messages since start(). */
#define ASSERT_FILES(undo, recovered, told) \
   assert(   disk_count_named("/undo/") == (undo) \
          && disk_count_named("RetroArch-recovery-") == (recovered) \
          && osd_count == (told))

static void test_write_on_difference(void)
{
   struct retro_save_item items[1];
   start();
   app_dir  = "/app";
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   assert(!event_load_save_files(false));

   /* Untouched, with no file: nothing to write. */
   assert(save_files());
   assert(!disk_has("/s/game.srm") && raw_writes == 0);

   wram[0] = 1;
   iram[3] = 2;
   assert(save_files());
   assert(raw_writes == 1 && disk_holds("/s/game.srm", image(), 12));

   /* The file holds it already. */
   assert(save_files());
   assert(raw_writes == 1);

   /* An erase back to the bytes it started with still reaches disk. */
   wram[0] = 0;
   iram[3] = 0;
   assert(save_files());
   assert(raw_writes == 2 && disk_holds("/s/game.srm", image(), 12));
   ASSERT_FILES(0, 0, 0);
}

static void test_raw_by_default(void)
{
   static const unsigned char zeros[12] = {0};
   struct retro_save_item items[1];
   start();
   app_dir  = "/app";
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   event_load_save_files(false);
   wram[1] = 9;
   assert(event_save_files(true, true, NULL));
   assert(raw_writes == 1 && rzip_writes == 0);
   assert(!strcmp(renamed_from, "/s/game.srm.tmp"));
   ASSERT_FILES(0, 0, 0);

   start();
   items[0].flags = RETRO_SAVE_FLAG_FRONTEND_MAY_TRANSFORM;
   disk_put("/s/game.srm", zeros, sizeof(zeros));
   register_items(items, 1);
   assert(event_load_save_files(false) && rzip_reads == 1);
   wram[1] = 9;
   assert(event_save_files(true, true, NULL));
   assert(raw_writes == 0 && rzip_writes == 1);
   assert(disk_holds("/s/game.srm", image(), 12));
   assert(!strcmp(renamed_from, "/s/game.srm.tmp"));
   assert(!disk_has("/s/game.srm.tmp"));
   ASSERT_FILES(0, 0, 0);
}

static void test_load_prefix_and_cut(void)
{
   static const unsigned char shorter[5] = {9, 9, 9, 9, 9};
   unsigned char longer[20];
   struct retro_save_item items[1];
   unsigned i;

   start();
   memset(wram, 0x11, sizeof(wram));
   memset(iram, 0x22, sizeof(iram));
   disk_put("/s/game.srm", shorter, sizeof(shorter));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   assert(event_load_save_files(false));
   for (i = 0; i < 5; i++)
      assert(wram[i] == 9);
   assert(wram[5] == 0x11 && wram[7] == 0x11 && iram[0] == 0x22);

   start();
   for (i = 0; i < sizeof(longer); i++)
      longer[i] = (unsigned char)(i + 1);
   disk_put("/s/game.srm", longer, sizeof(longer));
   register_items(items, 1);
   assert(event_load_save_files(false));
   assert(!memcmp(image(), longer, 12));

   /* Once per load: a later LOAD_FILES leaves the items alone. */
   wram[0] = 0x77;
   event_load_save_files(false);
   assert(wram[0] == 0x77);

   /* Loading disabled: the item is taken up as it is. */
   start();
   disk_put("/s/game.srm", longer, sizeof(longer));
   register_items(items, 1);
   assert(!event_load_save_files(true));
   assert(wram[0] == 0);
   assert(save_item_files.files[0].taken_up);
}

static void test_blob(void)
{
   struct retro_save_item items[1];
   unsigned sizes;
   start();
   memset(&items[0], 0, sizeof(items[0]));
   items[0].id            = "flash";
   items[0].path          = "/s/game.flash";
   items[0].storage       = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   items[0].root          = RETRO_SAVE_ROOT_ABSOLUTE;
   items[0].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   memcpy(blob_state, "abcdef", 6);
   blob_len = 6;
   register_items(items, 1);
   assert(!event_load_save_files(false));
   assert(save_files());
   assert(!disk_has("/s/game.flash"));
   blob_state[0] = 'z';
   assert(save_files());
   assert(disk_holds("/s/game.flash", "zbcdef", 6));

   start();
   disk_put("/s/game.flash", "LOADED!", 7);
   register_items(items, 1);
   assert(event_load_save_files(false));
   assert(blob_len == 7 && !memcmp(blob_state, "LOADED!", 7));

   /* A refusal leaves the core's bytes as they were. */
   start();
   memcpy(blob_state, "keep", 4);
   blob_len    = 4;
   blob_refuse = true;
   disk_put("/s/game.flash", "other", 5);
   register_items(items, 1);
   assert(!event_load_save_files(false));
   assert(blob_len == 4 && !memcmp(blob_state, "keep", 4));

   /* Over max_size: never written, logged each time and shown at the
    * second failure in a row. */
   start();
   app_dir           = "/app";
   items[0].max_size = 4;
   memcpy(blob_state, "toolong", 7);
   blob_len = 7;
   register_items(items, 1);
   event_load_save_files(false);
   blob_state[0] = 'T';
   sizes         = blob_sizes;
   assert(!save_files());
   assert(blob_sizes == sizes + 1);
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 1);
   assert(!save_files());
   assert(!save_files());
   assert(!disk_has("/s/game.flash"));
   ASSERT_FILES(0, 0, 1);
   assert(err_count == 3);
}

/* blob_load() registers again and refuses: the refusal still names
 * the file. */
static void test_blob_reregister_refused(void)
{
   struct retro_save_item items[1];
   start();
   memset(&items[0], 0, sizeof(items[0]));
   items[0].id            = "flash";
   items[0].path          = "/s/game.flash";
   items[0].storage       = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   items[0].root          = RETRO_SAVE_ROOT_ABSOLUTE;
   items[0].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   disk_put("/s/game.flash", "other", 5);
   blob_refuse     = true;
   blob_reregister = true;
   rereg_items     = items;
   rereg_n         = 1;
   register_items(items, 1);
   assert(!event_load_save_files(false));
   assert(osd_count == 1 && !strcmp(osd_last, "/s/game.flash|"));
}

static void test_legacy_retired(void)
{
   static const unsigned char on_disk[12] = {1, 2, 3, 4, 5, 6, 7, 8,
      9, 10, 11, 12};
   struct retro_save_item items[1];
   start();
   path_init_savefile_rtc("/s/game.srm");
   disk_put("/s/game.srm", on_disk, sizeof(on_disk));
   items[0]               = buffer_item("srm", "/s/game.srm");
   items[0].legacy_memory = RETRO_MEMORY_SAVE_RAM;
   register_items(items, 1);

   /* The item reads the file; save RAM's own path is not asked, only
    * its size, against the item's. */
   assert(event_load_save_files(false));
   assert(!memcmp(image(), on_disk, 12));
   assert(legacy_ram[0] == 0);
   assert(memory_asks[RETRO_MEMORY_SAVE_RAM] == 1);
   assert(memory_asks[RETRO_MEMORY_RTC] == 1);

   /* RTC has no memory here, so save RAM's half reports false. */
   wram[0] = 0x42;
   save_files();
   assert(memory_asks[RETRO_MEMORY_SAVE_RAM] == 1);
   assert(memory_asks[RETRO_MEMORY_RTC] == 2);
   assert(disk_holds("/s/game.srm", image(), 12));

   assert(autosave_init(false, 1));
   assert(!autosave_state.list[0] && !autosave_state.list[1]);
   assert(save_item_files.files[0].autosave);
   assert(memory_asks[RETRO_MEMORY_SAVE_RAM] == 1);
   autosave_deinit();
   assert(!save_item_files.files[0].autosave);
}

/* Save RAM the core reports only after it registered: an item at the
 * .srm, or a folder holding it, is left out at the first fill, before
 * it is read, told or written, and the frontend's own .srm handling
 * keeps the file. */
static void test_reported_at_fill(void)
{
   static const unsigned char on_disk[4] = { 7, 7, 7, 7 };
   static const unsigned char later[4]   = { 9, 7, 7, 7 };
   static const char *const srm[]        = { "*.srm", NULL };
   static const char *const want[]       = {
      "Core: save item 0 \"srm\" (\"Battery\"): legacy_memory = "
      "RETRO_SAVE_NO_LEGACY_MEMORY: the file \"/s/game.srm\" is where the "
      "frontend writes RETRO_MEMORY_SAVE_RAM, which the core reports and "
      "no item replaces, and a file has one writer. See libretro.h, "
      "SET_SAVE_INTERFACE, \"Ownership\". Item left out; it is not saved.",
      "Core: save item 0 \"cards\" (\"Battery\"): path = \"/s\": the "
      "folder holds "
      "\"/s/game.srm\", where the frontend writes RETRO_MEMORY_SAVE_RAM, "
      "which the core reports and no item replaces, and a file has one "
      "writer; exclude it, or name that memory in legacy_memory. See "
      "libretro.h, SET_SAVE_INTERFACE, \"Ownership\". Item left out; the "
      "frontend makes no copies or backups of it."
   };
   const save_items_state_t *st = save_items_state_get_ptr();
   struct retro_save_frontend_interface fi;
   struct retro_save_item items[2];
   unsigned i;

   memset(&fi, 0, sizeof(fi));
   fi.interface_version = 1;
   for (i = 0; i < 3; i++)
   {
      start();
      path_init_savefile_rtc("/s/game.srm");
      disk_put("/s/game.srm", on_disk, sizeof(on_disk));
      set_savefile = "/s/game.srm";
      items[0]       = buffer_item("srm", "/s/game.srm");
      items[0].label = "Battery";
      if (i)
      {
         items[0].id           = "cards";
         items[0].path         = "/s";
         items[0].storage      = RETRO_SAVE_STORAGE_CORE_FOLDER;
         items[0].segments     = NULL;
         items[0].num_segments = 0;
         items[0].include      = srm;
      }
      /* The third registers the folder once the game runs. */
      items[1]       = buffer_item("eep", "/s/game.eep");
      items[1].label = "EEPROM";
      if (i == 2)
      {
         register_items(&items[1], 1);
         assert(event_load_save_files(false));
         assert(loaded_calls == 1);
      }
      register_items(items, i == 2 ? 2 : 1);
      assert(st->num_issues == 0 && st->current->items[0].path);
      if (i == 2)
         event_save_items_fill(false);
      else
         assert(event_load_save_files(false));
      assert(st->num_issues == 1 && st->issues[0].item == 0);
      assert(st->issues[0].verdict == RETRO_SAVE_VERDICT_LEFT_OUT);
      assert(st->issues[0].field == (i ? RETRO_SAVE_FIELD_PATH
               : RETRO_SAVE_FIELD_LEGACY_MEMORY));
      assert(!strcmp(st->issues[0].text, want[i ? 1 : 0]));
      assert(err_count == 1 && !strncmp(err_last, "[SRAM] ", 7)
            && !strncmp(err_last + 7, want[i ? 1 : 0],
               sizeof(err_last) - 8));
      /* Told only of the item the frontend would write. */
      assert(notices == (i ? 0u : 1u));
      assert(!strcmp(notice_last, i ? "" : "Battery"));
      assert(save_items_get_frontend_interface(&fi,
               RETRO_SAVE_INSTANCE_PRIMARY));
      assert(!fi.get_path(0, NULL, 0) && !st->current->items[0].path);
      assert(loaded_calls == (i == 2 ? 1u : 0u));
      /* Killed here: the .srm as it was, loaded into save RAM; once
       * made, the copy of what it held at the load. A folder registered
       * after the load gives back no file read since. */
#ifdef HAVE_THREADS
      copies_wait(true);
#endif
      assert(disk_count_named("") == (i == 2 ? 1u : 2u));
      assert(disk_holds("/s/game.srm", on_disk, sizeof(on_disk)));
      assert(i == 2 || disk_holds("/undo/game.srm", on_disk,
               sizeof(on_disk)));
      assert(copy_logs == (i == 2 ? 0u : 1u));
      assert(!memcmp(legacy_ram, on_disk, sizeof(on_disk)));
      /* Written as without the item (RTC, not reported, fails). */
      legacy_ram[0] = 9;
      event_save_files(true, false, NULL);
      assert(disk_count_named("") == (i == 2 ? 1u : 2u));
      assert(disk_holds("/s/game.srm", later, sizeof(later)));
      assert(err_count == 1 && notices == (i ? 0u : 1u));
   }

   /* Still not reported at the fill: kept, and not checked again. */
   start();
   path_init_savefile_rtc("/s/game.srm");
   set_savefile = "/s/game.srm";
   no_ram       = true;
   items[0]       = buffer_item("srm", "/s/game.srm");
   items[0].label = "Battery";
   register_items(items, 1);
   event_load_save_files(false);
   no_ram       = false;
   event_save_items_fill(false);
   assert(st->num_issues == 0 && err_count == 0 && notices == 0);
   assert(st->current->items[0].path && loaded_calls == 1);
}

#ifdef HAVE_THREADS
static void set_hold(bool *hold, bool on);
static void wait_held(unsigned n);
#endif

/* An earlier session kept the item at the .srm; left out at its first
 * fill now, the file goes back to save RAM's own handling, which reads
 * it as save RAM: the bytes read are copied first, off the main thread.
 * Pass 1 is killed while that copy is held; pass 2 is a first session,
 * with no file. */
static void test_left_late_copied(void)
{
   static const unsigned char srm[4]   = { 5, 6, 7, 8 };
   static const unsigned char later[4] = { 1, 6, 7, 8 };
   const save_items_state_t *st = save_items_state_get_ptr();
   struct retro_save_segment seg;
   struct retro_save_item items[1];
   unsigned pass;

   seg.data              = iram;
   seg.size              = sizeof(iram);
   items[0]              = buffer_item("srm", "/s/game.srm");
   items[0].label        = "Battery";
   items[0].segments     = &seg;
   items[0].num_segments = 1;
   for (pass = 0; pass < 3; pass++)
   {
      if (pass < 2)
      {
         start();
         path_init_savefile_rtc("/s/game.srm");
         set_savefile = "/s/game.srm";
         no_ram       = true;
         register_items(items, 1);
         event_load_save_files(false);
         assert(st->num_issues == 0 && st->current->items[0].path);
         memcpy(iram, srm, sizeof(srm));
         save_files();
         assert(disk_count_named("") == 1);
         assert(disk_holds("/s/game.srm", srm, sizeof(srm)));
      }

      start();
      path_init_savefile_rtc("/s/game.srm");
      set_savefile = "/s/game.srm";
      if (pass < 2)
         disk_put("/s/game.srm", srm, sizeof(srm));
#ifdef HAVE_THREADS
      if (pass == 1)
         set_hold(&hold_undo, true);
#endif
      no_ram = true;
      register_items(items, 1);
      no_ram = false;
      event_load_save_files(false);
      assert(st->num_issues == 1 && !st->current->items[0].path);
      assert(err_count == 1 && notices == 1);
#ifdef HAVE_THREADS
      if (pass == 1)
      {
         wait_held(1);
         /* Killed here: the .srm as the item wrote it, alone. */
         assert(disk_count_named("") == 1);
         assert(disk_holds("/s/game.srm", srm, sizeof(srm)));
         ASSERT_FILES(0, 0, 0);
         set_hold(&hold_undo, false);
      }
      copies_wait(true);
      assert(main_undo_writes == 0);
#endif
      if (pass == 2)
      {
         assert(disk_count_named("") == 0 && copy_logs == 0);
         legacy_ram[0] = 1;
         save_files();
         assert(disk_count_named("") == 1);
         assert(disk_holds("/s/game.srm", legacy_ram, sizeof(legacy_ram)));
         ASSERT_FILES(0, 0, 0);
         assert(err_count == 1 && notices == 1 && copy_logs == 0);
         continue;
      }
      assert(!memcmp(legacy_ram, srm, sizeof(srm)));
      assert(disk_count_named("") == 2);
      assert(disk_holds("/s/game.srm", srm, sizeof(srm)));
      assert(disk_holds("/undo/game.srm", srm, sizeof(srm)));
      assert(copy_logs == 1 && strstr(copy_log, "/undo/game.srm"));
      ASSERT_FILES(1, 0, 0);
      /* Save RAM's own writer saves it as before the interface. */
      legacy_ram[0] = 1;
      save_files();
      assert(disk_count_named("") == 2);
      assert(disk_holds("/s/game.srm", later, sizeof(later)));
      assert(disk_holds("/undo/game.srm", srm, sizeof(srm)));
      ASSERT_FILES(1, 0, 0);
      assert(err_count == 1 && notices == 1 && copy_logs == 1);
   }
}

/* An item a later call leaves out, valid at its file again while the
 * last image of the item that left still waits: the fill takes that
 * image, and only that image is written, once. */
static void test_left_out_back(void)
{
   const save_items_state_t *st = save_items_state_get_ptr();
   struct retro_save_item items[1];
   unsigned char older[12];
   unsigned char newest[12];
   start();
   app_dir        = "/app";
   items[0]       = buffer_item("srm", "/s/game.srm");
   items[0].label = "Battery";
   register_items(items, 1);
   event_load_save_files(false);
   wram[0] = 1;
   event_save_files(true, false, NULL);
   memcpy(older, image(), sizeof(older));
   assert(disk_holds("/s/game.srm", older, sizeof(older)));
   wram[0] = 2;
   memcpy(newest, image(), sizeof(newest));
   items[0].path = NULL;
   register_items(items, 1);
   assert(st->num_issues == 1);
   assert(st->issues[0].verdict == RETRO_SAVE_VERDICT_LEFT_OUT);
   assert(save_item_files.num_orphans == 1);
   wram[0]       = 3;
   items[0].path = "/s/game.srm";
   register_items(items, 1);
   assert(st->num_issues == 0);
   event_save_items_fill(false);
   assert(!memcmp(image(), newest, sizeof(newest)));
   /* Killed here: the file as last written, alone. */
   assert(disk_count_named("") == 1);
   assert(disk_holds("/s/game.srm", older, sizeof(older)));
   ASSERT_FILES(0, 0, 0);
   event_save_files(true, false, NULL);
   assert(save_item_files.num_orphans == 0);
   assert(disk_count_named("") == 1);
   assert(disk_holds("/s/game.srm", newest, sizeof(newest)));
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 1);
   event_save_files(true, false, NULL);
   assert(disk_holds("/s/game.srm", newest, sizeof(newest)));
   ASSERT_FILES(0, 0, 0);
}

static void test_autosave_item(void)
{
   struct retro_save_item items[1];
   autosave_t *h;
   start();
   app_dir  = "/app";
   items[0] = buffer_item("srm", "/s/auto.srm");
   register_items(items, 1);
   event_load_save_files(false);
   /* Compression asked for, but the item is not the frontend's to
    * transform. */
   h = start_autosave(true);
   /* The image it started from is freed. */
   assert(!h->retro_buffer);

   capture(h);
   assert(raw_writes == 0);

   /* Whole or not at all: the atomic write, never save RAM's stream. */
   wram[1] = 5;
   iram[0] = 6;
   capture(h);
   assert(raw_writes == 1 && rzip_writes == 0 && worker_opens == 0);
   assert(disk_holds("/s/auto.srm", image(), 12));
   assert(!disk_has("/s/auto.srm.tmp"));
   ASSERT_FILES(0, 0, 0);

   capture(h);
   assert(raw_writes == 1);
   autosave_deinit();

   /* Allowed to transform: compressed through a temp file. */
   start();
   items[0].flags = RETRO_SAVE_FLAG_FRONTEND_MAY_TRANSFORM;
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(true);
   wram[1] = 5;
   capture(h);
   assert(rzip_writes == 1 && raw_writes == 0 && worker_opens == 0);
   assert(disk_holds("/s/auto.srm", image(), 12));
   assert(!disk_has("/s/auto.srm.tmp"));
   autosave_deinit();
}

/* With no memory to remember an item was told of, it is not told of
 * again and again: the left-out items run out. */
static void test_told_no_memory(void)
{
   unsigned n = 0;
   struct retro_save_item items[1];
   start();
   items[0] = buffer_item("srm", ".sav");
   items[0].root = RETRO_SAVE_ROOT_FRONTEND;
   fail_items_reallocs = true;
   register_items(items, 1);
   while (n < 100 && save_items_take_left_out())
      n++;
   fail_items_reallocs = false;
   assert(n == 0);
   assert(save_items_state_get_ptr()->current->items[0].left_out);
}

/* A buffer replacing a legacy memory of another size is logged
 * once, at its first fill; at that memory's size, not at all. */
static void test_memory_size(void)
{
   struct retro_save_item items[1];
   struct retro_save_segment seg;
   start();
   items[0]               = buffer_item("srm", "/s/size.srm");
   items[0].legacy_memory = RETRO_MEMORY_SAVE_RAM;
   register_items(items, 1);
   event_load_save_files(false);
   assert(warned("Core: save item 0 \"srm\": legacy_memory = 0: that "
         "memory is 4 bytes and the item 12, and an item that replaces a "
         "memory keeps its bytes there too, for netplay's save sync. See "
         "libretro.h, SET_SAVE_INTERFACE, \"Layout changes\".") == 1);
   register_items(items, 1);
   event_save_items_fill(false);
   assert(warned("that memory is 4 bytes and the item 12") == 1);

   start();
   seg.data               = legacy_ram;
   seg.size               = sizeof(legacy_ram);
   items[0].segments      = &seg;
   items[0].num_segments  = 1;
   register_items(items, 1);
   event_load_save_files(false);
   assert(!warned("that memory is"));
}

/* An item replacing save RAM at a new name starts empty; the old
 * .srm it does not list is named in the log. Listed, it is loaded. */
static void test_old_file_unlisted(void)
{
   static const char *const old[] = { "/s/game.srm", NULL };
   static const unsigned char on_disk[12] = {9, 8, 7, 6, 5, 4, 3, 2, 1,
      0, 1, 2};
   struct retro_save_item items[1];
   start();
   path_init_savefile_rtc("/s/game.srm");
   disk_put("/s/game.srm", on_disk, sizeof(on_disk));
   items[0]               = buffer_item("srm", "/s/game.sav");
   items[0].legacy_memory = RETRO_MEMORY_SAVE_RAM;
   register_items(items, 1);
   event_load_save_files(false);
   assert(wram[0] == 0);
   assert(warned("\"srm\": legacy_paths = NULL: \"/s/game.srm\", where "
         "the frontend saved this memory before, is not among them. See "
         "libretro.h, SET_SAVE_INTERFACE, \"Cross-core compatibility\". It "
         "was not loaded.") == 1);

   start();
   path_init_savefile_rtc("/s/game.srm");
   disk_put("/s/game.srm", on_disk, sizeof(on_disk));
   items[0].legacy_paths  = old;
   register_items(items, 1);
   assert(event_load_save_files(false));
   assert(!memcmp(image(), on_disk, 12));
   assert(!warned("was not loaded"));
}

/* At a save of the files and the close: a blob over its
 * max_size names the sizes, a false blob_save() says so, once each; a
 * short blob_save() leaves zeros, not heap. */
static void test_blob_unsaved_logged(void)
{
   struct retro_save_item items[1];
   memset(&items[0], 0, sizeof(items[0]));
   items[0].id            = "flash";
   items[0].path          = "/s/u.flash";
   items[0].storage       = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   items[0].root          = RETRO_SAVE_ROOT_ABSOLUTE;
   items[0].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;

   start();
   app_dir           = "/app";
   items[0].max_size = 4;
   memcpy(blob_state, "toolong", 7);
   blob_len = 7;
   register_items(items, 1);
   event_load_save_files(false);
   assert(!save_files());
   assert(!save_files());
   event_save_items_final(true, false);
   assert(warned("\"flash\": blob_size = 7: over its max_size 4. See "
         "libretro.h, retro_save_item::max_size. \"/s/u.flash\" is not "
         "saved.") == 1);

   start();
   items[0].max_size = 0;
   memcpy(blob_state, "abcdef", 6);
   blob_len       = 6;
   register_items(items, 1);
   event_load_save_files(false);
   blob_fail_save = true;
   assert(!save_files());
   event_save_items_final(true, false);
   assert(warned("\"flash\": blob_save = false for 6 bytes. See "
         "libretro.h, retro_save_blob_save_t. \"/s/u.flash\" is not "
         "saved.") == 1);

   start();
   memcpy(blob_state, "abcdef", 6);
   blob_len   = 6;
   blob_short = 2;
   register_items(items, 1);
   event_load_save_files(false);
   assert(save_files());
   assert(disk_holds("/s/u.flash", "ab\0\0\0\0", 6));
   assert(!warned("is not saved"));
}

static void test_generation(void)
{
   struct retro_save_item items[1];
   autosave_t *h;
   start();
   use_generation = true;
   items[0] = buffer_item("srm", "/s/gen.srm");
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);

   /* Bytes changed but the generation did not: no copy, no write. */
   wram[0] = 1;
   capture(h);
   assert(raw_writes == 0);

   generation++;
   capture(h);
   assert(raw_writes == 1 && disk_holds("/s/gen.srm", image(), 12));

   /* A failed write is retried with the generation unchanged. */
   slock_lock(disk_lock);
   fail_writes = true;
   slock_unlock(disk_lock);
   wram[0] = 2;
   generation++;
   capture(h);
   assert(raw_writes == 2 && !disk_holds("/s/gen.srm", image(), 12));
   slock_lock(disk_lock);
   fail_writes = false;
   slock_unlock(disk_lock);
   capture(h);
   assert(raw_writes == 3 && disk_holds("/s/gen.srm", image(), 12));

   capture(h);
   assert(raw_writes == 3);
   autosave_deinit();
}

/* A generation that stood still while the bytes changed is caught
 * at a save of the files, logged once, and no longer trusted: the next
 * interval copies. */
static void test_generation_stuck(void)
{
   struct retro_save_item items[1];
   autosave_t *h;
   start();
   use_generation = true;
   items[0] = buffer_item("srm", "/s/stuck.srm");
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   wram[0] = 1;
   capture(h);
   assert(!disk_has("/s/stuck.srm"));
   assert(save_files());
   assert(disk_holds("/s/stuck.srm", image(), 12));
   assert(warned("Core: save item 0 \"srm\": get_generation = 0: it did "
         "not change while the bytes did. See libretro.h, "
         "retro_save_get_generation_t. Autosave now copies the item every "
         "interval.") == 1);
   wram[0] = 2;
   capture(h);
   capture(h);
   assert(disk_holds("/s/stuck.srm", image(), 12));
   assert(save_files());
   assert(warned("did not change while the bytes did") == 1);
   autosave_deinit();

   /* Nearest valid: the bytes did not change. */
   start();
   use_generation = true;
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   capture(h);
   assert(save_files());
   assert(!warned("did not change"));
   autosave_deinit();
}

/* At autosave: a blob grown over its max_size, or whose
 * blob_save() returns false, is logged once and stays unsaved, with no
 * notice; a short blob_save() leaves zeros, not the last image. */
static void test_blob_autosave_unsaved(void)
{
   struct retro_save_item items[1];
   autosave_t *h;
   start();
   items[0]          = blob_item("flash", "/s/a.flash");
   items[0].max_size = 8;
   memcpy(blob_state, "abcdef", 6);
   blob_len = 6;
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   memcpy(blob_state, "toolong!!", 9);
   blob_len = 9;
   capture(h);
   capture(h);
   assert(warned("\"flash\": blob_size = 9: over its max_size 8. See "
         "libretro.h, retro_save_item::max_size. \"/s/a.flash\" is not "
         "saved.") == 1);
   assert(!disk_has("/s/a.flash") && osd_count == 0);
   autosave_deinit();

   start();
   items[0].max_size = 0;
   memcpy(blob_state, "abcdef", 6);
   blob_len = 6;
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   blob_fail_save = true;
   blob_state[0]  = 'z';
   capture(h);
   capture(h);
   assert(warned("\"flash\": blob_save = false for 6 bytes. See "
         "libretro.h, retro_save_blob_save_t. \"/s/a.flash\" is not "
         "saved.") == 1);
   assert(!disk_has("/s/a.flash") && osd_count == 0);
   blob_fail_save = false;
   capture(h);
   assert(disk_holds("/s/a.flash", "zbcdef", 6));
   blob_short    = 2;
   blob_state[0] = 'y';
   capture(h);
   capture(h);
   assert(disk_holds("/s/a.flash", "yb\0\0\0\0", 6));
   autosave_deinit();
}

/* can_snapshot() false: the copy waits, retried each frame, and is
 * made on the first that says true; the file never holds the image it
 * refused. Asked only once the generation moved. Pass 1: a blob, never
 * encoded while refused. */
static void test_snapshot_wait(void)
{
   struct retro_save_item items[1];
   unsigned char done[12];
   unsigned pass;
   for (pass = 0; pass < 2; pass++)
   {
      autosave_t *h;
      const char *path = pass ? "/s/wait.flash" : "/s/wait.srm";
      size_t len       = pass ? 6 : 12;
      unsigned saves;
      start();
      app_dir          = "/app";
      use_generation   = true;
      use_can_snapshot = true;
      if (pass)
      {
         items[0] = blob_item("flash", path);
         disk_put(path, "abcdef", 6);
      }
      else
         items[0] = buffer_item("srm", path);
      register_items(items, 1);
      event_load_save_files(false);
      h = start_autosave(false);
      capture(h);
      assert(snapshot_calls == 0 && raw_writes == 0);

      /* A save the game finished. */
      wram[0]       = 1;
      blob_state[0] = 'b';
      generation++;
      capture(h);
      assert(snapshot_calls == 1 && raw_writes == 1);
      memcpy(done, pass ? blob_state : image(), len);
      assert(disk_holds(path, done, len));

      /* Partway through the next: not copied, asked again each frame. */
      snapshot_ready = false;
      saves          = blob_saves;
      wram[0]        = 2;
      blob_state[0]  = 'c';
      generation++;
      autosave_check();
      assert(snapshot_calls == 2);
      assert(retro_atomic_load_acquire_int(&h->snapshot_requested));
      wram[1]       = 3;
      blob_state[1] = 'd';
      generation++;
      event_save_items_fill(false);
      autosave_check();
      assert(snapshot_calls == 3 && blob_saves == saves);
      /* Killed here: the finished save is the file. */
      assert(raw_writes == 1 && disk_holds(path, done, len));

      /* Finished: copied on that frame. */
      snapshot_ready = true;
      event_save_items_fill(false);
      capture(h);
      assert(snapshot_calls == 4 && raw_writes == 2);
      assert(disk_holds(path, pass ? blob_state : image(), len));
      ASSERT_FILES(0, 0, 0);
      assert(err_count == 0 && saving_logs == 0);
      autosave_deinit();
   }
}

/* @n frames in which the core ran, at @fps. */
static void core_frames(unsigned n, double fps)
{
   unsigned i;
   for (i = 0; i < n; i++)
   {
      event_save_items_fill(false);
      event_save_items_ran(fps);
      autosave_check();
   }
}

/* can_snapshot() false for good: copied anyway once 10 s of frames the
 * core ran have passed at its frame rate, logged once per item. Passes
 * that ran no frame, paused, in the menu or held by netplay, do not
 * count; a later interval waits afresh, at the core's rate then. */
static void test_snapshot_cap(void)
{
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned i;
   start();
   app_dir          = "/app";
   use_can_snapshot = true;
   snapshot_ready   = false;
   items[0] = buffer_item("srm", "/s/cap.srm");
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   wram[0] = 1;
   autosave_check();
   for (i = 0; i < 2 * 300; i++)
      autosave_check();
   /* Held by netplay: the frame starts, retro_run() does not. */
   for (i = 0; i < 2 * 300; i++)
   {
      event_save_items_fill(false);
      autosave_check();
   }
   core_frames(299, 30.0);
   assert(raw_writes == 0);
   assert(retro_atomic_load_acquire_int(&h->snapshot_requested));
   event_save_items_fill(false);
   event_save_items_ran(30.0);
   capture(h);
   assert(raw_writes == 1 && disk_holds("/s/cap.srm", image(), 12));
   assert(saving_logs == 1 && err_count == 0);
   ASSERT_FILES(0, 0, 0);

   /* The next interval copies as soon as the answer is true. */
   snapshot_ready = true;
   wram[0]        = 2;
   capture(h);
   assert(raw_writes == 2 && disk_holds("/s/cap.srm", image(), 12));

   /* The core now runs at 120 fps: 1200 frames; told once. */
   snapshot_ready = false;
   wram[0]        = 3;
   autosave_check();
   core_frames(1199, 120.0);
   assert(raw_writes == 2);
   assert(retro_atomic_load_acquire_int(&h->snapshot_requested));
   event_save_items_fill(false);
   event_save_items_ran(120.0);
   capture(h);
   assert(raw_writes == 3 && disk_holds("/s/cap.srm", image(), 12));
   assert(saving_logs == 1 && err_count == 0);
   ASSERT_FILES(0, 0, 0);
   autosave_deinit();

   /* A true answer whose fill fails ends the refusal: the next false
    * waits the whole cap again. */
   start();
   app_dir          = "/app";
   use_can_snapshot = true;
   snapshot_ready   = false;
   items[0] = blob_item("flash", "/s/cap.flash");
   disk_put("/s/cap.flash", "abcdef", 6);
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   blob_state[0] = 'b';
   autosave_check();
   core_frames(299, 30.0);
   snapshot_ready = true;
   blob_len       = 0;
   core_frames(1, 30.0);
   snapshot_ready = false;
   blob_len       = 6;
   core_frames(1, 30.0);
   assert(retro_atomic_load_acquire_int(&h->snapshot_requested));
   assert(raw_writes == 0 && saving_logs == 0 && err_count == 0);
   autosave_deinit();
}

/* A registration from inside can_snapshot() is refused with one error;
 * the copy that asked goes on. */
static void test_snapshot_reregister(void)
{
   struct retro_save_item items[1];
   struct retro_save_item other[1];
   autosave_t *h;
   start();
   app_dir          = "/app";
   use_can_snapshot = true;
   items[0] = buffer_item("srm", "/s/rr.srm");
   other[0] = buffer_item("b", "/s/b.srm");
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   wram[0]      = 1;
   rereg_items  = other;
   rereg_n      = 1;
   rereg_inside = REREG_SNAPSHOT;
   capture(h);
   assert(rereg_inside == 0 && rereg_refusals == 1);
   assert(err_count == 1 && strstr(err_last, "can_snapshot()"));
   assert(save_items_state_get_ptr()->current->num_items == 1);
   assert(disk_holds("/s/rr.srm", image(), 12) && !disk_has("/s/b.srm"));
   ASSERT_FILES(0, 0, 0);
   autosave_deinit();
}

/* With disk_lock held: until *@count reaches @n, or about 30 s. */
static void disk_wait_count(const unsigned *count, unsigned n)
{
   retro_time_t end = cpu_features_get_time_usec() + 30000000;
   while (*count < n && cpu_features_get_time_usec() < end)
      scond_wait_timeout(disk_cond, disk_lock, 1000000);
}

static void watchdog(void *data)
{
   unsigned i;
   (void)data;
   for (i = 0; i < 3000; i++)
   {
      bool done;
      slock_lock(disk_lock);
      done = retire_returned;
      slock_unlock(disk_lock);
      if (done)
         return;
      retro_sleep(1);
   }
   slock_lock(disk_lock);
   retire_timed_out = true;
   hold_write       = false;
   scond_broadcast(disk_cond);
   slock_unlock(disk_lock);
}

static void test_retire_without_join(void)
{
   unsigned char last[12];
   struct retro_save_item items[1];
   autosave_t *h;
   sthread_t *dog;
   unsigned i;

   start();
   app_dir = "/app";
   items[0] = buffer_item("srm", "/s/retire.srm");
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);

   /* The worker starts a write and is held inside it. */
   slock_lock(disk_lock);
   hold_write = true;
   slock_unlock(disk_lock);
   wram[2] = 7;
   autosave_lock();
   autosave_unlock();
   slock_lock(disk_lock);
   disk_wait_count(&raw_writes, 1);
   assert(raw_writes == 1);
   slock_unlock(disk_lock);

   /* The item leaves the set while its worker is stuck: a join here
    * would never return, so a watchdog lets the write go and the
    * test fails. */
   wram[2] = 8;
   memcpy(last, image(), sizeof(last));
   retire_returned = false;
   dog             = sthread_create(watchdog, NULL);
   assert(dog);
   register_items(NULL, 0);
   slock_lock(disk_lock);
   retire_returned = true;
   assert(!retire_timed_out);
   assert(autosave_state.num_retired == 1);
   assert(!retro_atomic_load_acquire_int(&h->exited));
   hold_write = false;
   scond_broadcast(disk_cond);
   slock_unlock(disk_lock);
   sthread_join(dog);
   sthread_join(h->thread);
   h->thread = NULL;
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 0);

   /* A later frame frees it, once it has written the last image. */
   for (i = 0; autosave_state.num_retired && i < 5000; i++)
   {
      autosave_check();
      retro_sleep(1);
   }
   assert(autosave_state.num_retired == 0);
   assert(disk_holds("/s/retire.srm", last, sizeof(last)));
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 0);
   autosave_deinit();
}

static void test_orphans(void)
{
   struct retro_save_item items[2];
   start();
   app_dir  = "/app";
   items[0] = buffer_item("a", "/s/a.srm");
   register_items(items, 1);
   event_load_save_files(false);
   wram[0] = 3;
   register_items(NULL, 0);
   assert(save_item_files.num_orphans == 1 && !disk_has("/s/a.srm"));
   wram[0] = 4;
   assert(save_files());
   assert(save_item_files.num_orphans == 0);
   wram[0] = 3;
   assert(disk_holds("/s/a.srm", image(), 12));
   ASSERT_FILES(0, 0, 0);

   /* Never written before the load has filled it. */
   start();
   register_items(items, 1);
   wram[0] = 5;
   register_items(NULL, 0);
   assert(save_item_files.num_orphans == 0);
}

static void test_late_fill(void)
{
   static const unsigned char saved[6] = {'S', 'A', 'V', 'E', 'D', '!'};
   static const unsigned char other[6] = {'O', 'T', 'H', 'E', 'R', '!'};
   static const unsigned char blank[6] = {0};
   struct retro_save_item items[2];
   autosave_t *h;

   /* A card inserted while the game runs, its file on disk: the
    * registration reads nothing, a SAVE_FILES before the fill leaves
    * the file alone, the fill reads it. */
   start();
   disk_put("/s/one.card", saved, sizeof(saved));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   event_load_save_files(false);
   items[1] = card_item("card", "/s/one.card");
   register_items(items, 2);
   assert(save_item_files.fill_pending && !save_item_files.files[1].taken_up);
   assert(!memcmp(card, blank, sizeof(card)));
   save_files();
   assert(raw_writes == 0 && disk_holds("/s/one.card", saved, 6));
   event_save_items_fill(false);
   assert(!save_item_files.fill_pending && save_item_files.files[1].taken_up);
   assert(!memcmp(card, saved, sizeof(card)));
   assert(save_files());
   assert(raw_writes == 0);

   /* The same id moved to another file reads that file. */
   disk_put("/s/two.card", other, sizeof(other));
   items[1].path = "/s/two.card";
   register_items(items, 2);
   assert(save_item_files.fill_pending);
   event_save_items_fill(false);
   assert(!memcmp(card, other, sizeof(card)));
   assert(save_files());
   assert(raw_writes == 0 && disk_holds("/s/one.card", saved, 6));

   /* No file: taken up as it is, written once it changes. */
   start();
   register_items(items, 1);
   event_load_save_files(false);
   memcpy(card, "fresh!", 6);
   items[1].path = "/s/new.card";
   register_items(items, 2);
   event_save_items_fill(false);
   assert(save_item_files.files[1].taken_up);
   assert(save_files());
   assert(!disk_has("/s/new.card"));
   card[0] = 'F';
   assert(save_files());
   assert(disk_holds("/s/new.card", card, sizeof(card)));

   /* Loading disabled: the file is not read. */
   start();
   disk_put("/s/one.card", saved, sizeof(saved));
   register_items(items, 1);
   event_load_save_files(false);
   items[1].path = "/s/one.card";
   register_items(items, 2);
   event_save_items_fill(true);
   assert(!memcmp(card, blank, sizeof(card)));
   assert(save_item_files.files[1].taken_up);

   /* Its worker starts after the fill, from the image read. */
   start();
   disk_put("/s/one.card", saved, sizeof(saved));
   register_items(items, 1);
   event_load_save_files(false);
   start_autosave(false);
   register_items(items, 2);
   assert(!save_item_files.files[1].autosave);
   event_save_items_fill(false);
   assert((h = save_item_files.files[1].autosave));
   h->interval = 0;
   wait_requested(h);
   capture(h);
   assert(raw_writes == 0);
   autosave_deinit();
}

/* A file in a folder is the core's while the folder's patterns select
 * it, and every file when there is no memory to match them. */
static void test_ownership_patterns(void)
{
   static const char *const mcd[]  = { "*.mcd", NULL };
   static const char *const skip[] = { "b*", NULL };
   struct retro_save_item items[1];
   start();
   memset(&items[0], 0, sizeof(items[0]));
   items[0].id            = "cards";
   items[0].path          = "/s/mc";
   items[0].storage       = RETRO_SAVE_STORAGE_CORE_FOLDER;
   items[0].root          = RETRO_SAVE_ROOT_ABSOLUTE;
   items[0].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   items[0].include       = mcd;
   items[0].exclude       = skip;
   register_items(items, 1);
   assert(content_savefile_is_live("/s/mc"));
   assert(content_savefile_is_live("/s/mc/a.mcd"));
   assert(content_savefile_is_live("/S/MC/A.MCD"));
   assert(content_savefile_is_live("/s/mc/sub\\c.mcd"));
   assert(!content_savefile_is_live("/s/mc/a.sav"));
   assert(!content_savefile_is_live("/s/mc/b.mcd"));
   assert(!content_savefile_is_live("/s/mcx/a.mcd"));
   retro_atomic_store_release_int(&fail_malloc_size, 2 * PATH_MAX_LENGTH);
   assert(content_savefile_is_live("/s/mc/a.sav"));
   retro_atomic_store_release_int(&fail_malloc_size, 0);
   assert(err_count == 0);
}

static void test_ownership(void)
{
   struct retro_save_item items[4];
   unsigned i;
   start();
   path_deinit_savefile();
   items[0] = buffer_item("srm", "/s/Game.srm");
   memset(&items[1], 0, sizeof(items[1]));
   items[1].id            = "core";
   items[1].path          = "/s/core.dat";
   items[1].storage       = RETRO_SAVE_STORAGE_CORE_FILE;
   items[1].root          = RETRO_SAVE_ROOT_ABSOLUTE;
   items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   items[2]         = items[1];
   items[2].id      = "card";
   items[2].path    = "/s/card";
   items[2].storage = RETRO_SAVE_STORAGE_CORE_FOLDER;
   items[3]         = items[1];
   items[3].id      = "tmp";
   items[3].path    = "/s/tmp.bin";
   items[3].storage = RETRO_SAVE_STORAGE_NOT_PERSISTED;
   register_items(items, 4);
   assert(content_savefile_is_live("/S/GAME.SRM"));
   assert(content_savefile_is_live("/s/core.dat"));
   assert(content_savefile_is_live("/s/card"));
   assert(content_savefile_is_live("/s/card/slot1.mcr"));
   assert(!content_savefile_is_live("/s/cardx"));
   assert(!content_savefile_is_live("/s/tmp.bin"));
   save_items_clear();
   assert(!content_savefile_is_live("/s/Game.srm"));

   /* An item that left the set owns its file until its last image is
    * written: an orphan's at the next SAVE_FILES, a worker's until the
    * worker is freed. */
   start();
   register_items(items, 1);
   event_load_save_files(false);
   register_items(NULL, 0);
   assert(save_item_files.num_orphans == 1);
   assert(content_savefile_is_live("/S/GAME.SRM"));
   assert(save_files());
   assert(!content_savefile_is_live("/s/Game.srm"));

   /* After a failed write its image is in a recovery file. */
   start();
   app_dir = "/app";
   register_items(items, 1);
   event_load_save_files(false);
   wram[0] = 7;
   register_items(NULL, 0);
   set_fail_save_writes(true);
   assert(!save_files());
   set_fail_save_writes(false);
   assert(!content_savefile_is_live("/s/Game.srm"));
   assert(disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
   ASSERT_FILES(0, 1, 1);

   start();
   register_items(items, 1);
   event_load_save_files(false);
   start_autosave(false);
   register_items(NULL, 0);
   assert(autosave_state.num_retired == 1);
   assert(content_savefile_is_live("/s/Game.srm"));
   for (i = 0; autosave_state.num_retired && i < 5000; i++)
   {
      autosave_check();
      retro_sleep(1);
   }
   assert(autosave_state.num_retired == 0);
   assert(!content_savefile_is_live("/s/Game.srm"));
   autosave_deinit();
}

/* A disk file whose path holds @part. */
static bool disk_named(const char *part)
{
   unsigned i;
   bool has = false;
   slock_lock(disk_lock);
   for (i = 0; i < DISK_FILES; i++)
      if (disk[i].exists && strstr(disk[i].path, part))
         has = true;
   slock_unlock(disk_lock);
   return has;
}

/* How many disk files hold @data at a path holding @part. */
static unsigned disk_named_holds(const char *part, const void *data,
      size_t len)
{
   unsigned i;
   unsigned n = 0;
   slock_lock(disk_lock);
   for (i = 0; i < DISK_FILES; i++)
      if (     disk[i].exists && strstr(disk[i].path, part)
            && disk[i].size == len && !memcmp(disk[i].data, data, len))
         n++;
   slock_unlock(disk_lock);
   return n;
}

/* How many messages since start() name, second, a file holding @data. */
static unsigned told_holding(const void *data, size_t len)
{
   char name[96];
   unsigned n      = 0;
   const char *s   = osd_hist;
   const char *bar;
   while ((bar = strchr(s, '|')))
   {
      const char *end = strchr(bar, '\n');
      size_t _len     = (size_t)(end - bar - 1);
      if (_len < sizeof(name))
      {
         memcpy(name, bar + 1, _len);
         name[_len] = '\0';
         if (disk_holds(name, data, len))
            n++;
      }
      s = end + 1;
   }
   return n;
}

/* How many disk files' paths hold @part. */
static unsigned disk_count_named(const char *part)
{
   unsigned i;
   unsigned n = 0;
   slock_lock(disk_lock);
   for (i = 0; i < DISK_FILES; i++)
      if (disk[i].exists && strstr(disk[i].path, part))
         n++;
   slock_unlock(disk_lock);
   return n;
}

/* The last log line naming a file in /undo/ holds @part. */
static bool copy_logged(const char *part)
{
   bool has;
   slock_lock(disk_lock);
   has = strstr(copy_log, part) != NULL;
   slock_unlock(disk_lock);
   return has;
}

static void fill_bytes(unsigned char *buf, size_t len, unsigned from)
{
   size_t i;
   for (i = 0; i < len; i++)
      buf[i] = (unsigned char)(from + i);
}

static void set_fail_writes(bool fail)
{
   slock_lock(disk_lock);
   fail_writes = fail;
   slock_unlock(disk_lock);
}

static void set_hold(bool *hold, bool on)
{
   slock_lock(disk_lock);
   *hold = on;
   scond_broadcast(disk_cond);
   slock_unlock(disk_lock);
}

/* Until a worker has been held @n times. */
static void wait_held(unsigned n)
{
   slock_lock(disk_lock);
   disk_wait_count(&worker_holds, n);
   assert(worker_holds == n);
   slock_unlock(disk_lock);
}

static void set_fail_undo(bool fail)
{
   slock_lock(disk_lock);
   fail_undo = fail;
   slock_unlock(disk_lock);
}

static void set_fail_undo_left(unsigned n)
{
   slock_lock(disk_lock);
   fail_undo_left = n;
   slock_unlock(disk_lock);
}

static void set_fail_save_writes(bool fail)
{
   slock_lock(disk_lock);
   fail_save_writes = fail;
   slock_unlock(disk_lock);
}

static void set_fail_read(const char *path)
{
   slock_lock(disk_lock);
   fail_read_path = path;
   slock_unlock(disk_lock);
}

/* The bytes past the item come back with every write. */
static void test_tail_written_back(void)
{
   unsigned char file[16];
   unsigned char want[16];
   struct retro_save_item items[1];
   autosave_t *h;

   start();
   app_dir = "/app";
   fill_bytes(file, sizeof(file), 0x40);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   assert(event_load_save_files(false) && !memcmp(image(), file, 12));
   assert(save_items_state_get_ptr()->current->items[0].tail_size == 4);

   /* Head and tail match the file: nothing to write. */
   assert(save_files() && raw_writes == 0);
   memcpy(want, file, sizeof(want));
   wram[0] = want[0] = 1;
   assert(save_files());
   assert(raw_writes == 1 && disk_holds("/s/game.srm", want, 16));

   /* The worker copies the tail into its snapshot once. */
   h = start_autosave(false);
   wram[1] = want[1] = 2;
   capture(h);
   assert(disk_holds("/s/game.srm", want, 16) && h->tail_kept);
   wram[2] = want[2] = 3;
   capture(h);
   assert(raw_writes == 3 && disk_holds("/s/game.srm", want, 16));
   autosave_deinit();

   /* An orphan's last image keeps it too. */
   wram[3] = want[3] = 4;
   items[0] = card_item("card", "/s/card.bin");
   register_items(items, 1);
   assert(save_files());
   assert(disk_holds("/s/game.srm", want, 16));
   ASSERT_FILES(0, 0, 0);
}

/* A cart moving port keeps the bytes its file holds past it. */
static void test_port_move_tail(void)
{
   unsigned char file[16];
   unsigned char want[16];
   struct retro_save_item items[1];
   const save_item_t *it;

   start();
   fill_bytes(file, sizeof(file), 0x70);
   disk_put("/s/cart.sav", file, sizeof(file));
   items[0] = buffer_item("tpak1", "/s/cart.sav");
   register_items(items, 1);
   assert(event_load_save_files(false));
   items[0].id = "tpak2";
   register_items(items, 1);
   it = &save_items_state_get_ptr()->current->items[0];
   assert(it->tail_size == 4 && !memcmp(it->tail, file + 12, 4));
   assert(save_item_files.files[0].taken_up);
   memcpy(want, file, sizeof(want));
   wram[0] = want[0] = 1;
   assert(save_files());
   assert(raw_writes == 1 && disk_holds("/s/cart.sav", want, 16));
}

/* An id that stops being a buffer leaves its tail with the buffer's
 * last image; a buffer again reads the file's current one, or none
 * when it is taken up without a read. */
static void test_storage_change_tail(void)
{
   unsigned char file[16];
   struct retro_save_item items[1];

   start();
   fill_bytes(file, sizeof(file), 0x80);
   disk_put("/s/game.sav", file, sizeof(file));
   items[0] = buffer_item("sav", "/s/game.sav");
   register_items(items, 1);
   assert(event_load_save_files(false));

   items[0].storage      = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   items[0].segments     = NULL;
   items[0].num_segments = 0;
   register_items(items, 1);
   assert(!save_items_state_get_ptr()->current->items[0].tail_size);
   event_save_items_fill(false);
   assert(blob_len == 16 && !memcmp(blob_state, file, 16));
   assert(save_files());
   assert(raw_writes == 0 && disk_holds("/s/game.sav", file, 16));
   memcpy(blob_state, "abcdef", 6);
   blob_len = 6;
   assert(save_files());
   assert(disk_holds("/s/game.sav", "abcdef", 6));

   items[0] = buffer_item("sav", "/s/game.sav");
   register_items(items, 1);
   assert(!save_items_state_get_ptr()->current->items[0].tail_size);
   event_save_items_fill(true);
   wram[0] = 1;
   assert(save_files());
   assert(disk_holds("/s/game.sav", image(), 12));
}

/* A successor at the file that took the tail but not the record
 * (FRONTEND_MAY_TRANSFORM flips): the closed record's last image
 * keeps the end, written before the successor's fill or after. Pass 1:
 * the fill first; 2: cut too; 3: a moved id; 4: a grow pending. */
static void test_flag_change_tail(void)
{
   unsigned char file[16];
   unsigned char want[16];
   struct retro_save_item items[1];
   unsigned pass;

   fill_bytes(file, sizeof(file), 0x90);
   for (pass = 0; pass < 5; pass++)
   {
      start();
      disk_put("/s/game.srm", file, sizeof(file));
      items[0] = buffer_item("srm", "/s/game.srm");
      register_items(items, 1);
      assert(event_load_save_files(false));
      if (pass == 4)
      {
         items[0].num_segments = 1;
         register_items(items, 1);
         memset(iram, 0xee, sizeof(iram));
         items[0].num_segments = 2;
         register_items(items, 1);
         assert(save_item_files.files[0].grow_from == 8);
      }
      memcpy(want, file, sizeof(want));
      wram[0] = want[0] = 1;
      if (pass == 2)
         items[0].num_segments = 1;
      if (pass == 3)
         items[0].id = "srm2";
      items[0].flags = RETRO_SAVE_FLAG_FRONTEND_MAY_TRANSFORM;
      register_items(items, 1);
      assert(save_item_files.num_orphans == 1);
      if (pass == 1)
         event_save_items_fill(false);
      assert(save_files());
      assert(disk_holds("/s/game.srm", want, 16));
      event_save_items_fill(false);
      wram[1] = want[1] = 2;
      assert(save_files());
      assert(disk_holds("/s/game.srm", want, 16));
   }
}

/* As above with a worker: its last image keeps the end, renamed
 * before the successor's fill reads the file (pass 0), or taken by
 * that fill while the worker still writes it (pass 1). */
static void test_flag_change_tail_worker(void)
{
   unsigned char file[16];
   unsigned char want[16];
   struct retro_save_item items[1];
   unsigned pass, i;

   fill_bytes(file, sizeof(file), 0xa0);
   for (pass = 0; pass < 2; pass++)
   {
      start();
      disk_put("/s/game.srm", file, sizeof(file));
      items[0] = buffer_item("srm", "/s/game.srm");
      register_items(items, 1);
      assert(event_load_save_files(false));
      start_autosave(false);
      memcpy(want, file, sizeof(want));
      wram[0] = want[0] = 1;
      if (pass)
         set_hold(&hold_temp, true);
      items[0].flags = RETRO_SAVE_FLAG_FRONTEND_MAY_TRANSFORM;
      register_items(items, 1);
      assert(autosave_state.num_retired == 1);
      if (pass)
      {
         wait_held(1);
         event_save_items_fill(false);
         assert(!memcmp(image(), want, 12));
         set_hold(&hold_temp, false);
      }
      for (i = 0; autosave_state.num_retired && i < 5000; i++)
      {
         autosave_check();
         retro_sleep(1);
      }
      assert(autosave_state.num_retired == 0);
      assert(disk_holds("/s/game.srm", want, 16));
      event_save_items_fill(false);
      memcpy(want, image(), 12);
      wram[1] = want[1] = 2;
      assert(save_files());
      assert(disk_holds("/s/game.srm", want, 16));
      autosave_deinit();
   }
}

/* No memory for the new set's records: a closed record's last image
 * still keeps the end its successor took. */
static void test_close_no_memory_tail(void)
{
   unsigned char file[16];
   unsigned char want[16];
   struct retro_save_item items[1];

   start();
   fill_bytes(file, sizeof(file), 0xb0);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   assert(event_load_save_files(false));
   memcpy(want, file, sizeof(want));
   wram[0] = want[0] = 1;
   retro_atomic_store_release_int(&fail_callocs, 1);
   register_items(items, 1);
   retro_atomic_store_release_int(&fail_callocs, 0);
   assert(!save_item_files.num_files && save_item_files.num_orphans == 1);
   /* Shown: what the core holds now is not written again. */
   assert(osd_count == 1 && err_count == 1);
   assert(save_files());
   assert(disk_holds("/s/game.srm", want, 16));
   assert(osd_count == 1 && !disk_named("/undo/"));
}

/* No memory to keep what a cut takes off: the closed record's last
 * image keeps the whole end, and its successor reads it back. */
static void test_cut_no_memory_tail(void)
{
   unsigned char file[20];
   unsigned char want[20];
   struct retro_save_item items[1];

   start();
   fill_bytes(file, sizeof(file), 0xc0);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   assert(event_load_save_files(false));
   memcpy(want, file, sizeof(want));
   wram[0] = want[0] = 1;
   items[0].num_segments = 1;
   items[0].flags        = RETRO_SAVE_FLAG_FRONTEND_MAY_TRANSFORM;
   /* The 4 cut bytes and the 8 past the item. */
   retro_atomic_store_release_int(&fail_malloc_size, 12);
   register_items(items, 1);
   retro_atomic_store_release_int(&fail_malloc_size, 0);
   assert(err_count == 1 && strstr(err_last, "cut by"));
   assert(save_item_files.num_orphans == 1);
   assert(save_files());
   assert(disk_holds("/s/game.srm", want, 20));
   event_save_items_fill(false);
   wram[1] = want[1] = 2;
   assert(save_files());
   assert(disk_holds("/s/game.srm", want, 20));
}

/* The same for an item that keeps its file and record, with an end
 * past it or none: the write that drops the end copies the file first,
 * and without the copy leaves it as it is. Passes 2-3 the worker, the
 * others SAVE_FILES; 4-5 a file of the item's size; in odd passes the
 * first copy fails. */
static void test_cut_no_memory_kept(void)
{
   unsigned char file[20];
   struct retro_save_item items[1];
   unsigned pass;

   fill_bytes(file, sizeof(file), 0xd0);
   for (pass = 0; pass < 6; pass++)
   {
      autosave_t *h = NULL;
      size_t size   = pass < 4 ? 20 : 12;

      start();
      app_dir = "/app";
      disk_put("/s/game.srm", file, size);
      items[0] = buffer_item("srm", "/s/game.srm");
      register_items(items, 1);
      assert(event_load_save_files(false));
      if (pass == 2 || pass == 3)
         h = start_autosave(false);
      wram[0]               = 1;
      items[0].num_segments = 1;
      /* The copy starts at the cut. */
      set_fail_undo((pass & 1) != 0);
      /* The 4 cut bytes and the end past the item. */
      retro_atomic_store_release_int(&fail_malloc_size, (int)size - 8);
      register_items(items, 1);
      retro_atomic_store_release_int(&fail_malloc_size, 0);
      assert(err_count == 1 && strstr(err_last, "cut by"));
      assert(     save_item_files.files[0].taken_up
               && save_item_files.files[0].autosave == h
               && !save_item_files.num_orphans);

      if (h)
         capture(h);
      else
         assert(save_files() == !(pass & 1));
      if (pass & 1)
      {
         assert(disk_holds("/s/game.srm", file, size));
         if (h)
            assert(retro_atomic_load_acquire_int(&h->failed));
         else
            assert(disk_named("RetroArch-recovery-"));
         set_fail_undo(false);
         if (h)
            capture(h);
         else
         {
            /* Recovered again, the copy tried after it. */
            assert(!save_files());
            assert(save_files());
         }
      }
      assert(disk_holds("/undo/game.srm", file, size));
      assert(disk_holds("/s/game.srm", image(), 8));
      if (h)
         autosave_deinit();
   }
}

/* A re-registration that keeps the file cuts into the tail or grows
 * from it, writing nothing into the core's buffers during the call. */
static void test_reshape(void)
{
   unsigned char file[16];
   struct retro_save_item items[1];
   const save_item_t *it;

   start();
   fill_bytes(file, sizeof(file), 0x60);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   assert(event_load_save_files(false));

   items[0].num_segments = 1;
   register_items(items, 1);
   it = &save_items_state_get_ptr()->current->items[0];
   assert(it->tail_size == 8 && !memcmp(it->tail, file + 8, 8));
   assert(save_files() && raw_writes == 0);

   memset(iram, 0xee, sizeof(iram));
   items[0].num_segments = 2;
   register_items(items, 1);
   assert(iram[0] == 0xee && save_item_files.files[0].grow_from == 8);
   assert(save_files() && raw_writes == 0);
   event_save_items_fill(false);
   it = &save_items_state_get_ptr()->current->items[0];
   assert(!memcmp(iram, file + 8, 4) && it->tail_size == 4);
   assert(!save_item_files.files[0].grow_from);
   assert(save_files() && raw_writes == 0);
}

/* EXACT_SIZE: the file is copied, then cut to the item. */
static void test_exact_size_cut_after_copy(void)
{
   unsigned char file[16];
   unsigned char want[16];
   struct retro_save_item items[2];
   autosave_t *h;
   unsigned i;
   unsigned logs;

   start();
   app_dir = "/app";
   fill_bytes(file, sizeof(file), 0x20);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0]       = buffer_item("srm", "/s/game.srm");
   items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   register_items(items, 1);
   assert(event_load_save_files(false));
   assert(save_item_files.files[0].guard.copy_first);
   assert(save_files());
   assert(disk_holds("/undo/game.srm", file, 16));
   assert(disk_holds("/s/game.srm", file, 12));
   assert(raw_writes == 2 && undo_writes == 1);
   assert(!save_item_files.files[0].guard.copy_first);
   assert(!save_items_state_get_ptr()->current->items[0].tail_size);
   assert(save_files());
   assert(raw_writes == 2 && undo_writes == 1);
   ASSERT_FILES(1, 0, 0);

   /* No copy can be made: the tail stays this time, the copy is owed. */
   start();
   app_dir = "/app";
   event_save_items_set_undo_dir(NULL);
   disk_put("/s/game.srm", file, sizeof(file));
   register_items(items, 1);
   event_load_save_files(false);
   wram[0] = 0x55;
   assert(save_files());
   assert(raw_writes == 1 && undo_writes == 0);
   assert(save_item_files.files[0].guard.copy_first);
   memcpy(want, image(), 12);
   memcpy(want + 12, file + 12, 4);
   assert(!disk_named("/undo/") && disk_holds("/s/game.srm", want, 16));
   ASSERT_FILES(0, 0, 0);
   event_save_items_set_undo_dir("/undo");
   assert(save_files());
   assert(disk_holds("/s/game.srm", image(), 12));
   assert(disk_has("/undo/game.srm"));
   ASSERT_FILES(1, 0, 0);

   /* The worker copies on its own thread; the frame start collects. */
   start();
   disk_put("/s/game.srm", file, sizeof(file));
   register_items(items, 1);
   load_for_worker();
   h = start_autosave(false);
   wram[1] = 0x66;
   capture(h);
   assert(disk_holds("/undo/game.srm", file, 16));
   assert(disk_holds("/s/game.srm", image(), 12));
   assert(raw_writes == 2 && undo_writes == 1);
   event_save_items_fill(false);
   assert(!save_item_files.files[0].guard.copy_first);
   assert(!save_items_state_get_ptr()->current->items[0].tail_size);
   capture(h);
   assert(raw_writes == 2 && undo_writes == 1);
   autosave_deinit();

   /* A re-registration that cuts the item owes a copy too. */
   start();
   disk_put("/s/game.srm", file, 12);
   register_items(items, 1);
   event_load_save_files(false);
   items[0].num_segments = 1;
   register_items(items, 1);
   assert(save_item_files.files[0].guard.copy_first);
   assert(save_files());
   assert(disk_holds("/undo/game.srm", file, 12));
   assert(disk_holds("/s/game.srm", file, 8));
   ASSERT_FILES(1, 0, 0);

   /* A worker retired before its first write copies and cuts at its
    * last one, and the reap tells the copy. */
   start();
   disk_put("/s/game.srm", file, sizeof(file));
   items[0].num_segments = 2;
   register_items(items, 1);
   load_for_worker();
   start_autosave(false);
   logs     = copy_logs;
   items[1] = buffer_item("c", "/s/u.srm");
   register_items(items + 1, 1);
   for (i = 0; autosave_state.num_retired && i < 5000; i++)
   {
      autosave_check();
      retro_sleep(1);
   }
   assert(!autosave_state.num_retired);
   assert(disk_holds("/undo/game.srm", file, 16));
   assert(disk_holds("/s/game.srm", file, 12));
   assert(copy_logs == logs + 1);
   assert(strstr(copy_log, "\"/s/game.srm\" in \"/undo/game.srm\"; "
         "dropping its last 4 bytes"));
   autosave_deinit();

   /* A live write's copy not collected before the worker retires is
    * told with what that write dropped, not what its last one drops. */
   start();
   disk_put("/s/game.srm", file, sizeof(file));
   register_items(items, 1);
   load_for_worker();
   h       = start_autosave(false);
   wram[1] = 0x66;
   capture(h);
   assert(disk_holds("/undo/game.srm", file, 16));
   logs           = copy_logs;
   items[0].flags = 0;
   register_items(items, 1);
   assert(save_item_files.files[0].autosave == h);
   register_items(items + 1, 1);
   for (i = 0; autosave_state.num_retired && i < 5000; i++)
   {
      autosave_check();
      retro_sleep(1);
   }
   assert(!autosave_state.num_retired);
   assert(copy_logs == logs + 1);
   assert(strstr(copy_log, "dropping its last 4 bytes"));
   autosave_deinit();
}

/* Every read goes through rzip, which reads raw files too. */
static void test_rzip_read(void)
{
   unsigned char file[12];
   struct retro_save_item items[1];
   start();
   fill_bytes(file, sizeof(file), 1);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   assert(event_load_save_files(false) && rzip_reads == 1);
   assert(!memcmp(image(), file, 12));
}

/* A file blob_load() refused is copied before it is replaced, and is
 * never replaced without the copy. */
static void test_refused(void)
{
   static const unsigned char old[4] = {7, 7, 7, 7};
   struct retro_save_item items[1];

   start();
   disk_put("/s/game.blob", old, sizeof(old));
   items[0]              = buffer_item("blob", "/s/game.blob");
   items[0].storage      = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   items[0].segments     = NULL;
   items[0].num_segments = 0;
   blob_refuse           = true;
   register_items(items, 1);
   assert(!event_load_save_files(false));
   assert(osd_count == 1 && strstr(osd_last, "/s/game.blob"));
   blob_refuse = false;
   memset(blob_state, 3, 4);
   blob_len    = 4;
   assert(save_files());
   assert(disk_holds("/undo/game.blob", old, 4));
   assert(disk_holds("/s/game.blob", blob_state, 4));

   start();
   event_save_items_set_undo_dir(NULL);
   app_dir     = "/app";
   disk_put("/s/game.blob", old, sizeof(old));
   blob_refuse = true;
   register_items(items, 1);
   event_load_save_files(false);
   blob_refuse = false;
   memset(blob_state, 3, 4);
   blob_len    = 4;
   assert(!save_files());
   assert(disk_holds("/s/game.blob", old, 4));
   assert(disk_named("RetroArch-recovery-") && osd_count == 2);
   assert(strstr(osd_last, "/s/game.blob|/app/RetroArch-recovery-"));

   /* Once per item: the same image is not recovered again, and a newer
    * one replaces it in the file the warning named. */
   assert(!save_files());
   assert(recovery_writes == 1 && osd_count == 2);
   blob_state[0] = 4;
   assert(!save_files());
   assert(recovery_writes == 1 && osd_count == 2);
   assert(disk_named_holds("RetroArch-recovery-", blob_state, 4));
   assert(disk_holds("/s/game.blob", old, 4));
   /* No legacy memory: no type in the name. */
   assert(!disk_named("4294967295"));
}

/* Two recoveries in one second each get a file of their own, and each
 * item keeps its own current. */
static void test_recovery_same_second(void)
{
   static const unsigned char old[4] = {7, 7, 7, 7};
   struct retro_save_item items[2];
   unsigned k;

   start();
   event_save_items_set_undo_dir(NULL);
   app_dir = "/app";
   disk_put("/s/a.blob", old, sizeof(old));
   disk_put("/s/b.blob", old, sizeof(old));
   for (k = 0; k < 2; k++)
   {
      items[k] = buffer_item(k ? "b" : "a", k ? "/s/b.blob" : "/s/a.blob");
      items[k].storage      = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
      items[k].segments     = NULL;
      items[k].num_segments = 0;
   }
   blob_refuse = true;
   register_items(items, 2);
   assert(!event_load_save_files(false));
   blob_refuse = false;
   memset(blob_state, 3, 4);
   blob_len    = 4;
   assert(!save_files());
   assert(recovery_writes == 2);
   assert(disk_count_named("/app/RetroArch-recovery-") == 2);
   blob_state[0] = 4;
   assert(!save_files());
   assert(recovery_writes == 2);
   assert(disk_count_named("/app/RetroArch-recovery-") == 2);
   assert(disk_named_holds("/app/RetroArch-recovery-", blob_state, 4) == 2);
   assert(disk_holds("/s/a.blob", old, 4) && disk_holds("/s/b.blob", old, 4));
}

/* Loading disabled: nothing is read, so the first write shortens the
 * file as today, and no copy is taken. */
static void test_load_disabled_tail(void)
{
   unsigned char file[16];
   struct retro_save_item items[1];
   start();
   fill_bytes(file, sizeof(file), 0x30);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   event_load_save_files(true);
   wram[0] = 1;
   assert(save_files());
   assert(disk_holds("/s/game.srm", image(), 12) && !disk_named("/undo/"));
}

/* An old core's longer .srm is copied at load and the warning shows
 * once; the .srm is then written as before the interface. */
static void test_old_core_copy(void)
{
   static const unsigned char srm[6]   = {1, 2, 3, 4, 5, 6};
   static const unsigned char stale[4] = {9, 9, 9, 9};
   unsigned pass;
   start();
   path_init_savefile_rtc("/s/old.srm");
   disk_put("/s/old.srm", srm, sizeof(srm));
   assert(event_load_save_files(false));
   assert(!memcmp(legacy_ram, srm, 4));
   assert(disk_holds("/undo/old.srm", srm, 6));
   assert(osd_count == 1 && strstr(osd_last, "/undo/old.srm"));
   event_load_save_files(false);
   assert(osd_count == 1);
   legacy_ram[0] = 9;
   save_files();
   assert(disk_holds("/s/old.srm", legacy_ram, 4));
   assert(disk_holds("/undo/old.srm", srm, 6));
   ASSERT_FILES(1, 0, 1);
   assert(err_count == 0);

   /* Beside an older copy, and not again while the file is the same. */
   start();
   path_init_savefile_rtc("/s/old.srm");
   disk_put("/undo/old.srm", stale, sizeof(stale));
   disk_put("/s/old.srm", srm, sizeof(srm));
   assert(event_load_save_files(false));
   assert(disk_holds("/undo/old.srm", stale, sizeof(stale)));
   assert(disk_holds("/undo/old-2.srm", srm, 6));
   assert(strstr(osd_last, "/undo/old-2.srm"));
   assert(undo_writes == 1);
   event_load_save_files(false);
   assert(undo_writes == 1 && !disk_named("/undo/old-3"));
   ASSERT_FILES(2, 0, 1);
   assert(err_count == 0);

   /* No copy can be made: the whole file goes to a recovery file, and
    * the warning names it. Pass 0 with no undo folder, pass 1 with the
    * copy failing. */
   for (pass = 0; pass < 2; pass++)
   {
      start();
      app_dir = "/app";
      if (pass)
         set_fail_undo(true);
      else
         event_save_items_set_undo_dir(NULL);
      path_init_savefile_rtc("/s/old.srm");
      disk_put("/s/old.srm", srm, sizeof(srm));
      assert(event_load_save_files(false));
      set_fail_undo(false);
      assert(disk_named_holds("RetroArch-recovery-", srm, 6) == 1);
      ASSERT_FILES(0, 1, 1);
      assert(strstr(osd_last, "/s/old.srm|/app/RetroArch-recovery-"));
      assert(err_count == 0);
   }
}

/* A file that cannot be read is kept until it is copied: until then
 * each image goes to one recovery file, named once. */
static void test_unreadable_file(void)
{
   unsigned char file[12];
   struct retro_save_item items[1];

   start();
   app_dir = "/app";
   fill_bytes(file, sizeof(file), 0x70);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   set_fail_read("/s/game.srm");
   assert(!event_load_save_files(false));
   wram[0] = 1;
   save_files_unreaped();
   /* Killed here. */
   assert(disk_holds("/s/game.srm", file, 12));
   assert(disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
   ASSERT_FILES(0, 1, 1);
   assert(err_count == 1);
   frame_start_reap();
   assert(disk_holds("/s/game.srm", file, 12));
   assert(disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
   ASSERT_FILES(0, 1, 1);
   assert(err_count == 1);

   set_fail_read(NULL);
   wram[0] = 2;
   save_files();
   assert(disk_holds("/undo/game.srm", file, 12));
   assert(   disk_holds("/s/game.srm", image(), 12)
          || disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
   ASSERT_FILES(1, 1, 1);
   /* Written once the copy is made. */
   assert(save_files());
   assert(disk_holds("/s/game.srm", image(), 12));
   ASSERT_FILES(1, 1, 1);
}

/* SAVE_FILES before the frame start fills a grow: the grow is filled
 * first, so the file keeps what lies past the head. */
static void test_save_files_pending_grow(void)
{
   unsigned char file[16];
   unsigned char want[16];
   struct retro_save_item items[1];

   start();
   app_dir = "/app";
   fill_bytes(file, sizeof(file), 0x60);
   memcpy(want, file, sizeof(want));
   want[0] = 0x99;
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   assert(event_load_save_files(false));
   items[0].num_segments = 1;
   register_items(items, 1);
   memset(iram, 0xee, sizeof(iram));
   items[0].num_segments = 2;
   register_items(items, 1);
   assert(save_item_files.files[0].grow_from == 8);
   wram[0] = 0x99;
   assert(save_files());
   assert(disk_holds("/s/game.srm", want, 16));
   assert(!memcmp(iram, file + 8, sizeof(iram)));
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 0);
}

/* No memory to keep a file's end: the file is copied before the first
 * write cuts it, or kept with the image in a recovery file. */
static void test_tail_no_memory(void)
{
   unsigned char file[16];
   struct retro_save_item items[1];
   unsigned pass;

   for (pass = 0; pass < 2; pass++)
   {
      start();
      app_dir = "/app";
      if (pass)
         event_save_items_set_undo_dir(NULL);
      fill_bytes(file, sizeof(file), 0x50);
      disk_put("/s/game.srm", file, sizeof(file));
      items[0] = buffer_item("srm", "/s/game.srm");
      register_items(items, 1);
      retro_atomic_store_release_int(&fail_malloc_size, 4);
      assert(event_load_save_files(false));
      retro_atomic_store_release_int(&fail_malloc_size, 0);
      assert(!save_items_state_get_ptr()->current->items[0].tail_size);
      assert(err_count == 1);
      wram[0] = 1;
      save_files_unreaped();
      /* Killed here. */
      if (pass)
      {
         assert(disk_holds("/s/game.srm", file, 16));
         assert(disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
         ASSERT_FILES(0, 1, 1);
      }
      else
      {
         assert(disk_holds("/undo/game.srm", file, 16));
         assert(disk_holds("/s/game.srm", image(), 12));
         ASSERT_FILES(1, 0, 0);
      }
      assert(err_count == 1);
      frame_start_reap();
      assert(disk_holds("/s/game.srm", pass ? file : image(), pass ? 16 : 12));
      ASSERT_FILES(!pass, pass, pass);
      assert(err_count == 1);
   }
}

/* No memory to keep a closed item's image: it goes to a recovery
 * file at once. */
static void test_orphan_no_memory(void)
{
   struct retro_save_item items[1];

   start();
   app_dir  = "/app";
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   event_load_save_files(false);
   wram[0] = 1;
   retro_atomic_store_release_int(&fail_reallocs, 1);
   register_items(NULL, 0);
   retro_atomic_store_release_int(&fail_reallocs, 0);
   assert(!save_item_files.num_orphans);
   assert(disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
   ASSERT_FILES(0, 1, 1);
   assert(strstr(osd_last, "/s/game.srm|/app/RetroArch-recovery-"));
   assert(err_count == 0);
}

/* A failed write sends each newer image to the item's one recovery
 * file, named once. */
static void test_save_write_fails(void)
{
   unsigned char file[12];
   struct retro_save_item items[1];

   start();
   app_dir = "/app";
   fill_bytes(file, sizeof(file), 0x40);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   assert(event_load_save_files(false));
   set_fail_save_writes(true);
   wram[0] = 1;
   assert(!save_files());
   wram[0] = 2;
   assert(!save_files());
   set_fail_save_writes(false);
   assert(disk_holds("/s/game.srm", file, 12));
   assert(disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
   ASSERT_FILES(0, 1, 1);
   assert(err_count == 2);
}

/* A kill while a recovery file is written leaves no part of it under
 * a recovery file's name. */
static void test_recovery_cut(void)
{
   unsigned char file[12];
   struct retro_save_item items[1];

   start();
   app_dir = "/app";
   fill_bytes(file, sizeof(file), 0x40);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   assert(event_load_save_files(false));
   set_fail_save_writes(true);
   slock_lock(disk_lock);
   cut_recovery = true;
   slock_unlock(disk_lock);
   wram[0] = 1;
   assert(!save_files());
   assert(disk_count_named("RetroArch-recovery-") == 1);
   assert(disk_count_named(".tmp") == 1);
   assert(disk_holds("/s/game.srm", file, 12));
   assert(osd_count == 0 && err_count == 1);
}

/* The core gives no image to write: logged, and shown at the second
 * failure in a row when there is no memory for it. */
static void test_save_no_image(void)
{
   unsigned char file[12];
   struct retro_save_item items[1];

   start();
   app_dir = "/app";
   fill_bytes(file, sizeof(file), 0x40);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   assert(event_load_save_files(false));
   wram[0] = 1;
   retro_atomic_store_release_int(&fail_main_malloc_size, 12);
   assert(!save_files());
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 1);
   assert(!save_files());
   retro_atomic_store_release_int(&fail_main_malloc_size, 0);
   assert(disk_holds("/s/game.srm", file, 12));
   ASSERT_FILES(0, 0, 1);
   assert(err_count == 2);

   start();
   app_dir = "/app";
   disk_put("/s/game.flash", "abcd", 4);
   memset(&items[0], 0, sizeof(items[0]));
   items[0].id            = "flash";
   items[0].path          = "/s/game.flash";
   items[0].storage       = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   items[0].root          = RETRO_SAVE_ROOT_ABSOLUTE;
   items[0].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   register_items(items, 1);
   assert(event_load_save_files(false));
   blob_state[0]  = 'z';
   blob_fail_save = true;
   assert(!save_files());
   blob_fail_save = false;
   assert(disk_holds("/s/game.flash", "abcd", 4));
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 1);
}

/* A worker's failed write is reported from the main thread. */
static void test_worker_failed(void)
{
   struct retro_save_item items[1];
   autosave_t *h;
   start();
   app_dir  = "/app";
   items[0] = buffer_item("srm", "/s/auto.srm");
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   /* One failed write that the retry fixes is not told. */
   set_fail_writes(true);
   wram[0] = 1;
   capture(h);
   assert(retro_atomic_load_acquire_int(&h->failed));
   event_save_items_fill(false);
   assert(osd_count == 0 && err_count == 0);
   set_fail_writes(false);
   capture(h);
   assert(!retro_atomic_load_acquire_int(&h->failed));
   assert(disk_holds("/s/auto.srm", image(), 12));
   event_save_items_fill(false);
   assert(osd_count == 0 && err_count == 0);
   /* The second in a row is, once. */
   set_fail_writes(true);
   wram[0] = 2;
   capture(h);
   capture(h);
   assert(!save_item_files.files[0].failed_told);
   event_save_items_fill(false);
   assert(save_item_files.files[0].failed_told);
   assert(osd_count == 1 && strstr(osd_last, "\"/s/auto.srm\""));
   assert(err_count == 1 && strstr(err_last, "\"/s/auto.srm\""));
   capture(h);
   event_save_items_fill(false);
   assert(osd_count == 1 && err_count == 1);
   /* A success ends it; the next streak is told again. */
   set_fail_writes(false);
   capture(h);
   event_save_items_fill(false);
   assert(!save_item_files.files[0].failed_told);
   set_fail_writes(true);
   wram[0] = 3;
   capture(h);
   capture(h);
   event_save_items_fill(false);
   assert(osd_count == 2 && err_count == 2);
   set_fail_writes(false);
   autosave_deinit();
   ASSERT_FILES(0, 0, 2);
}

/* A worker's failed writes and SAVE_FILES's failures to save the same
 * file are one streak, shown once whichever is shown first; a success
 * ends it. */
static void test_worker_and_main_failed(void)
{
   struct retro_save_item items[1];
   autosave_t *h;
   start();
   items[0] = buffer_item("srm", "/s/auto.srm");
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   set_fail_writes(true);
   wram[0] = 1;
   capture(h);
   capture(h);
   event_save_items_fill(false);
   assert(osd_count == 1 && err_count == 1);
   retro_atomic_store_release_int(&fail_main_malloc_size, 12);
   assert(!event_save_files(true, false, NULL));
   assert(!event_save_files(true, false, NULL));
   retro_atomic_store_release_int(&fail_main_malloc_size, 0);
   assert(osd_count == 1 && err_count == 3);
   set_fail_writes(false);
   capture(h);
   event_save_items_fill(false);
   assert(event_save_files(true, false, NULL));
   assert(disk_holds("/s/auto.srm", image(), 12));
   assert(osd_count == 1 && err_count == 3);
   /* SAVE_FILES first this time. */
   wram[0] = 2;
   retro_atomic_store_release_int(&fail_main_malloc_size, 12);
   assert(!event_save_files(true, false, NULL));
   assert(!event_save_files(true, false, NULL));
   retro_atomic_store_release_int(&fail_main_malloc_size, 0);
   assert(osd_count == 2 && err_count == 5);
   set_fail_writes(true);
   capture(h);
   capture(h);
   event_save_items_fill(false);
   assert(osd_count == 2 && err_count == 6);
   set_fail_writes(false);
   assert(event_save_files(true, false, NULL));
   assert(disk_holds("/s/auto.srm", image(), 12));
   autosave_deinit();
   ASSERT_FILES(0, 0, 2);
   assert(err_count == 6);
}

/* An item closed before its fill writes nothing: removed, or ended
 * by a refused set. */
static void test_close_unfilled(void)
{
   static const unsigned char saved[6] = {'S', 'A', 'V', 'E', 'D', '!'};
   struct retro_save_item items[2];
   struct retro_save_item twice[2];

   start();
   app_dir = "/app";
   disk_put("/s/one.card", saved, sizeof(saved));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   event_load_save_files(false);
   items[1] = card_item("card", "/s/one.card");
   register_items(items, 2);
   register_items(items, 1);
   assert(save_item_files.num_orphans == 0);
   assert(save_files());
   assert(raw_writes == 0 && disk_holds("/s/one.card", saved, 6));

   register_items(items, 2);
   twice[0] = buffer_item("a", "/s/x.srm");
   twice[1] = card_item("a", "/s/y.srm");
   register_refused(twice, 2);
   assert(save_item_files.num_orphans == 1);
   assert(save_files());
   assert(raw_writes == 0 && disk_holds("/s/one.card", saved, 6));
   ASSERT_FILES(0, 0, 0);
}

/* Grown, then closed before the fill: the file as it was, with the
 * current head, so nothing past it moves. */
static void test_close_pending_grow(void)
{
   unsigned char file[16];
   unsigned char want[16];
   struct retro_save_item items[1];
   unsigned i;

   fill_bytes(file, sizeof(file), 0x60);
   memcpy(want, file, sizeof(want));
   want[0] = 0x99;
   for (i = 0; i < 3; i++)
   {
      start();
      disk_put("/s/game.srm", file, sizeof(file));
      items[0]       = buffer_item("srm", "/s/game.srm");
      items[0].flags = i == 2 ? RETRO_SAVE_FLAG_EXACT_SIZE : 0;
      register_items(items, 1);
      assert(event_load_save_files(false));
      /* Pass 1: its worker writes the last image. */
      if (i == 1)
         start_autosave(false);
      items[0].num_segments = 1;
      register_items(items, 1);
      memset(iram, 0xee, sizeof(iram));
      items[0].num_segments = 2;
      register_items(items, 1);
      assert(save_item_files.files[0].grow_from == 8);
      wram[0] = 0x99;
      register_items(NULL, 0);
      if (i == 1)
      {
         unsigned k;
         for (k = 0; autosave_state.num_retired && k < 5000; k++)
         {
            autosave_check();
            retro_sleep(1);
         }
         assert(!autosave_state.num_retired);
         autosave_deinit();
      }
      assert(save_files());
      /* EXACT_SIZE: copied, then cut to the item as its fill would
       * have left it. */
      if (i == 2)
         assert(disk_holds("/undo/game.srm", file, 16)
               && disk_holds("/s/game.srm", want, 12));
      else
         assert(disk_holds("/s/game.srm", want, 16));
   }
}

/* Several re-registrations before the fill: the new bytes come from
 * the tail's front where the first grow began, and a cut drops the
 * bytes the fill never wrote. */
static void test_reshapes_before_fill(void)
{
   unsigned char file[16];
   struct retro_save_item items[1];
   unsigned i;

   fill_bytes(file, sizeof(file), 0x60);
   for (i = 0; i < 3; i++)
   {
      start();
      disk_put("/s/game.srm", file, sizeof(file));
      items[0] = buffer_item("srm", "/s/game.srm");
      register_items(items, 1);
      assert(event_load_save_files(false));
      items[0].num_segments = 1;
      register_items(items, 1);
      memset(iram, 0xee, sizeof(iram));
      segs[1].size          = 2;
      items[0].num_segments = 2;
      register_items(items, 1);
      if (i == 0)
         segs[1].size = 4;
      else if (i == 1)
         segs[1].size = 1;
      else
      {
         segs[0].size          = 6;
         items[0].num_segments = 1;
      }
      register_items(items, 1);
      event_save_items_fill(false);
      assert(!save_item_files.files[0].grow_from);
      assert(save_files());
      assert(raw_writes == 0 && disk_holds("/s/game.srm", file, 16));
      segs[0].size = sizeof(wram);
      segs[1].size = sizeof(iram);
   }
}

/* A cut that owes a copy while the worker is making one gets its own
 * copy: the one in progress may predate it. */
static void test_copy_owed_again(void)
{
   unsigned char file[16];
   unsigned char want[12];
   struct retro_save_item items[1];
   autosave_t *h;

   start();
   fill_bytes(file, sizeof(file), 0x20);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0]       = buffer_item("srm", "/s/game.srm");
   items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   register_items(items, 1);
   load_for_worker();
   h = start_autosave(false);
   /* The copy the longer file owes is made at the start. */
   assert(disk_holds("/undo/game.srm", file, 16));

   /* The worker is held inside a write when the cut owes a copy. */
   slock_lock(disk_lock);
   hold_write = true;
   slock_unlock(disk_lock);
   iram[0] = 0x77;
   memcpy(want, image(), sizeof(want));
   autosave_lock();
   autosave_unlock();
   slock_lock(disk_lock);
   disk_wait_count(&raw_writes, 2);
   assert(raw_writes == 2);
   slock_unlock(disk_lock);

   items[0].num_segments = 1;
   register_items(items, 1);
   slock_lock(disk_lock);
   hold_write = false;
   scond_broadcast(disk_cond);
   slock_unlock(disk_lock);
   wait_requested(h);
   assert(disk_holds("/s/game.srm", want, 12));

   /* Once that write has landed, the file holding the cut bytes is
    * copied next to the first copy, before the next write. */
   assert(disk_holds("/undo/game.srm", file, 16));
   assert(disk_holds("/undo/game-2.srm", want, 12));
   capture(h);
   assert(disk_holds("/s/game.srm", want, 8));
   event_save_items_fill(false);
   assert(copy_logged("in \"/undo/game-2.srm\""));
   autosave_deinit();
}

/* After the copy a type change owed, a cut whose copy fails keeps the
 * end this time in the worker too, as SAVE_FILES does. */
static void test_copy_then_cut(void)
{
   unsigned char file[12];
   struct retro_save_item items[1];
   autosave_t *h;

   start();
   app_dir = "/app";
   fill_bytes(file, sizeof(file), 0x30);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = card_item("a", "/s/game.srm");
   register_items(items, 1);
   event_load_save_files(false);
   items[0]       = buffer_item("b", "/s/game.srm");
   items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   register_items(items, 1);
   assert(save_item_files.files[0].guard.keep_file);
   event_save_items_fill(false);
   h = start_autosave(false);
   wram[0] ^= 0xff;
   capture(h);
   assert(disk_holds("/undo/game.srm", file, 12));
   assert(disk_holds("/s/game.srm", image(), 12));
   event_save_items_fill(false);
   assert(!save_item_files.files[0].guard.keep_file);
   ASSERT_FILES(1, 0, 1);

   items[0].num_segments = 1;
   register_items(items, 1);
   set_fail_undo(true);
   wram[1] ^= 0xff;
   capture(h);
   assert(!retro_atomic_load_acquire_int(&h->failed));
   assert(disk_holds("/s/game.srm", image(), 12));
   ASSERT_FILES(1, 0, 1);
   assert(err_count == 0);
   set_fail_undo(false);
   autosave_deinit();
}

/* No file is no copy: the guard is met, and no copy is announced,
 * not even an older one of the same name. Pass 0 SAVE_FILES, pass 1
 * the worker. */
static void test_copy_no_file(void)
{
   static const unsigned char stale[4] = {9, 9, 9, 9};
   struct retro_save_item items[1];
   unsigned pass;

   for (pass = 0; pass < 2; pass++)
   {
      start();
      disk_put("/undo/game.srm", stale, sizeof(stale));
      items[0] = card_item("a", "/s/game.srm");
      register_items(items, 1);
      event_load_save_files(false);
      items[0] = buffer_item("b", "/s/game.srm");
      register_items(items, 1);
      event_save_items_fill(false);
      assert(save_item_files.files[0].guard.keep_file);
      if (pass == 0)
      {
         wram[0] = 1;
         assert(save_files());
      }
      else
      {
         autosave_t *h = start_autosave(false);
         wram[0]       = 1;
         capture(h);
         event_save_items_fill(false);
      }
      assert(osd_count == 0);
      assert(!save_item_files.files[0].guard.copy_first);
      assert(!save_item_files.files[0].guard.keep_file);
      assert(disk_holds("/s/game.srm", image(), 12));
      assert(disk_holds("/undo/game.srm", stale, sizeof(stale)));
      autosave_deinit();
   }
}

/* A copy never replaces an earlier one: it takes the next free -2 ..
 * -99 name, and none is written when a copy already holds the bytes.
 * The log names the copy. Pass 0 two copies by SAVE_FILES; 1 the
 * bytes already in game-2.srm; 2 the same in the worker. */
static void test_copy_numbered(void)
{
   static const unsigned char stale[4] = {9, 9, 9, 9};
   unsigned char file[16];
   struct retro_save_item items[1];
   unsigned pass;

   fill_bytes(file, sizeof(file), 0x40);
   for (pass = 0; pass < 3; pass++)
   {
      start();
      disk_put("/s/game.srm", file, sizeof(file));
      if (pass)
      {
         disk_put("/undo/game.srm", stale, sizeof(stale));
         disk_put("/undo/game-2.srm", file, sizeof(file));
      }
      items[0]       = buffer_item("srm", "/s/game.srm");
      items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
      register_items(items, 1);
      event_load_save_files(false);
      if (pass < 2)
         assert(save_files());
      else
      {
         autosave_t *h = start_autosave(false);
         wram[0]      ^= 0xff;
         capture(h);
         event_save_items_fill(false);
      }
      assert(disk_holds("/s/game.srm", image(), 12));
      if (pass)
      {
         assert(undo_writes == 0);
         assert(disk_holds("/undo/game.srm", stale, sizeof(stale)));
         assert(copy_logged("in \"/undo/game-2.srm\""));
         autosave_deinit();
         continue;
      }
      assert(disk_holds("/undo/game.srm", file, 16));
      assert(copy_logged("in \"/undo/game.srm\""));
      items[0].num_segments = 1;
      register_items(items, 1);
      assert(save_files());
      assert(disk_holds("/undo/game.srm", file, 16));
      assert(disk_holds("/undo/game-2.srm", file, 12));
      assert(disk_holds("/s/game.srm", file, 8));
      assert(copy_logged("in \"/undo/game-2.srm\""));
      assert(undo_writes == 2);
   }
}

/* Past -99 the copy takes the -99 name, as recovery files do. */
static void test_copy_numbered_full(void)
{
   unsigned char file[16];
   unsigned char other[4];
   char name[32];
   struct retro_save_item items[1];
   unsigned n;

   start();
   for (n = 1; n < 100; n++)
   {
      if (n == 1)
         strlcpy(name, "/undo/game.srm", sizeof(name));
      else
         snprintf(name, sizeof(name), "/undo/game-%u.srm", n);
      fill_bytes(other, sizeof(other), n);
      disk_put(name, other, sizeof(other));
   }
   fill_bytes(file, sizeof(file), 0x60);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0]       = buffer_item("srm", "/s/game.srm");
   items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   register_items(items, 1);
   event_load_save_files(false);
   assert(save_files());
   assert(disk_holds("/undo/game-99.srm", file, 16));
   fill_bytes(other, sizeof(other), 98);
   assert(disk_holds("/undo/game-98.srm", other, sizeof(other)));
   assert(!disk_named("/undo/game-100"));
   assert(copy_logged("in \"/undo/game-99.srm\""));
}

/* A type change's copy beside an older one: the warning names the
 * file written. Pass 0 SAVE_FILES, pass 1 the worker. */
static void test_copy_numbered_type_change(void)
{
   static const unsigned char stale[4] = {9, 9, 9, 9};
   unsigned char file[12];
   struct retro_save_item items[1];
   unsigned pass;

   fill_bytes(file, sizeof(file), 0x70);
   for (pass = 0; pass < 2; pass++)
   {
      start();
      disk_put("/undo/game.srm", stale, sizeof(stale));
      disk_put("/s/game.srm", file, sizeof(file));
      items[0] = card_item("a", "/s/game.srm");
      register_items(items, 1);
      event_load_save_files(false);
      items[0] = buffer_item("b", "/s/game.srm");
      register_items(items, 1);
      event_save_items_fill(false);
      if (pass == 0)
      {
         wram[0] ^= 0xff;
         assert(save_files());
      }
      else
      {
         autosave_t *h = start_autosave(false);
         wram[0]      ^= 0xff;
         capture(h);
         event_save_items_fill(false);
      }
      assert(disk_holds("/undo/game.srm", stale, sizeof(stale)));
      assert(disk_holds("/undo/game-2.srm", file, sizeof(file)));
      assert(disk_holds("/s/game.srm", image(), 12));
      assert(osd_count == 1);
      assert(strstr(osd_last, "/s/game.srm|/undo/game-2.srm"));
      autosave_deinit();
   }
}

/* SAVE_FILES while the item's own worker makes the copy it owes:
 * the newest image goes to a recovery file no one is told of, gone
 * once the path holds it, named when the copy fails. */
static void test_save_files_worker_copying(void)
{
   unsigned char file[12];
   unsigned char newest[12];
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned pass;

   fill_bytes(file, sizeof(file), 0x70);
   for (pass = 0; pass < 2; pass++)
   {
      start();
      app_dir = "/app";
      disk_put("/s/game.srm", file, sizeof(file));
      items[0] = card_item("a", "/s/game.srm");
      register_items(items, 1);
      event_load_save_files(false);
      items[0] = buffer_item("b", "/s/game.srm");
      register_items(items, 1);
      event_save_items_set_undo_dir(NULL);
      event_save_items_fill(false);
      event_save_items_set_undo_dir("/undo");
      assert(save_item_files.files[0].guard.keep_file);
      set_hold(&hold_undo, true);
      assert(autosave_init(false, 1));
      assert((h = save_item_files.files[0].autosave));
      h->interval = 0;
      wait_held(1);
      wram[0] ^= 0xff;
      memcpy(newest, image(), sizeof(newest));
      assert(save_files_unreaped());
      /* Killed here. */
      assert(disk_holds("/s/game.srm", file, sizeof(file)));
      assert(disk_named_holds("RetroArch-recovery-", newest, 12) == 1);
      ASSERT_FILES(0, 1, 0);
      assert(err_count == 0);
      if (pass)
         set_fail_undo(true);
      set_hold(&hold_undo, false);
      wait_requested(h);
      /* Killed here. */
      assert(disk_holds("/s/game.srm", file, sizeof(file)));
      assert(disk_named_holds("RetroArch-recovery-", newest, 12) == 1);
      /* Pass 1: the worker's dump of the old type's image too. */
      ASSERT_FILES(pass ? 0 : 1, pass ? 2 : 1, 0);
      assert(err_count == 0);
      if (pass == 0)
      {
         assert(disk_holds("/undo/game.srm", file, sizeof(file)));
         event_save_items_fill(false);
         capture(h);
         /* Killed here. */
         assert(disk_holds("/s/game.srm", newest, 12));
         ASSERT_FILES(1, 0, 1);
         event_save_items_fill(false);
         assert(disk_holds("/s/game.srm", newest, 12));
         ASSERT_FILES(1, 0, 1);
         assert(strstr(osd_last, "/s/game.srm|/undo/game.srm"));
      }
      else
      {
         event_save_items_fill(false);
         ASSERT_FILES(0, 2, 2);
         assert(told_holding(newest, 12) == 1);
         assert(told_holding(file, sizeof(file)) == 1);
         capture(h);
         event_save_items_fill(false);
         /* Still no copy: the image goes to the file named. */
         assert(!save_files());
         ASSERT_FILES(0, 2, 2);
         wram[0] ^= 0x0f;
         assert(!save_files());
         assert(disk_holds("/s/game.srm", file, sizeof(file)));
         ASSERT_FILES(0, 2, 2);
         assert(told_holding(image(), 12) == 1);
         set_fail_undo(false);
      }
      assert(err_count == 0);
      autosave_deinit();
   }
}

/* An item closed while its worker owes the copy of its kept file: the
 * recovery file no one was told of keeps the newest image until the
 * worker's last write lands it, then goes. Pass 1: that write fails,
 * and the file is named once. Pass 2: the content closes. */
static void test_close_worker_copying(void)
{
   unsigned char file[12];
   unsigned char newest[12];
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned pass;

   fill_bytes(file, sizeof(file), 0x30);
   for (pass = 0; pass < 3; pass++)
   {
      start();
      app_dir = "/app";
      disk_put("/s/game.srm", file, sizeof(file));
      items[0] = card_item("a", "/s/game.srm");
      register_items(items, 1);
      event_load_save_files(false);
      items[0] = buffer_item("b", "/s/game.srm");
      register_items(items, 1);
      event_save_items_set_undo_dir(NULL);
      event_save_items_fill(false);
      event_save_items_set_undo_dir("/undo");
      assert(save_item_files.files[0].guard.keep_file);
      set_hold(&hold_undo, true);
      assert(autosave_init(false, 1));
      assert((h = save_item_files.files[0].autosave));
      h->interval = 0;
      wait_held(1);
      wram[0] ^= 0xff;
      memcpy(newest, image(), sizeof(newest));
      assert(save_files_unreaped());
      register_items(NULL, 0);
      /* Killed here. */
      assert(disk_holds("/s/game.srm", file, sizeof(file)));
      assert(disk_named_holds("RetroArch-recovery-", newest, 12) == 1);
      ASSERT_FILES(0, 1, 0);
      assert(err_count == 0);
      if (pass == 1)
         set_fail_save_writes(true);
      set_hold(&hold_undo, false);
      if (pass == 2)
      {
         event_save_items_final(true, false);
         assert(disk_holds("/s/game.srm", newest, 12));
         assert(disk_holds("/undo/game.srm", file, sizeof(file)));
         ASSERT_FILES(1, 0, 1);
         assert(err_count == 0);
         autosave_deinit();
         continue;
      }
      retired_join(false);
      set_fail_save_writes(false);
      /* Killed here. */
      assert(disk_holds("/undo/game.srm", file, sizeof(file)));
      assert(disk_holds("/s/game.srm", pass ? file : newest, 12));
      assert(disk_named_holds("RetroArch-recovery-", newest, 12) == pass);
      ASSERT_FILES(1, pass, 0);
      assert(err_count == 0);
      autosave_reap(false);
      assert(!autosave_state.num_retired);
      assert(disk_holds("/s/game.srm", pass ? file : newest, 12));
      ASSERT_FILES(1, pass, 1 + pass);
      assert(told_holding(newest, 12) == pass);
      assert(err_count == 0);
      autosave_deinit();
   }
}

/* Two items of one name in two folders: each file gets its own copy,
 * one at a time. */
static void test_copy_same_name(void)
{
   unsigned char a[16];
   unsigned char b[10];
   struct retro_save_item items[2];
   autosave_t *h0, *h1;
   unsigned i;

   start();
   app_dir = "/app";
   fill_bytes(a, sizeof(a), 0x20);
   fill_bytes(b, sizeof(b), 0x90);
   disk_put("/s/a/g.srm", a, sizeof(a));
   disk_put("/s/b/g.srm", b, sizeof(b));
   items[0]       = buffer_item("a", "/s/a/g.srm");
   items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   items[1]       = card_item("b", "/s/b/g.srm");
   items[1].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   register_items(items, 2);
   load_for_worker();
   assert(save_item_files.files[0].guard.copy_first);
   assert(save_item_files.files[1].guard.copy_first);
   set_hold(&hold_undo, true);
   assert(autosave_init(false, 1));
   assert((h0 = save_item_files.files[0].autosave));
   assert((h1 = save_item_files.files[1].autosave));
   h0->interval = h1->interval = 0;
   wait_held(1);
   /* The other tries the same name meanwhile, then asks for a
    * snapshot. */
   for (i = 0; i < 5000; i++)
   {
      if (     retro_atomic_load_acquire_int(&h0->snapshot_requested)
            || retro_atomic_load_acquire_int(&h1->snapshot_requested))
         break;
      retro_sleep(1);
   }
   assert(i < 5000);
   set_hold(&hold_undo, false);
   wait_requested(h0);
   wait_requested(h1);
   wram[0] = 1;
   card[0] = 1;
   capture(h0);
   capture(h1);
   capture(h0);
   capture(h1);
   assert(disk_named_holds("/undo/", a, sizeof(a)) == 1);
   assert(disk_named_holds("/undo/", b, sizeof(b)) == 1);
   assert(!disk_named(".tmp"));
   ASSERT_FILES(2, 0, 0);
   assert(err_count == 0);
   autosave_deinit();
}

/* An item empty when filled gets its worker at a frame start once it
 * has bytes; nothing calls the core inside a registration. */
static void test_start_after_empty(void)
{
   struct retro_save_item items[1];
   start();
   memset(&items[0], 0, sizeof(items[0]));
   items[0].id            = "flash";
   items[0].path          = "/s/game.flash";
   items[0].storage       = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   items[0].root          = RETRO_SAVE_ROOT_ABSOLUTE;
   items[0].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   use_generation         = true;
   register_items(items, 1);
   event_load_save_files(false);
   assert(autosave_init(false, 1));
   assert(!save_item_files.files[0].autosave);

   memcpy(blob_state, "data", 4);
   blob_len = 4;
   register_items(items, 1);
   assert(core_calls_in_set == 0);
   assert(!save_item_files.files[0].autosave);
   event_save_items_fill(false);
   assert(save_item_files.files[0].autosave);
   autosave_deinit();
}

/* A worker that could not start is tried again when the item's
 * generation moves, or at its next registration; never every frame
 * for an item that has not changed. Pass 1: no get_generation. */
static void test_start_retry(void)
{
   struct retro_save_item items[1];
   unsigned pass, i, saves;

   for (pass = 0; pass < 2; pass++)
   {
      start();
      memset(&items[0], 0, sizeof(items[0]));
      items[0].id            = "flash";
      items[0].path          = "/s/game.flash";
      items[0].storage       = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
      items[0].root          = RETRO_SAVE_ROOT_ABSOLUTE;
      items[0].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
      use_generation         = !pass;
      memcpy(blob_state, "data", 4);
      blob_len               = 4;
      register_items(items, 1);
      event_load_save_files(false);
      blob_fail_save         = true;
      saves                  = blob_saves;
      assert(autosave_init(false, 1));
      assert(!save_item_files.files[0].autosave);
      assert(blob_saves == saves + 1);
      for (i = 0; i < 3; i++)
         event_save_items_fill(false);
      assert(blob_saves == saves + 1);
      generation++;
      event_save_items_fill(false);
      event_save_items_fill(false);
      assert(blob_saves == saves + (pass ? 1 : 2));
      assert(!save_item_files.files[0].autosave);

      blob_fail_save = false;
      register_items(items, 1);
      assert(core_calls_in_set == 0);
      event_save_items_fill(false);
      assert(save_item_files.files[0].autosave);
      autosave_deinit();
   }
}

/* An item empty when filled is tried again once its generation
 * moves; without get_generation, at every frame start. Pass 1: none. */
static void test_empty_retry(void)
{
   struct retro_save_item items[1];
   unsigned pass, i, sizes;

   for (pass = 0; pass < 2; pass++)
   {
      start();
      memset(&items[0], 0, sizeof(items[0]));
      items[0].id            = "flash";
      items[0].path          = "/s/game.flash";
      items[0].storage       = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
      items[0].root          = RETRO_SAVE_ROOT_ABSOLUTE;
      items[0].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
      use_generation         = !pass;
      register_items(items, 1);
      event_load_save_files(false);
      assert(autosave_init(false, 1));
      assert(!save_item_files.files[0].autosave);
      sizes = blob_sizes;
      for (i = 0; i < 3; i++)
         event_save_items_fill(false);
      assert(blob_sizes == sizes + (pass ? 3 : 0));
      generation++;
      event_save_items_fill(false);
      event_save_items_fill(false);
      assert(blob_sizes == sizes + (pass ? 5 : 1));
      memcpy(blob_state, "data", 4);
      blob_len = 4;
      generation++;
      event_save_items_fill(false);
      assert(save_item_files.files[0].autosave);
      autosave_deinit();
   }
}

/* A settings change gives a worker that could not start one fresh
 * try. Pass 1: no get_generation. */
static void test_start_retry_settings(void)
{
   struct retro_save_item items[1];
   unsigned pass, saves;

   for (pass = 0; pass < 2; pass++)
   {
      start();
      memset(&items[0], 0, sizeof(items[0]));
      items[0].id            = "flash";
      items[0].path          = "/s/game.flash";
      items[0].storage       = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
      items[0].root          = RETRO_SAVE_ROOT_ABSOLUTE;
      items[0].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
      use_generation         = !pass;
      memcpy(blob_state, "data", 4);
      blob_len               = 4;
      register_items(items, 1);
      event_load_save_files(false);
      blob_fail_save         = true;
      assert(autosave_init(false, 1));
      assert(!save_item_files.files[0].autosave);
      saves = blob_saves;
      autosave_deinit();
      assert(autosave_init(false, 2));
      assert(blob_saves == saves + 1);
      event_save_items_fill(false);
      assert(blob_saves == saves + 1);
      assert(!save_item_files.files[0].autosave);

      blob_fail_save = false;
      autosave_deinit();
      assert(autosave_init(false, 1));
      assert(save_item_files.files[0].autosave);
      autosave_deinit();
   }
}

/* Stopping a worker never allocates, so one is never lost: its slot
 * is kept from its start, and a start without one waits for the next
 * frame start. */
static void test_retire_no_memory(void)
{
   struct retro_save_item items[1];
   unsigned i;

   start();
   items[0] = buffer_item("srm", "/s/retire.srm");
   register_items(items, 1);
   event_load_save_files(false);
   start_autosave(false);
   wram[0] = 1;
   retro_atomic_store_release_int(&fail_reallocs, 1);
   register_items(NULL, 0);
   retro_atomic_store_release_int(&fail_reallocs, 0);
   assert(autosave_state.num_retired == 1);
   for (i = 0; autosave_state.num_retired && i < 5000; i++)
   {
      autosave_check();
      retro_sleep(1);
   }
   assert(autosave_state.num_retired == 0);
   assert(disk_holds("/s/retire.srm", image(), 12));
   autosave_deinit();

   start();
   register_items(items, 1);
   event_load_save_files(false);
   retro_atomic_store_release_int(&fail_reallocs, 1);
   assert(autosave_init(false, 1));
   retro_atomic_store_release_int(&fail_reallocs, 0);
   assert(!save_item_files.files[0].autosave);
   event_save_items_fill(false);
   assert(save_item_files.files[0].autosave);
   autosave_deinit();
}

/* SAVE_FILES while the worker's older image waits to be renamed:
 * each writes its own temp, and the older one is dropped. The file
 * then holds what the worker did not write, so its next write goes
 * ahead even when the item went back to what it last wrote. */
static void test_save_files_mid_write(void)
{
   unsigned char newer[12];
   struct retro_save_item items[1];
   autosave_t *h;

   start();
   app_dir  = "/app";
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);

   set_hold(&hold_temp, true);
   wram[0] = 1;
   autosave_lock();
   autosave_unlock();
   wait_held(1);
   wram[0] = 2;
   memcpy(newer, image(), sizeof(newer));
   assert(save_files());
   assert(disk_holds("/s/game.srm", newer, 12));
   set_hold(&hold_temp, false);
   wait_requested(h);
   assert(disk_holds("/s/game.srm", newer, 12));
   assert(!disk_named(".tmp"));
   assert(!retro_atomic_load_acquire_int(&h->failed));
   assert(failed_renames == 0 && osd_count == 0);
   wram[0] = 0;
   capture(h);
   assert(disk_holds("/s/game.srm", image(), 12));
   ASSERT_FILES(0, 0, 0);
   autosave_deinit();
}

/* SAVE_FILES while the worker renames an older image. Pass 0, before
 * the move: the older image lands last, and the next write replaces
 * it. Pass 1, after it: the newer lands last, and the next write goes
 * ahead even when the item went back to what the worker wrote. */
static void test_save_files_at_rename(void)
{
   unsigned char older[12];
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned pass;

   for (pass = 0; pass < 2; pass++)
   {
      bool *hold = pass ? &hold_renamed : &hold_rename;
      start();
      app_dir  = "/app";
      items[0] = buffer_item("srm", "/s/game.srm");
      register_items(items, 1);
      event_load_save_files(false);
      h = start_autosave(false);

      set_hold(hold, true);
      wram[0] = 1;
      memcpy(older, image(), sizeof(older));
      autosave_lock();
      autosave_unlock();
      wait_held(1);
      wram[0] = 2;
      assert(save_files());
      assert(disk_holds("/s/game.srm", image(), 12));
      set_hold(hold, false);
      wait_requested(h);
      assert(!retro_atomic_load_acquire_int(&h->failed));
      assert(failed_renames == 0 && !disk_named(".tmp"));
      /* Killed here: the newer image is written last, with no frame start. */
      assert(disk_holds("/s/game.srm", image(), 12));
      ASSERT_FILES(0, 0, 0);
      assert(err_count == 0);
      if (pass)
         wram[0] = 1;
      capture(h);
      assert(disk_holds("/s/game.srm", image(), 12));
      autosave_deinit();
   }
}

/* An item retired with its last image held before the rename, and
 * the same file registered again. */
static void retire_held(struct retro_save_item *items,
      unsigned char *last)
{
   start();
   app_dir = "/app";
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   event_load_save_files(false);
   start_autosave(false);
   set_hold(&hold_temp, true);
   wram[0] = 1;
   memcpy(last, image(), 12);
   register_items(NULL, 0);
   wait_held(1);
   register_items(items, 1);
   event_save_items_fill(false);
}

/* One worker per file: the new item's starts once the retired one has
 * exited. Its fill took the retired image, which that worker still
 * writes. */
static void test_retired_then_new_item(void)
{
   unsigned char last[12];
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned i;

   retire_held(items, last);
   for (i = 0; i < 3; i++)
   {
      autosave_check();
      event_save_items_fill(false);
      assert(!save_item_files.files[0].autosave);
   }
   set_hold(&hold_temp, false);
   retired_join(false);
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 0);
   for (i = 0; !save_item_files.files[0].autosave && i < 5000; i++)
   {
      autosave_check();
      event_save_items_fill(false);
      retro_sleep(1);
   }
   assert((h = save_item_files.files[0].autosave));
   assert(disk_holds("/s/game.srm", last, 12));
   assert(!memcmp(image(), last, 12));
   h->interval = 0;
   wait_requested(h);
   wram[1] = 5;
   capture(h);
   assert(disk_holds("/s/game.srm", image(), 12));
   assert(failed_renames == 0 && !disk_named(".tmp"));
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 0);
   autosave_deinit();
}

/* SAVE_FILES writes the new item while the retired worker's last
 * image waits to be renamed: that image is dropped. */
static void test_save_files_during_retired_write(void)
{
   unsigned char last[12];
   struct retro_save_item items[1];
   unsigned i;

   retire_held(items, last);
   wram[0] = 2;
   assert(save_files());
   set_hold(&hold_temp, false);
   retired_join(false);
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 0);
   for (i = 0; autosave_state.num_retired && i < 5000; i++)
   {
      autosave_check();
      retro_sleep(1);
   }
   assert(autosave_state.num_retired == 0);
   assert(disk_holds("/s/game.srm", image(), 12));
   assert(failed_renames == 0 && !disk_named(".tmp"));
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 0);
   autosave_deinit();
}

static unsigned raw_writes_now(void)
{
   unsigned n;
   slock_lock(disk_lock);
   n = raw_writes;
   slock_unlock(disk_lock);
   return n;
}

/* A four-byte item at the .srm in save RAM's place, written once by its
 * worker, then gone from the set while that worker is held writing its
 * last image: @older on disk, @last in the temp. */
static void given_back_held(struct retro_save_item *items,
      struct retro_save_segment *seg, unsigned char *older,
      unsigned char *last)
{
   autosave_t *h;
   start();
   app_dir = "/app";
   path_init_savefile_rtc("/s/game.srm");
   seg->data              = iram;
   seg->size              = sizeof(iram);
   items[0]               = buffer_item("srm", "/s/game.srm");
   items[0].segments      = seg;
   items[0].num_segments  = 1;
   items[0].legacy_memory = RETRO_MEMORY_SAVE_RAM;
   register_items(items, 1);
   event_load_save_files(false);
   h       = start_autosave(false);
   iram[0] = 1;
   memcpy(older, iram, sizeof(iram));
   capture(h);
   assert(disk_holds("/s/game.srm", older, sizeof(iram)));
   set_hold(&hold_temp, true);
   iram[0] = 2;
   memcpy(last, iram, sizeof(iram));
   register_items(NULL, 0);
   wait_held(1);
   memset(legacy_ram, 9, sizeof(legacy_ram));
}

/* Save RAM given back while the item's worker still writes its last
 * image: each save holds it, nothing waits, and the first save after
 * that image has landed writes it. */
static void test_given_back_held(void)
{
   unsigned char older[4];
   unsigned char last[4];
   struct retro_save_segment seg;
   struct retro_save_item items[1];
   unsigned w;
   unsigned i;

   given_back_held(items, &seg, older, last);
   w = raw_writes_now();
   for (i = 0; i < 3; i++)
   {
      event_save_files(true, false, NULL);
      autosave_check();
   }
   /* Killed here: the older image, the last in the worker's temp. */
   assert(raw_writes_now() == w);
   assert(!autosave_state.list[0]);
   assert(disk_holds("/s/game.srm", older, sizeof(older)));
   assert(disk_count_named("") == 2 && disk_count_named(".tmp") == 1);
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 0);
   set_hold(&hold_temp, false);
   retired_join(false);
   assert(disk_holds("/s/game.srm", last, sizeof(last)));
   /* The frame start reaps it. */
   autosave_check();
   assert(autosave_state.num_retired == 0);
   event_save_files(true, false, NULL);
   assert(disk_holds("/s/game.srm", legacy_ram, sizeof(legacy_ram)));
   assert(disk_count_named("") == 1);
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 0);
   autosave_deinit();
}

/* Save RAM's own autosave, off while an item held its place, starts
 * once the item's last image has landed, and writes its first snapshot
 * changed or not; it writes nothing while the memory is retired again
 * or a write is owed its file. */
static void test_given_back_autosave(void)
{
   unsigned char older[4];
   unsigned char last[4];
   unsigned char was[4];
   struct retro_save_segment seg;
   struct retro_save_item items[1];
   autosave_t *r;
   autosave_t *h;
   unsigned i;

   given_back_held(items, &seg, older, last);
   autosave_check();
   assert(!autosave_state.list[0]);
   set_hold(&hold_temp, false);
   retired_join(false);
   autosave_check();
   assert(autosave_state.num_retired == 0);
   assert((r = autosave_state.list[0]));
   r->interval = 0;
   wait_requested(r);
   capture(r);
   assert(disk_holds("/s/game.srm", legacy_ram, sizeof(legacy_ram)));
   legacy_ram[0] = 8;
   capture(r);
   assert(disk_holds("/s/game.srm", legacy_ram, sizeof(legacy_ram)));
   memcpy(was, legacy_ram, sizeof(was));

   /* The item back in its place. */
   register_items(items, 1);
   for (i = 0; !save_item_files.files[0].autosave && i < 3; i++)
      event_save_items_fill(false);
   assert((h = save_item_files.files[0].autosave));
   assert(!memcmp(iram, was, sizeof(was)));
   legacy_ram[0] = 7;
   autosave_lock();
   autosave_unlock();
   assert(retro_atomic_load_acquire_int(&r->snapshot_requested));
   assert(disk_holds("/s/game.srm", was, sizeof(was)));

   /* Gone again, its worker held writing its last image. */
   set_hold(&hold_temp, true);
   iram[0] = 5;
   memcpy(last, iram, sizeof(last));
   register_items(NULL, 0);
   wait_held(2);
   autosave_lock();
   autosave_unlock();
   assert(retro_atomic_load_acquire_int(&r->snapshot_requested));
   assert(disk_holds("/s/game.srm", was, sizeof(was)));
   set_hold(&hold_temp, false);
   retired_join(false);
   assert(disk_holds("/s/game.srm", last, sizeof(last)));
   capture(r);
   assert(autosave_state.num_retired == 0);
   assert(disk_holds("/s/game.srm", legacy_ram, sizeof(legacy_ram)));
   assert(disk_count_named("") == 1);
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 0);
   autosave_deinit();
}

/* Autosave started again from the settings after save RAM was given
 * back writes its first snapshot changed or not, never while a write is
 * owed its file. Restarted while the item's worker still writes its
 * last image (pass 0), after save RAM's own autosave started but before
 * its first write (1), and while an orphan owes the file its image (2). */
static void test_given_back_reinit(void)
{
   unsigned char older[4];
   unsigned char last[4];
   struct retro_save_segment seg;
   struct retro_save_item items[1];
   autosave_t *r;
   unsigned pass;
   unsigned w;
   unsigned i;

   for (pass = 0; pass < 3; pass++)
   {
      if (pass < 2)
      {
         given_back_held(items, &seg, older, last);
         set_hold(&hold_temp, false);
      }
      else
      {
         start();
         app_dir = "/app";
         path_init_savefile_rtc("/s/game.srm");
         seg.data               = iram;
         seg.size               = sizeof(iram);
         items[0]               = buffer_item("srm", "/s/game.srm");
         items[0].segments      = &seg;
         items[0].num_segments  = 1;
         items[0].legacy_memory = RETRO_MEMORY_SAVE_RAM;
         register_items(items, 1);
         event_load_save_files(false);
         iram[0] = 1;
         memcpy(older, iram, sizeof(older));
         event_save_files(true, false, NULL);
         iram[0] = 2;
         memcpy(last, iram, sizeof(last));
         register_items(NULL, 0);
         assert(save_item_files.num_orphans == 1);
         memset(legacy_ram, 9, sizeof(legacy_ram));
      }
      if (pass == 1)
      {
         retired_join(false);
         autosave_check();
         assert((r = autosave_state.list[0]));
         wait_requested(r);
      }
      autosave_deinit();
      assert(autosave_init(false, 1));
      assert((r = autosave_state.list[0]));
      r->interval = 0;
      wait_requested(r);
      w = worker_writes;
      if (pass == 2)
      {
         for (i = 0; i < 3; i++)
         {
            autosave_lock();
            autosave_unlock();
         }
         /* Killed here: the item's older image, alone. */
         assert(retro_atomic_load_acquire_int(&r->snapshot_requested));
         assert(worker_writes == w);
         assert(disk_holds("/s/game.srm", older, sizeof(older)));
         event_save_files(true, false, NULL);
         assert(save_item_files.num_orphans == 0);
      }
      else
         assert(disk_holds("/s/game.srm", last, sizeof(last)));
      capture(r);
      assert(worker_writes == w + 1);
      assert(disk_holds("/s/game.srm", legacy_ram, sizeof(legacy_ram)));
      assert(disk_count_named("") == 1);
      ASSERT_FILES(0, 0, 0);
      assert(err_count == 0);
      autosave_deinit();
   }
}

/* As test_left_out_back(), the newest image held by the left-out
 * item's retired worker rather than an orphan. */
static void test_left_out_back_retired(void)
{
   const save_items_state_t *st = save_items_state_get_ptr();
   struct retro_save_item items[1];
   unsigned char older[12];
   unsigned char newest[12];
   autosave_t *h;
   start();
   app_dir        = "/app";
   items[0]       = buffer_item("srm", "/s/game.srm");
   items[0].label = "Battery";
   register_items(items, 1);
   event_load_save_files(false);
   h       = start_autosave(false);
   wram[0] = 1;
   memcpy(older, image(), sizeof(older));
   capture(h);
   assert(disk_holds("/s/game.srm", older, sizeof(older)));
   set_hold(&hold_temp, true);
   wram[0] = 2;
   memcpy(newest, image(), sizeof(newest));
   items[0].path = NULL;
   register_items(items, 1);
   assert(st->num_issues == 1);
   assert(st->issues[0].verdict == RETRO_SAVE_VERDICT_LEFT_OUT);
   assert(save_item_files.num_orphans == 0);
   assert(autosave_state.num_retired == 1);
   wait_held(1);
   wram[0]       = 3;
   items[0].path = "/s/game.srm";
   register_items(items, 1);
   assert(st->num_issues == 0);
   event_save_items_fill(false);
   assert(!memcmp(image(), newest, sizeof(newest)));
   /* Killed here: the file as last written, the newest in the temp. */
   assert(disk_count_named("") == 2 && disk_count_named(".tmp") == 1);
   assert(disk_holds("/s/game.srm", older, sizeof(older)));
   ASSERT_FILES(0, 0, 0);
   set_hold(&hold_temp, false);
   retired_join(false);
   autosave_check();
   assert(autosave_state.num_retired == 0);
   assert(disk_count_named("") == 1);
   assert(disk_holds("/s/game.srm", newest, sizeof(newest)));
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 1);
   event_save_files(true, false, NULL);
   assert(disk_count_named("") == 1);
   assert(disk_holds("/s/game.srm", newest, sizeof(newest)));
   ASSERT_FILES(0, 0, 0);
   autosave_deinit();
}

/* Where case is ignored, SAVE_FILES at one spelling of a file drops a
 * retired worker's older image waiting at another. */
static void test_retired_other_spelling(void)
{
   struct retro_save_item items[1];
   unsigned i;

   start();
   slock_lock(disk_lock);
   disk_nocase = true;
   slock_unlock(disk_lock);
   items[0] = buffer_item("srm", "/s/Game.srm");
   register_items(items, 1);
   event_load_save_files(false);
   start_autosave(false);
   set_hold(&hold_temp, true);
   wram[0] = 1;
   items[0].path = "/s/game.srm";
   register_items(items, 1);
   wait_held(1);
   event_save_items_fill(false);
   wram[0] = 2;
   assert(save_files());
   set_hold(&hold_temp, false);
   for (i = 0; autosave_state.num_retired && i < 5000; i++)
   {
      autosave_check();
      retro_sleep(1);
   }
   assert(autosave_state.num_retired == 0);
   assert(disk_holds("/s/game.srm", image(), 12));
   assert(failed_renames == 0 && !disk_named(".tmp"));
   autosave_deinit();
}

/* An old-layout file goes whole to legacy_load(): no tail, the old
 * file stays, the first write creates the new one, and the
 * conversion is logged. A refusal moves to the next name; with every
 * name refused the item is as the core left it, still told, and the
 * warning names the first. */
static void test_legacy_load(void)
{
   static const char *const legacy[] = {"/s/a.sav", "/s/b.sav", NULL};
   unsigned char old[16];
   struct retro_save_item items[1];

   start();
   fill_bytes(old, sizeof(old), 0x70);
   disk_put("/s/b.sav", old, sizeof(old));
   use_legacy_load       = true;
   items[0]              = buffer_item("srm", "/s/game.srm");
   items[0].legacy_paths = legacy;
   register_items(items, 1);
   assert(event_load_save_files(false));
   assert(legacy_calls == 1 && legacy_last_index == 1);
   assert(legacy_last_len == 16 && wram[0] == 0x33 && loaded_calls == 1);
   assert(!save_items_state_get_ptr()->current->items[0].tail_size);
   assert(save_item_files.files[0].initial);
   assert(save_files());
   assert(disk_holds("/s/game.srm", image(), 12));
   assert(disk_holds("/s/b.sav", old, 16));

   start();
   disk_put("/s/a.sav", old, 12);
   disk_put("/s/b.sav", old, 16);
   use_legacy_load     = true;
   legacy_refuse_below = 1;
   register_items(items, 1);
   assert(event_load_save_files(false));
   assert(legacy_calls == 2 && legacy_last_index == 1 && osd_count == 0);

   start();
   disk_put("/s/a.sav", old, 12);
   disk_put("/s/b.sav", old, 16);
   use_legacy_load     = true;
   legacy_refuse_below = 2;
   register_items(items, 1);
   assert(!event_load_save_files(false));
   assert(legacy_calls == 2 && wram[0] == 0 && loaded_calls == 1);
   assert(osd_count == 1 && strstr(osd_last, "/s/a.sav"));
   assert(save_files() && !disk_has("/s/game.srm"));
   assert(disk_holds("/s/a.sav", old, 12));
}

/* Without legacy_load() a buffer reads an earlier name raw, its tail
 * following it to the new name, a size that differs shown;
 * a blob gets it through blob_load(). */
static void test_legacy_raw(void)
{
   static const char *const legacy[] = {"/s/a.sav", NULL};
   unsigned char old[16];
   struct retro_save_item items[1];

   start();
   fill_bytes(old, sizeof(old), 0x10);
   disk_put("/s/a.sav", old, sizeof(old));
   items[0]              = buffer_item("srm", "/s/game.srm");
   items[0].legacy_paths = legacy;
   register_items(items, 1);
   assert(event_load_save_files(false) && !memcmp(image(), old, 12));
   assert(osd_count == 1 && strstr(osd_last, "/s/a.sav"));
   assert(save_files());
   assert(disk_holds("/s/game.srm", old, 16));

   start();
   disk_put("/s/a.sav", old, 12);
   register_items(items, 1);
   assert(event_load_save_files(false) && osd_count == 0);

   start();
   disk_put("/s/a.sav", old, 4);
   items[0].storage      = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   items[0].segments     = NULL;
   items[0].num_segments = 0;
   register_items(items, 1);
   assert(event_load_save_files(false) && blob_len == 4);
   assert(!memcmp(blob_state, old, 4));
}

/* An earlier name newer than the own file is shown once. */
static void test_legacy_newer(void)
{
   static const char *const legacy[] = {"/s/a.sav", NULL};
   unsigned char file[12];
   struct retro_save_item items[1];

   start();
   fill_bytes(file, sizeof(file), 1);
   disk_put("/s/game.srm", file, sizeof(file));
   disk_put("/s/a.sav", file, sizeof(file));
   items[0]              = buffer_item("srm", "/s/game.srm");
   items[0].legacy_paths = legacy;
   register_items(items, 1);
   assert(event_load_save_files(false) && osd_count == 0);

   start();
   disk_put("/s/game.srm", file, sizeof(file));
   disk_put("/s/a.sav", file, sizeof(file));
   newer_path = "/s/a.sav";
   register_items(items, 1);
   assert(event_load_save_files(false) && !memcmp(image(), file, 12));
   assert(osd_count == 1 && strstr(osd_last, "/s/a.sav"));
}

/* The core registers again from inside legacy_load() and loaded(): no
 * item is filled or told twice. */
static void test_legacy_reregister(void)
{
   static const char *const legacy[] = {"/s/a.sav", NULL};
   unsigned char old[12];
   struct retro_save_item items[1];

   start();
   app_dir = "/app";
   fill_bytes(old, sizeof(old), 0x50);
   disk_put("/s/a.sav", old, sizeof(old));
   use_legacy_load       = true;
   legacy_reregister     = true;
   items[0]              = buffer_item("srm", "/s/game.srm");
   items[0].legacy_paths = legacy;
   rereg_items           = items;
   rereg_n               = 1;
   register_items(items, 1);
   assert(event_load_save_files(false));
   assert(legacy_calls == 1 && loaded_calls == 1 && wram[0] == 0x33);
   event_save_items_fill(false);
   assert(legacy_calls == 1 && loaded_calls == 1);

   start();
   app_dir = "/app";
   disk_put("/s/a.sav", old, sizeof(old));
   use_legacy_load   = true;
   loaded_reregister = true;
   register_items(items, 1);
   assert(event_load_save_files(false));
   assert(legacy_calls == 1 && loaded_calls == 1);
   event_save_items_fill(false);
   assert(legacy_calls == 1 && loaded_calls == 1);
ASSERT_FILES(0, 0, 0);
assert(err_count == 0);
}

/* A grow over a kept tail is filled at the frame start, then told. */
static void test_grow_told(void)
{
   unsigned char file[16];
   struct retro_save_item items[1];

   start();
   fill_bytes(file, sizeof(file), 0x60);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0]              = buffer_item("srm", "/s/game.srm");
   items[0].num_segments = 1;
   register_items(items, 1);
   assert(event_load_save_files(false) && loaded_calls == 1);
   items[0].num_segments = 2;
   register_items(items, 1);
   assert(loaded_calls == 1);
   event_save_items_fill(false);
   assert(loaded_calls == 2 && !memcmp(iram, file + 8, 4));
}

static void test_undo_dir(void)
{
   char dir[256];
   char want[256];
   fill_pathname_join(want, "/assets", "state_undo", sizeof(want));
   fill_pathname_join(want, want, "Beetle PSX", sizeof(want));
   fill_pathname_join(want, want, "Game.v1.2", sizeof(want));
   assert(save_items_state_undo_dir(dir, sizeof(dir), "/assets",
         "Beetle PSX", "/roms/psx/Game.v1.2") == strlen(want));
   assert(!strcmp(dir, want));
   assert(!save_items_state_undo_dir(dir, sizeof(dir), "", "core",
         "/roms/Game"));
   assert(!save_items_state_undo_dir(dir, sizeof(dir), "/assets", NULL,
         "/roms/Game"));
   assert(!save_items_state_undo_dir(dir, sizeof(dir), "/assets", "core",
         "/roms/"));
}

static void test_loaded(void)
{
   static const char *const legacy[] = {"/s/old.srm", NULL};
   unsigned char file[12];
   struct retro_save_item items[2];
   start();
   memset(file, 0, sizeof(file));
   file[0]  = 0x5a;
   file[11] = 0xa5;
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   memset(&items[1], 0, sizeof(items[1]));
   items[1].id            = "core";
   items[1].path          = "/s/core.dat";
   items[1].storage       = RETRO_SAVE_STORAGE_CORE_FILE;
   items[1].root          = RETRO_SAVE_ROOT_ABSOLUTE;
   items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   register_items(items, 2);
   assert(loaded_calls == 0);
   assert(event_load_save_files(false));
   /* Once, for the frontend's item only, after its bytes are in. */
   assert(loaded_calls == 1 && loaded_saw_file);

   /* Told even when there is nothing to read. */
   start();
   register_items(items, 1);
   assert(!event_load_save_files(false));
   assert(loaded_calls == 1 && !loaded_saw_file);

   /* An empty file of its own: taken up as it is, no earlier name
    * tried. */
   start();
   disk_put("/s/game.srm", file, 0);
   disk_put("/s/old.srm", file, sizeof(file));
   wram[0]               = 0x11;
   items[0].legacy_paths = legacy;
   register_items(items, 1);
   assert(!event_load_save_files(false));
   assert(loaded_calls == 1 && wram[0] == 0x11);
   assert(save_item_files.files[0].initial);
   assert(save_item_files.files[0].initial_size == 12);
   assert(!memcmp(save_item_files.files[0].initial, image(), 12));
}

static void test_final_write(void)
{
   struct retro_save_item items[1];
   unsigned char after[12];
   start();
   app_dir  = "/app";
   items[0] = buffer_item("srm", "/s/final.srm");
   register_items(items, 1);
   event_load_save_files(false);
   wram[0] = 1;
   /* The close's SAVE_FILES, before the core unloads. */
   assert(save_files());
   assert(raw_writes == 1);

   /* retro_unload_game() writes once more, an emulated power-off. */
   wram[0] = 2;
   iram[0] = 3;
   memcpy(after, image(), sizeof(after));
   event_save_items_final(true, false);
   assert(raw_writes == 2 && disk_holds("/s/final.srm", after, 12));
   assert(save_item_files.num_files == 0 && !save_item_files.loaded);
   ASSERT_FILES(0, 0, 0);
   /* The set stays until save_items_clear(). */
   assert(save_items_state_get_ptr()->current);

   /* Nothing changed during the unload: no second write. */
   start();
   register_items(items, 1);
   event_load_save_files(false);
   wram[0] = 1;
   assert(save_files());
   event_save_items_final(true, false);
   assert(raw_writes == 1);

   /* Save RAM not in use: nothing at all. */
   start();
   register_items(items, 1);
   event_load_save_files(false);
   wram[0] = 1;
   event_save_items_final(false, false);
   assert(raw_writes == 0);

   /* The workers are stopped before it writes. */
   start();
   register_items(items, 1);
   event_load_save_files(false);
   start_autosave(false);
   wram[0] = 4;
   event_save_items_final(true, false);
   assert(!autosave_state.num_retired);
   assert(disk_holds("/s/final.srm", image(), 12));
   autosave_deinit();
}

/* The close as runloop makes it: autosave stopped, SAVE_FILES, then the
 * final write after retro_unload_game(). */
static void snapshot_close(void)
{
   autosave_deinit();
   assert(save_files());
   /* Killed here. */
   event_save_items_final(true, false);
}

/* can_snapshot() false at SAVE_FILES or the close: when autosave's last
 * snapshot was taken on true, nothing new is written and the file holds
 * that snapshot, logged once per item until a frame runs; otherwise the
 * image now is written, as without it. */
static void test_snapshot_close(void)
{
   struct retro_save_item items[1];
   unsigned char done[12];
   autosave_t *h;

   /* Settled: nothing new while running, at the close, or after it. */
   start();
   app_dir          = "/app";
   use_can_snapshot = true;
   items[0] = buffer_item("srm", "/s/close.srm");
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   wram[0] = 1;
   capture(h);
   assert(raw_writes == 1);
   memcpy(done, image(), sizeof(done));
   snapshot_ready = false;
   wram[0]        = 2;
   assert(save_files());
   assert(raw_writes == 1 && disk_holds("/s/close.srm", done, 12));
   core_frames(1, 60.0);
   autosave_deinit();
   assert(save_files());
   assert(raw_writes == 1 && disk_holds("/s/close.srm", done, 12));
   event_save_items_final(true, false);
   assert(raw_writes == 1 && disk_holds("/s/close.srm", done, 12));
   assert(snapshot_calls == 5 && saving_logs == 2 && err_count == 0);
   ASSERT_FILES(0, 0, 0);

   /* True again by the final write: the image now. */
   start();
   app_dir          = "/app";
   use_can_snapshot = true;
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   wram[0] = 1;
   capture(h);
   snapshot_ready = false;
   wram[0]        = 2;
   autosave_deinit();
   assert(save_files());
   assert(raw_writes == 1);
   snapshot_ready = true;
   wram[1]        = 3;
   event_save_items_final(true, false);
   assert(raw_writes == 2 && disk_holds("/s/close.srm", image(), 12));
   assert(saving_logs == 1 && err_count == 0);
   ASSERT_FILES(0, 0, 0);

   /* The worker's write of a settled snapshot it had not started when
    * the close stopped it lands at the join. */
   start();
   app_dir          = "/app";
   use_can_snapshot = true;
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   wram[0] = 1;
   assert(autosave_fill_item(h));
   memcpy(done, image(), sizeof(done));
   retro_atomic_store_release_int(&h->snapshot_requested, 0);
   retro_atomic_store_release_int(&h->quit, 1);
   snapshot_ready = false;
   wram[0]        = 2;
   autosave_deinit();
   /* Killed here. */
   assert(raw_writes == 1 && disk_holds("/s/close.srm", done, 12));
   assert(save_files());
   event_save_items_final(true, false);
   assert(raw_writes == 1 && disk_holds("/s/close.srm", done, 12));
   assert(saving_logs == 1 && err_count == 0);
   ASSERT_FILES(0, 0, 0);

   /* Its last snapshot was taken at the cap: the image now. */
   start();
   app_dir          = "/app";
   use_can_snapshot = true;
   snapshot_ready   = false;
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   wram[0] = 1;
   autosave_check();
   core_frames(600, 60.0);
   capture(h);
   assert(raw_writes == 1);
   wram[0] = 2;
   snapshot_close();
   assert(raw_writes == 2 && disk_holds("/s/close.srm", image(), 12));
   assert(saving_logs == 1 && err_count == 0);
   ASSERT_FILES(0, 0, 0);

   /* Its write of the settled snapshot failed: the image now. */
   start();
   app_dir          = "/app";
   use_can_snapshot = true;
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   slock_lock(disk_lock);
   fail_writes = true;
   slock_unlock(disk_lock);
   wram[0] = 1;
   capture(h);
   slock_lock(disk_lock);
   fail_writes = false;
   slock_unlock(disk_lock);
   assert(!disk_has("/s/close.srm"));
   snapshot_ready = false;
   wram[0]        = 2;
   snapshot_close();
   assert(disk_holds("/s/close.srm", image(), 12));
   assert(saving_logs == 0 && err_count == 0);
   ASSERT_FILES(0, 0, 0);

   /* Autosave stopped mid-game, then a frame ran: the image now. */
   start();
   app_dir          = "/app";
   use_can_snapshot = true;
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   wram[0] = 1;
   capture(h);
   autosave_deinit();
   event_save_items_fill(false);
   snapshot_ready = false;
   wram[0]        = 2;
   assert(save_files());
   assert(raw_writes == 2 && disk_holds("/s/close.srm", image(), 12));
   event_save_items_final(true, false);
   assert(raw_writes == 2 && saving_logs == 0 && err_count == 0);
   ASSERT_FILES(0, 0, 0);

   /* Autosave off: the image now. */
   start();
   app_dir          = "/app";
   use_can_snapshot = true;
   snapshot_ready   = false;
   register_items(items, 1);
   event_load_save_files(false);
   wram[0] = 1;
   assert(save_files());
   event_save_items_final(true, false);
   assert(raw_writes == 1 && disk_holds("/s/close.srm", image(), 12));
   assert(saving_logs == 0 && err_count == 0);
   ASSERT_FILES(0, 0, 0);
}

/* The close writes the image now because the file owes its copy: the
 * final write keeps that image only if it was taken while the game said
 * true. Pass 0: it was not, so what the core wrote while unloading is
 * written. Pass 1: it was. */
static void test_snapshot_close_owed(void)
{
   unsigned char file[16];
   unsigned char cut[16];
   unsigned char closed[12];
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned pass;
   unsigned copies;

   fill_bytes(file, sizeof(file), 0x60);
   for (pass = 0; pass < 2; pass++)
   {
      start();
      app_dir          = "/app";
      use_can_snapshot = true;
      disk_put("/s/owed.srm", file, sizeof(file));
      items[0]       = buffer_item("srm", "/s/owed.srm");
      items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
      register_items(items, 1);
      event_save_items_set_undo_dir(NULL);
      event_load_save_files(false);
      event_save_items_set_undo_dir("/undo");
      assert(save_item_files.files[0].guard.copy_first);
      set_fail_undo(true);
      copies = copy_logs;
      h = start_autosave(false);
      wram[0] = 1;
      capture(h);
      /* Killed here: settled, written without the copy, the tail kept. */
      memcpy(cut, image(), 12);
      memcpy(cut + 12, file + 12, 4);
      assert(disk_holds("/s/owed.srm", cut, 16));
      ASSERT_FILES(0, 0, 0);
      assert(saving_logs == 0 && err_count == 0);
      set_fail_undo(false);
      snapshot_ready = pass == 1;
      wram[0]        = 2;
      autosave_deinit();
      assert(save_item_files.files[0].guard.copy_first);
      assert(save_files());
      /* Killed here. */
      memcpy(closed, image(), 12);
      assert(disk_holds("/s/owed.srm", closed, 12));
      assert(disk_holds("/undo/owed.srm", cut, 16));
      ASSERT_FILES(1, 0, 0);
      assert(saving_logs == 0 && err_count == 0);
      /* Written while unloading. */
      snapshot_ready = false;
      wram[1]        = 3;
      event_save_items_final(true, false);
      assert(disk_holds("/s/owed.srm", pass ? closed : image(), 12));
      assert(disk_holds("/undo/owed.srm", cut, 16));
      ASSERT_FILES(1, 0, 0);
      assert(saving_logs == pass && err_count == 0
            && copy_logs == copies + 1);
   }
}

/* After a failed write, SAVE_FILES writes the image now while the game
 * is still saving, under the worker's retry of a settled snapshot: the
 * retry is dropped, so the close does not keep that image. */
static void test_snapshot_close_superseded(void)
{
   unsigned char mid[12];
   struct retro_save_item items[1];
   autosave_t *h;

   start();
   app_dir          = "/app";
   use_can_snapshot = true;
   items[0] = buffer_item("srm", "/s/sup.srm");
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   set_fail_writes(true);
   wram[0] = 1;
   capture(h);
   set_fail_writes(false);
   assert(!disk_has("/s/sup.srm"));
   set_hold(&hold_temp, true);
   wram[0] = 2;
   autosave_lock();
   autosave_unlock();
   wait_held(1);
   snapshot_ready = false;
   wram[0]        = 3;
   memcpy(mid, image(), sizeof(mid));
   assert(save_files());
   set_hold(&hold_temp, false);
   wait_requested(h);
   /* Killed here. */
   assert(disk_holds("/s/sup.srm", mid, 12));
   ASSERT_FILES(0, 0, 0);
   assert(saving_logs == 0 && err_count == 0);
   autosave_deinit();
   /* Killed here: joined. */
   assert(disk_holds("/s/sup.srm", mid, 12));
   assert(save_files());
   assert(disk_holds("/s/sup.srm", mid, 12));
   /* Written while unloading. */
   wram[1] = 4;
   event_save_items_final(true, false);
   assert(disk_holds("/s/sup.srm", image(), 12));
   ASSERT_FILES(0, 0, 0);
   assert(saving_logs == 0 && err_count == 0 && snapshot_calls == 5);
}

/* SAVE_FILES writes a settled image while the worker writes an older
 * settled snapshot: pass 0 drops that write, pass 1 renames it first
 * and writes the newer again. The game then saves and the user quits:
 * the close and the final write keep the settled image. */
static void test_snapshot_close_main_settled(void)
{
   unsigned char settled[12];
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned pass;

   for (pass = 0; pass < 2; pass++)
   {
      bool *hold = pass ? &hold_rename : &hold_temp;
      start();
      app_dir          = "/app";
      use_can_snapshot = true;
      items[0] = buffer_item("srm", "/s/main.srm");
      register_items(items, 1);
      event_load_save_files(false);
      h = start_autosave(false);
      wram[0] = 1;
      capture(h);
      set_hold(hold, true);
      wram[0] = 2;
      autosave_lock();
      autosave_unlock();
      wait_held(1);
      wram[0] = 3;
      memcpy(settled, image(), sizeof(settled));
      assert(event_save_files(true, false, NULL));
      set_hold(hold, false);
      wait_requested(h);
      /* Killed here. */
      assert(disk_holds("/s/main.srm", settled, 12));
      ASSERT_FILES(0, 0, 0);
      assert(saving_logs == 0 && err_count == 0);
      snapshot_ready = false;
      wram[0]        = 4;
      autosave_lock();
      autosave_unlock();
      autosave_deinit();
      assert(event_save_files(true, false, NULL));
      /* Killed here. */
      assert(disk_holds("/s/main.srm", settled, 12));
      /* Written while unloading. */
      wram[1] = 5;
      event_save_items_final(true, false);
      assert(disk_holds("/s/main.srm", settled, 12));
      ASSERT_FILES(0, 0, 0);
      assert(saving_logs == 1 && err_count == 0 && snapshot_calls == 6);
   }
}

/* The worker's older snapshot lands over a settled image SAVE_FILES
 * wrote, and writing that image again fails: the file holds the older
 * one, so the close writes the image now. */
static void test_snapshot_close_reissue_failed(void)
{
   unsigned char older[12];
   struct retro_save_item items[1];
   autosave_t *h;

   start();
   app_dir          = "/app";
   use_can_snapshot = true;
   items[0] = buffer_item("srm", "/s/main.srm");
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   wram[0] = 1;
   capture(h);
   set_hold(&hold_rename, true);
   wram[0] = 2;
   memcpy(older, image(), sizeof(older));
   autosave_lock();
   autosave_unlock();
   wait_held(1);
   wram[0] = 3;
   assert(event_save_files(true, false, NULL));
   set_fail_writes(true);
   set_hold(&hold_rename, false);
   wait_requested(h);
   set_fail_writes(false);
   /* Killed here. */
   assert(disk_holds("/s/main.srm", older, 12));
   snapshot_ready = false;
   wram[0]        = 4;
   autosave_lock();
   autosave_unlock();
   autosave_deinit();
   assert(event_save_files(true, false, NULL));
   /* Killed here. */
   assert(disk_holds("/s/main.srm", image(), 12));
   /* Written while unloading. */
   wram[1] = 5;
   event_save_items_final(true, false);
   assert(disk_holds("/s/main.srm", image(), 12));
   ASSERT_FILES(0, 0, 0);
   assert(saving_logs == 0 && err_count == 0);
}

static void test_legacy_paths(void)
{
   static const char *const legacy[] = {"/s/older.sav", "/s/oldest.sav",
      NULL};
   static const unsigned char old[12] = {7, 7, 7, 7, 7, 7, 7, 7,
      7, 7, 7, 7};
   struct retro_save_item items[1];
   start();
   app_dir = "/app";
   disk_put("/s/oldest.sav", old, sizeof(old));
   items[0]              = buffer_item("srm", "/s/game.srm");
   items[0].legacy_paths = legacy;
   register_items(items, 1);
   assert(event_load_save_files(false));
   assert(!memcmp(image(), old, 12));

   /* The next write goes to the item's own name; the old file stays. */
   wram[0] = 1;
   assert(save_files());
   assert(disk_holds("/s/game.srm", image(), 12));
   assert(disk_holds("/s/oldest.sav", old, sizeof(old)));

   /* The item's own name wins once it exists. */
   start();
   app_dir = "/app";
   disk_put("/s/oldest.sav", old, sizeof(old));
   disk_put("/s/game.srm", image(), 12);
   register_items(items, 1);
   assert(event_load_save_files(false));
   assert(wram[0] == 0);
ASSERT_FILES(0, 0, 0);
assert(err_count == 0);
}

static void test_late_loaded(void)
{
   static const unsigned char saved[6] = {'S', 'A', 'V', 'E', 'D', '!'};
   struct retro_save_item items[2];
   start();
   disk_put("/s/one.card", saved, sizeof(saved));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   event_load_save_files(false);
   assert(loaded_calls == 1);

   /* Never from inside the call that registered it. */
   items[1] = card_item("card", "/s/one.card");
   register_items(items, 2);
   assert(loaded_calls == 1);
   event_save_items_fill(false);
   assert(loaded_calls == 2 && loaded_in_set == 0);
   assert(!memcmp(card, saved, sizeof(card)));

   /* Once per fill. */
   event_save_items_fill(false);
   assert(loaded_calls == 2);

   /* A kept id moved to another file is filled and told again. */
   items[1].path = "/s/two.card";
   register_items(items, 2);
   assert(loaded_calls == 2);
   event_save_items_fill(false);
   assert(loaded_calls == 3 && loaded_in_set == 0);
}

/* loaded() registers again without the item it was told: the items
 * left, moved to the indices already passed, are still told. */
static void test_loaded_reregister_moves(void)
{
   struct retro_save_item items[3];
   start();
   app_dir = "/app";
   memset(&items[0], 0, sizeof(items[0]));
   items[0].id            = "flash";
   items[0].path          = "/s/game.flash";
   items[0].storage       = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   items[0].root          = RETRO_SAVE_ROOT_ABSOLUTE;
   items[0].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   items[1]               = buffer_item("srm", "/s/game.srm");
   items[2]               = card_item("card", "/s/one.card");
   loaded_reregister      = true;
   rereg_items            = items + 1;
   rereg_n                = 2;
   register_items(items, 3);
   event_load_save_files(false);
   assert(loaded_calls == 3);
   event_save_items_fill(false);
   assert(loaded_calls == 3);
ASSERT_FILES(0, 0, 0);
assert(err_count == 0);
}

/* A set registered right after a refusal fills from the ended set's
 * image, not the older file. An orphan's write is dropped; a retired
 * worker's still lands. */
static void test_waiting_image(void)
{
   unsigned char want[12];
   unsigned char file[12];
   struct retro_save_item items[2];
   unsigned writes;
#ifdef HAVE_THREADS
   autosave_t *h;
   unsigned i;
#endif

   start();
   app_dir = "/app";
   disk_put("/s/w.srm", image(), 12);
   items[0] = buffer_item("srm", "/s/w.srm");
   items[1] = items[0];
   register_items(items, 1);
   event_load_save_files(false);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   register_refused(items, 2);
   assert(save_item_files.num_orphans == 1);
   register_items(items, 1);
   assert(save_item_files.num_orphans == 1);
   event_save_items_fill(false);
   assert(loaded_calls == 2);
   assert(!memcmp(image(), want, sizeof(want)));
   assert(save_item_files.num_orphans == 0);
   writes = raw_writes;
   assert(save_files());
   assert(raw_writes == writes + 1);
   assert(disk_holds("/s/w.srm", want, sizeof(want)));
   ASSERT_FILES(0, 0, 0);
   /* The refusal's two findings: the id, and the file. */
   assert(err_count == 2);

   /* A file not written yet: the orphan's baseline goes with it. */
   start();
   app_dir = "/app";
   items[0] = buffer_item("srm", "/s/new.srm");
   items[1] = items[0];
   register_items(items, 1);
   event_load_save_files(false);
   register_refused(items, 2);
   register_items(items, 1);
   event_save_items_fill(false);
   assert(save_item_files.files[0].initial);
   assert(save_files());
   assert(!disk_has("/s/new.srm"));
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 2);

#ifdef HAVE_THREADS
   /* A retired worker held inside a write: the fill takes a copy of
    * its last image, and the worker still writes both. */
   start();
   app_dir = "/app";
   disk_put("/s/w.srm", image(), 12);
   memcpy(file, image(), sizeof(file));
   items[0] = buffer_item("srm", "/s/w.srm");
   items[1] = items[0];
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   slock_lock(disk_lock);
   hold_write = true;
   slock_unlock(disk_lock);
   wram[2] = 7;
   autosave_lock();
   autosave_unlock();
   slock_lock(disk_lock);
   disk_wait_count(&raw_writes, 1);
   assert(raw_writes == 1);
   slock_unlock(disk_lock);
   wram[2] = 8;
   memcpy(want, image(), sizeof(want));
   register_refused(items, 2);
   assert(autosave_state.num_retired == 1);
   register_items(items, 1);
   event_save_items_fill(false);
   assert(!memcmp(image(), want, sizeof(want)));
   assert(retro_atomic_load_acquire_int(&h->superseded) == h->final_mark);
   assert(disk_holds("/s/w.srm", file, sizeof(file)));
   slock_lock(disk_lock);
   hold_write = false;
   scond_broadcast(disk_cond);
   slock_unlock(disk_lock);
   retired_join(false);
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 2);
   for (i = 0; autosave_state.num_retired && i < 5000; i++)
   {
      autosave_check();
      retro_sleep(1);
   }
   assert(autosave_state.num_retired == 0);
   assert(disk_holds("/s/w.srm", want, sizeof(want)));
   autosave_deinit();
   assert(save_files());
   assert(disk_holds("/s/w.srm", want, sizeof(want)));
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 2);
#else
   (void)file;
#endif
}

/* Where case is ignored, an item at one spelling of a file is filled
 * from the image waiting at another: an orphan's (pass 0), or a
 * retired worker's held in its last write (pass 1). */
static void test_waiting_other_spelling(void)
{
   unsigned char want[12];
   struct retro_save_item items[1];
   unsigned pass;

   for (pass = 0; pass < 2; pass++)
   {
#ifndef HAVE_THREADS
      if (pass)
         break;
#endif
      start();
      app_dir = "/app";
      slock_lock(disk_lock);
      disk_nocase = true;
      slock_unlock(disk_lock);
      disk_put("/s/W.srm", image(), 12);
      items[0] = buffer_item("srm", "/s/W.srm");
      register_items(items, 1);
      event_load_save_files(false);
#ifdef HAVE_THREADS
      if (pass)
      {
         start_autosave(false);
         set_hold(&hold_temp, true);
      }
#endif
      wram[1] = 0x44;
      memcpy(want, image(), sizeof(want));
      items[0].path = "/s/w.srm";
      register_items(items, 1);
#ifdef HAVE_THREADS
      if (pass)
         wait_held(1);
#endif
      event_save_items_fill(false);
      assert(!memcmp(image(), want, sizeof(want)));
#ifdef HAVE_THREADS
      if (pass)
      {
         unsigned i;
         set_hold(&hold_temp, false);
         retired_join(false);
         ASSERT_FILES(0, 0, 0);
         assert(err_count == 0);
         for (i = 0; autosave_state.num_retired && i < 5000; i++)
         {
            autosave_check();
            retro_sleep(1);
         }
         assert(!autosave_state.num_retired);
         autosave_deinit();
      }
#endif
      assert(!save_item_files.num_orphans);
      assert(save_files());
      assert(disk_holds("/s/w.srm", want, sizeof(want)));
      ASSERT_FILES(0, 0, 0);
      assert(err_count == 0);
   }
}

/* A blob at the file refuses an orphan's waiting image: the orphan
 * keeps its write, so the image lands at the next SAVE_FILES and is
 * copied before the blob's write. Pass 1 registers again from inside
 * blob_load(). */
static void test_waiting_refused(void)
{
   unsigned char old[12];
   unsigned char a[12];
   struct retro_save_item items[2];
   unsigned pass;

   memset(old, 0x21, sizeof(old));
   for (pass = 0; pass < 2; pass++)
   {
      start();
      app_dir = "/app";
      disk_put("/s/t.srm", old, sizeof(old));
      items[0] = buffer_item("a", "/s/t.srm");
      register_items(items, 1);
      event_load_save_files(false);
      wram[1]  = 0x44;
      memcpy(a, image(), sizeof(a));
      register_items(NULL, 0);
      assert(save_item_files.num_orphans == 1);
      memset(&items[1], 0, sizeof(items[1]));
      items[1].id            = "b";
      items[1].path          = "/s/t.srm";
      items[1].storage       = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
      items[1].root          = RETRO_SAVE_ROOT_ABSOLUTE;
      items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
      register_items(items + 1, 1);
      memcpy(blob_state, "keep", 4);
      blob_len    = 4;
      blob_refuse = true;
      if (pass)
      {
         blob_reregister = true;
         rereg_items     = items + 1;
         rereg_n         = 1;
      }
      event_save_items_fill(false);
      assert(blob_len == 4 && !memcmp(blob_state, "keep", 4));
      blob_refuse = false;
      assert(save_files());
      assert(disk_holds("/undo/t.srm", a, sizeof(a)));
      assert(disk_holds("/s/t.srm", "keep", 4));
      assert(!save_item_files.num_orphans);
      ASSERT_FILES(1, 0, 1);
      assert(err_count == 0);
   }
}

/* A cart moving port: the same tag and size under another id takes
 * the record over, so nothing is filled or told and the bytes written
 * since the fill are kept. */
static void test_port_move(void)
{
   unsigned char want[12];
   struct retro_save_item items[1];
#ifdef HAVE_THREADS
   autosave_t *h;
#endif

   start();
   disk_put("/s/cart.sav", image(), 12);
   items[0]        = buffer_item("tpak1", "/s/cart.sav");
   items[0].format = "gb:sram";
   items[0].port   = 1;
   register_items(items, 1);
   event_load_save_files(false);
#ifdef HAVE_THREADS
   h = start_autosave(false);
#endif
   assert(loaded_calls == 1);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   items[0].id   = "tpak2";
   items[0].port = 2;
   register_items(items, 1);
   assert(save_item_files.files[0].taken_up);
   assert(!save_item_files.fill_pending);
   assert(save_item_files.num_orphans == 0);
#ifdef HAVE_THREADS
   assert(save_item_files.files[0].autosave == h);
   assert(autosave_state.num_retired == 0);
#endif
   event_save_items_fill(false);
   assert(loaded_calls == 1);
   assert(!memcmp(image(), want, sizeof(want)));
#ifdef HAVE_THREADS
   autosave_deinit();
#endif
   assert(save_files());
   assert(disk_holds("/s/cart.sav", want, sizeof(want)));
}

/* Another tag at a file whose last image still waits: the copy holds
 * that image, the newest save of the old type, not the older file. */
static void test_type_change_waiting(void)
{
   unsigned char want[12];
   struct retro_save_item items[2];
#ifdef HAVE_THREADS
   autosave_t *h;
   unsigned pass, i;
   unsigned n;
   unsigned logs;
#endif

   start();
   disk_put("/s/t.srm", image(), 12);
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   assert(save_item_files.files[0].guard.type_changed);
   assert(save_item_files.num_orphans == 1);
   event_save_items_fill(false);
   assert(save_item_files.num_orphans == 0);
   assert(!memcmp(image(), want, sizeof(want)));
   wram[2] = 9;
   assert(save_files());
   assert(disk_holds("/undo/t.srm", want, sizeof(want)));
   assert(disk_holds("/s/t.srm", image(), 12));
   assert(strstr(osd_last, "/undo/t.srm"));
   assert(!save_item_files.files[0].guard.copy_image);

#ifdef HAVE_THREADS
   /* With workers the fill takes a copy of the retired worker's last
    * image, which that worker still writes. The new worker, started
    * once the retired one is reaped, copies it as it starts, also when
    * stopped (1) or retired (2, 3) before its first write, and the main
    * thread tells that copy as a live worker's, from the reap (2) or at
    * the close (3). */
   for (pass = 0; pass < 4; pass++)
   {
      start();
      disk_put("/s/t.srm", image(), 12);
      items[0].id     = "a";
      items[0].format = "core:a";
      register_items(items, 1);
      event_load_save_files(false);
      start_autosave(false);
      wram[1] = 0x44;
      items[0].id     = "b";
      items[0].format = "core:b";
      register_items(items, 1);
      event_save_items_fill(false);
      for (i = 0; !save_item_files.files[0].autosave && i < 5000; i++)
      {
         autosave_check();
         event_save_items_fill(false);
         retro_sleep(1);
      }
      assert((h = save_item_files.files[0].autosave) && h->copy_image);
      assert(disk_holds("/s/t.srm", want, sizeof(want)));
      h->interval = 0;
      wram[2]     = 9;
      if (!pass)
      {
         wait_requested(h);
         capture(h);
         assert(disk_holds("/undo/t.srm", want, sizeof(want)));
         assert(disk_holds("/s/t.srm", image(), 12));
      }
      if (pass >= 2)
      {
         n        = osd_count;
         logs     = copy_logs;
         items[1] = buffer_item("c", "/s/u.srm");
         register_items(items + 1, 1);
         for (i = 0; pass == 2 && i < 5000
               && !retro_atomic_load_acquire_int(&h->exited); i++)
            retro_sleep(1);
         assert(pass != 2 || retro_atomic_load_acquire_int(&h->exited));
         /* The worker tells nothing. */
         assert(osd_count == n && copy_logs == logs);
         for (i = 0; pass == 2 && autosave_state.num_retired
               && i < 5000; i++)
         {
            autosave_check();
            retro_sleep(1);
         }
         assert(pass != 2 || (!autosave_state.num_retired
               && copy_logs == logs + 1 && osd_count == n + 1));
         autosave_deinit();
         assert(copy_logs == logs + 1 && osd_count == n + 1);
         assert(strstr(copy_log, "\"/s/t.srm\" in \"/undo/t.srm\""));
         assert(strstr(osd_last, "/s/t.srm|/undo/t.srm"));
         assert(disk_holds("/undo/t.srm", want, sizeof(want)));
         continue;
      }
      autosave_deinit();
      assert(!save_item_files.files[0].guard.copy_image);
      assert(save_files());
      assert(disk_holds("/undo/t.srm", want, sizeof(want)));
      assert(disk_holds("/s/t.srm", image(), 12));
   }
#endif
}

/* A type change's copy that can never be made: the file is never
 * written over and the item's image goes to the recovery file; the
 * last write that owes the copy sends the taken image to a second one
 * under its own name, named on screen. Passes: the close; an orphan's
 * write; a worker that hands the image back at the close; a retired
 * worker's failed last write, recovered by the reap or at the close,
 * also when the item has not changed since its fill. */
static void test_type_change_uncopied(void)
{
   unsigned char old[12];
   unsigned char want[12];
   unsigned char last[12];
   struct retro_save_item items[2];
   unsigned pass;
#ifdef HAVE_THREADS
   autosave_t *h;
   unsigned i;
   unsigned n = 0;
#endif

   memset(old, 0x21, sizeof(old));
#ifdef HAVE_THREADS
   for (pass = 0; pass < 6; pass++)
#else
   for (pass = 0; pass < 2; pass++)
#endif
   {
      start();
      event_save_items_set_undo_dir(NULL);
      app_dir         = "/app";
      disk_put("/s/t.srm", old, sizeof(old));
      items[0]        = buffer_item("a", "/s/t.srm");
      items[0].format = "core:a";
      register_items(items, 1);
      event_load_save_files(false);
#ifdef HAVE_THREADS
      if (pass >= 2 && pass != 5)
         start_autosave(false);
#endif
      wram[1]         = 0x44;
      memcpy(want, image(), sizeof(want));
      items[0].id     = "b";
      items[0].format = "core:b";
      register_items(items, 1);
      event_save_items_fill(false);
#ifdef HAVE_THREADS
      /* Pass 5: the worker starts after the fill, which took the
       * orphan's image, and the item stays as the fill left it. */
      if (pass == 5)
         start_autosave(false);
#endif
      if (pass != 5)
         wram[2] = 9;
      memcpy(last, image(), sizeof(last));
      if (pass == 0)
      {
         /* A failed copy stays owed until the close; the old type's
          * image is in a recovery file of its own at once. */
         assert(!save_files());
         assert(save_item_files.files[0].guard.copy_image);
         assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
         assert(disk_count_named("RetroArch-recovery-") == 2);
      }
      if (pass == 1)
      {
         /* The item leaves the set: its orphan's write is its last. */
         items[1] = buffer_item("c", "/s/u.srm");
         register_items(items + 1, 1);
         assert(save_item_files.num_orphans == 1);
         assert(!save_files());
         assert(!save_item_files.num_orphans);
      }
#ifdef HAVE_THREADS
      if (pass >= 2)
      {
         /* Its worker, started once the retired one is reaped, cannot
          * copy either; at the close it hands the image back (2). */
         for (i = 0; !save_item_files.files[0].autosave && i < 5000; i++)
         {
            autosave_check();
            event_save_items_fill(false);
            retro_sleep(1);
         }
         assert((h = save_item_files.files[0].autosave) && h->copy_image);
         h->interval = 0;
         wait_requested(h);
         capture(h);
      }
      if (pass >= 3)
      {
         /* Nor can its retired worker once the item leaves the set:
          * the main thread recovers both images, from the reap (3, 5)
          * or at the close (4). Passes 3 and 4 change it first. */
         if (pass != 5)
         {
            wram[3] = 7;
            memcpy(last, image(), sizeof(last));
         }
         n        = osd_count;
         items[1] = buffer_item("c", "/s/u.srm");
         register_items(items + 1, 1);
         assert(!save_item_files.num_orphans);
         for (i = 0; pass != 4 && i < 5000
               && !retro_atomic_load_acquire_int(&h->exited); i++)
            retro_sleep(1);
         assert(pass == 4 || retro_atomic_load_acquire_int(&h->exited));
         /* Unchanged, it still writes: it owes the copy. */
         assert(pass != 5
               || retro_atomic_load_acquire_int(&h->final_failed));
         if (pass != 4)
         {
            /* The worker recovers both images and shows nothing. */
            assert(osd_count == n);
            assert(disk_count_named("RetroArch-recovery-") == 2);
            assert(disk_named_holds("RetroArch-recovery-", last, 12));
            assert(disk_named_holds("RetroArch-recovery-", want, 12));
            for (i = 0; autosave_state.num_retired && i < 5000; i++)
            {
               autosave_check();
               retro_sleep(1);
            }
            assert(!autosave_state.num_retired && osd_count == n + 2);
            assert(disk_count_named("RetroArch-recovery-") == 2);
         }
      }
#endif
      if (pass != 1)
         event_save_items_final(true, false);
#ifdef HAVE_THREADS
      if (pass == 4)
         assert(osd_count == n + 2);
      if (pass >= 2)
         autosave_deinit();
#endif
      /* The retired worker wrote its last image, taken by the fill;
       * pass 5's was an orphan's, whose write the take drops. */
      assert(disk_holds("/s/t.srm", pass >= 2 && pass != 5 ? want : old,
            sizeof(old)));
      assert(disk_named_holds("RetroArch-recovery-", last, sizeof(last)));
      assert(strstr(osd_last, "/s/t.srm|/app/RetroArch-recovery-"));
      /* Pass 5's item still holds that image. */
      assert(told_holding(want, sizeof(want)) == 1 + (pass == 5));
      assert(disk_named_holds("RetroArch-recovery-", want, 12)
            == 1 + (pass == 5));
   }
}

/* Two type changes at a file before a write, with no worker: the
 * second's fill takes the first's image, whose own copy, of the image
 * before it, is still made at the next save of the files, or recovered
 * when no copy can be made (pass 1). */
static void test_type_change_twice(void)
{
   unsigned char old[12];
   unsigned char a[12];
   unsigned char b[12];
   struct retro_save_item items[1];
   unsigned pass;

   memset(old, 0x21, sizeof(old));
   for (pass = 0; pass < 2; pass++)
   {
      start();
      if (pass)
      {
         event_save_items_set_undo_dir(NULL);
         app_dir = "/app";
      }
      disk_put("/s/t.srm", old, sizeof(old));
      items[0]        = buffer_item("a", "/s/t.srm");
      items[0].format = "core:a";
      register_items(items, 1);
      event_load_save_files(false);
      wram[1]         = 0x44;
      memcpy(a, image(), sizeof(a));
      items[0].id     = "b";
      items[0].format = "core:b";
      register_items(items, 1);
      event_save_items_fill(false);
      wram[2]         = 0x55;
      memcpy(b, image(), sizeof(b));
      items[0].id     = "c";
      items[0].format = "core:c";
      register_items(items, 1);
      event_save_items_fill(false);
      assert(!memcmp(image(), b, sizeof(b)));
      wram[3]         = 0x66;
      if (!pass)
      {
         assert(save_files());
         assert(disk_named_holds("/undo/", a, sizeof(a)) == 1);
         assert(disk_named_holds("/undo/", b, sizeof(b)) == 1);
         assert(disk_holds("/s/t.srm", image(), 12));
         ASSERT_FILES(2, 0, 2);
         assert(err_count == 0);
         continue;
      }
      assert(!save_files());
      event_save_items_final(true, false);
      assert(disk_holds("/s/t.srm", old, sizeof(old)));
      assert(disk_named_holds("RetroArch-recovery-", a, sizeof(a)) == 1);
      assert(disk_named_holds("RetroArch-recovery-", b, sizeof(b)) == 1);
      assert(disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
      ASSERT_FILES(0, 3, 3);
      assert(err_count == 0);
   }
}

/* A blob item that took the image waiting at a file kept another way
 * changes and leaves before its first write: its copy of that image is
 * still made at the next save of the files and its own image written,
 * or both recovered (pass 1). */
static void test_type_change_blob_gone(void)
{
   unsigned char old[12];
   unsigned char a[12];
   unsigned char b[12];
   struct retro_save_item items[2];
   unsigned pass;

   memset(old, 0x21, sizeof(old));
   for (pass = 0; pass < 2; pass++)
   {
      start();
      if (pass)
      {
         event_save_items_set_undo_dir(NULL);
         app_dir = "/app";
      }
      disk_put("/s/t.srm", old, sizeof(old));
      items[0] = buffer_item("a", "/s/t.srm");
      register_items(items, 1);
      event_load_save_files(false);
      wram[1]  = 0x44;
      memcpy(a, image(), sizeof(a));
      memset(&items[1], 0, sizeof(items[1]));
      items[1].id            = "b";
      items[1].path          = "/s/t.srm";
      items[1].storage       = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
      items[1].root          = RETRO_SAVE_ROOT_ABSOLUTE;
      items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
      register_items(items + 1, 1);
      event_save_items_fill(false);
      assert(blob_len == sizeof(a) && !memcmp(blob_state, a, sizeof(a)));
      blob_state[0] ^= 0xff;
      memcpy(b, blob_state, sizeof(b));
      register_items(NULL, 0);
      save_files();
      if (!pass)
      {
         assert(disk_holds("/s/t.srm", b, sizeof(b)));
         assert(disk_holds("/undo/t.srm", a, sizeof(a)));
      }
      else
      {
         assert(disk_holds("/s/t.srm", old, sizeof(old)));
         assert(disk_named_holds("RetroArch-recovery-", a, sizeof(a)) == 1);
         assert(disk_named_holds("RetroArch-recovery-", b, sizeof(b)) == 1);
      }
      assert(!save_item_files.num_orphans);
      ASSERT_FILES((pass ? 0 : 1), (pass ? 2 : 0), (pass ? 2 : 1));
      assert(err_count == 0);
   }
}

#ifdef HAVE_THREADS
/* An orphan a blob at its file refused waits to be written: the blob's
 * worker starts only once it is, so the copy holds it. */
static void test_waiting_refused_worker(void)
{
   unsigned char old[12];
   unsigned char a[12];
   struct retro_save_item items[2];

   start();
   app_dir = "/app";
   memset(old, 0x21, sizeof(old));
   disk_put("/s/t.srm", old, sizeof(old));
   items[0] = buffer_item("a", "/s/t.srm");
   register_items(items, 1);
   event_load_save_files(false);
   wram[1]  = 0x44;
   memcpy(a, image(), sizeof(a));
   register_items(NULL, 0);
   memset(&items[1], 0, sizeof(items[1]));
   items[1].id            = "b";
   items[1].path          = "/s/t.srm";
   items[1].storage       = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   items[1].root          = RETRO_SAVE_ROOT_ABSOLUTE;
   items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   register_items(items + 1, 1);
   memcpy(blob_state, "keep", 4);
   blob_len    = 4;
   blob_refuse = true;
   event_save_items_fill(false);
   blob_refuse = false;
   assert(autosave_init(false, 1));
   assert(save_files_unreaped());
   /* Killed here. */
   assert(disk_holds("/undo/t.srm", a, sizeof(a)));
   assert(disk_holds("/s/t.srm", "keep", 4));
   ASSERT_FILES(1, 0, 1);
   assert(!err_count && !save_item_files.num_orphans);
   frame_start_reap();
   assert(disk_holds("/undo/t.srm", a, sizeof(a)));
   assert(disk_holds("/s/t.srm", "keep", 4));
   ASSERT_FILES(1, 0, 1);
   assert(!err_count);
   autosave_check();
   event_save_items_fill(false);
   assert(save_item_files.files[0].autosave);
   autosave_deinit();
}
#endif

/* No memory to copy the waiting image a type change takes: the orphan
 * keeps its write and the fill reads the file, so the copy is of the
 * file once the orphan's image is there. */
static void test_type_change_no_memory(void)
{
   unsigned char old[12];
   unsigned char want[12];
   struct retro_save_item items[1];

   start();
   app_dir = "/app";
   memset(old, 0x21, sizeof(old));
   disk_put("/s/t.srm", old, sizeof(old));
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   retro_atomic_store_release_int(&fail_main_malloc_size, 12);
   event_save_items_fill(false);
   retro_atomic_store_release_int(&fail_main_malloc_size, 0);
   wram[2] = 9;
#ifdef HAVE_THREADS
   /* Its copy job is held until SAVE_FILES is done. */
   set_hold(&hold_undo, true);
#endif
   assert(event_save_files(true, false, NULL));
#ifdef HAVE_THREADS
   wait_held(1);
   set_hold(&hold_undo, false);
   copies_wait(false);
#endif
   /* Killed here. */
   assert(disk_holds("/undo/t.srm", want, sizeof(want)));
   assert(disk_holds("/s/t.srm", image(), 12));
   assert(image()[1] == 0x21);
   ASSERT_FILES(1, 0, 0);
   assert(!err_count && !save_item_files.num_orphans);
   frame_start_reap();
   assert(disk_holds("/undo/t.srm", want, sizeof(want)));
   assert(disk_holds("/s/t.srm", image(), 12));
   ASSERT_FILES(1, 0, 1);
   assert(strstr(osd_last, "/s/t.srm|/undo/t.srm"));
   assert(!err_count);
}

/* A blob that registers again from inside blob_load() and takes the
 * image: its file is not kept, so it is written with no copy, also
 * when copies fail (pass 1). */
static void test_blob_reregister_taken(void)
{
   struct retro_save_item items[1];
   unsigned pass;

   for (pass = 0; pass < 2; pass++)
   {
      start();
      app_dir = "/app";
      memset(&items[0], 0, sizeof(items[0]));
      items[0].id            = "flash";
      items[0].path          = "/s/game.flash";
      items[0].storage       = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
      items[0].root          = RETRO_SAVE_ROOT_ABSOLUTE;
      items[0].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
      disk_put("/s/game.flash", "other", 5);
      blob_reregister = true;
      rereg_items     = items;
      rereg_n         = 1;
      set_fail_undo(pass == 1);
      register_items(items, 1);
      assert(event_load_save_files(false));
      assert(blob_len == 5 && !memcmp(blob_state, "other", 5));
      memcpy(blob_state, "newer", 5);
      assert(save_files());
      assert(disk_holds("/s/game.flash", "newer", 5));
      ASSERT_FILES(0, 0, 0);
      assert(!err_count);
   }
}

/* No memory to keep a type change's item that leaves: its image and
 * the old type's image it owed a copy of each go to a recovery file. */
static void test_orphan_no_memory_type_change(void)
{
   static const unsigned char zero[12];
   unsigned char want[12];
   struct retro_save_item items[1];

   start();
   app_dir = "/app";
   event_save_items_set_undo_dir(NULL);
   disk_put("/s/t.srm", image(), 12);
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   event_save_items_fill(false);
   assert(save_item_files.files[0].guard.copy_image);
   wram[2] = 9;
   retro_atomic_store_release_int(&fail_reallocs, 1);
   register_items(NULL, 0);
   retro_atomic_store_release_int(&fail_reallocs, 0);
   assert(!save_item_files.num_orphans);
   assert(disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
   ASSERT_FILES(0, 2, 2);
   assert(err_count == 0);
   assert(disk_holds("/s/t.srm", zero, 12));
}

#ifdef HAVE_THREADS
/* A retired worker that exits owing a copy it could not make passes it
 * to the item now at its file, which then keeps the file as its own
 * debt would; that item's worker starts once the reap has done so. The
 * image both hold is recovered once. */
static void test_retired_copy_passes(void)
{
   unsigned char old[12];
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned i;
   unsigned n;

   start();
   event_save_items_set_undo_dir(NULL);
   app_dir         = "/app";
   memset(old, 0x21, sizeof(old));
   disk_put("/s/t.srm", old, sizeof(old));
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   event_save_items_fill(false);
   h       = start_autosave(false);
   wram[2] = 9;
   capture(h);
   n       = osd_count;
   /* Ended owing the copy: its last write fails. */
   register_items(NULL, 0);
   for (i = 0; i < 5000 && !retro_atomic_load_acquire_int(&h->exited); i++)
      retro_sleep(1);
   assert(retro_atomic_load_acquire_int(&h->exited));
   /* The same id again, not marked: its fill takes the image only. */
   register_items(items, 1);
   event_save_items_fill(false);
   assert(!save_item_files.files[0].guard.copy_first);
   assert(!save_item_files.files[0].autosave);
   /* SAVE_FILES before the reap: the copy the old worker owes comes
    * first and cannot be made, so the file is kept and the image goes
    * to the recovery file, once. */
   assert(!save_files());
   assert(disk_holds("/s/t.srm", old, sizeof(old)));
   assert(osd_count == n + 1);
   ASSERT_FILES(0, 3, 1);
   assert(err_count == 0);
   autosave_check();
   assert(!autosave_state.num_retired && osd_count == n + 2);
   assert(save_item_files.files[0].guard.copy_first);
   assert(save_item_files.files[0].guard.keep_file);
   assert(save_item_files.files[0].guard.type_changed);
   assert(!save_files());
   assert(disk_holds("/s/t.srm", old, sizeof(old)));
   assert(osd_count == n + 2);
   ASSERT_FILES(0, 2, 2);
   assert(err_count == 0);
   event_save_items_fill(false);
   assert((h = save_item_files.files[0].autosave));
   assert(retro_atomic_load_acquire_int(&h->keep_file));
   autosave_deinit();
}
#endif

#ifdef HAVE_THREADS
/* SAVE_FILES at a file whose retired worker has not made the copy it
 * owes hands the newer image to that worker, which writes it after its
 * copy. The copy is told once. */
static void test_save_files_retired_copy(void)
{
   unsigned char before[12];
   unsigned char want[12];
   struct retro_save_item items[1];
   unsigned i;
   unsigned n;
   unsigned logs;

   start();
   disk_put("/s/t.srm", image(), 12);
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   memcpy(before, image(), sizeof(before));
   wram[1]         = 0x44;
   memcpy(want, image(), sizeof(want));
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   event_save_items_set_undo_dir(NULL);
   event_save_items_fill(false);
   event_save_items_set_undo_dir("/undo");
   /* Its copy at the start fails: still owed. */
   set_fail_undo(true);
   start_autosave(false);
   set_fail_undo(false);
   /* Ended before its first write: its last one copies first, and is
    * held once that copy's temp is written. */
   set_hold(&hold_temp, true);
   n    = osd_count;
   logs = copy_logs;
   register_items(NULL, 0);
   wait_held(1);
   /* The same id again, not marked: it owes nothing. */
   register_items(items, 1);
   event_save_items_fill(false);
   assert(!save_item_files.files[0].guard.copy_first);
   wram[2] = 9;
   assert(event_save_files(true, false, NULL));
   assert(!disk_has("/undo/t.srm") && disk_holds("/s/t.srm", before, 12));
   assert(copy_logs == logs && osd_count == n);
   set_hold(&hold_temp, false);
   for (i = 0; autosave_state.num_retired && i < 5000; i++)
   {
      autosave_check();
      retro_sleep(1);
   }
   assert(!autosave_state.num_retired);
   assert(disk_holds("/undo/t.srm", want, sizeof(want)));
   assert(disk_holds("/s/t.srm", image(), 12));
   assert(copy_logs == logs + 1 && osd_count == n + 1);
   assert(strstr(osd_last, "/s/t.srm|/undo/t.srm"));
   assert(!disk_has("/undo/t-2.srm"));
   assert(failed_renames == 0 && !disk_named(".tmp"));
   ASSERT_FILES(1, 0, 1);
   assert(err_count == 0);
   autosave_deinit();
}

/* A registration from retro_run() is reaped before the next fill: the
 * retired worker's failed last image goes to a recovery file the new
 * item, which never took that image, does not write over. */
static void test_reap_before_fill(void)
{
   unsigned char old[12];
   unsigned char last[12];
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned i;

   start();
   event_save_items_set_undo_dir(NULL);
   app_dir         = "/app";
   memset(old, 0x21, sizeof(old));
   disk_put("/s/t.srm", old, sizeof(old));
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   event_save_items_fill(false);
   h       = start_autosave(false);
   wram[2] = 9;
   capture(h);
   memcpy(last, image(), sizeof(last));
   items[0].id     = "c";
   items[0].format = "core:c";
   register_items(items, 1);
   for (i = 0; i < 5000 && !retro_atomic_load_acquire_int(&h->exited); i++)
      retro_sleep(1);
   assert(retro_atomic_load_acquire_int(&h->final_failed));
   autosave_check();
   assert(!autosave_state.num_retired);
   event_save_items_fill(false);
   assert(!memcmp(image(), old, sizeof(old)));
   wram[3] = 7;
   autosave_deinit();
   event_save_items_final(true, false);
   assert(disk_holds("/s/t.srm", old, sizeof(old)));
   assert(disk_named_holds("RetroArch-recovery-", last, sizeof(last)) == 1);
   assert(disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
}

/* A blob at the file refuses a retired worker's last image: it does
 * not share that worker's recovery file, so its own failed write never
 * replaces the image there. Pass 1 registers again from inside
 * blob_load(). */
static void test_retired_take_refused(void)
{
   unsigned char old[12];
   unsigned char last[12];
   struct retro_save_item items[2];
   autosave_t *h;
   unsigned i;
   unsigned pass;

   memset(old, 0x21, sizeof(old));
   for (pass = 0; pass < 2; pass++)
   {
      start();
      event_save_items_set_undo_dir(NULL);
      app_dir         = "/app";
      disk_put("/s/t.srm", old, sizeof(old));
      items[0]        = buffer_item("a", "/s/t.srm");
      items[0].format = "core:a";
      register_items(items, 1);
      event_load_save_files(false);
      items[0].id     = "b";
      items[0].format = "core:b";
      register_items(items, 1);
      event_save_items_fill(false);
      h       = start_autosave(false);
      wram[2] = 9;
      capture(h);
      memcpy(last, image(), sizeof(last));
      /* Ended owing the copy: its last write fails. */
      register_items(NULL, 0);
      for (i = 0; i < 5000 && !retro_atomic_load_acquire_int(&h->exited);
            i++)
         retro_sleep(1);
      assert(retro_atomic_load_acquire_int(&h->final_failed));
      memset(&items[1], 0, sizeof(items[1]));
      items[1].id            = "c";
      items[1].path          = "/s/t.srm";
      items[1].storage       = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
      items[1].root          = RETRO_SAVE_ROOT_ABSOLUTE;
      items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
      register_items(items + 1, 1);
      memcpy(blob_state, "keep", 4);
      blob_len    = 4;
      blob_refuse = true;
      if (pass)
      {
         blob_reregister = true;
         rereg_items     = items + 1;
         rereg_n         = 1;
      }
      event_save_items_fill(false);
      assert(blob_len == 4 && !memcmp(blob_state, "keep", 4));
      blob_refuse = false;
      autosave_check();
      assert(!autosave_state.num_retired);
      autosave_deinit();
      event_save_items_final(true, false);
      assert(disk_holds("/s/t.srm", old, sizeof(old)));
      assert(disk_named_holds("RetroArch-recovery-", last, sizeof(last))
            == 1);
      assert(disk_named_holds("RetroArch-recovery-", "keep", 4) == 1);
   ASSERT_FILES(0, 3, 4);
   assert(err_count == 0);
   }
}
#endif

/* A kept id moved onto the file of an item kept another way is
 * guarded as a new id there is: filled from the image waiting for
 * that file, which its copy holds. One kept at its own file is not
 * guarded, whatever its tag. */
static void test_kept_move_type_change(void)
{
   unsigned char want[12];
   unsigned char want_card[6];
   struct retro_save_item items[2];

   /* The cart moves onto the file of the gone card. */
   start();
   disk_put("/s/a.srm", image(), 12);
   disk_put("/s/c.sav", card, sizeof(card));
   items[0]        = buffer_item("a", "/s/a.srm");
   items[0].format = "core:a";
   items[1]        = card_item("c", "/s/c.sav");
   items[1].format = "core:c";
   register_items(items, 2);
   event_load_save_files(false);
   card[1] = 0x44;
   memcpy(want_card, card, sizeof(want_card));
   items[0].path = "/s/c.sav";
   register_items(items, 1);
   assert(save_item_files.files[0].guard.type_changed);
   assert(save_item_files.num_orphans == 2);
   event_save_items_fill(false);
   assert(save_item_files.num_orphans == 1);
   assert(!memcmp(wram, want_card, sizeof(want_card)));
   wram[7] = 9;
   assert(save_files());
   assert(disk_holds("/undo/c.sav", want_card, sizeof(want_card)));
   assert(disk_holds("/s/c.sav", image(), 12));
   assert(osd_count == 1 && strstr(osd_last, "/undo/c.sav"));

   /* Two kept ids swap files of different tags: both guarded, and
    * each copy holds the other's last image. */
   start();
   disk_put("/s/a.srm", image(), 12);
   disk_put("/s/c.sav", card, sizeof(card));
   items[0]        = buffer_item("a", "/s/a.srm");
   items[0].format = "core:a";
   items[1]        = card_item("c", "/s/c.sav");
   items[1].format = "core:c";
   register_items(items, 2);
   event_load_save_files(false);
   wram[1] = 0x55;
   card[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   memcpy(want_card, card, sizeof(want_card));
   items[0].path = "/s/c.sav";
   items[1].path = "/s/a.srm";
   register_items(items, 2);
   assert(save_item_files.files[0].guard.type_changed);
   assert(save_item_files.files[1].guard.type_changed);
   event_save_items_fill(false);
   assert(save_item_files.num_orphans == 0);
   assert(save_files());
   assert(disk_holds("/undo/c.sav", want_card, sizeof(want_card)));
   assert(disk_holds("/undo/a.srm", want, sizeof(want)));
   assert(osd_count == 2);

   /* Kept at their own files, a tag grown by "+<memory>": carried, no
    * guard. */
   items[0].format = "core:a+rtc";
   items[1].format = "core:c+rtc";
   register_items(items, 2);
   assert(save_item_files.files[0].taken_up);
   assert(save_item_files.files[1].taken_up);
   assert(!save_item_files.files[0].guard.type_changed);
   assert(!save_item_files.files[1].guard.type_changed);

   /* Under another tag: a new layout, guarded. */
   items[0].format = "core:b";
   register_items(items, 2);
   assert(save_item_files.files[0].guard.type_changed);
   assert(!save_item_files.files[1].guard.type_changed);
}

#ifdef HAVE_THREADS
/* A refused blob at /s/game.blob, holding @old, with no worker: the
 * fill owes its copy. */
static void load_refused_blob(const unsigned char *old)
{
   struct retro_save_item items[1];
   disk_put("/s/game.blob", old, 4);
   items[0]              = buffer_item("blob", "/s/game.blob");
   items[0].storage      = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   items[0].segments     = NULL;
   items[0].num_segments = 0;
   blob_refuse           = true;
   register_items(items, 1);
   assert(!event_load_save_files(false));
   blob_refuse           = false;
   blob_len              = 4;
}

/* An owed copy with no worker is made off the main thread: SAVE_FILES
 * reads nothing and writes no copy itself. Passes: a refused blob; a
 * file longer than an EXACT_SIZE item. */
static void test_copy_off_main(void)
{
   static const unsigned char old[4] = {7, 7, 7, 7};
   unsigned char file[16];
   struct retro_save_item items[1];

   start();
   load_refused_blob(old);
   memset(blob_state, 3, 4);
   main_reads = 0;
   assert(save_files());
   assert(main_reads == 0 && main_undo_writes == 0);
   assert(disk_holds("/undo/game.blob", old, 4));
   assert(disk_holds("/s/game.blob", blob_state, 4));
   assert(copy_logs == 1);

   start();
   fill_bytes(file, sizeof(file), 0x30);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0]       = buffer_item("srm", "/s/game.srm");
   items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   register_items(items, 1);
   assert(event_load_save_files(false));
   main_reads = 0;
   assert(save_files());
   assert(main_reads == 0 && main_undo_writes == 0);
   assert(disk_holds("/undo/game.srm", file, 16));
   assert(disk_holds("/s/game.srm", file, 12));
   assert(!save_item_files.files[0].guard.copy_first);
}

/* SAVE_FILES while the copy job runs hands it the image, which it
 * writes after its copy: no recovery file, no notice. A second hand-off
 * replaces the first, which is never written. */
static void test_copy_job_handoff(void)
{
   static const unsigned char old[4] = {7, 7, 7, 7};
   unsigned n;
   unsigned logs;

   start();
   app_dir = "/app";
   set_hold(&hold_temp, true);
   load_refused_blob(old);
   wait_held(1);
   n    = osd_count;
   logs = copy_logs;
   memset(blob_state, 3, 4);
   assert(event_save_files(true, false, NULL));
   assert(disk_holds("/s/game.blob", old, 4) && save_writes == 0);
   memset(blob_state, 5, 4);
   assert(event_save_files(true, false, NULL));
   assert(disk_holds("/s/game.blob", old, 4) && save_writes == 0);
   assert(osd_count == n && !disk_named("RetroArch-recovery-"));
   set_hold(&hold_temp, false);
   copies_wait(false);
   ASSERT_FILES(1, 0, 1);
   assert(err_count == 0);
   copies_wait(true);
   assert(disk_holds("/undo/game.blob", old, 4));
   assert(disk_holds("/s/game.blob", blob_state, 4) && save_writes == 1);
   assert(osd_count == n && !disk_named("RetroArch-recovery-"));
   assert(copy_logs == logs + 1 && main_undo_writes == 0);
   assert(!save_item_files.files[0].guard.copy_first);
   assert(failed_renames == 0 && !disk_named(".tmp"));
   ASSERT_FILES(1, 0, 1);
   assert(err_count == 0);
}

/* A copy job that has finished leaves SAVE_FILES its result: the main
 * thread writes the image itself, or takes the failed-copy path (pass
 * 1): the file kept, the image in the recovery file, named on screen. */
static void test_copy_job_done(void)
{
   static const unsigned char old[4] = {7, 7, 7, 7};
   unsigned pass;

   for (pass = 0; pass < 2; pass++)
   {
      unsigned n;
      start();
      app_dir = "/app";
      set_fail_undo(pass == 1);
      load_refused_blob(old);
      copies_wait(false);
      assert(autosave_state.num_retired == 1);
      n = osd_count;
      memset(blob_state, 3, 4);
      assert(event_save_files(true, false, NULL) == !pass);
      assert(main_undo_writes == 0);
      if (!pass)
      {
         assert(disk_holds("/undo/game.blob", old, 4));
         assert(disk_holds("/s/game.blob", blob_state, 4));
         assert(copy_logs == 1 && osd_count == n);
         continue;
      }
      assert(disk_holds("/s/game.blob", old, 4));
      assert(disk_named_holds("RetroArch-recovery-", blob_state, 4) == 1);
      assert(osd_count == n + 1 && strstr(osd_last, "/app/RetroArch-"));
      assert(save_item_files.files[0].guard.copy_first);
   }
}

/* A copy owed to a worker resting its interval wakes it: the copy is
 * made before its next write, though nothing changed. */
static void test_worker_woken_for_copy(void)
{
   unsigned char file[12];
   struct retro_save_item items[1];
   autosave_t *h;

   start();
   fill_bytes(file, sizeof(file), 0x50);
   disk_put("/s/game.srm", file, sizeof(file));
   memcpy(wram, file, sizeof(wram));
   memcpy(iram, file + 8, sizeof(iram));
   items[0]       = buffer_item("srm", "/s/game.srm");
   items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   register_items(items, 1);
   assert(event_load_save_files(false));
   h           = start_autosave(false);
   h->interval = 100;
   autosave_lock();
   autosave_unlock();
   set_hold(&hold_renamed, true);
   items[0].num_segments = 1;
   register_items(items, 1);
   wait_held(1);
   slock_lock(disk_lock);
   assert(undo_writes == 1 && save_writes == 0);
   slock_unlock(disk_lock);
   assert(disk_holds("/undo/game.srm", file, 12));
   set_hold(&hold_renamed, false);
   /* Asked after the copy is told. */
   wait_requested(h);
   event_save_items_fill(false);
   assert(copy_logs == 1);
   autosave_deinit();
}

static void release_at_close(void *data)
{
   (void)data;
   slock_lock(disk_lock);
   while (!closing)
      scond_wait(disk_cond, disk_lock);
   hold_temp = false;
   scond_broadcast(disk_cond);
   slock_unlock(disk_lock);
}

/* The close joins a copy job still running: the final write is the
 * normal one, with no notice. */
static void test_close_joins_copy(void)
{
   static const unsigned char old[4] = {7, 7, 7, 7};
   sthread_t *t;
   unsigned n;

   start();
   app_dir = "/app";
   set_hold(&hold_temp, true);
   load_refused_blob(old);
   wait_held(1);
   n = osd_count;
   memset(blob_state, 3, 4);
   assert((t = sthread_create(release_at_close, NULL)));
   slock_lock(disk_lock);
   closing = true;
   scond_broadcast(disk_cond);
   slock_unlock(disk_lock);
   event_save_items_final(true, false);
   sthread_join(t);
   assert(disk_holds("/undo/game.blob", old, 4));
   assert(disk_holds("/s/game.blob", blob_state, 4));
   assert(osd_count == n && !disk_named("RetroArch-recovery-"));
   assert(copy_logs == 1 && main_undo_writes == 0);
   assert(disk_count_named("/undo/") == 1);
}

/* A kept file whose copy failed: SAVE_FILES sends the image to the
 * recovery file before it returns and tries the copy again without it.
 * The recovery file is named once and kept current; once a copy is
 * made, the image is written. */
static void test_copy_failed_again(void)
{
   static const unsigned char old[4] = {7, 7, 7, 7};
   unsigned n;

   start();
   app_dir = "/app";
   set_fail_undo(true);
   load_refused_blob(old);
   copies_wait(true);
   assert(save_item_files.files[0].guard.copy_failed);
   n = osd_count;
   memset(blob_state, 3, 4);
   assert(!event_save_files(true, false, NULL));
   assert(disk_named_holds("RetroArch-recovery-", blob_state, 4) == 1);
   assert(osd_count == n + 1 && disk_holds("/s/game.blob", old, 4));
   copies_wait(true);
   memset(blob_state, 5, 4);
   assert(!event_save_files(true, false, NULL));
   assert(disk_named_holds("RetroArch-recovery-", blob_state, 4) == 1);
   assert(disk_count_named("RetroArch-recovery-") == 1);
   assert(osd_count == n + 1);
   copies_wait(true);
   set_fail_undo(false);
   assert(!event_save_files(true, false, NULL));
   copies_wait(true);
   assert(disk_holds("/undo/game.blob", old, 4) && copy_logs == 1);
   assert(save_files() && disk_holds("/s/game.blob", blob_state, 4));
   assert(osd_count == n + 1 && disk_count_named("RetroArch-recovery-") == 1);
}

/* A copy job handed an image whose copy then fails writes the image to
 * the recovery file itself before it exits; the reap names it, and the
 * next SAVE_FILES keeps that file current. */
static void test_copy_job_fails_handed(void)
{
   static const unsigned char old[4] = {7, 7, 7, 7};
   unsigned n;

   start();
   app_dir = "/app";
   set_hold(&hold_undo, true);
   load_refused_blob(old);
   wait_held(1);
   n = osd_count;
   memset(blob_state, 3, 4);
   assert(event_save_files(true, false, NULL));
   set_fail_undo(true);
   set_hold(&hold_undo, false);
   copies_wait(false);
   assert(disk_named_holds("RetroArch-recovery-", blob_state, 4) == 1);
   assert(osd_count == n);
   autosave_reap(true);
   assert(osd_count == n + 1 && strstr(osd_last, "/app/RetroArch-"));
   assert(save_item_files.files[0].guard.recovered);
   memset(blob_state, 5, 4);
   assert(!event_save_files(true, false, NULL));
   copies_wait(true);
   assert(disk_named_holds("RetroArch-recovery-", blob_state, 4) == 1);
   assert(disk_count_named("RetroArch-recovery-") == 1);
   assert(osd_count == n + 1 && disk_holds("/s/game.blob", old, 4));
}

/* An orphan's image handed to a copy job whose copy of the old type's
 * image fails: each of the two images reaches a recovery file. */
static void test_orphan_job_fails_handed(void)
{
   unsigned char old[12];
   unsigned char want[12];
   unsigned char last[12];
   struct retro_save_item items[2];

   start();
   app_dir = "/app";
   memset(old, 0x21, sizeof(old));
   disk_put("/s/t.srm", old, sizeof(old));
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   set_hold(&hold_undo, true);
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   event_save_items_fill(false);
   wait_held(1);
   wram[2] = 9;
   memcpy(last, image(), sizeof(last));
   items[1] = buffer_item("c", "/s/u.srm");
   register_items(items + 1, 1);
   assert(save_item_files.num_orphans == 1);
   assert(save_item_files.orphans[0].guard.job);
   assert(event_save_files(true, false, NULL));
   set_fail_undo(true);
   set_hold(&hold_undo, false);
   copies_wait(false);
   ASSERT_FILES(0, 2, 0);
   assert(err_count == 0);
   copies_wait(true);
   assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
   assert(disk_holds("/s/t.srm", old, 12));
ASSERT_FILES(0, 2, 2);
assert(err_count == 0);
}

/* An orphan's copy job that holds the only image of the old type and
 * cannot copy it writes it to a recovery file before it exits; the
 * reap names it once. Pass 0's orphan hands it an image too; pass 1's
 * was taken by a second type change. */
static void test_orphan_job_dumps_copy(void)
{
   unsigned char old[12];
   unsigned char want[12];
   unsigned char last[12];
   struct retro_save_item items[2];
   unsigned pass;
   unsigned n;

   for (pass = 0; pass < 2; pass++)
   {
      start();
      app_dir = "/app";
      memset(old, 0x21, sizeof(old));
      disk_put("/s/t.srm", old, sizeof(old));
      items[0]        = buffer_item("a", "/s/t.srm");
      items[0].format = "core:a";
      register_items(items, 1);
      event_load_save_files(false);
      wram[1] = 0x44;
      memcpy(want, image(), sizeof(want));
      set_hold(&hold_undo, true);
      items[0].id     = "b";
      items[0].format = "core:b";
      register_items(items, 1);
      event_save_items_fill(false);
      wait_held(1);
      wram[2] = 9;
      memcpy(last, image(), sizeof(last));
      if (pass == 0)
      {
         items[1] = buffer_item("c", "/s/u.srm");
         register_items(items + 1, 1);
      }
      else
      {
         items[0].id     = "c";
         items[0].format = "core:c";
         register_items(items, 1);
         event_save_items_fill(false);
      }
      assert(save_item_files.num_orphans == 1);
      assert(save_item_files.orphans[0].guard.job);
      assert(event_save_files(true, false, NULL));
      n = osd_count;
      set_fail_undo(true);
      set_hold(&hold_undo, false);
      copies_wait(false);
      assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
      assert(osd_count == n);
      /* Pass 1: c's image, the old type's too, in one untold file. */
      if (pass)
      {
         assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
         ASSERT_FILES(0, 2, n);
      }
      autosave_reap(true);
      assert(osd_count == n + (pass == 0 ? 2 : 1));
      assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
      assert(disk_holds("/s/t.srm", old, 12));
      set_fail_undo(false);
      if (pass)
      {
         assert(told_holding(last, 12) == 0);
         assert(save_files());
         assert(disk_holds("/s/t.srm", last, 12));
         assert(disk_holds("/undo/t.srm", last, 12));
         ASSERT_FILES(1, 1, n + 2);
         assert(err_count == 0);
      }
   }
}

/* SAVE_FILES after a copy job's copy failed, before its reap: the image
 * goes to the recovery file once, the old type's image the record
 * still owes to one of its own, and the reap adds none. */
static void test_failed_job_detached(void)
{
   unsigned char old[12];
   unsigned char want[12];
   unsigned char last[12];
   struct retro_save_item items[1];
   unsigned n;

   start();
   app_dir = "/app";
   memset(old, 0x21, sizeof(old));
   disk_put("/s/t.srm", old, sizeof(old));
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   set_fail_undo(true);
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   event_save_items_fill(false);
   copies_wait(false);
   /* Killed here: the job wrote the old type's image itself. */
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
   assert(disk_count_named("RetroArch-recovery-") == 1);
   assert(disk_holds("/s/t.srm", old, sizeof(old)));
   n = osd_count;
   wram[2] = 9;
   memcpy(last, image(), sizeof(last));
   assert(!event_save_files(true, false, NULL));
   assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(osd_count == n + 2);
   copies_wait(true);
   assert(osd_count == n + 2 && !err_count);
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(!disk_named("/undo/"));
   assert(save_item_files.files[0].guard.copy_image);
}

/* A type change while the replaced item's worker still writes its last
 * image: that worker copies the file after its write, then writes the
 * image SAVE_FILES hands it. No recovery file, and no notice but the
 * type change's. */
static void test_type_change_retired_busy(void)
{
   unsigned char want[12];
   struct retro_save_item items[1];
   unsigned n;

   start();
   app_dir = "/app";
   disk_put("/s/t.srm", image(), 12);
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   start_autosave(false);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   set_hold(&hold_save_renamed, true);
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   wait_held(1);
   event_save_items_fill(false);
   assert(save_item_files.files[0].guard.copy_image);
   assert(!save_item_files.files[0].autosave);
   n = osd_count;
   wram[2] = 9;
   event_save_files(true, false, NULL);
   assert(osd_count == n && !disk_named("RetroArch-recovery-"));
   set_hold(&hold_save_renamed, false);
   retired_join(false);
   autosave_check();
   assert(!autosave_state.num_retired);
   assert(disk_holds("/undo/t.srm", want, sizeof(want)));
   assert(disk_holds("/s/t.srm", image(), 12));
   assert(osd_count == n + 1 && strstr(osd_last, "/s/t.srm|/undo/t.srm"));
   assert(!save_item_files.files[0].guard.copy_first);
   assert(!save_item_files.files[0].guard.copy_image);
   ASSERT_FILES(1, 0, 1);
   assert(err_count == 0);
   autosave_deinit();
}

/* The replaced item's worker still owes its own copy when a type change
 * owes another at its file: it makes both, its own first, then writes
 * the image SAVE_FILES hands it. */
static void test_retired_owes_and_type_change(void)
{
   unsigned char file[16];
   unsigned char want[12];
   struct retro_save_item items[1];
   unsigned n;

   start();
   app_dir = "/app";
   fill_bytes(file, sizeof(file), 0x30);
   disk_put("/s/t.srm", file, sizeof(file));
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   items[0].flags  = RETRO_SAVE_FLAG_EXACT_SIZE;
   register_items(items, 1);
   load_for_worker();
   set_hold(&hold_undo, true);
   assert(autosave_init(false, 1));
   wait_held(1);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   event_save_items_fill(false);
   n = osd_count;
   wram[2] = 9;
   event_save_files(true, false, NULL);
   assert(osd_count == n && !disk_named("RetroArch-recovery-"));
   set_hold(&hold_undo, false);
   retired_join(false);
   autosave_check();
   assert(!autosave_state.num_retired);
   assert(disk_holds("/undo/t.srm", file, 16));
   assert(disk_named_holds("/undo/", want, 12) == 1);
   assert(disk_holds("/s/t.srm", image(), 12));
   assert(!disk_named("RetroArch-recovery-"));
   assert(!save_item_files.files[0].guard.copy_first);
   ASSERT_FILES(2, 0, 1);
   assert(err_count == 0);
   autosave_deinit();
}

/* A cut after a copy job wrote a handed image: the next image is
 * written only after a copy of the file that holds the cut bytes, by
 * that job while it still runs (0) or by a new one once it has
 * finished (1). */
static void test_cut_after_handed(void)
{
   unsigned char file[16];
   unsigned char first[12];
   struct retro_save_item items[1];
   unsigned pass;

   for (pass = 0; pass < 2; pass++)
   {
      start();
      app_dir = "/app";
      fill_bytes(file, sizeof(file), 0x30);
      disk_put("/s/game.srm", file, sizeof(file));
      items[0]       = buffer_item("srm", "/s/game.srm");
      items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
      set_hold(&hold_temp, true);
      register_items(items, 1);
      assert(event_load_save_files(false));
      wait_held(1);
      wram[0] = 0x11;
      memcpy(first, image(), sizeof(first));
      assert(event_save_files(true, false, NULL));
      set_hold(&hold_save_renamed, !pass);
      set_hold(&hold_temp, false);
      if (!pass)
         wait_held(2);
      else
         copies_wait(false);
      assert(disk_holds("/s/game.srm", first, 12));
      items[0].num_segments = 1;
      register_items(items, 1);
      wram[1] = 0x22;
      assert(event_save_files(true, false, NULL));
      set_hold(&hold_save_renamed, false);
      copies_wait(true);
      assert(disk_holds("/undo/game.srm", file, 16));
      assert(disk_named_holds("/undo/", first, 12) == 1);
      assert(disk_holds("/s/game.srm", wram, 8));
      assert(!save_item_files.files[0].guard.copy_first);
   ASSERT_FILES(2, 0, 0);
   assert(err_count == 0);
   }
}
/* A type change's copy that fails sends the old type's image to a
 * recovery file of its own at once, told once; the next write's image
 * goes to the recovery file, and the copy made later is told without
 * the type change's notice. */
static void test_type_change_copy_fails(void)
{
   unsigned char old[12];
   unsigned char want[12];
   unsigned char last[12];
   struct retro_save_item items[1];
   unsigned n;

   start();
   app_dir = "/app";
   memset(old, 0x21, sizeof(old));
   disk_put("/s/t.srm", old, sizeof(old));
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   set_fail_undo(true);
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   n = osd_count;
   event_save_items_fill(false);
   /* Killed here: the job wrote the old type's image itself. */
   copies_wait(false);
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
   assert(disk_count_named("RetroArch-recovery-") == 1);
   assert(disk_holds("/s/t.srm", old, sizeof(old)) && osd_count == n);
   autosave_reap(true);
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
   assert(disk_count_named("RetroArch-recovery-") == 1);
   assert(osd_count == n + 1 && !disk_named("/undo/"));
   assert(disk_holds("/s/t.srm", old, sizeof(old)));
   set_fail_undo(false);
   wram[2] = 9;
   memcpy(last, image(), sizeof(last));
   assert(!event_save_files(true, false, NULL));
   assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(osd_count == n + 2);
   copies_wait(true);
   assert(disk_holds("/undo/t.srm", want, sizeof(want)));
   assert(disk_count_named("/undo/") == 1 && copy_logs == 1);
   assert(osd_count == n + 2);
   assert(save_files() && disk_holds("/s/t.srm", last, 12));
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(disk_count_named("/undo/") == 1);
   assert(osd_count == n + 2 && !err_count);
}

/* Two SAVE_FILES while the replaced item's worker carries the type
 * change's copy: the second image keeps the copy the first carried,
 * and the worker's last image still lands first, held after its rename
 * (0) or before (1). */
static void test_carried_copy_handed_twice(void)
{
   unsigned char want[12];
   struct retro_save_item items[1];
   unsigned pass, n, logs;
   bool *hold;

   for (pass = 0; pass < 2; pass++)
   {
      hold = pass ? &hold_save_temp : &hold_save_renamed;
      start();
      app_dir = "/app";
      disk_put("/s/t.srm", image(), 12);
      items[0]        = buffer_item("a", "/s/t.srm");
      items[0].format = "core:a";
      register_items(items, 1);
      event_load_save_files(false);
      start_autosave(false);
      wram[1] = 0x44;
      memcpy(want, image(), sizeof(want));
      set_hold(hold, true);
      items[0].id     = "b";
      items[0].format = "core:b";
      register_items(items, 1);
      wait_held(1);
      event_save_items_fill(false);
      assert(save_item_files.files[0].guard.copy_image);
      n    = osd_count;
      logs = copy_logs;
      wram[2] = 9;
      assert(event_save_files(true, false, NULL));
      wram[3] = 0x55;
      assert(event_save_files(true, false, NULL));
      assert(osd_count == n && !disk_named("RetroArch-recovery-"));
      set_hold(hold, false);
      retired_join(false);
      ASSERT_FILES(1, 0, 0);
      assert(err_count == 0);
      retired_join(false);
      autosave_check();
      assert(!autosave_state.num_retired);
      assert(disk_holds("/undo/t.srm", want, sizeof(want)));
      assert(disk_holds("/s/t.srm", image(), 12));
      assert(disk_count_named("/undo/") == 1);
      assert(!disk_named("RetroArch-recovery-"));
      assert(osd_count == n + 1 && strstr(osd_last, "/s/t.srm|/undo/t.srm"));
      assert(copy_logs == logs + 1 && !err_count);
      assert(!save_item_files.files[0].guard.copy_first);
      ASSERT_FILES(1, 0, 1);
      assert(err_count == 0);
      autosave_deinit();
   }
}

/* Two SAVE_FILES after a cut while the copy job that wrote a handed
 * image still runs: the second image keeps the copy the first carried,
 * and the job's earlier image still lands first, held after its rename
 * (0) or before (1). Each copy is told once. */
static void test_cut_handed_twice(void)
{
   unsigned char file[16];
   unsigned char first[12];
   struct retro_save_item items[1];
   unsigned pass, n;
   bool *hold;

   for (pass = 0; pass < 2; pass++)
   {
      hold = pass ? &hold_save_temp : &hold_save_renamed;
      start();
      fill_bytes(file, sizeof(file), 0x30);
      disk_put("/s/game.srm", file, sizeof(file));
      items[0]       = buffer_item("srm", "/s/game.srm");
      items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
      set_hold(&hold_temp, true);
      register_items(items, 1);
      assert(event_load_save_files(false));
      wait_held(1);
      n = osd_count;
      wram[0] = 0x11;
      memcpy(first, image(), sizeof(first));
      assert(event_save_files(true, false, NULL));
      set_hold(hold, true);
      set_hold(&hold_temp, false);
      wait_held(2);
      items[0].num_segments = 1;
      register_items(items, 1);
      wram[1] = 0x22;
      assert(event_save_files(true, false, NULL));
      wram[2] = 0x33;
      assert(event_save_files(true, false, NULL));
      set_hold(hold, false);
      copies_wait(true);
      assert(disk_holds("/undo/game.srm", file, 16));
      assert(disk_named_holds("/undo/", first, 12) == 1);
      assert(disk_count_named("/undo/") == 2);
      assert(disk_holds("/s/game.srm", wram, 8));
      assert(!disk_named("RetroArch-recovery-"));
      assert(osd_count == n && copy_logs == 2 && !err_count);
      assert(!save_item_files.files[0].guard.copy_first);
   }
}

/* No memory to hand an image to a copy job still copying: the job is
 * kept, the image goes to the recovery file, and the copy it makes
 * still counts. */
static void test_hand_no_memory(void)
{
   static const unsigned char old[4] = {7, 7, 7, 7};
   unsigned n;

   start();
   app_dir = "/app";
   set_hold(&hold_undo, true);
   load_refused_blob(old);
   wait_held(1);
   n = osd_count;
   memset(blob_state, 3, 4);
   retro_atomic_store_release_int(&fail_main_malloc_size,
         (int)(sizeof(autosave_image_t) + 4));
   assert(!event_save_files(true, false, NULL));
   retro_atomic_store_release_int(&fail_main_malloc_size, 0);
   assert(save_item_files.files[0].guard.job);
   assert(disk_named_holds("RetroArch-recovery-", blob_state, 4) == 1);
   assert(osd_count == n + 1);
   set_hold(&hold_undo, false);
   copies_wait(false);
   ASSERT_FILES(1, 1, 2);
   assert(err_count == 0);
   copies_wait(true);
   assert(!save_item_files.files[0].guard.copy_first);
   assert(disk_holds("/undo/game.blob", old, 4) && copy_logs == 1);
   memset(blob_state, 5, 4);
   assert(save_files() && disk_holds("/s/game.blob", blob_state, 4));
   assert(disk_count_named("RetroArch-recovery-") == 1);
   assert(disk_count_named("/undo/") == 1);
   assert(osd_count == n + 1 && !err_count);
ASSERT_FILES(1, 1, 2);
assert(err_count == 0);
}

/* No memory to hand the type change's image to the replaced item's
 * worker still writing: SAVE_FILES returns, both images go to recovery
 * files, and the copy is made by the next write. */
static void test_carry_no_memory(void)
{
   unsigned char want[12];
   unsigned char last[12];
   struct retro_save_item items[1];
   unsigned n;

   start();
   app_dir = "/app";
   disk_put("/s/t.srm", image(), 12);
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   start_autosave(false);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   set_hold(&hold_save_renamed, true);
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   wait_held(1);
   event_save_items_fill(false);
   n = osd_count;
   wram[2] = 9;
   memcpy(last, image(), sizeof(last));
   retro_atomic_store_release_int(&fail_main_malloc_size,
         (int)(sizeof(autosave_image_t) + 12));
   assert(!event_save_files(true, false, NULL));
   retro_atomic_store_release_int(&fail_main_malloc_size, 0);
   assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(osd_count == n + 2);
   set_hold(&hold_save_renamed, false);
   retired_join(false);
   autosave_check();
   assert(!autosave_state.num_retired);
   assert(save_files());
   assert(disk_holds("/undo/t.srm", want, sizeof(want)));
   assert(disk_count_named("/undo/") == 1);
   assert(disk_holds("/s/t.srm", last, 12));
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(osd_count == n + 2 && !err_count);
   autosave_deinit();
}

/* A copy that failed once, then made by the retry before its reap: a
 * cut that loses the cut bytes later owes a copy its own job makes at
 * once, with no recovery file. */
static void test_copy_failed_then_made(void)
{
   unsigned char file[16];
   struct retro_save_item items[1];
   unsigned n;

   start();
   app_dir = "/app";
   fill_bytes(file, sizeof(file), 0x30);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0]       = buffer_item("srm", "/s/game.srm");
   items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   set_fail_undo(true);
   register_items(items, 1);
   assert(event_load_save_files(false));
   copies_wait(true);
   assert(save_item_files.files[0].guard.copy_failed);
   set_fail_undo(false);
   n = osd_count;
   wram[0] = 0x11;
   assert(event_save_files(true, false, NULL));
   copies_wait(false);
   wram[0] = 0x12;
   assert(event_save_files(true, false, NULL));
   copies_wait(true);
   assert(!save_item_files.files[0].guard.copy_failed);
   assert(!save_item_files.files[0].guard.copy_first);
   assert(disk_holds("/undo/game.srm", file, 16));
   assert(disk_holds("/s/game.srm", image(), 12));
   assert(disk_count_named("/undo/") == 1 && copy_logs == 1);
   assert(!disk_named("RetroArch-recovery-"));
   assert(osd_count == n && !err_count);
}

/* A replaced item's worker that still owes the copy of its own type
 * change carries the next one's too: each copy is told once under its
 * own name, and each type change's notice names its own copy. */
static void test_carrier_tells_each_copy(void)
{
   unsigned char a0[12];
   unsigned char a1[12];
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned i, n, logs;

   start();
   app_dir = "/app";
   disk_put("/s/t.srm", image(), 12);
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   start_autosave(false);
   wram[1] = 0x44;
   memcpy(a0, image(), sizeof(a0));
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   event_save_items_fill(false);
   set_hold(&hold_undo, true);
   for (i = 0; !save_item_files.files[0].autosave && i < 5000; i++)
   {
      autosave_check();
      event_save_items_fill(false);
      retro_sleep(1);
   }
   assert((h = save_item_files.files[0].autosave) && h->copy_image);
   wait_held(1);
   wram[2] = 0x55;
   memcpy(a1, image(), sizeof(a1));
   items[0].id     = "c";
   items[0].format = "core:c";
   register_items(items, 1);
   event_save_items_fill(false);
   assert(save_item_files.files[0].guard.copy_image);
   n    = osd_count;
   logs = copy_logs;
   wram[3] = 9;
   assert(event_save_files(true, false, NULL));
   assert(osd_count == n && !disk_named("RetroArch-recovery-"));
   set_hold(&hold_undo, false);
   retired_join(false);
   autosave_check();
   assert(!autosave_state.num_retired);
   assert(disk_holds("/undo/t.srm", a0, 12));
   assert(disk_holds("/undo/t-2.srm", a1, 12));
   assert(disk_count_named("/undo/") == 2);
   assert(disk_holds("/s/t.srm", image(), 12));
   assert(!disk_named("RetroArch-recovery-"));
   assert(copy_logs == logs + 2 && osd_count == n + 2 && !err_count);
   assert(strstr(osd_hist, "/s/t.srm|/undo/t.srm\n"));
   assert(strstr(osd_hist, "/s/t.srm|/undo/t-2.srm\n"));
   autosave_deinit();
}

/* A copy job's worker that wrote a handed image it could not write to a
 * recovery file it named, then wrote a newer one at its file: nobody
 * was told of that file, which holds an older image, so it goes. */
static void test_job_dump_then_written(void)
{
   static const unsigned char old[4] = {7, 7, 7, 7};
   unsigned n;

   start();
   app_dir = "/app";
   set_hold(&hold_undo, true);
   load_refused_blob(old);
   wait_held(1);
   n = osd_count;
   memset(blob_state, 3, 4);
   assert(event_save_files(true, false, NULL));
   set_fail_undo(true);
   set_hold(&hold_recovery, true);
   set_hold(&hold_undo, false);
   wait_held(2);
   memset(blob_state, 5, 4);
   assert(event_save_files(true, false, NULL));
   set_fail_undo(false);
   set_hold(&hold_recovery, false);
   copies_wait(true);
   assert(disk_holds("/s/game.blob", blob_state, 4));
   assert(disk_holds("/undo/game.blob", old, 4));
   assert(disk_count_named("/undo/") == 1);
   assert(!disk_named("RetroArch-recovery-"));
   assert(osd_count == n && copy_logs == 1 && !err_count);
}

/* A type change's item leaves the set while the replaced item's worker
 * carries its copy: that worker's last image is the old type's, so the
 * orphan lets its own copy go; the worker copies the file, then writes
 * the orphan's image. No recovery file. When that copy fails (pass 1)
 * the old type's image is at the file when killed, and the reap sends
 * it to a recovery file of its own; the orphan's image went to one. */
static void test_carried_orphan(void)
{
   unsigned char want[12];
   unsigned char last[12];
   struct retro_save_item items[2];
   unsigned pass, n, logs;

   for (pass = 0; pass < 2; pass++)
   {
      start();
      app_dir = "/app";
      disk_put("/s/t.srm", image(), 12);
      items[0]        = buffer_item("a", "/s/t.srm");
      items[0].format = "core:a";
      register_items(items, 1);
      event_load_save_files(false);
      start_autosave(false);
      wram[1] = 0x44;
      memcpy(want, image(), sizeof(want));
      set_hold(&hold_save_renamed, true);
      items[0].id     = "b";
      items[0].format = "core:b";
      register_items(items, 1);
      wait_held(1);
      event_save_items_fill(false);
      n    = osd_count;
      logs = copy_logs;
      wram[2] = 9;
      assert(event_save_files(true, false, NULL));
      wram[3] = 0x55;
      memcpy(last, image(), sizeof(last));
      items[1] = buffer_item("c", "/s/u.srm");
      register_items(items + 1, 1);
      assert(save_item_files.num_orphans == 1);
      assert(event_save_files(true, false, NULL));
      assert(!save_item_files.num_orphans);
      set_fail_undo(pass == 1);
      set_hold(&hold_save_renamed, false);
      retired_join(false);
      if (pass == 1)
      {
         /* Killed here. */
         assert(disk_holds("/s/t.srm", want, 12));
         assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
         assert(disk_count_named("RetroArch-recovery-") == 1);
         assert(osd_count == n && !disk_named("/undo/"));
      }
      autosave_check();
      assert(!autosave_state.num_retired);
      if (pass == 1)
      {
         assert(disk_holds("/s/t.srm", want, 12));
         assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
         assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
         assert(disk_count_named("RetroArch-recovery-") == 2);
         assert(!disk_named("/undo/"));
         assert(copy_logs == logs && osd_count == n + 2 && !err_count);
         autosave_deinit();
         continue;
      }
      assert(disk_holds("/undo/t.srm", want, sizeof(want)));
      assert(disk_count_named("/undo/") == 1);
      assert(disk_holds("/s/t.srm", last, 12));
      assert(!disk_named("RetroArch-recovery-"));
      assert(copy_logs == logs + 1 && osd_count == n + 1 && !err_count);
      autosave_deinit();
   }
}

/* The replaced item's worker cannot make the copy it carries: the image
 * it was handed goes to a recovery file named for that image's item,
 * and the old type's image to another, named for the old item. */
static void test_carried_dump_type(void)
{
   unsigned char want[12];
   unsigned char last[12];
   struct retro_save_item items[1];
   unsigned n;

   start();
   app_dir = "/app";
   disk_put("/s/t.srm", image(), 12);
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   start_autosave(false);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   set_hold(&hold_save_renamed, true);
   items[0].id            = "b";
   items[0].format        = "core:b";
   items[0].legacy_memory = RETRO_MEMORY_RTC;
   register_items(items, 1);
   wait_held(1);
   event_save_items_fill(false);
   n = osd_count;
   wram[2] = 9;
   memcpy(last, image(), sizeof(last));
   assert(event_save_files(true, false, NULL));
   set_fail_undo(true);
   set_hold(&hold_save_renamed, false);
   retired_join(false);
   /* Killed here. */
   assert(disk_named_holds("RetroArch-recovery-1-", last, 12) == 1);
   assert(disk_holds("/s/t.srm", want, 12) && osd_count == n);
   autosave_check();
   assert(!autosave_state.num_retired);
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
   assert(!disk_named_holds("RetroArch-recovery-1-", want, 12));
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(disk_holds("/s/t.srm", want, 12) && !disk_named("/undo/"));
   assert(osd_count == n + 2 && !err_count);
   autosave_deinit();
}

/* An item's worker whose type change's copy fails writes the old type's
 * image to a recovery file itself, before any SAVE_FILES. SAVE_FILES
 * names it and sends the image the worker may not write to the recovery
 * file: one file each, told once. The copy made later is told without
 * the type change's notice. */
static void test_worker_copy_fails(void)
{
   unsigned char old[12];
   unsigned char want[12];
   unsigned char last[12];
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned n, logs;

   start();
   app_dir = "/app";
   memset(old, 0x21, sizeof(old));
   disk_put("/s/t.srm", old, sizeof(old));
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   /* No copy job: the worker makes the copy. */
   event_save_items_set_undo_dir(NULL);
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   event_save_items_fill(false);
   event_save_items_set_undo_dir("/undo");
   set_fail_undo(true);
   n    = osd_count;
   logs = copy_logs;
   h    = start_autosave(false);
   assert(h->copy_image);
   wram[2] = 9;
   memcpy(last, image(), sizeof(last));
   capture(h);
   /* Killed here. */
   assert(disk_holds("/s/t.srm", old, sizeof(old)));
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
   assert(disk_count_named("RetroArch-recovery-") == 1);
   assert(osd_count == n && !err_count);
   assert(!event_save_files(true, false, NULL));
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(osd_count == n + 2 && !err_count);
   wram[3] = 7;
   memcpy(last, image(), sizeof(last));
   assert(!event_save_files(true, false, NULL));
   assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(osd_count == n + 2 && disk_holds("/s/t.srm", old, 12));
   set_fail_undo(false);
   capture(h);
   assert(event_save_files(true, false, NULL));
   assert(disk_holds("/undo/t.srm", want, sizeof(want)));
   assert(disk_count_named("/undo/") == 1 && copy_logs == logs + 1);
   assert(disk_holds("/s/t.srm", last, 12));
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(osd_count == n + 2 && !err_count);
   assert(!save_item_files.files[0].guard.copy_first);
   autosave_deinit();
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(osd_count == n + 2 && !err_count);
}

/* The replaced item's worker fails both its last write and the copy it
 * carries: the old type's image is in the recovery file of that last
 * write only, told once. */
static void test_carrier_final_and_copy_fail(void)
{
   unsigned char want[12];
   unsigned char last[12];
   unsigned char first[12];
   struct retro_save_item items[1];
   unsigned n;

   start();
   app_dir = "/app";
   memcpy(first, image(), sizeof(first));
   disk_put("/s/t.srm", first, 12);
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   start_autosave(false);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   set_hold(&hold_save_temp, true);
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   wait_held(1);
   event_save_items_fill(false);
   n = osd_count;
   wram[2] = 9;
   memcpy(last, image(), sizeof(last));
   assert(event_save_files(true, false, NULL));
   /* Its last write's rename fails. */
   filestream_delete("/s/t.srm.auto.tmp");
   set_fail_undo(true);
   set_hold(&hold_save_temp, false);
   retired_join(false);
   /* Killed here. */
   assert(disk_holds("/s/t.srm", first, 12));
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(osd_count == n);
   autosave_check();
   assert(!autosave_state.num_retired);
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(osd_count == n + 2 && !err_count);
   wram[3] = 7;
   assert(!save_files());
   assert(disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(osd_count == n + 2 && !err_count && !disk_named("/undo/"));
   autosave_deinit();
}

/* The old type's image recovered while its copy job still runs: the
 * copy the job then makes is told without the type change's notice. */
static void test_job_copy_after_recovery(void)
{
   unsigned char old[12];
   unsigned char want[12];
   unsigned char last[12];
   struct retro_save_item items[1];
   unsigned n;

   start();
   app_dir = "/app";
   memset(old, 0x21, sizeof(old));
   disk_put("/s/t.srm", old, sizeof(old));
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   set_hold(&hold_undo, true);
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   event_save_items_fill(false);
   wait_held(1);
   n = osd_count;
   wram[2] = 9;
   memcpy(last, image(), sizeof(last));
   retro_atomic_store_release_int(&fail_main_malloc_size,
         (int)(sizeof(autosave_image_t) + 12));
   assert(!event_save_files(true, false, NULL));
   retro_atomic_store_release_int(&fail_main_malloc_size, 0);
   assert(save_item_files.files[0].guard.job);
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
   assert(osd_count == n + 2);
   set_hold(&hold_undo, false);
   copies_wait(true);
   assert(disk_holds("/undo/t.srm", want, sizeof(want)));
   assert(disk_count_named("/undo/") == 1 && copy_logs == 1);
   assert(osd_count == n + 2);
   assert(save_files() && disk_holds("/s/t.srm", last, 12));
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(osd_count == n + 2 && !err_count);
}

/* A copy job holds an image of the old type when a type change's busy
 * SAVE_FILES sends the new one to the recovery file the take gave the
 * new item: the job's failed copy sends its image to a file of its
 * own, never to that one. */
static void test_job_dump_own_name(void)
{
   unsigned char file[12];
   unsigned char b2[12];
   unsigned char b3[12];
   unsigned char c1[12];
   struct retro_save_item items[1];
   unsigned n;

   start();
   app_dir = "/app";
   fill_bytes(file, sizeof(file), 0x40);
   disk_put("/s/t.srm", file, sizeof(file));
   items[0]        = buffer_item("b", "/s/t.srm");
   items[0].format = "core:b";
   register_items(items, 1);
   set_fail_undo(true);
   set_fail_read("/s/t.srm");
   event_load_save_files(false);
   set_fail_read(NULL);
   assert(err_count == 1);
   copies_wait(true);
   assert(save_item_files.files[0].guard.copy_failed);
   set_fail_undo(false);
   set_hold(&hold_undo, true);
   wram[0] = 1;
   assert(!event_save_files(true, false, NULL));
   assert(save_item_files.files[0].guard.recovered);
   assert(save_item_files.files[0].guard.job);
   wait_held(1);
   wram[0] = 2;
   memcpy(b2, image(), sizeof(b2));
   assert(event_save_files(true, false, NULL));
   wram[0] = 3;
   memcpy(b3, image(), sizeof(b3));
   items[0].id     = "c";
   items[0].format = "core:c";
   register_items(items, 1);
   event_save_items_fill(false);
   assert(!memcmp(image(), b3, sizeof(b3)));
   assert(!save_item_files.num_orphans);
   n       = osd_count;
   wram[0] = 0xc;
   memcpy(c1, image(), sizeof(c1));
   assert(event_save_files(true, false, NULL));
   set_fail_undo(true);
   set_hold(&hold_undo, false);
   copies_wait(false);
   assert(disk_holds("/s/t.srm", file, 12));
   assert(disk_named_holds("RetroArch-recovery-", c1, 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", b3, 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", b2, 12) == 1);
   ASSERT_FILES(0, 3, n);
   assert(err_count == 1);
   autosave_reap(true);
   ASSERT_FILES(0, 3, n + 1);
   assert(err_count == 1);
   assert(told_holding(b2, 12) == 1 && told_holding(c1, 12) == 1);
   assert(told_holding(b3, 12) == 0);
   assert(disk_holds("/s/t.srm", file, 12));
   /* C's own copy, tried again, fails: b3's file is named. The job may
    * end before the hand-off, and SAVE_FILES then fails itself. */
   save_files();
   assert(disk_holds("/s/t.srm", file, 12));
   assert(disk_named_holds("RetroArch-recovery-", c1, 12) == 1);
   ASSERT_FILES(0, 3, n + 2);
   assert(told_holding(b3, 12) == 1);
   assert(err_count == 1);
   set_fail_undo(false);
}

/* A retired worker whose copy fails writes its last image to a
 * recovery file it names; SAVE_FILES before the reap sends a newer one
 * to the item's recovery file. The older goes at the reap: one file,
 * named once. */
static void test_retired_dump_superseded(void)
{
   unsigned char file[12];
   unsigned char last[12];
   struct retro_save_item items[1];
   unsigned n, errs;

   start();
   app_dir = "/app";
   fill_bytes(file, sizeof(file), 0x40);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   set_fail_undo(true);
   set_fail_read("/s/game.srm");
   event_load_save_files(false);
   set_fail_read(NULL);
   start_autosave(false);
   wram[0] = 1;
   memcpy(last, image(), sizeof(last));
   register_items(NULL, 0);
   register_items(items, 1);
   event_save_items_fill(false);
   retired_join(false);
   n       = osd_count;
   errs    = err_count;
   wram[1] = 2;
   assert(!event_save_files(true, false, NULL));
   assert(disk_holds("/s/game.srm", file, 12));
   assert(disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
   ASSERT_FILES(0, 2, n + 1);
   autosave_reap(false);
   assert(!autosave_state.num_retired);
   assert(disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
   assert(told_holding(image(), 12) == 1);
   ASSERT_FILES(0, 1, n + 1);
   assert(err_count == errs && disk_holds("/s/game.srm", file, 12));
   autosave_deinit();
}

/* A replaced item's worker whose own copy is made only on the attempt
 * for the copy it carries: that copy is its own, told without the next
 * type change's notice, whose image went to a recovery file. */
static void test_carrier_own_copy_late(void)
{
   unsigned char a0[12];
   unsigned char a1[12];
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned i, n;

   start();
   app_dir = "/app";
   disk_put("/s/t.srm", image(), 12);
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   start_autosave(false);
   wram[1] = 0x44;
   memcpy(a0, image(), sizeof(a0));
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   event_save_items_fill(false);
   set_fail_undo_left(1);
   for (i = 0; !save_item_files.files[0].autosave && i < 5000; i++)
   {
      autosave_check();
      event_save_items_fill(false);
      retro_sleep(1);
   }
   assert((h = save_item_files.files[0].autosave) && h->copy_image);
   wait_requested(h);
   set_hold(&hold_undo, true);
   event_save_items_fill(false);
   assert(disk_named_holds("RetroArch-recovery-", a0, 12) == 1);
   assert(osd_count == 1 && told_holding(a0, 12) == 1);
   n = osd_count;
   wram[2] = 0x55;
   memcpy(a1, image(), sizeof(a1));
   items[0].id     = "c";
   items[0].format = "core:c";
   register_items(items, 1);
   event_save_items_fill(false);
   wait_held(1);
   assert(save_item_files.files[0].guard.copy_image);
   wram[3] = 9;
   assert(event_save_files(true, false, NULL));
   set_fail_undo_left(2);
   set_hold(&hold_undo, false);
   retired_join(false);
   /* Killed here. */
   assert(disk_holds("/undo/t.srm", a0, 12));
   assert(disk_holds("/s/t.srm", image(), 12));
   assert(disk_named_holds("RetroArch-recovery-", a1, 12) == 1);
   ASSERT_FILES(1, 2, n);
   assert(err_count == 0);
   autosave_check();
   assert(!autosave_state.num_retired);
   assert(disk_holds("/undo/t.srm", a0, 12));
   assert(disk_holds("/s/t.srm", image(), 12));
   assert(disk_named_holds("RetroArch-recovery-", a1, 12) == 1);
   ASSERT_FILES(1, 2, n + 1);
   assert(!strstr(osd_hist, "|/undo/"));
   assert(err_count == 0);
   autosave_deinit();
}

/* A copy job's failed write of a handed image, cut by a kill while it
 * goes to a new recovery file: no part of it is under a recovery name.
 * The reap writes it whole. */
static void test_job_dump_cut(void)
{
   static const unsigned char old[4] = {7, 7, 7, 7};
   unsigned n;

   start();
   app_dir = "/app";
   set_hold(&hold_undo, true);
   load_refused_blob(old);
   wait_held(1);
   n = osd_count;
   memset(blob_state, 3, 4);
   assert(event_save_files(true, false, NULL));
   set_fail_undo(true);
   slock_lock(disk_lock);
   cut_recovery = true;
   slock_unlock(disk_lock);
   set_hold(&hold_undo, false);
   copies_wait(false);
   assert(disk_count_named("RetroArch-recovery-") == 1);
   assert(disk_count_named(".tmp") == 1);
   assert(disk_holds("/s/game.blob", old, 4) && osd_count == n);
   assert(err_count == 0);
   slock_lock(disk_lock);
   cut_recovery = false;
   slock_unlock(disk_lock);
   autosave_reap(true);
   assert(disk_named_holds("RetroArch-recovery-", blob_state, 4) == 1);
   assert(disk_count_named("RetroArch-recovery-") == 2);
   assert(disk_count_named(".tmp") == 1);
   assert(osd_count == n + 1 && told_holding(blob_state, 4) == 1);
   assert(disk_holds("/s/game.blob", old, 4));
   assert(err_count == 0);
}

/* SAVE_FILES while the worker owes the copy that a cut with no memory
 * for its bytes made it keep the file for: the newest image is in an
 * untold file until the worker writes it after the copy. */
static void test_save_files_cut_copying(void)
{
   unsigned char file[20];
   unsigned char newest[8];
   struct retro_save_item items[1];
   autosave_t *h;

   start();
   app_dir = "/app";
   fill_bytes(file, sizeof(file), 0xd0);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   assert(event_load_save_files(false));
   h       = start_autosave(false);
   wram[0] = 1;
   items[0].num_segments = 1;
   retro_atomic_store_release_int(&fail_malloc_size, 12);
   register_items(items, 1);
   retro_atomic_store_release_int(&fail_malloc_size, 0);
   assert(err_count == 1 && strstr(err_last, "cut by"));
   assert(save_item_files.files[0].autosave == h);
   assert(save_item_files.files[0].guard.keep_file);
   memcpy(newest, image(), sizeof(newest));
   /* The worker waits for a snapshot: its write makes the copy. */
   assert(save_files_unreaped());
   /* Killed here. */
   assert(disk_holds("/s/game.srm", file, sizeof(file)));
   assert(disk_named_holds("RetroArch-recovery-", newest, 8) == 1);
   ASSERT_FILES(0, 1, 0);
   capture(h);
   /* Killed here: the untold file went as its image landed. */
   assert(disk_holds("/s/game.srm", newest, 8));
   ASSERT_FILES(1, 0, 0);
   event_save_items_fill(false);
   assert(disk_holds("/undo/game.srm", file, sizeof(file)));
   assert(disk_holds("/s/game.srm", newest, 8));
   ASSERT_FILES(1, 0, 0);
   assert(err_count == 1);
   autosave_deinit();
}

/* Chains type changes a -> b -> c at /s/t.srm, from a file of @old:
 * b's copy job of a's image @want is held in its copy, and c took b's
 * image @last, of which it owes a copy. */
static void busy_chain(unsigned char *old, unsigned char *want,
      unsigned char *last, struct retro_save_item *items)
{
   memset(old, 0x21, 12);
   disk_put("/s/t.srm", old, 12);
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   wram[1] = 0x44;
   memcpy(want, image(), 12);
   set_hold(&hold_undo, true);
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   event_save_items_fill(false);
   wait_held(1);
   wram[2] = 9;
   memcpy(last, image(), 12);
   items[0].id     = "c";
   items[0].format = "core:c";
   register_items(items, 1);
   event_save_items_fill(false);
}

/* A type change while the replaced item's copy job runs: SAVE_FILES
 * writes the new image and the old type's silently. The next one makes
 * the copy and both files go (pass 0); when that copy fails, each is
 * named once (pass 1); when every write fails, the untold files are
 * named, not lost (pass 2). */
static void test_busy_job_type_change(void)
{
   unsigned char old[12];
   unsigned char want[12];
   unsigned char last[12];
   unsigned char c1[12];
   struct retro_save_item items[1];
   unsigned pass;
   unsigned n;

   for (pass = 0; pass < 3; pass++)
   {
      start();
      app_dir = "/app";
      busy_chain(old, want, last, items);
      wram[3] = 5;
      memcpy(c1, image(), sizeof(c1));
      n = osd_count;
      assert(event_save_files(true, false, NULL));
      set_hold(&hold_undo, false);
      copies_wait(false);
      /* Killed here. */
      assert(disk_holds("/s/t.srm", old, 12));
      assert(disk_holds("/undo/t.srm", want, 12));
      assert(disk_named_holds("RetroArch-recovery-", c1, 12) == 1);
      assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
      ASSERT_FILES(1, 2, n);
      assert(err_count == 0);
      if (pass == 1)
         set_fail_undo(true);
      if (pass == 2)
         set_fail_writes(true);
      /* A failed copy may end the job before the hand-off. */
      assert(save_files() || pass);
      set_fail_writes(false);
      if (pass == 0)
      {
         assert(disk_holds("/s/t.srm", c1, 12));
         assert(disk_named_holds("/undo/", last, 12) == 1);
         ASSERT_FILES(2, 0, n + 2);
      }
      else
      {
         assert(disk_holds("/s/t.srm", old, 12));
         assert(disk_named_holds("RetroArch-recovery-", c1, 12) == 1);
         assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
         assert(told_holding(c1, 12) == 1 && told_holding(last, 12) == 1);
         ASSERT_FILES(1, 2, n + 3);
         set_fail_undo(false);
      }
      assert(err_count == 0);
   }
}

/* An orphan whose file another copy job holds: its image and the old
 * type's are written silently, and it stays until the next SAVE_FILES,
 * which writes it once the job is done. */
static void test_busy_orphan_image(void)
{
   unsigned char old[12];
   unsigned char want[12];
   unsigned char last[12];
   unsigned char c1[12];
   struct retro_save_item items[1];
   unsigned n;

   start();
   app_dir = "/app";
   busy_chain(old, want, last, items);
   wram[3] = 5;
   memcpy(c1, image(), sizeof(c1));
   items[0] = card_item("u", "/s/u.srm");
   register_items(items, 1);
   event_save_items_fill(false);
   assert(save_item_files.num_orphans == 2);
   n = osd_count;
   assert(event_save_files(true, false, NULL));
   assert(save_item_files.num_orphans == 1);
   set_hold(&hold_undo, false);
   copies_wait(false);
   /* Killed here. */
   assert(disk_holds("/s/t.srm", old, 12));
   assert(disk_holds("/undo/t.srm", want, 12));
   assert(disk_named_holds("RetroArch-recovery-", c1, 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
   ASSERT_FILES(1, 2, n);
   assert(err_count == 0);
   assert(save_files());
   assert(!save_item_files.num_orphans);
   assert(disk_holds("/s/t.srm", c1, 12));
   assert(disk_named_holds("/undo/", last, 12) == 1);
   ASSERT_FILES(2, 0, n + 2);
   assert(err_count == 0);
}

/* Two type changes while the replaced item's worker still writes its
 * last image, which the second does not carry: the copy-only orphan and
 * the record write their images silently. The record's worker makes its
 * copy, the orphan's job its own, and the files go. */
static void test_busy_retired_no_carry(void)
{
   unsigned char old[12];
   unsigned char a1[12];
   unsigned char b1[12];
   unsigned char c1[12];
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned n;

   start();
   app_dir = "/app";
   memset(old, 0x21, sizeof(old));
   disk_put("/s/t.srm", old, sizeof(old));
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   start_autosave(false);
   wram[1] = 0x44;
   memcpy(a1, image(), sizeof(a1));
   set_hold(&hold_save_renamed, true);
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   wait_held(1);
   event_save_items_fill(false);
   assert(save_item_files.files[0].guard.copy_image);
   wram[2] = 9;
   memcpy(b1, image(), sizeof(b1));
   items[0].id     = "c";
   items[0].format = "core:c";
   register_items(items, 1);
   event_save_items_fill(false);
   assert(save_item_files.num_orphans == 1);
   assert(!save_item_files.orphans[0].data);
   wram[3] = 5;
   memcpy(c1, image(), sizeof(c1));
   n = osd_count;
   assert(event_save_files(true, false, NULL));
   set_hold(&hold_save_renamed, false);
   retired_join(false);
   /* Killed here. */
   assert(disk_holds("/s/t.srm", a1, 12));
   assert(disk_named_holds("RetroArch-recovery-", a1, 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", b1, 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", c1, 12) == 1);
   ASSERT_FILES(0, 3, n);
   assert(err_count == 0);
   autosave_check();
   event_save_items_fill(false);
   assert((h = save_item_files.files[0].autosave));
   h->interval = 0;
   wait_requested(h);
   capture(h);
   /* Killed here. */
   assert(disk_holds("/s/t.srm", c1, 12));
   assert(!disk_named_holds("RetroArch-recovery-", c1, 12));
   ASSERT_FILES(1, 2, n);
   assert(err_count == 0);
   event_save_items_fill(false);
   assert(disk_holds("/s/t.srm", c1, 12));
   assert(save_files());
   assert(!save_item_files.num_orphans);
   assert(disk_named_holds("/undo/", a1, 12) == 1);
   assert(disk_named_holds("/undo/", b1, 12) == 1);
   ASSERT_FILES(2, 0, n + 2);
   assert(err_count == 0);
   autosave_deinit();
}

/* test_busy_retired_no_carry with no recovery folder: the copy-only
 * orphan's image of the old type stays in memory, as the record's
 * does; the file's failure is shown once, at the second SAVE_FILES in a
 * row.
 * The copies are made once the worker is done. */
static void test_busy_orphan_untold_fails(void)
{
   unsigned char old[12];
   unsigned char a1[12];
   unsigned char b1[12];
   unsigned char c1[12];
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned n;

   start();
   memset(old, 0x21, sizeof(old));
   disk_put("/s/t.srm", old, sizeof(old));
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   start_autosave(false);
   wram[1] = 0x44;
   memcpy(a1, image(), sizeof(a1));
   set_hold(&hold_save_renamed, true);
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   wait_held(1);
   event_save_items_fill(false);
   wram[2] = 9;
   memcpy(b1, image(), sizeof(b1));
   items[0].id     = "c";
   items[0].format = "core:c";
   register_items(items, 1);
   event_save_items_fill(false);
   assert(save_item_files.num_orphans == 1);
   assert(!save_item_files.orphans[0].data);
   wram[3] = 5;
   memcpy(c1, image(), sizeof(c1));
   n = osd_count;
   assert(!event_save_files(true, false, NULL));
   ASSERT_FILES(0, 0, n);
   assert(err_count == 2);
   assert(!event_save_files(true, false, NULL));
   set_hold(&hold_save_renamed, false);
   retired_join(false);
   /* Killed here. */
   assert(disk_holds("/s/t.srm", a1, 12));
   assert(save_item_files.num_orphans == 1);
   assert(save_item_files.orphans[0].guard.copy_image);
   ASSERT_FILES(0, 0, n + 1);
   assert(strstr(osd_last, "/s/t.srm"));
   assert(err_count == 4);
   autosave_check();
   event_save_items_fill(false);
   assert((h = save_item_files.files[0].autosave));
   h->interval = 0;
   wait_requested(h);
   capture(h);
   event_save_items_fill(false);
   assert(disk_holds("/s/t.srm", c1, 12));
   assert(save_files());
   assert(!save_item_files.num_orphans);
   assert(disk_named_holds("/undo/", a1, 12) == 1);
   assert(disk_named_holds("/undo/", b1, 12) == 1);
   ASSERT_FILES(2, 0, n + 3);
   assert(err_count == 4);
   autosave_deinit();
}

/* Releases the held copies once @data more recovery files are written. */
static void release_after_recovery(void *data)
{
   unsigned until = *(const unsigned*)data;
   slock_lock(disk_lock);
   while (recovery_writes < until)
      scond_wait(disk_cond, disk_lock);
   hold_undo = false;
   scond_broadcast(disk_cond);
   slock_unlock(disk_lock);
}

/* The close with a copy-only orphan, whose job starts first, and a
 * record owing a copy of the same file: the record's write waits for
 * the job and lands after it, with no recovery file. */
static void test_busy_close(void)
{
   unsigned char old[12];
   unsigned char want[12];
   unsigned char b1[12];
   unsigned char c1[12];
   struct retro_save_item items[1];
   sthread_t *t;
   unsigned until;
   unsigned n;

   start();
   app_dir = "/app";
   memset(old, 0x21, sizeof(old));
   disk_put("/s/t.srm", old, sizeof(old));
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   event_save_items_set_undo_dir(NULL);
   wram[1] = 0x44;
   memcpy(want, image(), sizeof(want));
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   event_save_items_fill(false);
   wram[2] = 9;
   memcpy(b1, image(), sizeof(b1));
   items[0].id     = "c";
   items[0].format = "core:c";
   register_items(items, 1);
   event_save_items_fill(false);
   event_save_items_set_undo_dir("/undo");
   assert(save_item_files.num_orphans == 1);
   assert(!save_item_files.orphans[0].guard.job);
   wram[3] = 5;
   memcpy(c1, image(), sizeof(c1));
   n = osd_count;
   set_hold(&hold_undo, true);
   until = recovery_writes + 2;
   assert((t = sthread_create(release_after_recovery, &until)));
   event_save_items_final(true, false);
   sthread_join(t);
   assert(disk_holds("/s/t.srm", c1, 12));
   assert(disk_named_holds("/undo/", want, 12) == 1);
   assert(disk_named_holds("/undo/", b1, 12) == 1);
   ASSERT_FILES(2, 0, n + 2);
   assert(err_count == 0);
}

static void release_undo_at_close(void *data)
{
   (void)data;
   slock_lock(disk_lock);
   while (!closing)
      scond_wait(disk_cond, disk_lock);
   hold_undo = false;
   scond_broadcast(disk_cond);
   slock_unlock(disk_lock);
}

/* The close's SAVE_FILES hands its image to a copy job, joined before
 * the core unloads: a crash in the unload loses nothing. */
static void test_close_join_copies(void)
{
   static const unsigned char old[4] = {7, 7, 7, 7};
   sthread_t *t;
   unsigned e;
   unsigned n;

   start();
   app_dir = "/app";
   event_save_items_set_undo_dir(NULL);
   load_refused_blob(old);
   event_save_items_set_undo_dir("/undo");
   n = osd_count;
   e = err_count;
   memset(blob_state, 3, 4);
   set_hold(&hold_undo, true);
   autosave_deinit();
   assert(event_save_files(true, false, NULL));
   assert(save_item_files.files[0].guard.job);
   assert((t = sthread_create(release_undo_at_close, NULL)));
   slock_lock(disk_lock);
   closing = true;
   scond_broadcast(disk_cond);
   slock_unlock(disk_lock);
   event_save_items_join();
   /* Killed in the unload. */
   assert(disk_holds("/s/game.blob", blob_state, 4));
   assert(disk_holds("/undo/game.blob", old, 4));
   ASSERT_FILES(1, 0, n);
   sthread_join(t);
   event_save_items_final(true, false);
   assert(disk_holds("/s/game.blob", blob_state, 4));
   ASSERT_FILES(1, 0, n);
   assert(err_count == e);
}

/* The close's SAVE_FILES holds save RAM behind the last image of the
 * item that gave it back: written after the join, before the unload. */
static void test_given_back_close(void)
{
   unsigned char older[4];
   unsigned char last[4];
   struct retro_save_segment seg;
   struct retro_save_item items[1];
   sthread_t *t;

   given_back_held(items, &seg, older, last);
   event_save_files(true, false, NULL);
   assert(save_item_files.ram_held);
   assert(disk_holds("/s/game.srm", older, sizeof(older)));
   assert((t = sthread_create(release_at_close, NULL)));
   slock_lock(disk_lock);
   closing = true;
   scond_broadcast(disk_cond);
   slock_unlock(disk_lock);
   event_save_items_join();
   /* Killed in the unload. */
   assert(!save_item_files.ram_held);
   assert(disk_holds("/s/game.srm", legacy_ram, sizeof(legacy_ram)));
   assert(disk_count_named("") == 1);
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 0);
   sthread_join(t);
   autosave_deinit();
   event_save_items_final(true, false);
   assert(disk_holds("/s/game.srm", legacy_ram, sizeof(legacy_ram)));
   assert(disk_count_named("") == 1);
   ASSERT_FILES(0, 0, 0);
}
/* Workers held since start(). */
static unsigned held_count(void)
{
   unsigned n;
   slock_lock(disk_lock);
   n = worker_holds;
   slock_unlock(disk_lock);
   return n;
}

/* Until copier @h has exited, or a worker was held @holds times. */
static void wait_tried(autosave_t *h, unsigned holds)
{
   unsigned i;
   for (i = 0; i < 5000; i++)
   {
      if (     held_count() >= holds
            || retro_atomic_load_acquire_int(&h->exited))
         return;
      retro_sleep(1);
   }
   assert(!"copier never tried");
}

/* Joins every copy job but @skip once it has exited without being
 * held. */
static void join_jobs_but(const autosave_t *skip)
{
   unsigned i;
   for (i = 0; i < autosave_state.num_retired; i++)
   {
      autosave_t *h = autosave_state.retired[i];
      if (h == skip || !h->job || !h->thread)
         continue;
      wait_tried(h, held_count() + 1);
      assert(retro_atomic_load_acquire_int(&h->exited));
      sthread_join(h->thread);
      h->thread = NULL;
   }
}

/* Item "x" at /s/b/g.srm, whose file @xfile is longer than it, and
 * items[1] at /s/a/g.srm: x's copy job holds the undo name g.srm,
 * held in its copy. Returns that job. */
static autosave_t *busy_name_start(unsigned char *xfile,
      struct retro_save_item *items)
{
   autosave_t *x;
   fill_bytes(xfile, 16, 0x40);
   disk_put("/s/b/g.srm", xfile, 16);
   items[0]       = buffer_item("x", "/s/b/g.srm");
   items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   register_items(items, 2);
   set_hold(&hold_undo, true);
   event_load_save_files(false);
   wait_held(1);
   assert((x = save_item_files.files[0].guard.job));
   return x;
}

/* A copy job finds the undo name taken by a file of the same name in
 * another folder: it copies nothing, writes the old type's image
 * silently and hands its debt back to its record, which makes the copy
 * once the name is free. */
static void test_busy_name_job(void)
{
   unsigned char xfile[16];
   unsigned char yold[6];
   unsigned char ycur[6];
   unsigned char ynew[6];
   struct retro_save_item items[2];
   autosave_t *h, *x;
   unsigned n;

   start();
   app_dir = "/app";
   memset(yold, 0x31, sizeof(yold));
   disk_put("/s/a/g.srm", yold, sizeof(yold));
   items[1]        = card_item("y", "/s/a/g.srm");
   items[1].format = "core:y";
   x = busy_name_start(xfile, items);
   card[0] = 0x55;
   memcpy(ycur, card, sizeof(ycur));
   items[1].id     = "y2";
   items[1].format = "core:y2";
   register_items(items, 2);
   event_save_items_fill(false);
   assert((h = save_item_files.files[1].guard.job));
   n = osd_count;
   wait_tried(h, 2);
   assert(held_count() == 1);
   join_jobs_but(x);
   /* Killed here. */
   assert(disk_holds("/s/a/g.srm", yold, 6));
   assert(disk_named_holds("RetroArch-recovery-", ycur, 6) == 1);
   ASSERT_FILES(0, 1, n);
   assert(err_count == 0);
   card[1] = 0x66;
   memcpy(ynew, card, sizeof(ynew));
   assert(event_save_files(true, false, NULL));
   join_jobs_but(x);
   /* Killed here. */
   assert(held_count() == 1);
   assert(disk_holds("/s/a/g.srm", yold, 6));
   assert(disk_named_holds("RetroArch-recovery-", ycur, 6) == 1);
   assert(disk_named_holds("RetroArch-recovery-", ynew, 6) == 1);
   ASSERT_FILES(0, 2, n);
   assert(err_count == 0);
   set_hold(&hold_undo, false);
   copies_wait(true);
   assert(save_item_files.files[1].guard.busy);
   ASSERT_FILES(1, 2, n);
   assert(save_files());
   assert(disk_holds("/s/a/g.srm", ynew, 6));
   assert(disk_named_holds("/undo/", xfile, 16) == 1);
   assert(disk_named_holds("/undo/", ycur, 6) == 1);
   ASSERT_FILES(2, 0, n + 1);
   assert(err_count == 0);
}

/* Save RAM's item leaves while its copy is busy, x's job holding the
 * undo name: the close's SAVE_FILES keeps that copy owed and holds save
 * RAM behind it. After the join the copy is made and save RAM written,
 * before the unload. */
static void test_given_back_busy_close(void)
{
   unsigned char xfile[16];
   unsigned char last[12];
   struct retro_save_item items[2];
   autosave_t *x;
   sthread_t *t;

   start();
   app_dir = "/app";
   path_init_savefile_rtc("/s/a/g.srm");
   items[1]               = buffer_item("y", "/s/a/g.srm");
   items[1].legacy_memory = RETRO_MEMORY_SAVE_RAM;
   x = busy_name_start(xfile, items);
   wram[0] = 1;
   memcpy(last, image(), sizeof(last));
   register_items(items, 1);
   join_jobs_but(x);
   memset(legacy_ram, 9, sizeof(legacy_ram));
   event_save_files(true, false, NULL);
   /* Killed here. */
   assert(save_item_files.ram_held);
   assert(disk_holds("/s/a/g.srm", last, sizeof(last)));
   assert(disk_named_holds("RetroArch-recovery-", last, sizeof(last)) == 1);
   ASSERT_FILES(0, 1, 0);
   assert((t = sthread_create(release_undo_at_close, NULL)));
   slock_lock(disk_lock);
   closing = true;
   scond_broadcast(disk_cond);
   slock_unlock(disk_lock);
   event_save_items_join();
   /* Killed in the unload. */
   assert(!save_item_files.ram_held);
   assert(save_item_files.num_orphans == 0);
   assert(disk_holds("/s/a/g.srm", legacy_ram, sizeof(legacy_ram)));
   assert(disk_named_holds("/undo/", last, sizeof(last)) == 1);
   assert(disk_named_holds("/undo/", xfile, sizeof(xfile)) == 1);
   ASSERT_FILES(2, 0, 0);
   sthread_join(t);
   event_save_items_final(true, false);
   assert(disk_holds("/s/a/g.srm", legacy_ram, sizeof(legacy_ram)));
   ASSERT_FILES(2, 0, 0);
   assert(err_count == 0);
}

/* Fills @h's snapshot and hands it to its worker, as autosave_check()
 * does, for @h alone. */
static void fill_one(autosave_t *h)
{
   assert(autosave_fill_item(h));
   retro_atomic_store_release_int(&h->snapshot_requested, 0);
   retro_eventcount_notify(&h->ec);
}

/* Two items at /s/a/g.srm and /s/b/g.srm, both longer than the item:
 * one worker holds the undo name g.srm in its copy, so the other's
 * settled write lands and its copy stays owed. can_snapshot() says false
 * at the close: that copy is made and the image now written, as without
 * can_snapshot(), and the close returns; no recovery file is left. The
 * close's image was not settled, so the final write writes the image
 * now. */
static void test_busy_name_close(void)
{
   unsigned char files[2][16];
   unsigned char cut[16];
   struct retro_save_item items[2];
   autosave_t *h[2];
   unsigned i;
   unsigned b;

   start();
   app_dir          = "/app";
   use_can_snapshot = true;
   fill_bytes(files[0], 16, 0x40);
   fill_bytes(files[1], 16, 0x31);
   disk_put("/s/a/g.srm", files[0], 16);
   disk_put("/s/b/g.srm", files[1], 16);
   items[0]       = card_item("x", "/s/a/g.srm");
   items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   items[1]       = card_item("y", "/s/b/g.srm");
   items[1].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   event_save_items_set_undo_dir(NULL);
   register_items(items, 2);
   event_load_save_files(false);
   event_save_items_set_undo_dir("/undo");
   set_hold(&hold_undo, true);
   assert(autosave_init(false, 1));
   for (i = 0; i < 2; i++)
   {
      assert((h[i] = save_item_files.files[i].autosave));
      h[i]->interval = 0;
   }
   wait_held(1);
   /* The worker not held found the name taken and asks for a
    * snapshot. */
   for (i = 0; i < 5000; i++)
   {
      if (     retro_atomic_load_acquire_int(&h[0]->snapshot_requested)
            || retro_atomic_load_acquire_int(&h[1]->snapshot_requested))
         break;
      retro_sleep(1);
   }
   b = retro_atomic_load_acquire_int(&h[0]->snapshot_requested) ? 0 : 1;
   assert(retro_atomic_load_acquire_int(&h[b]->snapshot_requested));
   card[0] = 0x55;
   memcpy(cut, card, 6);
   memcpy(cut + 6, files[b] + 6, 10);
   fill_one(h[b]);
   wait_requested(h[b]);
   /* Killed here: the tail stays until the copy is made. */
   assert(disk_holds(items[b].path, cut, 16));
   assert(held_count() == 1);
   ASSERT_FILES(0, 0, 0);
   snapshot_ready = false;
   card[1]        = 0x66;
   set_hold(&hold_undo, false);
   wait_requested(h[!b]);
   autosave_deinit();
   assert(save_item_files.files[b].guard.busy);
   assert(save_files());
   /* Killed here. */
   assert(disk_holds(items[b].path, card, 6));
   assert(disk_holds(items[!b].path, card, 6));
   assert(disk_named_holds("/undo/", cut, 16) == 1);
   assert(disk_named_holds("/undo/", files[!b], 16) == 1);
   ASSERT_FILES(2, 0, 0);
   assert(saving_logs == 0 && err_count == 0);
   /* Written while unloading. */
   card[2] = 0x77;
   event_save_items_final(true, false);
   assert(disk_holds(items[b].path, card, 6));
   assert(disk_holds(items[!b].path, card, 6));
   assert(disk_named_holds("/undo/", cut, 16) == 1);
   assert(disk_named_holds("/undo/", files[!b], 16) == 1);
   ASSERT_FILES(2, 0, 0);
   assert(saving_logs == 0 && err_count == 0);
}

/* Blob y at /s/a/g.srm, added after the load, refuses its file and
 * owes its copy while x's copy job holds the undo name g.srm: its own
 * job finds the name taken, so its file is busy at the final write,
 * where the core has no
 * image to give: pass 0 an empty blob, 1 one over max_size, 2 no
 * memory, 3 blob_save() fails. The final write returns; the file keeps
 * its save, the close's image of it is in a recovery file, and a failed
 * write is logged once, as for an item that is not busy. */
static void test_busy_close_stopped(void)
{
   unsigned char xfile[16];
   unsigned char yfile[6];
   unsigned char closed[6];
   struct retro_save_item items[2];
   autosave_t *x;
   unsigned pass;

   for (pass = 0; pass < 4; pass++)
   {
      start();
      app_dir = "/app";
      fill_bytes(yfile, 6, 0x31);
      disk_put("/s/a/g.srm", yfile, 6);
      fill_bytes(xfile, 16, 0x40);
      disk_put("/s/b/g.srm", xfile, 16);
      items[0]       = buffer_item("x", "/s/b/g.srm");
      items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
      register_items(items, 1);
      set_hold(&hold_undo, true);
      event_load_save_files(false);
      wait_held(1);
      assert((x = save_item_files.files[0].guard.job));
      items[1]          = blob_item("y", "/s/a/g.srm");
      items[1].max_size = 6;
      register_items(items, 2);
      blob_refuse = true;
      event_save_items_fill(false);
      blob_refuse   = false;
      blob_len      = 6;
      blob_state[0] = 0x55;
      memcpy(closed, blob_state, sizeof(closed));
      assert(event_save_files(true, false, NULL));
      join_jobs_but(x);
      set_hold(&hold_undo, false);
      copies_wait(true);
      assert(save_item_files.files[1].guard.busy);
      /* Killed here. */
      assert(disk_holds("/s/a/g.srm", yfile, 6));
      assert(disk_named_holds("RetroArch-recovery-", closed, 6) == 1);
      ASSERT_FILES(1, 1, 1);
      assert(saving_logs == 0 && err_count == 0);
      /* Unloaded. */
      if (pass == 0)
         blob_len = 0;
      else if (pass == 1)
         blob_len = 7;
      else if (pass == 2)
         retro_atomic_store_release_int(&fail_main_malloc_size, 6);
      else
         blob_fail_save = true;
      event_save_items_final(true, false);
      retro_atomic_store_release_int(&fail_main_malloc_size, 0);
      assert(disk_holds("/s/a/g.srm", yfile, 6));
      assert(disk_named_holds("/undo/", xfile, 16) == 1);
      assert(disk_named_holds("RetroArch-recovery-", closed, 6) == 1);
      ASSERT_FILES(1, 1, 2);
      assert(saving_logs == 0 && err_count == (pass ? 1u : 0u));
   }
}

/* Item y at /s/a/g.srm, a type change of z (old image *@zcur), with
 * its worker held back by x's copy of the same name. Returns it. */
static autosave_t *busy_name_worker(unsigned char *xfile,
      unsigned char *zcur, struct retro_save_item *items, autosave_t **x)
{
   autosave_t *h;
   unsigned char zold[6];
   memset(zold, 0x31, sizeof(zold));
   disk_put("/s/a/g.srm", zold, sizeof(zold));
   items[1]        = card_item("z", "/s/a/g.srm");
   items[1].format = "core:z";
   *x = busy_name_start(xfile, items);
   card[0] = 0x55;
   memcpy(zcur, card, 6);
   event_save_items_set_undo_dir(NULL);
   items[1].id     = "y";
   items[1].format = "core:y";
   register_items(items, 2);
   event_save_items_fill(false);
   event_save_items_set_undo_dir("/undo");
   assert(save_item_files.files[1].guard.copy_image);
   assert(autosave_init(false, 1));
   assert((h = save_item_files.files[1].autosave));
   h->interval = 0;
   wait_requested(h);
   assert(held_count() == 1);
   return h;
}

/* The retired one of @path that is not a copy job. */
static autosave_t *retired_of(const char *path)
{
   unsigned i;
   for (i = 0; i < autosave_state.num_retired; i++)
      if (     !autosave_state.retired[i]->job
            && string_is_equal_noncase(autosave_state.retired[i]->path,
               path))
         return autosave_state.retired[i];
   return NULL;
}

/* A retired worker that owes a copy finds the undo name taken: its last
 * image and the old type's go to files no one is told of, and the reap
 * makes them an orphan, which the next SAVE_FILES writes after the
 * copy. */
static void test_busy_name_retired(void)
{
   unsigned char xfile[16];
   unsigned char zcur[6];
   unsigned char ycur[6];
   struct retro_save_item items[2];
   autosave_t *r, *x;
   unsigned n;

   start();
   app_dir = "/app";
   busy_name_worker(xfile, zcur, items, &x);
   card[1] = 0x66;
   memcpy(ycur, card, sizeof(ycur));
   n = osd_count;
   register_items(items, 1);
   assert((r = retired_of("/s/a/g.srm")));
   wait_tried(r, 2);
   assert(held_count() == 1);
   sthread_join(r->thread);
   r->thread = NULL;
   /* Killed here. */
   assert(!disk_holds("/s/a/g.srm", ycur, 6));
   assert(disk_named_holds("RetroArch-recovery-", ycur, 6) == 1);
   assert(disk_named_holds("RetroArch-recovery-", zcur, 6) == 1);
   ASSERT_FILES(0, 2, n);
   assert(err_count == 0);
   set_hold(&hold_undo, false);
   copies_wait(false);
   autosave_reap(false);
   assert(save_item_files.num_orphans == 1);
   ASSERT_FILES(1, 2, n);
   assert(save_files());
   assert(!save_item_files.num_orphans);
   assert(disk_holds("/s/a/g.srm", ycur, 6));
   assert(disk_named_holds("/undo/", xfile, 16) == 1);
   assert(disk_named_holds("/undo/", zcur, 6) == 1);
   ASSERT_FILES(2, 0, n + 1);
   assert(err_count == 0);
   autosave_deinit();
}

/* A type change b of y while y's retired worker, held back by another
 * file's copy of the same name, still writes: b's image goes to that
 * worker after the copy it carries, which it cannot make either. b gets
 * its image back; y's last image and the old type's become an orphan,
 * written before b's newer image, never over it. */
static void test_busy_name_carrier(void)
{
   unsigned char xfile[16];
   unsigned char zcur[6];
   unsigned char ycur[6];
   unsigned char b1[6];
   struct retro_save_item items[2];
   autosave_t *r, *x;
   unsigned held;
   unsigned n;

   start();
   app_dir = "/app";
   busy_name_worker(xfile, zcur, items, &x);
   card[1] = 0x66;
   memcpy(ycur, card, sizeof(ycur));
   set_hold(&hold_recovery, true);
   items[1].id     = "b";
   items[1].format = "core:b";
   register_items(items, 2);
   assert((r = retired_of("/s/a/g.srm")));
   wait_held(2);
   event_save_items_fill(false);
   assert(save_item_files.files[1].guard.copy_image);
   card[2] = 0x77;
   memcpy(b1, card, sizeof(b1));
   n = osd_count;
   assert(event_save_files(true, false, NULL));
   assert(save_item_files.files[1].guard.job == r);
   set_hold(&hold_recovery, false);
   wait_tried(r, 99);
   sthread_join(r->thread);
   r->thread = NULL;
   /* Killed here. */
   assert(!disk_holds("/s/a/g.srm", ycur, 6));
   assert(disk_named_holds("RetroArch-recovery-", zcur, 6) == 1);
   assert(disk_named_holds("RetroArch-recovery-", ycur, 6) == 1);
   assert(disk_named_holds("RetroArch-recovery-", b1, 6) == 1);
   ASSERT_FILES(0, 3, n);
   assert(err_count == 0);
   set_hold(&hold_undo, false);
   copies_wait(false);
   autosave_deinit();
   assert(save_item_files.num_orphans == 1);
   assert(save_item_files.files[1].guard.busy);
   ASSERT_FILES(1, 3, n);
   /* The orphan's job copies; b waits for it. */
   held = held_count();
   set_hold(&hold_undo, true);
   assert(event_save_files(true, false, NULL));
   wait_held(held + 1);
   assert(!save_item_files.num_orphans);
   assert(save_item_files.files[1].guard.busy);
   assert(!save_item_files.files[1].guard.job);
   set_hold(&hold_undo, false);
   copies_wait(true);
   /* The orphan's image first, b's after. */
   assert(disk_holds("/s/a/g.srm", ycur, 6));
   assert(disk_named_holds("RetroArch-recovery-", b1, 6) == 1);
   ASSERT_FILES(2, 1, n + 1);
   assert(save_files());
   assert(disk_holds("/s/a/g.srm", b1, 6));
   assert(disk_named_holds("/undo/", xfile, 16) == 1);
   assert(disk_named_holds("/undo/", zcur, 6) == 1);
   assert(disk_named_holds("/undo/", ycur, 6) == 1);
   ASSERT_FILES(3, 0, n + 2);
   assert(err_count == 0);
}

/* test_busy_name_carrier with every write failing once the carrier
 * holds b's image: what the reap takes back stays in memory, b's image
 * with b and y's in an orphan, each failure logged. b's next write
 * fails too and is shown; the orphan's job writes y's after its copy. */
static void test_busy_name_untold_fails(void)
{
   unsigned char xfile[16];
   unsigned char zcur[6];
   unsigned char ycur[6];
   unsigned char b1[6];
   struct retro_save_item items[2];
   autosave_t *r, *x;
   unsigned held;
   unsigned n;

   start();
   app_dir = "/app";
   busy_name_worker(xfile, zcur, items, &x);
   card[1] = 0x66;
   memcpy(ycur, card, sizeof(ycur));
   set_hold(&hold_recovery, true);
   items[1].id     = "b";
   items[1].format = "core:b";
   register_items(items, 2);
   assert((r = retired_of("/s/a/g.srm")));
   wait_held(2);
   event_save_items_fill(false);
   card[2] = 0x77;
   memcpy(b1, card, sizeof(b1));
   n = osd_count;
   assert(event_save_files(true, false, NULL));
   assert(save_item_files.files[1].guard.job == r);
   set_fail_writes(true);
   set_hold(&hold_recovery, false);
   wait_tried(r, 99);
   sthread_join(r->thread);
   r->thread = NULL;
   /* Killed here. */
   assert(!disk_holds("/s/a/g.srm", ycur, 6));
   ASSERT_FILES(0, 0, n);
   assert(err_count == 0);
   autosave_reap(false);
   assert(save_item_files.num_orphans == 1);
   assert(save_item_files.orphans[0].data);
   assert(save_item_files.files[1].guard.busy);
   ASSERT_FILES(0, 0, n);
   assert(err_count == 2);
   /* y's job, started now, holds its image until writes work again. */
   held = held_count();
   set_hold(&hold_recovery, true);
   assert(!event_save_files(true, false, NULL));
   wait_held(held + 1);
   assert(!save_item_files.num_orphans);
   ASSERT_FILES(0, 0, n + 1);
   assert(strstr(osd_last, "/s/a/g.srm"));
   assert(err_count == 3);
   set_fail_writes(false);
   set_hold(&hold_recovery, false);
   join_jobs_but(x);
   set_hold(&hold_undo, false);
   copies_wait(true);
   assert(save_item_files.num_orphans == 1);
   ASSERT_FILES(1, 2, n + 1);
   assert(err_count == 3);
   /* The orphan's job copies; b waits for it. */
   held = held_count();
   set_hold(&hold_undo, true);
   assert(event_save_files(true, false, NULL));
   wait_held(held + 1);
   assert(!save_item_files.num_orphans);
   assert(save_item_files.files[1].guard.busy);
   set_hold(&hold_undo, false);
   copies_wait(true);
   assert(disk_holds("/s/a/g.srm", ycur, 6));
   assert(disk_named_holds("RetroArch-recovery-", b1, 6) == 1);
   ASSERT_FILES(2, 1, n + 2);
   assert(save_files());
   assert(disk_holds("/s/a/g.srm", b1, 6));
   assert(disk_named_holds("/undo/", xfile, 16) == 1);
   assert(disk_named_holds("/undo/", zcur, 6) == 1);
   assert(disk_named_holds("/undo/", ycur, 6) == 1);
   ASSERT_FILES(3, 0, n + 3);
   assert(err_count == 3);
   autosave_deinit();
}

/* The same, but the item comes back under another spelling of its path
 * and hands its newer image to the retired worker without a copy: y's
 * last image, older, never reaches the file, and its silent file goes. */
static void test_busy_name_superseded(void)
{
   unsigned char xfile[16];
   unsigned char zcur[6];
   unsigned char ycur[6];
   unsigned char b1[6];
   unsigned char b2[6];
   struct retro_save_item items[2];
   autosave_t *r, *x;
   unsigned n;

   start();
   app_dir = "/app";
   slock_lock(disk_lock);
   disk_nocase = true;
   slock_unlock(disk_lock);
   busy_name_worker(xfile, zcur, items, &x);
   card[1] = 0x66;
   memcpy(ycur, card, sizeof(ycur));
   set_hold(&hold_recovery, true);
   items[1].path = "/s/a/G.srm";
   register_items(items, 2);
   assert((r = retired_of("/s/a/g.srm")));
   wait_held(2);
   event_save_items_fill(false);
   assert(!save_item_files.files[1].guard.copy_first);
   card[2] = 0x77;
   memcpy(b1, card, sizeof(b1));
   n = osd_count;
   assert(event_save_files(true, false, NULL));
   set_hold(&hold_recovery, false);
   wait_tried(r, 99);
   sthread_join(r->thread);
   r->thread = NULL;
   /* Killed here. */
   assert(disk_named_holds("RetroArch-recovery-", zcur, 6) == 1);
   assert(disk_named_holds("RetroArch-recovery-", b1, 6) == 1);
   assert(disk_named_holds("RetroArch-recovery-", ycur, 6) == 1);
   ASSERT_FILES(0, 3, n);
   assert(err_count == 0);
   set_hold(&hold_undo, false);
   copies_wait(false);
   autosave_deinit();
   assert(save_item_files.num_orphans == 1);
   assert(!disk_named_holds("RetroArch-recovery-", ycur, 6));
   ASSERT_FILES(1, 2, n);
   card[3] = 0x78;
   memcpy(b2, card, sizeof(b2));
   assert(save_files());
   assert(disk_holds("/s/a/g.srm", b2, 6));
   assert(save_files());
   assert(disk_holds("/s/a/g.srm", b2, 6));
   assert(disk_named_holds("/undo/", xfile, 16) == 1);
   assert(disk_named_holds("/undo/", zcur, 6) == 1);
   assert(!disk_named_holds("/undo/", ycur, 6));
   ASSERT_FILES(2, 0, n + 1);
   assert(told_holding(ycur, 6) == 0);
   assert(err_count == 0);
}

/* An orphan hands the image it wrote silently to a copy job: that file
 * stays until the job has written the image. */
static void test_busy_orphan_handed_kept(void)
{
   unsigned char old[12];
   unsigned char want[12];
   unsigned char last[12];
   unsigned char c1[12];
   struct retro_save_item items[1];
   unsigned held;
   unsigned n;

   start();
   app_dir = "/app";
   busy_chain(old, want, last, items);
   wram[3] = 5;
   memcpy(c1, image(), sizeof(c1));
   items[0] = card_item("u", "/s/u.srm");
   register_items(items, 1);
   event_save_items_fill(false);
   assert(event_save_files(true, false, NULL));
   set_hold(&hold_undo, false);
   copies_wait(true);
   assert(save_item_files.num_orphans == 1);
   n    = osd_count;
   held = held_count();
   set_hold(&hold_undo, true);
   assert(event_save_files(true, false, NULL));
   wait_held(held + 1);
   assert(!save_item_files.num_orphans);
   /* Killed here: the job copies; c1 is still on disk. */
   assert(disk_holds("/s/t.srm", old, 12));
   assert(disk_named_holds("RetroArch-recovery-", c1, 12) == 1);
   assert(disk_named_holds("RetroArch-recovery-", last, 12) == 1);
   ASSERT_FILES(1, 2, n);
   set_hold(&hold_undo, false);
   copies_wait(false);
   /* Killed here. */
   assert(disk_holds("/s/t.srm", c1, 12));
   assert(disk_named_holds("/undo/", last, 12) == 1);
   ASSERT_FILES(2, 0, n);
   assert(err_count == 0);
   frame_start_reap();
   assert(disk_holds("/s/t.srm", c1, 12));
   assert(disk_named_holds("/undo/", last, 12) == 1);
   ASSERT_FILES(2, 0, n + 1);
   assert(err_count == 0);
}

/* Blob y at /s/a/g.srm refused its file @yold; its worker, held back by
 * x's copy of the same name, owes the copy of the file. y leaves and
 * comes back: its new record takes the worker's last image @y1, which
 * the worker could not write. Then the copy of x's file is made and the
 * worker reaped: an orphan holds y1 and the copy. Returns the number of
 * messages before the reap. */
static unsigned busy_name_took(unsigned char *xfile,
      const unsigned char *yold, const unsigned char *y1,
      struct retro_save_item *items)
{
   autosave_t *h;
   unsigned n;
   fill_bytes(xfile, 16, 0x40);
   disk_put("/s/b/g.srm", xfile, 16);
   disk_put("/s/a/g.srm", yold, 4);
   items[0]       = buffer_item("x", "/s/b/g.srm");
   items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   register_items(items, 1);
   set_hold(&hold_undo, true);
   event_load_save_files(false);
   wait_held(1);
   items[1]    = blob_item("y", "/s/a/g.srm");
   memcpy(blob_state, y1, 4);
   blob_len    = 4;
   blob_refuse = true;
   event_save_items_set_undo_dir(NULL);
   register_items(items, 2);
   event_save_items_fill(false);
   event_save_items_set_undo_dir("/undo");
   blob_refuse = false;
   assert(save_item_files.files[1].guard.keep_file);
   assert(autosave_init(false, 1));
   assert((h = save_item_files.files[1].autosave));
   h->interval = 0;
   wait_requested(h);
   n = osd_count;
   register_items(items, 1);
   register_items(items, 2);
   event_save_items_fill(false);
   assert(!memcmp(blob_state, y1, 4));
   wait_tried(h, 99);
   sthread_join(h->thread);
   h->thread = NULL;
   /* Killed here. */
   assert(held_count() == 1);
   assert(disk_holds("/s/a/g.srm", yold, 4));
   assert(disk_named_holds("RetroArch-recovery-", y1, 4) == 1);
   ASSERT_FILES(0, 1, n);
   assert(err_count == 0);
   set_hold(&hold_undo, false);
   copies_wait(false);
   autosave_reap(false);
   assert(save_item_files.num_orphans == 1);
   ASSERT_FILES(1, 1, n);
   return n;
}

/* The same record's newer image waits for the orphan's copy, in a file
 * no one is told of, and then lands: the file is copied once, by the
 * orphan. Each untold file goes as its image lands. Passes: SAVE_FILES
 * writes y2; its worker does; the first again, the job's write of y1
 * failing, so its untold file stays and is named at the reap; the
 * second again, the worker's first write failing. */
static void test_busy_name_took(void)
{
   static const unsigned char yold[4] = {0x31, 0x31, 0x31, 0x31};
   static const unsigned char y1[4]   = {0x41, 0x41, 0x41, 0x41};
   static const unsigned char y2[4]   = {0x42, 0x42, 0x42, 0x42};
   unsigned char xfile[16];
   struct retro_save_item items[2];
   unsigned pass;

   for (pass = 0; pass < 4; pass++)
   {
      unsigned held, n;
      bool worker    = (pass & 1) != 0;
      bool job_fails = pass == 2;
      const unsigned char *at = job_fails ? yold : y1;
      start();
      app_dir = "/app";
      n = busy_name_took(xfile, yold, y1, items);
      memcpy(blob_state, y2, 4);
      held = held_count();
      set_hold(&hold_undo, true);
      assert(event_save_files(true, false, NULL));
      wait_held(held + 1);
      assert(!save_item_files.num_orphans);
      /* Killed here: the orphan's job copies; y2 waits. */
      assert(disk_holds("/s/a/g.srm", yold, 4));
      assert(disk_named_holds("RetroArch-recovery-", y1, 4) == 1);
      assert(disk_named_holds("RetroArch-recovery-", y2, 4) == 1);
      ASSERT_FILES(1, 2, n);
      set_fail_save_writes(job_fails);
      set_hold(&hold_undo, false);
      copies_wait(false);
      set_fail_save_writes(false);
      /* Killed here. */
      assert(disk_holds("/s/a/g.srm", at, 4));
      assert(disk_named_holds("/undo/", yold, 4) == 1);
      assert(disk_named_holds("RetroArch-recovery-", y1, 4) == job_fails);
      assert(disk_named_holds("RetroArch-recovery-", y2, 4) == 1);
      ASSERT_FILES(2, 1 + job_fails, n);
      assert(err_count == 0);
      frame_start_reap();
      assert(disk_holds("/s/a/g.srm", at, 4));
      assert(disk_named_holds("RetroArch-recovery-", y1, 4) == job_fails);
      assert(told_holding(y1, 4) == job_fails);
      assert(disk_named_holds("RetroArch-recovery-", y2, 4) == 1);
      ASSERT_FILES(2, 1 + job_fails, n + job_fails);
      assert(err_count == 0);
      if (worker)
      {
         autosave_t *h;
         event_save_items_fill(false);
         assert((h = save_item_files.files[1].autosave));
         h->interval = 0;
         wait_requested(h);
         if (pass == 3)
         {
            set_fail_save_writes(true);
            capture(h);
            set_fail_save_writes(false);
            /* Killed here: y2's untold file is its only copy. */
            assert(disk_holds("/s/a/g.srm", y1, 4));
            assert(disk_named_holds("RetroArch-recovery-", y2, 4) == 1);
            ASSERT_FILES(2, 1, n);
            assert(err_count == 0);
         }
         capture(h);
      }
      else
         assert(save_files_unreaped());
      /* Killed here. */
      assert(disk_holds("/s/a/g.srm", y2, 4));
      assert(disk_named_holds("/undo/", xfile, 16) == 1);
      assert(disk_named_holds("/undo/", yold, 4) == 1);
      assert(!disk_named_holds("/undo/", y1, 4));
      assert(!disk_named_holds("RetroArch-recovery-", y2, 4));
      assert(told_holding(y1, 4) == job_fails);
      ASSERT_FILES(2, job_fails, n + job_fails);
      assert(err_count == 0);
      if (worker)
         event_save_items_fill(false);
      else
         frame_start_reap();
      assert(disk_holds("/s/a/g.srm", y2, 4));
      ASSERT_FILES(2, job_fails, n + job_fails);
      assert(err_count == 0);
      autosave_deinit();
   }
}

/* SAVE_FILES's hand to the copy job of /s/a/g.srm finds it done. */
static void job_done_first(void)
{
   unsigned i;
   for (i = 0; i < autosave_state.num_retired; i++)
      if (     autosave_state.retired[i]->job
            && !strcmp(autosave_state.retired[i]->path, "/s/a/g.srm"))
         wait_tried(autosave_state.retired[i], 99);
}

/* The same, but the orphan's copy fails: the file stays, y1 goes to a
 * recovery file shown once, and the record's images follow into it
 * until the copy is made. Passes: the job fails after SAVE_FILES hands
 * it y1; before, so SAVE_FILES takes the failed copy itself; the first
 * again, with the reap's write into the record's file failing. */
static void test_busy_name_took_fails(void)
{
   static const unsigned char yold[4] = {0x31, 0x31, 0x31, 0x31};
   static const unsigned char y1[4]   = {0x41, 0x41, 0x41, 0x41};
   static const unsigned char y2[4]   = {0x42, 0x42, 0x42, 0x42};
   unsigned char xfile[16];
   struct retro_save_item items[2];
   unsigned pass;

   for (pass = 0; pass < 3; pass++)
   {
      unsigned n, held;
      bool done_first = pass == 1;
      start();
      app_dir = "/app";
      n = busy_name_took(xfile, yold, y1, items);
      memcpy(blob_state, y2, 4);
      set_fail_undo(true);
      held = held_count();
      if (done_first)
      {
         main_malloc_hook      = job_done_first;
         main_malloc_hook_size = sizeof(autosave_image_t) + 4;
      }
      else
         set_hold(&hold_undo, true);
      event_save_files(true, false, NULL);
      main_malloc_hook = NULL;
      if (!done_first)
      {
         wait_held(held + 1);
         set_hold(&hold_undo, false);
      }
      copies_wait(false);
      /* Killed here. */
      assert(disk_holds("/s/a/g.srm", yold, 4));
      assert(disk_named_holds("RetroArch-recovery-", y2, 4) == 1);
      assert(!done_first || !disk_named_holds("RetroArch-recovery-", y1, 4));
      assert(done_first || disk_named_holds("RetroArch-recovery-", y1, 4));
      assert(osd_count == n + done_first);
      assert(err_count == 0);
      autosave_reap(true);
      assert(disk_holds("/s/a/g.srm", yold, 4));
      assert(disk_named_holds("RetroArch-recovery-", y2, 4) == 1);
      /* The told file holds y1 until the record's next write, which
       * went there already when SAVE_FILES told it. */
      assert(told_holding(done_first ? y2 : y1, 4) == 1);
      ASSERT_FILES(1, 2 - done_first, n + 1);
      held = held_count();
      set_hold(&hold_undo, true);
      event_save_files(true, false, NULL);
      wait_held(held + 1);
      set_hold(&hold_undo, false);
      copies_wait(false);
      /* Killed here: the record's file holds y2, so the job's failed
       * copy leaves no file of its own. */
      assert(disk_holds("/s/a/g.srm", yold, 4));
      assert(disk_named_holds("RetroArch-recovery-", y2, 4) == 1);
      assert(told_holding(done_first ? y2 : y1, 4) == 1);
      ASSERT_FILES(1, 2 - done_first, n + 1);
      assert(err_count == 0);
      if (pass == 2)
         set_fail_writes(true);
      frame_start_reap();
      set_fail_writes(false);
      assert(disk_holds("/s/a/g.srm", yold, 4));
      assert(disk_named_holds("RetroArch-recovery-", y2, 4) == 1);
      assert(told_holding(y2, 4) == 1);
      ASSERT_FILES(1, 1, n + 1);
      assert(err_count == 0);
      set_fail_undo(false);
      save_files();
      assert(disk_holds("/s/a/g.srm", yold, 4));
      assert(disk_named_holds("/undo/", yold, 4) == 1);
      assert(save_files());
      assert(disk_holds("/s/a/g.srm", y2, 4));
      assert(!disk_named_holds("/undo/", y1, 4));
      ASSERT_FILES(2, 1, n + 1);
      assert(err_count == 0);
      autosave_deinit();
   }
}

/* The same, but with no undo folder no copier starts: the orphan's
 * image goes to a recovery file shown once, and the record's after it. */
static void test_busy_name_took_no_undo(void)
{
   static const unsigned char yold[4] = {0x31, 0x31, 0x31, 0x31};
   static const unsigned char y1[4]   = {0x41, 0x41, 0x41, 0x41};
   static const unsigned char y2[4]   = {0x42, 0x42, 0x42, 0x42};
   unsigned char xfile[16];
   struct retro_save_item items[2];
   unsigned n;

   start();
   app_dir = "/app";
   n = busy_name_took(xfile, yold, y1, items);
   memcpy(blob_state, y2, 4);
   event_save_items_set_undo_dir(NULL);
   save_files_unreaped();
   /* Killed here. */
   assert(disk_holds("/s/a/g.srm", yold, 4));
   assert(disk_named_holds("RetroArch-recovery-", y2, 4) == 1);
   assert(told_holding(y2, 4) == 1);
   ASSERT_FILES(1, 1, n + 1);
   assert(err_count == 0);
   frame_start_reap();
   assert(disk_holds("/s/a/g.srm", yold, 4));
   assert(disk_named_holds("RetroArch-recovery-", y2, 4) == 1);
   assert(told_holding(y2, 4) == 1);
   ASSERT_FILES(1, 1, n + 1);
   assert(err_count == 0);
   event_save_items_set_undo_dir("/undo");
   save_files();
   assert(save_files());
   assert(disk_holds("/s/a/g.srm", y2, 4));
   assert(disk_named_holds("/undo/", yold, 4) == 1);
   ASSERT_FILES(2, 1, n + 1);
   assert(err_count == 0);
   autosave_deinit();
}

/* The same record leaves while it waits: its last image, an orphan
 * too, waits behind the first orphan's copy, and lands after it. */
static void test_busy_name_took_leaves(void)
{
   static const unsigned char yold[4] = {0x31, 0x31, 0x31, 0x31};
   static const unsigned char y1[4]   = {0x41, 0x41, 0x41, 0x41};
   static const unsigned char y3[4]   = {0x43, 0x43, 0x43, 0x43};
   unsigned char xfile[16];
   struct retro_save_item items[2];
   unsigned held, n;

   start();
   app_dir = "/app";
   n = busy_name_took(xfile, yold, y1, items);
   memcpy(blob_state, y3, 4);
   register_items(items, 1);
   assert(save_item_files.num_orphans == 2);
   held = held_count();
   set_hold(&hold_undo, true);
   assert(event_save_files(true, false, NULL));
   wait_held(held + 1);
   assert(save_item_files.num_orphans == 1);
   /* Killed here. */
   assert(disk_holds("/s/a/g.srm", yold, 4));
   assert(disk_named_holds("RetroArch-recovery-", y1, 4) == 1);
   assert(disk_named_holds("RetroArch-recovery-", y3, 4) == 1);
   ASSERT_FILES(1, 2, n);
   set_hold(&hold_undo, false);
   copies_wait(false);
   /* Killed here. */
   assert(disk_holds("/s/a/g.srm", y1, 4));
   assert(!disk_named_holds("RetroArch-recovery-", y1, 4));
   assert(disk_named_holds("RetroArch-recovery-", y3, 4) == 1);
   ASSERT_FILES(2, 1, n);
   assert(err_count == 0);
   frame_start_reap();
   assert(disk_holds("/s/a/g.srm", y1, 4));
   assert(save_files());
   assert(!save_item_files.num_orphans);
   assert(disk_holds("/s/a/g.srm", y3, 4));
   assert(disk_named_holds("/undo/", yold, 4) == 1);
   assert(!disk_named_holds("/undo/", y1, 4));
   ASSERT_FILES(2, 0, n);
   assert(err_count == 0);
   autosave_deinit();
}

#endif

/* A registration from inside blob_size(), blob_save() or
 * get_generation() is refused with one error: the set they read stays,
 * and the write that called them goes on. */
static void test_reregister_in_callback(void)
{
   static const char *const names[3] =
         { "blob_size()", "blob_save()", "get_generation()" };
   const save_items_state_t *st = save_items_state_get_ptr();
   char want[320];
   struct retro_save_item items[1];
   struct retro_save_item other[1];
   unsigned pass;
   other[0] = buffer_item("b", "/s/b.srm");
   for (pass = 0; pass < 3; pass++)
   {
      autosave_t *h = NULL;
      start();
      app_dir        = "/app";
      use_generation = true;
      items[0]       = blob_item("flash", "/s/game.flash");
      disk_put("/s/game.flash", "abcdef", 6);
      register_items(items, 1);
      assert(event_load_save_files(false));
      if (pass == 2)
         h = start_autosave(false);
      /* The earlier call's list: its unlabelled item. */
      assert(st->num_issues == 1
            && st->issues[0].verdict == RETRO_SAVE_VERDICT_IGNORED);
      memcpy(blob_state, "zbcdef", 6);
      generation++;
      rereg_items  = other;
      rereg_n      = 1;
      rereg_inside = pass + 1;
      if (h)
         capture(h);
      else
         assert(save_files_unreaped());
      /* Killed here. */
      assert(rereg_inside == 0 && rereg_refusals == 1);
      assert(err_count == 1 && strstr(err_last, names[pass]));
      assert(save_items_state_get_ptr()->current->num_items == 1);
      /* get_issues lists the refused call's finding alone. */
      snprintf(want, sizeof(want), "Core: save registration: "
            "registration = made from inside the core's %s, which reads "
            "the current set; a core makes it from retro_run(). See "
            "libretro.h, SET_SAVE_INTERFACE. Registration refused.",
            names[pass]);
      assert(st->num_issues == 1 && st->issues_room == 0);
      assert(st->issues[0].item == RETRO_SAVE_ISSUE_NO_ITEM);
      assert(st->issues[0].field == RETRO_SAVE_FIELD_NONE);
      assert(st->issues[0].verdict == RETRO_SAVE_VERDICT_REFUSED);
      assert(!strcmp(st->issues[0].text, want));
      assert(!strcmp(err_last + 7, want) || strstr(err_last, want));
      assert(disk_holds("/s/game.flash", "zbcdef", 6));
      assert(!disk_has("/s/b.srm"));
      ASSERT_FILES(0, 0, 0);
      /* The set is still registered and written. */
      blob_state[0] = 'y';
      generation++;
      if (h)
         capture(h);
      else
         assert(save_files());
      assert(disk_holds("/s/game.flash", "ybcdef", 6));
      assert(err_count == 1);
      ASSERT_FILES(0, 0, 0);
      if (h)
         autosave_deinit();
   }
}

/* A refusal inside a callback before an added item's first fill keeps
 * the room that fill takes: its leave-out follows the refusal, and the
 * list does not move. */
static void test_refused_before_fill(void)
{
   const save_items_state_t *st = save_items_state_get_ptr();
   const struct retro_save_issue *list;
   struct retro_save_segment seg;
   struct retro_save_item items[2];
   struct retro_save_item other[1];

   start();
   app_dir        = "/app";
   use_generation = true;
   path_init_savefile_rtc("/s/game.srm");
   set_savefile   = "/s/game.srm";
   seg.data       = iram;
   seg.size       = sizeof(iram);
   items[0]       = blob_item("flash", "/s/game.flash");
   items[0].label = "Flash";
   disk_put("/s/game.flash", "abcdef", 6);
   register_items(items, 1);
   assert(event_load_save_files(false));
   items[1]              = buffer_item("srm", "/s/game.srm");
   items[1].label        = "Battery";
   items[1].segments     = &seg;
   items[1].num_segments = 1;
   no_ram                = true;
   register_items(items, 2);
   no_ram                = false;
   assert(st->num_issues == 0 && st->issues_room == 1);
   other[0]     = buffer_item("b", "/s/b.srm");
   memcpy(blob_state, "zbcdef", 6);
   generation++;
   rereg_items  = other;
   rereg_n      = 1;
   rereg_inside = REREG_SAVE;
   event_save_files(true, false, NULL);
   assert(rereg_inside == 0 && rereg_refusals == 1);
   assert(st->num_issues == 1 && st->issues_room == 1);
   assert(st->issues[0].verdict == RETRO_SAVE_VERDICT_REFUSED);
   list = st->issues;
   event_save_items_fill(false);
   assert(st->issues == list);
   assert(st->num_issues == 2 && st->issues_room == 0);
   assert(st->issues[0].verdict == RETRO_SAVE_VERDICT_REFUSED);
   assert(st->issues[1].item == 1);
   assert(st->issues[1].verdict == RETRO_SAVE_VERDICT_LEFT_OUT);
   assert(!st->current->items[1].path && st->current->items[0].path);
   assert(disk_holds("/s/game.flash", "zbcdef", 6));
   assert(!disk_has("/s/b.srm"));
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 2 && notices == 1);
}

/* A blob that leaves the set is saved inside the SET that drops it:
 * its image lands at its file through an orphan, or as its retired
 * worker's last write. A registration inside that blob_save() is
 * refused. */
static void test_blob_leaves(void)
{
   struct retro_save_item items[1];
   struct retro_save_item other[1];
   unsigned pass;
   other[0] = buffer_item("b", "/s/b.srm");
   /* 0: an orphan; 1: its worker retires; 2: the same, then SAVE_FILES
    * (an Android pause); 3: an orphan, registering inside blob_save(). */
   for (pass = 0; pass < 4; pass++)
   {
      unsigned saves;
      autosave_t *h = NULL;
      start();
      app_dir  = "/app";
      items[0] = blob_item("flash", "/s/game.flash");
      disk_put("/s/game.flash", "abcdef", 6);
      register_items(items, 1);
      assert(event_load_save_files(false));
      if (pass == 1 || pass == 2)
         h = start_autosave(false);
      memcpy(blob_state, "zbcdef", 6);
      if (pass == 3)
      {
         rereg_items  = other;
         rereg_n      = 1;
         rereg_inside = REREG_SAVE;
      }
      saves = blob_saves;
      register_items(NULL, 0);
      assert(core_calls_in_set == 2 && blob_saves == saves + 1);
      assert(save_items_state_get_ptr()->current->num_items == 0);
      if (pass == 2)
         assert(save_files_unreaped());
      if (h)
         retired_join(false);
      /* Killed here. */
      if (h)
         assert(disk_holds("/s/game.flash", "zbcdef", 6));
      else
      {
         assert(save_item_files.num_orphans == 1);
         assert(disk_holds("/s/game.flash", "abcdef", 6));
      }
      assert(rereg_refusals == (pass == 3 ? 1 : 0));
      assert(err_count == (pass == 3 ? 1 : 0));
      ASSERT_FILES(0, 0, 0);
      frame_start_reap();
      assert(disk_holds("/s/game.flash", h ? "zbcdef" : "abcdef", 6));
      assert(err_count == (pass == 3 ? 1 : 0));
      ASSERT_FILES(0, 0, 0);
      assert(save_files());
      assert(save_item_files.num_orphans == 0);
      assert(disk_holds("/s/game.flash", "zbcdef", 6));
      assert(!disk_has("/s/b.srm"));
      assert(err_count == (pass == 3 ? 1 : 0));
      ASSERT_FILES(0, 0, 0);
      if (h)
         autosave_deinit();
   }
}

/* A registration from inside a blob's own blob_load() that drops it
 * leaves its file as it was: the blob is not saved during that SET.
 * Passes: nothing takes the file; a new item does, filled from it. */
static void test_blob_load_drops(void)
{
   struct retro_save_item items[1];
   struct retro_save_item other[1];
   unsigned pass;
   for (pass = 0; pass < 2; pass++)
   {
      start();
      app_dir  = "/app";
      items[0] = blob_item("flash", "/s/game.flash");
      other[0] = card_item("card", "/s/game.flash");
      disk_put("/s/game.flash", "abcdef", 6);
      memcpy(blob_state, "zzzzzz", 6);
      blob_len        = 6;
      blob_reregister = true;
      rereg_items     = other;
      rereg_n         = pass;
      register_items(items, 1);
      event_load_save_files(false);
      copies_wait(false);
      /* Killed here. */
      assert(!blob_reregister);
      assert(save_item_files.num_orphans == 0);
      assert(disk_holds("/s/game.flash", "abcdef", 6));
      if (pass)
         assert(!memcmp(card, "abcdef", 6));
      ASSERT_FILES(pass, 0, 0);
      assert(save_files());
      assert(disk_holds("/s/game.flash", "abcdef", 6));
      /* The new item's type change copies the file and tells it. */
      if (pass)
         assert(disk_named_holds("/undo/", "abcdef", 6) == 1);
      ASSERT_FILES(pass, 0, pass);
      assert(err_count == 0);
   }
}

/* A copy is named in the undo folder its copier had, when the folder
 * changed while the copy was made. */
static void test_copy_told_old_dir(void)
{
   unsigned char file[12];
   struct retro_save_item items[1];
   autosave_t *h;
   fill_bytes(file, sizeof(file), 0x70);
   start();
   app_dir = "/app";
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = card_item("a", "/s/game.srm");
   register_items(items, 1);
   event_load_save_files(false);
   items[0] = buffer_item("b", "/s/game.srm");
   register_items(items, 1);
   event_save_items_set_undo_dir(NULL);
   event_save_items_fill(false);
   event_save_items_set_undo_dir("/undo");
   set_hold(&hold_undo, true);
   assert(autosave_init(false, 1));
   assert((h = save_item_files.files[0].autosave));
   h->interval = 0;
   wait_held(1);
   event_save_items_set_undo_dir("/other");
   set_hold(&hold_undo, false);
   wait_requested(h);
   /* Killed here. */
   assert(disk_holds("/undo/game.srm", file, sizeof(file)));
   assert(copy_logs == 0);
   ASSERT_FILES(1, 0, 0);
   event_save_items_fill(false);
   assert(copy_logs == 1 && copy_logged("in \"/undo/game.srm\""));
   assert(strstr(osd_last, "/s/game.srm|/undo/game.srm"));
   ASSERT_FILES(1, 0, 1);
   assert(err_count == 0);
   autosave_deinit();
}

/* An old core's save RAM and RTC both cut their files at one load:
 * copies in the undo folder are shown once (pass 0); each copy in a
 * recovery file is shown, naming it (pass 1), as is each cut with no
 * copy at all: no recovery folder (pass 2), or its write fails (pass
 * 3). Loading the same files again in that load adds no copy and no
 * notice. */
static void test_old_core_two_cuts(void)
{
   static const unsigned char srm[6] = {1, 2, 3, 4, 5, 6};
   static const unsigned char rtc[4] = {7, 8, 9, 10};
   unsigned pass;
   for (pass = 0; pass < 4; pass++)
   {
      start();
      app_dir      = pass == 2 ? NULL : "/app";
      cut_recovery = pass == 3;
      use_rtc      = true;
      if (pass)
         event_save_items_set_undo_dir(NULL);
      path_init_savefile_rtc("/s/old.srm");
      disk_put("/s/old.srm", srm, sizeof(srm));
      disk_put("/s/old.rtc", rtc, sizeof(rtc));
      assert(event_load_save_files(false));
      assert(!memcmp(legacy_ram, srm, 4) && !memcmp(legacy_rtc, rtc, 2));
      if (pass == 0)
      {
         assert(disk_holds("/undo/old.srm", srm, 6));
         assert(disk_holds("/undo/old.rtc", rtc, 4));
         ASSERT_FILES(2, 0, 1);
      }
      else if (pass == 1)
      {
         assert(disk_named_holds("RetroArch-recovery-", srm, 6) == 1);
         assert(disk_named_holds("RetroArch-recovery-", rtc, 4) == 1);
         assert(strstr(osd_last, "/s/old.rtc|/app/RetroArch-recovery-"));
         ASSERT_FILES(0, 2, 2);
      }
      if (pass < 2)
      {
         assert(event_load_save_files(false));
         assert(disk_named_holds(pass ? "RetroArch-recovery-" : "/undo/",
               srm, 6) == 1);
         assert(disk_named_holds(pass ? "RetroArch-recovery-" : "/undo/",
               rtc, 4) == 1);
         ASSERT_FILES(pass ? 0 : 2, pass ? 2 : 0, pass ? 2 : 1);
      }
      else
      {
         assert(strstr(osd_last, "/s/old.rtc"));
         assert(   disk_count_named("RetroArch-recovery-")
                == disk_count_named(".tmp"));
         assert(osd_count == 2);
         assert(event_load_save_files(false));
         assert(   disk_count_named("RetroArch-recovery-")
                == disk_count_named(".tmp"));
         ASSERT_FILES(0, disk_count_named(".tmp"), 2);
      }
      assert(err_count == 0);
   }
}

#ifdef HAVE_THREADS
/* SAVE_FILES that cannot take the image of an item whose worker writes
 * fine: shown at the second failure in a row, not at each SAVE_FILES;
 * after a success the next streak is shown again. */
static void test_save_no_image_worker(void)
{
   unsigned char file[12];
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned k;

   start();
   app_dir = "/app";
   fill_bytes(file, sizeof(file), 0x40);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   assert(event_load_save_files(false));
   h       = start_autosave(false);
   wram[0] = 1;
   retro_atomic_store_release_int(&fail_main_malloc_size, 12);
   for (k = 0; k < 4; k++)
   {
      assert(!event_save_files(true, false, NULL));
      event_save_items_fill(false);
      assert(osd_count == (k ? 1 : 0));
   }
   retro_atomic_store_release_int(&fail_main_malloc_size, 0);
   /* Killed here. */
   assert(!retro_atomic_load_acquire_int(&h->failed));
   assert(disk_holds("/s/game.srm", file, 12));
   ASSERT_FILES(0, 0, 1);
   assert(err_count == 4);
   assert(event_save_files(true, false, NULL));
   assert(disk_holds("/s/game.srm", image(), 12));
   wram[0] = 2;
   retro_atomic_store_release_int(&fail_main_malloc_size, 12);
   assert(!event_save_files(true, false, NULL));
   assert(osd_count == 1);
   assert(!event_save_files(true, false, NULL));
   retro_atomic_store_release_int(&fail_main_malloc_size, 0);
   ASSERT_FILES(0, 0, 2);
   assert(err_count == 6);
   autosave_deinit();
}

/* test_save_files_cut_copying with no recovery folder: the newest
 * image is only in memory until the worker writes it after the copy,
 * so the failed write is shown at the second in a row. */
static void test_untold_write_fails(void)
{
   unsigned char file[20];
   unsigned char newest[8];
   struct retro_save_item items[1];
   autosave_t *h;

   start();
   fill_bytes(file, sizeof(file), 0xd0);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = buffer_item("srm", "/s/game.srm");
   register_items(items, 1);
   assert(event_load_save_files(false));
   h       = start_autosave(false);
   wram[0] = 1;
   items[0].num_segments = 1;
   retro_atomic_store_release_int(&fail_malloc_size, 12);
   register_items(items, 1);
   retro_atomic_store_release_int(&fail_malloc_size, 0);
   assert(err_count == 1 && strstr(err_last, "cut by"));
   assert(save_item_files.files[0].guard.keep_file);
   memcpy(newest, image(), sizeof(newest));
   assert(!event_save_files(true, false, NULL));
   /* Killed here. */
   assert(disk_holds("/s/game.srm", file, sizeof(file)));
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 2);
   assert(!event_save_files(true, false, NULL));
   ASSERT_FILES(0, 0, 1);
   assert(strstr(osd_last, "/s/game.srm"));
   assert(err_count == 3);
   assert(!event_save_files(true, false, NULL));
   ASSERT_FILES(0, 0, 1);
   assert(err_count == 4);
   capture(h);
   event_save_items_fill(false);
   assert(disk_holds("/undo/game.srm", file, sizeof(file)));
   assert(disk_holds("/s/game.srm", newest, 8));
   ASSERT_FILES(1, 0, 1);
   assert(err_count == 4);
   autosave_deinit();
}
#endif

#ifdef HAVE_THREADS
/* A retired worker of a kept file, still owing its copy, is handed an
 * image it cannot write without that copy: it puts it in a recovery
 * file no one is told of. A newer image then goes to the item's
 * recovery file, and that older one goes at the reap. */
static void test_retired_left_superseded(void)
{
   unsigned char before[12];
   unsigned char want[12];
   unsigned char y1[12];
   unsigned char y2[12];
   struct retro_save_item items[1];
   autosave_t *h;
   unsigned i;
   unsigned n;

   start();
   app_dir         = "/app";
   disk_put("/s/t.srm", image(), 12);
   items[0]        = buffer_item("a", "/s/t.srm");
   items[0].format = "core:a";
   register_items(items, 1);
   event_load_save_files(false);
   memcpy(before, image(), sizeof(before));
   wram[1]         = 0x44;
   memcpy(want, image(), sizeof(want));
   items[0].id     = "b";
   items[0].format = "core:b";
   register_items(items, 1);
   event_save_items_set_undo_dir(NULL);
   event_save_items_fill(false);
   event_save_items_set_undo_dir("/undo");
   set_fail_undo(true);
   h = start_autosave(false);
   /* Ended owing the copy: its last write tries it again, held first. */
   set_hold(&hold_undo, true);
   n = osd_count;
   register_items(NULL, 0);
   wait_held(1);
   register_items(items, 1);
   event_save_items_fill(false);
   assert(!save_item_files.files[0].guard.copy_first);
   wram[2] = 9;
   memcpy(y1, image(), sizeof(y1));
   assert(event_save_files(true, false, NULL));
   set_hold(&hold_undo, false);
   for (i = 0; i < 5000 && !retro_atomic_load_acquire_int(&h->exited); i++)
      retro_sleep(1);
   assert(retro_atomic_load_acquire_int(&h->exited));
   retired_join(false);
   /* Killed here: its last image and the old type's are recovered too. */
   assert(disk_holds("/s/t.srm", before, 12));
   assert(disk_named_holds("RetroArch-recovery-", want, 12) == 2);
   assert(disk_named_holds("RetroArch-recovery-", y1, 12) == 1);
   ASSERT_FILES(0, 3, n);
   wram[3] = 7;
   memcpy(y2, image(), sizeof(y2));
   assert(!event_save_files(true, false, NULL));
   /* Killed here. */
   assert(disk_holds("/s/t.srm", before, 12));
   assert(disk_named_holds("RetroArch-recovery-", y2, 12) == 1);
   assert(told_holding(y2, 12) == 1);
   ASSERT_FILES(0, 4, n + 1);
   autosave_reap(false);
   assert(!autosave_state.num_retired);
   assert(disk_named_holds("RetroArch-recovery-", y2, 12) == 1);
   assert(told_holding(want, 12) == 1);
   ASSERT_FILES(0, 2, n + 2);
   assert(err_count == 0);
   set_fail_undo(false);
   assert(save_files());
   assert(disk_holds("/s/t.srm", image(), 12));
   assert(disk_holds("/undo/t.srm", before, 12));
   assert(strstr(osd_last, "/s/t.srm|/undo/t.srm"));
   ASSERT_FILES(1, 2, n + 3);
   assert(err_count == 0);
   autosave_deinit();
}

/* test_retired_left_superseded for a cut file the item has no recovery
 * file of: the newer image lands at the file, and the older one's
 * recovery file goes at the reap untold. */
static void test_retired_left_written(void)
{
   unsigned char file[16];
   unsigned char y1[16];
   unsigned char y2[16];
   struct retro_save_item items[1];

   start();
   app_dir        = "/app";
   fill_bytes(file, sizeof(file), 0x50);
   disk_put("/s/t.srm", file, sizeof(file));
   items[0]       = buffer_item("a", "/s/t.srm");
   items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   register_items(items, 1);
   load_for_worker();
   assert(save_item_files.files[0].guard.copy_first);
   assert(!save_item_files.files[0].guard.keep_file);
   set_fail_undo(true);
   start_autosave(false);
   /* Ended owing the copy: its last write tries it again, held first. */
   set_hold(&hold_undo, true);
   register_items(NULL, 0);
   wait_held(1);
   /* Back without EXACT_SIZE: it owes nothing. */
   items[0].flags = 0;
   register_items(items, 1);
   event_save_items_fill(false);
   assert(!save_item_files.files[0].guard.copy_first);
   wram[2] = 9;
   memcpy(y1, image(), 12);
   memcpy(y1 + 12, file + 12, 4);
   assert(event_save_files(true, false, NULL));
   /* Its last image lands; y1's write fails. */
   set_hold(&hold_save_temp, true);
   set_hold(&hold_undo, false);
   wait_held(2);
   set_fail_save_writes(true);
   set_hold(&hold_save_temp, false);
   retired_join(false);
   set_fail_save_writes(false);
   /* Killed here. */
   assert(disk_holds("/s/t.srm", file, 16));
   assert(disk_named_holds("RetroArch-recovery-", y1, 16) == 1);
   ASSERT_FILES(0, 1, 0);
   wram[3] = 7;
   memcpy(y2, image(), 12);
   memcpy(y2 + 12, file + 12, 4);
   assert(event_save_files(true, false, NULL));
   /* Killed here. */
   assert(disk_holds("/s/t.srm", y2, 16));
   assert(disk_named_holds("RetroArch-recovery-", y1, 16) == 1);
   ASSERT_FILES(0, 1, 0);
   autosave_reap(false);
   assert(!autosave_state.num_retired);
   assert(disk_holds("/s/t.srm", y2, 16));
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 0);
   set_fail_undo(false);
   assert(save_files());
   assert(disk_holds("/s/t.srm", y2, 16));
   assert(disk_holds("/undo/t.srm", y2, 16));
   ASSERT_FILES(1, 0, 0);
   assert(err_count == 0);
   autosave_deinit();
}

/* test_retired_left_written for the retired worker's own last write
 * failing: the newer image lands at the file, and the last one's
 * recovery file goes at the reap untold. */
static void test_retired_final_written(void)
{
   unsigned char file[16];
   unsigned char y1[16];
   unsigned char y2[16];
   struct retro_save_item items[1];

   start();
   app_dir        = "/app";
   fill_bytes(file, sizeof(file), 0x50);
   disk_put("/s/t.srm", file, sizeof(file));
   items[0]       = buffer_item("a", "/s/t.srm");
   items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   register_items(items, 1);
   load_for_worker();
   assert(save_item_files.files[0].guard.copy_first);
   assert(!save_item_files.files[0].guard.keep_file);
   set_fail_undo(true);
   start_autosave(false);
   wram[2] = 9;
   memcpy(y1, image(), 12);
   memcpy(y1 + 12, file + 12, 4);
   set_fail_save_writes(true);
   register_items(NULL, 0);
   retired_join(false);
   set_fail_save_writes(false);
   /* Killed here. */
   assert(disk_holds("/s/t.srm", file, 16));
   assert(disk_named_holds("RetroArch-recovery-", y1, 16) == 1);
   ASSERT_FILES(0, 1, 0);
   /* Back without EXACT_SIZE: it owes nothing. */
   items[0].flags = 0;
   register_items(items, 1);
   event_save_items_fill(false);
   assert(!save_item_files.files[0].guard.copy_first);
   assert(!memcmp(image(), y1, 12));
   wram[3] = 7;
   memcpy(y2, image(), 12);
   memcpy(y2 + 12, file + 12, 4);
   assert(event_save_files(true, false, NULL));
   /* Killed here. */
   assert(disk_holds("/s/t.srm", y2, 16));
   assert(disk_named_holds("RetroArch-recovery-", y1, 16) == 1);
   ASSERT_FILES(0, 1, 0);
   autosave_reap(false);
   assert(!autosave_state.num_retired);
   assert(disk_holds("/s/t.srm", y2, 16));
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 0);
   set_fail_undo(false);
   assert(save_files());
   assert(disk_holds("/s/t.srm", y2, 16));
   assert(disk_holds("/undo/t.srm", y2, 16));
   ASSERT_FILES(1, 0, 0);
   assert(err_count == 0);
   autosave_deinit();
}

/* test_copy_failed_again, the job started again after the recovery
 * write failing to start its thread: nothing leaks, the image is
 * recovered once, and a later job makes the copy. */
static void test_copy_job_start_fails(void)
{
   static const unsigned char old[4] = {7, 7, 7, 7};
   unsigned n;
   unsigned errs;

   start();
   app_dir = "/app";
   set_fail_undo(true);
   load_refused_blob(old);
   copies_wait(true);
   assert(save_item_files.files[0].guard.copy_failed);
   n    = osd_count;
   errs = err_count;
   memset(blob_state, 3, 4);
   retro_atomic_store_release_int(&fail_thread_starts, 1);
   assert(!event_save_files(true, false, NULL));
   retro_atomic_store_release_int(&fail_thread_starts, 0);
   assert(!save_item_files.files[0].guard.job);
   assert(disk_named_holds("RetroArch-recovery-", blob_state, 4) == 1);
   assert(disk_holds("/s/game.blob", old, 4));
   ASSERT_FILES(0, 1, n + 1);
   assert(err_count == errs + 1);
   set_fail_undo(false);
   assert(!event_save_files(true, false, NULL));
   copies_wait(true);
   assert(disk_holds("/undo/game.blob", old, 4));
   assert(save_files() && disk_holds("/s/game.blob", blob_state, 4));
   ASSERT_FILES(1, 1, n + 1);
   assert(err_count == errs + 1);
}

/* test_busy_job_type_change with no recovery folder: the images the
 * busy copier keeps from the file stay in memory only, shown at the
 * second SAVE_FILES in a row; the next makes the copy and writes them. */
static void test_busy_untold_fails(void)
{
   unsigned char old[12];
   unsigned char want[12];
   unsigned char last[12];
   unsigned char c1[12];
   struct retro_save_item items[1];
   unsigned n;
   unsigned errs;

   start();
   busy_chain(old, want, last, items);
   wram[3] = 5;
   memcpy(c1, image(), sizeof(c1));
   n    = osd_count;
   errs = err_count;
   assert(!event_save_files(true, false, NULL));
   assert(osd_count == n && err_count == errs + 1);
   assert(!event_save_files(true, false, NULL));
   set_hold(&hold_undo, false);
   copies_wait(false);
   /* Killed here. */
   assert(disk_holds("/s/t.srm", old, 12));
   assert(disk_holds("/undo/t.srm", want, 12));
   ASSERT_FILES(1, 0, n + 1);
   assert(strstr(osd_last, "/s/t.srm"));
   assert(err_count == errs + 2);
   assert(save_files());
   assert(disk_holds("/s/t.srm", c1, 12));
   assert(disk_named_holds("/undo/", last, 12) == 1);
   ASSERT_FILES(2, 0, n + 3);
   assert(err_count == errs + 2);
}
#endif

int main(void)
{
   retro_atomic_int_init(&fail_reallocs, 0);
   retro_atomic_int_init(&fail_callocs, 0);
   retro_atomic_int_init(&fail_main_malloc_size, 0);
   retro_atomic_int_init(&fail_malloc_size, 0);
   retro_atomic_int_init(&fail_thread_starts, 0);
   main_thread  = sthread_get_current_thread_id();
   disk_lock    = slock_new();
   disk_cond    = scond_new();
   assert(disk_lock && disk_cond);
   segs[0].data = wram;
   segs[0].size = sizeof(wram);
   segs[1].data = iram;
   segs[1].size = sizeof(iram);
   card_seg.data = card;
   card_seg.size = sizeof(card);

   test_write_on_difference();
   test_raw_by_default();
   test_load_prefix_and_cut();
   test_blob();
   test_blob_reregister_refused();
   test_reregister_in_callback();
   test_refused_before_fill();
   test_blob_leaves();
   test_blob_load_drops();
   test_copy_told_old_dir();
   test_legacy_retired();
   test_reported_at_fill();
   test_left_late_copied();
   test_left_out_back();
   test_left_out_back_retired();
   test_given_back_saved();
   test_given_back_shorter();
   test_given_back_unsized();
   test_told_no_memory();
   test_memory_size();
   test_old_file_unlisted();
   test_blob_unsaved_logged();
   test_autosave_item();
   test_generation();
   test_generation_stuck();
   test_blob_autosave_unsaved();
   test_snapshot_wait();
   test_snapshot_cap();
   test_snapshot_reregister();
   test_retire_without_join();
   test_orphans();
   test_late_fill();
   test_ownership();
   test_ownership_patterns();
   test_tail_written_back();
   test_port_move_tail();
   test_storage_change_tail();
   test_flag_change_tail();
   test_flag_change_tail_worker();
   test_close_no_memory_tail();
   test_cut_no_memory_tail();
   test_cut_no_memory_kept();
   test_reshape();
   test_exact_size_cut_after_copy();
   test_rzip_read();
   test_refused();
   test_recovery_same_second();
   test_load_disabled_tail();
   test_old_core_copy();
   test_unreadable_file();
   test_save_files_pending_grow();
   test_tail_no_memory();
   test_orphan_no_memory();
   test_save_write_fails();
   test_recovery_cut();
   test_save_no_image();
   test_worker_failed();
   test_worker_and_main_failed();
   test_close_unfilled();
   test_close_pending_grow();
   test_reshapes_before_fill();
   test_copy_owed_again();
   test_copy_then_cut();
   test_copy_no_file();
   test_copy_numbered();
   test_copy_numbered_full();
   test_copy_numbered_type_change();
   test_save_files_worker_copying();
   test_close_worker_copying();
   test_copy_same_name();
   test_start_after_empty();
   test_start_retry();
   test_empty_retry();
   test_start_retry_settings();
   test_retire_no_memory();
   test_save_files_mid_write();
   test_save_files_at_rename();
   test_retired_then_new_item();
   test_save_files_during_retired_write();
   test_given_back_held();
   test_given_back_autosave();
   test_given_back_reinit();
   test_given_back_close();
   test_retired_other_spelling();
   test_undo_dir();
   test_loaded();
   test_final_write();
   test_snapshot_close();
   test_snapshot_close_owed();
   test_snapshot_close_superseded();
   test_snapshot_close_main_settled();
   test_snapshot_close_reissue_failed();
   test_legacy_paths();
   test_late_loaded();
   test_loaded_reregister_moves();
   test_legacy_load();
   test_legacy_raw();
   test_legacy_newer();
   test_legacy_reregister();
   test_grow_told();
   test_waiting_image();
   test_waiting_other_spelling();
   test_waiting_refused();
   test_port_move();
   test_type_change_waiting();
   test_type_change_uncopied();
   test_type_change_twice();
   test_type_change_blob_gone();
   test_type_change_no_memory();
   test_blob_reregister_taken();
   test_orphan_no_memory_type_change();
#ifdef HAVE_THREADS
   test_waiting_refused_worker();
   test_retired_copy_passes();
   test_save_files_retired_copy();
   test_reap_before_fill();
   test_retired_take_refused();
   test_copy_off_main();
   test_copy_job_handoff();
   test_copy_job_done();
   test_worker_woken_for_copy();
   test_close_joins_copy();
   test_copy_failed_again();
   test_copy_job_fails_handed();
   test_orphan_job_fails_handed();
   test_orphan_job_dumps_copy();
   test_failed_job_detached();
   test_type_change_retired_busy();
   test_retired_owes_and_type_change();
   test_cut_after_handed();
   test_type_change_copy_fails();
   test_carried_copy_handed_twice();
   test_cut_handed_twice();
   test_hand_no_memory();
   test_carry_no_memory();
   test_copy_failed_then_made();
   test_carrier_tells_each_copy();
   test_job_dump_then_written();
   test_carried_orphan();
   test_carried_dump_type();
   test_worker_copy_fails();
   test_carrier_final_and_copy_fail();
   test_job_copy_after_recovery();
   test_job_dump_own_name();
   test_retired_dump_superseded();
   test_carrier_own_copy_late();
   test_job_dump_cut();
   test_busy_job_type_change();
   test_busy_orphan_image();
   test_busy_retired_no_carry();
   test_busy_orphan_untold_fails();
   test_busy_close();
   test_close_join_copies();
   test_save_files_cut_copying();
   test_busy_name_job();
   test_given_back_busy_close();
   test_busy_name_close();
   test_busy_close_stopped();
   test_busy_name_retired();
   test_busy_name_carrier();
   test_busy_name_untold_fails();
   test_busy_name_superseded();
   test_busy_orphan_handed_kept();
   test_busy_name_took();
   test_busy_name_took_fails();
   test_busy_name_took_no_undo();
   test_busy_name_took_leaves();
   test_save_no_image_worker();
   test_untold_write_fails();
   test_retired_left_superseded();
   test_retired_left_written();
   test_retired_final_written();
   test_copy_job_start_fails();
   test_busy_untold_fails();
#endif
   test_kept_move_type_change();
   test_old_core_two_cuts();

   event_save_items_deinit();
   save_items_clear();
   path_deinit_savefile();
   scond_free(disk_cond);
   slock_free(disk_lock);
   printf("save_items_io_test: OK\n");
   return 0;
}
#else
/* No worker: the close's SAVE_FILES writes the image while
 * can_snapshot() says true, and the final write, the game still saving
 * it, keeps that image, logged once. Pass 1: a frame runs between them,
 * so the final write writes the image now. */
static void test_snapshot_close_unthreaded(void)
{
   unsigned char settled[12];
   struct retro_save_item items[1];
   unsigned pass;

   for (pass = 0; pass < 2; pass++)
   {
      unsigned i;
      unsigned files = 0;
      start();
      app_dir          = "/app";
      use_can_snapshot = true;
      items[0] = buffer_item("srm", "/s/close.srm");
      register_items(items, 1);
      event_load_save_files(false);
      wram[0] = 1;
      memcpy(settled, image(), sizeof(settled));
      assert(event_save_files(true, false, NULL));
      /* Killed here. */
      assert(disk_holds("/s/close.srm", settled, 12));
      if (pass)
         event_save_items_fill(false);
      snapshot_ready = false;
      /* Written while unloading. */
      wram[1] = 2;
      event_save_items_final(true, false);
      assert(disk_holds("/s/close.srm", pass ? image() : settled, 12));
      for (i = 0; i < DISK_FILES; i++)
         if (disk[i].exists)
            files++;
      assert(files == 1 && osd_count == 0 && err_count == 0);
      assert(saving_logs == !pass && snapshot_calls == 2);
   }
}

int main(void)
{
   retro_atomic_int_init(&fail_reallocs, 0);
   retro_atomic_int_init(&fail_callocs, 0);
   retro_atomic_int_init(&fail_main_malloc_size, 0);
   retro_atomic_int_init(&fail_malloc_size, 0);
   main_thread  = sthread_get_current_thread_id();
   disk_lock    = slock_new();
   disk_cond    = scond_new();
   assert(disk_lock && disk_cond);
   segs[0].data = wram;
   segs[0].size = sizeof(wram);
   segs[1].data = iram;
   segs[1].size = sizeof(iram);

   test_snapshot_close_unthreaded();
   test_given_back_saved();
   test_given_back_shorter();
   test_given_back_unsized();

   event_save_items_deinit();
   save_items_clear();
   path_deinit_savefile();
   scond_free(disk_cond);
   slock_free(disk_lock);
   printf("save_items_io_test: OK\n");
   return 0;
}
#endif
