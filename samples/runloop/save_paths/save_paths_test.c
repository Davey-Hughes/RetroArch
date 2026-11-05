/* Where runloop_path_set_redirect() puts a game's saves and states.
 *
 * With sort-by-content on, the content folder's name becomes a folder
 * under the save and state roots, except "backups" and "records" in
 * any case, which the roots keep for safety copies and records: those
 * games use the root (and the core's folder when that is on).
 *
 * The shipping function is included and run over a real folder. */
#include "../../../runloop.c"
#include <stdio.h>

#define ROOT "save_paths_test_files"

static int failures;
static settings_t test_settings;

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

void RARCH_LOG(const char *fmt, ...) { (void)fmt; }
void RARCH_WARN(const char *fmt, ...) { (void)fmt; }
void RARCH_ERR(const char *fmt, ...) { (void)fmt; }
bool verbosity_is_enabled(void) { return false; }
const char *msg_hash_to_str(enum msg_hash_enums msg) { (void)msg; return ""; }
void dir_set(enum rarch_dir_type type, const char *path) { (void)type; (void)path; }

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

   redirect("/roms/backups2/Game", false);
   CHECK(same_path(runloop_state.name.savefile, ROOT "/saves/backups2/Game.srm"));

   CHECK(!path_is_directory(ROOT "/saves/backups"));
   CHECK(!path_is_directory(ROOT "/saves/Records"));
   CHECK(!path_is_directory(ROOT "/states/backups"));
   CHECK(!path_is_directory(ROOT "/states/Records"));
   CHECK(!path_is_directory(ROOT "/saves/BACKUPS"));
   CHECK(!path_is_directory(ROOT "/states/BACKUPS"));
}

int main(void)
{
   rm_tree(ROOT);
   CHECK(path_mkdir(ROOT "/saves"));
   CHECK(path_mkdir(ROOT "/states"));
   reserved_rom_folder();
   rm_tree(ROOT);
   puts(failures ? "FAIL save_paths_test" : "PASS save_paths_test");
   return failures ? 1 : 0;
}
