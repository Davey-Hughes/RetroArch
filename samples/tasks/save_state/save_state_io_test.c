/* Copyright  (C) 2010-2026 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (save_state_io_test.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Regression oracle for tasks/task_save.c's file I/O.
 *
 * The unit under test is the shipping tasks/task_save.c translation
 * unit, compiled from the tree, driven through the real task queue
 * against a stub frontend.  The core, the settings and the clock are
 * the test's; everything on the I/O path - task_save_handler,
 * task_load_handler, the rastate writer and the rastate block walker,
 * intfstream, the VFS - is production code.
 *
 * Why these properties
 * --------------------
 * The save and load handlers used to transfer exactly one
 * SAVE_STATE_CHUNK per queue tick.  That made throughput a function
 * of the tick rate rather than of the device: SAVE_STATE_CHUNK *
 * 60Hz is about 6 MB/s no matter what the storage can do, so a 16 MB
 * state took 164 ticks - 2.7 seconds at 60Hz - while spending 99.6%
 * of each frame idle (one 100 KB write measures ~62us of a 16667us
 * frame on NVMe).  A tick is now bounded by a wall-clock budget and
 * transfers as many quanta as fit in it.
 *
 * A time budget is only an improvement if it cannot be worse than
 * the fixed count it replaced, so that is asserted directly rather
 * than argued: with a clock that consumes the whole budget on its
 * first observation, the handler must still transfer exactly one
 * quantum per tick and still produce a correct file.  That is the old
 * behaviour, reached through the new code, and it is what a device
 * slow enough to blow the budget on a single write will see.
 *
 * The rest are correctness properties the throughput work must not
 * be allowed to trade away, and three bugs the audit turned up:
 *
 *  - A truncated state file overread the buffer.  The block walker's
 *    loop bound was 'input < stop', so it could enter the body with
 *    one byte left and then read the block length out of input[4..7]
 *    - six bytes past the end.  ASan reports it for any state
 *    truncated to 9..15 bytes.  content_deserialize_state had the
 *    same shape one level up: it reads seven bytes for the magic and
 *    input[7] for the version with no length check at all.
 *    Savestates are shared, synced and recovered off failing media,
 *    and the load handler's own short-read path produces exactly this
 *    buffer, so this is a file-format parser being handed hostile
 *    input.
 *
 *  - A block's declared 32-bit length was never checked against the
 *    bytes actually remaining, and was then passed to core_unserialize
 *    as the size of the buffer at that offset.
 *
 *  - A failed core_serialize left state->data NULL and state->size 0,
 *    at which point every test in the handler read as success
 *    (remaining == 0 == written, written == size) and the task
 *    reported a COMPLETED save.  The file had already been opened and
 *    truncated, so the slot was left holding zero bytes and the user
 *    was told the save succeeded.
 *
 *  - A failed open was a bare return: not errored, not finished.  The
 *    queue re-entered the task on the next tick and it retried the
 *    open forever.  Save tasks are TASK_TYPE_BLOCKING, so that one
 *    task wedged every other blocking task for the rest of the
 *    session.
 *
 * What this test does NOT assert
 * ------------------------------
 * Little about compressed (rzip) states: HAVE_COMPRESSION is off for
 * the default binary, so the uncompressed intfstream path is the one
 * under test.  The RZIP=1 build turns it on, runs every lane through
 * rzip with compression off, and adds the one that needs it (13).
 * Nothing about cheevos or replay blocks, which are compiled out.
 *
 * The clock
 * ---------
 * cpu_features_get_time_usec() is provided here rather than linked
 * from features_cpu.c, and advances a fixed step per observation.
 * That is what makes the budget assertable: step 0 means the budget
 * never expires (one tick for the whole transfer), step >=
 * SAVE_STATE_TICK_BUDGET_US means it expires on first observation
 * (one quantum per tick).  Both are exact, and neither depends on how
 * many times anything else reads the clock, because extra reads can
 * only advance it further.
 *
 * The two lanes
 * -------------
 * The threaded queue is a lane of this binary rather than a separate
 * one.  The pacing protocol is identical with and without
 * HAVE_THREADS - the locks only serialise it - so every tick-count
 * assertion belongs to the default lane, which initialises the queue
 * unthreaded and is therefore exactly reproducible.  What the "conc"
 * lane adds is a target for TSan: under a threaded queue the transfer
 * loop this change rewrote runs on the worker while
 * content_deserialize_state and the undo bookkeeping run on whichever
 * thread calls task_queue_check(), and the question worth asking of a
 * loop that newly carries a deadline across iterations is whether any
 * of it ended up shared.
 *
 * Build:  make            (SANITIZER=address,undefined, or
 *                          SANITIZER=thread, for a checked run)
 * Run:    ./save_state_io_test        default lane
 *         ./save_state_io_test conc   threaded queue
 *         make sweep                  both lanes under all three
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

#include <retro_common_api.h>
#include <compat/strl.h>
#include <queues/message_queue.h>
#include <queues/task_queue.h>
#include <streams/file_stream.h>
#include <file/file_path.h>
#include <lists/dir_list.h>
#include <string/stdstring.h>
#include <vfs/vfs_implementation.h>
#include <retro_timers.h>

#include "../../../content.h"
#include "../../../configuration.h"
#include "../../../runloop.h"
#include "../../../core.h"
#include "../../../msg_hash.h"
#include "../../../save_backups.h"
#include "../../../tasks/tasks_internal.h"
#include "../../../gfx/video_driver.h"

/* Must match tasks/task_save.c.  Duplicated rather than exported: the
 * constants are an implementation detail of the handler and a test
 * that forced them into a header would be asserting against its own
 * mirror.  A drift here shows up as a failing tick-count assertion,
 * which is the intended way to find out. */
#define TEST_SAVE_STATE_CHUNK        (100 * 1024)
#define TEST_TICK_BUDGET_US          2000

static int failures = 0;

static void ok(const char *what)   { printf("[ok]   %s\n", what); }
static void fail(const char *what) { printf("[FAIL] %s\n", what); failures = 1; }

static void okf(int cond, const char *what)
{
   if (cond) ok(what); else fail(what);
}

/* -----------------------------------------------------------------
 * Deterministic clock
 * ----------------------------------------------------------------- */
static uint64_t clock_now  = 1000000;
static uint64_t clock_step = 0;

retro_time_t cpu_features_get_time_usec(void)
{
   /* Atomic: with a task parked, the worker's queue reads the clock
    * while this thread's gather may too. */
   return (retro_time_t)__atomic_fetch_add(&clock_now,
         __atomic_load_n(&clock_step, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
}

#ifdef HAVE_COMPRESSION
/* rzip sizes its chunk compressor's pool from this; the rest of
 * features_cpu.c is not linked, the clock above being the test's */
unsigned cpu_features_get_core_amount(void) { return 2; }
#endif

/* task_save.c's parks, counted on the way into the real queue. */
static unsigned park_calls = 0;
void __real_task_queue_park(retro_task_t *task, retro_time_t horizon_usec);
void __wrap_task_queue_park(retro_task_t *task, retro_time_t horizon_usec)
{
   __atomic_fetch_add(&park_calls, 1, __ATOMIC_RELAXED);
   __real_task_queue_park(task, horizon_usec);
}

/* The other half of not linking features_cpu.c.  intfstream_get_crc()
 * pulls in encoding_crc32.c, which dispatches on this; nothing on the
 * savestate path calls it, and reporting no features keeps the scalar
 * table path if anything ever does. */
uint64_t cpu_features_get(void) { return 0; }

/* -----------------------------------------------------------------
 * Stub core
 * ----------------------------------------------------------------- */
static size_t   core_len       = 0;
static uint8_t *core_mem       = NULL;
static int      core_ser_fails = 0;

static void core_fill(size_t n)
{
   size_t i;
   free(core_mem);
   core_len = n;
   core_mem = n ? (uint8_t*)malloc(n) : NULL;
   for (i = 0; i < n; i++)
      core_mem[i] = (uint8_t)(i * 31u + 7u);
}

size_t core_serialize_size(void) { return core_len; }

/* Counting allocator, for the one property here that cannot be seen
 * any other way.
 *
 * The undo snapshot keeps its allocation between captures instead of
 * freeing and remallocing it.  Pointer identity cannot observe that:
 * a state-sized block goes to mmap, munmap on free hands the region
 * back, and the next request of the same size is usually handed the
 * same address - so a run of captures looks identical whether the
 * buffer was reused or reallocated, while the page faults that make
 * it expensive are only paid in one of them.  Counting the calls is
 * the discriminator.
 *
 * Only state-sized requests are counted, so the frontend's small
 * incidental allocations (task structs, titles, paths) do not drown
 * the signal.  Linked with -Wl,--wrap so this sees the calls made by
 * the unit under test, not just by this file; under a sanitizer
 * __real_malloc resolves to the sanitizer's allocator, which is what
 * we want - the count is of calls, not of pages. */
#define BIG_ALLOC_BYTES (64 * 1024)

static int big_allocs     = 0;
static int alloc_count_on = 0;

extern void *__real_malloc(size_t n);

void *__wrap_malloc(size_t n)
{
   if (alloc_count_on && n >= BIG_ALLOC_BYTES)
      big_allocs++;
   return __real_malloc(n);
}

static void alloc_count_begin(void)
{
   big_allocs     = 0;
   alloc_count_on = 1;
}

static void alloc_count_end(void) { alloc_count_on = 0; }

/* While set, every calloc() of a path-carrying size fails: what a
 * save or load task allocating its result when it finishes would ask
 * for.  The task's own state is allocated when it is pushed, before
 * this is set. */
static int calloc_fail_big = 0;

extern void *__real_calloc(size_t n, size_t size);

void *__wrap_calloc(size_t n, size_t size)
{
   if (calloc_fail_big && size && n >= PATH_MAX_LENGTH / size)
      return NULL;
   return __real_calloc(n, size);
}

static int ser_dest_calls = 0;

static void ser_dest_reset(void) { ser_dest_calls = 0; }
static void ser_dest_note(void *p) { (void)p; ser_dest_calls++; }

static unsigned core_ser_calls;

bool core_serialize(retro_ctx_serialize_info_t *info)
{
   core_ser_calls++;
   if (core_ser_fails || !info || info->size < core_len)
      return false;
   ser_dest_note((void*)info->data);
   memcpy((void*)info->data, core_mem, core_len);
   return true;
}

bool core_unserialize(retro_ctx_serialize_info_t *info)
{
   const uint8_t   *src;
   volatile uint8_t sink = 0;
   size_t           i;

   if (!info)
      return false;
   if (!(src = (const uint8_t*)(info->data_const
               ? info->data_const : info->data)))
      return false;

   /* Read every byte the caller says the buffer holds, before
    * deciding whether the size is one this core likes.  That is what
    * a real core does, and it is the entire hazard in an unchecked
    * block length: task_save.c hands core_unserialize a size taken
    * verbatim from the file.  Touching the bytes here is what makes
    * ASan - rather than this stub's own opinion of the size - the
    * thing that catches a block that declares more than the state
    * contains.  A stub that checked the size first would report the
    * over-long block as refused while the read never happened, and
    * would pass against the unfixed walker. */
   for (i = 0; i < info->size; i++)
      sink ^= src[i];
   (void)sink;

   if (info->size != core_len)
      return false;
   memcpy(core_mem, src, core_len);
   return true;
}

bool core_get_memory(retro_ctx_memory_info_t *info)
{
   if (info) { info->data = NULL; info->size = 0; }
   return false;
}

bool core_info_current_supports_savestate(void) { return true; }

/* -----------------------------------------------------------------
 * Stub frontend
 * ----------------------------------------------------------------- */
static settings_t           stub_settings;
static runloop_state_t      stub_runloop;
static video_driver_state_t stub_video;

settings_t *config_get_ptr(void) { return &stub_settings; }
runloop_state_t *runloop_state_get_ptr(void) { return &stub_runloop; }
video_driver_state_t *video_state_get_ptr(void) { return &stub_video; }
bool video_driver_cached_frame_is_hw_render(void) { return false; }
void *savefile_ptr_get(void) { return NULL; }

/* The flushes asked for before state loads, and what they owed */
static unsigned flushes;
static int flushes_owed;
void event_flush_save_files(bool sram_used, bool compress_files, int owed)
{
   (void)sram_used;
   (void)compress_files;
   flushes++;
   flushes_owed |= owed;
}
/* Save writes that have landed, for cloud sync's copies */
static unsigned save_writes;
void content_save_written(void) { save_writes++; }
bool audio_driver_jump_fade_begin(void) { return false; }
void audio_driver_jump_fade_end(bool ramped) { (void)ramped; }

bool runloop_get_savestate_path(char *path, size_t len, int slot)
{
   snprintf(path, len, "save_state_io_test_auto_%d.state", slot);
   return true;
}

const char *msg_hash_to_str(enum msg_hash_enums msg)
{
   (void)msg;
   return "state message";
}

void RARCH_LOG(const char *fmt, ...)  { (void)fmt; }
void RARCH_WARN(const char *fmt, ...) { (void)fmt; }
static unsigned errors_logged;
void RARCH_ERR(const char *fmt, ...)  { (void)fmt; errors_logged++; }

/* A thumbnail: "<state>.png" holding "new"; none when @shot_fails.
 * @png_at_shot: whether the slot still had one when it was taken. */
static unsigned shots;
static bool     shot_fails;
static bool     png_at_shot;
bool take_screenshot(const char *screenshot_dir, const char *path,
      bool silence, bool has_valid_framebuffer, bool fullpath,
      bool use_thread)
{
   char png[PATH_MAX_LENGTH];
   (void)screenshot_dir; (void)silence; (void)has_valid_framebuffer;
   (void)fullpath; (void)use_thread;
   shots++;
   snprintf(png, sizeof(png), "%s.png", path);
   png_at_shot = path_is_valid(png);
   if (shot_fails)
      return false;
   return filestream_write_file(png, "new", 3);
}

/* The shots asked to stop: the last one's file */
static unsigned shots_cancelled;
static char     shot_cancelled[PATH_MAX_LENGTH];
void take_screenshot_cancel(const char *path)
{
   shots_cancelled++;
   strlcpy(shot_cancelled, path, sizeof(shot_cancelled));
}

static unsigned msgs_pushed;
static void msg_push_stub(retro_task_t *task, const char *msg,
      unsigned prio, unsigned dur, bool flush)
{
   (void)task; (void)prio; (void)dur; (void)flush;
   /* Every finished task pushes, an empty one when it says nothing */
   if (msg && *msg)
      msgs_pushed++;
}

/* Renames refused at the VFS: the .auto's move into its backups, as
 * across drives; a copy landing there; the new state over the .auto;
 * a held name put back. */
#define REFUSE_MOVE   1
#define REFUSE_LAND   2
#define REFUSE_FINISH 4
#define REFUSE_BACK   8
static int      refuse_renames;
static unsigned file_syncs;
/* The next temp move in waits here: 1 armed, 2 waiting, 0 let go */
static int      hold_temp_move;
int __real_retro_vfs_file_rename_impl(const char *old_path,
      const char *new_path);
int __wrap_retro_vfs_file_rename_impl(const char *old_path,
      const char *new_path)
{
   int    refuse = __atomic_load_n(&refuse_renames, __ATOMIC_RELAXED);
   int    armed  = 1;
   size_t _len   = strlen(old_path);
   bool   temp   = _len > 4 && !strcmp(old_path + _len - 4, ".tmp");
   bool   into   = strstr(new_path, "backups") != NULL;
   bool   held   = _len > 5 && !strcmp(old_path + _len - 5, ".prev");
   if (     temp
         && __atomic_compare_exchange_n(&hold_temp_move, &armed, 2, false,
               __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      while (__atomic_load_n(&hold_temp_move, __ATOMIC_ACQUIRE) == 2)
         retro_sleep(1);
   if (     ((refuse & REFUSE_MOVE)   &&  into && !temp)
         || ((refuse & REFUSE_LAND)   &&  into &&  temp)
         || ((refuse & REFUSE_FINISH) && !into &&  temp)
         || ((refuse & REFUSE_BACK)   && !into &&  held))
      return -1;
   return __real_retro_vfs_file_rename_impl(old_path, new_path);
}
int __real_retro_vfs_file_sync_impl(libretro_vfs_implementation_file *stream);
int __wrap_retro_vfs_file_sync_impl(libretro_vfs_implementation_file *stream)
{
   __atomic_fetch_add(&file_syncs, 1, __ATOMIC_RELAXED);
   return __real_retro_vfs_file_sync_impl(stream);
}

static void frontend_reset(void)
{
   memset(&stub_settings, 0, sizeof(stub_settings));
   memset(&stub_runloop,  0, sizeof(stub_runloop));
   memset(&stub_video,    0, sizeof(stub_video));
   stub_video.frame_count      = 1000;   /* past the readiness gate */
   stub_settings.ints.state_slot = 3;
   core_ser_fails              = 0;
   clock_now                   = 1000000;
   clock_step                  = 0;
}

/* -----------------------------------------------------------------
 * Queue driving
 * ----------------------------------------------------------------- */
static bool any_task(retro_task_t *t, void *ud)
{
   (void)t; (void)ud;
   return true;
}

static bool queue_busy(void)
{
   task_finder_data_t fd;
   fd.func     = any_task;
   fd.userdata = NULL;
   return task_queue_find(&fd);
}

/* Ticks the queue until it drains or 'limit' is reached.  Returns the
 * tick count; reaching 'limit' with the queue still busy is itself a
 * finding (a task that never terminates), so callers check for it. */
static int pump(int limit)
{
   int ticks = 0;
   while (ticks < limit)
   {
      task_queue_check();
      ticks++;
      if (!queue_busy())
         break;
   }
   return ticks;
}

static long file_size(const char *p)
{
   RFILE *f = filestream_open(p, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   int64_t n;
   if (!f)
      return -1;
   n = filestream_get_size(f);
   filestream_close(f);
   return (long)n;
}

/* -----------------------------------------------------------------
 * 1. A budget that never expires transfers the whole state in one
 *    tick - the point of the change.
 * ----------------------------------------------------------------- */
static void test_budget_unlimited(void)
{
   const char *path = "sst_budget_full.state";
   size_t sz        = 64 * TEST_SAVE_STATE_CHUNK;   /* 6.25 MB */
   int ticks;

   frontend_reset();
   core_fill(sz);
   filestream_delete(path);
   clock_step = 0;   /* clock never advances: budget never expires */
   save_writes = 0;

   content_save_state(path, true);
   ticks = pump(1000);

   okf(ticks == 1,
       "unexpired budget writes a 64-quantum state in a single tick");
   okf(save_writes == 1, "the state written counts as a save write");
   if (ticks != 1)
      printf("       (took %d ticks)\n", ticks);

   okf(file_size(path) == (long)content_get_serialized_size(),
       "budgeted write produces a full-length file");

   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * 2. A budget that expires on first observation degrades to exactly
 *    one quantum per tick.  This is the no-worst-case-regression
 *    property: it is the behaviour the handler had before the budget
 *    existed, and it is what the slowest supported storage gets.
 * ----------------------------------------------------------------- */
static void test_budget_exhausted(void)
{
   const char *path = "sst_budget_none.state";
   size_t quanta    = 12;
   size_t sz        = quanta * TEST_SAVE_STATE_CHUNK;
   size_t total;
   int    ticks, expect;

   frontend_reset();
   core_fill(sz);
   filestream_delete(path);
   /* One observation spends the entire budget. */
   clock_step = TEST_TICK_BUDGET_US;

   total  = content_get_serialized_size();
   expect = (int)((total + TEST_SAVE_STATE_CHUNK - 1)
                  / TEST_SAVE_STATE_CHUNK);

   content_save_state(path, true);
   ticks = pump(1000);

   okf(ticks == expect,
       "exhausted budget falls back to exactly one quantum per tick");
   if (ticks != expect)
      printf("       (expected %d ticks, took %d)\n", expect, ticks);

   okf(file_size(path) == (long)total,
       "one-quantum-per-tick path still writes the whole file");

   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * 3. Round-trip under both pacings.  Throughput work that corrupts
 *    the payload is worse than the slow path it replaced.
 * ----------------------------------------------------------------- */
static void test_roundtrip(uint64_t step, const char *label)
{
   const char *path = "sst_roundtrip.state";
   size_t sz        = 7 * TEST_SAVE_STATE_CHUNK + 1234; /* unaligned tail */
   uint8_t *expect;

   frontend_reset();
   core_fill(sz);
   expect = (uint8_t*)malloc(sz);
   memcpy(expect, core_mem, sz);
   filestream_delete(path);
   clock_step = step;

   content_save_state(path, true);
   pump(1000);

   /* Refill the core with a different pattern so a load that does
    * nothing at all cannot pass. */
   memset(core_mem, 0xA5, sz);

   content_load_state(path, false, false);
   pump(1000);

   okf(memcmp(expect, core_mem, sz) == 0, label);

   free(expect);
   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * 4. A failed serialize must fail the task and must not leave a
 *    zero-byte file in the slot.
 * ----------------------------------------------------------------- */
static void test_serialize_failure(void)
{
   const char *path = "sst_serfail.state";
   int ticks;

   frontend_reset();
   core_fill(256 * 1024);
   filestream_delete(path);

   /* Defer serialization into the handler, then refuse it there. */
   set_save_state_in_background(true);
   core_ser_fails = 1;

   content_save_state(path, true);
   ticks = pump(1000);

   core_ser_fails = 0;
   set_save_state_in_background(false);

   okf(ticks < 1000, "failed serialize terminates the save task");
   okf(file_size(path) < 0,
       "failed serialize leaves no file (not a zero-byte one)");
   if (file_size(path) == 0)
      printf("       (a zero-byte state file was left in the slot)\n");

   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * A save is written beside its slot and moved in only once whole: a
 * save that fails over an existing state leaves that state as it was,
 * and none leaves its temporary file behind.
 * ----------------------------------------------------------------- */
static bool tmp_exists(const char *path)
{
   char tmp[256];
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   return file_size(tmp) >= 0;
}

/* The slot no longer holds @old: a write into it has begun. */
static bool written_into(const char *path, const void *old, int64_t n_old)
{
   void   *now = NULL;
   int64_t n   = 0;
   bool    changed;
   if (!filestream_read_file(path, &now, &n))
      return true;
   changed = n != n_old || memcmp(now, old, (size_t)n_old);
   free(now);
   return changed;
}

static void test_failed_save_keeps_old(void)
{
   const char *path = "sst_keep.state";
   size_t   sz      = 12 * TEST_SAVE_STATE_CHUNK;
   void    *before  = NULL, *after = NULL;
   int64_t  n_before = 0, n_after = 0;

   frontend_reset();
   core_fill(sz);
   filestream_delete(path);
   content_save_state(path, true);
   pump(1000);
   okf(!tmp_exists(path), "a save leaves nothing beside its slot");
   if (!filestream_read_file(path, &before, &n_before))
      n_before = -1;

   /* A quantum a tick, cancelled two ticks into the write: whatever
    * the save does first (keeping the old state for undo), the write
    * has begun once the slot is open, beside it or in it. */
   memset(core_mem, 0x5A, sz);
   clock_step = TEST_TICK_BUDGET_US;
   content_save_state(path, true);
   {
      int i;
      for (i = 0; i < 1000 && !tmp_exists(path) && !written_into(path, before, n_before); i++)
         task_queue_check();
   }
   task_queue_check();
   task_queue_check();
   task_queue_reset();
   pump(1000);
   clock_step = 0;

   if (!filestream_read_file(path, &after, &n_after))
      n_after = -2;
   okf(n_before > 0 && n_after == n_before && !memcmp(before, after, (size_t)n_before),
       "a save cancelled part-way over a state leaves that state as it was");
   okf(!tmp_exists(path), "a cancelled save leaves nothing beside its slot");

   free(before);
   free(after);
   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * A background save defers only the core's serialize to the handler
 * - that deferral is the whole point of the core's
 * SET_SAVE_STATE_IN_BACKGROUND request - and a foreground save pays
 * it at push. Where the serialize runs is the contract the thread
 * audit holds from the other side: the handler's path reads no live
 * frontend state, so the core call is the only work left in it.
 * ----------------------------------------------------------------- */
static void test_background_serialize_timing(void)
{
   const char *path = "sst_bg_timing.state";
   unsigned before;

   frontend_reset();
   core_fill(128 * 1024);
   filestream_delete(path);

   set_save_state_in_background(true);
   before = core_ser_calls;
   content_save_state(path, true);
   okf(core_ser_calls == before,
       "a background push serializes nothing at push");
   pump(1000);
   okf(core_ser_calls == before + 1,
       "the handler serializes the core exactly once");
   okf(file_size(path) > 0, "and the state landed");
   set_save_state_in_background(false);
   filestream_delete(path);

   before = core_ser_calls;
   content_save_state(path, true);
   okf(core_ser_calls == before + 1,
       "a foreground push serializes at push");
   pump(1000);
   okf(core_ser_calls == before + 1,
       "and the handler only writes what the push made");
   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * 5. A failed open must terminate the task, not retry forever.  Save
 *    tasks are blocking, so a task that never finishes takes the
 *    whole blocking queue with it.
 * ----------------------------------------------------------------- */
static void test_open_failure(void)
{
   /* A path whose parent directory does not exist: open fails and
    * will keep failing, which is the case the old bare-return
    * retried on every tick for the rest of the session. */
   const char *path = "sst_no_such_dir/deeper/still/x.state";
   int ticks;

   frontend_reset();
   core_fill(128 * 1024);

   content_save_state(path, true);
   ticks = pump(500);

   okf(ticks < 500, "failed open terminates the save task");
   if (ticks >= 500)
      printf("       (task still queued after 500 ticks - it is "
             "retrying the open, and it is blocking)\n");
}

/* -----------------------------------------------------------------
 * 6. Truncated state files.  Under ASan an overread aborts the
 *    process, so reaching the end of this function at all is half the
 *    assertion; the other half is that each truncation is rejected
 *    rather than accepted as a valid state.
 * ----------------------------------------------------------------- */
static void test_truncated_state(void)
{
   size_t   sz = 4096;
   size_t   full;
   uint8_t *good;
   size_t   cut;
   int      all_rejected = 1;

   frontend_reset();
   core_fill(sz);

   full = content_get_serialized_size();
   good = (uint8_t*)malloc(full);
   content_serialize_state_rewind(good, full);

   /* 0..16 spans: shorter than the magic, shorter than the version
    * byte, and the 9..15 range where the walker used to read a block
    * length that was not there. */
   for (cut = 0; cut <= 16 && cut < full; cut++)
   {
      uint8_t *slice = (uint8_t*)malloc(cut ? cut : 1);
      if (cut)
         memcpy(slice, good, cut);
      if (content_deserialize_state(slice, cut))
      {
         printf("       (a %zu-byte truncation was accepted)\n", cut);
         all_rejected = 0;
      }
      free(slice);
   }

   okf(all_rejected,
       "state files truncated to 0..16 bytes are rejected, not parsed");

   /* A well-formed header whose first block claims far more bytes
    * than the buffer holds.  Rejection must come from the extent
    * check, not from the core happening to dislike the size. */
   {
      uint8_t *evil = (uint8_t*)malloc(full);
      memcpy(evil, good, full);
      evil[8 + 4] = 0xFF;
      evil[8 + 5] = 0xFF;
      evil[8 + 6] = 0xFF;
      evil[8 + 7] = 0x0F;              /* ~256 MB in a 4 KB buffer */
      okf(!content_deserialize_state(evil, full),
          "a block longer than the buffer is refused");
      free(evil);
   }

   /* A length that overflows when aligned up to 8 on a 32-bit
    * size_t.  Must be refused by the extent check before the
    * alignment arithmetic runs. */
   {
      uint8_t *evil = (uint8_t*)malloc(full);
      memcpy(evil, good, full);
      evil[8 + 4] = 0xFF;
      evil[8 + 5] = 0xFF;
      evil[8 + 6] = 0xFF;
      evil[8 + 7] = 0xFF;              /* UINT32_MAX */
      okf(!content_deserialize_state(evil, full),
          "a block length of UINT32_MAX is refused before alignment");
      free(evil);
   }

   free(good);
}

/* -----------------------------------------------------------------
 * 7. A state file that is shorter than its own contents claim must
 *    fail the load, not half-apply it.
 * ----------------------------------------------------------------- */
static void test_short_file(void)
{
   const char *path = "sst_short.state";
   size_t sz        = 3 * TEST_SAVE_STATE_CHUNK;
   uint8_t *expect;
   RFILE *f;
   int64_t full;
   uint8_t *raw;

   frontend_reset();
   core_fill(sz);
   expect = (uint8_t*)malloc(sz);
   memcpy(expect, core_mem, sz);
   filestream_delete(path);

   content_save_state(path, true);
   pump(1000);

   /* Rewrite the file with its last quantum missing. */
   full = file_size(path);
   raw  = (uint8_t*)malloc((size_t)full);
   f    = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   filestream_read(f, raw, full);
   filestream_close(f);
   filestream_delete(path);
   f = filestream_open(path, RETRO_VFS_FILE_ACCESS_WRITE,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   filestream_write(f, raw, full - TEST_SAVE_STATE_CHUNK);
   filestream_close(f);
   free(raw);

   memset(core_mem, 0x5A, sz);

   content_load_state(path, false, false);
   pump(1000);

   {
      size_t i;
      int untouched = 1;
      for (i = 0; i < sz; i++)
         if (core_mem[i] != 0x5A) { untouched = 0; break; }
      okf(untouched,
          "a truncated state file is refused, not partially applied");
   }

   free(expect);
   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * 8. Mutual exclusion between save/load tasks.
 *
 *    Every task this file pushes is TASK_TYPE_BLOCKING, so the queue
 *    admits at most one of them at a time.  The rest are held behind
 *    it, in order, up to a bound, and refused past it.  Two of the
 *    fixes above rest on that rule and one of them is only severe
 *    because of it - a save task that retried a failed open forever
 *    did not merely fail to save, it held the single blocking slot for
 *    the rest of the session, so no state could be saved or loaded
 *    again without a restart.
 *
 *    The refusal path is also where task_push_save_state has to undo
 *    everything it built - the task, its title, the state struct, and
 *    the caller's serialized buffer, which it took ownership of.
 *    LeakSan is the assertion for that half; reaching the end of this
 *    function with the queue drained is the assertion for the rest.
 * ----------------------------------------------------------------- */
static void test_blocking_exclusion(void)
{
   const char *first  = "sst_excl_a.state";
   const char *second = "sst_excl_b.state";
   int         i, accepted;

   frontend_reset();
   core_fill(40 * TEST_SAVE_STATE_CHUNK);
   filestream_delete(first);
   filestream_delete(second);

   /* One observation spends the budget, so the first save needs ~40
    * ticks and is still in flight for the whole of this test. */
   clock_step = TEST_TICK_BUDGET_US;

   content_save_state(first, true);
   task_queue_check();                 /* first save now running */

   okf(queue_busy(), "the first blocking task is running");

   /* Push several more while it runs.  The first few are held behind
    * it, the rest refused, and each refusal must free everything it
    * allocated. */
   for (i = 0, accepted = 0; i < 8; i++)
   {
      if (content_save_state(second, true))
         accepted++;
      task_queue_check();
   }

   okf(accepted > 0 && accepted < 8,
       "saves behind one in flight are held up to a bound, then refused");
   okf(file_size(second) == -1,
       "a held save waits for the one in flight");

   /* A load refused the same way leaves no load pending: left set, the
    * flag held a content close and a movie recording waiting forever on
    * a load that never comes. LeakSan covers what the refusal frees. */
   {
      const char *third = "sst_excl_c.state";
      FILE       *f     = fopen(third, "wb");
      bool        ok;
      if (f)
      {
         fputs("not a state, never read", f);
         fclose(f);
      }
      ok = content_load_state(third, false, false);
      okf(!ok, "a load is refused while the hold is full");
      okf(!content_load_state_in_progress(NULL),
          "the refused load is not left pending");
      filestream_delete(third);
   }

   pump(1000);

   okf(!queue_busy(), "the queue drains after the refusals");
   okf(file_size(first) == (long)content_get_serialized_size(),
       "the admitted save completes intact despite the refusals");
   okf(file_size(second) == (long)content_get_serialized_size(),
       "the held saves run once the first is through");

   filestream_delete(first);
   filestream_delete(second);
}

/* -----------------------------------------------------------------
 * The threaded lane.
 *
 * Under HAVE_THREADS the queue runs handlers on its worker thread and
 * retires them - callbacks included - on whichever thread calls
 * task_queue_check().  So the budgeted transfer loop this change
 * rewrote executes off the main thread in production, while
 * content_deserialize_state and the undo bookkeeping stay on it.
 * That split is what TSan is pointed at here: the loop now reads a
 * clock and carries a deadline across iterations, and the question is
 * whether any of that ended up shared.
 *
 * It is deliberately one task at a time.  That is not a simplification
 * - it is the shape the queue enforces, per test_blocking_exclusion
 * above, and a lane that pushed concurrent save tasks would be
 * asserting against a configuration the frontend cannot produce.
 * What runs concurrently here is the handler against the main thread's
 * gather, pump and verification, which is exactly production.
 *
 * The fake clock is left at step 0 for this lane: it is read only by
 * the handler, i.e. only from the worker, so it stays a
 * single-threaded object even though the process is not.  A lane that
 * made the budget expire would have the main thread's frontend_reset
 * writing what the worker reads, and TSan would be reporting the
 * test's own scaffolding rather than the unit.
 * ----------------------------------------------------------------- */
static void pump_threaded(void)
{
   int spins = 0;
   /* The worker owns progress; this thread only gathers.  Bounded so
    * a handler that never terminates fails the run instead of hanging
    * CI. */
   while (spins++ < 20000)
   {
      task_queue_check();
      if (!queue_busy())
         return;
      retro_sleep(1);
   }
   fail("threaded queue drained within the spin bound");
}

static void test_threaded_roundtrip(void)
{
   const char *path = "sst_threaded.state";
   size_t sz        = 9 * TEST_SAVE_STATE_CHUNK + 77;
   uint8_t *expect;

   frontend_reset();
   core_fill(sz);
   expect = (uint8_t*)malloc(sz);
   memcpy(expect, core_mem, sz);
   filestream_delete(path);

   content_save_state(path, true);
   pump_threaded();

   okf(file_size(path) == (long)content_get_serialized_size(),
       "threaded queue: the save task writes a full-length file");

   memset(core_mem, 0xA5, sz);

   content_load_state(path, false, false);
   pump_threaded();

   okf(memcmp(expect, core_mem, sz) == 0,
       "threaded queue: round-trip is byte-exact");

   free(expect);
   filestream_delete(path);
}

/* A load pushed before the core's second frame waits for it parked, a
 * look about once a frame, and loads once the frame is in.  The clock
 * advances here so a park's horizon can pass. */
static void test_threaded_load_waits_for_frames(void)
{
   const char *path = "sst_gate.state";
   size_t sz        = 3 * TEST_SAVE_STATE_CHUNK + 5;
   uint8_t *expect;
   unsigned parks;
   int i;

   frontend_reset();
   core_fill(sz);
   expect = (uint8_t*)malloc(sz);
   memcpy(expect, core_mem, sz);
   filestream_delete(path);

   content_save_state(path, true);
   pump_threaded();
   memset(core_mem, 0xA5, sz);

   stub_video.frame_count = 0;
   __atomic_store_n(&clock_step, 2000, __ATOMIC_RELAXED);
   __atomic_store_n(&park_calls, 0, __ATOMIC_RELAXED);
   content_load_state(path, false, false);
   for (i = 0; i < 50; i++)
   {
      task_queue_check();
      retro_sleep(1);
   }
   parks = __atomic_load_n(&park_calls, __ATOMIC_RELAXED);
   okf(parks >= 1,
       "threaded queue: a load before the second frame parks");
   okf(parks <= 50,
       "threaded queue: it looks about once a frame while it waits");
   okf(memcmp(expect, core_mem, sz) != 0,
       "threaded queue: nothing is loaded before the second frame");

   /* With the clock stopped a parked task cannot come due, so once a
    * pass in flight has settled the handler is not looking; a gather
    * orders its last look before the write and the write before its
    * next one. */
   __atomic_store_n(&clock_step, 0, __ATOMIC_RELAXED);
   for (i = 0; i < 20; i++)
   {
      task_queue_check();
      retro_sleep(1);
   }
   stub_video.frame_count = 2;
   task_queue_check();
   __atomic_store_n(&clock_step, 2000, __ATOMIC_RELAXED);
   pump_threaded();
   okf(memcmp(expect, core_mem, sz) == 0,
       "threaded queue: the state loads once the second frame is in");

   __atomic_store_n(&clock_step, 0, __ATOMIC_RELAXED);
   free(expect);
   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * 9. The undo snapshot reuses its allocation.
 *
 *    content_load_state_cb retakes the undo snapshot on every single
 *    state load, so this is the hottest serialize the frontend does,
 *    and it used to free its destination and malloc a new one each
 *    time.  A state-sized allocation is served by mmap, so the free
 *    hands the pages back to the kernel and the next serialize faults
 *    every one of them in on first touch - measured at 63% of the
 *    cost of a 16 MiB serialize, more than the copy it exists to
 *    hold.
 *
 *    The snapshot now keeps the outgoing allocation as a spare and
 *    swaps the two, so a run of snapshots at a fixed state size must
 *    settle onto exactly two buffers.  Two rather than one because it
 *    is a swap, which is what preserves the failure semantics tested
 *    below; a third would mean something is still reallocating.
 * ----------------------------------------------------------------- */
static void test_undo_snapshot_reuses(void)
{
   int i, taken = 0;

   frontend_reset();
   core_fill(8 * TEST_SAVE_STATE_CHUNK);
   content_reset_savestate_backups();
   ser_dest_reset();

   alloc_count_begin();
   for (i = 0; i < 8; i++)
      if (content_save_state("RAM", false))
         taken++;
   alloc_count_end();

   okf(taken == 8, "eight consecutive undo snapshots all succeed");
   okf(ser_dest_calls == 8, "eight snapshots ran eight serializes");

   /* Two, not one: it is a swap, and the second capture has to build
    * its snapshot somewhere while the first is still the live one.
    * From the third onwards the two allocations ping-pong and nothing
    * further is requested.  Reallocating per capture - what this
    * replaced - measures eight. */
   okf(big_allocs == 2,
       "eight snapshots allocate two state-sized buffers, not eight");
   if (big_allocs != 2)
      printf("       (%d state-sized allocations for 8 snapshots)\n",
            big_allocs);

   content_reset_savestate_backups();
}

/* -----------------------------------------------------------------
 * 10. The reused buffer still grows, and a grown snapshot is correct.
 *
 *     The serialized length is not fixed - it tracks the core's state
 *     size, which changes when content changes.  A reused buffer that
 *     failed to grow would be an overflow, and one that grew but kept
 *     a stale length would be a truncated snapshot.  Undoing back to
 *     each size is what checks both.
 * ----------------------------------------------------------------- */
static void test_undo_snapshot_grows(void)
{
   size_t   small = 3 * TEST_SAVE_STATE_CHUNK;
   size_t   big   = 11 * TEST_SAVE_STATE_CHUNK;
   uint8_t *expect;

   frontend_reset();
   content_reset_savestate_backups();

   /* Small state, snapshot it, then grow the core and snapshot again:
    * the second snapshot must not be served out of the first, smaller
    * allocation. */
   core_fill(small);
   okf(content_save_state("RAM", false), "snapshot at the smaller size");

   core_fill(big);
   expect = (uint8_t*)malloc(big);
   memcpy(expect, core_mem, big);
   okf(content_save_state("RAM", false), "snapshot after the state grew");

   memset(core_mem, 0x3C, big);
   okf(content_undo_load_state(), "undo of the grown snapshot succeeds");
   okf(memcmp(expect, core_mem, big) == 0,
       "a grown snapshot restores byte-exactly");

   free(expect);
   content_reset_savestate_backups();
}

/* -----------------------------------------------------------------
 * 11. A failed serialize leaves the previous snapshot intact.
 *
 *     This is the property the swap exists to preserve, and the one
 *     that an in-place rewrite of the live snapshot would have
 *     silently broken: serializing straight over the buffer would
 *     leave a half-written snapshot that still looks valid, so an
 *     undo after a failed capture would restore garbage.  Filling the
 *     spare and only then swapping means the live snapshot is
 *     untouched until a full one is ready to replace it.
 * ----------------------------------------------------------------- */
static void test_undo_snapshot_failure_keeps_previous(void)
{
   size_t   sz = 5 * TEST_SAVE_STATE_CHUNK;
   uint8_t *expect;

   frontend_reset();
   core_fill(sz);
   content_reset_savestate_backups();

   expect = (uint8_t*)malloc(sz);
   memcpy(expect, core_mem, sz);

   okf(content_save_state("RAM", false), "a good snapshot is taken");

   /* Move the core on, then fail the next capture. */
   memset(core_mem, 0x77, sz);
   core_ser_fails = 1;
   okf(!content_save_state("RAM", false),
       "a failed serialize reports failure");
   core_ser_fails = 0;

   /* The snapshot that survives must be the good one, not a partial
    * overwrite of it. */
   memset(core_mem, 0x11, sz);
   okf(content_undo_load_state(),
       "the previous snapshot is still undoable after a failure");
   okf(memcmp(expect, core_mem, sz) == 0,
       "a failed capture leaves the previous snapshot byte-exact");

   free(expect);
   content_reset_savestate_backups();
}

/* -----------------------------------------------------------------
 * 12. Undo allocates nothing in steady state.
 *
 *     content_undo_load_state has to hold on to the snapshot it is
 *     restoring while it captures the current state over the top of
 *     it, and it used to do that by malloc'ing a full-size temporary
 *     and copying the whole state into it - a second state-sized
 *     allocation and a second full copy, on top of the capture's own.
 *     It now detaches the snapshot's allocation instead, and hands it
 *     to the spare afterwards.
 *
 *     So a run of undos, each of which also takes a capture, must
 *     allocate nothing at all once the two buffers exist: the
 *     snapshot being restored becomes the next spare, and the spare
 *     becomes the next snapshot.  Before the change each undo cost
 *     one temporary, and before the capture change it cost a second.
 * ----------------------------------------------------------------- */
static void test_undo_allocates_nothing(void)
{
   size_t   sz = 6 * TEST_SAVE_STATE_CHUNK;
   uint8_t *first;
   int      i, undone = 0;

   frontend_reset();
   core_fill(sz);
   content_reset_savestate_backups();

   first = (uint8_t*)malloc(sz);
   memcpy(first, core_mem, sz);

   /* Reach steady state first: one capture, then one undo.  The undo
    * is what leaves both allocations in play - the snapshot it built
    * and the one it detached and handed to the spare - so only the
    * undos after it are measuring a system that has stopped growing.
    * The core alternates between the two states from here on. */
   content_save_state("RAM", false);
   memset(core_mem, 0x21, sz);
   content_undo_load_state();

   alloc_count_begin();
   for (i = 0; i < 6; i++)
      if (content_undo_load_state())
         undone++;
   alloc_count_end();

   okf(undone == 6, "six consecutive undos all succeed");
   okf(big_allocs == 0,
       "undo allocates no state-sized buffer in steady state");
   if (big_allocs != 0)
      printf("       (%d state-sized allocations for 6 undos)\n",
            big_allocs);

   /* An even number of undos of a two-state alternation lands back
    * where it started, which is what proves the buffers were actually
    * swapped and not merely not-allocated. */
   okf(memcmp(first, core_mem, sz) == 0,
       "an even number of undos returns the original state");

   free(first);
   content_reset_savestate_backups();
}

/* -----------------------------------------------------------------
 * Closing content while a save is in flight.
 *
 * runloop_event_deinit_core() unloads the core's dylib, and the save
 * and load handlers call into it - retro_serialize via
 * content_get_serialized_data, retro_unserialize via
 * core_unserialize.  A worker still inside one of those when the
 * library goes away dispatches into freed code.  That is why closing
 * content calls content_wait_for_save_state_task(), and why that
 * wait cannot simply be deleted the way the other blocking waits in
 * this series were.
 *
 * What this pins is the property the unload depends on: the wait
 * does not return until the save has actually finished.  If it ever
 * returned early - on a budget boundary, say - the unload would land
 * while a worker was still inside the core.
 *
 * The file is checked incomplete before the wait and complete after,
 * so a wait that silently became a no-op fails here rather than
 * turning into a crash on a device.
 * ----------------------------------------------------------------- */
static void test_close_waits_for_save(void)
{
   const char *path = "sst_close_wait.state";
   size_t sz        = 64 * TEST_SAVE_STATE_CHUNK;   /* 6.25 MB */
   long   before;

   frontend_reset();
   core_fill(sz);
   filestream_delete(path);
   /* One quantum per tick: the slow-storage case, where a close is
    * long enough for the user to feel it. */
   clock_step = TEST_TICK_BUDGET_US;

   content_save_state(path, true);

   before = file_size(path);
   okf(before != (long)content_get_serialized_size(),
       "the save is genuinely unfinished when the close begins");

   /* Exactly what closing content does. */
   content_wait_for_save_state_task();

   okf(file_size(path) == (long)content_get_serialized_size(),
       "closing content waits for the save to finish before "
       "the core could be unloaded");

   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * A caller of content_save_state_notify / content_load_state_notify
 * is told how its task ended even when nothing more can be
 * allocated by then: a finishing task hands its own state to the
 * callback rather than allocating a copy that may fail.
 * ----------------------------------------------------------------- */
static unsigned told_calls;
static int      told_error;
static char     told_path[PATH_MAX_LENGTH];

static void told_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   (void)task; (void)user_data;
   told_calls++;
   told_error = error != NULL;
   strlcpy(told_path, task_data ? (const char*)task_data : "",
         sizeof(told_path));
}

static void test_notify_without_allocation(void)
{
   const char *path = "sst_notify.state";

   frontend_reset();
   core_fill(3 * TEST_SAVE_STATE_CHUNK);
   filestream_delete(path);
   clock_step = 0;

   told_calls = 0;
   okf(content_save_state_notify(path, true, told_cb, NULL),
         "notified save is pushed");
   calloc_fail_big = 1;
   pump(1000);
   calloc_fail_big = 0;
   okf(told_calls == 1 && !told_error && !strcmp(told_path, path),
         "notified save is told it was written, allocation failing");

   told_calls = 0;
   okf(content_load_state_notify(path, false, false, told_cb, NULL),
         "notified load is pushed");
   calloc_fail_big = 1;
   pump(1000);
   calloc_fail_big = 0;
   okf(told_calls == 1 && !told_error && !strcmp(told_path, path),
         "notified load is told it was applied, allocation failing");

   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * Load State pressed while a save is still writing.
 *
 *    It was turned away by the queue and dropped without a word: the
 *    hotkey did nothing. It is held behind the save now and runs when
 *    the save is through - after it, so it loads what the save wrote,
 *    not the file the save replaced.
 * ----------------------------------------------------------------- */
static void test_load_held_behind_save(void)
{
   const char *path = "sst_held.state";
   size_t      n    = 40 * TEST_SAVE_STATE_CHUNK;

   frontend_reset();
   core_fill(n);
   filestream_delete(path);
   clock_step = 0;

   /* an older state in the file, so the next save backs it up first */
   content_save_state(path, true);
   pump(1000);
   okf(file_size(path) > 0, "held: the first save is written");

   /* the save in flight for a while: one observation spends the budget */
   clock_step = TEST_TICK_BUDGET_US;
   memset(core_mem, 0x11, n);
   okf(content_save_state(path, true), "held: the second save is pushed");
   task_queue_check();
   /* its backup read of the older state first, then the write */
   okf(queue_busy(), "held: the second save is in flight");

   memset(core_mem, 0x22, n);
   told_calls = 0;
   okf(content_load_state_notify(path, false, false, told_cb, NULL),
       "held: Load State during the save is taken, not refused");
   okf(content_load_state_in_progress(NULL),
       "held: and counts as pending, for a close to wait on");

   pump(20000);
   okf(!queue_busy() && !content_load_state_in_progress(NULL)
         && !content_save_state_in_progress(NULL),
       "held: the save and the load both finish");
   okf(told_calls == 1 && !told_error,
       "held: the load is told it was applied");
   okf(core_mem[0] == 0x11 && core_mem[n - 1] == 0x11,
       "held: the load runs after the save and loads what it wrote");

   clock_step = 0;
   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * Undo Save reverts only the last save, in its own slot: a save to
 * another slot leaves nothing to undo.
 * ----------------------------------------------------------------- */
static void test_undo_save_slot(void)
{
   const char *one = "sst_undo_one.state";
   const char *two = "sst_undo_two.state";
   void   *was = NULL, *now = NULL;
   int64_t n_was = -1, n_now = -2;

   frontend_reset();
   content_reset_savestate_backups();
   filestream_delete(one);
   filestream_delete(two);
   core_fill(2 * TEST_SAVE_STATE_CHUNK + 7);
   content_save_state(one, true);
   pump(1000);
   memset(core_mem, 0x33, core_len);
   content_save_state(one, true);
   pump(1000);
   okf(!content_undo_save_buf_is_empty(),
       "undo slot: a save over slot one can be undone");
   if (!filestream_read_file(one, &was, &n_was))
      n_was = -1;

   memset(core_mem, 0x44, core_len);
   content_save_state(two, true);
   pump(1000);
   okf(content_undo_save_buf_is_empty(),
       "undo slot: after a save to slot two, nothing to undo");
   okf(!content_undo_save_state(), "undo slot: the undo is refused");
   pump(1000);
   if (!filestream_read_file(one, &now, &n_now))
      n_now = -2;
   okf(n_was > 0 && n_now == n_was && !memcmp(was, now, (size_t)n_was),
       "undo slot: slot one keeps the save made over it");

   free(was);
   free(now);
   filestream_delete(one);
   filestream_delete(two);
}

/* -----------------------------------------------------------------
 * An Undo Save turned away (the hold behind a state task is full)
 * keeps its buffer: twice, then the queue drains, under ASan, and the
 * retry writes that buffer. Loads fill the hold: a save's backup read
 * would turn the undo away before the hold, and replace the buffer.
 * ----------------------------------------------------------------- */
static void test_undo_save_refused_push(void)
{
   const char *path = "sst_undo_refused.state";
   void   *was      = NULL;
   int64_t n_was    = -1;
   unsigned i;

   frontend_reset();
   content_reset_savestate_backups();
   filestream_delete(path);
   core_fill(40 * TEST_SAVE_STATE_CHUNK);
   content_save_state(path, true);
   pump(1000);
   if (!filestream_read_file(path, &was, &n_was))
      n_was = -1;
   memset(core_mem, 0x55, core_len);
   content_save_state(path, true);
   pump(1000);

   /* one load in flight, four held behind it */
   clock_step = TEST_TICK_BUDGET_US;
   for (i = 0; i < 5; i++)
   {
      content_load_state(path, false, false);
      task_queue_check();
   }
   okf(!content_undo_save_state(), "undo refused: turned away once");
   okf(!content_undo_save_state(), "undo refused: turned away twice");
   okf(!content_undo_save_buf_is_empty(),
       "undo refused: the undo is still there");
   pump(100000);
   okf(!queue_busy(), "undo refused: the queue drains");
   okf(content_undo_save_state(), "undo refused: taken once there is room");
   pump(1000);
   okf(n_was > 0 && !written_into(path, was, n_was),
       "undo refused: the retry writes the buffer it kept");
   clock_step = 0;
   free(was);
   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * Undo Save pressed while the save it undoes still writes waits
 * behind it, and the slot ends with the file from before that save.
 * ----------------------------------------------------------------- */
static void test_undo_save_during_write(void)
{
   const char *path = "sst_undo_write.state";
   void   *before   = NULL;
   int64_t n_before = -1;
   int i;

   frontend_reset();
   content_reset_savestate_backups();
   filestream_delete(path);
   core_fill(40 * TEST_SAVE_STATE_CHUNK);
   content_save_state(path, true);
   pump(1000);
   if (!filestream_read_file(path, &before, &n_before))
      n_before = -1;

   /* the save over it past its backup read, still writing */
   clock_step = TEST_TICK_BUDGET_US;
   memset(core_mem, 0x99, core_len);
   content_save_state(path, true);
   for (i = 0; i < 1000 && content_load_state_in_progress(NULL); i++)
      task_queue_check();
   okf(!content_load_state_in_progress(NULL)
         && content_save_state_in_progress(NULL),
       "undo during write: the save is writing");
   okf(content_undo_save_state(), "undo during write: Undo Save is taken");
   pump(1000);
   okf(!queue_busy(), "undo during write: the queue drains");
   okf(n_before > 0 && !written_into(path, before, n_before),
       "undo during write: the slot holds the state from before the save");
   okf(content_undo_save_buf_is_empty(),
       "undo during write: nothing is left to undo");
   clock_step = 0;
   free(before);
   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * A backup read counts against Undo Save until it is through: also
 * one the reset discards while held, and one pushed through
 * content_load_state().
 * ----------------------------------------------------------------- */
static void test_undo_save_count(void)
{
   const char *path = "sst_undo_count.state";
   void   *before   = NULL;
   int64_t n_before = -1;

   frontend_reset();
   content_reset_savestate_backups();
   filestream_delete(path);
   core_fill(40 * TEST_SAVE_STATE_CHUNK);
   content_save_state(path, true);
   pump(1000);

   /* a save's backup read held behind a load, then discarded */
   clock_step = TEST_TICK_BUDGET_US;
   content_load_state(path, false, false);
   task_queue_check();
   memset(core_mem, 0xAA, core_len);
   content_save_state(path, true);
   okf(content_undo_save_buf_in_use(), "undo count: the backup read is held");
   content_reset_savestate_backups();
   pump(1000);
   clock_step = 0;
   okf(!content_undo_save_buf_in_use(),
       "undo count: the discarded one is let go");

   if (!filestream_read_file(path, &before, &n_before))
      n_before = -1;
   memset(core_mem, 0xBB, core_len);
   content_save_state(path, true);
   pump(1000);
   okf(content_undo_save_state(), "undo count: Undo Save is taken after it");
   pump(1000);
   okf(n_before > 0 && !written_into(path, before, n_before),
       "undo count: and puts back the state from before the save");

   content_load_state(path, true, false);
   okf(content_undo_save_buf_in_use(),
       "undo count: a backup read through content_load_state() counts");
   pump(1000);
   okf(!content_undo_save_buf_in_use(),
       "undo count: and is let go once read");
   free(before);
   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * Undo Save pressed while a task that frees or replaces its buffer
 * runs is turned away. Held behind the task, it wrote its buffer into
 * the slot after the task had freed it: here the first of two undos
 * in a row, below the backup read of a save over the slot.
 * ----------------------------------------------------------------- */
static void test_undo_save_twice(void)
{
   const char *path = "sst_undo_twice.state";
   void   *was      = NULL;
   int64_t n_was    = -1;

   frontend_reset();
   content_reset_savestate_backups();
   filestream_delete(path);
   core_fill(40 * TEST_SAVE_STATE_CHUNK);
   content_save_state(path, true);
   pump(1000);
   if (!filestream_read_file(path, &was, &n_was))
      n_was = -1;
   memset(core_mem, 0x66, core_len);
   content_save_state(path, true);
   pump(1000);

   /* the first undo still writing */
   clock_step = TEST_TICK_BUDGET_US;
   okf(content_undo_save_state(), "undo twice: the first is pushed");
   task_queue_check();
   okf(!content_undo_save_state(),
       "undo twice: the second, while the first writes, is turned away");
   pump(1000);
   okf(!queue_busy(), "undo twice: the queue drains");
   okf(n_was > 0 && !written_into(path, was, n_was),
       "undo twice: the slot holds the state from before the save");
   okf(!content_undo_save_state(),
       "undo twice: a retry finds the save already undone");
   pump(1000);
   okf(n_was > 0 && !written_into(path, was, n_was),
       "undo twice: and the slot still holds it");
   clock_step = 0;
   free(was);
   filestream_delete(path);
}

static void test_undo_save_during_backup(void)
{
   const char *path  = "sst_undo_backup.state";
   const char *ref   = "sst_undo_backup_ref.state";
   void   *before    = NULL, *last = NULL;
   int64_t n_before  = -1, n_last = -2;

   frontend_reset();
   content_reset_savestate_backups();
   filestream_delete(path);
   filestream_delete(ref);
   core_fill(40 * TEST_SAVE_STATE_CHUNK);
   content_save_state(path, true);
   pump(1000);
   /* what the save in flight below writes */
   memset(core_mem, 0x88, core_len);
   content_save_state(ref, true);
   pump(1000);
   if (!filestream_read_file(ref, &last, &n_last))
      n_last = -2;
   memset(core_mem, 0x77, core_len);
   content_save_state(path, true);
   pump(1000);
   if (!filestream_read_file(path, &before, &n_before))
      n_before = -1;

   /* the next save's backup read of the slot still running */
   clock_step = TEST_TICK_BUDGET_US;
   memset(core_mem, 0x88, core_len);
   content_save_state(path, true);
   task_queue_check();
   okf(content_load_state_in_progress(NULL),
       "undo during save: the backup read is in flight");
   okf(!content_undo_save_state(),
       "undo during save: Undo Save is turned away");
   pump(1000);
   okf(!queue_busy(), "undo during save: the queue drains");
   okf(n_last > 0 && !written_into(path, last, n_last),
       "undo during save: the slot holds the save");
   okf(content_undo_save_state(), "undo during save: a retry is taken");
   pump(1000);
   okf(n_before > 0 && !written_into(path, before, n_before),
       "undo during save: and puts back the state from before it");
   clock_step = 0;
   free(before);
   free(last);
   filestream_delete(path);
   filestream_delete(ref);
}

/* -----------------------------------------------------------------
 * A save over a slot that backs nothing up (its backup read fails, or
 * the core turned undo off) leaves nothing to undo: the buffer holds
 * the file from before an earlier save, an older state.
 * ----------------------------------------------------------------- */
static void test_undo_save_not_backed_up(void)
{
   const char *path = "sst_undo_stale.state";
   int k;

   for (k = 0; k < 2; k++)
   {
      void   *last   = NULL;
      int64_t n_last = -1;

      frontend_reset();
      content_reset_savestate_backups();
      filestream_delete(path);
      core_fill(4096);
      content_save_state(path, true);
      pump(1000);
      memset(core_mem, 0x33, core_len);
      content_save_state(path, true);
      pump(1000);

      memset(core_mem, 0x44, core_len);
      if (k)
         set_save_state_disable_undo(true);
      content_save_state(path, true);
      /* the backup read finds no file */
      if (!k)
         filestream_delete(path);
      pump(1000);
      set_save_state_disable_undo(false);
      if (!filestream_read_file(path, &last, &n_last))
         n_last = -1;

      okf(content_undo_save_buf_is_empty(),
          k ? "undo stale: after a save with undo off, nothing to undo"
            : "undo stale: after a failed backup read, nothing to undo");
      okf(!content_undo_save_state(),
          k ? "undo stale: undo off, the undo is refused"
            : "undo stale: failed read, the undo is refused");
      pump(1000);
      okf(n_last > 0 && !written_into(path, last, n_last),
          k ? "undo stale: undo off, the slot keeps the last save"
            : "undo stale: failed read, the slot keeps the last save");
      free(last);
   }
   filestream_delete(path);
}

#ifdef HAVE_COMPRESSION
/* -----------------------------------------------------------------
 * 13. Undo puts back the file a save replaced, as it was.
 *
 *     A save over a slot first keeps the file there, for undo. That
 *     backup used to be read through rzip - the old state inflated -
 *     and an undo wrote it back through rzip again, deflated afresh
 *     under whatever the compression setting had since become. It is
 *     now the file's own bytes, read and written back as they are:
 *     nothing inflated on every save over a slot, nothing deflated on
 *     undo, and the slot comes back as it was. The setting is turned
 *     between the save and the one over it, both ways, so a backup
 *     that went through rzip would come back in the other form.
 * ----------------------------------------------------------------- */
static void test_undo_save_restores_file(void)
{
   const char *path = "sst_undo_file.state";
   int z;

   for (z = 0; z < 2; z++)
   {
      void   *was = NULL, *over = NULL, *back = NULL;
      int64_t n_was = -1, n_over = -2, n_back = -3;

      frontend_reset();
      filestream_delete(path);
      core_fill(3 * TEST_SAVE_STATE_CHUNK + 123);
      stub_settings.bools.savestate_file_compression = z;
      content_save_state(path, true);
      pump(1000);
      if (!filestream_read_file(path, &was, &n_was))
         n_was = -1;

      stub_settings.bools.savestate_file_compression = !z;
      memset(core_mem, 0x5A, core_len);
      content_save_state(path, true);
      pump(1000);
      if (!filestream_read_file(path, &over, &n_over))
         n_over = -2;
      okf(n_over > 0 && (n_over != n_was || memcmp(was, over, (size_t)n_was)),
          z ? "a plain save over a compressed slot replaces it"
            : "a compressed save over a plain slot replaces it");

      okf(content_undo_save_state(), "the undo of that save is pushed");
      pump(1000);
      if (!filestream_read_file(path, &back, &n_back))
         n_back = -3;
      okf(n_was > 0 && n_back == n_was && !memcmp(was, back, (size_t)n_was),
          z ? "undo puts the compressed slot back byte for byte"
            : "undo puts the plain slot back byte for byte");

      free(was);
      free(over);
      free(back);
   }
   filestream_delete(path);
}
#endif

/* -----------------------------------------------------------------
 * The user's state loads (a file: hotkey, menu, entry slot, auto-load,
 * the network command; Undo Load; a load from RAM) ask for a flush
 * that owes a before-state-load copy. The backup load of a slot about
 * to be saved over, a failed read, and the direct unserialize run-ahead,
 * rewind, netplay and replays use do not.
 * ----------------------------------------------------------------- */
static void test_state_load_kinds(void)
{
   const char *path = "sst_kinds.state";
   size_t sz        = 4096;
   void *buf        = NULL;
   int64_t len      = 0;
   const int bsl    = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   frontend_reset();
   core_fill(sz);
   filestream_delete(path);
   content_save_state(path, true);
   pump(1000);
   flushes      = 0;
   flushes_owed = 0;

   content_load_state(path, false, false);
   pump(1000);
   okf(flushes == 1 && flushes_owed == bsl,
         "kinds: a load from a file asks one before-state-load flush");
   content_load_state(path, false, true);
   pump(1000);
   okf(flushes == 2, "kinds: the auto-load and entry-slot form flushes");
   okf(content_undo_load_state() && flushes == 3, "kinds: Undo Load flushes");
   okf(content_save_state_to_ram() && content_load_state_from_ram()
         && flushes == 4, "kinds: a load from RAM flushes");

   content_save_state(path, true);
   pump(1000);
   okf(flushes == 4, "kinds: the backup load before a save flushed");
   content_load_state("sst_kinds_missing.state", false, false);
   pump(1000);
   okf(flushes == 4, "kinds: a load that read nothing flushed");
   okf(filestream_read_file(path, &buf, &len)
         && content_deserialize_state(buf, (size_t)len) && flushes == 4,
         "kinds: a direct unserialize flushed");
   free(buf);
   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * A load that puts save RAM back (block_sram_overwrite: from a file
 * and Undo Load) rolls nothing back and does not flush; a load from RAM
 * does not put it back. Undo Load with nothing to undo flushes nothing.
 * ----------------------------------------------------------------- */
static void test_state_load_guards(void)
{
   const char *path = "sst_guards.state";
   size_t sz        = 4096;
   const int bsl    = 1 << SAVE_BACKUP_BEFORE_STATE_LOAD;

   frontend_reset();
   core_fill(sz);
   filestream_delete(path);
   content_reset_savestate_backups();
   flushes = 0;
   content_undo_load_state();
   okf(flushes == 0, "guards: Undo Load with nothing to undo flushed");

   content_save_state(path, true);
   pump(1000);
   stub_settings.bools.block_sram_overwrite = true;
   flushes      = 0;
   flushes_owed = 0;
   content_load_state(path, false, false);
   pump(1000);
   okf(flushes == 0, "guards: a blocked load from a file flushed");
   okf(content_undo_load_state() && flushes == 0,
         "guards: a blocked Undo Load flushed");
   okf(content_save_state_to_ram() && content_load_state_from_ram()
         && flushes == 1 && flushes_owed == bsl,
         "guards: a load from RAM owes its copy, blocked or not");
   stub_settings.bools.block_sram_overwrite = false;
   filestream_delete(path);
}

/* -----------------------------------------------------------------
 * Undo Save State writing the slot back, and a RAM state written to
 * its file, each count as one save write, as cloud sync's copies
 * watch for.
 * ----------------------------------------------------------------- */
static void test_writes_counted(void)
{
   const char *path     = "sst_writes.state";
   const char *ram_path = "sst_writes_ram.state";

   frontend_reset();
   core_fill(4096);
   filestream_delete(path);
   filestream_delete(ram_path);
   content_save_state(path, true);
   pump(1000);
   memset(core_mem, 0x5A, core_len);
   content_save_state(path, true);
   pump(1000);
   save_writes = 0;
   okf(content_undo_save_state(), "writes: the undo of a save is pushed");
   pump(1000);
   okf(save_writes == 1, "writes: Undo Save State counts as a save write");

   okf(content_save_state_to_ram(), "writes: a state is kept in RAM");
   save_writes = 0;
   okf(content_ram_state_to_file(ram_path) && save_writes == 1,
         "writes: a RAM state written to file counts as a save write");
   filestream_delete(path);
   filestream_delete(ram_path);
}

/* -----------------------------------------------------------------
 * Automatic state saves: the interval counts from the auto-load, and
 * an .auto the session wanted and did not load goes to the backups
 * before the first write over it.
 * ----------------------------------------------------------------- */
/* NULL @want: any content */
static unsigned auto_backups(const char *auto_path, const char *want)
{
   char dir[PATH_MAX_LENGTH];
   unsigned i, n = 0;
   struct string_list *list;
   struct save_backup_name name;

   if (!save_backups_dir(dir, sizeof(dir), auto_path))
      return 0;
   if (!(list = dir_list_new(dir, NULL, false, true, false, false)))
      return 0;
   for (i = 0; i < list->size; i++)
   {
      void   *data = NULL;
      int64_t len  = 0;
      if (     save_backups_parse(path_basename(list->elems[i].data), &name)
            && name.reason == SAVE_BACKUP_NOT_LOADED
            && (  !want
               || (  filestream_read_file(list->elems[i].data, &data, &len)
                  && len == (int64_t)strlen(want)
                  && !memcmp(data, want, (size_t)len))))
         n++;
      free(data);
   }
   dir_list_free(list);
   return n;
}

static void auto_state_clear(const char *dir)
{
   unsigned i;
   char backups[PATH_MAX_LENGTH];
   struct string_list *list;
   fill_pathname_join(backups, dir, "backups", sizeof(backups));
   if ((list = dir_list_new(backups, NULL, false, true, false, false)))
   {
      for (i = 0; i < list->size; i++)
         filestream_delete(list->elems[i].data);
      dir_list_free(list);
   }
   filestream_delete(backups);
   if ((list = dir_list_new(dir, NULL, false, true, false, false)))
   {
      for (i = 0; i < list->size; i++)
         filestream_delete(list->elems[i].data);
      dir_list_free(list);
   }
   filestream_delete(dir);
}

static void test_auto_state(void)
{
   const char *dir  = "sst_auto";
   const char *path = "sst_auto/Game.state.auto";
   const char *auto_slot = "save_state_io_test_auto_-1.state";

   frontend_reset();
   core_fill(TEST_SAVE_STATE_CHUNK);
   auto_state_clear(dir);
   retro_vfs_mkdir_impl(dir);
   save_backups_set_roots(NULL, dir, NULL, NULL, NULL, NULL, "Core");

   /* interval_after_autoload: from the auto-load on, a whole interval */
   stub_settings.uints.savestate_automatic_interval = 1;
   okf(!content_save_state_automatic(10000000),
       "auto state: no interval save before the auto-load");
   stub_runloop.auto_state_save_ready = true;
   okf(!content_save_state_automatic(10500000),
       "auto state: none until an interval after it");
   okf(content_save_state_automatic(11000000) && file_size(auto_slot) > 0,
       "auto state: one an interval after it");
   filestream_delete(auto_slot);

   /* the next content, its auto-load skipped before any frame: the
    * interval counts from its first frame, not the last content's */
   stub_runloop.auto_state_interval_started = false;
   okf(!content_save_state_automatic(20000000),
       "auto state: skipped, no interval save on the first frame");
   okf(!content_save_state_automatic(20500000),
       "auto state: skipped, none until an interval after it");
   okf(content_save_state_automatic(21000000) && file_size(auto_slot) > 0,
       "auto state: skipped, one an interval after it");
   filestream_delete(auto_slot);

   /* auto_not_loaded_backed_up */
   stub_runloop.auto_state_owned           = false;
   stub_runloop.auto_state_wanted          = true;
   filestream_write_file(path, "old", 3);
   okf(content_auto_save_state(path), "auto state: the save is written");
   okf(file_size(path) > (long)core_len, "auto state: holds the new state");
   okf(auto_backups(path, "old") == 1,
       "auto state: the unloaded one is in the backups, not-loaded");
   okf(content_auto_save_state(path) && auto_backups(path, NULL) == 1,
       "auto state: once a session");

   /* auto_load_turned_on: off at the load, on at the close */
   auto_state_clear(dir);
   retro_vfs_mkdir_impl(dir);
   stub_runloop.auto_state_owned           = false;
   stub_runloop.auto_state_wanted          = false;
   stub_settings.bools.savestate_auto_load = true;
   filestream_write_file(path, "old", 3);
   okf(content_auto_save_state(path) && file_size(path) > (long)core_len,
       "auto state: turned on, the save is written");
   okf(auto_backups(path, "old") == 1,
       "auto state: turned on, the unloaded one is in the backups");
   okf(content_auto_save_state(path) && auto_backups(path, NULL) == 1,
       "auto state: turned on, once a session");
   stub_settings.bools.savestate_auto_load = false;

   /* loaded, or Auto Load State off: written over as it is */
   auto_state_clear(dir);
   retro_vfs_mkdir_impl(dir);
   stub_runloop.auto_state_owned = false;
   stub_runloop.auto_state_wanted = false;
   filestream_write_file(path, "old", 3);
   okf(content_auto_save_state(path) && auto_backups(path, "old") == 0,
       "auto state: no copy without Auto Load State");
   stub_runloop.auto_state_owned = true;
   stub_runloop.auto_state_wanted = true;
   filestream_write_file(path, "old", 3);
   okf(content_auto_save_state(path) && auto_backups(path, "old") == 0,
       "auto state: no copy of the one loaded");

   /* no backups folder to keep it in: the unloaded one stays */
   auto_state_clear(dir);
   retro_vfs_mkdir_impl(dir);
   filestream_write_file("sst_auto/backups", "file", 4);
   stub_runloop.auto_state_owned = false;
   filestream_write_file(path, "old", 3);
   okf(!content_auto_save_state(path) && file_size(path) == 3,
       "auto state: not kept, not written over");

   auto_state_clear(dir);
   save_backups_set_roots(NULL, NULL, NULL, NULL, NULL, NULL, NULL);
}

/* -----------------------------------------------------------------
 * Undo Save or Undo Load of the .auto hands it back: the next
 * automatic save keeps it again. An undo of another slot does not.
 * ----------------------------------------------------------------- */
static void test_auto_state_undo(void)
{
   const char *auto_slot = "save_state_io_test_auto_-1.state";
   const char *other     = "sst_auto_undo.state";
   void   *was = NULL, *back = NULL;
   int64_t n_was = -1, n_back = -2;

   frontend_reset();
   content_reset_savestate_backups();
   filestream_delete(auto_slot);
   filestream_delete(other);
   core_fill(TEST_SAVE_STATE_CHUNK);

   content_save_state(auto_slot, true);
   pump(1000);
   if (!filestream_read_file(auto_slot, &was, &n_was))
      n_was = -1;
   memset(core_mem, 0x33, core_len);
   content_save_state(auto_slot, true);
   pump(1000);
   stub_runloop.auto_state_owned = true;
   okf(content_undo_save_state(), "auto undo: Undo Save of the .auto is pushed");
   okf(!stub_runloop.auto_state_owned,
       "auto undo: the .auto is handed back at the push");
   /* An automatic save before the write takes it again */
   stub_runloop.auto_state_owned = true;
   pump(1000);
   if (!filestream_read_file(auto_slot, &back, &n_back))
      n_back = -2;
   okf(n_was > 0 && n_back == n_was && !memcmp(was, back, (size_t)n_was)
         && !stub_runloop.auto_state_owned,
       "auto undo: the .auto put back is not the session's");

   stub_runloop.auto_state_owned = true;
   content_load_state(auto_slot, false, false);
   pump(1000);
   stub_runloop.auto_state_owned = true;
   okf(content_undo_load_state() && !stub_runloop.auto_state_owned,
       "auto undo: Undo Load of the .auto hands it back");
   stub_runloop.auto_state_owned = true;
   okf(content_undo_load_state() && stub_runloop.auto_state_owned,
       "auto undo: undoing that undo keeps the .auto the session's");

   stub_runloop.auto_state_owned = true;
   content_save_state(other, true);
   pump(1000);
   content_save_state(other, true);
   pump(1000);
   okf(content_undo_save_state(), "auto undo: Undo Save of a slot is pushed");
   pump(1000);
   okf(stub_runloop.auto_state_owned,
       "auto undo: Undo Save of a slot keeps the .auto the session's");
   content_load_state(auto_slot, false, false);
   pump(1000);
   content_load_state(other, false, false);
   pump(1000);
   okf(content_undo_load_state() && stub_runloop.auto_state_owned,
       "auto undo: Undo Load of a slot keeps the .auto the session's");

   free(was);
   free(back);
   filestream_delete(auto_slot);
   filestream_delete(other);
}

/* -----------------------------------------------------------------
 * The interval save skips its tick while another state save is still
 * writing, as both write the .auto's temp, or a load reads it; the
 * next frame tries again.
 * ----------------------------------------------------------------- */
static void test_auto_state_skip_tick(void)
{
   const char *auto_slot = "save_state_io_test_auto_-1.state";
   const char *other     = "sst_skip_tick.state";
   void   *before = NULL, *landed = NULL;
   int64_t n_before = -1, n_landed = -1;
   int i;

   frontend_reset();
   content_reset_savestate_backups();
   filestream_delete(auto_slot);
   filestream_delete(other);
   core_fill(40 * TEST_SAVE_STATE_CHUNK);
   stub_settings.uints.savestate_automatic_interval = 1;
   stub_runloop.auto_state_save_ready = true;
   /* only the save is skipped: the interval starts on its frame */
   content_save_state(other, true);
   content_save_state_automatic(10000000);
   pump(1000);
   okf(content_save_state_automatic(11000000) && file_size(auto_slot) > 0,
       "skip tick: nothing writing, the interval save writes");
   if (!filestream_read_file(auto_slot, &before, &n_before))
      n_before = -1;

   /* a save to the Auto slot past its backup read, still writing */
   clock_step = TEST_TICK_BUDGET_US;
   memset(core_mem, 0x99, core_len);
   content_save_state(auto_slot, true);
   for (i = 0; i < 1000 && content_load_state_in_progress(NULL); i++)
      task_queue_check();
   task_queue_check();
   okf(content_save_state_in_progress(NULL) && tmp_exists(auto_slot),
       "skip tick: the Auto slot save is writing");
   okf(!content_save_state_automatic(12000000)
         && n_before > 0 && !written_into(auto_slot, before, n_before),
       "skip tick: the interval save skips its tick");
   pump(1000);
   clock_step = 0;
   if (!filestream_read_file(auto_slot, &landed, &n_landed))
      n_landed = -1;
   memset(core_mem, 0x55, core_len);
   okf(content_save_state_automatic(12016000)
         && n_landed > 0 && written_into(auto_slot, landed, n_landed),
       "skip tick: the next frame saves");

   /* a load of the Auto slot still reading it */
   free(landed);
   landed = NULL;
   if (!filestream_read_file(auto_slot, &landed, &n_landed))
      n_landed = -1;
   clock_step = TEST_TICK_BUDGET_US;
   content_load_state(auto_slot, false, false);
   task_queue_check();
   memset(core_mem, 0x66, core_len);
   okf(     content_load_state_in_progress(NULL)
         && !content_save_state_in_progress(NULL),
       "skip tick: a load of the Auto slot is reading");
   okf(!content_save_state_automatic(13016000)
         && n_landed > 0 && !written_into(auto_slot, landed, n_landed),
       "skip tick: the interval save skips its tick during a load");
   pump(1000);
   clock_step = 0;
   memset(core_mem, 0x66, core_len);
   okf(content_save_state_automatic(13032000)
         && written_into(auto_slot, landed, n_landed),
       "skip tick: the next frame saves after the load");

   free(before);
   free(landed);
   filestream_delete(auto_slot);
   filestream_delete(other);
}

/* -----------------------------------------------------------------
 * A not-loaded .auto that cannot move into its backups (across drives)
 * waits beside itself under a held name while a task copies it there.
 * ----------------------------------------------------------------- */
#define KEEP_DIR   "sst_keep"
#define KEEP_AUTO  "sst_keep/Game.state.auto"
#define KEEP_AUTO2 "sst_keep/Other.state.auto"

static void keep_held(char *s, size_t len, const char *path, unsigned n)
{
   char base[256];
   snprintf(base, sizeof(base), "%s.not-loaded", path);
   fill_pathname_aside(s, base, n, len);
}

static bool keep_holds(const char *path, const char *text)
{
   return filestream_matches_buf(path, text, strlen(text));
}

static void keep_put(const char *path, const char *text)
{
   filestream_write_file(path, text, (int64_t)strlen(text));
}

static void keep_reset(void)
{
   frontend_reset();
   core_fill(TEST_SAVE_STATE_CHUNK);
   auto_state_clear(KEEP_DIR);
   retro_vfs_mkdir_impl(KEEP_DIR);
   save_backups_set_roots(NULL, KEEP_DIR, NULL, NULL, NULL, NULL, "Core");
   stub_runloop.system.info.library_name   = "Core";
   stub_runloop.auto_state_wanted          = true;
   __atomic_store_n(&refuse_renames, REFUSE_MOVE, __ATOMIC_RELAXED);
}

/* Files in the backups whose names end ".tmp" */
static unsigned keep_temps(void)
{
   unsigned i, n = 0;
   struct string_list *list = dir_list_new(KEEP_DIR "/backups", NULL,
         false, true, false, false);
   for (i = 0; list && i < list->size; i++)
      if (string_ends_with(list->elems[i].data, ".tmp"))
         n++;
   if (list)
      dir_list_free(list);
   return n;
}

/* The not-loaded copy of @path holding @text, into @s */
static bool keep_copy(char *s, size_t len, const char *path,
      const char *text)
{
   unsigned i;
   bool found = false;
   char dir[256];
   struct string_list *list;
   if (!save_backups_dir(dir, sizeof(dir), path)
         || !(list = dir_list_new(dir, NULL, false, true, false, false)))
      return false;
   for (i = 0; !found && i < list->size; i++)
      if (     !string_ends_with(list->elems[i].data, ".tmp")
            && keep_holds(list->elems[i].data, text))
      {
         strlcpy(s, list->elems[i].data, len);
         found = true;
      }
   dir_list_free(list);
   return found;
}

/* Every file in the backups a minute old */
static void keep_age(void)
{
   unsigned i;
   struct string_list *list = dir_list_new(KEEP_DIR "/backups", NULL,
         false, true, false, false);
   for (i = 0; list && i < list->size; i++)
      path_set_mtime(list->elems[i].data, (int64_t)time(NULL) - 61);
   if (list)
      dir_list_free(list);
}

static bool keep_count_task(retro_task_t *t, void *ud)
{
   (void)t;
   (*(unsigned*)ud)++;
   return false;
}

static unsigned keep_tasks(void)
{
   unsigned n = 0;
   task_finder_data_t fd;
   fd.func     = keep_count_task;
   fd.userdata = &n;
   task_queue_find(&fd);
   return n;
}

static void test_auto_state_keep(void)
{
   unsigned k;
   int ticks;
   unsigned syncs, errors, msgs, finish_errors;
   bool once;
   char h[SAVE_BACKUPS_HELD_MAX + 1][256];
   char copy[256], name[256], dir[256];
   char *big = (char*)malloc(4 * TEST_SAVE_STATE_CHUNK + 2);

   for (k = 1; k <= SAVE_BACKUPS_HELD_MAX; k++)
      keep_held(h[k], sizeof(h[k]), KEEP_AUTO, k);
   memset(big, 'b', 4 * TEST_SAVE_STATE_CHUNK + 1);
   big[4 * TEST_SAVE_STATE_CHUNK + 1] = '\0';

   /* across drives: the new state at once, the old one held beside it
    * and copied on a task, which counts it in the newest 10 */
   keep_reset();
   retro_vfs_mkdir_impl(KEEP_DIR "/backups");
   for (k = 0; k < 10; k++)
   {
      char text[4];
      snprintf(name, sizeof(name), KEEP_DIR "/backups/Game - 2000-01-01 "
            "00-00-0%u - Core - not-loaded.state.auto", k);
      snprintf(text, sizeof(text), "p%u", k);
      keep_put(name, text);
   }
   keep_put(KEEP_AUTO, "old");
   syncs  = file_syncs;
   errors = errors_logged;
   msgs   = msgs_pushed;
   okf(content_auto_save_state(KEEP_AUTO)
         && file_size(KEEP_AUTO) > (long)core_len,
       "keep: across drives, the new state is written at once");
   okf(keep_holds(h[1], "old") && auto_backups(KEEP_AUTO, "old") == 0
         && stub_runloop.auto_state_owned && keep_tasks() == 1,
       "keep: the old one is held beside it, its copy on a task");
   pump(100);
   okf(keep_copy(copy, sizeof(copy), KEEP_AUTO, "old")
         && auto_backups(KEEP_AUTO, NULL) == 10
         && !path_is_valid(KEEP_DIR "/backups/Game - 2000-01-01 00-00-00 "
            "- Core - not-loaded.state.auto"),
       "keep: the copy lands, counted in the newest 10");
   okf(keep_holds(h[1], "old") && file_syncs == syncs,
       "keep: unthreaded, no sync, and the held one stays");
   okf(errors_logged == errors && msgs_pushed == msgs && !keep_temps(),
       "keep: nothing logged or shown, no temp left");

   /* one piece a tick */
   for (k = 0; k < 2; k++)
   {
      keep_reset();
      keep_put(KEEP_AUTO, big);
      clock_step = k ? TEST_TICK_BUDGET_US : 0;
      content_auto_save_state(KEEP_AUTO);
      ticks = pump(100);
      okf(auto_backups(KEEP_AUTO, big) == 1 && (k ? ticks >= 7 : ticks <= 4),
          k ? "keep: a spent budget copies one piece a tick"
            : "keep: an open budget copies it in one tick");
   }

   /* a failed copy: the held one stays, the error logged, no notice */
   keep_reset();
   keep_put(KEEP_AUTO, "old");
   __atomic_store_n(&refuse_renames, REFUSE_MOVE | REFUSE_LAND,
         __ATOMIC_RELAXED);
   errors = errors_logged;
   msgs   = msgs_pushed;
   content_auto_save_state(KEEP_AUTO);
   pump(100);
   okf(keep_holds(h[1], "old") && auto_backups(KEEP_AUTO, NULL) == 0
         && !keep_temps(),
       "keep: a failed copy keeps the held one");
   okf(errors_logged > errors && msgs_pushed == msgs,
       "keep: a failed copy is logged, not shown");

   /* the new state could not take its place: the old one goes back */
   keep_reset();
   keep_put(KEEP_AUTO, "old");
   __atomic_store_n(&refuse_renames, REFUSE_MOVE | REFUSE_FINISH,
         __ATOMIC_RELAXED);
   errors = errors_logged;
   okf(!content_auto_save_state(KEEP_AUTO) && keep_holds(KEEP_AUTO, "old")
         && !path_is_valid(h[1]) && !stub_runloop.auto_state_owned
         && !keep_tasks(),
       "keep: a failed finish puts the held one back");
   finish_errors = errors_logged - errors;

   /* nor put back: logged, held for the next load */
   keep_reset();
   keep_put(KEEP_AUTO, "old");
   __atomic_store_n(&refuse_renames, REFUSE_MOVE | REFUSE_FINISH
         | REFUSE_BACK, __ATOMIC_RELAXED);
   errors = errors_logged;
   okf(!content_auto_save_state(KEEP_AUTO) && keep_holds(h[1], "old")
         && !path_is_valid(KEEP_AUTO) && !stub_runloop.auto_state_owned
         && errors_logged - errors == finish_errors + 1,
       "keep: a failed put-back is logged, the held one kept");

   /* the first free held name; all nine taken, an inline copy */
   keep_reset();
   keep_put(h[1], "held1");
   keep_put(KEEP_AUTO, "old");
   content_auto_save_state(KEEP_AUTO);
   okf(keep_holds(h[1], "held1") && keep_holds(h[2], "old"),
       "keep: a held name in use, the next one");
   pump(100);
   okf(auto_backups(KEEP_AUTO, "held1") == 1
         && auto_backups(KEEP_AUTO, "old") == 1,
       "keep: both held ones copied");
   keep_reset();
   for (k = 1; k <= SAVE_BACKUPS_HELD_MAX; k++)
      keep_put(h[k], "held");
   keep_put(KEEP_AUTO, "old");
   okf(content_auto_save_state(KEEP_AUTO)
         && file_size(KEEP_AUTO) > (long)core_len
         && auto_backups(KEEP_AUTO, "old") == 1 && !keep_tasks()
         && keep_holds(h[SAVE_BACKUPS_HELD_MAX], "held"),
       "keep: all nine taken, copied inline and written");

   /* one task at a time; a path asked for twice is copied once */
   keep_reset();
   keep_put(KEEP_AUTO, "old1");
   keep_put(KEEP_AUTO2, "old2");
   content_auto_save_state(KEEP_AUTO);
   stub_runloop.auto_state_owned = false;
   content_auto_save_state(KEEP_AUTO2);
   content_auto_state_keep(KEEP_AUTO);
   okf(keep_tasks() == 1, "keep: a second path waits its turn");
   pump(100);
   /* the folder's not-loaded copies, of either game */
   okf(auto_backups(KEEP_AUTO, NULL) == 2
         && auto_backups(KEEP_AUTO, "old1") == 1
         && auto_backups(KEEP_AUTO2, "old2") == 1,
       "keep: each copied once");

   /* exit cancels: the temp goes, the held one stays */
   keep_reset();
   keep_put(KEEP_AUTO, big);
   clock_step = TEST_TICK_BUDGET_US;
   content_auto_save_state(KEEP_AUTO);
   task_queue_check();
   task_queue_check();
   okf(keep_temps() == 1, "keep: cancel, the copy under way");
   keep_held(name, sizeof(name), KEEP_AUTO2, 1);
   keep_put(name, "old2");
   content_auto_state_keep(KEEP_AUTO2);
   task_queue_reset();
   pump(100);
   okf(!keep_temps() && !auto_backups(KEEP_AUTO, NULL)
         && keep_holds(h[1], big),
       "keep: cancel, the temp goes and the held one stays");
   okf(keep_holds(name, "old2") && !keep_tasks(),
       "keep: cancel, the path owed waits for its next load");

   /* content load: nothing held, no task */
   keep_reset();
   content_auto_state_sweep(KEEP_AUTO);
   okf(!keep_tasks(), "keep: nothing held, nothing to do");

   /* left held, no copy; the .auto gone and never put back */
   keep_put(h[1], "old");
   content_auto_state_sweep(KEEP_AUTO);
   pump(100);
   okf(keep_copy(copy, sizeof(copy), KEEP_AUTO, "old")
         && keep_holds(h[1], "old") && !path_is_valid(KEEP_AUTO),
       "keep: a load copies one left held, never back as the .auto");

   /* left held with its copy made: kept until that copy is a minute
    * old, unthreaded, then deleted without another copy */
   content_auto_state_sweep(KEEP_AUTO);
   pump(100);
   okf(keep_holds(h[1], "old") && auto_backups(KEEP_AUTO, NULL) == 1,
       "keep: its copy too new, the held one stays");
   path_set_mtime(copy, (int64_t)time(NULL) - 61);
   content_auto_state_sweep(KEEP_AUTO);
   pump(100);
   okf(!path_is_valid(h[1]) && auto_backups(KEEP_AUTO, NULL) == 1,
       "keep: its copy a minute old, the held one goes, no new copy");

   /* a partial copy: its temp goes, a fresh copy */
   snprintf(name, sizeof(name), KEEP_DIR "/backups/Game - 2001-01-01 "
         "00-00-00 - Core - not-loaded.state.auto.tmp");
   keep_put(name, "part");
   keep_put(KEEP_DIR "/backups/Other - 2001-01-01 00-00-00 - Core - "
         "not-loaded.state.auto.tmp", "part");
   keep_put(h[1], "old2");
   content_auto_state_sweep(KEEP_AUTO);
   pump(100);
   okf(!path_is_valid(name) && keep_temps() == 1
         && keep_copy(copy, sizeof(copy), KEEP_AUTO, "old2"),
       "keep: a partial copy's temp goes, copied afresh");

   /* two left held: the one its newest copy holds goes, the other is
    * copied */
   path_set_mtime(copy, (int64_t)time(NULL) - 61);
   keep_put(h[2], "new2");
   content_auto_state_sweep(KEEP_AUTO);
   pump(100);
   okf(!path_is_valid(h[1]) && auto_backups(KEEP_AUTO, "old2") == 1
         && auto_backups(KEEP_AUTO, "new2") == 1 && keep_holds(h[2], "new2"),
       "keep: two left held, each taken in turn");

   /* the lower held name newer than the higher, over several loads:
    * each compared with all its copies, so none copied twice and both
    * go once their copies are a minute old */
   keep_reset();
   retro_vfs_mkdir_impl(KEEP_DIR "/backups");
   keep_put(KEEP_DIR "/backups/Game - 2000-01-01 00-00-00 - Core - "
         "not-loaded.state.auto", "Y");
   keep_put(h[1], "Z");
   keep_put(h[2], "Y");
   once = true;
   for (k = 0; k < 4; k++)
   {
      keep_age();
      content_auto_state_sweep(KEEP_AUTO);
      pump(100);
      if (     auto_backups(KEEP_AUTO, "Y") != 1
            || auto_backups(KEEP_AUTO, "Z") != 1)
         once = false;
   }
   okf(once, "keep: held ones older and newer, none copied twice");
   okf(!path_is_valid(h[1]) && !path_is_valid(h[2]),
       "keep: held ones older and newer, both go");

   /* a longer copy that starts the same is no copy of it */
   keep_reset();
   retro_vfs_mkdir_impl(KEEP_DIR "/backups");
   keep_put(KEEP_DIR "/backups/Game - 2000-01-01 00-00-00 - Core - "
         "not-loaded.state.auto", "YY");
   keep_put(h[1], "Y");
   keep_age();
   content_auto_state_sweep(KEEP_AUTO);
   pump(100);
   okf(auto_backups(KEEP_AUTO, "Y") == 1 && keep_holds(h[1], "Y"),
       "keep: a longer copy that starts the same is no copy");

   /* named as the copy starts, after a compare: never over a copy made
    * meanwhile */
   keep_reset();
   retro_vfs_mkdir_impl(KEEP_DIR "/backups");
   keep_put(KEEP_DIR "/backups/Game - 2000-01-01 00-00-00 - Core - "
         "not-loaded.state.auto", big);
   big[4 * TEST_SAVE_STATE_CHUNK] = 'c';
   keep_put(h[1], big);
   clock_step = TEST_TICK_BUDGET_US;
   content_auto_state_sweep(KEEP_AUTO);
   task_queue_check();
   task_queue_check();
   okf(save_backups_dir(dir, sizeof(dir), KEEP_AUTO)
         && save_backups_name_copy(name, sizeof(name), dir, KEEP_AUTO,
            "Core", SAVE_BACKUP_NOT_LOADED) == SAVE_BACKUP_MADE,
       "keep: a copy named meanwhile");
   keep_put(name, "made");
   pump(100);
   clock_step = 0;
   okf(keep_holds(name, "made") && auto_backups(KEEP_AUTO, big) == 1,
       "keep: the copy named as it starts, not over one made meanwhile");
   big[4 * TEST_SAVE_STATE_CHUNK] = 'b';

   __atomic_store_n(&refuse_renames, 0, __ATOMIC_RELAXED);
   clock_step = 0;
   auto_state_clear(KEEP_DIR);
   save_backups_set_roots(NULL, NULL, NULL, NULL, NULL, NULL, NULL);
   free(big);
}

/* Threaded: the copy is synced, then the held one goes. Asserted only
 * once the queue has drained, or on what the worker cannot touch. */
static void test_auto_state_keep_threaded(void)
{
   unsigned syncs;
   char h1[256];

   keep_held(h1, sizeof(h1), KEEP_AUTO, 1);
   keep_reset();
   keep_put(KEEP_AUTO, "old");
   syncs = __atomic_load_n(&file_syncs, __ATOMIC_RELAXED);
   okf(content_auto_save_state(KEEP_AUTO)
         && file_size(KEEP_AUTO) > (long)core_len,
       "threaded keep: the new state is written at once");
   pump_threaded();
   okf(auto_backups(KEEP_AUTO, "old") == 1 && !path_is_valid(h1)
         && __atomic_load_n(&file_syncs, __ATOMIC_RELAXED) > syncs,
       "threaded keep: the copy synced, the held one gone");

   /* left held with its copy made by an unthreaded session */
   keep_put(h1, "old");
   syncs = __atomic_load_n(&file_syncs, __ATOMIC_RELAXED);
   content_auto_state_sweep(KEEP_AUTO);
   pump_threaded();
   okf(!path_is_valid(h1) && auto_backups(KEEP_AUTO, NULL) == 1
         && __atomic_load_n(&file_syncs, __ATOMIC_RELAXED) > syncs,
       "threaded keep: its copy synced, the held one gone, no new copy");

   __atomic_store_n(&refuse_renames, 0, __ATOMIC_RELAXED);
   auto_state_clear(KEEP_DIR);
   save_backups_set_roots(NULL, NULL, NULL, NULL, NULL, NULL, NULL);
}

/* The worker waiting at its temp's move; the frame keeps gathering */
static bool temp_move_waits(void)
{
   int spins = 0;
   while (__atomic_load_n(&hold_temp_move, __ATOMIC_ACQUIRE) != 2)
   {
      if (spins++ >= 20000)
         return false;
      task_queue_check();
      retro_sleep(1);
   }
   return true;
}

static void test_auto_state_skip_tick_threaded(void)
{
   const char *auto_slot = "save_state_io_test_auto_-1.state";
   void   *was = NULL, *cur = NULL;
   int64_t n_was = -1, n_cur = -1;
   bool    waits;

   frontend_reset();
   content_reset_savestate_backups();
   filestream_delete(auto_slot);
   core_fill(TEST_SAVE_STATE_CHUNK);
   stub_settings.uints.savestate_automatic_interval = 1;
   stub_runloop.auto_state_save_ready = true;
   content_save_state_automatic(10000000);

   memset(core_mem, 0x11, core_len);
   content_save_state(auto_slot, true);
   pump_threaded();
   if (!filestream_read_file(auto_slot, &was, &n_was))
      n_was = -1;
   memset(core_mem, 0x33, core_len);
   content_save_state(auto_slot, true);
   pump_threaded();
   if (!filestream_read_file(auto_slot, &cur, &n_cur))
      n_cur = -1;

   /* Undo Save of the .auto, held at its move into place */
   __atomic_store_n(&hold_temp_move, 1, __ATOMIC_RELEASE);
   okf(content_undo_save_state(), "threaded skip tick: Undo Save is pushed");
   waits = temp_move_waits();
   okf(waits && !content_save_state_automatic(11000000)
         && n_cur > 0 && !written_into(auto_slot, cur, n_cur),
       "threaded skip tick: no interval save during the undo");
   __atomic_store_n(&hold_temp_move, 0, __ATOMIC_RELEASE);
   pump_threaded();
   okf(n_was > 0 && !written_into(auto_slot, was, n_was),
       "threaded skip tick: the undo lands");
   memset(core_mem, 0x55, core_len);
   okf(content_save_state_automatic(11016000)
         && written_into(auto_slot, was, n_was),
       "threaded skip tick: the next frame saves after the undo");

   /* Save State to the Auto slot, held the same way */
   free(cur);
   cur = NULL;
   if (!filestream_read_file(auto_slot, &cur, &n_cur))
      n_cur = -1;
   memset(core_mem, 0x77, core_len);
   __atomic_store_n(&hold_temp_move, 1, __ATOMIC_RELEASE);
   content_save_state(auto_slot, true);
   waits = temp_move_waits();
   okf(waits && !content_save_state_automatic(12016000)
         && n_cur > 0 && !written_into(auto_slot, cur, n_cur),
       "threaded skip tick: no interval save during the Auto slot save");
   __atomic_store_n(&hold_temp_move, 0, __ATOMIC_RELEASE);
   pump_threaded();
   free(was);
   was = NULL;
   if (!filestream_read_file(auto_slot, &was, &n_was))
      n_was = -1;
   memset(core_mem, 0x55, core_len);
   okf(content_save_state_automatic(12032000)
         && n_was > 0 && written_into(auto_slot, was, n_was),
       "threaded skip tick: the next frame saves after the Auto slot save");

   free(was);
   free(cur);
   filestream_delete(auto_slot);
}

/* -----------------------------------------------------------------
 * A save that fails takes no thumbnail: the slot's old one stays.
 * ----------------------------------------------------------------- */
static void test_thumbnail_on_success(void)
{
   const char *path = "sst_thumb.state";
   const char *png  = "sst_thumb.state.png";
   void   *data     = NULL;
   int64_t len      = 0;

   frontend_reset();
   content_reset_savestate_backups();
   core_fill(TEST_SAVE_STATE_CHUNK);
   filestream_delete(path);
   filestream_write_file(png, "old", 3);
   stub_settings.bools.savestate_thumbnail_enable = true;
   shots = 0;
   set_save_state_in_background(true);
   core_ser_fails = 1;
   content_save_state(path, true);
   pump(1000);
   core_ser_fails = 0;
   set_save_state_in_background(false);
   okf(shots == 0, "thumbnail: none for a save that failed");
   okf(     filestream_read_file(png, &data, &len) && len == 3
         && !memcmp(data, "old", 3), "thumbnail: the old one stays");
   free(data);
   content_save_state(path, true);
   pump(1000);
   okf(shots == 1, "thumbnail: one for a save that was written");
   filestream_delete(path);
   filestream_delete(png);
}

static bool holds_text(const char *path, const char *text)
{
   void   *data = NULL;
   int64_t len  = 0;
   bool    ok   = filestream_read_file(path, &data, &len)
         && len == (int64_t)strlen(text) && !memcmp(data, text, (size_t)len);
   free(data);
   return ok;
}

/* -----------------------------------------------------------------
 * A written state whose new thumbnail fails keeps none: never a
 * picture of an older state beside it.
 * ----------------------------------------------------------------- */
static void test_thumbnail_fails(void)
{
   const char *path = "sst_thumbfail.state";
   const char *png  = "sst_thumbfail.state.png";

   frontend_reset();
   content_reset_savestate_backups();
   core_fill(TEST_SAVE_STATE_CHUNK);
   filestream_delete(path);

   filestream_write_file(png, "old", 3);
   content_save_state(path, true);
   pump(1000);
   okf(file_size(path) > 0 && holds_text(png, "old"),
       "thumbnail: off, the slot's picture is left alone");

   stub_settings.bools.savestate_thumbnail_enable = true;
   shot_fails      = true;
   shots_cancelled = 0;
   content_save_state(path, true);
   pump(1000);
   okf(file_size(png) < 0,
       "thumbnail: a failed one leaves none beside a written state");
   filestream_write_file(png, "old", 3);
   okf(content_auto_save_state(path) && file_size(png) < 0,
       "thumbnail: a failed one leaves none beside an auto save");
   okf(shots_cancelled == 0,
       "thumbnail: a failed one stops no other shot of the slot");
   shot_fails = false;
   filestream_write_file(png, "old", 3);
   okf(content_auto_save_state(path) && holds_text(png, "new"),
       "thumbnail: an auto save's replaces the old one");
   filestream_delete(path);
   filestream_delete(png);
}

/* -----------------------------------------------------------------
 * A written state's slot keeps its picture until the new one is
 * taken: the old one goes only when that fails.
 * ----------------------------------------------------------------- */
static void test_thumbnail_kept_until_shot(void)
{
   const char *path = "sst_thumbkeep.state";
   const char *png  = "sst_thumbkeep.state.png";

   frontend_reset();
   content_reset_savestate_backups();
   core_fill(TEST_SAVE_STATE_CHUNK);
   filestream_delete(path);
   filestream_write_file(png, "old", 3);
   stub_settings.bools.savestate_thumbnail_enable = true;
   shots       = 0;
   png_at_shot = false;
   content_save_state(path, true);
   pump(1000);
   okf(shots == 1 && png_at_shot,
       "thumbnail: the old one stays until the new one is taken");
   okf(holds_text(png, "new"), "thumbnail: the new one replaces it");
   filestream_delete(path);
   filestream_delete(png);
}

/* -----------------------------------------------------------------
 * Undo Save and the RAM state written at close put a state of another
 * moment in the slot: with thumbnails on, its picture goes once the
 * state is written.
 * ----------------------------------------------------------------- */
static void test_thumbnail_other_moment(void)
{
   const char *path = "sst_thumbmoment.state";
   const char *png  = "sst_thumbmoment.state.png";
   const char *tmp  = "sst_thumbmoment.state.tmp";
   const char *rtmp = "sst_thumbmoment.state.ram.tmp";
   int i;

   for (i = 0; i < 3; i++)
   {
      frontend_reset();
      content_reset_savestate_backups();
      core_fill(TEST_SAVE_STATE_CHUNK);
      filestream_delete(path);
      content_save_state(path, true);
      pump(1000);
      memset(core_mem, 0x33, core_len);
      content_save_state(path, true);
      pump(1000);
      stub_settings.bools.savestate_thumbnail_enable = (i > 0);
      filestream_write_file(png, "new", 3);
      /* the third undo cannot write its temporary */
      if (i == 2)
         path_mkdir(tmp);
      shots_cancelled = 0;
      okf(content_undo_save_state(), "thumbnail: Undo Save is taken");
      pump(1000);
      if (i == 2)
         path_rmdir(tmp);
      okf(shots_cancelled == (i == 1)
            && (i != 1 || string_is_equal(shot_cancelled, png)),
          "thumbnail: Undo Save stops a shot of the slot only when it drops");
      if (i == 0)
         okf(holds_text(png, "new"),
             "thumbnail: off, Undo Save leaves the picture alone");
      else if (i == 1)
         okf(file_size(png) < 0,
             "thumbnail: Undo Save drops the undone save's picture");
      else
         okf(holds_text(png, "new"),
             "thumbnail: an Undo Save that fails leaves the picture");
   }

   for (i = 0; i < 3; i++)
   {
      frontend_reset();
      core_fill(TEST_SAVE_STATE_CHUNK);
      stub_settings.bools.savestate_thumbnail_enable = (i > 0);
      filestream_write_file(png, "new", 3);
      okf(content_save_state_to_ram(), "thumbnail: the RAM state is kept");
      /* the third cannot write either temporary */
      if (i == 2)
      {
         path_mkdir(rtmp);
         path_mkdir(tmp);
      }
      shots_cancelled = 0;
      okf(content_ram_state_to_file(path) == (i < 2),
          "thumbnail: the RAM state is written, or fails");
      if (i == 2)
      {
         path_rmdir(rtmp);
         path_rmdir(tmp);
      }
      okf(shots_cancelled == (i == 1)
            && (i != 1 || string_is_equal(shot_cancelled, png)),
          "thumbnail: the RAM state stops a shot of the slot only when it drops");
      if (i == 0)
         okf(holds_text(png, "new"),
             "thumbnail: off, the RAM state leaves the picture alone");
      else if (i == 1)
         okf(file_size(png) < 0,
             "thumbnail: the RAM state drops the slot's picture");
      else
         okf(holds_text(png, "new"),
             "thumbnail: a RAM state that fails leaves the picture");
   }
   filestream_delete(path);
   filestream_delete(png);
}

/* -----------------------------------------------------------------
 * A state task and the synchronous auto save writing one file at once
 * each write their own temporary: both files whole, none left.
 * ----------------------------------------------------------------- */
static void test_state_temps_distinct(void)
{
   const char *path = "sst_temps.state";
   const char *ref  = "sst_temps_ref.state";
   void   *a = NULL, *b = NULL;
   int64_t n_a = -1, n_b = -2;

   frontend_reset();
   content_reset_savestate_backups();
   filestream_delete(path);
   filestream_delete(ref);
   core_fill(20 * TEST_SAVE_STATE_CHUNK);
   content_save_state(ref, true);
   pump(1000);

   /* the task's temporary open, one quantum written */
   clock_step = TEST_TICK_BUDGET_US;
   content_save_state(path, true);
   task_queue_check();
   memset(core_mem, 0x77, core_len);
   okf(content_auto_save_state(path),
       "temps: the synchronous auto save is written");
   pump(100000);
   clock_step = 0;

   if (!filestream_read_file(path, &a, &n_a))
      n_a = -1;
   if (!filestream_read_file(ref, &b, &n_b))
      n_b = -2;
   okf(n_a > 0 && n_a == n_b && !memcmp(a, b, (size_t)n_a),
       "temps: the task's state is written whole");
   okf(!tmp_exists(path) && file_size("sst_temps.state.auto.tmp") < 0,
       "temps: no temporary left");
   free(a);
   free(b);
   filestream_delete(path);
   filestream_delete(ref);
}

/* -----------------------------------------------------------------
 * A state task and the RAM state written at close, one file at once:
 * each its own temporary, the task's state whole, none left.
 * ----------------------------------------------------------------- */
static void test_ram_state_temp_distinct(void)
{
   const char *path = "sst_ramtemps.state";
   const char *ref  = "sst_ramtemps_ref.state";
   void   *a = NULL, *b = NULL;
   int64_t n_a = -1, n_b = -2;

   frontend_reset();
   content_reset_savestate_backups();
   filestream_delete(path);
   filestream_delete(ref);
   core_fill(20 * TEST_SAVE_STATE_CHUNK);
   content_save_state(ref, true);
   pump(1000);

   /* the task's temporary open, one quantum written */
   clock_step = TEST_TICK_BUDGET_US;
   content_save_state(path, true);
   task_queue_check();
   memset(core_mem, 0x66, core_len);
   okf(content_save_state_to_ram() && content_ram_state_to_file(path),
       "ram temps: the RAM state is written");
   pump(100000);
   clock_step = 0;

   if (!filestream_read_file(path, &a, &n_a))
      n_a = -1;
   if (!filestream_read_file(ref, &b, &n_b))
      n_b = -2;
   okf(n_a > 0 && n_a == n_b && !memcmp(a, b, (size_t)n_a),
       "ram temps: the task's state is written whole");
   okf(!tmp_exists(path) && file_size("sst_ramtemps.state.ram.tmp") < 0,
       "ram temps: no temporary left");
   free(a);
   free(b);
   filestream_delete(path);
   filestream_delete(ref);
}

/* -----------------------------------------------------------------
 * The RAM state written at close replaces the slot's file, whole,
 * through its own temporary, with the state kept in RAM.
 * ----------------------------------------------------------------- */
static void test_ram_state_file(void)
{
   const char *path = "sst_ram.state";
   const char *ref  = "sst_ram_ref.state";
   void   *a = NULL, *b = NULL;
   int64_t n_a = -1, n_b = -2;

   frontend_reset();
   content_reset_savestate_backups();
   filestream_delete(ref);
   filestream_write_file(path, "old", 3);
   core_fill(4 * TEST_SAVE_STATE_CHUNK);
   content_save_state(ref, true);
   pump(1000);
   okf(content_save_state_to_ram(), "ram state: kept in RAM");
   memset(core_mem, 0x66, core_len);
   okf(content_ram_state_to_file(path), "ram state: written to its file");

   if (!filestream_read_file(path, &a, &n_a))
      n_a = -1;
   if (!filestream_read_file(ref, &b, &n_b))
      n_b = -2;
   okf(n_a > 0 && n_a == n_b && !memcmp(a, b, (size_t)n_a),
       "ram state: the file holds the state kept in RAM");
   okf(!tmp_exists(path) && file_size("sst_ram.state.ram.tmp") < 0,
       "ram state: no temporary left");
   free(a);
   free(b);
   filestream_delete(path);
   filestream_delete(ref);
}

/* The longest name this directory takes: 0 if none fits in the probe */
static size_t longest_name(void)
{
   char   name[1024];
   size_t lo = 0, hi = sizeof(name) - 1;
   memset(name, 'n', sizeof(name));
   /* @lo opens, @hi does not */
   while (hi - lo > 1)
   {
      size_t mid = (lo + hi) / 2;
      RFILE *f;
      name[mid] = '\0';
      if ((f = filestream_open(name, RETRO_VFS_FILE_ACCESS_WRITE,
            RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      {
         filestream_close(f);
         filestream_delete(name);
         lo = mid;
      }
      else
         hi = mid;
      name[mid] = 'n';
   }
   return lo;
}

static bool files_equal(const char *a, const char *b)
{
   void   *x = NULL, *y = NULL;
   int64_t n_x = -1, n_y = -2;
   bool    ok;
   if (!filestream_read_file(a, &x, &n_x))
      n_x = -1;
   if (!filestream_read_file(b, &y, &n_y))
      n_y = -2;
   ok = n_x > 0 && n_x == n_y && !memcmp(x, y, (size_t)n_x);
   free(x);
   free(y);
   return ok;
}

/* -----------------------------------------------------------------
 * A slot name too long for the auto save's or the RAM state's own
 * temporary saves through "<path>.tmp", as before; the RAM state never
 * while a state task writes that one.
 * ----------------------------------------------------------------- */
static void test_long_names(void)
{
   const char *ref = "sst_long_ref.state";
   char   path[1024], tmp[1040], atmp[1040], rtmp[1040];
   size_t n = longest_name();

   if (n < 16 || n + 8 >= sizeof(path))
   {
      printf("       (longest name %u bytes)\n", (unsigned)n);
      okf(false, "long names: no name length limit found");
      return;
   }
   /* "<path>.tmp" is the longest name the directory takes */
   memset(path, 'L', n - 4);
   path[n - 4] = '\0';
   snprintf(tmp,  sizeof(tmp),  "%s.tmp", path);
   snprintf(atmp, sizeof(atmp), "%s.auto.tmp", path);
   snprintf(rtmp, sizeof(rtmp), "%s.ram.tmp", path);
   printf("       (longest name %u bytes)\n", (unsigned)n);

   frontend_reset();
   content_reset_savestate_backups();
   filestream_delete(path);
   filestream_delete(ref);
   core_fill(4 * TEST_SAVE_STATE_CHUNK);
   content_save_state(ref, true);
   pump(1000);
   okf(content_auto_save_state(path) && files_equal(path, ref),
       "long names: the auto save is written");
   okf(file_size(tmp) < 0 && file_size(atmp) < 0,
       "long names: the auto save leaves no temporary");

   filestream_delete(path);
   okf(content_save_state_to_ram(), "long names: the RAM state is kept");
   memset(core_mem, 0x66, core_len);
   okf(content_ram_state_to_file(path) && files_equal(path, ref),
       "long names: the RAM state is written");
   okf(file_size(tmp) < 0 && file_size(rtmp) < 0,
       "long names: the RAM state leaves no temporary");

   /* the task's "<path>.tmp" open, one quantum written */
   filestream_delete(path);
   core_fill(20 * TEST_SAVE_STATE_CHUNK);
   memset(core_mem, 0x66, core_len);
   okf(content_save_state_to_ram(), "long names: the RAM state is kept");
   core_fill(20 * TEST_SAVE_STATE_CHUNK);
   content_save_state(ref, true);
   pump(1000);
   clock_step = TEST_TICK_BUDGET_US;
   content_save_state(path, true);
   task_queue_check();
   okf(!content_ram_state_to_file(path),
       "long names: the RAM state does not share a state task's temporary");
   pump(100000);
   clock_step = 0;
   okf(files_equal(path, ref) && file_size(tmp) < 0,
       "long names: the task's state is written whole");

   filestream_delete(path);
   filestream_delete(ref);
}

static int run_default_lane(void)
{
   printf("== task_save.c I/O regression oracle ==\n");
   printf("   quantum %d B, tick budget %d us\n\n",
         TEST_SAVE_STATE_CHUNK, TEST_TICK_BUDGET_US);

   task_queue_init(false, msg_push_stub);

   test_budget_unlimited();
   test_budget_exhausted();
   test_roundtrip(0, "round-trip is byte-exact with an unexpired budget");
   test_roundtrip(TEST_TICK_BUDGET_US,
         "round-trip is byte-exact at one quantum per tick");
   test_close_waits_for_save();
   test_serialize_failure();
   test_failed_save_keeps_old();
   test_background_serialize_timing();
   test_open_failure();
   test_truncated_state();
   test_short_file();
   test_blocking_exclusion();
   test_undo_snapshot_reuses();
   test_undo_snapshot_grows();
   test_undo_snapshot_failure_keeps_previous();
   test_undo_allocates_nothing();
   test_notify_without_allocation();
   test_load_held_behind_save();
   test_state_load_kinds();
   test_state_load_guards();
   test_writes_counted();
   test_undo_save_slot();
   test_undo_save_refused_push();
   test_undo_save_during_write();
   test_undo_save_count();
   test_undo_save_twice();
   test_undo_save_during_backup();
   test_undo_save_not_backed_up();
   test_auto_state();
   test_auto_state_undo();
   test_auto_state_skip_tick();
   test_auto_state_keep();
   test_thumbnail_on_success();
   test_thumbnail_fails();
   test_thumbnail_kept_until_shot();
   test_thumbnail_other_moment();
   test_long_names();
   test_state_temps_distinct();
   test_ram_state_temp_distinct();
   test_ram_state_file();
#ifdef HAVE_COMPRESSION
   test_undo_save_restores_file();
#endif

   content_reset_savestate_backups();
   task_queue_deinit();
   free(core_mem);
   core_mem = NULL;
   return failures;
}

static int run_threaded_lane(void)
{
   printf("== task_save.c I/O regression oracle (threaded queue) ==\n\n");

   task_queue_init(true, msg_push_stub);

   test_threaded_roundtrip();
   test_threaded_load_waits_for_frames();
   test_auto_state_keep_threaded();
   test_auto_state_skip_tick_threaded();

   content_reset_savestate_backups();
   task_queue_deinit();
   free(core_mem);
   core_mem = NULL;
   return failures;
}

int main(int argc, char *argv[])
{
   if (argc > 1 && strcmp(argv[1], "conc") == 0)
      run_threaded_lane();
   else
      run_default_lane();

   printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
   return failures;
}
