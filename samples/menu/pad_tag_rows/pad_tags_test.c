/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2026 - The RetroArch team
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* Controllers of one name told apart. Links the shipping objects with
 * only main() replaced (the playlist_nav pattern) and drives the real
 * frame loop with the test joypad driver, whose script connects pads
 * named "(vid:pid) name@@port@@serial". Each phase checks the tag the
 * frontend gives each pad.
 * Requires a completed non-Qt build:
 *   ./configure --disable-qt && make
 *   samples/menu/pad_tag_rows/build.sh
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <compat/strl.h>
#include <lists/file_list.h>
#include <queues/task_queue.h>
#include <time/rtime.h>
#include <file/config_file.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <features/features_cpu.h>

#include "../../../msg_hash_lbl_str.h"
#include "../../../configuration.h"
#include "../../../retroarch.h"
#include "../../../runloop.h"
#include "../../../menu/menu_cbs.h"
#include "../../../menu/menu_driver.h"
#include "../../../menu/menu_entries.h"
#include "../../../input/input_driver.h"
#include "../../../input/input_pad_tag.h"
#include "../../../tasks/tasks_internal.h"
#include "../../../gfx/video_driver.h"
#include "../../../frontend/frontend_driver.h"

/* The script's frames: a phase is checked SETTLE frames after its last
 * step, with every connect it made applied. */
#define F_CONNECT     10
#define F_SWAP_OUT    60
#define F_SWAP_IN     80
#define F_SERIAL_OUT 130
#define F_SERIAL_IN  150
#define F_ORDER_OUT  170
#define F_ORDER_IN   180
#define F_CLASH_OUT  200
#define F_CLASH_IN   220
#define F_NOID_OUT   270
#define F_NOID_IN    290
#define F_SHOWN_OUT  340
#define F_SHOWN_IN   360
#define SETTLE         8

#define PAD "(045e:028e) Harness Pad"

/* The identities PAD's ports and serial give (samples/input/pad_tag
 * pins them); the words they pick come from the list. */
#define ID_PORT1   0x8c6c6606UL
#define ID_PORT2   0x8b6c6473UL
#define ID_SERIAL9 0xa1e19035UL
#define ID_PORT11  0x45a4e895UL
#define ID_PORT22  0xbca1d253UL

static unsigned failures;
static unsigned steps;

static void check(int ok, const char *fmt, ...)
{
   va_list ap;
   if (ok)
      return;
   fprintf(stderr, "FAIL: ");
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fprintf(stderr, "\n");
   failures++;
}

/* A test joypad step: action 1 adds pad @num named @str, 2 removes it,
 * 16 + pad presses the buttons in mask @num, 32 + pad releases them. */
static void step(FILE *f, unsigned frame, unsigned action, unsigned num,
      const char *str)
{
   fprintf(f, "%s{ \"action\": %u, \"param_num\": %u, ",
         steps++ ? "," : "", action, num);
   if (str)
      fprintf(f, "\"param_str\": \"%s\", ", str);
   fprintf(f, "\"frame\": %u }\n", frame);
}

static void write_script(FILE *f)
{
   fprintf(f, "[\n");
   step(f, F_CONNECT,    1, 0, PAD "@@port-1");
   step(f, F_CONNECT,    1, 1, PAD "@@port-2");
   step(f, F_CONNECT,    1, 2, "(045e:028e) Other Pad@@port-3");
   step(f, F_SWAP_OUT,   2, 0, NULL);
   step(f, F_SWAP_OUT,   2, 1, NULL);
   step(f, F_SWAP_IN,    1, 0, PAD "@@port-2");
   step(f, F_SWAP_IN,    1, 1, PAD "@@port-1");
   step(f, F_SERIAL_OUT, 2, 1, NULL);
   step(f, F_SERIAL_IN,  1, 1, PAD "@@port-1@@serial-9");
   step(f, F_ORDER_OUT,  2, 0, NULL);
   step(f, F_ORDER_OUT,  2, 1, NULL);
   step(f, F_ORDER_IN,   1, 0, PAD "@@port-11");
   step(f, F_ORDER_IN,   1, 1, PAD "@@port-22");
   step(f, F_CLASH_OUT,  2, 0, NULL);
   step(f, F_CLASH_OUT,  2, 1, NULL);
   step(f, F_CLASH_IN,   1, 0, PAD "@@port-22");
   step(f, F_CLASH_IN,   1, 1, PAD "@@port-11");
   step(f, F_NOID_OUT,   2, 1, NULL);
   step(f, F_NOID_IN,    1, 1, PAD);
   step(f, F_SHOWN_OUT,  2, 1, NULL);
   step(f, F_SHOWN_IN,   1, 1, "(045e:028e) Wireless Pad@@port-1");
   fprintf(f, "]\n");
}

/* Real frames, each followed by the task queue as rarch_main() runs
 * it, until the script is SETTLE frames past @frame and every connect
 * and disconnect it made has been applied. */
static void run_until(unsigned frame)
{
   unsigned n = 0;
   while (     (video_driver_get_frame_count() <= (uint64_t)(frame + SETTLE)
            || input_autoconfigure_pending())
         && n++ < 2000)
   {
      runloop_iterate();
      task_queue_check();
   }
   check(n <= 2000, "frame %u: the script never settled", frame);
}

/* @fmt with its %s the word @ident picks; four calls fit in one
 * expression */
static const char *with_word(const char *fmt, unsigned long ident)
{
   static char bufs[4][NAME_MAX_LENGTH];
   static unsigned next;
   char *s = bufs[next++ & 3];
   snprintf(s, NAME_MAX_LENGTH, fmt, input_pad_tag_word((uint32_t)ident));
   return s;
}

static void check_tag(unsigned slot, const char *want, const char *when)
{
   char s[32];
   input_config_get_device_tag(slot, s, sizeof(s));
   check(string_is_equal(s, want), "%s: slot %u's tag is \"%s\", not \"%s\"",
         when, slot, s, want);
}

static void check_ident(unsigned slot, unsigned long want, const char *when)
{
   unsigned long got = (unsigned long)input_config_get_device_ident(slot);
   check(got == want, "%s: slot %u's identity is %08lx, not %08lx",
         when, slot, got, want);
}

/* Two pads of one name and one of its own */
static void phase_connect(void)
{
   run_until(F_CONNECT);
   check_tag(0, with_word(" (%s)", ID_PORT1), "two Harness Pads");
   check_tag(1, with_word(" (%s)", ID_PORT2), "two Harness Pads");
   check_tag(2, "",         "a name of its own");
   check_ident(0, ID_PORT1, "port-1");
   check_ident(1, ID_PORT2, "port-2");
}

/* The word follows the pad, not the slot */
static void phase_swap(void)
{
   run_until(F_SWAP_IN);
   check_tag(0, with_word(" (%s)", ID_PORT2),
         "the pads in each other's slots");
   check_tag(1, with_word(" (%s)", ID_PORT1),
         "the pads in each other's slots");
}

/* Alone, no tag; a serial wins over the port */
static void phase_serial(void)
{
   run_until(F_SERIAL_OUT);
   check_tag(0, "", "one Harness Pad left");
   check_ident(0, ID_PORT2, "one Harness Pad left");
   run_until(F_SERIAL_IN);
   check_tag(1, with_word(" (%s)", ID_SERIAL9), "a pad with a serial");
   check_ident(1, ID_SERIAL9, "a pad with a serial");
   check_tag(0, with_word(" (%s)", ID_PORT2), "beside a pad with a serial");
}

/* Two identities picking one word are numbered in identity order:
 * port-11's is the lower, in either slot */
static void phase_clash_order(void)
{
   run_until(F_ORDER_IN);
   check_ident(0, ID_PORT11, "port-11 first");
   check_ident(1, ID_PORT22, "port-11 first");
   check_tag(0, with_word(" (%s #1)", ID_PORT11), "port-11 first");
   check_tag(1, with_word(" (%s #2)", ID_PORT22), "port-11 first");
}

static void phase_clash(void)
{
   run_until(F_CLASH_IN);
   check_ident(0, ID_PORT22, "port-22");
   check_ident(1, ID_PORT11, "port-11");
   check_tag(0, with_word(" (%s #2)", ID_PORT22), "two pads picking one word");
   check_tag(1, with_word(" (%s #1)", ID_PORT11), "two pads picking one word");
}

/* One identity twice (a cloned serial): the port path decides; with
 * one path too, the slot */
static void phase_clash_tie(void)
{
   char phys[NAME_MAX_LENGTH];
   const char *own = input_config_get_device_phys(1);

   strlcpy(phys, own ? own : "", sizeof(phys));
   input_config_set_device_ident(1, ID_PORT22);
   check_tag(0, with_word(" (%s #2)", ID_PORT22), "one identity twice");
   check_tag(1, with_word(" (%s #1)", ID_PORT22), "one identity twice");
   input_config_set_device_phys(1, input_config_get_device_phys(0));
   check_tag(0, with_word(" (%s #1)", ID_PORT22), "one identity and path");
   check_tag(1, with_word(" (%s #2)", ID_PORT22), "one identity and path");
   input_config_set_device_phys(1, phys);
   input_config_set_device_ident(1, ID_PORT11);
}

/* No identity: the number */
static void phase_noid(void)
{
   run_until(F_NOID_IN);
   check_tag(1, " (#2)", "a pad with no identity");
   check_ident(1, 0, "a pad with no identity");
   check_tag(0, with_word(" (%s)", ID_PORT22),
         "beside a pad with no identity");
}

/* The name the menus show decides, not the driver's: as a profile's
 * input_device_display_name would, which the harness sets directly
 * (its pads load no profile). */
static void phase_shown(void)
{
   input_config_set_device_display_name(1, "Wireless Pad");
   check_tag(0, "", "one name, shown apart");
   check_tag(1, "", "one name, shown apart");
   run_until(F_SHOWN_IN);
   check_tag(0, "", "two names");
   check_tag(1, "", "two names");
   input_config_set_device_display_name(1, "Harness Pad");
   check(!input_config_get_device_name_index(1),
         "two names: slot 1's name_index is %u",
         input_config_get_device_name_index(1));
   check_tag(0, with_word(" (%s)", ID_PORT22), "two names shown as one");
   check_tag(1, with_word(" (%s)", ID_PORT1), "two names shown as one");
   check_tag(2, "",         "beside two names shown as one");
}

int main(int argc, char *argv[])
{
   FILE *f;
   char dir[512];
   static char cfg_path[640];
   char ratst_path[640];
   char *rarch_argv[8];
   int rarch_argc  = 0;
   const char *tmp = getenv("TMPDIR");

   (void)argc;
   (void)argv;
   if (!tmp || !*tmp)
      tmp = "/tmp";
   snprintf(dir, sizeof(dir), "%s/pad_tags_%lu", tmp,
         (unsigned long)cpu_features_get_time_usec());
   if (!path_mkdir(dir))
      return 1;
   snprintf(cfg_path,   sizeof(cfg_path),   "%s/harness.cfg", dir);
   snprintf(ratst_path, sizeof(ratst_path), "%s/pads.ratst",  dir);

   if (!(f = fopen(ratst_path, "wb")))
      return 1;
   write_script(f);
   fclose(f);
   if (!(f = fopen(cfg_path, "wb")))
      return 1;
   fprintf(f, "video_driver = \"null\"\n");
   fprintf(f, "video_threaded = \"false\"\n");
   fprintf(f, "video_vsync = \"false\"\n");
   fprintf(f, "audio_driver = \"null\"\n");
   fprintf(f, "audio_enable = \"false\"\n");
   fprintf(f, "input_driver = \"null\"\n");
   fprintf(f, "input_joypad_driver = \"test\"\n");
   fprintf(f, "test_input_file_joypad = \"%s\"\n", ratst_path);
   /* no profile: the pads bind nothing, so a press does nothing in
    * the menu but what is checked here */
   fprintf(f, "input_autodetect_enable = \"false\"\n");
   fprintf(f, "menu_driver = \"rgui\"\n");
   fprintf(f, "rgui_config_directory = \"%s\"\n", dir);
   fprintf(f, "pause_nonactive = \"false\"\n");
   fprintf(f, "gamemode_enable = \"false\"\n");
   fprintf(f, "suspend_screensaver_enable = \"false\"\n");
   fprintf(f, "config_save_on_exit = \"false\"\n");
   fclose(f);

   rarch_argv[rarch_argc++] = (char*)"retroarch";
   rarch_argv[rarch_argc++] = (char*)"--menu";
   rarch_argv[rarch_argc++] = (char*)"--config";
   rarch_argv[rarch_argc++] = cfg_path;

   /* Same prelude as rarch_main(); see playlist_nav. */
   config_file_set_io_default(config_file_io_filestream());
   rtime_init();
   retroarch_config_init();
   retroarch_ctl(RARCH_CTL_STATE_FREE, NULL);
   frontend_driver_init_first(NULL);
   runloop_msg_queue_init();
   if (!retroarch_main_init(rarch_argc, rarch_argv))
   {
      fprintf(stderr, "FAIL: retroarch_main_init failed\n");
      return 1;
   }
   retroarch_menu_running();

   phase_connect();
   phase_swap();
   phase_serial();
   phase_clash_order();
   phase_clash();
   phase_clash_tie();
   phase_noid();
   phase_shown();

   filestream_delete(ratst_path);
   filestream_delete(cfg_path);
   filestream_delete(dir);

   if (failures)
   {
      fprintf(stderr, "pad_tags_test: %u failure(s)\n", failures);
      return 1;
   }
   printf("pad_tags_test: controllers of one name are told apart\n");
   return 0;
}
