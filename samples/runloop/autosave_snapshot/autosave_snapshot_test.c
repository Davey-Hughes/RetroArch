/* Build with function sections and --gc-sections: only the autosave
 * portion of save.c is linked. Stream stubs control I/O failures. */
#include <assert.h>
#include <stdio.h>
#include <retro_timers.h>
#include "../../../save.c"

#define SRAM_SIZE 17

static slock_t *io_lock;
static scond_t *io_cond;
static unsigned opens;
static unsigned writes;
static unsigned compressed_opens;
static bool hold_write;
static unsigned fail_stage;
static unsigned char disk_image[SRAM_SIZE];
static autosave_t *current;
static unsigned char ram_storage[SRAM_SIZE + 1];
static unsigned char *ram = ram_storage + 1;

intfstream_t *intfstream_open_file(const char *path,
      unsigned mode, unsigned hints)
{
   bool fail;
   (void)path;
   (void)mode;
   (void)hints;
   slock_lock(io_lock);
   opens++;
   fail = fail_stage == 1;
   slock_unlock(io_lock);
   return fail ? NULL : (intfstream_t*)malloc(1);
}

intfstream_t *intfstream_open_rzip_file(const char *path, unsigned mode)
{
   slock_lock(io_lock);
   compressed_opens++;
   slock_unlock(io_lock);
   return intfstream_open_file(path, mode, 0);
}

int64_t intfstream_write(intfstream_t *file, const void *data, uint64_t len)
{
   bool fail;
   (void)file;
   assert(len == SRAM_SIZE);
   slock_lock(io_lock);
   writes++;
   scond_signal(io_cond);
   while (hold_write)
      scond_wait(io_cond, io_lock);
   memcpy(disk_image, data, SRAM_SIZE);
   fail = fail_stage == 2;
   slock_unlock(io_lock);
   return fail ? (int64_t)len - 1 : (int64_t)len;
}

int intfstream_flush(intfstream_t *file)
{
   int result;
   (void)file;
   slock_lock(io_lock);
   result = fail_stage == 3 ? -1 : 0;
   slock_unlock(io_lock);
   return result;
}

int intfstream_close(intfstream_t *file)
{
   int result;
   (void)file;
   slock_lock(io_lock);
   result = fail_stage == 4 ? -1 : 0;
   slock_unlock(io_lock);
   return result;
}

int filestream_finish_atomic_suffix(const char *path, const char *suffix,
      bool ok, bool sync)
{
   (void)path;
   (void)suffix;
   (void)sync;
   return ok ? 0 : -1;
}

int filestream_delete(const char *path)
{
   (void)path;
   return 0;
}

bool path_is_valid(const char *path)
{
   (void)path;
   return true;
}

/* No copy is ever owed here: the handles have no session state. */
enum save_backup_result save_backups_copy_file(const char *path,
      enum save_backup_reason reason, bool sync)
{
   (void)path;
   (void)reason;
   (void)sync;
   return SAVE_BACKUP_FAILED;
}

/* No flush runs here. */
size_t save_backups_recover(char *s, size_t len, const char *path,
      const void *data, size_t data_len, bool sync)
{
   (void)s;
   (void)len;
   (void)path;
   (void)data;
   (void)data_len;
   (void)sync;
   return 0;
}

const char *save_backups_reason_name(enum save_backup_reason reason)
{
   (void)reason;
   return "";
}

const char *path_basename_nocompression(const char *path)
{
   return path;
}

void RARCH_LOG(const char *fmt, ...) { (void)fmt; }
void RARCH_WARN(const char *fmt, ...) { (void)fmt; }

const char *msg_hash_to_str(enum msg_hash_enums msg)
{
   (void)msg;
   return "";
}

void runloop_msg_queue_push(const char *msg, size_t len,
      unsigned prio, unsigned duration, bool flush, char *title,
      enum message_queue_icon icon, enum message_queue_category category)
{
   (void)msg;
   (void)len;
   (void)prio;
   (void)duration;
   (void)flush;
   (void)title;
   (void)icon;
   (void)category;
}

bool core_get_memory(retro_ctx_memory_info_t *info)
{
   info->data = ram;
   info->size = SRAM_SIZE;
   return true;
}

static void wait_requested(void)
{
   unsigned i;
   for (i = 0; i < 5000; i++)
   {
      if (retro_atomic_load_acquire_int(&current->snapshot_requested))
         return;
      retro_sleep(1);
   }
   assert(!"snapshot request timed out");
}

static void capture_and_wait(void)
{
   autosave_lock();
   autosave_unlock();
   wait_requested();
}

static void start_save(bool compress)
{
   memset(ram, 0, SRAM_SIZE);
   current = autosave_new("unused.srm", ram, SRAM_SIZE, 0, compress,
         RETRO_MEMORY_SAVE_RAM, false, NULL);
   assert(current);
   autosave_state.list = &current;
   autosave_state.num  = 1;
   wait_requested();
}

static void stop_save(void)
{
   /* No further frames: shutdown must wake a worker awaiting a copy. */
   autosave_free(current, false);
   current = NULL;
   autosave_state.list = NULL;
   autosave_state.num  = 0;
}

int main(void)
{
   unsigned i;
   unsigned before;
   io_lock = slock_new();
   io_cond = scond_new();
   assert(io_lock && io_cond);
   start_save(false);

   /* Netplay may nest an SRAM scope inside the runloop's frame. */
   autosave_lock();
   autosave_lock();
   autosave_check();
   autosave_unlock();
   assert(retro_atomic_load_acquire_int(&current->snapshot_requested));
   autosave_unlock();
   wait_requested();

   capture_and_wait();
   assert(opens == 0);

   /* Paused/menu iterations service requests without a core frame. */
   memset(ram, 8, SRAM_SIZE);
   autosave_check();
   wait_requested();
   assert(memcmp(disk_image, ram, SRAM_SIZE) == 0);
   slock_lock(io_lock);
   writes = 0;
   slock_unlock(io_lock);

   slock_lock(io_lock);
   hold_write = true;
   slock_unlock(io_lock);
   memset(ram, 1, SRAM_SIZE);
   autosave_lock();
   autosave_unlock();
   slock_lock(io_lock);
   for (i = 0; !writes && i < 5; i++)
      scond_wait_timeout(io_cond, io_lock, 1000000);
   assert(writes == 1);
   slock_unlock(io_lock);

   /* A slow disk must neither stall frames nor observe subsequent SRAM. */
   for (i = 0; i < 100; i++)
   {
      memset(ram, 2, SRAM_SIZE);
      autosave_lock();
      autosave_unlock();
   }
   slock_lock(io_lock);
   hold_write = false;
   scond_signal(io_cond);
   slock_unlock(io_lock);
   wait_requested();
   for (i = 0; i < SRAM_SIZE; i++)
      assert(disk_image[i] == 1);
   capture_and_wait();
   assert(memcmp(disk_image, ram, SRAM_SIZE) == 0);
   before = opens;
   capture_and_wait();
   assert(opens == before);

   /* An unchanged image must be retried after any I/O failure. */
   for (i = 1; i <= 4; i++)
   {
      memset(ram, (int)i + 2, SRAM_SIZE);
      slock_lock(io_lock);
      fail_stage = i;
      before = opens;
      slock_unlock(io_lock);
      capture_and_wait();
      assert(opens == before + 1);
      assert(memcmp(current->buffer, ram, SRAM_SIZE) != 0);
      slock_lock(io_lock);
      fail_stage = 0;
      slock_unlock(io_lock);
      capture_and_wait();
      assert(opens == before + 2);
      assert(memcmp(current->buffer, ram, SRAM_SIZE) == 0);
   }
   /* A partial write also needs repair if SRAM reverts to the last
    * successfully saved image before the next request. */
   memset(ram, 9, SRAM_SIZE);
   slock_lock(io_lock);
   fail_stage = 2;
   before = opens;
   slock_unlock(io_lock);
   capture_and_wait();
   memcpy(ram, current->buffer, SRAM_SIZE);
   slock_lock(io_lock);
   fail_stage = 0;
   slock_unlock(io_lock);
   capture_and_wait();
   assert(opens == before + 2);
   assert(memcmp(disk_image, ram, SRAM_SIZE) == 0);
   stop_save();

   start_save(true);
   memset(ram, 7, SRAM_SIZE);
   capture_and_wait();
   assert(compressed_opens == 1);
   stop_save();
   /* Shutdown is also safe before the first snapshot is published. */
   start_save(false);
   stop_save();
   scond_free(io_cond);
   slock_free(io_lock);
   puts("autosave snapshot tests passed");
   return 0;
}
