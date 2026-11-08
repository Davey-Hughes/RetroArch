/* Include the shipping loop to exercise its private deferred dispatcher. */
#include "../../../runloop.c"
#include <assert.h>
#include <file/file_path.h>
#include <lists/dir_list.h>
#include <streams/file_stream.h>
#include <vfs/vfs_implementation.h>

static settings_t test_settings;
static video_driver_state_t test_video;
static unsigned runs, size_checks, loads, settings_checks, path_checks;
static unsigned initialize_after;
#ifdef HAVE_NETWORKING
static bool allow_frame = true;
bool netplay_driver_ctl(enum rarch_netplay_ctl_state state, void *data)
{
   (void)data;
   return state == RARCH_NETPLAY_CTL_PRE_FRAME ? allow_frame : false;
}
#endif
#ifdef HAVE_CHEEVOS
static bool hardcore;
bool rcheevos_hardcore_active(void) { return hardcore; }
#endif

settings_t *config_get_ptr(void)
{
   settings_checks++;
   return &test_settings;
}

video_driver_state_t *video_state_get_ptr(void) { return &test_video; }
void input_driver_poll(void) {}
void input_driver_poll_between_frames(void (*poll)(void)) { poll(); }
void video_driver_cached_frame(void) {}
void audio_driver_frame_end(void) {}
void recording_start_frame(void) {}
void audio_driver_publish_runloop(void) {}
void RARCH_LOG(const char *fmt, ...) { (void)fmt; }
void RARCH_DBG(const char *fmt, ...) { (void)fmt; }
/* runloop_msg_queue_push() */
void msg_queue_push(msg_queue_t *queue, const char *msg, unsigned prio,
      unsigned duration, char *title, enum message_queue_icon icon,
      enum message_queue_category category)
{ (void)queue; (void)msg; (void)prio; (void)duration; (void)title;
  (void)icon; (void)category; }
size_t msg_queue_size(msg_queue_t *queue) { (void)queue; return 0; }
void msg_queue_clear(msg_queue_t *queue) { (void)queue; }
void ui_companion_driver_msg_queue_push(const char *msg, unsigned priority,
      unsigned duration, bool flush)
{ (void)msg; (void)priority; (void)duration; (void)flush; }
bool verbosity_is_enabled(void) { return false; }
/* core_run() loads held saves (runloop_content_saves_ready) */
bool event_load_save_files(bool is_sram_load_disabled)
{ (void)is_sram_load_disabled; return true; }
bool command_event(enum event_command cmd, void *data)
{ (void)cmd; (void)data; return true; }

static void mock_run(void) { runs++; }
static size_t mock_size(void)
{
   size_checks++;
   return runs >= initialize_after ? 16 : 0;
}

bool content_load_state(const char *path, bool load_to_backup, bool autoload)
{
   assert(strcmp(path, "test.auto") == 0);
   assert(!load_to_backup && autoload);
   loads++;
   return true;
}

bool path_is_valid(const char *path)
{
   if (strcmp(path, "test.auto"))
      return filestream_exists(path);
   path_checks++;
   return true;
}

/* file_path_io.c's, which would bring its own path_is_valid() */
int64_t path_get_size(const char *path)
{
   int64_t size = 0;
   return retro_vfs_stat_64_impl(path, &size) ? size : -1;
}
/* 0: the file's; 1: none, as smb, nfs and SAF can report; 2: 0, as a
 * stat that leaves st_mtime unset does */
static int mtime_mode;
bool path_get_mtime(const char *path, int64_t *mtime)
{
   if (mtime_mode == 1)
      return false;
   if (mtime_mode == 2)
   {
      *mtime = 0;
      return true;
   }
   return retro_vfs_get_mtime_impl(path, mtime) == 0;
}

/* The frontend's save writes, bumped where task_save.c bumps them */
static unsigned writes, in_flight;
unsigned content_save_writes(void) { return writes; }
/* Cloud sync's state downloads */
static unsigned fetches;
unsigned content_state_fetches(void) { return fetches; }
bool content_save_state_in_progress(void *data)
{
   (void)data;
   return in_flight != 0;
}

/* Slot saves: the save task is the test's */
static retro_task_callback_t save_done;
static void *save_user;
static char saved_path[PATH_MAX_LENGTH];
static unsigned saves;
bool content_save_state(const char *path, bool save_to_disk)
{
   (void)save_to_disk;
   strlcpy(saved_path, path, sizeof(saved_path));
   saves++;
   return true;
}
bool content_save_state_notify(const char *path, bool save_to_disk,
      retro_task_callback_t cb, void *user_data)
{
   save_done = cb;
   save_user = user_data;
   in_flight++;
   return content_save_state(path, save_to_disk);
}

/* The 3DS RAM state: written to its file at close */
static bool ram_pending;
/* written beside it, as the in-flight save's worker renames its file */
static const char *ram_meanwhile;
bool content_save_state_to_ram(void)
{
   ram_pending = true;
   return true;
}
bool content_ram_state_to_file(const char *path)
{
   const char *name = path_basename(path);
   if (!ram_pending)
      return false;
   ram_pending = false;
   if (ram_meanwhile)
      assert(filestream_write_file(ram_meanwhile, path_basename(ram_meanwhile),
            (int64_t)strlen(path_basename(ram_meanwhile))));
   if (!filestream_write_file(path, name, (int64_t)strlen(name)))
      return false;
   writes++;
   return true;
}
bool core_info_current_supports_savestate(void) { return true; }
struct string_list *dir_list_new_special(const char *input_dir,
      enum dir_list_type type, const char *filter, bool show_hidden_files)
{
   (void)type;
   return dir_list_new(input_dir, filter, false, show_hidden_files,
         false, false);
}

const char *msg_hash_to_str(enum msg_hash_enums msg)
{
   (void)msg;
   return "test";
}

static void reset(unsigned frames, bool quirk)
{
   memset(&runloop_state, 0, sizeof(runloop_state));
   memset(&test_settings, 0, sizeof(test_settings));
   memset(&test_video, 0, sizeof(test_video));
   test_settings.bools.savestate_auto_load = true;
   runloop_state.current_core.flags = RETRO_CORE_FLAG_GAME_LOADED;
   runloop_state.current_core.retro_run = mock_run;
   runloop_state.current_core.retro_serialize_size = mock_size;
   runloop_state.current_core.serialization_quirks_v = quirk
      ? RETRO_SERIALIZATION_QUIRK_MUST_INITIALIZE : 0;
   strcpy(runloop_state.name.savestate, "test");
   runs = size_checks = loads = settings_checks = path_checks = 0;
   initialize_after = frames;
}

#define STATES "asl_states"
/* Older than any write the test makes, so a rewrite of the same size
 * still differs */
#define OLD_MTIME 1000000000

static void write_state(const char *name, const char *data, int64_t mtime)
{
   char path[PATH_MAX_LENGTH];
   fill_pathname_join(path, STATES, name, sizeof(path));
   assert(filestream_write_file(path, data, (int64_t)strlen(data)));
   if (mtime)
      assert(retro_vfs_set_mtime_impl(path, mtime) == 0);
}

static void put(const char *name) { write_state(name, name, 0); }
static void put_old(const char *name) { write_state(name, name, OLD_MTIME); }

static bool there(const char *name)
{
   char path[PATH_MAX_LENGTH];
   fill_pathname_join(path, STATES, name, sizeof(path));
   return filestream_exists(path);
}

static void states_clear(void)
{
   unsigned i;
   struct string_list *list = dir_list_new(STATES, NULL, false, true,
         false, false);
   for (i = 0; list && i < list->size; i++)
      filestream_delete(list->elems[i].data);
   dir_list_free(list);
}

static void states_reset(const char *game)
{
   states_clear();
   retro_vfs_mkdir_impl(STATES);
   reset(0, false);
   fill_pathname_join(runloop_state.name.savestate, STATES, game,
         sizeof(runloop_state.name.savestate));
   strlcat(runloop_state.name.savestate, ".state",
         sizeof(runloop_state.name.savestate));
   test_settings.bools.savestate_auto_index = true;
   test_settings.uints.savestate_max_keep   = 2;
   test_settings.ints.state_slot            = 0;
   save_done     = NULL;
   save_user     = NULL;
   saves         = 0;
   in_flight     = 0;
   ram_pending   = false;
   ram_meanwhile = NULL;
}

/* The RAM state written to @name now, as at close */
static void ram_write(const char *name)
{
   char path[PATH_MAX_LENGTH];
   fill_pathname_join(path, STATES, name, sizeof(path));
   ram_pending = true;
   assert(content_ram_state_to_file(path));
}

/* Ends the slot save the command pushed, once, as the task does */
static void save_finished(const char *error)
{
   retro_task_callback_t cb = save_done;
   save_done = NULL;
   if (in_flight)
      in_flight--;
   writes++;
   if (cb)
      cb(NULL, saved_path, save_user, error);
   save_user = NULL;
}

/* max_keep_on_success: nothing goes before the save is written */
static void lane_max_keep_on_success(void)
{
   states_reset("Game");
   put("Game.state1");
   put("Game.state2");
   put("Game.state1.png");
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE));
   assert(saves == 1);
   assert(there("Game.state1") && there("Game.state2"));
   save_finished("failed");
   assert(there("Game.state1") && there("Game.state2"));
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE));
   put("Game.state");
   save_finished(NULL);
   assert(there("Game.state") && !there("Game.state1")
         && !there("Game.state1.png") && there("Game.state2"));
}

/* max_keep_not_target: never the slot just written */
static void lane_max_keep_not_target(void)
{
   /* the lowest */
   states_reset("Game");
   put("Game.state");
   put("Game.state1");
   put("Game.state2");
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE));
   save_finished(NULL);
   assert(there("Game.state") && !there("Game.state1")
         && there("Game.state2"));

   /* over a slot, under the limit without it */
   states_reset("Game");
   test_settings.uints.savestate_max_keep = 3;
   test_settings.ints.state_slot          = 2;
   put("Game.state");
   put("Game.state2");
   put("Game.state3");
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE));
   save_finished(NULL);
   assert(there("Game.state") && there("Game.state2")
         && there("Game.state3"));
}

/* max_keep_victim_rewritten: the RAM state written at close lands in
 * the slot picked while the save was in flight */
static void lane_max_keep_victim_rewritten(void)
{
   states_reset("Game");
   test_settings.ints.state_slot = 2;
   put_old("Game.state");
   put_old("Game.state1");
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE));
   ram_write("Game.state");
   put("Game.state2");
   save_finished(NULL);
   assert(there("Game.state") && there("Game.state1")
         && there("Game.state2"));
}

/* max_keep_victim_fetched: a sync downloads the slot picked, the same
 * size, while the save is in flight */
static void lane_max_keep_victim_fetched(void)
{
   states_reset("Game");
   put_old("Game.state1");
   put_old("Game.state2");
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE));
   write_state("Game.state1", "Game.state9", 0);
   fetches++;
   put("Game.state");
   save_finished(NULL);
   assert(there("Game.state") && there("Game.state1")
         && there("Game.state2"));
}

/* max_keep_victim_resized: a download into the slot picked, keeping
 * the old mtime */
static void lane_max_keep_victim_resized(void)
{
   states_reset("Game");
   put_old("Game.state1");
   put_old("Game.state2");
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE));
   write_state("Game.state1", "Game.state1 from the server", OLD_MTIME);
   put("Game.state");
   save_finished(NULL);
   assert(there("Game.state") && there("Game.state1")
         && there("Game.state2"));
}

/* max_keep_refused: a save the hold turns away deletes nothing, now
 * or at a later save */
static void lane_max_keep_refused(void)
{
   states_reset("Game");
   put("Game.state1");
   put("Game.state2");
   runloop_state.save_load_hold = SAVE_LOAD_HOLD_CLOUD_SYNC;
   assert(!command_event_main_state(CMD_EVENT_SAVE_STATE));
   assert(saves == 0 && !save_done);
   assert(there("Game.state1") && there("Game.state2"));
   runloop_state.save_load_hold = 0;
   test_settings.uints.savestate_max_keep = 3;
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE));
   put("Game.state");
   save_finished(NULL);
   assert(there("Game.state") && there("Game.state1")
         && there("Game.state2"));

   /* max_keep_off_meanwhile: turned off before the save lands */
   states_reset("Game");
   put("Game.state1");
   put("Game.state2");
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE));
   test_settings.uints.savestate_max_keep = 0;
   put("Game.state");
   save_finished(NULL);
   assert(there("Game.state") && there("Game.state1")
         && there("Game.state2"));

   /* max_keep_raised_meanwhile: picked under the old limit */
   states_reset("Game");
   put("Game.state1");
   put("Game.state2");
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE));
   test_settings.uints.savestate_max_keep = 10;
   put("Game.state");
   save_finished(NULL);
   assert(there("Game.state") && there("Game.state1")
         && there("Game.state2"));
}

/* ram_state_max_keep: the RAM state's file keeps the limit too */
static void lane_ram_state_max_keep(void)
{
   char path[PATH_MAX_LENGTH];

   states_reset("Game");
   put_old("Game.state1");
   put_old("Game.state2");
   put_old("Game.state1.png");
   runloop_get_current_savestate_path(path, sizeof(path));
   assert(!command_event_ram_state_to_file(path));
   assert(!there("Game.state") && there("Game.state1")
         && there("Game.state1.png"));
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE_TO_RAM));
   assert(saves == 0);
   assert(command_event_ram_state_to_file(path));
   assert(there("Game.state") && !there("Game.state1")
         && !there("Game.state1.png") && there("Game.state2"));

   /* closed while a disk save is in flight */
   states_reset("Game");
   test_settings.ints.state_slot = 2;
   put_old("Game.state");
   put_old("Game.state1");
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE));
   test_settings.ints.state_slot = 0;
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE_TO_RAM));
   runloop_get_current_savestate_path(path, sizeof(path));
   assert(command_event_ram_state_to_file(path));
   put("Game.state2");
   save_finished(NULL);
   assert(there("Game.state") && there("Game.state1")
         && there("Game.state2"));

   /* the in-flight save lands in the slot the RAM state picked */
   states_reset("Game");
   test_settings.ints.state_slot = 1;
   put_old("Game.state1");
   put_old("Game.state2");
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE));
   test_settings.ints.state_slot = 0;
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE_TO_RAM));
   runloop_get_current_savestate_path(path, sizeof(path));
   ram_meanwhile = saved_path;
   assert(command_event_ram_state_to_file(path));
   save_finished(NULL);
   assert(there("Game.state") && there("Game.state1")
         && there("Game.state2"));
}

/* max_keep_exact_name: two names sharing their first 127 chars */
static void lane_max_keep_exact_name(void)
{
   char game[160];

   memset(game, 'T', 140);
   strcpy(game + 140, " (A)");
   states_reset(game);
   strcpy(game + 140, " (B).state");
   put(game);
   strcpy(game + 140, " (B).state1");
   put(game);
   strcpy(game + 140, " (B).state2");
   put(game);
   strcpy(game + 140, " (A).state1");
   put(game);
   strcpy(game + 140, " (A).state2");
   put(game);
   strcpy(game + 140, " (A).state.auto");
   put(game);
   assert(command_event_main_state(CMD_EVENT_SAVE_STATE));
   strcpy(game + 140, " (A).state");
   put(game);
   save_finished(NULL);
   strcpy(game + 140, " (A).state1");
   assert(!there(game));
   strcpy(game + 140, " (A).state2");
   assert(there(game));
   strcpy(game + 140, " (A).state.auto");
   assert(there(game));
   strcpy(game + 140, " (B).state");
   assert(there(game));
   strcpy(game + 140, " (B).state1");
   assert(there(game));
   strcpy(game + 140, " (B).state2");
   assert(there(game));
}

int main(void)
{
   unsigned i;
   for (mtime_mode = 0; mtime_mode < 3; mtime_mode++)
   {
      printf("max_keep, mtime mode %d\n", mtime_mode);
      fflush(stdout);
      lane_max_keep_on_success();
      lane_max_keep_not_target();
      lane_max_keep_victim_rewritten();
      lane_max_keep_victim_fetched();
      lane_max_keep_victim_resized();
      lane_max_keep_refused();
      lane_ram_state_max_keep();
      lane_max_keep_exact_name();
   }
   mtime_mode = 0;
   states_clear();
   filestream_delete(STATES);
   puts("max_keep tests passed");
   reset(3, true);
   assert(command_event_load_auto_state());
   runloop_load_deferred_auto_state();
   assert(loads == 0 && size_checks == 0);
   for (i = 0; i < 3; i++)
   {
      core_run();
      runloop_load_deferred_auto_state();
      assert(loads == (i == 2 ? 1U : 0U));
      assert(runloop_state.auto_state_load_pending == (i != 2));
      assert(path_checks == (i == 2 ? 2U : 1U));
   }
   assert(size_checks == 3);
   settings_checks = 0;
   for (i = 0; i < 10; i++)
   {
      core_run();
      runloop_load_deferred_auto_state();
   }
   assert(loads == 1 && size_checks == 3 && settings_checks == 0);
   assert(!command_event_load_auto_state());
   assert(loads == 1 && path_checks == 2);

   reset(1, true);
   assert(command_event_load_auto_state());
   runloop_state.current_core.retro_run = NULL;
   core_run();
   runloop_load_deferred_auto_state();
   assert(loads == 0 && size_checks == 0);

   reset(3, true);
   assert(command_event_load_auto_state());
   test_settings.bools.savestate_auto_load = false;
   runloop_load_deferred_auto_state();
   assert(!runloop_state.auto_state_load_pending && loads == 0);

   reset(3, true);
   assert(command_event_load_auto_state());
   runloop_state.content_closing = true;
   core_run();
   runloop_load_deferred_auto_state();
   assert(runs == 0 && loads == 0 && !runloop_state.auto_state_load_pending);

   reset(3, true);
   assert(command_event_load_auto_state());
   runloop_state.current_core.flags = 0;
   runloop_load_deferred_auto_state();
   assert(loads == 0 && !runloop_state.auto_state_load_pending);

   reset(3, true);
   assert(command_event_load_auto_state());
   core_run();
   runloop_reset_auto_state_load(&runloop_state);
   assert(!runloop_state.auto_state_load_pending);
   assert(!runloop_state.auto_state_load_attempted);
   assert(!runloop_state.auto_state_load_ready);
   assert(!runloop_state.auto_state_load_core_ran);
   assert(command_event_load_auto_state());
   runloop_load_deferred_auto_state();
   assert(loads == 0 && size_checks == 0);

   /* Ordinary cores may support loading without reporting a state size. */
   reset(100, false);
   assert(command_event_load_auto_state());
   assert(loads == 1 && size_checks == 0);

#ifdef HAVE_NETWORKING
   reset(1, true);
   assert(command_event_load_auto_state());
   allow_frame = false;
   core_run();
   runloop_load_deferred_auto_state();
   assert(runs == 0 && loads == 0 && size_checks == 0);
   assert(runloop_state.auto_state_load_pending);
   allow_frame = true;
   core_run();
   runloop_load_deferred_auto_state();
   assert(loads == 1);
#endif
#ifdef HAVE_CHEEVOS
   reset(1, true);
   hardcore = true;
   assert(!command_event_load_auto_state());
   assert(!runloop_state.auto_state_load_pending && loads == 0);
   hardcore = false;
#endif
   puts("auto-state initialization tests passed");
   return 0;
}
