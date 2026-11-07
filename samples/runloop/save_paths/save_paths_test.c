/* Where runloop_path_set_redirect() puts a game's saves and states.
 *
 * With sort-by-content on, the content folder's name becomes a folder
 * under the save and state roots, except "backups", "records" and
 * "system" in any case, which the roots keep for safety copies, records
 * and the system folder's copies: those games use the root (and the
 * core's folder when that is on).
 *
 * A netplay client whose ".netplay" folder can't be made writes no
 * saves for that content, says so once, and the next content saves
 * again. The flags that gate the writes stand in for them here;
 * save_e2e's nosave_netplay_disconnect lane counts the writes.
 *
 * A save asked for from another thread (3DS's suspend hook) is made
 * once, at the main thread's next frame.
 *
 * The shipping function is included and run over a real folder. */
#include "../../../runloop.c"
#include "../../../defaults.h"
#include <stdio.h>
#include <rthreads/rthreads.h>

#define ROOT "save_paths_test_files"

static int failures;
static settings_t test_settings;

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

void RARCH_LOG(const char *fmt, ...) { (void)fmt; }
void RARCH_WARN(const char *fmt, ...) { (void)fmt; }
void RARCH_ERR(const char *fmt, ...) { (void)fmt; }
bool verbosity_is_enabled(void) { return false; }
static int fallback_notices;
static int missing_notices;
const char *msg_hash_to_str(enum msg_hash_enums msg)
{
   switch (msg)
   {
      case MSG_SAVE_FOLDER_FALLBACK:
         fallback_notices++;
         return "save fallback %s %s";
      case MSG_SAVE_FOLDER_MISSING:
         missing_notices++;
         return "save missing %s";
      case MSG_STATE_FOLDER_FALLBACK:
         return "state fallback %s %s";
      case MSG_STATE_FOLDER_MISSING:
         return "state missing %s";
      case MSG_NETPLAY_SAVES_OFF:
         return "saves off";
      default:
         break;
   }
   return "";
}
struct defaults g_defaults;
/* The configured roots; "" is the default, as in the config file. */
static char cfg_saves[DIR_MAX_LENGTH];
static char cfg_states[DIR_MAX_LENGTH];
/* The roots in use, as the check sets them. */
static char live_saves[DIR_MAX_LENGTH];
static char live_states[DIR_MAX_LENGTH];
void dir_set(enum rarch_dir_type type, const char *path)
{
   if (type == RARCH_DIR_SAVEFILE)
      strlcpy(live_saves, path, sizeof(live_saves));
   else if (type == RARCH_DIR_SAVESTATE)
      strlcpy(live_states, path, sizeof(live_states));
}
char *dir_get_ptr(enum rarch_dir_type type)
{
   switch (type)
   {
      case RARCH_DIR_SAVESTATE_CONFIGURED:
         return cfg_states;
      case RARCH_DIR_SAVEFILE:
         return live_saves;
      case RARCH_DIR_SAVESTATE:
         return live_states;
      default:
         break;
   }
   return cfg_saves;
}
bool retroarch_override_setting_is_set(
      enum rarch_override_setting enum_idx, void *data)
{
   (void)enum_idx;
   (void)data;
   return false;
}

static bool netplay_client;
bool netplay_driver_ctl(enum rarch_netplay_ctl_state state, void *data)
{
   (void)data;
   switch (state)
   {
      case RARCH_NETPLAY_CTL_IS_ENABLED:
         return netplay_client;
      default:
         break;
   }
   return false;
}
static int saves_off_notices;
static char last_notice[DIR_MAX_LENGTH * 2 + 64];
void ui_companion_driver_msg_queue_push(const char *msg, unsigned priority,
      unsigned duration, bool flush)
{
   (void)priority; (void)duration; (void)flush;
   if (string_is_equal(msg, "saves off"))
      saves_off_notices++;
   strlcpy(last_notice, msg, sizeof(last_notice));
}
void path_deinit_savefile(void) { }
void path_init_savefile_new(void) { }
void path_init_savefile_rtc(const char *savefile_path) { (void)savefile_path; }

/* Equal but for which slash separates folders. */
static bool same_path(const char *a, const char *b)
{
   for (; *a && *b; a++, b++)
      if (*a != *b && !(PATH_CHAR_IS_SLASH(*a) && PATH_CHAR_IS_SLASH(*b)))
         return false;
   return !*a && !*b;
}

static void redirect(const char *content, bool per_core)
{
   memset(&runloop_state, 0, sizeof(runloop_state));
   memset(&test_settings, 0, sizeof(test_settings));
   test_settings.bools.sort_savefiles_by_content_enable  = true;
   test_settings.bools.sort_savestates_by_content_enable = true;
   test_settings.bools.sort_savefiles_enable             = per_core;
   test_settings.bools.sort_savestates_enable            = per_core;
   runloop_state.system.info.library_name = "Snes9x";
   strlcpy(runloop_state.runtime_content_path_basename, content,
         sizeof(runloop_state.runtime_content_path_basename));
   runloop_path_set_redirect(&test_settings, ROOT "/saves", ROOT "/states");
}

static void rm_tree(const char *dir)
{
   size_t i;
   struct string_list *l = dir_list_new(dir, NULL, true, true, false, false);
   if (l)
   {
      for (i = 0; i < l->size; i++)
      {
         if (path_is_directory(l->elems[i].data))
            rm_tree(l->elems[i].data);
         else
            filestream_delete(l->elems[i].data);
      }
      string_list_free(l);
   }
   path_rmdir(dir);
}

static void reserved_rom_folder(void)
{
   redirect("/roms/snes/Game", false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/snes/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate, ROOT "/states/snes/Game.state"));

   redirect("/roms/backups/Game", false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate, ROOT "/states/Game.state"));

   redirect("/roms/Records/Game", false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate, ROOT "/states/Game.state"));

   redirect("/roms/BACKUPS/Game", true);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/Snes9x/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate, ROOT "/states/Snes9x/Game.state"));

   redirect("/roms/system/Game", false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate, ROOT "/states/Game.state"));

   redirect("/roms/System/Game", true);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/Snes9x/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate, ROOT "/states/Snes9x/Game.state"));

   redirect("/roms/backups2/Game", false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/backups2/Game.srm"));

   CHECK(!path_is_directory(ROOT "/saves/backups"));
   CHECK(!path_is_directory(ROOT "/saves/Records"));
   CHECK(!path_is_directory(ROOT "/states/backups"));
   CHECK(!path_is_directory(ROOT "/states/Records"));
   CHECK(!path_is_directory(ROOT "/saves/BACKUPS"));
   CHECK(!path_is_directory(ROOT "/states/BACKUPS"));
   CHECK(!path_is_directory(ROOT "/saves/system"));
   CHECK(!path_is_directory(ROOT "/states/system"));
   CHECK(!path_is_directory(ROOT "/saves/System"));
   CHECK(!path_is_directory(ROOT "/states/System"));
}

/* A content load: the paths, then the save list. */
static void load(const char *content)
{
   runloop_path_set_redirect(&test_settings, ROOT "/saves", ROOT "/states");
   runloop_state.flags |= RUNLOOP_FLAG_USE_SRAM;
   strlcpy(runloop_state.runtime_content_path_basename, content,
         sizeof(runloop_state.runtime_content_path_basename));
   runloop_path_set_redirect(&test_settings, ROOT "/saves", ROOT "/states");
   runloop_path_init_savefile(&runloop_state);
}

static void netplay_no_folder(void)
{
   CHECK(path_mkdir(ROOT "/saves/snes"));
   CHECK(filestream_write_file(ROOT "/saves/snes/.netplay", "x", 1));

   redirect("/roms/snes/Game", false);
   netplay_client    = true;
   saves_off_notices = 0;
   load("/roms/snes/Game");
   CHECK(runloop_state.flags & RUNLOOP_FLAG_IS_SRAM_SAVE_DISABLED);
   CHECK(!(runloop_state.flags & RUNLOOP_FLAG_USE_SRAM));
   CHECK(saves_off_notices == 1);

   /* The next content, not under netplay, saves again. */
   netplay_client = false;
   load("/roms/snes/Game");
   CHECK(!(runloop_state.flags & RUNLOOP_FLAG_IS_SRAM_SAVE_DISABLED));
   CHECK(runloop_state.flags & RUNLOOP_FLAG_USE_SRAM);
   CHECK(saves_off_notices == 1);

   /* --sram-mode's own setting outlives the netplay session. */
   runloop_state.flags |= RUNLOOP_FLAG_IS_SRAM_SAVE_DISABLED;
   netplay_client       = true;
   load("/roms/snes/Game");
   netplay_client       = false;
   load("/roms/snes/Game");
   CHECK(runloop_state.flags & RUNLOOP_FLAG_IS_SRAM_SAVE_DISABLED);
   CHECK(saves_off_notices == 1);

   /* A folder that can be made: saves on. */
   CHECK(filestream_delete(ROOT "/saves/snes/.netplay") == 0);
   runloop_state.flags &= ~RUNLOOP_FLAG_IS_SRAM_SAVE_DISABLED;
   netplay_client       = true;
   load("/roms/snes/Game");
   CHECK(path_is_directory(ROOT "/saves/snes/.netplay"));
   CHECK(!(runloop_state.flags & RUNLOOP_FLAG_IS_SRAM_SAVE_DISABLED));
   CHECK(saves_off_notices == 1);
   netplay_client       = false;
}

/* The configured roots are checked at each load: a missing folder is
 * made again when its parent is there; a missing parent (a card or a
 * share not there) uses the default folder for that load with one
 * notice, and makes nothing under it; with no default either, saves
 * are off for that load. */
static void folder_check_each_load(void)
{
   strlcpy(cfg_saves, ROOT "/card/RetroArch/saves", sizeof(cfg_saves));
   strlcpy(cfg_states, ROOT "/card/RetroArch/states", sizeof(cfg_states));
   strlcpy(g_defaults.dirs[DEFAULT_DIR_SRAM], ROOT "/default/saves",
         sizeof(g_defaults.dirs[DEFAULT_DIR_SRAM]));
   strlcpy(g_defaults.dirs[DEFAULT_DIR_SAVESTATE], ROOT "/default/states",
         sizeof(g_defaults.dirs[DEFAULT_DIR_SAVESTATE]));
   CHECK(path_mkdir(ROOT "/card/RetroArch/saves"));
   CHECK(path_mkdir(ROOT "/card/RetroArch/states"));
   CHECK(path_mkdir(ROOT "/default/saves"));
   CHECK(path_mkdir(ROOT "/default/states"));
   fallback_notices = missing_notices = 0;

   redirect("/roms/snes/Game", false);
   load("/roms/snes/Game");
   CHECK(same_path(runloop_state.name.savefile,
            ROOT "/card/RetroArch/saves/snes/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/card/RetroArch/states/snes/Game.state"));
   CHECK(fallback_notices == 0);

   /* The card goes away before the next load. */
   rm_tree(ROOT "/card/RetroArch");
   last_notice[0] = '\0';
   load("/roms/snes/Game");
   CHECK(same_path(runloop_state.name.savefile,
            ROOT "/default/saves/snes/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/default/states/snes/Game.state"));
   CHECK(fallback_notices == 1);
   CHECK(same_path(last_notice, "save fallback "
            ROOT "/card/RetroArch/saves " ROOT "/default/saves"));
   CHECK(runloop_state.flags & RUNLOOP_FLAG_USE_SRAM);
   CHECK(!path_is_directory(ROOT "/card/RetroArch"));

   /* Back, but only its parent: the folders are made again. */
   CHECK(path_mkdir(ROOT "/card/RetroArch"));
   load("/roms/snes/Game");
   CHECK(same_path(runloop_state.name.savefile,
            ROOT "/card/RetroArch/saves/snes/Game.srm"));
   CHECK(path_is_directory(ROOT "/card/RetroArch/states/snes"));
   CHECK(fallback_notices == 1);

   /* No default either: no saves, and nothing beside the content. */
   rm_tree(ROOT "/card");
   rm_tree(ROOT "/default");
   last_notice[0] = '\0';
   load("/roms/snes/Game");
   CHECK(runloop_state.flags & RUNLOOP_FLAG_IS_SRAM_SAVE_DISABLED);
   CHECK(!(runloop_state.flags & RUNLOOP_FLAG_USE_SRAM));
   CHECK(missing_notices == 1);
   CHECK(same_path(last_notice, "save missing " ROOT "/card/RetroArch/saves"));
   CHECK(!same_path(runloop_state.name.savestate, "/roms/snes/Game.state"));
   CHECK(!path_is_directory(ROOT "/card"));

   /* The card again: saves on. */
   CHECK(path_mkdir(ROOT "/card/RetroArch/saves"));
   CHECK(path_mkdir(ROOT "/card/RetroArch/states"));
   load("/roms/snes/Game");
   CHECK(!(runloop_state.flags & RUNLOOP_FLAG_IS_SRAM_SAVE_DISABLED));
   CHECK(runloop_state.flags & RUNLOOP_FLAG_USE_SRAM);
   CHECK(missing_notices == 1);

   cfg_saves[0] = cfg_states[0] = '\0';
   g_defaults.dirs[DEFAULT_DIR_SRAM][0]      = '\0';
   g_defaults.dirs[DEFAULT_DIR_SAVESTATE][0] = '\0';
}

/* Netplay's saves-off from the last load is undone before the folder
 * check, so a root gone at this load still turns saves off. One
 * redirect: load()'s second one would hide the order. */
static void netplay_then_folder_gone(void)
{
   rm_tree(ROOT "/card");
   strlcpy(cfg_saves, ROOT "/card/saves", sizeof(cfg_saves));
   strlcpy(cfg_states, ROOT "/states", sizeof(cfg_states));
   CHECK(path_mkdir(ROOT "/card/saves/snes"));
   CHECK(filestream_write_file(ROOT "/card/saves/snes/.netplay", "x", 1));
   redirect("/roms/snes/Game", false);
   netplay_client = true;
   load("/roms/snes/Game");
   netplay_client = false;
   CHECK(runloop_netplay_saves_off);

   rm_tree(ROOT "/card");
   runloop_path_set_redirect(&test_settings, ROOT "/saves", ROOT "/states");
   CHECK(runloop_save_roots[0] == RUNLOOP_SAVE_ROOT_NONE);
   CHECK(runloop_save_folder_off);
   CHECK(runloop_state.flags & RUNLOOP_FLAG_IS_SRAM_SAVE_DISABLED);

   cfg_saves[0] = cfg_states[0] = '\0';
}

/* A netplay client with no usable save root makes no ".netplay"
 * chain under the missing parent. */
static void netplay_folder_gone(void)
{
   rm_tree(ROOT "/card");
   strlcpy(cfg_saves, ROOT "/card/saves", sizeof(cfg_saves));
   strlcpy(cfg_states, ROOT "/states", sizeof(cfg_states));
   strlcpy(g_defaults.dirs[DEFAULT_DIR_SRAM], ROOT "/default/saves",
         sizeof(g_defaults.dirs[DEFAULT_DIR_SRAM]));
   redirect("/roms/snes/Game", false);
   netplay_client = true;
   load("/roms/snes/Game");
   netplay_client = false;
   CHECK(!path_is_directory(ROOT "/card"));
   CHECK(!path_is_directory(ROOT "/default"));
   CHECK(runloop_save_folder_off);
   CHECK(!runloop_netplay_saves_off);
   CHECK(runloop_state.flags & RUNLOOP_FLAG_IS_SRAM_SAVE_DISABLED);

   cfg_saves[0] = cfg_states[0] = '\0';
   g_defaults.dirs[DEFAULT_DIR_SRAM][0] = '\0';
}

/* A savestate folder not found has its own notice, naming the folders;
 * a save folder not found comes first, with the save notice. */
static void state_folder_notice(void)
{
   rm_tree(ROOT "/card");
   rm_tree(ROOT "/default");
   strlcpy(cfg_saves, ROOT "/saves", sizeof(cfg_saves));
   strlcpy(cfg_states, ROOT "/card/states", sizeof(cfg_states));
   strlcpy(g_defaults.dirs[DEFAULT_DIR_SAVESTATE], ROOT "/default/states",
         sizeof(g_defaults.dirs[DEFAULT_DIR_SAVESTATE]));
   fallback_notices = missing_notices = 0;
   redirect("/roms/snes/Game", false);

   last_notice[0] = '\0';
   load("/roms/snes/Game");
   CHECK(same_path(last_notice, "state missing " ROOT "/card/states"));
   CHECK(!(runloop_state.flags & RUNLOOP_FLAG_IS_SRAM_SAVE_DISABLED));

   CHECK(path_mkdir(ROOT "/default/states"));
   last_notice[0] = '\0';
   load("/roms/snes/Game");
   CHECK(same_path(last_notice, "state fallback "
            ROOT "/card/states " ROOT "/default/states"));
   CHECK(fallback_notices == 0 && missing_notices == 0);

   strlcpy(cfg_saves, ROOT "/card/saves", sizeof(cfg_saves));
   strlcpy(g_defaults.dirs[DEFAULT_DIR_SRAM], ROOT "/default/saves",
         sizeof(g_defaults.dirs[DEFAULT_DIR_SRAM]));
   CHECK(path_mkdir(ROOT "/default/saves"));
   last_notice[0] = '\0';
   load("/roms/snes/Game");
   CHECK(same_path(last_notice, "save fallback "
            ROOT "/card/saves " ROOT "/default/saves"));
   CHECK(fallback_notices == 1);

   rm_tree(ROOT "/default");
   cfg_saves[0] = cfg_states[0] = '\0';
   g_defaults.dirs[DEFAULT_DIR_SRAM][0]      = '\0';
   g_defaults.dirs[DEFAULT_DIR_SAVESTATE][0] = '\0';
}

static unsigned kept_flushes;
void event_flush_kept_save_files(bool sram_used, bool compress_files)
{
   (void)compress_files;
   if (sram_used)
      kept_flushes++;
}

static void request_save_files(void *data)
{
   (void)data;
   runloop_request_save_files();
}

static void save_files_request(void)
{
   sthread_t *t;
   memset(&runloop_state, 0, sizeof(runloop_state));
   runloop_state.flags |= RUNLOOP_FLAG_USE_SRAM;
   kept_flushes = 0;
   runloop_take_save_files_request();
   CHECK(kept_flushes == 0);
   t = sthread_create(request_save_files, NULL);
   CHECK(t != NULL);
   if (t)
      sthread_join(t);
   CHECK(kept_flushes == 0);
   runloop_take_save_files_request();
   CHECK(kept_flushes == 1);
   runloop_take_save_files_request();
   CHECK(kept_flushes == 1);
}

int main(void)
{
   rm_tree(ROOT);
   CHECK(path_mkdir(ROOT "/saves"));
   CHECK(path_mkdir(ROOT "/states"));
   strlcpy(cfg_saves, ROOT "/saves", sizeof(cfg_saves));
   strlcpy(cfg_states, ROOT "/states", sizeof(cfg_states));
   reserved_rom_folder();
   netplay_no_folder();
   folder_check_each_load();
   netplay_then_folder_gone();
   netplay_folder_gone();
   state_folder_notice();
   save_files_request();
   rm_tree(ROOT);
   puts(failures ? "FAIL save_paths_test" : "PASS save_paths_test");
   return failures ? 1 : 0;
}
