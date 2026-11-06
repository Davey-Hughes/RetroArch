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
const char *msg_hash_to_str(enum msg_hash_enums msg)
{
   return msg == MSG_NETPLAY_SAVES_OFF ? "saves off" : "";
}
void dir_set(enum rarch_dir_type type, const char *path) { (void)type; (void)path; }

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
void ui_companion_driver_msg_queue_push(const char *msg, unsigned priority,
      unsigned duration, bool flush)
{
   (void)priority; (void)duration; (void)flush;
   if (string_is_equal(msg, "saves off"))
      saves_off_notices++;
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
   reserved_rom_folder();
   netplay_no_folder();
   save_files_request();
   rm_tree(ROOT);
   puts(failures ? "FAIL save_paths_test" : "PASS save_paths_test");
   return failures ? 1 : 0;
}
