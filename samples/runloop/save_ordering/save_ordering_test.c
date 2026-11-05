/* Save items in the shipping frontend: load and unload order.
 *
 * This links the real retroarch.c and loads save_core, a core with two
 * frontend-owned items that logs every call it gets.
 *
 * Lanes, among them:
 *  load    the item's file is read before loaded(), each item is told
 *          before the first retro_run(), one with no file too.
 *  switch  loading other content writes the first content's items after
 *          its retro_unload_game() and before the next retro_load_game(),
 *          what the core wrote while unloading included; an item nothing
 *          wrote gets no file.
 *  late    an item the core registers mid-game, its file already on
 *          disk, is filled from that file and told through loaded(),
 *          once, outside the registering call and before the next shown
 *          retro_run(); SAVE_FILES and the close leave the file as it
 *          was, and a change the core makes reaches it. Variants: the
 *          same id moved to another file; run-ahead on (one instance),
 *          whose own frames may make the registration.
 *  close   closing writes after retro_unload_game() and before
 *          retro_deinit().
 *
 * Usage: save_ordering_test <run dir>
 * The run dir is created, and emptied again at the end. On Linux
 * nothing else is written: XDG_CONFIG_HOME, which holds the recovery
 * folder, points into it, and no playlist is written. No sound, MIDI
 * or input device is opened; a build without the test drivers skips. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <compat/strl.h>
#include <dynamic/dylib.h>
#include <features/features_cpu.h>
#include <file/config_file.h>
#include <file/file_path.h>
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <queues/message_queue.h>
#include <queues/task_queue.h>
#include <streams/file_stream.h>
#include <time/rtime.h>

#include "../../../runloop.h"
#include "../../../retroarch.h"
#include "../../../command.h"
#include "../../../content.h"
#include "../../../configuration.h"
#include "../../../file_path_special.h"
#include "../../../frontend/frontend_driver.h"
#include "../../../tasks/task_content.h"
#include "../../../tasks/tasks_internal.h"
#include "../../../verbosity.h"

#if defined(_WIN32)
#define CORE_NAME "save_core.dll"
#elif defined(__APPLE__)
#define CORE_NAME "save_core.dylib"
#else
#define CORE_NAME "save_core.so"
#endif

#define SRM_SIZE 48
#define EEP_SIZE 8
#define CARD_SIZE 16
#define CARDS 3

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

typedef const char *(*core_log_t)(void);
typedef unsigned char *(*core_wram_t)(void);
typedef void (*core_late_t)(unsigned frames, const char *path,
      int after_rollback);
typedef unsigned (*core_count_t)(void);
typedef void (*core_again_t)(void);
typedef void (*core_busy_t)(int on);
typedef void (*core_fps_t)(double rate);

static core_log_t   core_log;
static core_wram_t  core_wram;
static core_wram_t  core_card;
static core_late_t  core_late;
static core_count_t core_unserializes;
static core_again_t core_again;
static core_busy_t  core_busy;
static core_count_t core_asks;
static core_count_t core_eep_ready;
static core_fps_t   core_fps;
static size_t log_mark;
/* The late lane's files: first card, moved card, run-ahead. */
static char cards[CARDS][PATH_MAX_LENGTH];
static unsigned char card_bytes[CARDS][CARD_SIZE];
static unsigned char changed[CARD_SIZE];
static char dir[PATH_MAX_LENGTH];
static char a_srm[PATH_MAX_LENGTH];
static char a_eep[PATH_MAX_LENGTH];
static char b_srm[PATH_MAX_LENGTH];
static char b_eep[PATH_MAX_LENGTH];
static unsigned char pattern[SRM_SIZE];

static bool content_running(void)
{
   return (content_get_flags() & CONTENT_ST_FLAG_IS_INITED)
      && runloop_state_get_ptr()->current_core_type != CORE_TYPE_DUMMY;
}

static bool switched(void)
{
   return !runloop_state_get_ptr()->content_switching
      && content_running()
      && strstr(core_log(), "load b.bin");
}

static bool b_ran(void)
{
   return strstr(core_log() + log_mark, "run\n") != NULL;
}

static bool registered(void)
{
   return strstr(core_log() + log_mark, "run after\n") != NULL;
}

static bool closed(void)
{
   return !content_running() && !runloop_state_get_ptr()->content_switching;
}

/* Passes of the frontend's main loop, until @done or 600 of them. */
static void run_until(bool (*done)(void))
{
   unsigned i;
   for (i = 0; i < 600 && !(done && done()); i++)
   {
      runloop_iterate();
      task_queue_check();
   }
}

static bool file_holds(const char *path, const unsigned char *data,
      size_t len)
{
   bool same;
   void *buf = NULL;
   int64_t n = 0;
   if (!filestream_read_file(path, &buf, &n))
      return false;
   same = n == (int64_t)len && !memcmp(buf, data, len);
   free(buf);
   return same;
}

static char ra_log[PATH_MAX_LENGTH];
static char idle_bin[PATH_MAX_LENGTH];
static size_t ra_log_mark;

static bool mode_ran(void)
{
   return !runloop_state_get_ptr()->content_switching
      && content_running()
      && strstr(core_log() + log_mark, "run\n") != NULL;
}

/* Occurrences of @needle in the frontend's log since the lane began. */
static unsigned log_count(const char *needle)
{
   unsigned n = 0;
   void *buf  = NULL;
   int64_t len;
   const char *at;
   if (     !filestream_read_file(ra_log, &buf, &len)
         || (size_t)len < ra_log_mark)
   {
      free(buf);
      return 0;
   }
   for (at = (const char*)buf + ra_log_mark; (at = strstr(at, needle));
         at++)
      n++;
   free(buf);
   return n;
}

/* Files in folder @path whose names hold @part. */
static unsigned dir_count(const char *path, const char *part)
{
   unsigned i;
   unsigned n                = 0;
   struct string_list *list  = dir_list_new(path, NULL, false, true,
         false, false);
   for (i = 0; list && i < list->size; i++)
      if (strstr(path_basename(list->elems[i].data), part))
         n++;
   string_list_free(list);
   return n;
}

/* Lane @name's folder of copies holds @undo files, and the recovery
 * folder @recovered recovery files. */
static bool lane_files(const char *name, unsigned undo, unsigned recovered)
{
   char p[PATH_MAX_LENGTH];
   char sub[64];
   snprintf(sub, sizeof(sub), "assets/state_undo/save_core/%s", name);
   fill_pathname_join(p, dir, sub, sizeof(p));
   if (dir_count(p, "") != undo)
      return false;
   if (!fill_pathname_application_data(p, sizeof(p)))
      return !recovered;
   return dir_count(p, "RetroArch-recovery-") == recovered;
}

/* <dir>/<name>.<ext> */
static void lane_file(char *s, const char *name, const char *ext)
{
   char base[64];
   snprintf(base, sizeof(base), "%s.%s", name, ext);
   fill_pathname_join(s, dir, base, PATH_MAX_LENGTH);
}

/* Switches to content @bin, whose own save files are now read, and
 * runs it once. */
static void switch_to(const char *bin)
{
   content_ctx_info_t info;
   memset(&info, 0, sizeof(info));
   log_mark = strlen(core_log());
   CHECK(task_push_load_content_with_core(bin, &info, CORE_TYPE_PLAIN,
            NULL, NULL), "%s did not start", bin);
   run_until(mode_ran);
}

/* <name>.bin, written as @bin, in its mode and run. */
static void mode_start(const char *name, char *bin, size_t *core_mark)
{
   char text[32];
   lane_file(bin, name, "bin");
   snprintf(text, sizeof(text), "mode:%s", name);
   filestream_write_file(bin, text, strlen(text));
   ra_log_mark = 0;
   {
      void *buf = NULL;
      int64_t len;
      if (filestream_read_file(ra_log, &buf, &len))
         ra_log_mark = (size_t)len;
      free(buf);
   }
   switch_to(bin);
   *core_mark = log_mark;
   CHECK(mode_ran(), "%s did not run", name);
}

/* <name>.bin in its mode, run, then closed by switching to idle. */
static void mode_lane(const char *name, size_t *core_mark)
{
   static char bin[PATH_MAX_LENGTH];
   mode_start(name, bin, core_mark);
   /* A game write the close must keep. */
   core_wram()[2] = 0x77;
   switch_to(idle_bin);
   filestream_delete(bin);
}

static void lane_load(void)
{
   char want[256];
   unsigned i, sum = 0;
   unsigned before = failures;
   for (i = 0; i < SRM_SIZE; i++)
      sum += pattern[i];
   snprintf(want, sizeof(want),
         "load a.bin\nloaded 0 %u\nloaded 1 0\nrun\n", sum);
   run_until(NULL);
   CHECK(content_running(), "the content did not load");
   CHECK(!strcmp(core_log(), want), "load order:\n%s\nwanted:\n%s",
         core_log(), want);
   printf("[%s] load\n", failures == before ? "ok" : "FAIL");
}

static void lane_switch(const char *b_bin)
{
   content_ctx_info_t info;
   unsigned char want[SRM_SIZE];
   unsigned before = failures;
   const char *at  = NULL;

   /* The game writes; with autosave off only the close persists it. */
   core_wram()[1] = 0x77;
   memcpy(want, pattern, sizeof(want));
   want[0] = 0x5a;
   want[1] = 0x77;

   memset(&info, 0, sizeof(info));
   CHECK(task_push_load_content_with_core(b_bin, &info, CORE_TYPE_PLAIN,
            NULL, NULL), "the switch did not start");
   run_until(switched);
   CHECK(switched(), "the second content did not load");

   at = strstr(core_log(), "run\n");
   CHECK(at && !strncmp(at, "run\nunload\ndeinit\nload b.bin\ncheck 5a\n"
            "loaded 0 0\nloaded 1 0\n", 58),
         "switch order:\n%s", core_log());
   CHECK(file_holds(a_srm, want, sizeof(want)),
         "the first content's save does not hold its last bytes");
   CHECK(!path_is_valid(a_eep), "an untouched item got a file");
   printf("[%s] switch\n", failures == before ? "ok" : "FAIL");
}

/* The core registers "card" at cards[c] mid-game. */
static void late_register(const char *name, unsigned c, int after_rollback)
{
   char want[96];
   unsigned i, sum = 0;
   unsigned before = failures;
   for (i = 0; i < CARD_SIZE; i++)
      sum += card_bytes[c][i];
   snprintf(want, sizeof(want),
         "register card\nloaded 2 %u\nrun after\n", sum);
   log_mark = strlen(core_log());
   core_late(3, cards[c], after_rollback);
   run_until(registered);
   CHECK(!strcmp(core_log() + log_mark, want), "%s order:\n%s\nwanted:\n%s",
         name, core_log() + log_mark, want);
   CHECK(!memcmp(core_card(), card_bytes[c], CARD_SIZE),
         "%s: the buffer does not hold the file", name);
   CHECK(!strstr(core_log(), "loaded in set"),
         "%s: loaded() came inside SET_SAVE_INTERFACE", name);
   CHECK(file_holds(cards[c], card_bytes[c], CARD_SIZE),
         "%s: the registration changed the file", name);
   printf("[%s] late: %s\n", failures == before ? "ok" : "FAIL", name);
}

static void lane_late(void)
{
   settings_t *settings = config_get_ptr();
   unsigned before;

   /* The switch may have left the Quick Menu up; the second content's
    * first frames run before the lane starts. */
   log_mark = strlen(core_log());
   retroarch_menu_running_finished(false);
   run_until(b_ran);

   late_register("new item", 0, 0);
   command_event(CMD_EVENT_SAVE_FILES, NULL);
   CHECK(file_holds(cards[0], card_bytes[0], CARD_SIZE),
         "SAVE_FILES changed a file the core never wrote");

   late_register("moved to another file", 1, 0);

   /* The game writes: the next SAVE_FILES carries it to the file. */
   before = failures;
   core_card()[0] ^= 0xff;
   memcpy(changed, core_card(), CARD_SIZE);
   command_event(CMD_EVENT_SAVE_FILES, NULL);
   CHECK(file_holds(cards[1], changed, CARD_SIZE),
         "a change after the fill did not reach the file");
   printf("[%s] late: a change reaches the file\n",
         failures == before ? "ok" : "FAIL");

   before = core_unserializes();
   settings->bools.run_ahead_enabled            = true;
   settings->uints.run_ahead_frames             = 1;
   settings->bools.run_ahead_secondary_instance = false;
   late_register("run-ahead", 2, 1);
   CHECK(core_unserializes() > before, "run-ahead did not run");
   settings->bools.run_ahead_enabled            = false;
}

static void lane_close(void)
{
   unsigned char want[SRM_SIZE];
   unsigned before = failures;
   const char *log;
   size_t len;

   memset(want, 0, sizeof(want));
   want[0] = 0x5a;
   command_event(CMD_EVENT_UNLOAD_CORE, NULL);
   run_until(closed);
   CHECK(closed(), "the content did not close");
   log = core_log();
   len = strlen(log);
   CHECK(len >= 14 && !strcmp(log + len - 14, "unload\ndeinit\n"),
         "close order:\n%s", log);
   /* 0x5a from the unload; 0xee would be retro_deinit's. */
   CHECK(file_holds(b_srm, want, sizeof(want)),
         "the close did not write what the unload left");
   CHECK(!path_is_valid(b_eep), "an untouched item got a file");
   /* The final write left the cards as their last SAVE_FILES did. */
   CHECK(file_holds(cards[0], card_bytes[0], CARD_SIZE)
         && file_holds(cards[2], card_bytes[2], CARD_SIZE),
         "the close changed a card the core never wrote");
   CHECK(file_holds(cards[1], changed, CARD_SIZE),
         "the close lost the change to the moved card");
   printf("[%s] close\n", failures == before ? "ok" : "FAIL");
}

/* What a 48-byte save holds after the lane's game write and unload. */
static void after_close(unsigned char *want, const unsigned char *from)
{
   memcpy(want, from, SRM_SIZE);
   want[0] = 0x5a;
   want[2] = 0x77;
}

static void lane_layout(void)
{
   static char a[PATH_MAX_LENGTH];
   static char b[PATH_MAX_LENGTH];
   static char c[PATH_MAX_LENGTH];
   unsigned char want[SRM_SIZE + 16];
   unsigned char conv[SRM_SIZE];
   unsigned char longer[SRM_SIZE + 16];
   unsigned before;
   size_t mark;
   int64_t t;

   memset(conv, 0, sizeof(conv));
   memset(conv, 0x33, 32);
   memcpy(longer, pattern, SRM_SIZE);
   memset(longer + SRM_SIZE, 0xab, 16);
   filestream_write_file(idle_bin, "mode:idle", 9);
   switch_to(idle_bin);

   /* An old-layout file converts; the new file comes at the close and
    * the old one stays; the conversion is logged once. */
   before = failures;
   lane_file(a, "convert", "old");
   lane_file(b, "convert", "srm");
   filestream_write_file(a, pattern, SRM_SIZE);
   mode_lane("convert", &mark);
   CHECK(strstr(core_log() + mark, "legacy 0 48 taken\nloaded 0 1632\n"),
         "convert:\n%s", core_log() + mark);
   after_close(want, conv);
   CHECK(file_holds(b, want, SRM_SIZE), "convert: the new file");
   CHECK(file_holds(a, pattern, SRM_SIZE), "convert: the old file changed");
   CHECK(log_count("Converted \"") == 1, "convert: not logged once");
   filestream_delete(a);
   filestream_delete(b);
   printf("[%s] convert\n", failures == before ? "ok" : "FAIL");

   /* The first name refused, the second taken; then every name
    * refused: the item as the core left it, still told, warned once. */
   before = failures;
   lane_file(a, "refuse", "old");
   lane_file(b, "refuse", "older");
   filestream_write_file(a, pattern, SRM_SIZE);
   filestream_write_file(b, pattern, SRM_SIZE);
   mode_lane("refuse", &mark);
   CHECK(strstr(core_log() + mark, "legacy 0 48 refused\n"
            "legacy 1 48 taken\nloaded 0 1632\n"),
         "refuse:\n%s", core_log() + mark);
   CHECK(lane_files("refuse", 0, 0), "refuse: files");
   filestream_delete(a);
   filestream_delete(b);
   lane_file(c, "refuse", "srm");
   filestream_delete(c);
   lane_file(a, "refuseall", "old");
   lane_file(b, "refuseall", "older");
   filestream_write_file(a, pattern, SRM_SIZE);
   filestream_write_file(b, pattern, SRM_SIZE);
   mode_lane("refuseall", &mark);
   CHECK(strstr(core_log() + mark, "legacy 0 48 refused\n"
            "legacy 1 48 refused\nloaded 0 0\n"),
         "refuseall:\n%s", core_log() + mark);
   CHECK(log_count("could not convert") == 1, "refuseall: not warned once");
   CHECK(lane_files("refuseall", 0, 0), "refuseall: files");
   CHECK(file_holds(a, pattern, SRM_SIZE) && file_holds(b, pattern,
            SRM_SIZE), "refuseall: an old file changed");
   filestream_delete(a);
   filestream_delete(b);
   lane_file(c, "refuseall", "srm");
   filestream_delete(c);
   printf("[%s] refuse\n", failures == before ? "ok" : "FAIL");

   /* Registered again from inside legacy_load() and loaded(): filled
    * and told once. */
   before = failures;
   lane_file(a, "reregister", "old");
   filestream_write_file(a, pattern, SRM_SIZE);
   mode_lane("reregister", &mark);
   CHECK(strstr(core_log() + mark, "legacy 0 48 taken\nloaded 0 1632\n"
            "loaded 1 0\nrun\n"), "reregister:\n%s", core_log() + mark);
   CHECK(lane_files("reregister", 0, 0), "reregister: files");
   filestream_delete(a);
   lane_file(c, "reregister", "srm");
   filestream_delete(c);
   printf("[%s] reregister\n", failures == before ? "ok" : "FAIL");

   /* An earlier name newer than the own file: warned once. */
   before = failures;
   lane_file(a, "fork", "old");
   lane_file(b, "fork", "srm");
   filestream_write_file(b, pattern, SRM_SIZE);
   filestream_write_file(a, conv, SRM_SIZE);
   if (path_get_mtime(b, &t))
      path_set_mtime(a, t + 100);
   mode_lane("fork", &mark);
   CHECK(!strstr(core_log() + mark, "legacy "), "fork: an old name read");
   CHECK(log_count("is newer than this one") == 1, "fork: not warned once");
   filestream_delete(a);
   filestream_delete(b);
   printf("[%s] fork\n", failures == before ? "ok" : "FAIL");

   /* The bytes past the item survive a game write and the close. */
   before = failures;
   lane_file(b, "tail", "srm");
   filestream_write_file(b, longer, sizeof(longer));
   mode_lane("tail", &mark);
   after_close(want, pattern);
   memset(want + SRM_SIZE, 0xab, 16);
   CHECK(file_holds(b, want, sizeof(want)), "tail: the end was lost");
   filestream_delete(b);
   printf("[%s] tail\n", failures == before ? "ok" : "FAIL");

   /* EXACT_SIZE: cut at the close, the copy in state_undo, logged. */
   before = failures;
   lane_file(b, "exact", "srm");
   filestream_write_file(b, longer, sizeof(longer));
   mode_lane("exact", &mark);
   after_close(want, pattern);
   CHECK(file_holds(b, want, SRM_SIZE), "exact: not cut");
   fill_pathname_join(c, dir, "assets/state_undo/save_core/exact/exact.srm",
         sizeof(c));
   CHECK(file_holds(c, longer, sizeof(longer)), "exact: no copy");
   CHECK(log_count("Kept a copy of") == 1, "exact: the copy not logged once");
   CHECK(lane_files("exact", 1, 0), "exact: files");
   filestream_delete(b);
   filestream_delete(c);
   printf("[%s] exact\n", failures == before ? "ok" : "FAIL");

   /* An old core: its 32 bytes written as today (before the unload),
    * the 48 kept, warned once. */
   before = failures;
   lane_file(b, "oldcore", "srm");
   filestream_write_file(b, pattern, SRM_SIZE);
   mode_lane("oldcore", &mark);
   memcpy(want, pattern, 32);
   want[2] = 0x77;
   CHECK(file_holds(b, want, 32), "oldcore: not today's .srm");
   fill_pathname_join(c, dir,
         "assets/state_undo/save_core/oldcore/oldcore.srm", sizeof(c));
   CHECK(file_holds(c, pattern, SRM_SIZE), "oldcore: no copy");
   CHECK(log_count("longer than this core's save memory") == 1,
         "oldcore: not warned once");
   CHECK(lane_files("oldcore", 1, 0), "oldcore: files");
   filestream_delete(b);
   filestream_delete(c);
   printf("[%s] oldcore\n", failures == before ? "ok" : "FAIL");

   /* Two writers for one file, twice: the newcomer is left out both
    * times, logged and told once; the item that held the file keeps
    * saving it. */
   before = failures;
   lane_file(b, "conflict", "srm");
   mode_lane("conflict", &mark);
   CHECK(!strstr(core_log() + mark, "registration refused"),
         "conflict:\n%s", core_log() + mark);
   CHECK(log_count("Item left out; it is not saved.") == 1,
         "conflict: not logged once");
   CHECK(log_count("could not be set up and is not being saved. "
            "Details are in the log.") == 1, "conflict: not told once");
   CHECK(path_is_valid(b), "conflict: the kept item not saved");
   filestream_delete(b);
   printf("[%s] conflict\n", failures == before ? "ok" : "FAIL");

   /* An item with no file is left out: the core reads why through
    * get_issues, the log names it once, the notice shows once with the
    * log hint, and the other item still saves. */
   before = failures;
   lane_file(b, "issues", "srm");
   mode_lane("issues", &mark);
   CHECK(strstr(core_log() + mark, "issue 1 22 1\n") != NULL,
         "issues: get_issues:\n%s", core_log() + mark);
   CHECK(log_count("save item 1 \"eep\": path = NULL: under ROOT_FRONTEND "
            "it is the extension. See libretro.h, retro_save_item::path. "
            "Item left out; it is not saved.") == 1,
         "issues: not logged once");
   CHECK(log_count("The core's save \"eep\" could not be set up and is "
            "not being saved. Details are in the log.") == 1,
         "issues: not told once");
   CHECK(path_is_valid(b), "issues: the other item not saved");
   filestream_delete(b);
   printf("[%s] issues\n", failures == before ? "ok" : "FAIL");

   /* A SET with no GET before it: logged once, and registered. */
   before = failures;
   lane_file(b, "noget", "srm");
   mode_lane("noget", &mark);
   CHECK(log_count("SET_SAVE_INTERFACE came before "
            "GET_SAVE_FRONTEND_INTERFACE") == 1, "noget: not logged once");
   CHECK(path_is_valid(b), "noget: not saved");
   filestream_delete(b);
   printf("[%s] noget\n", failures == before ? "ok" : "FAIL");
}

/* With logging off, the left-out notice still shows, once, without
 * the log hint; nothing is logged, and the other item still saves. */
static void lane_quiet(void)
{
   char b[PATH_MAX_LENGTH];
   size_t mark;
   msg_queue_entry_t e;
   unsigned shown           = 0;
   unsigned hinted          = 0;
   unsigned before          = failures;
   runloop_state_t *rs      = runloop_state_get_ptr();
   settings_t *settings     = config_get_ptr();
   bool font                = settings->bools.video_font_enable;
   /* rarch_main() sets the queue up; no frame takes notices off it. */
   runloop_msg_queue_init();
   settings->bools.video_font_enable = false;
   lane_file(b, "issues", "srm");
   verbosity_disable();
   mode_lane("issues", &mark);
   verbosity_enable();
   settings->bools.video_font_enable = font;
   while (     msg_queue_size(&rs->msg_queue)
         && msg_queue_extract(&rs->msg_queue, &e))
   {
      if (!strcmp(e.msg, "The core's save \"eep\" could not be set up "
               "and is not being saved."))
         shown++;
      if (strstr(e.msg, "Details are in the log."))
         hinted++;
   }
   runloop_msg_queue_deinit();
   rs->msg_queue_size = 0;
   CHECK(shown == 1 && hinted == 0, "quiet: told %u, with the hint %u",
         shown, hinted);
   CHECK(log_count("eep") == 0, "quiet: logged");
   CHECK(strstr(core_log() + mark, "issue 1 22 1\n") != NULL,
         "quiet: get_issues:\n%s", core_log() + mark);
   CHECK(path_is_valid(b), "quiet: the other item not saved");
   filestream_delete(b);
   printf("[%s] quiet\n", failures == before ? "ok" : "FAIL");
}

static const char *settle_word;

static bool settled(void)
{
   return strstr(core_log() + log_mark, settle_word) != NULL;
}

/* mode_lane(), with the game write once @word is in the core's log. */
static void mode_lane_after(const char *name, const char *word,
      size_t *core_mark)
{
   static char bin[PATH_MAX_LENGTH];
   mode_start(name, bin, core_mark);
   settle_word = word;
   run_until(settled);
   CHECK(settled(), "%s: no \"%s\"", name, word);
   core_wram()[2] = 0x77;
   switch_to(idle_bin);
   filestream_delete(bin);
}

static bool loaded_again(void)
{
   return strstr(core_log() + log_mark, "register again\n")
      && strstr(core_log() + log_mark, "loaded 1 ");
}

/* Another id at a file, a refused set, and a grow from inside
 * loaded(). */
/* A core that stops registering the item in save RAM's place: save RAM
 * is shorter, so the item's last image is copied, logged once; then
 * save RAM is written at each save and at the close, as before the
 * interface. */
static void lane_depart(void)
{
   static char b[PATH_MAX_LENGTH];
   static char c[PATH_MAX_LENGTH];
   static char bin[PATH_MAX_LENGTH];
   unsigned char want[SRM_SIZE];
   unsigned char ram[32];
   unsigned before = failures;
   unsigned i;
   size_t mark;

   lane_file(b, "depart", "srm");
   filestream_write_file(b, pattern, SRM_SIZE);
   mode_start("depart", bin, &mark);
   memcpy(want, pattern, SRM_SIZE);
   want[1] = 0x44;
   fill_pathname_join(c, dir,
         "assets/state_undo/save_core/depart/depart.srm", sizeof(c));
   for (i = 0; i < 600 && !file_holds(c, want, SRM_SIZE); i++)
   {
      runloop_iterate();
      task_queue_check();
   }
   CHECK(file_holds(c, want, SRM_SIZE), "depart: no copy of the last image");
   core_wram()[3] = 0x33;
   for (i = 0; i < 600 && !file_holds(b, core_wram(), 32); i++)
   {
      command_event(CMD_EVENT_SAVE_FILES, NULL);
      runloop_iterate();
      task_queue_check();
   }
   CHECK(file_holds(b, core_wram(), 32), "depart: save RAM not saved");
   core_wram()[2] = 0x77;
   command_event(CMD_EVENT_SAVE_FILES, NULL);
   CHECK(file_holds(b, core_wram(), 32), "depart: a later save not saved");
   core_wram()[4] = 0x55;
   memcpy(ram, core_wram(), sizeof(ram));
   switch_to(idle_bin);
   CHECK(file_holds(b, ram, sizeof(ram)), "depart: the close not saved");
   CHECK(file_holds(c, want, SRM_SIZE), "depart: the copy changed");
   CHECK(log_count("Kept a copy of") == 1, "depart: the copy not logged once");
   CHECK(lane_files("depart", 1, 0), "depart: files");
   filestream_delete(b);
   filestream_delete(c);
   filestream_delete(bin);
   printf("[%s] depart\n", failures == before ? "ok" : "FAIL");
}

static void lane_layout2(void)
{
   static char b[PATH_MAX_LENGTH];
   static char c[PATH_MAX_LENGTH];
   static char bin[PATH_MAX_LENGTH];
   unsigned char want[SRM_SIZE];
   unsigned char conv[SRM_SIZE];
   char line[80];
   unsigned i, sum;
   unsigned before;
   size_t mark;

   /* Another id at item 0's file with another tag: not refused, the
    * file copied before the first write, shown once. */
   before = failures;
   lane_file(b, "typechange", "srm");
   filestream_write_file(b, pattern, SRM_SIZE);
   mode_lane_after("typechange", "run\nloaded 0 ", &mark);
   CHECK(!strstr(core_log() + mark, "registration refused"),
         "typechange: refused");
   after_close(want, pattern);
   CHECK(file_holds(b, want, SRM_SIZE), "typechange: the new item");
   fill_pathname_join(c, dir,
         "assets/state_undo/save_core/typechange/typechange.srm", sizeof(c));
   CHECK(file_holds(c, pattern, SRM_SIZE), "typechange: no copy");
   CHECK(log_count("The save type of") == 1, "typechange: not shown once");
   CHECK(lane_files("typechange", 1, 0), "typechange: files");
   filestream_delete(b);
   filestream_delete(c);
   printf("[%s] typechange\n", failures == before ? "ok" : "FAIL");

   /* The same after a game write: the new item took the image still
    * waiting for the file, and the copy holds it, not the older
    * file. */
   before = failures;
   lane_file(b, "typewait", "srm");
   filestream_write_file(b, pattern, SRM_SIZE);
   memcpy(conv, pattern, SRM_SIZE);
   conv[1] = 0x44;
   mode_lane_after("typewait", "run\nloaded 0 ", &mark);
   after_close(want, conv);
   CHECK(file_holds(b, want, SRM_SIZE), "typewait: the new item");
   fill_pathname_join(c, dir,
         "assets/state_undo/save_core/typewait/typewait.srm", sizeof(c));
   CHECK(file_holds(c, conv, SRM_SIZE),
         "typewait: the copy is not the waiting image");
   CHECK(log_count("The save type of") == 1, "typewait: not shown once");
   CHECK(lane_files("typewait", 1, 0), "typewait: files");
   filestream_delete(b);
   filestream_delete(c);
   printf("[%s] typewait\n", failures == before ? "ok" : "FAIL");

   /* A cart moving port, the same tag and size under another id: the
    * record carries over, so nothing is filled or told and the change
    * made just before the move is kept; silent. */
   before = failures;
   lane_file(b, "tpakmove", "srm");
   filestream_write_file(b, pattern, SRM_SIZE);
   mode_lane_after("tpakmove", "run again\n", &mark);
   CHECK(strstr(core_log() + mark, "run\nrun again\n"),
         "tpakmove: filled again:\n%s", core_log() + mark);
   after_close(want, pattern);
   want[1] = 0x44;
   CHECK(file_holds(b, want, SRM_SIZE), "tpakmove: the change lost");
   fill_pathname_join(c, dir,
         "assets/state_undo/save_core/tpakmove/tpakmove.srm", sizeof(c));
   CHECK(!path_is_valid(c), "tpakmove: a copy");
   CHECK(     log_count("Kept a copy of") == 0
         && log_count("The save type of") == 0, "tpakmove: not silent");
   filestream_delete(b);
   printf("[%s] tpakmove\n", failures == before ? "ok" : "FAIL");

   /* A refused set after a game write: that image written at the next
    * save, nothing after it, the legacy .srm included; a later set is
    * filled from it, told and written again. */
   before = failures;
   lane_file(b, "dropset", "srm");
   filestream_write_file(b, pattern, SRM_SIZE);
   mode_start("dropset", bin, &mark);
   CHECK(strstr(core_log() + mark, "run\nregistration refused\n"),
         "dropset:\n%s", core_log() + mark);
   CHECK(log_count("its saves may not be written.") == 1,
         "dropset: not warned once");
   memcpy(want, pattern, SRM_SIZE);
   want[1]        = 0x44;
   core_wram()[2] = 0x77;
   command_event(CMD_EVENT_SAVE_FILES, NULL);
   CHECK(file_holds(b, want, SRM_SIZE), "dropset: not the ended image");
   log_mark = strlen(core_log());
   core_again();
   run_until(loaded_again);
   for (i = 0, sum = 0; i < SRM_SIZE; i++)
      sum += want[i];
   snprintf(line, sizeof(line), "register again\nloaded 0 %u\n", sum);
   CHECK(strstr(core_log() + log_mark, line), "dropset again:\n%s",
         core_log() + log_mark);
   switch_to(idle_bin);
   want[0] = 0x5a;
   CHECK(file_holds(b, want, SRM_SIZE), "dropset: the later set");
   CHECK(lane_files("dropset", 0, 0), "dropset: files");
   filestream_delete(b);
   filestream_delete(bin);
   printf("[%s] dropset\n", failures == before ? "ok" : "FAIL");

   /* A refused set and the full set again in one retro_run(): the new
    * items fill from the ended set's waiting image, not the older
    * file, so the change made before the refusal is kept; warned
    * once. */
   before = failures;
   lane_file(b, "dropnow", "srm");
   filestream_write_file(b, pattern, SRM_SIZE);
   memcpy(conv, pattern, SRM_SIZE);
   conv[1] = 0x44;
   for (i = 0, sum = 0; i < SRM_SIZE; i++)
      sum += conv[i];
   mode_lane_after("dropnow", "registration refused\nloaded 0 ", &mark);
   snprintf(line, sizeof(line),
         "run\nregistration refused\nloaded 0 %u\n", sum);
   CHECK(strstr(core_log() + mark, line), "dropnow:\n%s",
         core_log() + mark);
   CHECK(log_count("its saves may not be written.") == 1,
         "dropnow: not warned once");
   after_close(want, conv);
   CHECK(file_holds(b, want, SRM_SIZE), "dropnow: the change lost");
   CHECK(lane_files("dropnow", 0, 0), "dropnow: files");
   filestream_delete(b);
   printf("[%s] dropnow\n", failures == before ? "ok" : "FAIL");

   /* Grown from inside loaded() during a frame-start fill: filled from
    * the kept tail and told before the core runs again. */
   before = failures;
   lane_file(b, "grow", "srm");
   filestream_write_file(b, pattern, SRM_SIZE);
   for (i = 0, sum = 0; i < SRM_SIZE; i++)
      sum += pattern[i];
   mode_lane_after("grow", "run again\n", &mark);
   snprintf(line, sizeof(line), "run\nloaded 1 0\nloaded 0 %u\nrun again\n",
         sum);
   CHECK(strstr(core_log() + mark, line), "grow:\n%s", core_log() + mark);
   after_close(want, pattern);
   CHECK(file_holds(b, want, SRM_SIZE), "grow: the tail lost");
   filestream_delete(b);
   lane_file(c, "grow", "eep2");
   filestream_delete(c);
   printf("[%s] grow\n", failures == before ? "ok" : "FAIL");
}

/* The next recovery file a warning in the frontend's log names after
 * @at: 1 when it holds @data, 2 when it holds @other (if given), else
 * 0. Deleted once read, so a name given twice fails the second time. */
static int recovered_which(size_t *at, const unsigned char *data,
      const unsigned char *other)
{
   static const char key[] = "this game's save went to \"";
   static char p[PATH_MAX_LENGTH];
   int ok      = 0;
   void *buf   = NULL;
   int64_t len = 0;
   const char *s;
   const char *e;
   if (     filestream_read_file(ra_log, &buf, &len)
         && (size_t)len > *at
         && (s = strstr((const char*)buf + *at, key))
         && (e = strchr(s + sizeof(key) - 1, '"')))
   {
      s += sizeof(key) - 1;
      strlcpy(p, s, (size_t)(e - s) < sizeof(p)
            ? (size_t)(e - s) + 1 : sizeof(p));
      *at = (size_t)(e - (const char*)buf);
      if (file_holds(p, data, SRM_SIZE))
         ok = 1;
      else if (other && file_holds(p, other, SRM_SIZE))
         ok = 2;
      filestream_delete(p);
   }
   free(buf);
   return ok;
}

static bool recovered(size_t *at, const unsigned char *data)
{
   return recovered_which(at, data, NULL) == 1;
}

/* typewait whose copy can never be made (the assets folder is a
 * file): the file is never written over, the failed copy sends the
 * taken image to a recovery file of its own, and the item's image goes
 * to another, each later write replacing it there; each is named
 * once. */
static void lane_typelost(void)
{
   static char b[PATH_MAX_LENGTH];
   static char c[PATH_MAX_LENGTH];
   static char assets[PATH_MAX_LENGTH];
   static char app[PATH_MAX_LENGTH];
   unsigned char want[SRM_SIZE];
   unsigned char conv[SRM_SIZE];
   settings_t *settings = config_get_ptr();
   unsigned before      = failures;
   bool made_app        = false;
   size_t at;
   size_t mark;

   /* The recovery folder, made for the lane when missing. */
   if (     fill_pathname_application_data(app, sizeof(app))
         && !path_is_directory(app))
      made_app = path_mkdir(app);
   lane_file(b, "typelost", "srm");
   filestream_write_file(b, pattern, SRM_SIZE);
   lane_file(c, "typelost", "assets");
   filestream_write_file(c, pattern, SRM_SIZE);
   strlcpy(assets, settings->paths.directory_core_assets, sizeof(assets));
   strlcpy(settings->paths.directory_core_assets, c,
         sizeof(settings->paths.directory_core_assets));
   memcpy(conv, pattern, SRM_SIZE);
   conv[1] = 0x44;
   mode_lane_after("typelost", "run\nloaded 0 ", &mark);
   strlcpy(settings->paths.directory_core_assets, assets,
         sizeof(settings->paths.directory_core_assets));
   CHECK(file_holds(b, pattern, SRM_SIZE), "typelost: written over");
   CHECK(lane_files("typelost", 0, 2), "typelost: files");
   /* The taken image's file first; then the item's, which holds the
    * final write's image. */
   at = ra_log_mark;
   after_close(want, conv);
   CHECK(recovered(&at, conv), "typelost: the taken image");
   CHECK(recovered(&at, want), "typelost: the item's image");
   CHECK(log_count("this game's save went to") == 2,
         "typelost: not named twice");
   CHECK(log_count("The save type of") == 0, "typelost: not silent");
   filestream_delete(b);
   filestream_delete(c);
   if (made_app)
      path_rmdir(app);
   printf("[%s] typelost\n", failures == before ? "ok" : "FAIL");
}

/* typeretire waits for the old item's last image in its file, then for
 * a few frames. */
static const char *retire_path;
static const unsigned char *retire_image;
static unsigned retire_frames;

static bool retire_written(void)
{
   return file_holds(retire_path, retire_image, SRM_SIZE);
}

static bool retire_waited(void)
{
   return !retire_frames || !--retire_frames;
}

static char snapshot_srm[PATH_MAX_LENGTH];

/* Autosave has written the game's 0x77. */
static bool snapshot_written(void)
{
   void *buf = NULL;
   int64_t n = 0;
   bool done = filestream_read_file(snapshot_srm, &buf, &n)
      && n > 2 && ((unsigned char*)buf)[2] == 0x77;
   free(buf);
   return done;
}

static bool eep_copied(void)
{
   return core_eep_ready() != 0;
}

/* snapshot: closed while can_snapshot() says the game is still saving.
 * The file keeps the image autosave took while it said true: SAVE_FILES
 * and the write after retro_unload_game() add nothing, and no copy or
 * recovery file appears. */
static void lane_snapshot(void)
{
   static char bin[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();
   unsigned before      = failures;
   unsigned interval    = settings->uints.autosave_interval;
   void *done           = NULL;
   int64_t done_len     = 0;
   retro_time_t end;
   size_t mark;
   unsigned i;

   lane_file(snapshot_srm, "snapshot", "srm");
   settings->uints.autosave_interval = 1;
   mode_start("snapshot", bin, &mark);
   core_wram()[2] = 0x77;
   end = cpu_features_get_time_usec() + 30000000;
   while (!snapshot_written() && cpu_features_get_time_usec() < end)
      run_until(snapshot_written);
   CHECK(filestream_read_file(snapshot_srm, &done, &done_len)
         && done_len > 2 && ((unsigned char*)done)[2] == 0x77,
         "snapshot: autosave wrote nothing");
   /* The other item's worker asks on a clock of its own: it holds a
    * finished save too before the game starts saving. */
   while (!eep_copied() && cpu_features_get_time_usec() < end)
      run_until(eep_copied);
   CHECK(eep_copied(), "snapshot: eep never copied");
   /* Partway through the next save, for far fewer frames than the cap. */
   core_busy(1);
   core_wram()[2] = 0x99;
   core_wram()[3] = 0x99;
   for (i = 0; i < 30; i++)
   {
      runloop_iterate();
      task_queue_check();
   }
   switch_to(idle_bin);
   CHECK(done && file_holds(snapshot_srm, (const unsigned char*)done,
            (size_t)done_len), "snapshot: the close wrote over the last "
         "finished save");
   /* Both items, once for the close. */
   CHECK(log_count("Kept the last finished save") == 2,
         "snapshot: %u logs", log_count("Kept the last finished save"));
   CHECK(lane_files("snapshot", 0, 0), "snapshot: files");
   free(done);
   settings->uints.autosave_interval = interval;
   filestream_delete(bin);
   filestream_delete(snapshot_srm);
   printf("[%s] snapshot\n", failures == before ? "ok" : "FAIL");
}

/* @n passes of the main loop. */
static void passes(unsigned n)
{
   unsigned i;
   for (i = 0; i < n; i++)
   {
      runloop_iterate();
      task_queue_check();
   }
}

static char cap_srm[PATH_MAX_LENGTH];

static bool cap_written(void)
{
   void *buf = NULL;
   int64_t n = 0;
   bool done = filestream_read_file(cap_srm, &buf, &n)
      && n > 2 && ((unsigned char*)buf)[2] == 0x55;
   free(buf);
   return done;
}

/* cap: can_snapshot() says the game is still saving for good, at 5 fps.
 * Autosave copies anyway once the core has run 50 frames, 10 s of its
 * own, and logs it once per item. Paused passes, and passes whose
 * core_run() returns before retro_run(), as a netplay pause does, do
 * not count. */
static void lane_cap(void)
{
   static char bin[PATH_MAX_LENGTH];
   runloop_state_t *runloop_st = runloop_state_get_ptr();
   settings_t *settings        = config_get_ptr();
   unsigned before             = failures;
   unsigned interval           = settings->uints.autosave_interval;
   retro_time_t end;
   size_t mark;
   unsigned asks;

   lane_file(cap_srm, "cap", "srm");
   settings->uints.autosave_interval = 1;
   core_fps(5.0);
   mode_start("cap", bin, &mark);
   core_busy(1);
   core_wram()[2] = 0x55;
   asks = core_asks();
   end  = cpu_features_get_time_usec() + 30000000;
   while (core_asks() == asks && cpu_features_get_time_usec() < end)
      passes(1);
   CHECK(core_asks() != asks, "cap: autosave never asked");
   runloop_st->flags |= RUNLOOP_FLAG_PAUSED;
   passes(150);
   runloop_st->flags &= ~RUNLOOP_FLAG_PAUSED;
   asks = core_asks();
   runloop_st->content_closing = true;
   passes(150);
   runloop_st->content_closing = false;
   /* Asked on every pass: the copy is still waiting. */
   CHECK(core_asks() - asks >= 150 && !cap_written(), "cap: copied while "
         "the core did not run (%u asks)", core_asks() - asks);
   asks = core_asks();
   passes(40);
   CHECK(core_asks() - asks >= 40 && !cap_written(),
         "cap: copied before 50 frames (%u asks)", core_asks() - asks);
   /* Each item's worker asks on a clock of its own: an item not
    * copied yet at the close keeps its last finished save. */
   end = cpu_features_get_time_usec() + 30000000;
   while (   (!cap_written() || log_count("Autosave copied") < 2)
          && cpu_features_get_time_usec() < end)
      passes(1);
   CHECK(cap_written(), "cap: never copied");
   CHECK(log_count("Autosave copied") == 2, "cap: %u copies",
         log_count("Autosave copied"));
   core_fps(60.0);
   switch_to(idle_bin);
   CHECK(log_count("still saving") == 2, "cap: %u logs",
         log_count("still saving"));
   CHECK(!log_count("Kept the last finished save"), "cap: kept");
   CHECK(lane_files("cap", 0, 0), "cap: files");
   settings->uints.autosave_interval = interval;
   filestream_delete(bin);
   filestream_delete(cap_srm);
   printf("[%s] cap\n", failures == before ? "ok" : "FAIL");
}

/* typelost with autosave on, whose marked item a refused set ends
 * after a change: its retired worker cannot copy, so its last write
 * fails, and the main thread sends that image, and the taken one when
 * the item still held it, to the recovery folder, named on screen. The
 * file is never written over. Under load the orphan's write may do the
 * recovery instead; save_items_io_test pins the worker path. */
static void lane_typeretire(void)
{
   static char b[PATH_MAX_LENGTH];
   static char c[PATH_MAX_LENGTH];
   static char bin[PATH_MAX_LENGTH];
   static char assets[PATH_MAX_LENGTH];
   static char app[PATH_MAX_LENGTH];
   unsigned char conv[SRM_SIZE];
   unsigned char last[SRM_SIZE];
   settings_t *settings = config_get_ptr();
   unsigned before      = failures;
   unsigned interval    = settings->uints.autosave_interval;
   bool made_app        = false;
   unsigned w6;
   size_t at;
   size_t mark;

   if (     fill_pathname_application_data(app, sizeof(app))
         && !path_is_directory(app))
      made_app = path_mkdir(app);
   lane_file(b, "typeretire", "srm");
   filestream_write_file(b, pattern, SRM_SIZE);
   lane_file(c, "typeretire", "assets");
   filestream_write_file(c, pattern, SRM_SIZE);
   strlcpy(assets, settings->paths.directory_core_assets, sizeof(assets));
   strlcpy(settings->paths.directory_core_assets, c,
         sizeof(settings->paths.directory_core_assets));
   settings->uints.autosave_interval = 1;
   memcpy(conv, pattern, SRM_SIZE);
   conv[1] = 0x44;
   memcpy(last, conv, SRM_SIZE);
   last[2] = 0x66;
   mode_start("typeretire", bin, &mark);
   /* The old item's retired worker writes its last image and exits; the
    * frame's end reaps it and the next frame start starts the new
    * item's worker. Then the core ends the set. */
   retire_path   = b;
   retire_image  = conv;
   retire_frames = 4;
   run_until(retire_written);
   run_until(retire_waited);
   core_again();
   settle_word = "dropped\n";
   run_until(settled);
   CHECK(settled(), "typeretire: no \"dropped\"");
   settings->uints.autosave_interval = interval;
   switch_to(idle_bin);
   filestream_delete(bin);
   strlcpy(settings->paths.directory_core_assets, assets,
         sizeof(settings->paths.directory_core_assets));
   /* The old item's last image, written by its retired worker. */
   CHECK(   file_holds(b, pattern, SRM_SIZE)
         || file_holds(b, conv, SRM_SIZE), "typeretire: written over");
   at = ra_log_mark;
   w6 = log_count("this game's save went to");
   CHECK(lane_files("typeretire", 0, w6), "typeretire: files");
   /* The taken image: a second recovery file, named before or after
    * the retired image's, or the file when the new item's fill read it
    * there. */
   if (w6 == 2)
   {
      int first = recovered_which(&at, last, conv);
      CHECK(first + recovered_which(&at, last, conv) == 3,
            "typeretire: the retired and taken images");
   }
   else
   {
      CHECK(recovered(&at, last), "typeretire: the retired image");
      CHECK(w6 == 1 && file_holds(b, conv, SRM_SIZE),
            "typeretire: not named");
   }
   CHECK(log_count("The save type of") == 0, "typeretire: not silent");
   filestream_delete(b);
   filestream_delete(c);
   if (made_app)
      path_rmdir(app);
   printf("[%s] typeretire\n", failures == before ? "ok" : "FAIL");
}

/* The lanes' folders and idle's files, empty by now. */
static void layout_cleanup(void)
{
   static const char *const dirs[] = {
      "assets/state_undo/save_core/exact",
      "assets/state_undo/save_core/oldcore",
      "assets/state_undo/save_core/typechange",
      "assets/state_undo/save_core/typewait",
      "assets/state_undo/save_core", "assets/state_undo", "assets", NULL };
   static char p[PATH_MAX_LENGTH];
   unsigned i;
   filestream_delete(idle_bin);
   lane_file(p, "idle", "srm");
   filestream_delete(p);
   for (i = 0; dirs[i]; i++)
   {
      fill_pathname_join(p, dir, dirs[i], sizeof(p));
      path_rmdir(p);
   }
}

int main(int argc, char *argv[])
{
   static char core_path[PATH_MAX_LENGTH];
   static char cfg_path[PATH_MAX_LENGTH];
   static char a_bin[PATH_MAX_LENGTH];
   static char b_bin[PATH_MAX_LENGTH];
   static char text[PATH_MAX_LENGTH + 16];
   static char log_arg[PATH_MAX_LENGTH + 16];
   static char app_cfg[PATH_MAX_LENGTH];
   char *rarch_argv[8];
   int rarch_argc = 0;
   unsigned i;
   dylib_t lib;
   FILE *f;

#ifndef HAVE_TEST_DRIVERS
   /* The "null" joypad driver has no init: the first that opens,
    * udev's included, would be used. */
   printf("[skip] save_ordering_test: needs the test drivers\n");
   return 0;
#endif
   if (argc < 2)
   {
      fprintf(stderr, "usage: save_ordering_test <run dir>\n");
      return 2;
   }
   strlcpy(dir, argv[1], sizeof(dir));
   if (!path_mkdir(dir))
      return 1;
   path_resolve_realpath(dir, sizeof(dir), true);

   /* save_core sits next to the harness. Held open here too, so its
    * log outlives each retro_deinit(). */
   fill_pathname_basedir(core_path, argv[0], sizeof(core_path));
   fill_pathname_join(core_path, core_path, CORE_NAME, sizeof(core_path));
   path_resolve_realpath(core_path, sizeof(core_path), true);
   if (     !(lib = dylib_load(core_path))
         || !(core_log  = (core_log_t)dylib_proc(lib, "save_core_log"))
         || !(core_wram = (core_wram_t)dylib_proc(lib, "save_core_wram"))
         || !(core_card = (core_wram_t)dylib_proc(lib, "save_core_card"))
         || !(core_late = (core_late_t)dylib_proc(lib,
               "save_core_late_register"))
         || !(core_unserializes = (core_count_t)dylib_proc(lib,
               "save_core_unserializes"))
         || !(core_again = (core_again_t)dylib_proc(lib,
               "save_core_register_again"))
         || !(core_busy = (core_busy_t)dylib_proc(lib,
               "save_core_set_busy"))
         || !(core_asks = (core_count_t)dylib_proc(lib, "save_core_asks"))
         || !(core_eep_ready = (core_count_t)dylib_proc(lib,
               "save_core_eep_ready"))
         || !(core_fps = (core_fps_t)dylib_proc(lib, "save_core_set_fps")))
   {
      fprintf(stderr, "FAIL: cannot load %s\n", core_path);
      return 1;
   }

   fill_pathname_join(a_bin, dir, "a.bin", sizeof(a_bin));
   fill_pathname_join(b_bin, dir, "b.bin", sizeof(b_bin));
   fill_pathname_join(a_srm, dir, "a.srm", sizeof(a_srm));
   fill_pathname_join(a_eep, dir, "a.eep", sizeof(a_eep));
   fill_pathname_join(b_srm, dir, "b.srm", sizeof(b_srm));
   fill_pathname_join(b_eep, dir, "b.eep", sizeof(b_eep));
   fill_pathname_join(cfg_path, dir, "harness.cfg", sizeof(cfg_path));
   for (i = 0; i < SRM_SIZE; i++)
      pattern[i] = (unsigned char)(i ^ 0x3c);
   filestream_write_file(a_bin, "game a", 6);
   snprintf(text, sizeof(text), "check:%s", a_srm);
   filestream_write_file(b_bin, text, strlen(text));
   filestream_write_file(a_srm, pattern, sizeof(pattern));
   for (i = 0; i < CARDS; i++)
   {
      unsigned k;
      snprintf(text, sizeof(text), "card%u.mcr", i);
      fill_pathname_join(cards[i], dir, text, sizeof(cards[i]));
      for (k = 0; k < CARD_SIZE; k++)
         card_bytes[i][k] = (unsigned char)(0x40 + i * 16 + k);
      filestream_write_file(cards[i], card_bytes[i], CARD_SIZE);
   }

   if (!(f = fopen(cfg_path, "wb")))
      return 1;
   fprintf(f, "video_driver = \"null\"\n");
   fprintf(f, "audio_driver = \"null\"\n");
   fprintf(f, "input_driver = \"null\"\n");
   fprintf(f, "input_joypad_driver = \"test\"\n");
   fprintf(f, "midi_driver = \"null\"\n");
   fprintf(f, "microphone_driver = \"null\"\n");
   fprintf(f, "menu_driver = \"rgui\"\n");
   fprintf(f, "video_threaded = \"false\"\n");
   fprintf(f, "pause_nonactive = \"false\"\n");
   fprintf(f, "config_save_on_exit = \"false\"\n");
   fprintf(f, "savefile_directory = \"%s\"\n", dir);
   fprintf(f, "savestate_directory = \"%s\"\n", dir);
   fprintf(f, "sort_savefiles_enable = \"false\"\n");
   fprintf(f, "sort_savefiles_by_content_enable = \"false\"\n");
   fprintf(f, "savestate_auto_save = \"false\"\n");
   fprintf(f, "savestate_auto_load = \"false\"\n");
   fprintf(f, "save_file_compression = \"false\"\n");
   fprintf(f, "autosave_interval = \"0\"\n");
   fprintf(f, "core_assets_directory = \"%s/assets\"\n", dir);
   fclose(f);
   fill_pathname_join(ra_log, dir, "ra.log", sizeof(ra_log));
   fill_pathname_join(idle_bin, dir, "idle.bin", sizeof(idle_bin));
   snprintf(log_arg, sizeof(log_arg), "--log-file=%s", ra_log);
#ifndef _WIN32
   fill_pathname_join(app_cfg, dir, "config", sizeof(app_cfg));
   setenv("XDG_CONFIG_HOME", app_cfg, 1);
#endif

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
   rarch_argv[rarch_argc++] = a_bin;
   /* The layout lanes count the frontend's log lines. */
   rarch_argv[rarch_argc++] = (char*)"-v";
   rarch_argv[rarch_argc++] = log_arg;

   /* No playlist folder in the current directory, and no history. */
   retroarch_user_dirs_hold();
   if (!retroarch_main_init(rarch_argc, rarch_argv))
   {
      fprintf(stderr, "FAIL: retroarch_main_init failed\n");
      return 1;
   }

   lane_load();
   lane_switch(b_bin);
   lane_late();
   lane_layout();
   lane_quiet();
   lane_layout2();
   lane_depart();
   lane_typelost();
   lane_typeretire();
   lane_snapshot();
   lane_cap();
   lane_close();
   layout_cleanup();
   filestream_delete(ra_log);

   filestream_delete(a_bin);
   filestream_delete(b_bin);
   filestream_delete(a_srm);
   filestream_delete(b_srm);
   filestream_delete(cfg_path);
   for (i = 0; i < CARDS; i++)
      filestream_delete(cards[i]);
   /* The frontend's per-core folder, empty. */
   fill_pathname_join(text, dir, "save_core", sizeof(text));
   path_rmdir(text);
#ifndef _WIN32
   fill_pathname_join(text, app_cfg, "retroarch", sizeof(text));
   path_rmdir(text);
   path_rmdir(app_cfg);
#endif
   path_rmdir(dir);
   dylib_close(lib);

   if (failures)
   {
      fprintf(stderr, "FAIL save_ordering_test: %u failures\n", failures);
      return 1;
   }
   printf("save_ordering_test: all lanes pass\n");
   return 0;
}
