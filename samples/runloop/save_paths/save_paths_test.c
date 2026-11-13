/* Where runloop_path_set_redirect() puts a game's saves and states.
 *
 * With sort-by-content on, the content folder's name becomes a folder
 * under the save and state roots, except "backups", "records",
 * "system" and "cloud-sync" in any case, which the roots keep for safety
 * copies, records, the system folder's copies and cloud sync's backups:
 * those games use the root (and the core's folder when that is on).
 *
 * A netplay client whose ".netplay" folder can't be made writes no
 * saves for that content, says so once, and the next content saves
 * again. The flags that gate the writes stand in for them here;
 * save_e2e's nosave_netplay_disconnect lane counts the writes.
 *
 * A save asked for from another thread (3DS's suspend hook) is made
 * once, at the main thread's next frame.
 *
 * Cloud sync, which walks the main config's folders only, waits while
 * one of them is missing, and says so when it was the one replaced.
 * The log line comes once for each folder it waits on.
 *
 * The shipping function is included and run over a real folder. */
#include <string.h>
#include <lists/dir_list.h>

/* A folder named "roms" that can't be listed, as a flaky share's */
static bool roms_unlistable;

static struct string_list *test_dir_list_new(const char *dir,
      const char *ext, bool include_dirs, bool include_hidden,
      bool include_compressed, bool recursive)
{
   size_t n = strlen(dir);
   while (n && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
      n--;
   if (roms_unlistable && n >= 4 && !strncmp(dir + n - 4, "roms", 4))
      return NULL;
   return dir_list_new(dir, ext, include_dirs, include_hidden,
         include_compressed, recursive);
}

#define dir_list_new test_dir_list_new
#include "../../../runloop.c"
#undef dir_list_new
#include "../../../defaults.h"
#include <stdio.h>
#include <stdarg.h>
#include <rthreads/rthreads.h>

#define ROOT "save_paths_test_files"

static int failures;
static settings_t test_settings;

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

void RARCH_LOG(const char *fmt, ...) { (void)fmt; }
static int pause_logs;
static char last_log[DIR_MAX_LENGTH * 3];
void RARCH_WARN(const char *fmt, ...)
{
   va_list ap;
   if (strstr(fmt, "[CloudSync] Paused"))
      pause_logs++;
   va_start(ap, fmt);
   vsnprintf(last_log, sizeof(last_log), fmt, ap);
   va_end(ap);
}
void RARCH_ERR(const char *fmt, ...) { (void)fmt; }
bool verbosity_is_enabled(void) { return false; }
static int fallback_notices;
static int missing_notices;
static int sync_notices;
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
      case MSG_CLOUD_SYNC_PAUSED_FALLBACK:
         sync_notices++;
         return "sync paused";
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
/* The main config's roots, "default" resolved. */
static char base_saves[DIR_MAX_LENGTH];
static char base_states[DIR_MAX_LENGTH];
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
      case RARCH_DIR_SAVEFILE_BASE:
         return base_saves;
      case RARCH_DIR_SAVESTATE_BASE:
         return base_states;
      default:
         break;
   }
   return cfg_saves;
}
bool path_set(enum rarch_path_type type, const char *path)
{ (void)type; (void)path; return true; }
static char test_config_path[PATH_MAX_LENGTH];
const char *path_get(enum rarch_path_type type)
{
   return type == RARCH_PATH_CONFIG ? test_config_path : "";
}
/* A main config load, which records the file's folder */
static void config_loaded(const char *path)
{
   strlcpy(test_config_path, path, sizeof(test_config_path));
   runloop_save_root_set_config(path);
}
settings_t *config_get_ptr(void) { return &test_settings; }
/* -s */
static bool cli_save_path;
bool retroarch_override_setting_is_set(
      enum rarch_override_setting enum_idx, void *data)
{
   (void)data;
   return cli_save_path && enum_idx == RARCH_OVERRIDE_SETTING_SAVE_PATH;
}
static bool sync_paused;
void task_cloud_sync_set_paused(bool paused) { sync_paused = paused; }

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
static unsigned recover_calls;
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
/* The members file_archive_get_file_list() lists: pack.7z's, and any
 * other archive's. Each listing is counted. */
static const char *archive_members[4];
static const char *other_members[4];
static bool archive_unreadable;
static unsigned archive_lists;
struct string_list *file_archive_get_file_list(const char *path,
      const char *valid_exts)
{
   size_t i;
   union string_list_elem_attr attr;
   struct string_list *list;
   const char **members = strstr(path, "pack.7z")
      ? archive_members : other_members;
   (void)valid_exts;
   archive_lists++;
   if (archive_unreadable || !(list = string_list_new()))
      return NULL;
   attr.i = 0;
   for (i = 0; i < ARRAY_SIZE(archive_members) && members[i]; i++)
      string_list_append(list, members[i], attr);
   return list;
}

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

   redirect("/roms/cloud-sync/Game", false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate, ROOT "/states/Game.state"));

   redirect("/roms/Cloud-Sync/Game", true);
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
   CHECK(!path_is_directory(ROOT "/saves/cloud-sync"));
   CHECK(!path_is_directory(ROOT "/states/cloud-sync"));
   CHECK(!path_is_directory(ROOT "/saves/Cloud-Sync"));
   CHECK(!path_is_directory(ROOT "/states/Cloud-Sync"));
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
   CHECK(same_path(last_log, "[SRAM] save fallback "
            ROOT "/card/RetroArch/saves " ROOT "/default/saves\n"));
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
   CHECK(same_path(last_log, "[SRAM] save missing "
            ROOT "/card/RetroArch/saves\n"));
   CHECK(!same_path(runloop_state.name.savestate, "/roms/snes/Game.state"));
   CHECK(!path_is_directory(ROOT "/card"));
   recover_calls = 0;
   runloop_recover_save_files(true);
   CHECK(recover_calls == 1);

   /* A netplay client's save RAM is the host's: off, and not kept. */
   netplay_client    = true;
   saves_off_notices = 0;
   load("/roms/snes/Game");
   CHECK(runloop_state.flags & RUNLOOP_FLAG_IS_SRAM_SAVE_DISABLED);
   /* The folder's notice already said saves are off */
   CHECK(saves_off_notices == 0);
   runloop_recover_save_files(true);
   CHECK(recover_calls == 1);
   CHECK(missing_notices == 2);
   netplay_client    = false;

   /* The card again: saves on. */
   CHECK(path_mkdir(ROOT "/card/RetroArch/saves"));
   CHECK(path_mkdir(ROOT "/card/RetroArch/states"));
   load("/roms/snes/Game");
   CHECK(!(runloop_state.flags & RUNLOOP_FLAG_IS_SRAM_SAVE_DISABLED));
   CHECK(runloop_state.flags & RUNLOOP_FLAG_USE_SRAM);
   CHECK(missing_notices == 2);

   cfg_saves[0] = cfg_states[0] = '\0';
   g_defaults.dirs[DEFAULT_DIR_SRAM][0]      = '\0';
   g_defaults.dirs[DEFAULT_DIR_SAVESTATE][0] = '\0';
}

/* A member in a folder of an archive is named after the folder the
 * archive is in, as a loose file there would be. */
static void archive_member_folder(void)
{
   runloop_path_set_basename("/roms/snes/pack.7z#SNES/Game.sfc");
   CHECK(same_path(runloop_state.runtime_content_path_basename,
            "/roms/snes/Game"));
   runloop_path_set_basename("/roms/snes/pack.7z#Game.sfc");
   CHECK(same_path(runloop_state.runtime_content_path_basename,
            "/roms/snes/Game"));

   strlcpy(cfg_saves, ROOT "/saves", sizeof(cfg_saves));
   strlcpy(cfg_states, ROOT "/states", sizeof(cfg_states));
   memset(&test_settings, 0, sizeof(test_settings));
   test_settings.bools.sort_savefiles_by_content_enable = true;
   runloop_state.system.info.library_name = "Snes9x";
   runloop_path_set_basename("/roms/snes/pack.7z#Disks/Game.sfc");
   runloop_path_set_redirect(&test_settings, ROOT "/saves", ROOT "/states");
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/snes/Game.srm"));
   CHECK(!path_is_directory(ROOT "/saves/Disks"));
}

/* The core's extensions, as runloop_event_init_core() passes them */
static const char *member_exts = "sfc|smc";

static void member_load(const char *content, bool per_core)
{
   memset(&runloop_state, 0, sizeof(runloop_state));
   memset(&test_settings, 0, sizeof(test_settings));
   test_settings.bools.sort_savefiles_by_content_enable  = true;
   test_settings.bools.sort_savestates_by_content_enable = true;
   test_settings.bools.sort_savefiles_enable             = per_core;
   runloop_state.system.info.library_name = "Snes9x";
   runloop_path_set_basename(content);
   runloop_path_set_basename_kept(&runloop_state, content, member_exts);
   runloop_path_set_redirect(&test_settings, ROOT "/saves", ROOT "/states");
}

/* Sorted by content, a member's saves were in the folder named after
 * its folder inside the archive. That folder stays in use, for saves
 * and states apart, while it has the game's and the new one has none. */
static void archive_member_old_folder(void)
{
   const char *member = ROOT "/roms/snes/pack.7z#Disks/Game.sfc";
   rm_tree(ROOT "/saves");
   rm_tree(ROOT "/states");
   CHECK(path_mkdir(ROOT "/saves/Disks/Snes9x"));
   CHECK(path_mkdir(ROOT "/states/Disks"));
   strlcpy(cfg_saves, ROOT "/saves", sizeof(cfg_saves));
   strlcpy(cfg_states, ROOT "/states", sizeof(cfg_states));

   CHECK(filestream_write_file(ROOT "/saves/Disks/Game.srm", "x", 1));
   member_load(member, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/Disks/Game.srm"));
   CHECK(!path_is_directory(ROOT "/saves/snes"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/states/snes/Game.state"));

   CHECK(filestream_write_file(ROOT "/states/Disks/Game.state.auto", "x", 1));
   member_load(member, false);
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/states/Disks/Game.state"));
   CHECK(filestream_delete(ROOT "/states/Disks/Game.state.auto") == 0);
   CHECK(filestream_write_file(ROOT "/states/Disks/Game.state12", "x", 1));
   member_load(member, false);
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/states/Disks/Game.state"));
   CHECK(filestream_delete(ROOT "/states/Disks/Game.state12") == 0);
   CHECK(filestream_write_file(ROOT "/states/Disks/Game.state3.png", "x", 1));
   CHECK(filestream_write_file(ROOT "/states/Disks/Game2.state", "x", 1));
   member_load(member, false);
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/states/snes/Game.state"));

   CHECK(filestream_write_file(ROOT "/saves/Disks/Snes9x/Game.srm", "x", 1));
   member_load(member, true);
   CHECK(same_path(runloop_state.name.savefile,
            ROOT "/saves/Disks/Snes9x/Game.srm"));

   CHECK(path_mkdir(ROOT "/saves/snes"));
   CHECK(filestream_write_file(ROOT "/saves/snes/Game.srm", "x", 1));
   CHECK(filestream_write_file(ROOT "/states/Disks/Game.state", "x", 1));
   CHECK(filestream_write_file(ROOT "/states/snes/Game.state.auto", "x", 1));
   member_load(member, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/snes/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/states/snes/Game.state"));

   CHECK(filestream_delete(ROOT "/saves/snes/Game.srm") == 0);
   CHECK(filestream_delete(ROOT "/saves/Disks/Game.srm") == 0);
   member_load(member, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/snes/Game.srm"));

   /* Loose content, and a member in a folder the roots keep */
   CHECK(filestream_write_file(ROOT "/saves/Disks/Game.srm", "x", 1));
   member_load(ROOT "/roms/snes/Game.sfc", false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/snes/Game.srm"));
   CHECK(path_mkdir(ROOT "/saves/system"));
   CHECK(filestream_write_file(ROOT "/saves/system/Game.srm", "x", 1));
   member_load(ROOT "/roms/snes/pack.7z#system/Game.sfc", false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/snes/Game.srm"));

   /* Two folders down: named after the inner one */
   CHECK(path_mkdir(ROOT "/saves/B"));
   CHECK(filestream_write_file(ROOT "/saves/B/Game.srm", "x", 1));
   member_load(ROOT "/roms/snes/pack.7z#A/B/Game.sfc", false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/B/Game.srm"));

   /* No content (a contentless start): nothing is looked for */
   CHECK(filestream_write_file(ROOT "/saves/Disks/.srm", "x", 1));
   member_load(member, false);
   runloop_state.runtime_content_path_basename[0] = '\0';
   runloop_path_set_redirect(&test_settings, ROOT "/saves", ROOT "/states");
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/Snes9x.srm"));

   rm_tree(ROOT "/saves");
   rm_tree(ROOT "/states");
   CHECK(path_mkdir(ROOT "/saves"));
   CHECK(path_mkdir(ROOT "/states"));
}

static void member_load_beside(const char *content, bool by_content,
      bool per_core)
{
   memset(&runloop_state, 0, sizeof(runloop_state));
   memset(&test_settings, 0, sizeof(test_settings));
   test_settings.bools.savefiles_in_content_dir          = true;
   test_settings.bools.savestates_in_content_dir         = true;
   test_settings.bools.sort_savefiles_by_content_enable  = by_content;
   test_settings.bools.sort_savestates_by_content_enable = by_content;
   test_settings.bools.sort_savefiles_enable             = per_core;
   test_settings.bools.sort_savestates_enable            = per_core;
   runloop_state.system.info.library_name = "Snes9x";
   runloop_path_set_basename(content);
   runloop_path_set_basename_kept(&runloop_state, content, member_exts);
   runloop_path_set_redirect(&test_settings, ROOT "/saves", ROOT "/states");
}

/* Beside the content, a member's folders were folders there too, with
 * each sorting's folder under them. */
static void archive_member_old_folder_beside(void)
{
   const char *member = ROOT "/roms/pack.7z#Disks/Game.sfc";
   CHECK(path_mkdir(ROOT "/roms/Disks/Disks/Snes9x"));
   CHECK(filestream_write_file(ROOT "/roms/Disks/Disks/Game.srm", "x", 1));
   CHECK(filestream_write_file(ROOT "/roms/Disks/Disks/Game.state.auto",
            "x", 1));
   member_load_beside(member, true, false);
   CHECK(same_path(runloop_state.name.savefile,
            ROOT "/roms/Disks/Disks/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/roms/Disks/Disks/Game.state"));
   CHECK(!path_is_directory(ROOT "/roms/roms"));
   CHECK(filestream_write_file(ROOT "/roms/Disks/Disks/Snes9x/Game.srm",
            "x", 1));
   member_load_beside(member, true, true);
   CHECK(same_path(runloop_state.name.savefile,
            ROOT "/roms/Disks/Disks/Snes9x/Game.srm"));

   CHECK(path_mkdir(ROOT "/roms/A/B/B"));
   CHECK(filestream_write_file(ROOT "/roms/A/B/B/Game.srm", "x", 1));
   member_load_beside(ROOT "/roms/pack.7z#A/B/Game.sfc", true, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/roms/A/B/B/Game.srm"));

   /* Sorted by core only */
   CHECK(path_mkdir(ROOT "/roms/Disks/Snes9x"));
   CHECK(filestream_write_file(ROOT "/roms/Disks/Snes9x/Game.srm", "x", 1));
   CHECK(filestream_write_file(ROOT "/roms/Disks/Snes9x/Game.state3",
            "x", 1));
   member_load_beside(member, false, true);
   CHECK(same_path(runloop_state.name.savefile,
            ROOT "/roms/Disks/Snes9x/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/roms/Disks/Snes9x/Game.state"));
   CHECK(!path_is_directory(ROOT "/roms/Snes9x"));

   /* Not sorted; the new folder wins once it has a save too */
   CHECK(filestream_write_file(ROOT "/roms/Disks/Game.srm", "x", 1));
   CHECK(filestream_write_file(ROOT "/roms/Disks/Game.state", "x", 1));
   member_load_beside(member, false, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/roms/Disks/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/roms/Disks/Game.state"));
   CHECK(filestream_write_file(ROOT "/roms/Game.srm", "x", 1));
   member_load_beside(member, false, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/roms/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/roms/Disks/Game.state"));

   rm_tree(ROOT "/roms");
   rm_tree(ROOT "/saves");
   rm_tree(ROOT "/states");
   CHECK(path_mkdir(ROOT "/saves"));
   CHECK(path_mkdir(ROOT "/states"));
}

/* Members of one archive with one name in different folders keep
 * their folders, so their saves stay apart; the others don't. */
static void archive_member_same_name(void)
{
   const char *usa = ROOT "/roms/snes/pack.7z#USA/Game.sfc";
   archive_members[0] = "USA/Game.sfc";
   archive_members[1] = "JPN/";
   archive_members[2] = "JPN/game.smc";
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/USA/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/states/USA/Game.state"));
   member_load(ROOT "/roms/snes/pack.7z#JPN/game.smc", false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/JPN/game.srm"));

   /* Beside the content, the folder is made */
   CHECK(path_mkdir(ROOT "/roms"));
   member_load_beside(ROOT "/roms/pack.7z#USA/Game.sfc", false, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/roms/USA/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/roms/USA/Game.state"));
   CHECK(path_is_directory(ROOT "/roms/USA"));

   /* One folder, another name, or a member with no folder */
   archive_members[2] = "USA/Game.smc";
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/snes/Game.srm"));
   archive_members[2] = "JPN/Other.sfc";
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/snes/Game.srm"));
   archive_members[2] = "Game.sfc";
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/USA/Game.srm"));
   member_load(ROOT "/roms/snes/pack.7z#Game.sfc", false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/snes/Game.srm"));

   archive_members[0] = archive_members[1] = archive_members[2] = NULL;
   rm_tree(ROOT "/roms");
   rm_tree(ROOT "/saves");
   rm_tree(ROOT "/states");
   CHECK(path_mkdir(ROOT "/saves"));
   CHECK(path_mkdir(ROOT "/states"));
}

/* Beside the archive, a loose game of the name, or another archive's
 * member of it, keeps the folder too; past 32 archives there none is
 * listed and it is kept. The frontend's own files don't count, and an
 * archive that can't be listed changes nothing unless the folder is
 * there. */
static void archive_member_name_beside(void)
{
   char arc[PATH_MAX_LENGTH];
   unsigned i;
   const char *usa = ROOT "/roms/pack.7z#USA/Game.sfc";
   archive_members[0] = "USA/Game.sfc";
   CHECK(path_mkdir(ROOT "/roms"));
   CHECK(filestream_write_file(ROOT "/roms/Game.srm", "x", 1));
   CHECK(filestream_write_file(ROOT "/roms/Game.state1", "x", 1));
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/roms/Game.srm"));

   /* Only content the core takes counts, or the member's own kind */
   CHECK(filestream_write_file(ROOT "/roms/Game.bkr", "x", 1));
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/roms/Game.srm"));
   CHECK(filestream_write_file(ROOT "/roms/game.smc", "x", 1));
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/USA/Game.srm"));
   member_exts = NULL;
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/roms/Game.srm"));
   member_exts = "sfc|smc";
   CHECK(filestream_delete(ROOT "/roms/game.smc") == 0);

   CHECK(filestream_write_file(ROOT "/roms/other.zip", "x", 1));
   other_members[0] = "JPN/GAME.sfc";
   archive_lists = 0;
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/USA/Game.srm"));
   CHECK(archive_lists == 2);
   other_members[0] = "Other.sfc";
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/roms/Game.srm"));

   for (i = 1; i < 32; i++)
   {
      snprintf(arc, sizeof(arc), ROOT "/roms/a%02u.zip", i);
      CHECK(filestream_write_file(arc, "x", 1));
   }
   archive_lists = 0;
   member_load(usa, false);
   CHECK(archive_lists == 33);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/roms/Game.srm"));
   /* Kept once a 33rd comes; a save made without the folder is left
    * where it is */
   CHECK(filestream_write_file(ROOT "/saves/roms/Game.srm", "x", 1));
   CHECK(filestream_write_file(ROOT "/roms/a32.7z", "x", 1));
   archive_lists = 0;
   member_load(usa, false);
   CHECK(archive_lists == 1);
   CHECK(same_path(runloop_state.runtime_content_path_basename,
            ROOT "/roms/USA/Game"));
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/USA/Game.srm"));
   member_load_beside(usa, false, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/roms/USA/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/roms/USA/Game.state"));
   CHECK(path_is_valid(ROOT "/saves/roms/Game.srm"));
   CHECK(filestream_delete(ROOT "/saves/roms/Game.srm") == 0);

   rm_tree(ROOT "/roms");
   CHECK(path_mkdir(ROOT "/roms"));
   archive_members[1] = "JPN/Game.sfc";
   rm_tree(ROOT "/saves/USA");
   archive_unreadable = true;
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/roms/Game.srm"));
   CHECK(path_mkdir(ROOT "/saves/USA"));
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/USA/Game.srm"));
   /* Its content folder gone too */
   rm_tree(ROOT "/roms");
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/USA/Game.srm"));
   CHECK(path_mkdir(ROOT "/roms"));
   archive_unreadable = false;

#ifdef _WIN32
   /* Its own archive, named in another case */
   CHECK(filestream_write_file(ROOT "/roms/PACK.7z", "x", 1));
   archive_members[1] = NULL;
   other_members[0]   = "USA/Game.sfc";
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/roms/Game.srm"));
   CHECK(filestream_delete(ROOT "/roms/PACK.7z") == 0);
   archive_members[1] = "JPN/Game.sfc";
#endif

   /* A kept folder that climbs out is not made; "Vol..2" is made */
   archive_members[0] = "../up/Game.sfc";
   member_load_beside(ROOT "/roms/pack.7z#../up/Game.sfc", false, false);
   CHECK(same_path(runloop_state.runtime_content_path_basename,
            ROOT "/roms/../up/Game"));
   CHECK(!path_is_directory(ROOT "/up"));
   archive_members[0] = "Vol..2/Game.sfc";
   member_load_beside(ROOT "/roms/pack.7z#Vol..2/Game.sfc", false, false);
   CHECK(path_is_directory(ROOT "/roms/Vol..2"));

   archive_members[0] = archive_members[1] = other_members[0] = NULL;
   rm_tree(ROOT "/roms");
   rm_tree(ROOT "/saves");
   rm_tree(ROOT "/states");
   CHECK(path_mkdir(ROOT "/saves"));
   CHECK(path_mkdir(ROOT "/states"));
}

/* A member kept apart from a game of its name never takes that game's
 * saves where the name without its folders puts them: a loose game's,
 * a root member's, or a sibling's from before this member was added.
 * Its own start in its folder; theirs are left as they are. */
static void archive_member_kept_apart(void)
{
   char arc[PATH_MAX_LENGTH];
   unsigned i;
   const char *usa = ROOT "/roms/pack.7z#USA/Game.sfc";
   CHECK(path_mkdir(ROOT "/roms"));
   CHECK(path_mkdir(ROOT "/saves/roms"));
   CHECK(path_mkdir(ROOT "/states/roms"));
   CHECK(filestream_write_file(ROOT "/saves/roms/Game.srm", "x", 1));
   CHECK(filestream_write_file(ROOT "/states/roms/Game.state", "x", 1));
   CHECK(filestream_write_file(ROOT "/roms/Game.srm", "x", 1));
   CHECK(filestream_write_file(ROOT "/roms/Game.state", "x", 1));
   archive_members[0] = "USA/Game.sfc";

   CHECK(filestream_write_file(ROOT "/roms/Game.sfc", "x", 1));
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/USA/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/states/USA/Game.state"));
   member_load_beside(usa, false, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/roms/USA/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/roms/USA/Game.state"));
   CHECK(filestream_delete(ROOT "/roms/Game.sfc") == 0);

   archive_members[1] = "Game.sfc";
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/USA/Game.srm"));
   member_load_beside(usa, false, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/roms/USA/Game.srm"));

   archive_members[1] = "JPN/Game.sfc";
   member_load(ROOT "/roms/pack.7z#JPN/Game.sfc", false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/JPN/Game.srm"));
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/USA/Game.srm"));
   CHECK(path_is_valid(ROOT "/saves/roms/Game.srm"));

   /* An archive of its name, not opened; with 32 more beside it */
   archive_members[1] = NULL;
   CHECK(filestream_write_file(ROOT "/roms/Game.zip", "x", 1));
   archive_lists = 0;
   member_load(usa, false);
   CHECK(archive_lists == 1);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/USA/Game.srm"));
   for (i = 1; i <= 32; i++)
   {
      snprintf(arc, sizeof(arc), ROOT "/roms/a%02u.zip", i);
      CHECK(filestream_write_file(arc, "x", 1));
   }
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/USA/Game.srm"));
   member_load_beside(usa, false, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/roms/USA/Game.srm"));

   archive_members[0] = archive_members[1] = NULL;
   rm_tree(ROOT "/roms");
   rm_tree(ROOT "/saves");
   rm_tree(ROOT "/states");
   CHECK(path_mkdir(ROOT "/saves"));
   CHECK(path_mkdir(ROOT "/states"));
}

/* With the archive's folder unlisted, a member uses the folder a kept
 * name gives it only when that folder is there (a clash was seen),
 * else goes without it as when nothing clashes. A loose game of its
 * name keeps its own saves. */
static void archive_member_unlisted(void)
{
   const char *usa = ROOT "/roms/pack.7z#USA/Game.sfc";
   archive_members[0] = "USA/Game.sfc";
   CHECK(path_mkdir(ROOT "/roms"));
   CHECK(filestream_write_file(ROOT "/roms/Game.sfc", "x", 1));
   CHECK(filestream_write_file(ROOT "/roms/Game.srm", "x", 1));
   roms_unlistable = true;

   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/roms/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/states/roms/Game.state"));
   member_load_beside(usa, false, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/roms/Game.srm"));
   CHECK(!path_is_directory(ROOT "/roms/USA"));

   CHECK(path_mkdir(ROOT "/saves/USA"));
   CHECK(path_mkdir(ROOT "/states/USA"));
   member_load(usa, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/USA/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/states/USA/Game.state"));

   /* Beside the content: the loose game's save is not the member's */
   CHECK(path_mkdir(ROOT "/roms/USA"));
   member_load_beside(usa, false, false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/roms/USA/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/roms/USA/Game.state"));
   roms_unlistable = false;
   member_load_beside(usa, false, false);
   CHECK(runloop_archive_kept);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/roms/USA/Game.srm"));

   archive_members[0] = NULL;
   rm_tree(ROOT "/roms");
   rm_tree(ROOT "/saves");
   rm_tree(ROOT "/states");
   CHECK(path_mkdir(ROOT "/saves"));
   CHECK(path_mkdir(ROOT "/states"));
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
 * chain under the missing parent, and its saves are off as a client's:
 * the host's save RAM goes to no recovery file. */
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
   CHECK(!runloop_save_folder_off);
   CHECK(runloop_netplay_saves_off);
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

/* Sync waits while a main config folder is missing, whichever folder
 * the game uses: an override's or -s's, or the content's. Only a
 * replaced main folder has the notice; a replaced override folder
 * does not pause. */
static void pause_follows_main_folder(void)
{
   rm_tree(ROOT "/card");
   rm_tree(ROOT "/default");
   CHECK(path_mkdir(ROOT "/card/saves"));
   CHECK(path_mkdir(ROOT "/other/saves"));
   CHECK(path_mkdir(ROOT "/default/saves"));
   CHECK(path_mkdir(ROOT "/default/states"));
   strlcpy(g_defaults.dirs[DEFAULT_DIR_SRAM], ROOT "/default/saves",
         sizeof(g_defaults.dirs[DEFAULT_DIR_SRAM]));
   strlcpy(g_defaults.dirs[DEFAULT_DIR_SAVESTATE], ROOT "/default/states",
         sizeof(g_defaults.dirs[DEFAULT_DIR_SAVESTATE]));
   strlcpy(base_saves, ROOT "/card/saves", sizeof(base_saves));
   strlcpy(base_states, ROOT "/default/states", sizeof(base_states));
   cfg_states[0] = '\0';
   redirect("/roms/snes/Game", false);
   test_settings.bools.cloud_sync_enable = true;
   fallback_notices = sync_notices = 0;

   /* An override's folder gone */
   strlcpy(cfg_saves, ROOT "/gone/saves", sizeof(cfg_saves));
   sync_paused = true;
   load("/roms/snes/Game");
   CHECK(same_path(live_saves, ROOT "/default/saves"));
   CHECK(fallback_notices == 1);
   CHECK(!sync_paused);
   CHECK(sync_notices == 0);

   /* The main folder gone, an override's in use */
   rm_tree(ROOT "/card");
   strlcpy(cfg_saves, ROOT "/other/saves", sizeof(cfg_saves));
   load("/roms/snes/Game");
   CHECK(same_path(live_saves, ROOT "/other/saves"));
   CHECK(sync_paused);
   CHECK(sync_notices == 0);

   /* and in use */
   strlcpy(cfg_saves, ROOT "/card/saves", sizeof(cfg_saves));
   sync_paused = false;
   load("/roms/snes/Game");
   CHECK(same_path(live_saves, ROOT "/default/saves"));
   CHECK(sync_paused);
   CHECK(sync_notices == 1);

   /* Saves beside the content, then -s */
   test_settings.bools.savefiles_in_content_dir = true;
   sync_paused = false;
   load("/roms/snes/Game");
   CHECK(sync_paused);
   test_settings.bools.savefiles_in_content_dir = false;
   cli_save_path = true;
   sync_paused   = false;
   load("/roms/snes/Game");
   CHECK(sync_paused);
   CHECK(sync_notices == 1);

   CHECK(path_mkdir(ROOT "/card/saves"));
   load("/roms/snes/Game");
   CHECK(!sync_paused);
   cli_save_path = false;

   rm_tree(ROOT "/card");
   rm_tree(ROOT "/other");
   rm_tree(ROOT "/default");
   cfg_saves[0] = cfg_states[0] = '\0';
   base_saves[0] = base_states[0] = '\0';
   g_defaults.dirs[DEFAULT_DIR_SRAM][0]      = '\0';
   g_defaults.dirs[DEFAULT_DIR_SAVESTATE][0] = '\0';
}

/* Checked again where a main folder may have been made or set (Apple
 * makes the default folders after the config load), the state folder
 * as much as the save folder, and logged once for each folder the
 * pause waits on: a playlist redirects for each entry. */
static void pause_checked_again(void)
{
   rm_tree(ROOT "/main");
   CHECK(path_mkdir(ROOT "/main/saves"));
   strlcpy(base_saves, ROOT "/main/saves", sizeof(base_saves));
   strlcpy(base_states, ROOT "/main/states", sizeof(base_states));
   pause_logs  = 0;
   sync_paused = false;

   runloop_save_base_check();
   CHECK(sync_paused);
   CHECK(pause_logs == 1);
   redirect("/roms/snes/Game", false);
   redirect("/roms/snes/Other", false);
   CHECK(sync_paused);
   CHECK(pause_logs == 1);

   CHECK(path_mkdir(ROOT "/main/states"));
   runloop_save_base_check();
   CHECK(!sync_paused);
   CHECK(pause_logs == 1);

   rm_tree(ROOT "/main/states");
   runloop_save_base_check();
   CHECK(sync_paused);
   CHECK(pause_logs == 2);
   strlcpy(base_states, ROOT "/main/other", sizeof(base_states));
   runloop_save_base_check();
   CHECK(sync_paused);
   CHECK(pause_logs == 3);

   rm_tree(ROOT "/main");
   base_saves[0] = base_states[0] = '\0';
   runloop_save_base_check();
   CHECK(!sync_paused);
}

static unsigned kept_flushes;
void event_flush_kept_save_files(bool sram_used, bool compress_files)
{
   (void)compress_files;
   if (sram_used)
      kept_flushes++;
}

/* Saving off for want of a folder is save_e2e's folder_off_recovers;
 * here only whether it is asked for. */
void event_recover_save_files(bool sync)
{
   (void)sync;
   recover_calls++;
}

void event_seed_recovered_save_files(void) { }

bool event_save_files_recovered(void)
{
   return false;
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

/* A relative configured root is under the config file's folder,
 * wherever RetroArch was started from. */
static void relative_root(void)
{
   config_loaded(ROOT "/cfg/retroarch.cfg");
   strlcpy(cfg_saves, "saves", sizeof(cfg_saves));
   strlcpy(cfg_states, "states", sizeof(cfg_states));
   CHECK(path_mkdir(ROOT "/cfg"));
   fallback_notices = missing_notices = 0;

   redirect("/roms/snes/Game", false);
   load("/roms/snes/Game");
   CHECK(same_path(runloop_state.name.savefile,
            ROOT "/cfg/saves/snes/Game.srm"));
   CHECK(same_path(runloop_state.name.savestate,
            ROOT "/cfg/states/snes/Game.state"));
   CHECK(fallback_notices == 0 && missing_notices == 0);
   CHECK(!path_is_directory("saves"));

   config_loaded("");
   cfg_saves[0] = cfg_states[0] = '\0';
}

/* Not relative, or no config file: as written */
static void relative_root_kept(void)
{
   static const char *kept[] = {
      "", "default", "sd:/saves", "C:foo", "/saves",
#ifdef _WIN32
      "\\saves", "C:\\saves",
#endif
   };
   char s[DIR_MAX_LENGTH];
   size_t i;
   config_loaded(ROOT "/cfg/retroarch.cfg");
   for (i = 0; i < ARRAY_SIZE(kept); i++)
   {
      strlcpy(s, kept[i], sizeof(s));
      runloop_save_root_resolve(s, sizeof(s));
      if (!string_is_equal(s, kept[i]))
      {
         fprintf(stderr, "FAIL kept \"%s\": \"%s\"\n", kept[i], s);
         failures++;
      }
   }
   config_loaded("");
   strlcpy(s, "saves", sizeof(s));
   runloop_save_root_resolve(s, sizeof(s));
   CHECK(string_is_equal(s, "saves"));
}

/* A folder that does not fit under the config file's stays as written,
 * logged once, never cut. */
static void relative_root_too_long(void)
{
   char cfg[DIR_MAX_LENGTH + 16];
   char s[DIR_MAX_LENGTH];
   char big[PATH_MAX_LENGTH];
   size_t i = strlcpy(cfg, ROOT "/", sizeof(cfg));
   for (; i < DIR_MAX_LENGTH - 5; i++)
      cfg[i] = 'a';
   strlcpy(cfg + i, "/r.cfg", sizeof(cfg) - i);
   config_loaded(cfg);

   last_log[0] = '\0';
   strlcpy(s, "saves", sizeof(s));
   runloop_save_root_resolve(s, sizeof(s));
   CHECK(string_is_equal(s, "saves"));
   CHECK(strstr(last_log, "\"saves\"") != NULL);
   last_log[0] = '\0';
   runloop_save_root_resolve(s, sizeof(s));
   CHECK(string_is_equal(s, "saves"));
   CHECK(!*last_log);

   /* config_load_file's buffer has room for it */
   strlcpy(big, "saves", sizeof(big));
   runloop_save_root_resolve(big, sizeof(big));
   CHECK(strlen(big) == i + 1 + strlen("saves"));

   /* Longer than the resolver's copy */
   config_loaded(ROOT "/cfg/retroarch.cfg");
   memset(big, 'b', DIR_MAX_LENGTH + 8);
   big[DIR_MAX_LENGTH + 8] = '\0';
   runloop_save_root_resolve(big, sizeof(big));
   CHECK(strlen(big) == DIR_MAX_LENGTH + 8 && *big == 'b');
   config_loaded("");
}

/* A relative main folder gone: sync pauses and says so, as for an
 * absolute one. The notice names the folder resolved; the log line
 * names it as written too. */
static void relative_root_gone(void)
{
   CHECK(path_mkdir(ROOT "/cfg"));
   CHECK(path_mkdir(ROOT "/default/saves"));
   CHECK(path_mkdir(ROOT "/default/states"));
   config_loaded(ROOT "/cfg/retroarch.cfg");
   strlcpy(g_defaults.dirs[DEFAULT_DIR_SRAM], ROOT "/default/saves",
         sizeof(g_defaults.dirs[DEFAULT_DIR_SRAM]));
   strlcpy(g_defaults.dirs[DEFAULT_DIR_SAVESTATE], ROOT "/default/states",
         sizeof(g_defaults.dirs[DEFAULT_DIR_SAVESTATE]));
   /* As config_load_file sets them */
   strlcpy(cfg_saves, "../card/saves", sizeof(cfg_saves));
   strlcpy(base_saves, cfg_saves, sizeof(base_saves));
   runloop_save_root_resolve(base_saves, sizeof(base_saves));
   strlcpy(base_states, ROOT "/default/states", sizeof(base_states));
   cfg_states[0] = '\0';
   redirect("/roms/snes/Game", false);
   fallback_notices = missing_notices = sync_notices = 0;

   last_notice[0] = last_log[0] = '\0';
   load("/roms/snes/Game");
   CHECK(fallback_notices == 1);
   CHECK(same_path(last_notice, "save fallback " ROOT "/cfg/../card/saves "
            ROOT "/default/saves"));
   CHECK(same_path(last_log, "[SRAM] save fallback " ROOT "/cfg/../card/saves "
            ROOT "/default/saves (\"../card/saves\" -> \""
            ROOT "/cfg/../card/saves\")\n"));

   test_settings.bools.cloud_sync_enable = true;
   sync_paused = false;
   load("/roms/snes/Game");
   CHECK(fallback_notices == 2);
   CHECK(sync_paused);
   CHECK(sync_notices == 1);
   test_settings.bools.cloud_sync_enable = false;

   rm_tree(ROOT "/default");
   last_notice[0] = '\0';
   load("/roms/snes/Game");
   CHECK(missing_notices == 1);
   CHECK(same_path(last_notice, "save missing " ROOT "/cfg/../card/saves"));

   rm_tree(ROOT "/cfg");
   config_loaded("");
   cfg_saves[0] = '\0';
   base_saves[0] = base_states[0] = '\0';
   g_defaults.dirs[DEFAULT_DIR_SRAM][0]      = '\0';
   g_defaults.dirs[DEFAULT_DIR_SAVESTATE][0] = '\0';
}

/* "Save Configuration As" names another config file without loading
 * it: a relative root stays where it is, with the folder sync walks,
 * until a config is loaded again. */
static void relative_root_save_as(void)
{
   CHECK(path_mkdir(ROOT "/cfg/saves"));
   CHECK(path_mkdir(ROOT "/cfg/states"));
   CHECK(path_mkdir(ROOT "/cfg/config"));
   config_loaded(ROOT "/cfg/retroarch.cfg");
   strlcpy(cfg_saves, "saves", sizeof(cfg_saves));
   strlcpy(cfg_states, "states", sizeof(cfg_states));
   strlcpy(base_saves, cfg_saves, sizeof(base_saves));
   runloop_save_root_resolve(base_saves, sizeof(base_saves));
   strlcpy(base_states, cfg_states, sizeof(base_states));
   runloop_save_root_resolve(base_states, sizeof(base_states));
   redirect("/roms/snes/Game", false);
   fallback_notices = missing_notices = 0;
   load("/roms/snes/Game");
   CHECK(same_path(live_saves, ROOT "/cfg/saves"));

   strlcpy(test_config_path, ROOT "/cfg/config/mine.cfg",
         sizeof(test_config_path));
   sync_paused = false;
   load("/roms/snes/Game");
   CHECK(same_path(live_saves, base_saves));
   CHECK(same_path(live_states, base_states));
   CHECK(!path_is_directory(ROOT "/cfg/config/saves"));
   CHECK(!sync_paused);
   CHECK(fallback_notices == 0 && missing_notices == 0);

   config_loaded(ROOT "/cfg/config/mine.cfg");
   load("/roms/snes/Game");
   CHECK(same_path(live_saves, ROOT "/cfg/config/saves"));

   rm_tree(ROOT "/cfg");
   config_loaded("");
   cfg_saves[0] = cfg_states[0] = '\0';
   base_saves[0] = base_states[0] = '\0';
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
   archive_member_folder();
   archive_member_old_folder();
   archive_member_old_folder_beside();
   archive_member_same_name();
   archive_member_name_beside();
   archive_member_kept_apart();
   archive_member_unlisted();
   netplay_then_folder_gone();
   netplay_folder_gone();
   state_folder_notice();
   pause_follows_main_folder();
   pause_checked_again();
   save_files_request();
   relative_root();
   relative_root_kept();
   relative_root_too_long();
   relative_root_gone();
   relative_root_save_as();
   rm_tree(ROOT);
   puts(failures ? "FAIL save_paths_test" : "PASS save_paths_test");
   return failures ? 1 : 0;
}
