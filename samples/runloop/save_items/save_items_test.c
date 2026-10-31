/* The save item registry: negotiation, validation, deep copy, matching
 * by id, naming, earlier names, file conflicts and legacy retirement.
 * save_items.c is linked as it is; the logger is captured. */
/* The checks are asserts: keep them in every build. */
#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <lists/string_list.h>
#include <retro_miscellaneous.h>

#include "../../../save_items.h"

/* save_items_header.c, built as C89 */
unsigned header_sum_values(void);
void header_fill_frontend(struct retro_save_frontend_interface *fi);
void header_fill_item(struct retro_save_item *item,
      const struct retro_save_segment *segments, unsigned num_segments);
void header_fill_interface(struct retro_save_interface *iface,
      const struct retro_save_item *items, unsigned num_items);
void header_fill_entry(struct retro_save_entry *entry,
      struct retro_save_icon *icon);

/* Lines logged since logs_clear(), with their level: L, W or E. */
static char logs[64][1024];
static char log_levels[64];
static unsigned num_logs;

static void log_line(char level, const char *fmt, va_list ap)
{
   if (num_logs >= 64)
      return;
   vsnprintf(logs[num_logs], sizeof(logs[0]), fmt, ap);
   log_levels[num_logs++] = level;
}

void RARCH_LOG(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   log_line('L', fmt, ap);
   va_end(ap);
}

void RARCH_WARN(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   log_line('W', fmt, ap);
   va_end(ap);
}

void RARCH_ERR(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   log_line('E', fmt, ap);
   va_end(ap);
}

static void logs_clear(void)
{
   num_logs = 0;
}

/* Lines at @level since logs_clear(). */
static unsigned logs_at(char level)
{
   unsigned i, n = 0;
   for (i = 0; i < num_logs; i++)
      if (log_levels[i] == level)
         n++;
   return n;
}

/* "[SRAM] @line\n" was logged at @level since logs_clear(), once. */
static bool logged_once(char level, const char *line)
{
   unsigned i, n = 0;
   char want[1024];
   snprintf(want, sizeof(want), "[SRAM] %s\n", line);
   for (i = 0; i < num_logs; i++)
      if (log_levels[i] == level && !strcmp(logs[i], want))
         n++;
   return n == 1;
}

static unsigned char wram[64];
static unsigned char iram[128];
static struct retro_save_segment segs[2];

static bool RETRO_CALLCONV test_legacy_load(unsigned item, unsigned index,
      const char *format, const void *buf, size_t len)
{
   (void)item;
   (void)index;
   (void)format;
   (void)buf;
   (void)len;
   return true;
}

static struct save_items_paths plain_paths(void)
{
   struct save_items_paths p;
   memset(&p, 0, sizeof(p));
   p.core                = "TestCore";
   p.savefile            = "/saves/Game.v1.2.srm";
   p.savefile_dir        = "/saves";
   p.system_dir          = "/bios";
   p.content             = "/roms/nes/Game.v1.2.nes";
   p.savestate           = "/states/Game.v1.2.state";
   p.replay              = "/states/Game.v1.2.replay";
   p.disk_index          = "/saves/Game.v1.2.ldci";
   p.savefile_dir_is_dir = true;
   return p;
}

static struct retro_save_item item_named(const char *id)
{
   struct retro_save_item it;
   header_fill_item(&it, segs, 2);
   it.id = id;
   return it;
}

/* An item no check finds anything in. */
static struct retro_save_item clean_item(const char *id)
{
   struct retro_save_item it = item_named(id);
   it.include      = NULL;
   it.exclude      = NULL;
   it.legacy_paths = NULL;
   return it;
}

static void iface_of(struct retro_save_interface *iface,
      const struct retro_save_item *items, unsigned n)
{
   header_fill_interface(iface, items, n);
}

static const save_items_set_t *cur(void)
{
   return save_items_state_get_ptr()->current;
}

static const save_items_set_t *prev(void)
{
   return save_items_state_get_ptr()->previous;
}

static void test_negotiate(void)
{
   struct retro_save_frontend_interface fi;

   assert(save_items_get_frontend_interface(NULL,
         RETRO_SAVE_INSTANCE_PRIMARY));

   header_fill_frontend(&fi);
   fi.interface_version = 2;
   assert(!save_items_get_frontend_interface(&fi,
         RETRO_SAVE_INSTANCE_PRIMARY));

   header_fill_frontend(&fi);
   assert(save_items_get_frontend_interface(&fi,
         RETRO_SAVE_INSTANCE_PRIMARY));
   assert(fi.interface_version == 1);
   assert(fi.instance == RETRO_SAVE_INSTANCE_PRIMARY);
   assert(fi.storage_kinds == SAVE_ITEMS_STORAGE_KINDS);
   assert(fi.storage_kinds == 0x7f);
   assert(fi.item_flags == SAVE_ITEMS_ITEM_FLAGS);
   assert(fi.item_flags == 0x3ff);
   assert(fi.features == SAVE_ITEMS_FEATURES);
   assert(fi.features == RETRO_SAVE_FEATURE_LEGACY_LOAD);
   assert(fi.get_path && fi.notify);

   /* The frontend only clears bits. */
   header_fill_frontend(&fi);
   fi.storage_kinds = RETRO_SAVE_STORAGE_BIT(RETRO_SAVE_STORAGE_CORE_FILE);
   fi.item_flags    = 0;
   assert(save_items_get_frontend_interface(&fi,
         RETRO_SAVE_INSTANCE_SECONDARY));
   assert(fi.storage_kinds
         == RETRO_SAVE_STORAGE_BIT(RETRO_SAVE_STORAGE_CORE_FILE));
   assert(fi.item_flags == 0);
   assert(fi.instance == RETRO_SAVE_INSTANCE_SECONDARY);
}

/* Refused: nothing is current, and the set that was is previous,
 * dropped, until the end of the call. */
static void expect_refused(const struct retro_save_interface *iface)
{
   struct save_items_paths p = plain_paths();
   const save_items_set_t *before = cur();
   assert(!save_items_register(iface, &p));
   assert(!cur());
   assert(prev() == before);
   assert(save_items_state_get_ptr()->dropped == (before != NULL));
   save_items_drop_previous();
   assert(!prev() && !save_items_state_get_ptr()->dropped);
}

/* Registered, with item @i left out: no file, no retirement. */
static void expect_left_out(const struct retro_save_interface *iface,
      unsigned i)
{
   struct save_items_paths p = plain_paths();
   assert(save_items_register(iface, &p));
   assert(cur()->items[i].left_out && !cur()->items[i].path);
   assert(!save_items_frontend_owned(&cur()->items[i]));
   assert(cur()->items[i].item.legacy_memory == RETRO_SAVE_NO_LEGACY_MEMORY);
   assert(!save_items_take_refusal());
   save_items_drop_previous();
}

static void test_validation(void)
{
   struct retro_save_interface iface;
   struct retro_save_item items[2];
   struct retro_save_segment bad[1];
   struct save_items_paths p = plain_paths();

   /* Twice, so a refusal has a previous set to leave alone too. */
   items[0] = item_named("base");
   iface_of(&iface, items, 1);
   assert(save_items_register(&iface, &p));
   assert(save_items_register(&iface, &p));
   assert(cur() && cur()->num_items == 1);
   assert(prev() && prev()->num_items == 1);

   expect_refused(NULL);

   iface_of(&iface, items, 1);
   iface.item_size = SAVE_ITEMS_ITEM_MIN_SIZE - 1;
   expect_refused(&iface);

   iface_of(&iface, NULL, 1);
   expect_refused(&iface);

   items[0] = item_named(NULL);
   iface_of(&iface, items, 1);
   expect_refused(&iface);

   items[0] = item_named("same");
   items[1] = item_named("same");
   iface_of(&iface, items, 2);
   expect_refused(&iface);

   items[0] = item_named("buf");
   items[0].segments = NULL;
   iface_of(&iface, items, 1);
   expect_left_out(&iface, 0);

   items[0] = item_named("buf");
   items[0].num_segments = 0;
   iface_of(&iface, items, 1);
   expect_left_out(&iface, 0);

   bad[0].data = NULL;
   bad[0].size = 16;
   items[0] = item_named("buf");
   items[0].segments     = bad;
   items[0].num_segments = 1;
   iface_of(&iface, items, 1);
   expect_left_out(&iface, 0);

   items[0] = item_named("blob");
   items[0].storage = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   iface_of(&iface, items, 1);
   iface.blob_save = NULL;
   expect_left_out(&iface, 0);

   items[0] = item_named("req");
   iface_of(&iface, items, 1);
   iface.get_result = NULL;
   expect_refused(&iface);

   /* Zero items is a set; a core file needs no callbacks. */
   iface_of(&iface, NULL, 0);
   assert(save_items_register(&iface, &p));
   assert(cur()->num_items == 0);
   items[0] = item_named("card");
   items[0].storage  = RETRO_SAVE_STORAGE_CORE_FILE;
   items[0].segments = NULL;
   iface_of(&iface, items, 1);
   iface.request    = NULL;
   iface.get_result = NULL;
   iface.blob_size  = NULL;
   iface.blob_save  = NULL;
   iface.blob_load  = NULL;
   assert(save_items_register(&iface, &p));
   save_items_clear();
   assert(!cur() && !prev());
}

static char *dup_str(const char *s)
{
   char *d = (char*)malloc(strlen(s) + 1);
   assert(d);
   strcpy(d, s);
   return d;
}

static void test_deep_copy(void)
{
   struct
   {
      struct retro_save_item item;
      uint64_t newer_member;
   } wide[2];
   struct retro_save_interface iface;
   struct retro_save_segment *heap_segs;
   const char **legacy;
   char *id;
   char *label;
   const struct retro_save_item *c;
   struct save_items_paths p = plain_paths();

   heap_segs = (struct retro_save_segment*)malloc(2 * sizeof(*heap_segs));
   legacy    = (const char**)malloc(3 * sizeof(*legacy));
   assert(heap_segs && legacy);
   heap_segs[0].data = wram;
   heap_segs[0].size = sizeof(wram);
   heap_segs[1].data = iram;
   heap_segs[1].size = sizeof(iram);
   legacy[0] = dup_str("sav");
   legacy[1] = dup_str("old");
   legacy[2] = NULL;
   id        = dup_str("wram");
   label     = dup_str("Battery RAM");

   /* A core built against a newer header: a wider stride. */
   memset(wide, 0xa5, sizeof(wide));
   header_fill_item(&wide[0].item, heap_segs, 2);
   wide[0].item.flags       |= RETRO_SAVE_FLAG_EXACT_SIZE;
   wide[0].item.id           = id;
   wide[0].item.label        = label;
   wide[0].item.legacy_paths = legacy;
   header_fill_item(&wide[1].item, segs, 2);
   wide[1].item.id           = "rtc";
   wide[1].item.storage      = RETRO_SAVE_STORAGE_CORE_FILE;
   wide[1].item.root         = RETRO_SAVE_ROOT_SYSTEM_DIR;
   wide[1].item.path         = "clock.bin";
   wide[1].item.legacy_memory = RETRO_MEMORY_RTC;
   header_fill_interface(&iface, &wide[0].item, 2);
   iface.item_size   = sizeof(wide[0]);
   iface.legacy_load = test_legacy_load;

   assert(save_items_register(&iface, &p));

   /* Everything the core owned may go now; only segment data stays.
    * Scribbled first, so a shallow copy fails without ASan too. */
   memset((void*)legacy[0], 'X', strlen(legacy[0]));
   memset((void*)legacy[1], 'X', strlen(legacy[1]));
   memset(id, 'X', strlen(id));
   memset(label, 'X', strlen(label));
   free((void*)legacy[0]);
   free((void*)legacy[1]);
   free((void*)legacy);
   free(id);
   free(label);
   memset(heap_segs, 0, 2 * sizeof(*heap_segs));
   free(heap_segs);
   memset(wide, 0, sizeof(wide));

   assert(cur()->num_items == 2);
   assert(cur()->iface.items == NULL);
   assert(cur()->iface.entries == &cur()->entries);
   assert(cur()->entries.list != NULL);
   assert(cur()->iface.request && cur()->iface.blob_load);
   assert(cur()->iface.legacy_load == test_legacy_load);
   assert(!strcmp(cur()->items[0].legacy[0], "/saves/Game.v1.2.sav"));
   assert(!strcmp(cur()->items[0].legacy[1], "/saves/Game.v1.2.old"));
   assert(!cur()->items[0].legacy[2]);
   assert(!strcmp(cur()->items[1].legacy[0], "/bios/old"));
   assert(!strcmp(cur()->items[1].legacy[1], "/bios/older"));

   c = &cur()->items[0].item;
   assert(!strcmp(c->id, "wram"));
   assert(!strcmp(c->label, "Battery RAM"));
   assert(!strcmp(c->group, "cart"));
   assert(!strcmp(c->scope_key, "us"));
   assert(!strcmp(c->game_key, "serial:SLUS-00594"));
   assert(!strcmp(c->path, "srm"));
   assert(!strcmp(c->format, "nes:wram+n163"));
   assert(!strcmp(c->base_hash, "sha1:00"));
   assert(!strcmp(c->legacy_paths[0], "sav"));
   assert(!strcmp(c->legacy_paths[1], "old"));
   assert(c->legacy_paths[2] == NULL);
   assert(!strcmp(c->include[0], "slot1") && !c->include[1]);
   assert(!strcmp(c->exclude[0], "cache") && !c->exclude[1]);
   assert(!strcmp(c->accepts[0], "nes:wram") && !c->accepts[1]);
   assert(c->num_segments == 2);
   assert(c->segments[0].data == wram);
   assert(c->segments[0].size == sizeof(wram));
   assert(c->segments[1].data == iram);
   assert(c->storage == RETRO_SAVE_STORAGE_FRONTEND_BUFFER);
   assert(c->legacy_memory == RETRO_MEMORY_SAVE_RAM);
   assert(c->write_timing == RETRO_SAVE_WRITE_FRONTEND);
   assert(c->flags == SAVE_ITEMS_ITEM_FLAGS);

   c = &cur()->items[1].item;
   assert(!strcmp(c->id, "rtc"));
   assert(c->legacy_memory == RETRO_MEMORY_RTC);
   assert(!strcmp(cur()->items[1].path, "/bios/clock.bin"));

   save_items_clear();
}

static void register_ids(const char *const *ids, unsigned n)
{
   unsigned i;
   struct retro_save_interface iface;
   struct retro_save_item items[4];
   struct save_items_paths p = plain_paths();
   for (i = 0; i < n; i++)
   {
      items[i]      = item_named(ids[i]);
      items[i].path = ids[i];
   }
   iface_of(&iface, items, n);
   assert(save_items_register(&iface, &p));
}

static void test_matching(void)
{
   static const char *const a[] = { "wram", "rtc", "card" };
   static const char *const b[] = { "card", "wram" };
   static const char *const c[] = { "x", "card", "wram", "y" };

   register_ids(a, 3);
   assert(!prev());
   assert(cur()->items[0].prev == SAVE_ITEMS_NONE);

   register_ids(b, 2);
   assert(cur()->num_items == 2);
   assert(cur()->items[0].prev == 2);
   assert(cur()->items[1].prev == 0);
   assert(prev() && prev()->num_items == 3);
   assert(prev()->items[0].next == 1);
   assert(prev()->items[1].next == SAVE_ITEMS_NONE);
   assert(prev()->items[2].next == 0);
   /* The replaced set's buffers are still there to be read. */
   assert(prev()->items[1].item.segments[0].data == wram);
   save_items_drop_previous();
   assert(!prev());

   register_ids(c, 4);
   assert(cur()->items[0].prev == SAVE_ITEMS_NONE);
   assert(cur()->items[1].prev == 0);
   assert(cur()->items[2].prev == 1);
   assert(cur()->items[3].prev == SAVE_ITEMS_NONE);

   /* A set nobody dropped goes with the next registration. */
   register_ids(a, 3);
   assert(prev()->num_items == 4);
   assert(prev()->items[0].next == SAVE_ITEMS_NONE);
   assert(prev()->items[2].next == 0);

   save_items_clear();
   assert(!cur() && !prev());
}

/* RetroArch's own naming for one subsystem memory, as
 * runloop_path_init_subsystem() writes it. */
static void subsystem_oracle(char *out, size_t len, const char *content,
      const char *savefile_dir, bool dir_is_dir, const char *ext)
{
   char savename[NAME_MAX_LENGTH];
   fill_pathname(savename, content, "", sizeof(savename));
   if (dir_is_dir)
   {
      strlcpy(out, savefile_dir, len);
      fill_pathname_dir(out, savename, ext, len);
   }
   else
      fill_pathname(out, savename, ext, len);
}

static const char *path_of(unsigned root, const char *path,
      unsigned content_index, const struct save_items_paths *p)
{
   static char buf[PATH_MAX_LENGTH];
   struct retro_save_frontend_interface fi;
   struct retro_save_interface iface;
   struct retro_save_item items[1];
   size_t need;

   items[0]               = item_named("x");
   items[0].storage       = RETRO_SAVE_STORAGE_CORE_FILE;
   items[0].root          = root;
   items[0].path          = path;
   items[0].content_index = content_index;
   iface_of(&iface, items, 1);
   assert(save_items_register(&iface, p));
   save_items_drop_previous();

   header_fill_frontend(&fi);
   assert(save_items_get_frontend_interface(&fi,
         RETRO_SAVE_INSTANCE_PRIMARY));
   need = fi.get_path(0, NULL, 0);
   if (!need)
      return NULL;
   assert(fi.get_path(0, buf, sizeof(buf)) == need);
   assert(strlen(buf) == need);
   return buf;
}

static void test_naming(void)
{
   static char want[PATH_MAX_LENGTH];
   static char long_content[PATH_MAX_LENGTH];
   static char long_ext[NAME_MAX_LENGTH];
   char small[8];
   unsigned i, j;
   size_t _len;
   const char *got;
   struct retro_save_frontend_interface fi;
   struct save_items_paths p = plain_paths();
   struct string_list *subsystem;
   union string_list_elem_attr attr;
   static const char *const exts[] = { ".srm", ".rtc", ".sav2" };

   /* Beside content 0's .srm, the way .rtc is named. */
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_FRONTEND, "srm", 0, &p),
         "/saves/Game.v1.2.srm"));
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_FRONTEND, "rtc", 0, &p),
         "/saves/Game.v1.2.rtc"));
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_FRONTEND, "srm2", 0, &p),
         "/saves/Game.v1.2.srm2"));
   assert(!path_of(RETRO_SAVE_ROOT_FRONTEND, "srm", 1, &p));
   assert(!path_of(RETRO_SAVE_ROOT_FRONTEND, NULL, 0, &p));
   /* An extension that would leave the save dir gets no name. */
   assert(!path_of(RETRO_SAVE_ROOT_FRONTEND, "srm/../../x", 0, &p));
   assert(!path_of(RETRO_SAVE_ROOT_FRONTEND, "/tmp/x", 0, &p));
   assert(!path_of(RETRO_SAVE_ROOT_FRONTEND, "srm\\x", 0, &p));
   assert(!path_of(RETRO_SAVE_ROOT_FRONTEND, "..", 0, &p));
   /* So does one too long to keep whole. */
   memset(long_ext, 'a', NAME_MAX_LENGTH - 2);
   long_ext[NAME_MAX_LENGTH - 2] = '\0';
   assert((got = path_of(RETRO_SAVE_ROOT_FRONTEND, long_ext, 0, &p)));
   assert(strlen(got) == strlen("/saves/Game.v1.2.") + NAME_MAX_LENGTH - 2);
   long_ext[NAME_MAX_LENGTH - 2] = 'a';
   long_ext[NAME_MAX_LENGTH - 1] = '\0';
   assert(!path_of(RETRO_SAVE_ROOT_FRONTEND, long_ext, 0, &p));

   /* A --save override is save RAM's name whatever it ends in. */
   p.savefile = "/elsewhere/mine.sav";
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_FRONTEND, "srm", 0, &p),
         "/elsewhere/mine.sav"));
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_FRONTEND, "rtc", 0, &p),
         "/elsewhere/mine.rtc"));
   p = plain_paths();

   /* Per content in a subsystem session, as save RAM is named there. */
   attr.i    = 0;
   subsystem = string_list_new();
   assert(subsystem);
   assert(string_list_append(subsystem, "/roms/gb/Pokemon Red.gb", attr));
   assert(string_list_append(subsystem, "/roms/gb/Tetris.v1.1.gb", attr));
   p.subsystem = subsystem;
   for (j = 0; j < 2; j++)
   {
      p.savefile_dir_is_dir = j == 0;
      for (i = 0; i < 2; i++)
      {
         unsigned e;
         for (e = 0; e < 3; e++)
         {
            subsystem_oracle(want, sizeof(want), subsystem->elems[i].data,
                  p.savefile_dir, p.savefile_dir_is_dir, exts[e]);
            assert(!strcmp(path_of(RETRO_SAVE_ROOT_FRONTEND,
                  exts[e] + 1, i, &p), want));
         }
      }
   }
   p.savefile_dir_is_dir = true;
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_FRONTEND, "srm", 1, &p),
         "/saves/Tetris.v1.1.srm"));
   p.savefile_dir_is_dir = false;
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_FRONTEND, "srm", 1, &p),
         "/roms/gb/Tetris.v1.srm"));
   assert(!path_of(RETRO_SAVE_ROOT_FRONTEND, "srm", 2, &p));
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_CONTENT_DIR, "Tetris.sav", 1,
         &p), "/roms/gb/Tetris.sav"));
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_CONTENT_FILE, NULL, 1, &p),
         "/roms/gb/Tetris.v1.1.gb"));
   p.subsystem = NULL;
   string_list_free(subsystem);

   /* A long content name keeps the NAME_MAX_LENGTH stem runloop keeps. */
   _len = strlcpy(long_content, "/roms/gb/", sizeof(long_content));
   memset(long_content + _len, 'L', 2 * NAME_MAX_LENGTH);
   strlcpy(long_content + _len + 2 * NAME_MAX_LENGTH, ".gb",
         sizeof(long_content) - _len - 2 * NAME_MAX_LENGTH);
   subsystem = string_list_new();
   assert(subsystem);
   assert(string_list_append(subsystem, long_content, attr));
   p.subsystem           = subsystem;
   p.savefile_dir_is_dir = true;
   subsystem_oracle(want, sizeof(want), long_content, p.savefile_dir,
         true, ".srm");
   assert(strlen(want) == strlen("/saves/") + NAME_MAX_LENGTH - 1
         - strlen("/roms/gb/") + strlen(".srm"));
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_FRONTEND, "srm", 0, &p), want));
   p.savefile_dir_is_dir = false;
   subsystem_oracle(want, sizeof(want), long_content, p.savefile_dir,
         false, ".srm");
   assert(strlen(want) == NAME_MAX_LENGTH - 1 + strlen(".srm"));
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_FRONTEND, "srm", 0, &p), want));
   p.subsystem = NULL;
   string_list_free(subsystem);

   /* The other roots. */
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_SAVE_DIR, "core/card1.mcd", 0,
         &p), "/saves/core/card1.mcd"));
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_SYSTEM_DIR, "dc/vmu_a1.bin", 0,
         &p), "/bios/dc/vmu_a1.bin"));
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_SYSTEM_DIR, "/abs/vmu.bin", 0,
         &p), "/abs/vmu.bin"));
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_CONTENT_DIR, "Game.sav", 0, &p),
         "/roms/nes/Game.sav"));
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_CONTENT_FILE, NULL, 0, &p),
         "/roms/nes/Game.v1.2.nes"));
   assert(!strcmp(path_of(RETRO_SAVE_ROOT_ABSOLUTE, "/ini/saves", 0, &p),
         "/ini/saves"));
   assert(!path_of(RETRO_SAVE_ROOT_ABSOLUTE, NULL, 0, &p));
   assert(!path_of(RETRO_SAVE_ROOT_CONTENT_FILE, NULL, 1, &p));
   assert(!path_of(RETRO_SAVE_ROOT_DUMMY, "x", 0, &p));
   p.system_dir = NULL;
   assert(!path_of(RETRO_SAVE_ROOT_SYSTEM_DIR, "dc/vmu_a1.bin", 0, &p));

   /* Truncated as strlcpy truncates; the length still says it all. */
   header_fill_frontend(&fi);
   assert(save_items_get_frontend_interface(&fi,
         RETRO_SAVE_INSTANCE_PRIMARY));
   p = plain_paths();
   path_of(RETRO_SAVE_ROOT_SAVE_DIR, "core/card1.mcd", 0, &p);
   assert(fi.get_path(0, small, sizeof(small))
         == strlen("/saves/core/card1.mcd"));
   assert(!strcmp(small, "/saves/"));
   assert(fi.get_path(1, small, sizeof(small)) == 0);

   /* Re-resolved after the names move (netplay's .netplay dir). */
   p.savefile_dir = "/saves/.netplay";
   save_items_resolve(&p);
   assert(!strcmp(cur()->items[0].path, "/saves/.netplay/core/card1.mcd"));

   save_items_clear();
   assert(fi.get_path(0, small, sizeof(small)) == 0);
}

static void test_legacy(void)
{
   struct retro_save_interface iface;
   struct retro_save_item items[2];
   struct save_items_paths p = plain_paths();

   assert(!save_items_legacy_retired(RETRO_MEMORY_SAVE_RAM));

   items[0] = item_named("srm");
   items[1] = item_named("card");
   items[1].storage       = RETRO_SAVE_STORAGE_CORE_FILE;
   items[1].path          = "card";
   items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   assert(save_items_legacy_retired(RETRO_MEMORY_SAVE_RAM));
   assert(!save_items_legacy_retired(RETRO_MEMORY_RTC));
   assert(!save_items_legacy_retired(RETRO_SAVE_NO_LEGACY_MEMORY));

   /* A subsystem memory type, and a core file retiring RTC. */
   items[0].legacy_memory = 0x101;
   items[1].legacy_memory = RETRO_MEMORY_RTC;
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   assert(!save_items_legacy_retired(RETRO_MEMORY_SAVE_RAM));
   assert(save_items_legacy_retired(0x101));
   assert(save_items_legacy_retired(RETRO_MEMORY_RTC));

   save_items_clear();
   assert(!save_items_legacy_retired(RETRO_MEMORY_RTC));
}

static void test_legacy_load_feature(void)
{
   struct retro_save_frontend_interface fi;
   struct retro_save_interface iface;
   struct retro_save_item items[1];
   struct save_items_paths p = plain_paths();

   /* Kept for the primary instance only. */
   header_fill_frontend(&fi);
   assert(save_items_get_frontend_interface(&fi,
         RETRO_SAVE_INSTANCE_PRIMARY));
   assert(save_items_state_get_ptr()->features
         == RETRO_SAVE_FEATURE_LEGACY_LOAD);
   header_fill_frontend(&fi);
   fi.features = 0;
   assert(save_items_get_frontend_interface(&fi,
         RETRO_SAVE_INSTANCE_SECONDARY));
   assert(save_items_state_get_ptr()->features
         == RETRO_SAVE_FEATURE_LEGACY_LOAD);

   /* Called only with the bit negotiated and the callback set. */
   items[0] = item_named("srm");
   iface_of(&iface, items, 1);
   assert(save_items_register(&iface, &p));
   assert(!cur()->iface.legacy_load);
   assert(!save_items_can_legacy_load());
   iface.legacy_load = test_legacy_load;
   assert(save_items_register(&iface, &p));
   assert(save_items_can_legacy_load());
   header_fill_frontend(&fi);
   fi.features = 0;
   assert(save_items_get_frontend_interface(&fi,
         RETRO_SAVE_INSTANCE_PRIMARY));
   assert(fi.features == 0);
   assert(!save_items_can_legacy_load());

   header_fill_frontend(&fi);
   assert(save_items_get_frontend_interface(&fi,
         RETRO_SAVE_INSTANCE_PRIMARY));
   save_items_clear();
   assert(save_items_state_get_ptr()->features == 0);
   assert(!save_items_can_legacy_load());
}

static void test_legacy_names(void)
{
   static const char *const names[] = { "sav", "../x", "old", NULL };
   struct retro_save_interface iface;
   struct retro_save_item items[2];
   struct save_items_paths p = plain_paths();
   const save_item_t *it;

   items[0] = item_named("srm");
   items[0].legacy_paths = names;
   items[1] = item_named("disk");
   items[1].storage      = RETRO_SAVE_STORAGE_IN_CONTENT;
   items[1].root         = RETRO_SAVE_ROOT_CONTENT_FILE;
   items[1].path         = NULL;
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));

   /* An unsafe name is left out; the content file has no earlier name. */
   it = &cur()->items[0];
   assert(!strcmp(it->legacy[0], "/saves/Game.v1.2.sav"));
   assert(!strcmp(it->legacy[1], "/saves/Game.v1.2.old"));
   assert(!it->legacy[2]);
   assert(!cur()->items[1].legacy);

   /* They move with the path. */
   p.savefile = "/saves/.netplay/Game.v1.2.srm";
   save_items_resolve(&p);
   assert(!strcmp(it->legacy[0], "/saves/.netplay/Game.v1.2.sav"));

   save_items_clear();
}

/* A registered set, which the refusal that follows ends. */
static void conflict_start(void)
{
   struct retro_save_interface iface;
   struct retro_save_item items[1];
   struct save_items_paths p = plain_paths();
   save_items_clear();
   items[0] = item_named("srm");
   iface_of(&iface, items, 1);
   assert(save_items_register(&iface, &p));
}

/* Item @i alone is left out, for its file, and told once. */
static void expect_conflict(const struct retro_save_interface *iface,
      unsigned i, unsigned field)
{
   unsigned k, n = 0;
   const save_items_state_t *st = save_items_state_get_ptr();
   expect_left_out(iface, i);
   assert(!cur()->items[1 - i].left_out && cur()->items[1 - i].path);
   for (k = 0; k < st->num_issues; k++)
      if (st->issues[k].verdict == RETRO_SAVE_VERDICT_LEFT_OUT)
      {
         assert(st->issues[k].item == i && st->issues[k].field == field);
         n++;
      }
   assert(n == 1);
   assert(save_items_take_left_out());
   assert(!save_items_take_left_out());
   expect_left_out(iface, i);
   assert(!save_items_take_left_out());
}

static void test_conflict(void)
{
   static const char *const eep[]  = { "eep", NULL };
   static const char *const card[] = { "/abs/card.bin", NULL };
   static const char *const own[]  = { "/abs/a.bin", NULL };
   struct retro_save_interface iface;
   struct retro_save_item items[2];
   struct save_items_paths p = plain_paths();

   /* Two frontend-owned items, one file. */
   conflict_start();
   items[0] = item_named("srm");
   items[1] = item_named("blob");
   items[1].storage = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   iface_of(&iface, items, 2);
   expect_conflict(&iface, 1, RETRO_SAVE_FIELD_PATH);

   /* The item that held the file keeps it, whatever its index. */
   conflict_start();
   items[1] = items[0];
   items[0] = item_named("blob");
   items[0].storage = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   iface_of(&iface, items, 2);
   expect_conflict(&iface, 0, RETRO_SAVE_FIELD_PATH);
   items[0] = items[1];

   /* A frontend-owned file that a core file names, in any case: the
    * core writes it. */
   conflict_start();
   items[1] = item_named("card");
   items[1].storage = RETRO_SAVE_STORAGE_CORE_FILE;
   items[1].root    = RETRO_SAVE_ROOT_SAVE_DIR;
   items[1].path    = "GAME.V1.2.SRM";
   iface_of(&iface, items, 2);
   expect_conflict(&iface, 0, RETRO_SAVE_FIELD_PATH);

   /* One file spelled two ways. */
   conflict_start();
   items[0] = item_named("a");
   items[0].root = RETRO_SAVE_ROOT_ABSOLUTE;
   items[0].path = "/abs//x/./a.bin";
   items[1] = item_named("b");
   items[1].root = RETRO_SAVE_ROOT_ABSOLUTE;
   items[1].path = "/abs/x/a.bin/";
   iface_of(&iface, items, 2);
   expect_conflict(&iface, 1, RETRO_SAVE_FIELD_PATH);
   assert(!strcmp(cur()->items[0].path, "/abs/x/a.bin"));

   /* A frontend file inside a core folder. */
   conflict_start();
   items[1] = item_named("dir");
   items[1].storage = RETRO_SAVE_STORAGE_CORE_FOLDER;
   items[1].root    = RETRO_SAVE_ROOT_SAVE_DIR;
   items[1].path    = "card";
   items[1].include = NULL;
   items[1].exclude = NULL;
   items[0] = item_named("a");
   items[0].root = RETRO_SAVE_ROOT_SAVE_DIR;
   items[0].path = "card/a.bin";
   iface_of(&iface, items, 2);
   expect_conflict(&iface, 0, RETRO_SAVE_FIELD_PATH);
   /* Nearest valid: a sibling folder. */
   items[0].path = "cards/a.bin";
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   assert(!cur()->items[0].left_out && cur()->items[0].path);

   /* An earlier name that is another item's file. */
   conflict_start();
   items[0] = item_named("srm");
   items[0].legacy_paths = eep;
   items[1] = item_named("eep");
   items[1].path = "eep";
   iface_of(&iface, items, 2);
   expect_conflict(&iface, 0, RETRO_SAVE_FIELD_LEGACY_PATHS);

   conflict_start();
   items[0] = item_named("a");
   items[0].root         = RETRO_SAVE_ROOT_ABSOLUTE;
   items[0].path         = "/abs/a.bin";
   items[0].legacy_paths = card;
   items[1] = item_named("card");
   items[1].storage = RETRO_SAVE_STORAGE_CORE_FILE;
   items[1].root    = RETRO_SAVE_ROOT_ABSOLUTE;
   items[1].path    = "/abs/card.bin";
   iface_of(&iface, items, 2);
   expect_conflict(&iface, 0, RETRO_SAVE_FIELD_LEGACY_PATHS);

   /* Not conflicts: two descriptions of one core file, an item's own
    * file among its earlier names, a shared earlier name. */
   conflict_start();
   items[0] = item_named("card1");
   items[0].storage = RETRO_SAVE_STORAGE_CORE_FILE;
   items[0].root    = RETRO_SAVE_ROOT_ABSOLUTE;
   items[0].path    = "/abs/card.bin";
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   items[0] = item_named("a");
   items[0].root         = RETRO_SAVE_ROOT_ABSOLUTE;
   items[0].path         = "/abs/a.bin";
   items[0].legacy_paths = own;
   items[1] = item_named("b");
   items[1].path = "b";
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   assert(!save_items_take_refusal());

   /* A refusal for another reason is reported too, once. */
   items[1].id = "a";
   iface_of(&iface, items, 2);
   expect_refused(&iface);
   assert(save_items_take_refusal());
   assert(!save_items_take_refusal());

   save_items_clear();
}

/* A refusal ends the current set; what it retired stays retired until
 * the content unloads, whatever later sets name. */
static void test_dropped(void)
{
   struct retro_save_interface iface;
   struct retro_save_item items[2];
   struct save_items_paths p = plain_paths();

   save_items_clear();
   items[0] = item_named("srm");
   items[0].legacy_paths  = NULL;
   items[1] = item_named("rtc");
   items[1].path          = "rtc";
   items[1].legacy_paths  = NULL;
   items[1].legacy_memory = RETRO_MEMORY_RTC;
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   save_items_drop_previous();

   /* Refused for a duplicate id: the set ends. */
   items[1].id = "srm";
   iface_of(&iface, items, 2);
   expect_refused(&iface);
   assert(save_items_legacy_retired(RETRO_MEMORY_SAVE_RAM));
   assert(save_items_legacy_retired(RETRO_MEMORY_RTC));
   assert(!save_items_legacy_retired(RETRO_SAVE_NO_LEGACY_MEMORY));
   assert(save_items_state_get_ptr()->num_retired == 2);

   /* Refused again with nothing current: nothing more to end. */
   expect_refused(&iface);
   assert(save_items_state_get_ptr()->num_retired == 2);

   /* A later set starts anew; the ended set's memories stay retired. */
   items[0].legacy_memory = 0x101;
   items[1].id            = "eep";
   items[1].path          = "eep";
   items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   assert(!prev() && !save_items_state_get_ptr()->dropped);
   assert(cur()->items[0].prev == SAVE_ITEMS_NONE);
   assert(save_items_legacy_retired(0x101));
   assert(save_items_legacy_retired(RETRO_MEMORY_SAVE_RAM));
   assert(save_items_legacy_retired(RETRO_MEMORY_RTC));

   /* Ending it adds its memory once. */
   items[1].id = "srm";
   iface_of(&iface, items, 2);
   expect_refused(&iface);
   assert(save_items_state_get_ptr()->num_retired == 3);
   items[1].id = "eep";
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   items[1].id = "srm";
   iface_of(&iface, items, 2);
   expect_refused(&iface);
   assert(save_items_state_get_ptr()->num_retired == 3);

   save_items_clear();
   assert(!save_items_legacy_retired(RETRO_MEMORY_SAVE_RAM));
   assert(!save_items_legacy_retired(RETRO_MEMORY_RTC));
   assert(!save_items_state_get_ptr()->retired);
   assert(save_items_state_get_ptr()->num_retired == 0);
}

/* Registers @was, then @now; whether @now's one item is marked. */
static bool type_changed(const struct retro_save_item *was,
      const struct retro_save_item *now)
{
   struct retro_save_interface iface;
   struct save_items_paths p = plain_paths();
   save_items_clear();
   iface_of(&iface, was, 1);
   assert(save_items_register(&iface, &p));
   assert(!cur()->items[0].type_changed);
   iface_of(&iface, now, 1);
   assert(save_items_register(&iface, &p));
   return cur()->items[0].type_changed;
}

/* A new id at a file the replaced set used with another format, or,
 * either untagged, another size, owes a copy; the same id, or the same
 * format and size (a Transfer Pak cart moving port), does not. */
static void test_type_change(void)
{
   struct retro_save_item was = item_named("a");
   struct retro_save_item now;

   was.legacy_paths = NULL;
   now              = was;
   now.id           = "b";
   assert(!type_changed(&was, &now));
   now.format       = "nes:wram";
   assert(type_changed(&was, &now));

   /* The same tag at another size. */
   now.format       = was.format;
   now.num_segments = 1;
   assert(type_changed(&was, &now));
   now.num_segments = 2;

   /* The same id keeps its record, whatever its tag. */
   now.id           = "a";
   assert(!type_changed(&was, &now));

   /* Another file is not a change. */
   now.id           = "b";
   now.path         = "eep";
   assert(!type_changed(&was, &now));

   /* Untagged: the size decides. */
   now              = was;
   now.id           = "b";
   now.format       = NULL;
   assert(!type_changed(&was, &now));
   now.num_segments = 1;
   assert(type_changed(&was, &now));
   was.format       = NULL;
   now.num_segments = 2;
   now.format       = "nes:wram";
   assert(!type_changed(&was, &now));

   /* Another storage kind is another type. */
   now              = was;
   now.id           = "b";
   now.storage      = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   assert(type_changed(&was, &now));

   /* A core file the frontend now writes, in another case. */
   was              = item_named("cart");
   was.legacy_paths = NULL;
   was.storage      = RETRO_SAVE_STORAGE_CORE_FILE;
   was.root         = RETRO_SAVE_ROOT_ABSOLUTE;
   was.path         = "/abs/cart.sav";
   now              = item_named("sav");
   now.legacy_paths = NULL;
   now.root         = RETRO_SAVE_ROOT_ABSOLUTE;
   now.path         = "/abs/CART.sav";
   assert(type_changed(&was, &now));

   /* A Transfer Pak cart moving from port 1 to port 2. */
   was              = item_named("tpak1");
   was.legacy_paths = NULL;
   was.root         = RETRO_SAVE_ROOT_ABSOLUTE;
   was.path         = "/abs/cart.sav";
   was.format       = "gb:sram";
   was.num_segments = 1;
   was.port         = 1;
   now              = was;
   now.id           = "tpak2";
   now.port         = 2;
   assert(!type_changed(&was, &now));

   save_items_clear();
}

/* A new id at the file of a replaced item whose id is gone, kept the
 * same way, is linked to it as a kept id is (a Transfer Pak cart moving
 * port); another tag or size at that file is marked instead. */
static void test_port_move(void)
{
   struct retro_save_interface iface;
   struct retro_save_item items[2];
   struct save_items_paths p = plain_paths();

   save_items_clear();
   items[0]              = item_named("tpak1");
   items[0].legacy_paths = NULL;
   items[0].root         = RETRO_SAVE_ROOT_ABSOLUTE;
   items[0].path         = "/abs/cart.sav";
   items[0].format       = "gb:sram";
   items[0].port         = 1;
   iface_of(&iface, items, 1);
   assert(save_items_register(&iface, &p));

   /* The cart moves to port 2: linked, not marked. */
   items[0].id   = "tpak2";
   items[0].port = 2;
   iface_of(&iface, items, 1);
   assert(save_items_register(&iface, &p));
   assert(cur()->items[0].prev == 0 && prev()->items[0].next == 0);
   assert(!cur()->items[0].type_changed);

   /* Another tag at that file: marked, not linked. */
   items[0].id     = "tpak1";
   items[0].format = "gb:rtc";
   iface_of(&iface, items, 1);
   assert(save_items_register(&iface, &p));
   assert(cur()->items[0].prev == SAVE_ITEMS_NONE);
   assert(prev()->items[0].next == SAVE_ITEMS_NONE);
   assert(cur()->items[0].type_changed);

   /* The same tag at another size: marked, not linked. */
   items[0].id           = "tpak2";
   items[0].num_segments = 1;
   iface_of(&iface, items, 1);
   assert(save_items_register(&iface, &p));
   assert(cur()->items[0].prev == SAVE_ITEMS_NONE);
   assert(cur()->items[0].type_changed);

   /* Untagged, the same size: linked. */
   items[0].id     = "tpak1";
   items[0].format = NULL;
   iface_of(&iface, items, 1);
   assert(save_items_register(&iface, &p));
   assert(cur()->items[0].prev == 0);
   assert(!cur()->items[0].type_changed);

   /* A new id at a new file: neither. */
   items[0].id   = "tpak2";
   items[0].path = "/abs/other.sav";
   iface_of(&iface, items, 1);
   assert(save_items_register(&iface, &p));
   assert(cur()->items[0].prev == SAVE_ITEMS_NONE);
   assert(!cur()->items[0].type_changed);

   /* The id is kept at another file: a kept id wins, so the new id at
    * its old file is neither linked nor marked. */
   items[1]      = items[0];
   items[0].path = "/abs/x.sav";
   items[1].id   = "tpak1";
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   assert(cur()->items[0].prev == 0);
   assert(cur()->items[1].prev == SAVE_ITEMS_NONE);
   assert(!cur()->items[1].type_changed);

   /* After a refusal nothing is replaced, so nothing is linked. */
   items[1].id = "tpak2";
   iface_of(&iface, items, 2);
   expect_refused(&iface);
   items[1].id = "tpak1";
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   assert(cur()->items[0].prev == SAVE_ITEMS_NONE);
   assert(cur()->items[1].prev == SAVE_ITEMS_NONE);

   save_items_clear();
}

/* A kept id at a file another item of the replaced set kept another
 * way is marked as a new id is; at its own file, or swapping files
 * kept the same way, it is not. */
static void test_kept_move(void)
{
   struct retro_save_interface iface;
   struct retro_save_item items[2];
   struct save_items_paths p = plain_paths();

   save_items_clear();
   items[0]              = item_named("cart");
   items[0].legacy_paths = NULL;
   items[0].root         = RETRO_SAVE_ROOT_ABSOLUTE;
   items[0].path         = "/abs/cart.sav";
   items[0].format       = "gb:sram";
   items[1]              = items[0];
   items[1].id           = "rtc";
   items[1].path         = "/abs/rtc.sav";
   items[1].format       = "gb:rtc";
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));

   /* The cart moves onto the file of the gone rtc. */
   items[0].path = "/abs/rtc.sav";
   iface_of(&iface, items, 1);
   assert(save_items_register(&iface, &p));
   assert(cur()->items[0].prev == 0 && cur()->items[0].type_changed);

   /* It moves back, to a file no item had; the rtc comes back new. */
   items[0].path = "/abs/cart.sav";
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   assert(!cur()->items[0].type_changed && cur()->items[1].type_changed);

   /* Two kept ids swap files of different types: both marked. */
   items[0].path = "/abs/rtc.sav";
   items[1].path = "/abs/cart.sav";
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   assert(cur()->items[0].prev == 0 && cur()->items[1].prev == 1);
   assert(cur()->items[0].type_changed && cur()->items[1].type_changed);

   /* Kept at their own files: not marked, whatever their type. */
   items[0].format = "gb:rtc";
   items[1].format = "gb:sram";
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   assert(!cur()->items[0].type_changed && !cur()->items[1].type_changed);

   /* Swapping files kept the same way: not marked. */
   items[0].path   = "/abs/cart.sav";
   items[0].format = "gb:sram";
   items[1].path   = "/abs/rtc.sav";
   items[1].format = "gb:rtc";
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   assert(cur()->items[0].prev == 0 && cur()->items[1].prev == 1);
   assert(!cur()->items[0].type_changed && !cur()->items[1].type_changed);

   save_items_clear();
}

static void test_header(void)
{
   struct retro_save_entry entry;
   struct retro_save_icon icon;
   header_fill_entry(&entry, &icon);
   assert(entry.num_files == 1 && icon.frames == 1);
   assert(header_sum_values() == 1033);
}


/* A fresh content load with @n items, checked: @ok whether it
 * registered, then exactly one finding, (@item, @field, @verdict), and
 * its line logged once at its level as "[SRAM] TestCore: @line". */
static void expect_finding(struct retro_save_interface *iface, bool ok,
      unsigned item, unsigned field, unsigned verdict, const char *line)
{
   char text[1024];
   struct save_items_paths p = plain_paths();
   const save_items_state_t *st = save_items_state_get_ptr();
   save_items_clear();
   logs_clear();
   assert(save_items_register(iface, &p) == ok);
   snprintf(text, sizeof(text), "TestCore: %s", line);
   assert(st->num_issues == 1);
   assert(st->issues[0].item == item);
   assert(st->issues[0].field == field);
   assert(st->issues[0].verdict == verdict);
   assert(!strcmp(st->issues[0].text, text));
   assert(logged_once(verdict == RETRO_SAVE_VERDICT_IGNORED ? 'W' : 'E',
         text));
   assert(logs_at('W') + logs_at('E') == 1);
   if (verdict == RETRO_SAVE_VERDICT_LEFT_OUT)
   {
      assert(cur()->items[item].left_out && !cur()->items[item].path);
      assert(!save_items_legacy_retired(RETRO_MEMORY_SAVE_RAM)
            || item != 0);
   }
   save_items_drop_previous();
}

#define WHO "save item 0 \"x\" (\"Battery RAM\"): "
#define LEFT " Item left out; it is not saved."
#define LEFT_CORE " Item left out; the frontend makes no copies or backups " \
   "of it."

/* One case per rule, the nearest valid case first. */
static void test_rules(void)
{
   char line[512];
   struct retro_save_interface iface;
   struct retro_save_item items[2];
   struct retro_save_segment bad[1];
   struct save_items_paths p = plain_paths();

   items[0] = clean_item("x");
   iface_of(&iface, items, 1);
   save_items_clear();
   logs_clear();
   assert(save_items_register(&iface, &p));
   assert(save_items_state_get_ptr()->num_issues == 0);
   assert(logs_at('W') == 0 && logs_at('E') == 0);
   assert(save_items_legacy_retired(RETRO_MEMORY_SAVE_RAM));

   /* Refused. */
   iface_of(&iface, items, 1);
   iface.interface_version = 0;
   expect_finding(&iface, false, RETRO_SAVE_ISSUE_NO_ITEM,
         RETRO_SAVE_FIELD_INTERFACE_VERSION, RETRO_SAVE_VERDICT_REFUSED,
         "save registration: interface_version = 0: it is the version the "
         "core was written for, 1 or more. See libretro.h, "
         "retro_save_interface::interface_version. Registration refused.");
   iface_of(&iface, items, 1);
   iface.num_items = SAVE_ITEMS_MAX_ITEMS + 1;
   expect_finding(&iface, false, RETRO_SAVE_ISSUE_NO_ITEM,
         RETRO_SAVE_FIELD_NUM_ITEMS, RETRO_SAVE_VERDICT_REFUSED,
         "save registration: num_items = 4097: more than 4096 items is "
         "taken as garbage. See libretro.h, retro_save_interface::"
         "num_items. Registration refused.");
   iface_of(&iface, items, 1);
   iface.item_size = SAVE_ITEMS_ITEM_MAX_SIZE + 1;
   snprintf(line, sizeof(line), "save registration: item_size = 4097: it "
         "is sizeof(struct retro_save_item) in the core's build, %lu or "
         "more and at most 4096. See libretro.h, retro_save_interface::"
         "item_size. Registration refused.",
         (unsigned long)sizeof(struct retro_save_item));
   expect_finding(&iface, false, RETRO_SAVE_ISSUE_NO_ITEM,
         RETRO_SAVE_FIELD_ITEM_SIZE, RETRO_SAVE_VERDICT_REFUSED, line);
   iface_of(&iface, items, 1);
   iface.get_result = NULL;
   expect_finding(&iface, false, RETRO_SAVE_ISSUE_NO_ITEM,
         RETRO_SAVE_FIELD_GET_RESULT, RETRO_SAVE_VERDICT_REFUSED,
         "save registration: get_result = NULL: it is required with "
         "request. See libretro.h, retro_save_interface::get_result. "
         "Registration refused.");
   items[1] = clean_item("x");
   items[1].path          = "sav";
   items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   iface_of(&iface, items, 2);
   expect_finding(&iface, false, 1, RETRO_SAVE_FIELD_ID,
         RETRO_SAVE_VERDICT_REFUSED, "save item 1 \"x\" (\"Battery RAM\"): "
         "id = \"x\": item 0 has it too, and a frontend matches items by "
         "id. See libretro.h, retro_save_item::id. Registration refused.");

   /* Left out. */
   items[0].storage = 7;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_STORAGE,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "storage = 7: not an enum "
         "retro_save_storage value. See libretro.h, retro_save_item::"
         "storage." LEFT);
   items[0] = clean_item("x");
   items[0].segments = NULL;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_SEGMENTS,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "segments = NULL: a "
         "FRONTEND_BUFFER item needs at least one segment. See libretro.h, "
         "retro_save_item::segments." LEFT);
   bad[0].data = NULL;
   bad[0].size = 16;
   items[0] = clean_item("x");
   items[0].segments     = bad;
   items[0].num_segments = 1;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_SEGMENTS,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "segments = segment 0 data "
         "NULL: a segment of 16 bytes needs its memory. See libretro.h, "
         "retro_save_item::segments." LEFT);
   items[0] = clean_item("x");
   items[0].storage  = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   items[0].segments = NULL;
   iface_of(&iface, items, 1);
   iface.blob_save = NULL;
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_BLOB_SAVE,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "blob_save = NULL: a "
         "FRONTEND_BLOB item needs blob_size, blob_save and blob_load. See "
         "libretro.h, struct retro_save_interface." LEFT);
   items[0] = clean_item("x");
   items[0].root = 6;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_ROOT,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "root = 6: not an enum "
         "retro_save_root value. See libretro.h, retro_save_item::root."
         LEFT);
   items[0] = clean_item("x");
   items[0].path = ".srm";
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_PATH,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "path = \".srm\": under "
         "ROOT_FRONTEND it is the extension without a dot (\"srm\"). See "
         "libretro.h, retro_save_item::path." LEFT);
   items[0].path = NULL;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_PATH,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "path = NULL: under "
         "ROOT_FRONTEND it is the extension. See libretro.h, "
         "retro_save_item::path." LEFT);
   items[0].path = "../x";
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_PATH,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "path = \"../x\": under "
         "ROOT_FRONTEND it is an extension of letters, digits, '_', '-' "
         "and '.'. See libretro.h, retro_save_item::path." LEFT);
   items[0] = clean_item("x");
   items[0].root = RETRO_SAVE_ROOT_SAVE_DIR;
   items[0].path = NULL;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_PATH,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "path = NULL: this root needs a "
         "path. See libretro.h, retro_save_item::path." LEFT);
   items[0] = clean_item("x");
   items[0].content_index = 1;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_CONTENT_INDEX,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "content_index = 1: no content "
         "is loaded at that index. See libretro.h, retro_save_item::"
         "content_index." LEFT);

   /* The game file: never a frontend-owned item's. */
   items[0] = clean_item("x");
   items[0].root = RETRO_SAVE_ROOT_CONTENT_FILE;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_ROOT,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "root = ROOT_CONTENT_FILE: a "
         "frontend-owned item there would be written over the content "
         "file. See libretro.h, retro_save_item::root." LEFT);
   items[0].storage  = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   items[0].segments = NULL;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_ROOT,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "root = ROOT_CONTENT_FILE: a "
         "frontend-owned item there would be written over the content "
         "file. See libretro.h, retro_save_item::root." LEFT);
   items[0] = clean_item("x");
   items[0].root = RETRO_SAVE_ROOT_ABSOLUTE;
   items[0].path = "/roms/nes/Game.v1.2.nes";
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_PATH,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "path = \"/roms/nes/Game.v1.2."
         "nes\": it is the content file, which a frontend never writes. "
         "See libretro.h, SET_SAVE_INTERFACE, \"In content\"." LEFT);
   /* Nearest valid: in content, described only. */
   items[0] = clean_item("x");
   items[0].storage       = RETRO_SAVE_STORAGE_IN_CONTENT;
   items[0].root          = RETRO_SAVE_ROOT_CONTENT_FILE;
   items[0].segments      = NULL;
   items[0].flags         = 0;
   items[0].base_hash     = NULL;
   items[0].write_timing  = RETRO_SAVE_WRITE_UNKNOWN;
   items[0].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   iface_of(&iface, items, 1);
   save_items_clear();
   assert(save_items_register(&iface, &p));
   assert(save_items_state_get_ptr()->num_issues == 0);
   assert(!strcmp(cur()->items[0].path, "/roms/nes/Game.v1.2.nes"));

   /* Ignored. */
   items[0] = clean_item("x");
   items[0].label = NULL;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_LABEL,
         RETRO_SAVE_VERDICT_IGNORED, "save item 0 \"x\": label = NULL: it "
         "is the name the user sees. See libretro.h, retro_save_item::"
         "label. Ignored.");
   items[0] = clean_item("x");
   items[0].scope = 9;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_SCOPE,
         RETRO_SAVE_VERDICT_IGNORED, WHO "scope = 9: not an enum "
         "retro_save_scope value; read as UNKNOWN. See libretro.h, "
         "retro_save_item::scope. Ignored.");
   assert(cur()->items[0].item.scope == RETRO_SAVE_SCOPE_UNKNOWN);
   items[0] = clean_item("x");
   items[0].write_timing = RETRO_SAVE_WRITE_THROUGH;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_WRITE_TIMING,
         RETRO_SAVE_VERDICT_IGNORED, WHO "write_timing = 1: a "
         "frontend-owned item's is FRONTEND. See libretro.h, "
         "retro_save_item::write_timing. Ignored.");
   items[0] = clean_item("x");
   items[0].flags |= (uint64_t)1 << 40;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_FLAGS,
         RETRO_SAVE_VERDICT_IGNORED, WHO "flags = 0x0000010000000000: bits "
         "this frontend does not know are dropped. See libretro.h, "
         "retro_save_item::flags. Ignored.");
   assert(!(cur()->items[0].item.flags & ((uint64_t)1 << 40)));
   items[0] = clean_item("x");
   items[0].storage  = RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   items[0].segments = NULL;
   items[0].flags   |= RETRO_SAVE_FLAG_EXACT_SIZE;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_FLAGS,
         RETRO_SAVE_VERDICT_IGNORED, WHO "flags = EXACT_SIZE: it applies "
         "only to FRONTEND_BUFFER items. See libretro.h, "
         "RETRO_SAVE_FLAG_EXACT_SIZE. Ignored.");
   items[0] = clean_item("x");
   items[0].base_hash = NULL;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_BASE_HASH,
         RETRO_SAVE_VERDICT_IGNORED, WHO "base_hash = NULL: a "
         "DIFF_OF_CONTENT item names the content it applies to. See "
         "libretro.h, retro_save_item::base_hash. Ignored.");

   /* Legacy memory: 0 is SAVE_RAM. */
   items[0] = clean_item("x");
   items[1] = clean_item("y");
   items[1].path = "sav";
   iface_of(&iface, items, 2);
   expect_finding(&iface, true, 1, RETRO_SAVE_FIELD_LEGACY_MEMORY,
         RETRO_SAVE_VERDICT_IGNORED, "save item 1 \"y\" (\"Battery RAM\"): "
         "legacy_memory = 0: item 0 \"x\" replaces it too, and one item "
         "replaces each memory; 0 is RETRO_MEMORY_SAVE_RAM. See libretro.h, "
         "retro_save_item::legacy_memory. Ignored.");
   items[0] = clean_item("x");
   items[0].storage   = RETRO_SAVE_STORAGE_NOT_PERSISTED;
   items[0].segments  = NULL;
   items[0].flags     = 0;
   items[0].base_hash = NULL;
   items[0].write_timing = RETRO_SAVE_WRITE_UNKNOWN;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_LEGACY_MEMORY,
         RETRO_SAVE_VERDICT_IGNORED, WHO "legacy_memory = 0: this item "
         "keeps no file, so that memory is no longer saved; 0 is "
         "RETRO_MEMORY_SAVE_RAM. See libretro.h, retro_save_item::"
         "legacy_memory. Ignored.");
   items[0] = clean_item("x");
   items[0].legacy_memory = 2;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_LEGACY_MEMORY,
         RETRO_SAVE_VERDICT_IGNORED, WHO "legacy_memory = 2: not "
         "RETRO_MEMORY_SAVE_RAM, RETRO_MEMORY_RTC or a subsystem memory "
         "type. See libretro.h, retro_save_item::legacy_memory. Ignored.");

   /* A zero-filled item retires nothing: it is left out. */
   memset(&items[0], 0, sizeof(items[0]));
   items[0].id    = "x";
   items[0].label = "Battery RAM";
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_PATH,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "path = NULL: under "
         "ROOT_FRONTEND it is the extension. See libretro.h, "
         "retro_save_item::path." LEFT_CORE);
   assert(!save_items_legacy_retired(RETRO_MEMORY_SAVE_RAM));

   /* No save directory: logged, not the core's finding. */
   items[0] = clean_item("x");
   items[0].root = RETRO_SAVE_ROOT_SAVE_DIR;
   items[0].path = "x.sav";
   iface_of(&iface, items, 1);
   p.savefile_dir = NULL;
   save_items_clear();
   logs_clear();
   assert(save_items_register(&iface, &p));
   assert(save_items_state_get_ptr()->num_issues == 1);
   assert(save_items_state_get_ptr()->issues[0].field
         == RETRO_SAVE_FIELD_LEGACY_MEMORY);
   assert(logged_once('W', "TestCore: save item 0 \"x\" has no file: its "
         "directory or content is not known. It is not saved."));
   assert(!cur()->items[0].left_out && !save_items_take_left_out());
   save_items_clear();
}

/* Every finding of a set, in item order; each line once per content
 * load; at most 32 lines a call; the list stays until the next call. */
static void test_findings(void)
{
   unsigned i, n;
   struct retro_save_interface iface;
   struct retro_save_item items[40];
   struct retro_save_frontend_interface fi;
   const struct retro_save_issue *list = NULL;
   struct save_items_paths p = plain_paths();
   static char ids[40][8];

   header_fill_frontend(&fi);
   assert(save_items_get_frontend_interface(&fi,
         RETRO_SAVE_INSTANCE_PRIMARY));
   assert(fi.get_issues);
   save_items_clear();
   items[0] = clean_item("x");
   iface_of(&iface, items, 1);
   assert(save_items_register(&iface, &p));
   assert(fi.get_issues(&list) == 0);

   items[0].storage = 7;
   items[1] = clean_item("y");
   items[1].path  = ".sav";
   items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   items[2] = clean_item("z");
   items[2].path  = "zz";
   items[2].label = NULL;
   items[2].scope = 9;
   items[2].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   iface_of(&iface, items, 3);
   logs_clear();
   assert(save_items_register(&iface, &p));
   assert((n = fi.get_issues(&list)) == 4);
   assert(list[0].item == 0 && list[0].field == RETRO_SAVE_FIELD_STORAGE
         && list[0].verdict == RETRO_SAVE_VERDICT_LEFT_OUT);
   assert(list[1].item == 1 && list[1].field == RETRO_SAVE_FIELD_PATH
         && list[1].verdict == RETRO_SAVE_VERDICT_LEFT_OUT);
   assert(list[2].item == 2 && list[2].field == RETRO_SAVE_FIELD_LABEL
         && list[2].verdict == RETRO_SAVE_VERDICT_IGNORED);
   assert(list[3].item == 2 && list[3].field == RETRO_SAVE_FIELD_SCOPE
         && list[3].verdict == RETRO_SAVE_VERDICT_IGNORED);
   for (i = 0; i < n; i++)
      assert(logged_once(list[i].verdict == RETRO_SAVE_VERDICT_IGNORED
            ? 'W' : 'E', list[i].text));
   assert(logs_at('E') == 2 && logs_at('W') == 2);
   /* Told once per content load, by label, else id. */
   assert(!strcmp(save_items_take_left_out(), "Battery RAM"));
   assert(!strcmp(save_items_take_left_out(), "Battery RAM"));
   assert(!save_items_take_left_out());

   /* Again: nothing new logged, the same list. */
   logs_clear();
   assert(save_items_register(&iface, &p));
   assert(fi.get_issues(&list) == 4);
   assert(logs_at('E') == 0 && logs_at('W') == 0);
   assert(!save_items_take_left_out());
   save_items_drop_previous();

   /* A secondary instance lists none. */
   header_fill_frontend(&fi);
   assert(save_items_get_frontend_interface(&fi,
         RETRO_SAVE_INSTANCE_SECONDARY));
   assert(fi.get_issues(&list) == 0 && !list);

   /* A bad item_size stops at one line. */
   save_items_clear();
   iface_of(&iface, items, 3);
   iface.item_size = 1;
   logs_clear();
   assert(!save_items_register(&iface, &p));
   assert(save_items_state_get_ptr()->num_issues == 1);
   assert(logs_at('E') == 1);

   /* 40 findings: 32 lines, then the count. */
   save_items_clear();
   for (i = 0; i < 40; i++)
   {
      snprintf(ids[i], sizeof(ids[i]), "i%u", i);
      items[i] = clean_item(ids[i]);
      items[i].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
      items[i].scope         = 9;
      items[i].path          = ids[i];
   }
   iface_of(&iface, items, 40);
   logs_clear();
   assert(save_items_register(&iface, &p));
   assert(save_items_state_get_ptr()->num_issues == 40);
   assert(logs_at('W') == 33 && logs_at('E') == 0);
   assert(logged_once('W', "TestCore: and 8 more save item findings."));
   save_items_clear();
   assert(save_items_state_get_ptr()->num_issues == 0);
}

/* What registered is logged for new and changed items only. */
static void test_summary(void)
{
   struct retro_save_interface iface;
   struct retro_save_item items[2];
   struct save_items_paths p = plain_paths();

   save_items_clear();
   items[0] = clean_item("x");
   items[1] = clean_item("y");
   items[1].path          = "rtc";
   items[1].legacy_memory = RETRO_MEMORY_RTC;
   iface_of(&iface, items, 2);
   logs_clear();
   assert(save_items_register(&iface, &p));
   assert(logs_at('L') == 3);
   assert(logged_once('L', "TestCore: save item 0 \"x\" (\"Battery RAM\"): "
         "buffer, 192 bytes in 2 segments, file \"/saves/Game.v1.2.srm\", "
         "replaces SAVE_RAM."));
   assert(logged_once('L', "TestCore: buffers are read after "
         "retro_unload_game returns, so a core frees them in retro_deinit. "
         "See libretro.h, SET_SAVE_INTERFACE, \"Ordering at unload\"."));
   logs_clear();
   assert(save_items_register(&iface, &p));
   assert(logs_at('L') == 0);
   items[1].path = "rtc2";
   iface_of(&iface, items, 2);
   logs_clear();
   assert(save_items_register(&iface, &p));
   assert(logs_at('L') == 1);
   assert(logged_once('L', "TestCore: save item 1 \"y\" (\"Battery RAM\"): "
         "buffer, 192 bytes in 2 segments, file "
         "\"/saves/Game.v1.2.rtc2\", replaces RTC."));
   items[1].num_segments = 1;
   iface_of(&iface, items, 2);
   logs_clear();
   assert(save_items_register(&iface, &p));
   assert(logs_at('L') == 1);
   assert(logged_once('L', "TestCore: save item 1 \"y\" (\"Battery RAM\"): "
         "buffer, 64 bytes in 1 segment, file "
         "\"/saves/Game.v1.2.rtc2\", replaces RTC."));
   save_items_clear();
}

/* A kept item that turns bad is closed as one that left; what it
 * retired stays retired, and the rest keep their records. */
static void test_left_out_reregister(void)
{
   struct retro_save_interface iface;
   struct retro_save_item items[2];
   struct save_items_paths p = plain_paths();

   save_items_clear();
   items[0] = clean_item("x");
   items[1] = clean_item("y");
   items[1].path          = "rtc";
   items[1].legacy_memory = RETRO_MEMORY_RTC;
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   save_items_drop_previous();
   items[0].root = 6;
   iface_of(&iface, items, 2);
   assert(save_items_register(&iface, &p));
   assert(!save_items_take_refusal());
   assert(cur()->items[0].left_out && cur()->items[0].prev == SAVE_ITEMS_NONE);
   assert(prev()->items[0].next == SAVE_ITEMS_NONE);
   assert(cur()->items[1].prev == 1 && prev()->items[1].next == 1);
   save_items_drop_previous();
   assert(save_items_legacy_retired(RETRO_MEMORY_SAVE_RAM));
   assert(save_items_legacy_retired(RETRO_MEMORY_RTC));
   assert(!strcmp(save_items_take_left_out(), "Battery RAM"));
   assert(!save_items_take_left_out());

   /* A new content load tells it again. */
   save_items_clear();
   assert(save_items_register(&iface, &p));
   assert(save_items_take_left_out());
   assert(!save_items_legacy_retired(RETRO_MEMORY_SAVE_RAM));
   save_items_clear();
}

/* Past the room for told ids and logged lines, nothing is told or
 * logged again, and the told items run out. */
static void test_left_out_many(void)
{
   unsigned i, n = 0;
   struct retro_save_interface iface;
   static struct retro_save_item items[300];
   static char ids[300][8];
   struct save_items_paths p = plain_paths();

   save_items_clear();
   for (i = 0; i < 300; i++)
   {
      snprintf(ids[i], sizeof(ids[i]), "i%u", i);
      items[i] = clean_item(ids[i]);
      items[i].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
      items[i].path          = ".sav";
   }
   iface_of(&iface, items, 300);
   logs_clear();
   assert(save_items_register(&iface, &p));
   assert(save_items_state_get_ptr()->num_issues == 300);
   assert(logs_at('E') == 33);
   while (n < 1000 && save_items_take_left_out())
      n++;
   assert(n > 0 && n <= 300);
   logs_clear();
   assert(save_items_register(&iface, &p));
   assert(logs_at('W') + logs_at('E') == 0);
   assert(!save_items_take_left_out());
   save_items_clear();
}

/* Findings the review of the tiers turned up: who is told, the order
 * of the list, and what is left out or kept. */
static void test_left_out_cases(void)
{
   const struct retro_save_issue *list;
   const save_items_state_t *st = save_items_state_get_ptr();
   struct retro_save_interface iface;
   struct retro_save_item items[2];
   struct save_items_paths p = plain_paths();
   static const char *const earlier[] = { "sav2", NULL };
   static const char *const dotted[]  = { "./b.sav", NULL };
   static const char *const cards[]   = { "*.mcd", NULL };

   /* A core-written item the core goes on writing: no notice. */
   items[0] = clean_item("x");
   items[0].storage       = RETRO_SAVE_STORAGE_CORE_FOLDER;
   items[0].segments      = NULL;
   items[0].flags         = 0;
   items[0].base_hash     = NULL;
   items[0].write_timing  = RETRO_SAVE_WRITE_UNKNOWN;
   items[0].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   items[0].root          = 6;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_ROOT,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "root = 6: not an enum "
         "retro_save_root value. See libretro.h, retro_save_item::root."
         LEFT_CORE);
   assert(!save_items_take_left_out());

   /* A folder may be the save directory itself, naming its files. */
   items[0].root    = RETRO_SAVE_ROOT_SAVE_DIR;
   items[0].path    = "";
   items[0].include = cards;
   iface_of(&iface, items, 1);
   save_items_clear();
   assert(save_items_register(&iface, &p));
   assert(st->num_issues == 0);
   assert(cur()->items[0].path
         && !strcmp(cur()->items[0].path, "/saves"));

   /* A file inside a folder its patterns leave out. */
   items[0].path    = "cards";
   items[1] = clean_item("y");
   items[1].root          = RETRO_SAVE_ROOT_SAVE_DIR;
   items[1].path          = "cards/y.sav";
   items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   iface_of(&iface, items, 2);
   save_items_clear();
   logs_clear();
   assert(save_items_register(&iface, &p));
   assert(st->num_issues == 0 && !cur()->items[1].left_out);
   assert(logs_at('W') + logs_at('E') == 0);

   /* The list is in item order, though item 0's finding is made after
    * the paths resolve. */
   items[0] = clean_item("x");
   items[0].legacy_paths = earlier;
   items[1] = clean_item("y");
   items[1].path          = "sav2";
   items[1].label         = NULL;
   items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   iface_of(&iface, items, 2);
   save_items_clear();
   assert(save_items_register(&iface, &p));
   list = st->issues;
   assert(st->num_issues == 2);
   assert(list[0].item == 0 && list[0].field == RETRO_SAVE_FIELD_LEGACY_PATHS);
   assert(list[1].item == 1 && list[1].field == RETRO_SAVE_FIELD_LABEL);

   /* An earlier name has one spelling too. */
   items[0] = clean_item("a");
   items[0].root          = RETRO_SAVE_ROOT_SAVE_DIR;
   items[0].path          = "a.sav";
   items[0].legacy_paths  = dotted;
   items[0].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   items[1] = clean_item("b");
   items[1].root          = RETRO_SAVE_ROOT_SAVE_DIR;
   items[1].path          = "b.sav";
   items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   iface_of(&iface, items, 2);
   save_items_clear();
   assert(save_items_register(&iface, &p));
   assert(st->num_issues == 1 && cur()->items[0].left_out);

   /* A NULL id is never printed. */
   items[0] = clean_item("x");
   items[0].id = NULL;
   items[1] = clean_item("y");
   items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   iface_of(&iface, items, 2);
   save_items_clear();
   assert(!save_items_register(&iface, &p));
   assert(st->num_issues == 2 && st->issues[1].item == 1);
   assert(strstr(st->issues[1].text, "the file of item 0 \"\", "));
   save_items_clear();
}

/* A CORE_FOLDER item at @path under @root. */
static struct retro_save_item folder_item(const char *id, unsigned root,
      const char *path)
{
   struct retro_save_item it = clean_item(id);
   it.storage       = RETRO_SAVE_STORAGE_CORE_FOLDER;
   it.segments      = NULL;
   it.flags         = 0;
   it.base_hash     = NULL;
   it.write_timing  = RETRO_SAVE_WRITE_UNKNOWN;
   it.legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   it.root          = root;
   it.path          = path;
   return it;
}

/* A fresh content load of @n items with @p: registered, with
 * @findings findings and @lines warnings or errors logged. */
static void register_counted(const struct retro_save_item *items,
      unsigned n, const struct save_items_paths *p, unsigned findings,
      unsigned lines)
{
   struct retro_save_interface iface;
   iface_of(&iface, items, n);
   save_items_clear();
   logs_clear();
   assert(save_items_register(&iface, p));
   assert(save_items_state_get_ptr()->num_issues == findings);
   assert(logs_at('W') + logs_at('E') == lines);
}

#define WHO_Y "save item 1 \"y\" (\"Battery RAM\"): "
#define OWNERSHIP "See libretro.h, SET_SAVE_INTERFACE, \"Ownership\"."

/* A frontend-owned file inside a folder: kept only when the folder's
 * patterns leave it out. */
static void test_folder_patterns(void)
{
   static const char *const sav[]    = { "*.SAV", NULL };
   static const char *const temps[]  = { "*.SAV*", NULL };
   static const char *const sub[]    = { "sub/*.sav", NULL };
   static const char *const mcd[]    = { "*.mcd", NULL };
   static const char *const none[]   = { NULL };
   static const char *const listed[] = { "[xy].sa?", NULL };
   static const char *const tmp[]    = { "*.TMP", NULL };
   static const char *const autos[]  = { "*.auto.tmp", NULL };
   static const char *const escape[] = { "[\\]x]y.sav", NULL };
   static const char *const star[]   = { "\\*.sav", NULL };
   const save_items_state_t *st = save_items_state_get_ptr();
   struct retro_save_item items[2];
   struct save_items_paths p = plain_paths();

   items[0] = folder_item("x", RETRO_SAVE_ROOT_SAVE_DIR, "cards");
   items[1] = clean_item("y");
   items[1].root          = RETRO_SAVE_ROOT_SAVE_DIR;
   items[1].path          = "cards/y.sav";
   items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;

   /* Matched, case aside: left out, told once. */
   items[0].include = sav;
   register_counted(items, 2, &p, 1, 1);
   assert(st->issues[0].item == 1);
   assert(st->issues[0].field == RETRO_SAVE_FIELD_PATH);
   assert(st->issues[0].verdict == RETRO_SAVE_VERDICT_LEFT_OUT);
   assert(!strcmp(st->issues[0].text, "TestCore: " WHO_Y
         "path = \"/saves/cards/y.sav\": inside the folder and patterns "
         "of item 0 \"x\", and a file has one writer. " OWNERSHIP LEFT));
   assert(logged_once('E', st->issues[0].text));
   assert(cur()->items[1].left_out && !cur()->items[1].path);
   assert(!strcmp(save_items_take_left_out(), "Battery RAM"));
   assert(!save_items_take_left_out());

   /* One of the listed characters, then any one. */
   items[0].include = listed;
   register_counted(items, 2, &p, 1, 1);
   assert(cur()->items[1].left_out);

   /* A subfolder, '\\' read as '/'. */
   items[0].include = sub;
   items[1].path    = "cards\\sub\\y.sav";
   register_counted(items, 2, &p, 1, 1);
   assert(cur()->items[1].left_out);
   items[1].path    = "cards/y.sav";
   register_counted(items, 2, &p, 0, 0);
   assert(!cur()->items[1].left_out);

   /* Not included, or excluded with its write temps: accepted, nothing
    * said. */
   items[0].include = mcd;
   register_counted(items, 2, &p, 0, 0);
   assert(!cur()->items[1].left_out && cur()->items[1].path);
   assert(!save_items_take_left_out());
   items[0].include = NULL;
   items[0].exclude = temps;
   register_counted(items, 2, &p, 0, 0);
   assert(!cur()->items[1].left_out && cur()->items[1].path);

   /* NULL or an empty include is everything. */
   items[0].exclude = NULL;
   register_counted(items, 2, &p, 1, 1);
   assert(!strcmp(st->issues[0].text, "TestCore: " WHO_Y
         "path = \"/saves/cards/y.sav\": inside the folder of item 0 "
         "\"x\", and a file has one writer. " OWNERSHIP LEFT));
   assert(cur()->items[1].left_out);
   items[0].include = none;
   items[0].exclude = mcd;
   register_counted(items, 2, &p, 1, 1);
   assert(cur()->items[1].left_out);

   /* Its write temps are its files too. */
   items[0].include = tmp;
   items[0].exclude = NULL;
   register_counted(items, 2, &p, 1, 1);
   assert(st->issues[0].item == 1);
   assert(st->issues[0].field == RETRO_SAVE_FIELD_PATH);
   assert(st->issues[0].verdict == RETRO_SAVE_VERDICT_LEFT_OUT);
   assert(!strcmp(st->issues[0].text, "TestCore: " WHO_Y
         "path = \"/saves/cards/y.sav\": the frontend writes it through "
         "\"/saves/cards/y.sav.tmp\", inside the folder and patterns of "
         "item 0 \"x\", and a file has one writer. " OWNERSHIP LEFT));
   assert(logged_once('E', st->issues[0].text));
   assert(cur()->items[1].left_out && !cur()->items[1].path);
   assert(!strcmp(save_items_take_left_out(), "Battery RAM"));
   assert(!save_items_take_left_out());
   items[0].include = autos;
   register_counted(items, 2, &p, 1, 1);
   assert(strstr(st->issues[0].text,
         "through \"/saves/cards/y.sav.auto.tmp\""));
   assert(!strcmp(save_items_take_left_out(), "Battery RAM"));

   /* A backslash makes the next character literal, in a list too. */
   items[0].include = escape;
   items[1].path    = "cards/xy.sav";
   register_counted(items, 2, &p, 1, 1);
   assert(cur()->items[1].left_out);
   items[1].path    = "cards/]y.sav";
   register_counted(items, 2, &p, 1, 1);
   assert(cur()->items[1].left_out);
   items[0].include = star;
   items[1].path    = "cards/xy.sav";
   register_counted(items, 2, &p, 0, 0);
   assert(!cur()->items[1].left_out);
   save_items_clear();
}

/* Patterns of many '*' that fail late: the answer comes in time
 * proportional to pattern length times name length. */
static void test_folder_match_cost(void)
{
   static char path[1600];
   static char miss[128];
   static char hit[128];
   const char *miss_list[2];
   const char *hit_list[2];
   struct retro_save_item items[2];
   struct save_items_paths p = plain_paths();
   time_t start                 = time(NULL);
   size_t n                     = strlcpy(path, "cards/", sizeof(path));
   unsigned k;

   memset(path + n, 'a', 1500);
   strlcpy(path + n + 1500, ".sav", sizeof(path) - n - 1500);
   miss[0] = hit[0] = '\0';
   for (k = 0; k < 40; k++)
   {
      strlcat(miss, "*a", sizeof(miss));
      strlcat(hit, "*a", sizeof(hit));
   }
   strlcat(miss, "*b", sizeof(miss));
   strlcat(hit, "*.sav", sizeof(hit));
   miss_list[0] = miss;
   hit_list[0]  = hit;
   miss_list[1] = hit_list[1] = NULL;

   items[0] = folder_item("x", RETRO_SAVE_ROOT_SAVE_DIR, "cards");
   items[1] = clean_item("y");
   items[1].root          = RETRO_SAVE_ROOT_SAVE_DIR;
   items[1].path          = path;
   items[1].legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   items[0].include       = miss_list;
   register_counted(items, 2, &p, 0, 0);
   assert(!cur()->items[1].left_out && cur()->items[1].path);
   items[0].include       = hit_list;
   register_counted(items, 2, &p, 1, 1);
   assert(cur()->items[1].left_out);
   assert(!strcmp(save_items_take_left_out(), "Battery RAM"));
   /* Seconds, so a loaded machine passes too. */
   assert(difftime(time(NULL), start) < 20);
   save_items_clear();
}

/* A folder at the root directory itself names its own files. */
static void test_folder_root(void)
{
   static const char *const sav[]  = { "*.sav", NULL };
   static const char *const all[]  = { "*.mcd", "**", NULL };
   static const char *const none[] = { NULL };
   const save_items_state_t *st = save_items_state_get_ptr();
   const struct retro_save_issue *list;
   struct retro_save_frontend_interface fi;
   struct retro_save_interface iface;
   struct retro_save_item items[1];
   struct save_items_paths p = plain_paths();

   /* No include: left out, and no notice, as the core writes it. */
   items[0] = folder_item("x", RETRO_SAVE_ROOT_SAVE_DIR, "");
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_INCLUDE,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "include = NULL: a folder at the "
         "root directory itself names its own files, as that directory "
         "also holds files the core does not write. See libretro.h, "
         "retro_save_item::include." LEFT_CORE);
   assert(!save_items_take_left_out());
   save_items_clear();
   assert(save_items_register(&iface, &p));
   header_fill_frontend(&fi);
   assert(save_items_get_frontend_interface(&fi,
         RETRO_SAVE_INSTANCE_PRIMARY));
   assert(fi.get_issues(&list) == 1 && list == st->issues);
   assert(list[0].item == 0 && list[0].field == RETRO_SAVE_FIELD_INCLUDE
         && list[0].verdict == RETRO_SAVE_VERDICT_LEFT_OUT);
   assert(fi.get_path(0, NULL, 0) == 0);

   /* An empty list, under the other roots, and "." for "". */
   items[0].include = none;
   items[0].root    = RETRO_SAVE_ROOT_CONTENT_DIR;
   items[0].path    = ".";
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_INCLUDE,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "include = { NULL }: a folder at "
         "the root directory itself names its own files, as that directory "
         "also holds files the core does not write. See libretro.h, "
         "retro_save_item::include." LEFT_CORE);
   items[0].root    = RETRO_SAVE_ROOT_SYSTEM_DIR;
   items[0].path    = "./";
   register_counted(items, 1, &p, 1, 1);
   assert(cur()->items[0].left_out);

   /* Its own files: accepted. */
   items[0].include = sav;
   items[0].root    = RETRO_SAVE_ROOT_SAVE_DIR;
   items[0].path    = "";
   register_counted(items, 1, &p, 0, 0);
   assert(!cur()->items[0].left_out
         && !strcmp(cur()->items[0].path, "/saves"));

   /* Every file: accepted, and warned. */
   items[0].include = all;
   items[0].root    = RETRO_SAVE_ROOT_SYSTEM_DIR;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_INCLUDE,
         RETRO_SAVE_VERDICT_IGNORED, WHO "include = \"**\": it selects every "
         "file of the root directory itself, which also holds files the "
         "core does not write. See libretro.h, retro_save_item::include. "
         "Ignored.");
   register_counted(items, 1, &p, 1, 1);
   assert(!cur()->items[0].left_out
         && !strcmp(cur()->items[0].path, "/bios"));
   save_items_clear();
}

/* A folder that holds one of the frontend's savestates. */
static void test_folder_states(void)
{
   static const char *const slot[]  = { "*.state12", NULL };
   static const char *const autos[] = { "*.auto", NULL };
   static const char *const thumb[] = { "*.png", NULL };
   static const char *const state[] = { "*.state*", "*.replay*", NULL };
   static const char *const temp[]  = { "*.TMP", NULL };
   static const char *const slot_temp[] = { "*.state7.tmp", NULL };
   static const char *const auto_temp[] = { "*.auto.tmp", NULL };
   static const char *const entry[]     = { "*.entry", NULL };
   static const char *const replay[]    = { "*.replay", NULL };
   static const char *const replay9[]   = { "*.replay9", NULL };
   static const char *const disk[]      = { "*.ldci", NULL };
   static const char *const disk_temp[] = { "*.LDCI.tmp", NULL };
   const save_items_state_t *st = save_items_state_get_ptr();
   struct retro_save_interface iface;
   struct retro_save_item items[1];
   struct save_items_paths p = plain_paths();

   items[0] = folder_item("x", RETRO_SAVE_ROOT_ABSOLUTE, "/states");
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_PATH,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "path = \"/states\": the folder "
         "holds \"/states/Game.v1.2.state\", which the frontend writes "
         "for savestates, and a file has one writer; exclude it. " OWNERSHIP
         LEFT_CORE);
   assert(!save_items_take_left_out());

   items[0].include = slot;
   register_counted(items, 1, &p, 1, 1);
   assert(cur()->items[0].left_out);
   assert(strstr(st->issues[0].text, "\"/states/Game.v1.2.state12\""));
   items[0].include = autos;
   register_counted(items, 1, &p, 1, 1);
   assert(cur()->items[0].left_out);
   assert(strstr(st->issues[0].text, "\"/states/Game.v1.2.state.auto\""));
   items[0].include = thumb;
   register_counted(items, 1, &p, 1, 1);
   assert(cur()->items[0].left_out);
   assert(strstr(st->issues[0].text, "\"/states/Game.v1.2.state.png\""));

   /* The write temps of each. */
   items[0].include = temp;
   register_counted(items, 1, &p, 1, 1);
   assert(cur()->items[0].left_out);
   assert(strstr(st->issues[0].text, "\"/states/Game.v1.2.state.tmp\""));
   items[0].include = slot_temp;
   register_counted(items, 1, &p, 1, 1);
   assert(strstr(st->issues[0].text, "\"/states/Game.v1.2.state7.tmp\""));
   items[0].include = auto_temp;
   register_counted(items, 1, &p, 1, 1);
   assert(strstr(st->issues[0].text,
         "\"/states/Game.v1.2.state.auto.tmp\""));

   /* Entry states and replays, beside the savestates. */
   items[0].include = entry;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_PATH,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "path = \"/states\": the folder "
         "holds \"/states/Game.v1.2.state0.entry\", which the frontend "
         "loads as an entry state, and a file has one writer; exclude it. "
         OWNERSHIP LEFT_CORE);
   items[0].include = replay;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_PATH,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "path = \"/states\": the folder "
         "holds \"/states/Game.v1.2.replay\", which the frontend writes "
         "for replays, and a file has one writer; exclude it. " OWNERSHIP
         LEFT_CORE);
   items[0].include = replay9;
   register_counted(items, 1, &p, 1, 1);
   assert(strstr(st->issues[0].text, "\"/states/Game.v1.2.replay9\""));

   /* The disk index and its temp, in the save directory. */
   items[0] = folder_item("x", RETRO_SAVE_ROOT_SAVE_DIR, "");
   items[0].include = disk;
   iface_of(&iface, items, 1);
   expect_finding(&iface, true, 0, RETRO_SAVE_FIELD_PATH,
         RETRO_SAVE_VERDICT_LEFT_OUT, WHO "path = \"\": the folder holds "
         "\"/saves/Game.v1.2.ldci\", which the frontend writes for the "
         "disk index, and a file has one writer; exclude it. " OWNERSHIP
         LEFT_CORE);
   assert(!save_items_take_left_out());
   items[0].include = disk_temp;
   register_counted(items, 1, &p, 1, 1);
   assert(strstr(st->issues[0].text, "\"/saves/Game.v1.2.ldci.tmp\""));
   p.disk_index     = NULL;
   register_counted(items, 1, &p, 0, 0);
   assert(!cur()->items[0].left_out);

   /* Excluded, elsewhere, or no name. */
   items[0] = folder_item("x", RETRO_SAVE_ROOT_ABSOLUTE, "/states");
   items[0].exclude = state;
   register_counted(items, 1, &p, 0, 0);
   assert(!cur()->items[0].left_out);
   items[0].exclude = NULL;
   items[0].path    = "/states/other";
   register_counted(items, 1, &p, 0, 0);
   items[0].path    = "/states";
   p.savestate      = NULL;
   register_counted(items, 1, &p, 1, 1);
   assert(strstr(st->issues[0].text, "\"/states/Game.v1.2.replay\""));
   p.replay         = NULL;
   register_counted(items, 1, &p, 0, 0);
   assert(!cur()->items[0].left_out);
   save_items_clear();
}

int main(void)
{
   segs[0].data = wram;
   segs[0].size = sizeof(wram);
   segs[1].data = iram;
   segs[1].size = sizeof(iram);

   test_header();
   test_negotiate();
   test_validation();
   test_deep_copy();
   test_matching();
   test_naming();
   test_legacy();
   test_legacy_load_feature();
   test_legacy_names();
   test_conflict();
   test_dropped();
   test_type_change();
   test_port_move();
   test_kept_move();
   test_rules();
   test_findings();
   test_summary();
   test_left_out_reregister();
   test_left_out_many();
   test_left_out_cases();
   test_folder_patterns();
   test_folder_match_cost();
   test_folder_root();
   test_folder_states();
   printf("save_items_test: OK\n");
   return 0;
}
