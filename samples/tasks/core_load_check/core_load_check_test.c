/* Copyright (C) 2026 The RetroArch team
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <dynamic/dylib.h>
#include <file/file_path.h>
#include <queues/task_queue.h>
#include <retro_timers.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "../../../core_load_check.h"
#include "../../../core_trust_runtime.h"
#include "../../../runloop.h"
#include "../../../tasks/tasks_internal.h"
#include "../../../verbosity.h"

#define TMP_DIR "core_load_check_test_tmp"
#ifdef _WIN32
#define LIB_EXT ".dll"
#define CORE_EXT "dll"
#else
#define LIB_EXT ".so"
#define CORE_EXT "so"
#endif

static unsigned checks;
static unsigned failures;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
   printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* Frontend stand-ins: what the modules call outside themselves */
const char *msg_hash_to_str(enum msg_hash_enums msg)
{
   static char buf[4][32];
   static unsigned n;
   char *s = buf[n++ & 3];
   sprintf(s, "msg %d", (int)msg);
   return s;
}

#define OSD_MAX 16
static char osd[OSD_MAX][160];
static unsigned osd_count;

void runloop_msg_queue_push(const char *msg, size_t len,
      unsigned prio, unsigned duration, bool flush,
      char *title, enum message_queue_icon icon,
      enum message_queue_category category)
{
   (void)len; (void)prio; (void)duration; (void)flush;
   (void)title; (void)icon; (void)category;
   if (osd_count < OSD_MAX)
      strlcpy(osd[osd_count], msg, sizeof(osd[0]));
   osd_count++;
}

bool fill_pathname_application_data(char *s, size_t len)
{
   strlcpy(s, TMP_DIR, len);
   return true;
}

/* The shared I/O window: one item per open, so hashing takes a
 * handler call per chunk */
void task_nbio_slice_open(nbio_budget_t *b)
{
   b->floor = 1;
}

void task_nbio_slice_close(nbio_budget_t *b)
{
   (void)b;
}

bool task_nbio_slice_within_budget(void *ud, size_t avail, size_t len)
{
   nbio_budget_t *b = (nbio_budget_t*)ud;
   (void)avail;
   (void)len;
   if (!b->floor)
      return false;
   b->floor = 0;
   return true;
}

static unsigned menu_opens;
static bool menu_can_open = true;

void menu_core_warning_open(void)
{
   menu_opens++;
}

bool menu_core_warning_can_open(void)
{
   return menu_can_open;
}

/* The last warning or error's format, enough to name the line */
static char last_log[256];

void RARCH_LOG(const char *fmt, ...)
{
   (void)fmt;
}

void RARCH_WARN(const char *fmt, ...)
{
   strlcpy(last_log, fmt, sizeof(last_log));
}

void RARCH_ERR(const char *fmt, ...)
{
   strlcpy(last_log, fmt, sizeof(last_log));
}

static void pump(unsigned n)
{
   while (n--)
   {
      task_queue_check();
      retro_sleep(1);
   }
}

static bool count_task(retro_task_t *task, void *ud)
{
   (void)task;
   (*(unsigned*)ud)++;
   return false;
}

/* Tasks pushed and not yet retired */
static unsigned queued(void)
{
   task_finder_data_t find;
   unsigned n    = 0;
   find.func     = count_task;
   find.userdata = &n;
   task_queue_find(&find);
   return n;
}

static void drain(void)
{
   unsigned i;
   for (i = 0; i < 5000 && queued(); i++)
      pump(1);
   pump(2);
}

/* Test library a or b as TMP_DIR/cores/<name>, a new file each time.
 * The old file is moved aside, not deleted: a freed inode handed to the
 * new file, with the same size inside one coarse timestamp tick, would
 * give it the old identity. False while a held handle keeps the old
 * file in place (Windows). */
static bool put_core(char *s, const char *name, char which)
{
   static unsigned aside_n;
   char aside[64];
   void *buf   = NULL;
   int64_t len = 0;
   bool ok     = false;
   sprintf(s, TMP_DIR "/cores/%s", name);
   if (!filestream_read_file(which == 'a'
            ? "core_load_check_test_a" LIB_EXT
            : "core_load_check_test_b" LIB_EXT, &buf, &len))
      return false;
   sprintf(aside, TMP_DIR "/cores/aside_%u", ++aside_n);
   if (!path_is_valid(s) || !filestream_rename(s, aside))
      ok = filestream_write_file(s, buf, len);
   free(buf);
   return ok;
}

static int lib_id(dylib_t lib)
{
   int (*fn)(void) = lib
      ? (int (*)(void))dylib_proc(lib, "core_load_test_id") : NULL;
   return fn ? fn() : 0;
}

/* What Always Trust records for path: its hash and identity */
static bool trust(const char *path)
{
   struct core_check c;
   size_t n            = 0;
   const uint8_t *keys = core_sig_builtin_keys(&n);
   core_trust_set_t *s = core_trust_runtime_get();
   bool ok;
   if (!s)
      return false;
   core_check_file(s, keys, n, path, 0, &c);
   core_trust_set_put(s);
   ok = core_trust_runtime_always(&c);
   drain();
   return ok;
}

static unsigned store_n;

static void start(bool threaded)
{
   char store[64];
   task_queue_init(threaded, NULL);
   sprintf(store, TMP_DIR "/trust_%u", ++store_n);
   path_mkdir(TMP_DIR "/cores");
   CHECK(core_trust_runtime_init(store, NULL, NULL));
}

static void stop(void)
{
   drain();
   core_load_check_deinit();
   core_trust_runtime_deinit();
   task_queue_deinit();
}

static void test_inactive(void)
{
   dylib_t lib = (dylib_t)1;
   char core[256];
   task_queue_init(false, NULL);
   path_mkdir(TMP_DIR "/cores");
   CHECK(put_core(core, "inactive_libretro" LIB_EXT, 'a'));
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_OFF);
   CHECK(!lib);
   CHECK(!core_load_check_refused(core));
   task_queue_deinit();
}

static void test_checks_off(void)
{
   dylib_t lib = NULL;
   char core[256];
   start(false);
   CHECK(put_core(core, "off_libretro" LIB_EXT, 'a'));
   CHECK(core_trust_runtime_set_checks(false));
   drain();
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_OPENED);
   CHECK(lib_id(lib) == 1);
   CHECK(queued() == 0);
   if (lib)
      dylib_close(lib);
   stop();
}

static void test_refused_cached(void)
{
   dylib_t lib = NULL;
   char core[256];
   char copy[256];
   start(false);
   CHECK(put_core(core, "unsigned_libretro" LIB_EXT, 'a'));
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   CHECK(!lib);
   CHECK(core_load_check_refused(core));
   CHECK(queued() == 1);
   drain();
   /* The quiet check answers the next try: no second hash */
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   CHECK(queued() == 0);
   /* A new file under that name is checked again */
   CHECK(put_core(core, "unsigned_libretro" LIB_EXT, 'b'));
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   CHECK(queued() == 1);
   drain();
   /* The store changed (these bytes trusted under another name), which
    * the quick check can't see: the refusal no longer answers */
   CHECK(put_core(copy, "unsigned_copy_libretro" LIB_EXT, 'b'));
   CHECK(trust(copy));
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   CHECK(queued() == 1);
   drain();
   /* Checks turned off: the quick check answers before a refusal */
   CHECK(put_core(core, "unsigned_libretro" LIB_EXT, 'a'));
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   drain();
   CHECK(core_trust_runtime_set_checks(false));
   drain();
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_OPENED);
   CHECK(lib_id(lib) == 1);
   if (lib)
      dylib_close(lib);
   stop();
}

static void test_attach(void)
{
   dylib_t lib = NULL;
   char core[256];
   start(false);
   CHECK(put_core(core, "twice_libretro" LIB_EXT, 'a'));
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   CHECK(queued() == 1);
   stop();
}

static void test_full_check(bool threaded)
{
   dylib_t lib = NULL;
   char core[256];
   start(threaded);
   CHECK(put_core(core, "hash_libretro" LIB_EXT, 'a'));
   CHECK(trust(core));
   /* The same bytes in a new file: its hash is trusted, not its identity */
   CHECK(put_core(core, "hash_libretro" LIB_EXT, 'a'));
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   drain();
#ifdef _WIN32
   /* No identity binds a quiet pass here, and no handle is kept for
    * it: the file can be replaced, and the next open checks again */
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   CHECK(queued() == 1);
   drain();
   CHECK(put_core(core, "hash_libretro" LIB_EXT, 'a'));
#else
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_OPENED);
   CHECK(lib_id(lib) == 1);
   if (lib)
      dylib_close(lib);
#endif
   stop();
}

static void test_slot_binds_file(void)
{
   dylib_t lib = NULL;
   char core[256];
   start(false);
   CHECK(put_core(core, "bind_libretro" LIB_EXT, 'a'));
   CHECK(trust(core));
   CHECK(put_core(core, "bind_libretro" LIB_EXT, 'a'));
#ifdef _WIN32
   /* Only a waited pass is kept here, with its handle, and nothing can
    * replace the file while it is held */
   core_load_check_set_startup(true);
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_OPENED);
   core_load_check_set_startup(false);
   if (lib)
      dylib_close(lib);
   lib = NULL;
   CHECK(!put_core(core, "bind_libretro" LIB_EXT, 'b'));
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_OPENED);
   CHECK(lib_id(lib) == 1);
   if (lib)
      dylib_close(lib);
#else
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   drain();
   /* Replaced after its check: the pass was for the file checked */
   CHECK(put_core(core, "bind_libretro" LIB_EXT, 'a'));
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   CHECK(!lib);
   CHECK(queued() == 1);
#endif
   stop();
}

/* D4: while the seed is pending, a core it covers is refused quietly
 * and not checked; nothing from before the seed answers after it */
static void test_seed_probe(void)
{
   dylib_t lib = NULL;
   char store[64];
   char core[256];
   void *buf   = NULL;
   int64_t len = 0;
   task_queue_init(false, NULL);
   sprintf(store, TMP_DIR "/trust_%u", ++store_n);
   path_mkdir(TMP_DIR "/seed0");
   strlcpy(core, TMP_DIR "/seed0/probe_libretro." CORE_EXT, sizeof(core));
   CHECK(filestream_read_file("core_load_check_test_b" LIB_EXT, &buf, &len));
   CHECK(buf && filestream_write_file(core, buf, len));
   free(buf);
   CHECK(core_trust_runtime_init(store, TMP_DIR "/seed0", CORE_EXT));
   CHECK(core_trust_runtime_seed_pending());
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   CHECK(core_load_check_refused(core));
   /* Only the seed's own task */
   CHECK(queued() == 1);
   drain();
   CHECK(!core_trust_runtime_seed_pending());
#ifdef _WIN32
   /* No identity record here: checked again, not answered from before */
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   CHECK(queued() == 1);
#else
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_OPENED);
   CHECK(lib_id(lib) == 2);
   if (lib)
      dylib_close(lib);
#endif
   stop();
}

/* Cancelled before its hash ended: no verdict, so checked again */
static void test_cancelled(void)
{
   dylib_t lib = NULL;
   char core[256];
   start(false);
   CHECK(put_core(core, "cancel_libretro" LIB_EXT, 'b'));
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   CHECK(queued() == 1);
   task_queue_reset();
   drain();
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   CHECK(!lib);
   CHECK(queued() == 1);
   if (lib)
      dylib_close(lib);
   stop();
}

/* Unreadable or gone: the caller's error, not a check's refusal */
static void test_unopenable(void)
{
   dylib_t lib = NULL;
   start(false);
   last_log[0] = '\0';
   CHECK(core_load_check_open(TMP_DIR "/cores/missing_libretro" LIB_EXT,
            &lib) == CORE_LOAD_GATE_REFUSED);
   CHECK(!core_load_check_refused(TMP_DIR "/cores/missing_libretro" LIB_EXT));
   CHECK(strstr(last_log, "Could not open") != NULL);
   CHECK(queued() == 0);
   stop();
}

static void test_startup_inline(void)
{
   dylib_t lib = NULL;
   char good[256];
   char bad[256];
   start(false);
   CHECK(put_core(good, "start_good_libretro" LIB_EXT, 'a'));
   CHECK(trust(good));
   CHECK(put_core(good, "start_good_libretro" LIB_EXT, 'a'));
   CHECK(put_core(bad, "start_bad_libretro" LIB_EXT, 'b'));
   core_load_check_set_startup(true);
   /* Checked whole in place: no frame passes before the answer */
   CHECK(core_load_check_open(good, &lib) == CORE_LOAD_GATE_OPENED);
   CHECK(lib_id(lib) == 1);
   if (lib)
      dylib_close(lib);
   lib = NULL;
   CHECK(core_load_check_open(bad, &lib) == CORE_LOAD_GATE_REFUSED);
   core_load_check_set_startup(false);
   CHECK(core_load_check_refused(bad));
   drain();
   CHECK(core_load_check_open(bad, &lib) == CORE_LOAD_GATE_REFUSED);
   CHECK(queued() == 0);
   stop();
}

static void test_deinit_drops(void)
{
   dylib_t lib = NULL;
   char core[256];
   start(false);
   CHECK(put_core(core, "gen_libretro" LIB_EXT, 'a'));
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   core_load_check_deinit();
   drain();
   /* A result from before the deinit answers nothing */
   CHECK(core_load_check_open(core, &lib) == CORE_LOAD_GATE_REFUSED);
   CHECK(queued() == 1);
   stop();
}

int main(void)
{
   if (!path_mkdir(TMP_DIR))
      return 1;
   test_inactive();
   test_checks_off();
   test_refused_cached();
   test_attach();
   test_full_check(false);
   test_full_check(true);
   test_slot_binds_file();
   test_startup_inline();
   test_deinit_drops();
   test_cancelled();
   test_seed_probe();
   test_unopenable();
   printf("%u checks, %u failures\n", checks, failures);
   return failures ? 1 : 0;
}
