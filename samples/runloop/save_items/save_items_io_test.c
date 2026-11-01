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

static void *io_test_main_malloc(size_t len)
{
   int fail = retro_atomic_load_acquire_int(&fail_main_malloc_size);
   if (     fail && (size_t)fail == len
         && sthread_get_current_thread_id() == main_thread)
      return NULL;
   return malloc(len);
}

#define realloc io_test_realloc
#define calloc  io_test_calloc
#define malloc  io_test_main_malloc
#include "../../../save.c"
#undef realloc
#undef calloc
#undef malloc
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
static unsigned memory_asks[2];
static bool hold_write;
static bool fail_writes;
/* Only the copies into /undo/ fail. */
static bool fail_undo;
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
static bool retire_returned;
static bool retire_timed_out;
/* A worker is held after writing a temp, or before or after a rename's
 * move, while these are set; the main thread never is. */
static bool hold_temp;
static bool hold_rename;
static bool hold_renamed;
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
static bool blob_fail_save;
/* blob_save() fills only this many bytes when nonzero. */
static size_t blob_short;
static unsigned blob_sizes;
static unsigned blob_saves;
static unsigned char card[6];
static struct retro_save_segment card_seg;
static uint64_t generation;
static bool use_generation;
/* can_snapshot()'s answer, and its calls. */
static bool use_can_snapshot;
static bool snapshot_ready;
static unsigned snapshot_calls;
/* Log lines that say the game was still saving. */
static unsigned saving_logs;
static const struct retro_save_item *rereg_items;
static unsigned rereg_n;
/* The core registers rereg_items again from inside this callback, once. */
#define REREG_SIZE       1
#define REREG_SAVE       2
#define REREG_GENERATION 3
#define REREG_SNAPSHOT   4
static unsigned rereg_inside;
static unsigned rereg_refusals;

/* The last log line that names a file in /undo/. */
static char copy_log[256];

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
      undo_writes++;
   scond_broadcast(disk_cond);
   while (hold_write)
      scond_wait(disk_cond, disk_lock);
   if ((ok =    !fail_writes
             && !(fail_undo && !strncmp(path, "/undo/", 6))
             && !(fail_save_writes && !strncmp(path, "/s/", 3))))
   {
      f       = disk_find(path, true);
      memcpy(f->data, data, len);
      f->size = len;
      if (is_temp(path))
         worker_hold(&hold_temp);
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
   recovery_writes++;
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

/* As runloop.c's SET_SAVE_INTERFACE case does it; false when refused
 * before anything changed. */
static bool set_items(const struct retro_save_item *items, unsigned n)
{
   struct retro_save_interface iface;
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
   fail_writes  = false;
   fail_undo    = false;
   cut_recovery = false;
   fail_save_writes = false;
   fail_read_path   = NULL;
   hold_temp    = false;
   hold_rename  = false;
   hold_renamed = false;
   worker_holds = failed_renames = 0;
   renamed_from[0] = '\0';
   disk_nocase  = false;
   err_count    = 0;
   err_last[0]  = '\0';
   warn_hist[0] = '\0';
   copy_log[0]  = '\0';
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
   generation     = 0;
   use_generation = false;
   use_can_snapshot = false;
   snapshot_ready   = true;
   snapshot_calls   = 0;
   saving_logs      = 0;
   app_dir        = NULL;
   osd_count      = 0;
   osd_last[0]    = '\0';
   core_calls_in_set = 0;
   rereg_inside      = 0;
   rereg_refusals    = 0;
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

/* Joins the retired handles' threads: they have written their last. */
static void retired_join(void)
{
   unsigned i;
   for (i = 0; i < autosave_state.num_retired; i++)
   {
      sthread_join(autosave_state.retired[i]->thread);
      autosave_state.retired[i]->thread = NULL;
   }
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
   assert(event_save_files(true, false, NULL));
   assert(!disk_has("/s/game.srm") && raw_writes == 0);

   wram[0] = 1;
   iram[3] = 2;
   assert(event_save_files(true, false, NULL));
   assert(raw_writes == 1 && disk_holds("/s/game.srm", image(), 12));

   /* The file holds it already. */
   assert(event_save_files(true, false, NULL));
   assert(raw_writes == 1);

   /* An erase back to the bytes it started with still reaches disk. */
   wram[0] = 0;
   iram[3] = 0;
   assert(event_save_files(true, false, NULL));
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
   assert(event_save_files(true, false, NULL));
   assert(!disk_has("/s/game.flash"));
   blob_state[0] = 'z';
   assert(event_save_files(true, false, NULL));
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
   assert(!event_save_files(true, false, NULL));
   assert(blob_sizes == sizes + 1);
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 1);
   assert(!event_save_files(true, false, NULL));
   assert(!event_save_files(true, false, NULL));
   assert(!disk_has("/s/game.flash"));
   ASSERT_FILES(0, 0, 1);
   assert(err_count == 3);
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
   event_save_files(true, false, NULL);
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
 * it is read or written, and the frontend's own .srm handling keeps
 * the file. */
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
      /* Killed here: the .srm as it was, alone, and loaded into save
       * RAM. */
      assert(disk_count_named("") == 1);
      assert(disk_holds("/s/game.srm", on_disk, sizeof(on_disk)));
      assert(!memcmp(legacy_ram, on_disk, sizeof(on_disk)));
      /* Written as without the item (RTC, not reported, fails). */
      legacy_ram[0] = 9;
      event_save_files(true, false, NULL);
      assert(disk_count_named("") == 1);
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
   assert(st->current->items[0].path);
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
   assert(event_save_files(true, false, NULL));
   assert(disk_holds("/s/stuck.srm", image(), 12));
   assert(warned("Core: save item 0 \"srm\": get_generation = 0: it did "
         "not change while the bytes did. See libretro.h, "
         "retro_save_get_generation_t. Autosave now copies the item every "
         "interval.") == 1);
   wram[0] = 2;
   capture(h);
   capture(h);
   assert(disk_holds("/s/stuck.srm", image(), 12));
   assert(event_save_files(true, false, NULL));
   assert(warned("did not change while the bytes did") == 1);
   autosave_deinit();

   /* Nearest valid: the bytes did not change. */
   start();
   use_generation = true;
   register_items(items, 1);
   event_load_save_files(false);
   h = start_autosave(false);
   capture(h);
   assert(event_save_files(true, false, NULL));
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

   /* A later frame frees it, once it has written the last image. */
   for (i = 0; autosave_state.num_retired && i < 5000; i++)
   {
      autosave_check();
      retro_sleep(1);
   }
   assert(autosave_state.num_retired == 0);
   assert(disk_holds("/s/retire.srm", last, sizeof(last)));
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
   assert(event_save_files(true, false, NULL));
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
   event_save_files(true, false, NULL);
   assert(raw_writes == 0 && disk_holds("/s/one.card", saved, 6));
   event_save_items_fill(false);
   assert(!save_item_files.fill_pending && save_item_files.files[1].taken_up);
   assert(!memcmp(card, saved, sizeof(card)));
   assert(event_save_files(true, false, NULL));
   assert(raw_writes == 0);

   /* The same id moved to another file reads that file. */
   disk_put("/s/two.card", other, sizeof(other));
   items[1].path = "/s/two.card";
   register_items(items, 2);
   assert(save_item_files.fill_pending);
   event_save_items_fill(false);
   assert(!memcmp(card, other, sizeof(card)));
   assert(event_save_files(true, false, NULL));
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
   assert(event_save_files(true, false, NULL));
   assert(!disk_has("/s/new.card"));
   card[0] = 'F';
   assert(event_save_files(true, false, NULL));
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
   assert(event_save_files(true, false, NULL));
   assert(!content_savefile_is_live("/s/Game.srm"));

   /* After a failed write its image is in a recovery file. */
   start();
   app_dir = "/app";
   register_items(items, 1);
   event_load_save_files(false);
   wram[0] = 7;
   register_items(NULL, 0);
   set_fail_save_writes(true);
   assert(!event_save_files(true, false, NULL));
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
   assert(event_save_files(true, false, NULL) && raw_writes == 0);
   memcpy(want, file, sizeof(want));
   wram[0] = want[0] = 1;
   assert(event_save_files(true, false, NULL));
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
   assert(event_save_files(true, false, NULL));
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
   assert(event_save_files(true, false, NULL));
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
   assert(event_save_files(true, false, NULL));
   assert(raw_writes == 0 && disk_holds("/s/game.sav", file, 16));
   memcpy(blob_state, "abcdef", 6);
   blob_len = 6;
   assert(event_save_files(true, false, NULL));
   assert(disk_holds("/s/game.sav", "abcdef", 6));

   items[0] = buffer_item("sav", "/s/game.sav");
   register_items(items, 1);
   assert(!save_items_state_get_ptr()->current->items[0].tail_size);
   event_save_items_fill(true);
   wram[0] = 1;
   assert(event_save_files(true, false, NULL));
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
      {
         event_save_items_fill(false);
         memcpy(want, file, sizeof(want));
      }
      assert(event_save_files(true, false, NULL));
      assert(disk_holds("/s/game.srm", want, 16));
      event_save_items_fill(false);
      wram[1] = want[1] = 2;
      assert(event_save_files(true, false, NULL));
      assert(disk_holds("/s/game.srm", want, 16));
   }
}

/* As above with a worker: its last image keeps the end, renamed
 * before the successor's fill reads the file (pass 0) or after. */
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
         assert(!memcmp(image(), file, 12));
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
      assert(event_save_files(true, false, NULL));
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
   assert(event_save_files(true, false, NULL));
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
   assert(event_save_files(true, false, NULL));
   assert(disk_holds("/s/game.srm", want, 20));
   event_save_items_fill(false);
   wram[1] = want[1] = 2;
   assert(event_save_files(true, false, NULL));
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
      /* The 4 cut bytes and the end past the item. */
      retro_atomic_store_release_int(&fail_malloc_size, (int)size - 8);
      register_items(items, 1);
      retro_atomic_store_release_int(&fail_malloc_size, 0);
      assert(err_count == 1 && strstr(err_last, "cut by"));
      assert(     save_item_files.files[0].taken_up
               && save_item_files.files[0].autosave == h
               && !save_item_files.num_orphans);

      set_fail_undo((pass & 1) != 0);
      if (h)
         capture(h);
      else
         assert(event_save_files(true, false, NULL) == !(pass & 1));
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
            assert(event_save_files(true, false, NULL));
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
   assert(event_save_files(true, false, NULL) && raw_writes == 0);

   memset(iram, 0xee, sizeof(iram));
   items[0].num_segments = 2;
   register_items(items, 1);
   assert(iram[0] == 0xee && save_item_files.files[0].grow_from == 8);
   assert(event_save_files(true, false, NULL) && raw_writes == 0);
   event_save_items_fill(false);
   it = &save_items_state_get_ptr()->current->items[0];
   assert(!memcmp(iram, file + 8, 4) && it->tail_size == 4);
   assert(!save_item_files.files[0].grow_from);
   assert(event_save_files(true, false, NULL) && raw_writes == 0);
}

/* EXACT_SIZE: the file is copied, then cut to the item. */
static void test_exact_size_cut_after_copy(void)
{
   unsigned char file[16];
   unsigned char want[16];
   struct retro_save_item items[1];
   autosave_t *h;

   start();
   app_dir = "/app";
   fill_bytes(file, sizeof(file), 0x20);
   disk_put("/s/game.srm", file, sizeof(file));
   items[0]       = buffer_item("srm", "/s/game.srm");
   items[0].flags = RETRO_SAVE_FLAG_EXACT_SIZE;
   register_items(items, 1);
   assert(event_load_save_files(false));
   assert(save_item_files.files[0].guard.copy_first);
   assert(event_save_files(true, false, NULL));
   assert(disk_holds("/undo/game.srm", file, 16));
   assert(disk_holds("/s/game.srm", file, 12));
   assert(raw_writes == 2 && undo_writes == 1);
   assert(!save_item_files.files[0].guard.copy_first);
   assert(!save_items_state_get_ptr()->current->items[0].tail_size);
   assert(event_save_files(true, false, NULL));
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
   assert(event_save_files(true, false, NULL));
   assert(raw_writes == 1 && undo_writes == 0);
   assert(save_item_files.files[0].guard.copy_first);
   memcpy(want, image(), 12);
   memcpy(want + 12, file + 12, 4);
   assert(!disk_named("/undo/") && disk_holds("/s/game.srm", want, 16));
   ASSERT_FILES(0, 0, 0);
   event_save_items_set_undo_dir("/undo");
   assert(event_save_files(true, false, NULL));
   assert(disk_holds("/s/game.srm", image(), 12));
   assert(disk_has("/undo/game.srm"));
   ASSERT_FILES(1, 0, 0);

   /* The worker copies on its own thread; the frame start collects. */
   start();
   disk_put("/s/game.srm", file, sizeof(file));
   register_items(items, 1);
   event_load_save_files(false);
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
   assert(event_save_files(true, false, NULL));
   assert(disk_holds("/undo/game.srm", file, 12));
   assert(disk_holds("/s/game.srm", file, 8));
   ASSERT_FILES(1, 0, 0);
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
   assert(event_save_files(true, false, NULL));
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
   assert(!event_save_files(true, false, NULL));
   assert(disk_holds("/s/game.blob", old, 4));
   assert(disk_named("RetroArch-recovery-") && osd_count == 2);
   assert(strstr(osd_last, "/s/game.blob|/app/RetroArch-recovery-"));

   /* Once per item: the same image is not recovered again, and a newer
    * one replaces it in the file the warning named. */
   assert(!event_save_files(true, false, NULL));
   assert(recovery_writes == 1 && osd_count == 2);
   blob_state[0] = 4;
   assert(!event_save_files(true, false, NULL));
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
   assert(!event_save_files(true, false, NULL));
   assert(recovery_writes == 2);
   assert(disk_count_named("/app/RetroArch-recovery-") == 2);
   blob_state[0] = 4;
   assert(!event_save_files(true, false, NULL));
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
   assert(event_save_files(true, false, NULL));
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
   event_save_files(true, false, NULL);
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
   event_save_files(true, false, NULL);
   assert(disk_holds("/s/game.srm", file, 12));
   assert(disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
   ASSERT_FILES(0, 1, 1);
   assert(err_count == 1);

   set_fail_read(NULL);
   wram[0] = 2;
   event_save_files(true, false, NULL);
   assert(disk_holds("/undo/game.srm", file, 12));
   assert(   disk_holds("/s/game.srm", image(), 12)
          || disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
   ASSERT_FILES(1, 1, 1);
   /* Written once the copy is made. */
   assert(event_save_files(true, false, NULL));
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
   assert(event_save_files(true, false, NULL));
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
      event_save_files(true, false, NULL);
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
   assert(!event_save_files(true, false, NULL));
   wram[0] = 2;
   assert(!event_save_files(true, false, NULL));
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
   assert(!event_save_files(true, false, NULL));
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
   assert(!event_save_files(true, false, NULL));
   ASSERT_FILES(0, 0, 0);
   assert(err_count == 1);
   assert(!event_save_files(true, false, NULL));
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
   assert(!event_save_files(true, false, NULL));
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
   assert(event_save_files(true, false, NULL));
   assert(raw_writes == 0 && disk_holds("/s/one.card", saved, 6));

   register_items(items, 2);
   twice[0] = buffer_item("a", "/s/x.srm");
   twice[1] = card_item("a", "/s/y.srm");
   register_refused(twice, 2);
   assert(save_item_files.num_orphans == 1);
   assert(event_save_files(true, false, NULL));
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
      assert(event_save_files(true, false, NULL));
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
      assert(event_save_files(true, false, NULL));
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
   event_load_save_files(false);
   h = start_autosave(false);

   /* The worker is held inside the copy the longer file owes. */
   slock_lock(disk_lock);
   hold_write = true;
   slock_unlock(disk_lock);
   iram[0] = 0x77;
   memcpy(want, image(), sizeof(want));
   autosave_lock();
   autosave_unlock();
   slock_lock(disk_lock);
   disk_wait_count(&raw_writes, 1);
   assert(raw_writes == 1);
   slock_unlock(disk_lock);

   items[0].num_segments = 1;
   register_items(items, 1);
   slock_lock(disk_lock);
   hold_write = false;
   scond_broadcast(disk_cond);
   slock_unlock(disk_lock);
   wait_requested(h);
   assert(disk_holds("/undo/game.srm", file, 16));
   assert(disk_holds("/s/game.srm", want, 12));
   event_save_items_fill(false);
   assert(save_item_files.files[0].guard.copy_first);

   /* The next write copies the file holding the cut bytes first, next
    * to the first copy. */
   capture(h);
   assert(disk_holds("/undo/game.srm", file, 16));
   assert(disk_holds("/undo/game-2.srm", want, 12));
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
         assert(event_save_files(true, false, NULL));
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
         assert(event_save_files(true, false, NULL));
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
      assert(event_save_files(true, false, NULL));
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
   assert(event_save_files(true, false, NULL));
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
         assert(event_save_files(true, false, NULL));
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

/* SAVE_FILES while the item's own worker owes the copy: the newest
 * image goes to a recovery file no one is told of, gone once the path
 * holds it, named when the copy fails. */
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
      event_save_items_fill(false);
      assert(save_item_files.files[0].guard.keep_file);
      h = start_autosave(false);
      if (pass == 0)
      {
         /* Held in its copy. */
         set_hold(&hold_temp, true);
         wram[0] ^= 0xff;
         autosave_lock();
         autosave_unlock();
         wait_held(1);
      }
      wram[0] ^= 0x0f;
      memcpy(newest, image(), sizeof(newest));
      assert(event_save_files(true, false, NULL));
      /* Killed here. */
      assert(disk_holds("/s/game.srm", file, sizeof(file)));
      assert(disk_named_holds("RetroArch-recovery-", newest, 12) == 1);
      /* Pass 0: the copy's temp. */
      ASSERT_FILES(pass ? 0 : 1, 1, 0);
      assert(disk_count_named(".auto.tmp") == (pass ? 0 : 1));
      assert(err_count == 0);
      if (pass == 0)
      {
         set_hold(&hold_temp, false);
         wait_requested(h);
         /* K2: the older snapshot is not written over the newer. */
         assert(disk_holds("/s/game.srm", file, sizeof(file)));
         assert(disk_holds("/undo/game.srm", file, sizeof(file)));
         assert(disk_named_holds("RetroArch-recovery-", newest, 12) == 1);
         ASSERT_FILES(1, 1, 0);
         event_save_items_fill(false);
         capture(h);
         event_save_items_fill(false);
         assert(disk_holds("/s/game.srm", newest, 12));
         ASSERT_FILES(1, 0, 1);
         assert(strstr(osd_last, "/s/game.srm|/undo/game.srm"));
         assert(err_count == 0);
      }
      else
      {
         set_fail_undo(true);
         capture(h);
         /* Killed here. */
         assert(disk_holds("/s/game.srm", file, sizeof(file)));
         assert(disk_named_holds("RetroArch-recovery-", newest, 12) == 1);
         ASSERT_FILES(0, 1, 0);
         event_save_items_fill(false);
         ASSERT_FILES(0, 1, 1);
         assert(strstr(osd_last, "RetroArch-recovery-"));
         assert(err_count == 0);
         /* The second failed write in a row is told too. */
         capture(h);
         event_save_items_fill(false);
         ASSERT_FILES(0, 1, 2);
         assert(err_count == 1);
         wram[0] ^= 0x0f;
         assert(!event_save_files(true, false, NULL));
         assert(disk_holds("/s/game.srm", file, sizeof(file)));
         assert(disk_named_holds("RetroArch-recovery-", image(), 12) == 1);
         ASSERT_FILES(0, 1, 2);
         set_fail_undo(false);
      }
      autosave_deinit();
   }
}

/* How many copies in /undo/ are @size bytes and end in @end. */
static unsigned undo_ends(const unsigned char *end, size_t len,
      size_t size)
{
   unsigned i;
   unsigned n = 0;
   slock_lock(disk_lock);
   for (i = 0; i < DISK_FILES; i++)
      if (     disk[i].exists && !strncmp(disk[i].path, "/undo/", 6)
            && disk[i].size == size
            && !memcmp(disk[i].data + size - len, end, len))
         n++;
   slock_unlock(disk_lock);
   return n;
}

/* An item closed while its worker owes the copy of its kept file: the
 * recovery file no one was told of keeps the newest image until the
 * worker's last write has landed it. */
static void test_close_worker_copying(void)
{
   unsigned char file[12];
   unsigned char newest[12];
   struct retro_save_item items[1];

   fill_bytes(file, sizeof(file), 0x30);
   start();
   app_dir = "/app";
   disk_put("/s/game.srm", file, sizeof(file));
   items[0] = card_item("a", "/s/game.srm");
   register_items(items, 1);
   event_load_save_files(false);
   items[0] = buffer_item("b", "/s/game.srm");
   register_items(items, 1);
   event_save_items_fill(false);
   assert(save_item_files.files[0].guard.keep_file);
   start_autosave(false);
   /* Held in its copy. */
   set_hold(&hold_temp, true);
   wram[0] ^= 0xff;
   autosave_lock();
   autosave_unlock();
   wait_held(1);
   wram[0] ^= 0x0f;
   memcpy(newest, image(), sizeof(newest));
   assert(event_save_files(true, false, NULL));
   register_items(NULL, 0);
   /* Killed here: the copy's temp too. */
   assert(disk_holds("/s/game.srm", file, sizeof(file)));
   assert(disk_named_holds("RetroArch-recovery-", newest, 12) == 1);
   ASSERT_FILES(1, 1, 0);
   assert(err_count == 0);
   set_hold(&hold_temp, false);
   retired_join();
   /* Killed here. */
   assert(disk_holds("/s/game.srm", newest, 12));
   assert(disk_holds("/undo/game.srm", file, sizeof(file)));
   ASSERT_FILES(1, 1, 0);
   autosave_reap();
   assert(!autosave_state.num_retired);
   ASSERT_FILES(1, 0, 0);
   assert(err_count == 0);
   autosave_deinit();
}

/* Two items of one name in two folders: each file gets its own copy,
 * one at a time. The one that waits writes keeping its end first. */
static void test_copy_same_name(void)
{
   unsigned char a[16];
   unsigned char b[10];
   struct retro_save_item items[2];
   autosave_t *h0, *h1;

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
   event_load_save_files(false);
   assert(save_item_files.files[0].guard.copy_first);
   assert(save_item_files.files[1].guard.copy_first);
   assert(autosave_init(false, 1));
   assert((h0 = save_item_files.files[0].autosave));
   assert((h1 = save_item_files.files[1].autosave));
   h0->interval = h1->interval = 0;
   wait_requested(h0);
   wait_requested(h1);
   /* Both write at once, each held after its first temp. */
   set_hold(&hold_temp, true);
   wram[0] = 1;
   card[0] = 1;
   autosave_lock();
   autosave_unlock();
   wait_held(2);
   set_hold(&hold_temp, false);
   wait_requested(h0);
   wait_requested(h1);
   wram[0] = 2;
   card[0] = 2;
   capture(h0);
   capture(h1);
   capture(h0);
   capture(h1);
   assert(undo_ends(a + 12, 4, sizeof(a)) == 1);
   assert(undo_ends(b + 6, 4, sizeof(b)) == 1);
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
   assert(event_save_files(true, false, NULL));
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
      assert(event_save_files(true, false, NULL));
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
 * exited. */
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
   for (i = 0; !save_item_files.files[0].autosave && i < 5000; i++)
   {
      autosave_check();
      event_save_items_fill(false);
      retro_sleep(1);
   }
   assert((h = save_item_files.files[0].autosave));
   assert(disk_holds("/s/game.srm", last, 12));
   h->interval = 0;
   wait_requested(h);
   wram[1] = 5;
   capture(h);
   assert(disk_holds("/s/game.srm", image(), 12));
   assert(failed_renames == 0 && !disk_named(".tmp"));
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
   assert(event_save_files(true, false, NULL));
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
   retired_join();
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
   retired_join();
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
   retired_join();
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
         retired_join();
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
   assert(event_save_files(true, false, NULL));
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
         assert(event_save_files(true, false, NULL));
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
         assert(event_save_files(true, false, NULL));
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
         assert(event_save_files(true, false, NULL));
      if (h)
         retired_join();
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
      assert(event_save_files(true, false, NULL));
      assert(save_item_files.num_orphans == 0);
      assert(disk_holds("/s/game.flash", "zbcdef", 6));
      assert(!disk_has("/s/b.srm"));
      assert(err_count == (pass == 3 ? 1 : 0));
      ASSERT_FILES(0, 0, 0);
      if (h)
         autosave_deinit();
   }
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
   card_seg.data = card;
   card_seg.size = sizeof(card);

   test_write_on_difference();
   test_raw_by_default();
   test_load_prefix_and_cut();
   test_blob();
   test_reregister_in_callback();
   test_refused_before_fill();
   test_blob_leaves();
   test_legacy_retired();
   test_reported_at_fill();
   test_told_no_memory();
   test_memory_size();
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
   test_given_back_saved();
   test_retired_other_spelling();
   test_undo_dir();
   test_save_no_image_worker();
   test_untold_write_fails();
   test_old_core_two_cuts();

   event_save_items_deinit();
   save_items_clear();
   path_deinit_savefile();
   scond_free(disk_cond);
   slock_free(disk_lock);
   printf("save_items_io_test: OK\n");
   return 0;
}
