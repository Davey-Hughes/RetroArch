/* A core with two frontend-owned save items, for save_ordering_test.
 * It logs what it sees at each call the frontend makes, so the harness
 * can check the order: the fill before loaded(), loaded() before the
 * first retro_run(), and the final write after retro_unload_game().
 * A content file holding "check:<path>" makes retro_load_game() log
 * the first byte of <path>. save_core_late_register() makes a later
 * retro_run() register a third item, "card", mid-game. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <libretro.h>

#define WRAM_SIZE 32
#define IRAM_SIZE 16
#define EEP_SIZE  8
#define CARD_SIZE 16

static uint8_t wram[WRAM_SIZE];
static uint8_t iram[IRAM_SIZE];
static uint8_t eep[EEP_SIZE];
static uint8_t card[CARD_SIZE];
static uint8_t state[16];
static uint32_t frame[16 * 16];
static char log_buf[4096];
static char card_path[512];
static struct retro_save_segment srm_segs[2];
static struct retro_save_segment eep_seg;
static struct retro_save_segment card_seg;
static struct retro_save_item items[3];
static retro_video_refresh_t video_cb;
static retro_environment_t   environ_cb;
static unsigned unserializes;
static int late_frames = -1;
static int late_run;
static int late_after_rollback;
static int late_rolled;
static int in_set;
static int ran;
/* "mode:<word>" content: the layout lanes. */
static char mode[16];
static int rereg_legacy;
static int rereg_loaded;
static int conflicting;
/* What GET_SAVE_FRONTEND_INTERFACE gave. */
static struct retro_save_frontend_interface front;
/* typechange, typewait, typelost, typeretire, tpakmove, grow: the first
 * retro_run() registered again. */
static int moved;
/* grow: item 0 covers iram too. */
static int grown;
/* dropset: two items with one id; then the full set again. */
static int dup_id;
/* Refused: version 0. */
static int no_version;
static int again;
/* snapshot, cap: can_snapshot() says the game is still saving; its
 * calls for item 0. */
static int busy;
static unsigned asks;
/* snapshot: can_snapshot() said true for item 1 since the load. */
static unsigned eep_ready;
static double fps = 60.0;
static unsigned runs;
static const char *const legacy_names[] = { "old", "older", NULL };

static void register_items(unsigned n);

const char *save_core_log(void) { return log_buf; }
uint8_t *save_core_wram(void) { return wram; }
uint8_t *save_core_card(void) { return card; }
unsigned save_core_unserializes(void) { return unserializes; }
/* dropset: register the full set on the next retro_run(). */
void save_core_register_again(void) { again = 1; }
void save_core_set_busy(int on) { busy = on; }
unsigned save_core_asks(void) { return asks; }
unsigned save_core_eep_ready(void) { return eep_ready; }
/* The rate the next load reports. */
void save_core_set_fps(double rate) { fps = rate; }

/* After @frames more retro_run() calls, register "card" at @path.
 * With @after_rollback (run-ahead), "run after" waits for the next
 * retro_unserialize(): the frames run before it in the same frame are
 * run-ahead's own, rolled back. */
void save_core_late_register(unsigned frames, const char *path,
      int after_rollback)
{
   strncpy(card_path, path, sizeof(card_path) - 1);
   late_after_rollback = after_rollback;
   late_rolled         = 0;
   late_frames         = (int)frames;
}

static void log_line(const char *line)
{
   size_t len = strlen(log_buf);
   if (len + strlen(line) + 2 < sizeof(log_buf))
   {
      strcpy(log_buf + len, line);
      strcat(log_buf, "\n");
   }
}

static unsigned sum(const uint8_t *a, size_t n, const uint8_t *b, size_t m)
{
   unsigned s = 0;
   size_t i;
   for (i = 0; i < n; i++)
      s += a[i];
   for (i = 0; i < m; i++)
      s += b[i];
   return s;
}

static void RETRO_CALLCONV item_loaded(unsigned item)
{
   char line[64];
   if (in_set)
      log_line("loaded in set");
   snprintf(line, sizeof(line), "loaded %u %u", item, item == 0
         ? sum(wram, WRAM_SIZE, iram, IRAM_SIZE)
         : item == 1 ? sum(eep, EEP_SIZE, NULL, 0)
         : sum(card, CARD_SIZE, NULL, 0));
   log_line(line);
   if (rereg_loaded)
   {
      rereg_loaded = 0;
      register_items(2);
   }
   /* grow: item 0 over its kept tail, from inside loaded(). */
   if (item == 1 && moved && !grown && !strcmp(mode, "grow"))
   {
      grown = 1;
      register_items(2);
   }
}

static int wants_legacy(void)
{
   return !strcmp(mode, "convert") || !strcmp(mode, "refuse")
      || !strcmp(mode, "refuseall") || !strcmp(mode, "reregister")
      || !strcmp(mode, "fork");
}

/* Converts by filling wram with 0x33; "refuse" refuses the first
 * name, "refuseall" every one. */
static bool RETRO_CALLCONV item_legacy_load(unsigned item, unsigned index,
      const char *format, const void *buf, size_t len)
{
   char line[64];
   int take = strcmp(mode, "refuseall")
      && (strcmp(mode, "refuse") || index > 0);
   (void)item;
   (void)format;
   (void)buf;
   snprintf(line, sizeof(line), "legacy %u %u %s", index, (unsigned)len,
         take ? "taken" : "refused");
   log_line(line);
   if (!take)
      return false;
   memset(wram, 0x33, sizeof(wram));
   if (rereg_legacy)
   {
      rereg_legacy = 0;
      register_items(2);
   }
   return true;
}

static bool RETRO_CALLCONV item_can_snapshot(unsigned item)
{
   if (!item)
      asks++;
   else if (item == 1 && !busy)
      eep_ready++;
   return !busy;
}

static void register_items(unsigned n)
{
   struct retro_save_interface iface;
   srm_segs[0].data = wram;
   srm_segs[0].size = WRAM_SIZE;
   srm_segs[1].data = iram;
   srm_segs[1].size = IRAM_SIZE;
   eep_seg.data     = eep;
   eep_seg.size     = EEP_SIZE;
   card_seg.data    = card;
   card_seg.size    = CARD_SIZE;
   memset(items, 0, sizeof(items));
   items[0].id            = "srm";
   items[0].path          = "srm";
   items[0].storage       = RETRO_SAVE_STORAGE_FRONTEND_BUFFER;
   items[0].root          = RETRO_SAVE_ROOT_FRONTEND;
   items[0].segments      = srm_segs;
   items[0].num_segments  = 2;
   items[0].legacy_memory = RETRO_MEMORY_SAVE_RAM;
   items[1]               = items[0];
   items[1].id            = "eep";
   items[1].path          = "eep";
   items[1].segments      = &eep_seg;
   items[1].num_segments  = 1;
   items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   if (wants_legacy())
      items[0].legacy_paths = legacy_names;
   if (!strcmp(mode, "exact"))
      items[0].flags        = RETRO_SAVE_FLAG_EXACT_SIZE;
   /* Two writers for one file: the newcomer is left out. */
   if (conflicting)
      items[1].path         = "srm";
   /* Refused: two items with one id. */
   if (dup_id)
      items[1].id           = "srm";
   /* Another id at item 0's file: another tag, or a cart moving port. */
   if (     !strcmp(mode, "typechange") || !strcmp(mode, "typewait")
         || !strcmp(mode, "typelost") || !strcmp(mode, "typeretire"))
   {
      items[0].format       = moved ? "save_core:b" : "save_core:a";
      items[0].id           = moved ? "srm2" : "srm";
   }
   if (!strcmp(mode, "tpakmove"))
   {
      items[0].format       = "gb:sram";
      items[0].id           = moved ? "tpak2" : "tpak1";
      items[0].port         = moved ? 2 : 1;
   }
   if (!strcmp(mode, "grow"))
   {
      items[0].num_segments = grown ? 2 : 1;
      items[1].path         = moved ? "eep2" : "eep";
   }
   items[2]               = items[1];
   items[2].id            = "card";
   items[2].path          = card_path;
   items[2].root          = RETRO_SAVE_ROOT_ABSOLUTE;
   items[2].segments      = &card_seg;
   memset(&iface, 0, sizeof(iface));
   iface.interface_version = no_version ? 0 : 1;
   iface.num_items         = n;
   iface.item_size         = sizeof(items[0]);
   iface.items             = items;
   iface.loaded            = item_loaded;
   iface.can_snapshot      = strcmp(mode, "snapshot") && strcmp(mode, "cap")
      ? NULL : item_can_snapshot;
   iface.legacy_load       = wants_legacy() ? item_legacy_load : NULL;
   /* issues: an item with no file, left out; the core reads why. */
   if (!strcmp(mode, "issues"))
      items[1].path        = NULL;
   in_set                  = 1;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_SAVE_INTERFACE, &iface))
      log_line("registration refused");
   in_set                  = 0;
   if (!strcmp(mode, "issues") && front.get_issues)
   {
      const struct retro_save_issue *issues = NULL;
      unsigned i, count = front.get_issues(&issues);
      for (i = 0; i < count; i++)
      {
         char line[64];
         snprintf(line, sizeof(line), "issue %u %u %u", issues[i].item,
               issues[i].field, issues[i].verdict);
         log_line(line);
      }
   }
}


void retro_set_environment(retro_environment_t cb)
{
   bool no_game = false;
   environ_cb   = cb;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
void retro_set_input_poll(retro_input_poll_t cb) { (void)cb; }
void retro_set_input_state(retro_input_state_t cb) { (void)cb; }
void retro_init(void) { }

/* The frontend's last write must have read the buffers already. */
void retro_deinit(void)
{
   memset(wram, 0xee, sizeof(wram));
   memset(iram, 0xee, sizeof(iram));
   memset(eep, 0xee, sizeof(eep));
   memset(card, 0xee, sizeof(card));
   log_line("deinit");
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "save_core";
   info->library_version  = "1";
   info->valid_extensions = "bin";
   info->need_fullpath    = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->geometry.base_width   = 16;
   info->geometry.base_height  = 16;
   info->geometry.max_width    = 16;
   info->geometry.max_height   = 16;
   info->timing.fps            = fps;
   info->timing.sample_rate    = 48000.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
void retro_reset(void) { }

/* "run after": the next retro_run() after a mid-game registration,
 * which loaded() must come before. */
void retro_run(void)
{
   if (!ran)
      log_line("run");
   if (!ran && !strcmp(mode, "conflict"))
   {
      conflicting = 1;
      register_items(2);
      register_items(2);
      conflicting = 0;
   }
   if (!ran && (!strcmp(mode, "typechange") || !strcmp(mode, "typewait")
            || !strcmp(mode, "typelost") || !strcmp(mode, "typeretire")
            || !strcmp(mode, "tpakmove") || !strcmp(mode, "grow")))
   {
      /* tpakmove, typewait, typelost, typeretire: a change the move
       * must not read over. */
      if (     !strcmp(mode, "tpakmove") || !strcmp(mode, "typewait")
            || !strcmp(mode, "typelost") || !strcmp(mode, "typeretire"))
         wram[1] = 0x44;
      moved = 1;
      register_items(2);
   }
   if (!ran && (!strcmp(mode, "dropset") || !strcmp(mode, "dropnow")))
   {
      wram[1] = 0x44;
      dup_id  = 1;
      register_items(2);
      dup_id  = 0;
      /* dropnow: the full set again at once. */
      if (!strcmp(mode, "dropnow"))
         register_items(2);
   }
   /* typeretire, once asked: a change, then a refused set ends the
    * marked item while its worker owes the copy. */
   if (again && !strcmp(mode, "typeretire"))
   {
      again       = 0;
      wram[2]     = 0x66;
      no_version  = 1;
      register_items(2);
      no_version  = 0;
      log_line("dropped");
   }
   if (again)
   {
      again = 0;
      log_line("register again");
      register_items(2);
   }
   if (     runs++ == 1
         && (!strcmp(mode, "grow") || !strcmp(mode, "tpakmove")))
      log_line("run again");
   ran = 1;
   if (late_frames > 0 && --late_frames == 0)
   {
      late_frames = -1;
      late_run    = 1;
      log_line("register card");
      register_items(3);
   }
   else if (late_run && (!late_after_rollback || late_rolled))
   {
      late_run = 0;
      log_line("run after");
   }
   state[0]++;
   video_cb(frame, 16, 16, 16 * sizeof(uint32_t));
}

/* A state without the save items, for run-ahead. */
size_t retro_serialize_size(void) { return sizeof(state); }

bool retro_serialize(void *data, size_t size)
{
   if (size < sizeof(state))
      return false;
   memcpy(data, state, sizeof(state));
   return true;
}

bool retro_unserialize(const void *data, size_t size)
{
   if (size < sizeof(state))
      return false;
   memcpy(state, data, sizeof(state));
   unserializes++;
   if (late_run)
      late_rolled = 1;
   return true;
}
void retro_cheat_reset(void) { }
void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }

static void check_file(const char *content_path)
{
   char text[512];
   char line[64];
   size_t n;
   int byte;
   FILE *f = fopen(content_path, "rb");
   if (!f)
      return;
   n = fread(text, 1, sizeof(text) - 1, f);
   fclose(f);
   text[n] = '\0';
   if (!strncmp(text, "mode:", 5))
   {
      strncpy(mode, text + 5, sizeof(mode) - 1);
      rereg_legacy = rereg_loaded = !strcmp(mode, "reregister");
      return;
   }
   if (strncmp(text, "check:", 6))
      return;
   byte = -1;
   if ((f = fopen(text + 6, "rb")))
   {
      byte = fgetc(f);
      fclose(f);
   }
   snprintf(line, sizeof(line), "check %02x", byte & 0xff);
   log_line(line);
}

bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   struct retro_save_frontend_interface fi;
   const char *base;
   char line[600];

   ran         = 0;
   runs        = 0;
   busy        = 0;
   eep_ready   = 0;
   moved       = 0;
   grown       = 0;
   late_frames = -1;
   late_run    = 0;
   mode[0]     = '\0';
   memset(wram, 0, sizeof(wram));
   memset(iram, 0, sizeof(iram));
   memset(eep, 0, sizeof(eep));
   memset(card, 0, sizeof(card));
   base = game && game->path ? strrchr(game->path, '/') : NULL;
   snprintf(line, sizeof(line), "load %s", base ? base + 1 : "?");
   log_line(line);
   if (game && game->path)
      check_file(game->path);

   memset(&fi, 0, sizeof(fi));
   fi.interface_version = 1;
   fi.storage_kinds     =
      RETRO_SAVE_STORAGE_BIT(RETRO_SAVE_STORAGE_FRONTEND_BUFFER);
   fi.item_flags        = RETRO_SAVE_FLAG_EXACT_SIZE;
   fi.features          = RETRO_SAVE_FEATURE_LEGACY_LOAD;
   memset(&front, 0, sizeof(front));
   if (!strcmp(mode, "oldcore"))
      log_line("old core");
   /* noget: a SET with no GET before it. */
   else if (!strcmp(mode, "noget"))
      register_items(2);
   else if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_FRONTEND_INTERFACE, &fi)
         || fi.instance != RETRO_SAVE_INSTANCE_PRIMARY)
      log_line("no save interface");
   else
   {
      front = fi;
      register_items(2);
   }
   return environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}

bool retro_load_game_special(unsigned t, const struct retro_game_info *g, size_t n)
{ (void)t; (void)g; (void)n; return false; }

/* An emulated power-off that writes the save. */
void retro_unload_game(void)
{
   wram[0] = 0x5a;
   log_line("unload");
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

/* Save RAM keeps answering for the frontend's other users. */
void *retro_get_memory_data(unsigned id)
{
   return id == RETRO_MEMORY_SAVE_RAM ? wram : NULL;
}
size_t retro_get_memory_size(unsigned id)
{
   return id == RETRO_MEMORY_SAVE_RAM ? WRAM_SIZE : 0;
}
