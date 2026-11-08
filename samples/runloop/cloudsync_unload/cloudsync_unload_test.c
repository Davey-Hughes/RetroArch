/* Cloud sync on content unload, in the shipping frontend.
 *
 * With Cloud Sync in automatic mode, closing content asks for a sync
 * so the save the content leaves behind reaches the server.  The save
 * RAM only reaches disk when the content's deinit flushes it, which a
 * close from the menu runs a frame or more later, and until that
 * deinit finishes the save file belongs to the running core (the sync
 * defers anything it would write there).  A sync started before then
 * uploads, and hashes, whatever the file held last.
 *
 * This links the real retroarch.c and loads sram_core.so, writes a
 * pattern into the core's save RAM and closes the content through
 * CMD_EVENT_UNLOAD_CORE, running frames until the close completes.
 * task_push_cloud_sync is wrapped (-Wl,--wrap): at each call the
 * harness records what the save file holds and whether a core still
 * owns it or the content's auto state.  No server is contacted.
 *
 * cloud_sync_begin is wrapped too: a real sync task, pushed with
 * __real_task_push_cloud_sync, stays in flight until the harness ends
 * it (its begin fails), standing in for a slow startup sync.
 *
 * Lanes:
 *  launch cli  content from the command line while the startup sync
 *            runs: nothing waits for it, the saves load once it ends,
 *            from the file it left, and frames run meanwhile.
 *  unload    one sync, pushed once the file on disk is the core's
 *            last save RAM and no core owns it.
 *  no content  unloading with no content running syncs at once, as
 *            before: nothing waits to be flushed.
 *  launch menu  the same for content launched from the menu.
 *  close while held  closing content whose saves wait writes none.
 *  interval held  the timed auto state leaves .auto alone while held.
 *  rewind held  rewind starts from the loaded saves, not the held core.
 *  states held  Save State and Load State are refused while held.
 *  replays held  replay record and playback are refused while held.
 *  host held  netplay hosting is refused while held.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

#include <boolean.h>

#include "../../../runloop.h"
#include "../../../retroarch.h"
#include "../../../command.h"
#include "../../../content.h"
#include "../../../configuration.h"
#include "../../../frontend/frontend_driver.h"
#include "../../../input/input_driver.h"
#ifdef HAVE_REWIND
#include "../../../state_manager.h"
#endif
#include "../../../tasks/tasks_internal.h"
#include "../../../verbosity.h"

#include <time/rtime.h>
#include <retro_timers.h>
#include <string/stdstring.h>
#include <file/file_path.h>
#include <file/config_file.h>
#include <features/features_cpu.h>
#include <queues/task_queue.h>

#include "../../../network/cloud_sync_driver.h"
#ifdef HAVE_NETWORKING
#include "../../../network/netplay/netplay.h"
#endif
#include "../../../tasks/task_content.h"

static unsigned failures = 0;

#define CHECK(cond, ...) \
   do { \
      if (!(cond)) \
      { \
         fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
         fprintf(stderr, __VA_ARGS__); \
         fprintf(stderr, "\n"); \
         failures++; \
      } \
   } while (0)

/* ---- the wrapped push ---------------------------------------------- */

static char     srm_path[600];
static char     auto_path[600];
static uint8_t  pattern[256];
static size_t   pattern_size;
static unsigned n_syncs;
static bool     last_on_disk;
static bool     last_live;
static bool     last_auto_live;

static bool file_holds_pattern_of(const uint8_t *data)
{
   uint8_t buf[512];
   size_t  n;
   FILE   *f = fopen(srm_path, "rb");
   if (!f)
      return false;
   n = fread(buf, 1, sizeof(buf), f);
   fclose(f);
   return n == pattern_size && !memcmp(buf, data, pattern_size);
}

static bool file_holds_pattern(void)
{
   return file_holds_pattern_of(pattern);
}

static bool     real_syncs;
static unsigned blocking_pushes;
/* Unthreaded, the close's push cannot see the sync in flight, so two
 * can be alive at once. */
#define MAX_BEGINS 4
static cloud_sync_complete_handler_t begin_cb[MAX_BEGINS];
static void    *begin_ud[MAX_BEGINS];
static unsigned n_begins;

bool __real_task_push_cloud_sync(retro_task_callback_t cb, void *cb_data);

bool __wrap_task_push_cloud_sync(retro_task_callback_t cb, void *cb_data)
{
   char state_auto[PATH_MAX_LENGTH + 8];
   n_syncs++;
   last_on_disk   = file_holds_pattern();
   last_live      = content_savefile_is_live(srm_path);
   snprintf(state_auto, sizeof(state_auto), "%s.auto",
         runloop_state_get_ptr()->name.savestate);
   last_auto_live = content_savefile_is_live(state_auto);
   /* Done at once: a caller that waits for it goes on. */
   if (cb)
   {
      blocking_pushes++;
      cb(NULL, NULL, cb_data, NULL);
      return true;
   }
   if (real_syncs)
      return __real_task_push_cloud_sync(cb, cb_data);
   return true;
}

/* The sync's first call: kept until end_sync(). */
bool __wrap_cloud_sync_begin(cloud_sync_complete_handler_t cb, void *user_data)
{
   if (n_begins >= MAX_BEGINS)
      return false;
   begin_cb[n_begins]   = cb;
   begin_ud[n_begins++] = user_data;
   return true;
}

/* The oldest sync in flight ends, as a failed one does. */
static void end_sync(void)
{
   unsigned i;
   cloud_sync_complete_handler_t cb;
   void *ud;
   if (!n_begins)
      return;
   cb = begin_cb[0];
   ud = begin_ud[0];
   for (i = 1; i < n_begins; i++)
   {
      begin_cb[i - 1] = begin_cb[i];
      begin_ud[i - 1] = begin_ud[i];
   }
   n_begins--;
   cb(ud, NULL, false, NULL);
}

/* ---- helpers ------------------------------------------------------- */

static bool content_running(void)
{
   return (content_get_flags() & CONTENT_ST_FLAG_IS_INITED)
      && runloop_state_get_ptr()->current_core_type != CORE_TYPE_DUMMY;
}

static void *core_sym(const char *core_path, const char *name)
{
   void *core = dlopen(core_path, RTLD_NOW | RTLD_NOLOAD);
   void *sym  = core ? dlsym(core, name) : NULL;
   if (core)
      dlclose(core);
   return sym;
}

static bool sram_holds(const char *core_path, const uint8_t *data)
{
   uint8_t *(*get_sram)(void) = (uint8_t *(*)(void))core_sym(core_path,
         "harness_core_sram");
   return get_sram && !memcmp(get_sram(), data, pattern_size);
}

static unsigned core_runs(const char *core_path)
{
   unsigned (*runs)(void) = (unsigned (*)(void))core_sym(core_path,
         "harness_core_runs");
   return runs ? runs() : 0;
}

static void write_save(const uint8_t *data)
{
   FILE *f = fopen(srm_path, "wb");
   if (f)
   {
      fwrite(data, 1, pattern_size, f);
      fclose(f);
   }
}

/* Frames while the saves wait: each one presents promptly and the core
 * does not run. Returns the longest frame in microseconds. */
static retro_time_t run_held(const char *core_path, unsigned frames)
{
   unsigned i;
   unsigned runs        = core_runs(core_path);
   retro_time_t longest = 0;
   for (i = 0; i < frames; i++)
   {
      retro_time_t t = cpu_features_get_time_usec();
      runloop_iterate();
      task_queue_check();
      t = cpu_features_get_time_usec() - t;
      if (t > longest)
         longest = t;
   }
   CHECK(core_runs(core_path) == runs, "the core ran while its saves waited");
   return longest;
}

/* The sync lands what the server has (@data) and ends; the saves load
 * from it at a later frame. */
static void land_sync(const char *core_path, const uint8_t *data)
{
   unsigned i;
   write_save(data);
   end_sync();
   for (i = 0; i < 120 && !sram_holds(core_path, data); i++)
   {
      runloop_iterate();
      task_queue_check();
   }
}

static void run_until_closed(void)
{
   unsigned i;
   for (i = 0; i < 600; i++)
   {
      if (!content_running() && !runloop_state_get_ptr()->content_switching)
         return;
      /* One pass of the frontend's main loop. */
      runloop_iterate();
      task_queue_check();
   }
}

/* ---- lanes --------------------------------------------------------- */

static void lane_unload(const char *core_path)
{
   char     state_auto[PATH_MAX_LENGTH + 8];
   void    *core = dlopen(core_path, RTLD_NOW | RTLD_NOLOAD);
   uint8_t *(*get_sram)(void);
   uint8_t *sram;
   size_t   i;

   CHECK(content_running(), "the content did not load");
   CHECK(core != NULL, "sram_core.so is not loaded");
   if (!core || !content_running())
      return;
   get_sram = (uint8_t *(*)(void))dlsym(core, "harness_core_sram");
   CHECK(get_sram != NULL, "sram_core.so has no harness_core_sram");
   if (!get_sram)
      return;
   sram         = get_sram();
   pattern_size = 256;
   for (i = 0; i < pattern_size; i++)
      pattern[i] = (uint8_t)(0xA5 ^ i);
   memcpy(sram, pattern, pattern_size);
   dlclose(core);

   CHECK(content_savefile_is_live(srm_path),
         "the save file is not the running core's: %s", srm_path);
   snprintf(state_auto, sizeof(state_auto), "%s.auto",
         runloop_state_get_ptr()->name.savestate);
   CHECK(runloop_state_get_ptr()->name.savestate[0], "the content has no state name");
   CHECK(content_savefile_is_live(state_auto),
         "the auto state is not the running core's: %s", state_auto);

   n_syncs = 0;
   command_event(CMD_EVENT_UNLOAD_CORE, NULL);
   run_until_closed();

   CHECK(!content_running(), "the content did not close");
   CHECK(n_syncs == 1, "%u syncs on unload, expected 1", n_syncs);
   CHECK(last_on_disk,
         "the sync was pushed before the save RAM reached disk");
   CHECK(!last_live,
         "the sync was pushed while a core still owned the save file");
   CHECK(!last_auto_live,
         "the sync was pushed while the auto state was still owned");
   printf("[%s] unload\n", failures ? "FAIL" : "ok");
}

static uint8_t pre_sync[256], synced[256];

static void lane_launch_cli(const char *core_path)
{
   unsigned before = failures;
   CHECK(n_syncs == 1, "%u syncs before the content loaded, expected 1", n_syncs);
   CHECK(!last_live, "the startup sync was pushed after the core took the save file");
   CHECK(!last_auto_live,
         "the startup sync was pushed while the auto state was owned");
   CHECK(blocking_pushes == 0, "the command-line launch waited for the startup sync");
   CHECK(content_running(), "the content did not load");
   CHECK(!sram_holds(core_path, pre_sync),
         "the save was loaded before the startup sync ended");
   CHECK(!content_savefile_is_live(srm_path),
         "a save that waits for the sync is the core's: the sync would defer it");
   CHECK(run_held(core_path, 30) < 50000, "a frame took 50 ms or more while held");
   land_sync(core_path, synced);
   CHECK(sram_holds(core_path, synced), "the save was not loaded from the synced file");
   runloop_iterate();
   CHECK(core_runs(core_path) > 0, "the core does not run after its saves loaded");
   CHECK(content_savefile_is_live(srm_path), "the loaded save is not the core's");
   printf("[%s] launch cli\n", failures == before ? "ok" : "FAIL");
}

static bool launch_from_menu(const char *core_path, const char *content_path)
{
   unsigned i;
   content_ctx_info_t content_info = {0};
   if (!task_push_load_content_with_new_core_from_menu(core_path,
            content_path, &content_info, CORE_TYPE_PLAIN, NULL, NULL))
      return false;
   /* through its last stage (the drivers) as well */
   for (i = 0; i < 600 && (!content_running() || task_content_load_pending()); i++)
   {
      runloop_iterate();
      task_queue_check();
   }
   return content_running();
}

/* The startup sync rarch_main() pushes, still running. */
static void start_sync(void)
{
   real_syncs = true;
   task_push_cloud_sync(NULL, NULL);
   task_queue_check();
}

static void lane_launch_menu(const char *core_path, const char *content_path)
{
   unsigned before = failures;
   write_save(pre_sync);
   start_sync();
   CHECK(launch_from_menu(core_path, content_path), "the content did not load");
   CHECK(!sram_holds(core_path, pre_sync),
         "the pre-sync save was loaded while the startup sync ran");
   CHECK(!content_savefile_is_live(srm_path),
         "a save that waits for the sync is the core's: the sync would defer it");
   CHECK(run_held(core_path, 30) < 50000, "a frame took 50 ms or more while held");
   land_sync(core_path, synced);
   CHECK(sram_holds(core_path, synced), "the save was not loaded from the synced file");
   printf("[%s] launch menu\n", failures == before ? "ok" : "FAIL");
}

static void lane_close_while_held(const char *core_path, const char *content_path)
{
   unsigned i, before = failures;
   uint8_t *(*get_sram)(void);
   command_event(CMD_EVENT_UNLOAD_CORE, NULL);
   run_until_closed();
   write_save(pre_sync);
   start_sync();
   CHECK(launch_from_menu(core_path, content_path), "the content did not load");
   get_sram = (uint8_t *(*)(void))core_sym(core_path, "harness_core_sram");
   if (get_sram)
      memcpy(get_sram(), synced, pattern_size);
   command_event(CMD_EVENT_UNLOAD_CORE, NULL);
   run_until_closed();
   CHECK(!content_running(), "the content did not close");
   CHECK(file_holds_pattern_of(pre_sync),
         "a save that never loaded was written at the close");
   /* The held sync ends, then the one the close asked for. */
   for (i = 0; i < 10; i++)
   {
      end_sync();
      task_queue_check();
   }
   /* The next content is not held by the last one's sync. */
   real_syncs = false;
   CHECK(launch_from_menu(core_path, content_path), "the content did not load again");
   CHECK(sram_holds(core_path, pre_sync), "the next content's save did not load at once");
   command_event(CMD_EVENT_UNLOAD_CORE, NULL);
   run_until_closed();
   printf("[%s] close while held\n", failures == before ? "ok" : "FAIL");
}

static void write_auto(const char *data)
{
   FILE *f = fopen(auto_path, "wb");
   if (f)
   {
      fputs(data, f);
      fclose(f);
   }
}

static bool auto_holds(const char *data)
{
   char  buf[16];
   size_t n;
   FILE *f = fopen(auto_path, "rb");
   if (!f)
      return false;
   n = fread(buf, 1, sizeof(buf), f);
   fclose(f);
   return n == strlen(data) && !memcmp(buf, data, n);
}

/* savestate_automatic_interval writes the .auto the session would
 * resume from, the file the sync may be fetching. */
static void lane_interval_held(const char *core_path, const char *content_path)
{
   unsigned before      = failures;
   settings_t *settings = config_get_ptr();
   char state_auto[PATH_MAX_LENGTH + 8];
   write_save(pre_sync);
   write_auto("kept");
   configuration_set_uint(settings,
         settings->uints.savestate_automatic_interval, 1);
   start_sync();
   CHECK(launch_from_menu(core_path, content_path), "the content did not load");
   snprintf(state_auto, sizeof(state_auto), "%s.auto",
         runloop_state_get_ptr()->name.savestate);
   CHECK(string_is_equal(state_auto, auto_path),
         "the auto state is %s, not %s", state_auto, auto_path);
   run_held(core_path, 30);
   CHECK(auto_holds("kept"),
         "the timed auto state was written while the saves waited");
   land_sync(core_path, synced);
   CHECK(sram_holds(core_path, synced), "the save was not loaded from the synced file");
   runloop_iterate();
   CHECK(!auto_holds("kept"),
         "the timed auto state was not written once the saves loaded");
   configuration_set_uint(settings,
         settings->uints.savestate_automatic_interval, 0);
   real_syncs = false;
   command_event(CMD_EVENT_UNLOAD_CORE, NULL);
   run_until_closed();
   printf("[%s] interval held\n", failures == before ? "ok" : "FAIL");
}

#ifdef HAVE_REWIND
static void lane_rewind_held(const char *core_path, const char *content_path)
{
   unsigned i, t, before        = failures;
   char s[128];
   settings_t *settings         = config_get_ptr();
   runloop_state_t *runloop_st  = runloop_state_get_ptr();
   write_save(pre_sync);
   configuration_set_bool(settings, settings->bools.rewind_enable, true);
   configuration_set_uint(settings, settings->uints.rewind_granularity, 1);
   start_sync();
   CHECK(launch_from_menu(core_path, content_path), "the content did not load");
   run_held(core_path, 30);
   CHECK(!runloop_st->rewind_st.state,
         "rewind took states of the core while its saves waited");
   land_sync(core_path, synced);
   CHECK(sram_holds(core_path, synced), "the save was not loaded from the synced file");
   runloop_iterate();
   CHECK(runloop_st->rewind_st.state != NULL,
         "rewind did not start at the first frame after the saves loaded");
   for (i = 0; i < 10; i++)
      runloop_iterate();
   /* back past the first frame: the end of the buffer */
   for (i = 0; i < 200; i++)
      state_manager_check_rewind(&runloop_st->rewind_st,
            &runloop_st->current_core, true, 1, false, s, sizeof(s), &t);
   CHECK(sram_holds(core_path, synced),
         "rewinding to the start gave back the save RAM before the load");
   configuration_set_bool(settings, settings->bools.rewind_enable, false);
   real_syncs = false;
   command_event(CMD_EVENT_UNLOAD_CORE, NULL);
   run_until_closed();
   printf("[%s] rewind held\n", failures == before ? "ok" : "FAIL");
}
#endif

static uint8_t state_x[256];

/* Frames with time between them: a load asked for before the core's
 * second frame looks again after a few milliseconds. */
static void run_slow(unsigned frames)
{
   unsigned i;
   for (i = 0; i < frames; i++)
   {
      runloop_iterate();
      task_queue_check();
      retro_sleep(2);
   }
}

static void run_held_slow(const char *core_path, unsigned frames)
{
   unsigned i;
   for (i = 0; i < frames; i++)
   {
      run_held(core_path, 1);
      retro_sleep(2);
   }
}

static void run_until_file(const char *path)
{
   unsigned i;
   for (i = 0; i < 300 && !path_is_valid(path); i++)
      run_slow(1);
   /* and its task through */
   run_slow(10);
}

/* A state saved while held has no saves in it; one loaded is loaded
 * over when they load. */
static void lane_states_held(const char *core_path, const char *content_path)
{
   unsigned i, before   = failures;
   uint8_t held_sram[256];
   uint8_t *sram;
   char slot0[PATH_MAX_LENGTH], slot1[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();

   /* slot 0 holds state_x, from content that was not held */
   real_syncs = false;
   write_save(pre_sync);
   CHECK(launch_from_menu(core_path, content_path), "the content did not load");
   sram = (uint8_t*)((uint8_t *(*)(void))core_sym(core_path,
            "harness_core_sram"))();
   memcpy(sram, state_x, pattern_size);
   runloop_get_savestate_path(slot0, sizeof(slot0), 0);
   runloop_get_savestate_path(slot1, sizeof(slot1), 1);
   remove(slot0);
   remove(slot1);
   configuration_set_int(settings, settings->ints.state_slot, 0);
   command_event(CMD_EVENT_SAVE_STATE, NULL);
   run_until_file(slot0);
   CHECK(path_is_valid(slot0), "the state was not saved to %s", slot0);
   command_event(CMD_EVENT_UNLOAD_CORE, NULL);
   run_until_closed();

   write_save(pre_sync);
   start_sync();
   CHECK(launch_from_menu(core_path, content_path), "the content did not load");
   sram = (uint8_t*)((uint8_t *(*)(void))core_sym(core_path,
            "harness_core_sram"))();
   memcpy(held_sram, sram, pattern_size);
   command_event(CMD_EVENT_LOAD_STATE, NULL);
   run_held_slow(core_path, 50);
   CHECK(!memcmp(sram, held_sram, pattern_size),
         "a state was loaded while the saves waited");
   configuration_set_int(settings, settings->ints.state_slot, 1);
   command_event(CMD_EVENT_SAVE_STATE, NULL);
   run_held_slow(core_path, 50);
   CHECK(!path_is_valid(slot1), "a state was saved while the saves waited");
   land_sync(core_path, synced);
   CHECK(sram_holds(core_path, synced), "the save was not loaded from the synced file");

   command_event(CMD_EVENT_SAVE_STATE, NULL);
   run_until_file(slot1);
   CHECK(path_is_valid(slot1), "the state was not saved once the saves loaded");
   configuration_set_int(settings, settings->ints.state_slot, 0);
   command_event(CMD_EVENT_LOAD_STATE, NULL);
   for (i = 0; i < 300 && !sram_holds(core_path, state_x); i++)
      run_slow(1);
   CHECK(sram_holds(core_path, state_x),
         "the state was not loaded once the saves loaded");
   real_syncs = false;
   command_event(CMD_EVENT_UNLOAD_CORE, NULL);
   run_until_closed();
   printf("[%s] states held\n", failures == before ? "ok" : "FAIL");
}

#ifdef HAVE_BSV_MOVIE
static bool replay_playing(void)
{
   input_driver_state_t *input_st = input_state_get_ptr();
   return input_st->bsv_movie_state_handle
      || input_st->bsv_movie_state_next_handle
      || (input_st->bsv_movie_state.flags & BSV_FLAG_MOVIE_PLAYBACK);
}

/* A replay recorded while held starts from a core without its saves;
 * one played is loaded over when they load. */
static void lane_replays_held(const char *core_path, const char *content_path)
{
   unsigned i, before             = failures;
   uint8_t held_sram[256];
   uint8_t *sram;
   char slot0[PATH_MAX_LENGTH], slot1[PATH_MAX_LENGTH];
   settings_t *settings           = config_get_ptr();
   input_driver_state_t *input_st = input_state_get_ptr();

   configuration_set_bool(settings, settings->bools.replay_auto_index, false);

   /* slot 0 holds a replay from state_x, from content that was not held */
   real_syncs = false;
   write_save(pre_sync);
   CHECK(launch_from_menu(core_path, content_path), "the content did not load");
   sram = (uint8_t*)((uint8_t *(*)(void))core_sym(core_path,
            "harness_core_sram"))();
   memcpy(sram, state_x, pattern_size);
   runloop_get_replay_path(slot0, sizeof(slot0), 0);
   runloop_get_replay_path(slot1, sizeof(slot1), 1);
   remove(slot0);
   remove(slot1);
   configuration_set_int(settings, settings->ints.replay_slot, 0);
   command_event(CMD_EVENT_RECORD_REPLAY, NULL);
   run_until_file(slot0);
   CHECK(path_is_valid(slot0)
         && (input_st->bsv_movie_state.flags & BSV_FLAG_MOVIE_RECORDING),
         "the replay was not recorded to %s", slot0);
   command_event(CMD_EVENT_HALT_REPLAY, NULL);
   command_event(CMD_EVENT_UNLOAD_CORE, NULL);
   run_until_closed();

   write_save(pre_sync);
   start_sync();
   CHECK(launch_from_menu(core_path, content_path), "the content did not load");
   sram = (uint8_t*)((uint8_t *(*)(void))core_sym(core_path,
            "harness_core_sram"))();
   memcpy(held_sram, sram, pattern_size);
   command_event(CMD_EVENT_PLAY_REPLAY, NULL);
   run_held_slow(core_path, 50);
   CHECK(!replay_playing() && !memcmp(sram, held_sram, pattern_size),
         "a replay was played while the saves waited");
   configuration_set_int(settings, settings->ints.replay_slot, 1);
   command_event(CMD_EVENT_RECORD_REPLAY, NULL);
   run_held_slow(core_path, 50);
   CHECK(!path_is_valid(slot1), "a replay was recorded while the saves waited");
   land_sync(core_path, synced);
   CHECK(sram_holds(core_path, synced), "the save was not loaded from the synced file");

   command_event(CMD_EVENT_RECORD_REPLAY, NULL);
   run_until_file(slot1);
   CHECK(path_is_valid(slot1), "the replay was not recorded once the saves loaded");
   command_event(CMD_EVENT_HALT_REPLAY, NULL);
   configuration_set_int(settings, settings->ints.replay_slot, 0);
   command_event(CMD_EVENT_PLAY_REPLAY, NULL);
   for (i = 0; i < 300 && !sram_holds(core_path, state_x); i++)
      run_slow(1);
   CHECK(replay_playing() && sram_holds(core_path, state_x),
         "the replay was not played once the saves loaded");
   command_event(CMD_EVENT_HALT_REPLAY, NULL);
   real_syncs = false;
   command_event(CMD_EVENT_UNLOAD_CORE, NULL);
   run_until_closed();
   printf("[%s] replays held\n", failures == before ? "ok" : "FAIL");
}
#endif

#ifdef HAVE_NETWORKING
/* A lockstep host started while held is ended by the release's netplay
 * init, which finds it running. */
static void lane_host_held(const char *core_path, const char *content_path)
{
   unsigned before      = failures;
   settings_t *settings = config_get_ptr();
   bool held;
   configuration_set_bool(settings, settings->bools.netplay_lockstep, true);
   configuration_set_bool(settings, settings->bools.netplay_public_announce,
         false);
   configuration_set_bool(settings, settings->bools.netplay_nat_traversal,
         false);
   configuration_set_bool(settings, settings->bools.netplay_use_mitm_server,
         false);
   configuration_set_uint(settings, settings->uints.netplay_port,
         40000 + (unsigned)(getpid() % 20000));
   write_save(pre_sync);
   start_sync();
   CHECK(launch_from_menu(core_path, content_path), "the content did not load");
   /* not hosting for real when the hold is broken */
   held = runloop_state_get_ptr()->save_load_hold != 0;
   CHECK(held, "the saves did not wait for the sync");
   if (held)
   {
      CHECK(!command_event(CMD_EVENT_NETPLAY_ENABLE_HOST, NULL),
            "hosting started while the saves waited");
      run_held(core_path, 10);
      CHECK(!netplay_driver_ctl(RARCH_NETPLAY_CTL_IS_ENABLED, NULL)
            && !netplay_driver_ctl(RARCH_NETPLAY_CTL_IS_DATA_INITED, NULL),
            "netplay is on while the saves wait");
   }
   land_sync(core_path, synced);
   CHECK(sram_holds(core_path, synced), "the save was not loaded from the synced file");
   runloop_iterate();
   CHECK(!netplay_driver_ctl(RARCH_NETPLAY_CTL_IS_ENABLED, NULL),
         "netplay is on after the release with no host asked for");
   command_event(CMD_EVENT_NETPLAY_DEINIT, NULL);
   configuration_set_bool(settings, settings->bools.netplay_lockstep, false);
   real_syncs = false;
   command_event(CMD_EVENT_UNLOAD_CORE, NULL);
   run_until_closed();
   printf("[%s] host held\n", failures == before ? "ok" : "FAIL");
}
#endif

/* A sync the pause refused (a stand-in save folder) is not in flight,
 * so it holds no load. */
static void lane_launch_paused(const char *core_path, const char *content_path)
{
   unsigned before = failures;
   write_save(pre_sync);
   task_cloud_sync_set_paused(true);
   start_sync();
   real_syncs = false;
   CHECK(launch_from_menu(core_path, content_path), "the content did not load");
   CHECK(sram_holds(core_path, pre_sync),
         "the save load waited for a sync the pause refused");
   task_cloud_sync_set_paused(false);
   command_event(CMD_EVENT_UNLOAD_CORE, NULL);
   run_until_closed();
   printf("[%s] launch paused\n", failures == before ? "ok" : "FAIL");
}

static void lane_no_content(void)
{
   unsigned before = failures;
   n_syncs = 0;
   command_event(CMD_EVENT_UNLOAD_CORE, NULL);
   CHECK(n_syncs == 1, "%u syncs on an unload with no content, expected 1 at once",
         n_syncs);
   run_until_closed();
   CHECK(n_syncs == 1, "%u syncs after the frames that follow, expected 1",
         n_syncs);
   printf("[%s] no content\n", failures == before ? "ok" : "FAIL");
}

int main(int argc, char *argv[])
{
   char cfg_path[512];
   char content_path[512];
   char core_path[1024];
   char cmd[700];
   char dir[400];
   char *rarch_argv[8];
   int rarch_argc = 0;
   size_t i;
   FILE *f;
   const char *self = argc > 0 ? argv[0] : "cloudsync_unload_test";
   const char *slash;

   /* sram_core.so sits next to the harness. */
   {
      char cwd[400];
      int  dir_len = (slash = strrchr(self, '/')) ? (int)(slash - self) : 1;
      const char *self_dir = slash ? self : ".";
      if (self_dir[0] == '/')
         snprintf(core_path, sizeof(core_path), "%.*s/sram_core.so",
               dir_len, self_dir);
      else if (getcwd(cwd, sizeof(cwd)))
         snprintf(core_path, sizeof(core_path), "%s/%.*s/sram_core.so",
               cwd, dir_len, self_dir);
      else
         return 1;
   }

   snprintf(dir, sizeof(dir), "/tmp/cloudsync_unload_%ld", (long)getpid());
   snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir);
   if (system(cmd) != 0)
      return 1;

   snprintf(content_path, sizeof(content_path), "%s/game.bin", dir);
   snprintf(srm_path, sizeof(srm_path), "%s/game.srm", dir);
   snprintf(auto_path, sizeof(auto_path), "%s/game.state.auto", dir);
   pattern_size = 256;
   for (i = 0; i < pattern_size; i++)
   {
      pre_sync[i] = (uint8_t)(0x11 + i);
      synced[i]   = (uint8_t)(0x77 ^ i);
      state_x[i]  = (uint8_t)(0x3C + 3 * i);
   }
   write_save(pre_sync);
   if ((f = fopen(content_path, "wb")))
   {
      fputs("game", f);
      fclose(f);
   }

   snprintf(cfg_path, sizeof(cfg_path), "%s/harness.cfg", dir);
   if ((f = fopen(cfg_path, "wb")))
   {
      fprintf(f, "video_driver = \"null\"\n");
      fprintf(f, "audio_driver = \"null\"\n");
      fprintf(f, "audio_enable = \"false\"\n");
      fprintf(f, "microphone_enable = \"false\"\n");
      fprintf(f, "pause_nonactive = \"false\"\n");
      fprintf(f, "input_driver = \"null\"\n");
      fprintf(f, "input_joypad_driver = \"test\"\n");
      fprintf(f, "menu_driver = \"rgui\"\n");
      fprintf(f, "video_threaded = \"false\"\n");
      fprintf(f, "config_save_on_exit = \"false\"\n");
      fprintf(f, "savefile_directory = \"%s\"\n", dir);
      fprintf(f, "savestate_directory = \"%s\"\n", dir);
      fprintf(f, "sort_savefiles_enable = \"false\"\n");
      fprintf(f, "sort_savefiles_by_content_enable = \"false\"\n");
      fprintf(f, "sort_savestates_enable = \"false\"\n");
      fprintf(f, "sort_savestates_by_content_enable = \"false\"\n");
      fprintf(f, "savestate_auto_save = \"false\"\n");
      fprintf(f, "save_file_compression = \"false\"\n");
      fprintf(f, "autosave_interval = \"0\"\n");
      fprintf(f, "cloud_sync_enable = \"true\"\n");
      fprintf(f, "cloud_sync_sync_mode = \"0\"\n");
      fclose(f);
   }

   config_file_set_io_default(config_file_io_filestream());
   rtime_init();
   retroarch_config_init();
   retroarch_ctl(RARCH_CTL_STATE_FREE, NULL);
   frontend_driver_init_first(NULL);

   rarch_argv[rarch_argc++] = (char*)"retroarch";
   rarch_argv[rarch_argc++] = (char*)"--config";
   rarch_argv[rarch_argc++] = cfg_path;
   rarch_argv[rarch_argc++] = (char*)"-L";
   rarch_argv[rarch_argc++] = core_path;
   rarch_argv[rarch_argc++] = content_path;
   if (getenv("HARNESS_VERBOSE"))
      rarch_argv[rarch_argc++] = (char*)"-v";

   real_syncs = true;

   if (!retroarch_main_init(rarch_argc, rarch_argv))
   {
      fprintf(stderr, "FAIL: retroarch_main_init failed\n");
      return 1;
   }

   lane_launch_cli(core_path);
   real_syncs = false;
   lane_unload(core_path);
   lane_no_content();
   lane_launch_menu(core_path, content_path);
   lane_close_while_held(core_path, content_path);
   lane_launch_paused(core_path, content_path);
   lane_interval_held(core_path, content_path);
#ifdef HAVE_REWIND
   lane_rewind_held(core_path, content_path);
#endif
   lane_states_held(core_path, content_path);
#ifdef HAVE_BSV_MOVIE
   lane_replays_held(core_path, content_path);
#endif
#ifdef HAVE_NETWORKING
   lane_host_held(core_path, content_path);
#endif

   snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
   if (system(cmd) != 0) { }

   if (failures)
   {
      fprintf(stderr, "FAIL cloudsync_unload_test: %u failures\n", failures);
      return 1;
   }
   printf("cloudsync_unload_test: all lanes pass\n");
   return 0;
}
