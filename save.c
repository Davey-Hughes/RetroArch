/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2016-2019 - Brad Parker
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
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>
#include <time.h>

#include <lists/string_list.h>
#include <streams/interface_stream.h>
#include <streams/file_stream.h>
#include <streams/rzip_stream.h>
#include <rthreads/rthreads.h>
#ifdef HAVE_THREADS
#include <rthreads/retro_eventcount.h>
#endif
#include <retro_atomic.h>
#include <encodings/crc32.h>
#include <file/file_path.h>
#include <string/stdstring.h>
#include <time/rtime.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "content.h"
#include "core.h"
#include "core_info.h"
#include "file_path_special.h"
#include "msg_hash.h"
#include "runloop.h"
#include "save_items.h"
#include "verbosity.h"
#ifdef HAVE_CHEATS
#include "cheat_manager.h"
#include <compat/strl.h>
#endif

struct ram_type
{
   const char *path;
   int type;
};

static struct string_list *task_save_files = NULL;

#ifdef HAVE_THREADS
typedef struct autosave autosave_t;
#endif

/* A safety copy owed before the next write of a file. */
typedef struct save_item_guard
{
   /* The recovery file that took the image of a kept file; named
    * once, then kept current. */
   char *recovered;
   /* The newest image while an item's worker makes the copy owed, in a
    * recovery file no one is told of: named if the copy fails, deleted
    * once the worker writes a snapshot taken at untold_mark or later. */
   char *untold;
   int untold_mark;
   /* Bytes at the image's end dropped once the copy is made. */
   size_t drop_size;
   /* A type change's copy is of this image, not of the file: the
    * newest save of the old type, which a waiting write held. */
   void *copy_image;
   size_t copy_size;
   bool copy_first;
   /* Never written over without a copy: the core refused the file, or
    * a replaced item kept it another way. */
   bool keep_file;
   /* A replaced item kept the file another way: the copy is shown on
    * screen once made. */
   bool type_changed;
   /* Failed saves on the main thread in a row, up to 2: the second is
    * shown. */
   unsigned save_fails;
} save_item_guard_t;

/* What is kept for each registered item, indexed like
 * save_items_state::current->items. Main-thread data. */
typedef struct save_item_file
{
   /* What the item held when it was taken up with no file to read:
    * until it has a file, it is written only once it differs. */
   void *initial;
#ifdef HAVE_THREADS
   autosave_t *autosave;
   /* The retired worker whose last image the fill took. */
   const autosave_t *took;
#endif
   size_t initial_size;
   /* The segments' size before a re-registration grew the item over
    * its tail; nonzero until the frame start fills the new bytes. */
   size_t grow_from;
   /* The generation the item had when its worker could not start. */
   uint64_t start_generation;
   save_item_guard_t guard;
   /* A worker's failed write was reported. */
   bool failed_told;
   /* A worker that could not start was reported. */
   bool start_told;
#ifdef HAVE_THREADS
   /* Autosave copied it while the game was still saving it, and said
    * so. */
   bool cap_told;
#endif
   /* Since the last frame, the file holds an image taken while
    * can_snapshot() said true, written here or by its stopped worker. */
   bool settled;
   /* Kept it since the last frame, and said so. */
   bool kept_told;
   /* Its worker could not start, or the item was empty and has a
    * generation: tried again once that moves, at the next registration
    * or when the autosave settings change. */
   bool start_failed;
   /* Filled from its file, or taken up as it was. */
   bool taken_up;
   /* Checked again at its first fill (save_items_check_reported()). */
   bool checked;
   /* loaded() is owed for the fill. */
   bool tell;
   /* Why the blob was not copied, and the legacy memory's size, were
    * logged. */
   bool blob_told;
   bool size_told;
#ifdef HAVE_THREADS
   /* get_generation() stood still while the bytes changed: autosave
    * copies the item every interval. */
   bool generation_stuck;
   /* The bytes of the last autosave copy, or of the start. */
   uint32_t copy_crc;
   size_t copy_crc_size;
#endif
} save_item_file_t;

/* The last image of an item that left the set, written at the next
 * save of the files. With no data, only the type change's copy it owed
 * of guard.copy_image is left to make. */
typedef struct save_item_orphan
{
   char *path;
   void *data;
   void *initial;
   size_t size;
   size_t initial_size;
   save_item_guard_t guard;
   unsigned type;
   bool transform;
} save_item_orphan_t;

static struct save_item_files
{
   save_item_file_t *files;
   save_item_orphan_t *orphans;
   unsigned num_files;
   unsigned num_orphans;
   /* The record of the blob whose fill runs: its file is still its
    * save, so a registration that drops it keeps nothing of it. */
   const save_item_file_t *filling;
   /* Counts registrations: a core callback may register again. */
   unsigned registrations;
   /* Bits of the legacy memories an item gave back: their file may hold
    * its image, so an autosave started later writes its first snapshot. */
   unsigned char given_back;
   /* The load has filled the items; nothing is written before. */
   bool loaded;
   /* An item added or moved since waits for its fill. */
   bool fill_pending;
} save_item_files;

/* Each writer of an item file has a temp of its own, so two never
 * write into one. Cloud sync skips names ending in .tmp. */
#define SAVE_ITEM_TMP      ".tmp"
#ifdef HAVE_THREADS
#define SAVE_ITEM_AUTO_TMP ".auto.tmp"
#endif

/* Writes @len bytes to @path plus @suffix, compressed when @transform.
 * Returns that temp's name, or NULL with nothing left behind. */
static char *save_item_write_temp(const char *path, const char *suffix,
      const void *data, size_t len, bool transform)
{
   bool ok;
   size_t _len     = strlen(path);
   size_t suf_size = strlen(suffix) + 1;
   char *tmp       = (char*)malloc(_len + suf_size);
   if (!tmp)
      return NULL;
   memcpy(tmp, path, _len);
   memcpy(tmp + _len, suffix, suf_size);
#if defined(HAVE_COMPRESSION)
   if (transform)
      ok = rzipstream_write_file(tmp, data, (int64_t)len);
   else
#else
   (void)transform;
#endif
      ok = filestream_write_file(tmp, data, (int64_t)len);
   if (!ok)
   {
      filestream_delete(tmp);
      free(tmp);
      return NULL;
   }
   return tmp;
}

/* Puts @tmp in place of @path, or deletes it. Frees @tmp. */
static bool save_item_rename_temp(char *tmp, const char *path)
{
   bool ok = filestream_rename(tmp, path) == 0;
   if (!ok)
      filestream_delete(tmp);
   free(tmp);
   return ok;
}

/* Writes an item's file whole or not at all, so a crash mid-write
 * leaves the previous one. @transform stores it compressed. */
static bool save_item_write_file(const char *path, const char *suffix,
      const void *data, size_t len, bool transform)
{
   char *tmp = save_item_write_temp(path, suffix, data, len, transform);
   return tmp && save_item_rename_temp(tmp, path);
}

/* Where this content's safety copies go; NULL for none. */
static char *save_undo_dir      = NULL;
/* The warning that save RAM cuts its file, naming its copy in the undo
 * folder, was shown since the content loaded. */
static bool save_ram_cut_told   = false;
/* The recovery file each memory's cut was copied to since then. */
static char *save_ram_cut_dumps[RETRO_MEMORY_RTC + 1];
/* Each memory's cut with no copy at all was shown since then. */
static bool save_ram_cut_bare[RETRO_MEMORY_RTC + 1];

static void save_item_copy_done(const save_items_set_t *set, unsigned i);
static bool save_item_write_pending(const char *path);
static void save_item_recover(const char *path, const void *data,
      size_t len, unsigned type, char **recovered);
static bool dump_to_file_desperate(const void *data,
      size_t len, unsigned type, char *s, size_t s_len);

/* What a safety copy did. Bits: a worker ORs in each result until the
 * main thread collects them. */
enum save_item_copy
{
   SAVE_ITEM_COPY_FAILED = 0,
   /* Nothing to keep. */
   SAVE_ITEM_NO_FILE     = 1,
   SAVE_ITEM_COPY_MADE   = 2,
   /* Not made: another copier makes one of that name. */
   SAVE_ITEM_COPY_BUSY   = 8
};

/* Puts -@n (nothing for 1), then @ext, at @len in @s. */
static void save_name_number(char *s, size_t size, size_t len,
      unsigned n, const char *ext)
{
   if (n > 1)
      snprintf(s + len, size - len, "-%u%s", n, ext);
   else
      strlcpy(s + len, ext, size - len);
}

/* Numbers the name at @len in @s from -2 while it names a file that
 * does not hold @data (NULL: any file); past -99 it stays -99. Returns
 * the number. */
static unsigned save_name_free(char *s, size_t size, size_t len,
      const char *ext, const void *data, size_t data_len)
{
   unsigned n = 1;
   save_name_number(s, size, len, n, ext);
   while (     n < 99
         && path_is_valid(s)
         && !(data && filestream_matches_buf(s, data, data_len)))
      save_name_number(s, size, len, ++n, ext);
   return n;
}

static unsigned save_name_hash(const char *s)
{
   unsigned h = 5381;
   for (; *s; s++)
      h = h * 33 + (unsigned)(*s >= 'A' && *s <= 'Z'
            ? *s + 32 : (unsigned char)*s);
   return h;
}

#ifdef HAVE_THREADS
/* Bit (hash % 31) of a basename is set while a file of that name is
 * copied: files of one name in two folders share undo names, so their
 * copies go one at a time. */
static retro_atomic_int_t save_undo_copying =
   RETRO_ATOMIC_INT_INITIALIZER(0);

static bool save_undo_claim(int bit)
{
   for (;;)
   {
      int was = retro_atomic_load_acquire_int(&save_undo_copying);
      if (was & bit)
         return false;
      if (retro_atomic_cas_int(&save_undo_copying, was, was | bit))
         return true;
   }
}

static void save_undo_release(int bit)
{
   for (;;)
   {
      int was = retro_atomic_load_acquire_int(&save_undo_copying);
      if (retro_atomic_cas_int(&save_undo_copying, was, was & ~bit))
         return;
   }
}
#endif

/* Names copy @n of @path in @dir: its basename, with -n before the
 * extension past 1. With @data, the first of those that is free or
 * holds it instead. Returns the number. */
static unsigned save_item_copy_name(char *s, size_t size,
      const char *dir, const char *path, unsigned n,
      const void *data, size_t len)
{
   size_t _len;
   const char *base = path_basename(path);
   const char *ext  = strrchr(base, '.');
   if (!ext)
      ext = "";
   fill_pathname_join(s, dir, base, size);
   _len = strlen(s) - strlen(ext);
   if (data)
      return save_name_free(s, size, _len, ext, data, len);
   save_name_number(s, size, _len, n, ext);
   return n;
}

/* Writes @image into @dir as a copy of the file at @path, through the
 * writer's @suffix and a temp of the file's own, as copy *@n
 * (save_item_copy_name()): none is written when one holds it already.
 * With @turn, SAVE_ITEM_COPY_BUSY while a file of its name is copied.
 * Either thread; logs nothing. */
static enum save_item_copy save_item_copy_image(const char *path,
      const char *dir, const char *suffix, const void *image, size_t len,
      unsigned *n, bool turn)
{
   bool ok;
   char *copy;
   char tmp[32];
#ifdef HAVE_THREADS
   bool claimed;
   int bit = 1 << (save_name_hash(path_basename(path)) % 31);
#endif

   *n = 1;
   if (string_is_empty(dir) || !(copy = (char*)malloc(PATH_MAX_LENGTH)))
      return SAVE_ITEM_COPY_FAILED;
#ifdef HAVE_THREADS
   if (!(claimed = save_undo_claim(bit)) && turn)
   {
      free(copy);
      return SAVE_ITEM_COPY_BUSY;
   }
#else
   (void)turn;
#endif
   snprintf(tmp, sizeof(tmp), ".%08x%s", save_name_hash(path), suffix);
   *n = save_item_copy_name(copy, PATH_MAX_LENGTH, dir, path, 1, image,
         len);
   ok =     filestream_matches_buf(copy, image, len)
         || (   path_mkdir(dir)
             && save_item_write_file(copy, tmp, image, len, false));
#ifdef HAVE_THREADS
   if (claimed)
      save_undo_release(bit);
#endif
   free(copy);
   return ok ? SAVE_ITEM_COPY_MADE : SAVE_ITEM_COPY_FAILED;
}

/* Copies the file at @path into @dir before a write drops its end or
 * replaces it, through the writer's @suffix, as copy *@n
 * (save_item_copy_image()). Either thread; logs nothing. */
static enum save_item_copy save_item_copy_file(const char *path,
      const char *dir, const char *suffix, unsigned *n, bool turn)
{
   enum save_item_copy copy = SAVE_ITEM_COPY_FAILED;
   int64_t len              = 0;
   void *buf                = NULL;

   *n = 1;
   if (!path_is_valid(path))
      return SAVE_ITEM_NO_FILE;
   if (     !string_is_empty(dir)
         && filestream_read_file(path, &buf, &len))
      copy = save_item_copy_image(path, dir, suffix, buf, (size_t)len, n,
            turn);
   free(buf);
   return copy;
}

/* Logs copy @n made in @dir, and the @dropped bytes it lets an
 * EXACT_SIZE item drop. */
static void save_item_log_copy(const char *path, const char *dir,
      unsigned n, size_t dropped)
{
   char *copy = (char*)malloc(PATH_MAX_LENGTH);
   if (!copy || string_is_empty(dir))
   {
      free(copy);
      return;
   }
   save_item_copy_name(copy, PATH_MAX_LENGTH, dir, path, n, NULL, 0);
   if (dropped)
      RARCH_LOG("[SRAM] Kept a copy of \"%s\" in \"%s\"; dropping its "
            "last %u bytes.\n", path, copy, (unsigned)dropped);
   else
      RARCH_LOG("[SRAM] Kept a copy of \"%s\" in \"%s\".\n", path, copy);
   free(copy);
}

/* A warning in the log and on screen, naming @a and, when the message
 * has a second %s, @b. Main thread. */
static void save_item_warn(enum msg_hash_enums msg, const char *a,
      const char *b)
{
   const char *fmt = msg_hash_to_str(msg);
   size_t len      = strlen(fmt) + strlen(a) + (b ? strlen(b) : 0) + 1;
   char *s         = (char*)malloc(len);
   if (!s)
      return;
   snprintf(s, len, fmt, a, b ? b : "");
   RARCH_WARN("[SRAM] %s\n", s);
   runloop_msg_queue_push(s, strlen(s), 1, 180, false, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_WARNING);
   free(s);
}

/* On screen: copy @n in @dir of a file a replaced item kept another
 * way is made. */
static void save_item_warn_type_changed(const char *path, const char *dir,
      unsigned n)
{
   char *copy = (char*)malloc(PATH_MAX_LENGTH);
   if (copy && !string_is_empty(dir))
   {
      save_item_copy_name(copy, PATH_MAX_LENGTH, dir, path, n, NULL, 0);
      if (path_is_valid(copy))
         save_item_warn(MSG_SAVE_ITEM_TYPE_CHANGED, path, copy);
   }
   free(copy);
}

/* Logs and shows that a write of @path failed. Main thread. */
static void save_item_warn_failed(const char *path)
{
   const char *msg = msg_hash_to_str(MSG_FAILED_TO_SAVE_SRAM);
   size_t len      = strlen(msg) + strlen(path) + 4;
   char *s         = (char*)malloc(len);
   if (!s)
      return;
   snprintf(s, len, "%s \"%s\"", msg, path);
   RARCH_ERR("[SRAM] %s.\n", s);
   runloop_msg_queue_push(s, strlen(s), 1, 180, false, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);
   free(s);
}

/* A blob over its max_size (@over), or whose blob_save() returned
 * false, is not saved: logged once per item. */
static void save_item_blob_unsaved(const save_items_set_t *set,
      unsigned i, size_t len, bool over)
{
   char who[256];
   const save_item_t *it = &set->items[i];
   save_item_file_t *f   = &save_item_files.files[i];
   if (     it->item.storage != RETRO_SAVE_STORAGE_FRONTEND_BLOB
         || f->blob_told
         || (over && (!it->item.max_size || len <= it->item.max_size)))
      return;
   f->blob_told = true;
   save_items_who(who, sizeof(who), set, i);
   if (over)
      RARCH_WARN("[SRAM] %s: blob_size = %lu: over its max_size %lu. See "
            "libretro.h, retro_save_item::max_size. \"%s\" is not saved."
            "\n", who, (unsigned long)len,
            (unsigned long)it->item.max_size, it->path);
   else
      RARCH_WARN("[SRAM] %s: blob_save = false for %lu bytes. See "
            "libretro.h, retro_save_blob_save_t. \"%s\" is not saved.\n",
            who, (unsigned long)len, it->path);
}

/* @guard's image could not be saved to @path: logged each time, shown
 * at the second failure in a row. */
static void save_item_write_failed(save_item_guard_t *guard,
      const char *path)
{
   if (guard->save_fails == 1)
      save_item_warn_failed(path);
   else
      RARCH_ERR("[SRAM] %s \"%s\".\n",
            msg_hash_to_str(MSG_FAILED_TO_SAVE_SRAM), path);
   if (guard->save_fails < 2)
      guard->save_fails++;
}

/* The image in recovery file *@untold, which no one was told of, is
 * at its path now, or a newer one is. */
static void save_item_untold_drop(char **untold)
{
   if (!*untold)
      return;
   filestream_delete(*untold);
   free(*untold);
   *untold = NULL;
}

/* @guard's copy was made: it owes none. */
static void save_item_guard_met(save_item_guard_t *guard)
{
   free(guard->copy_image);
   guard->copy_image   = NULL;
   guard->copy_first   = false;
   guard->keep_file    = false;
   guard->type_changed = false;
}

/* The write *@untold waited for will not come: its file is named as
 * save_item_recover() names one, and is *@recovered from then on. */
static void save_item_untold_tell(const char *path, char **untold,
      char **recovered)
{
   if (!*untold)
      return;
   if (*recovered && filestream_rename(*untold, *recovered) == 0)
   {
      free(*untold);
      *untold = NULL;
      return;
   }
   save_item_warn(MSG_SAVE_ITEM_FILE_RECOVERED, path, *untold);
   free(*recovered);
   *recovered = *untold;
   *untold    = NULL;
}

#ifdef HAVE_THREADS
/* Writes @data to recovery file *@untold, or to a new one no one is
 * told of; the same image is not written again. */
static bool save_item_untold_write(char **untold, const void *data,
      size_t len, unsigned type)
{
   char *s;
   bool ok = false;
   if (*untold && (   filestream_matches_buf(*untold, data, len)
                   || filestream_write_file_atomic(*untold, data, len)))
      return true;
   s = (char*)malloc(PATH_MAX_LENGTH + 256 + 32);
   if (s && (ok = dump_to_file_desperate(data, len, type, s,
            PATH_MAX_LENGTH + 256 + 32)))
   {
      save_item_untold_drop(untold);
      *untold = strdup(s);
   }
   else
      RARCH_WARN("[SRAM] Failed. Cannot recover save file.\n");
   free(s);
   return ok;
}

/* superseded moved past @mark. */
static bool autosave_mark_reached(int now, int mark)
{
   return (int)((unsigned)now - (unsigned)mark) >= 0;
}
#endif

/* An old core's save RAM is shorter than its file, whose end the next
 * write cuts: the whole file, already read into @buf, is copied first,
 * and a warning is shown. The .srm is written as it was before the save
 * item interface. */
static void save_ram_keep_copy(const char *path, unsigned type,
      const void *buf, size_t len)
{
   bool kept;
   bool undo   = false;
   char **dump = type <= RETRO_MEMORY_RTC ? &save_ram_cut_dumps[type]
      : NULL;
   char *copy  = (char*)malloc(PATH_MAX_LENGTH + 256 + 32);
   if (copy && !string_is_empty(save_undo_dir))
   {
      save_item_copy_name(copy, PATH_MAX_LENGTH, save_undo_dir, path, 1,
            buf, len);
      if ((undo =    filestream_matches_buf(copy, buf, len)
                  || (   path_mkdir(save_undo_dir)
                      && filestream_write_file_atomic(copy, buf, len))))
         RARCH_LOG("[SRAM] Kept a copy of \"%s\" in \"%s\".\n", path, copy);
   }
   kept = undo;
   /* Loaded again: the same bytes need no second recovery file. */
   if (!kept && dump && *dump && filestream_matches_buf(*dump, buf, len))
   {
      free(copy);
      return;
   }
   if (     copy && !kept
         && (kept = dump_to_file_desperate(buf, len, RETRO_MEMORY_SAVE_RAM,
               copy, PATH_MAX_LENGTH + 256 + 32))
         && dump)
   {
      free(*dump);
      *dump = strdup(copy);
   }
   /* Copies in the undo folder are shown once; each other cut is,
    * once for each memory when it has no copy. */
   if (!undo || !save_ram_cut_told)
   {
      if (undo)
         save_ram_cut_told = true;
      if (kept)
         save_item_warn(MSG_SAVE_RAM_CUT_COPIED, path, copy);
      else if (!dump || !save_ram_cut_bare[type])
      {
         if (dump)
            save_ram_cut_bare[type] = true;
         save_item_warn(MSG_SAVE_RAM_CUT, path, NULL);
      }
   }
   free(copy);
}

#ifdef HAVE_THREADS
/* An image the main thread wrote, which a worker may write again. The
 * bytes follow. */
typedef struct autosave_image
{
   size_t size;
} autosave_image_t;

/* Seconds of the core's frames while can_snapshot() says false before
 * autosave copies anyway. */
#define AUTOSAVE_SETTLE_SECONDS 10

/* Autosave support. */
struct autosave_st
{
   /* The core's frame rate, from event_save_items_ran(). */
   double fps;
   autosave_t **list;
   /* Item handles stopped on re-registration, freed once their
    * worker has exited. */
   autosave_t **retired;
   /* By slot: save RAM retired at the start, started once given back
    * and nothing owes its file a write. */
   bool *waits;
   unsigned num;
   unsigned num_retired;
   /* Room in retired, kept for every running item handle too, so
    * stopping one never allocates. */
   unsigned retired_cap;
   unsigned depth;
   /* 0 while autosave is off. */
   unsigned interval;
   /* Frames the core ran, counted by event_save_items_ran(). */
   unsigned frames;
   bool compress;
};

struct autosave
{
   void *buffer;
   void *snapshot;
   const void *retro_buffer;
   /* Save RAM writes in place, an item whole or not at all. */
   bool (*write)(autosave_t *save, const void *data, size_t len);
   /* A retired item's last image, handed over with quit. */
   void *final;
   char *path;
   /* The worker's own copy of save_undo_dir; NULL for none. */
   char *undo_dir;
   /* The record's copy_image, until the worker has copied it; freed
    * once the worker has exited. */
   void *copy_image;
   sthread_t *thread;
   size_t bufsize;
   /* Bytes in snapshot, and room for them: an item's size can
    * change. */
   size_t snapshot_size;
   size_t snapshot_cap;
   size_t final_size;
   size_t copy_size;
   /* What an EXACT_SIZE item drops from the end of the snapshot, and of
    * final, once its file is copied. */
   size_t drop_size;
   size_t final_drop;
   /* Worker only: drop_size when its copy was made. */
   size_t copy_drop;
   /* get_generation() when the snapshot was taken. */
   uint64_t generation;
   /* What the worker sleeps on: the copy it asked for, the end of its
    * interval, or quit. */
   retro_eventcount_t ec;
   unsigned interval;
   /* The save item it writes; SAVE_ITEMS_NONE for save RAM. */
   unsigned item;
   /* Main thread only: the retired item's legacy memory, which names
    * its recovery file. */
   unsigned type;
   /* Only the main thread reads live SRAM. The worker requests a copy
    * and waits until autosave_check() has made it and cleared this. */
   retro_atomic_int_t snapshot_requested;
   retro_atomic_int_t quit;
   /* The worker is done with the handle. */
   retro_atomic_int_t exited;
   /* The worker's last write failed. */
   retro_atomic_int_t failed;
   /* Its write of final failed. Stored before exited. */
   retro_atomic_int_t final_failed;
   /* Copy the file before the next write. The main thread adds one for
    * each copy owed; the worker clears only the value its copy began
    * under, then sets copied (enum save_item_copy bits) for the main
    * thread. */
   retro_atomic_int_t copy_first;
   retro_atomic_int_t copied;
   /* The number of the last copy made (save_item_copy_name()). */
   retro_atomic_int_t copy_number;
   /* Main thread only: the recovery file no one was told of that holds
    * the newest image of the item it wrote when that closed; deleted
    * once the worker has exited. */
   char *untold;
   /* No write without the copy (save_item_guard_t::keep_file); the
    * worker clears it once the copy is made. */
   retro_atomic_int_t keep_file;
   /* The main thread adds one when a newer image of the file is
    * written or handed to another worker; an image taken under an
    * older value is not renamed into place. */
   retro_atomic_int_t superseded;
   /* The image the main thread wrote at the last superseded, which the
    * worker writes again when its own rename may have landed after. */
   retro_atomic_ptr_t reissue;
   /* superseded when the image the worker last put at the file was
    * taken. */
   retro_atomic_int_t written;
   /* superseded when the snapshot, and final, were taken. */
   int fill_mark;
   int final_mark;
   /* Main thread only: frames when can_snapshot() first refused the
    * pending copy. */
   unsigned refused_at;
   bool compress;
   bool ec_inited;
   bool has_generation;
   /* can_snapshot() refuses the pending copy. */
   bool refusing;
   /* The snapshot was taken while can_snapshot() said true. */
   bool settled;
   /* Worker only until joined: its last snapshot is at the file. */
   bool landed;
   /* Worker only until joined: its last write was the main thread's
    * image again (autosave_reissue()). */
   bool reissued;
   /* Worker only until joined: its last rename may have landed over a
    * newer image of the main thread's that was not written again. */
   bool over_newer;
   /* Main thread only: superseded after its last write of the file,
    * and whether can_snapshot() said true for that image. */
   int wrote_mark;
   bool wrote_settled;
   /* Worker only: copy_image is copied; a later copy is of the file. */
   bool image_copied;
   /* Worker only: the copy is made, so drop_size applies. */
   bool copy_made;
   /* Worker only: the file may hold another writer's image, so the
    * next image is written even if unchanged. */
   bool rewrite;
   /* Worker only: the last write waited for a copy; nothing failed. */
   bool held_back;
   /* Main thread only: the snapshot holds the item's current tail. */
   bool tail_kept;
   /* Main thread only: the retired item owed a type change's copy. */
   bool type_changed;
   /* Main thread only: SAVE_FILES made its copy and told it. */
   bool copy_told;
};

static struct autosave_st autosave_state;

static bool autosave_quitting(autosave_t *save)
{
   return retro_atomic_load_acquire_int(&save->quit) != 0;
}

/* Keeps @len bytes just written as the image the next snapshot is
 * compared against. */
static void autosave_keep(autosave_t *save, const void *data, size_t len)
{
   if (len != save->bufsize)
   {
      void *resized = realloc(save->buffer, len);
      /* Unrecorded, it differs next time and is written again. */
      if (!resized)
         return;
      save->buffer  = resized;
      save->bufsize = len;
   }
   memcpy(save->buffer, data, len);
}

/* Save RAM's write, in place. */
static bool autosave_write(autosave_t *save, const void *data, size_t len)
{
   bool written;
   intfstream_t *file = NULL;

   if (save->compress)
      file = intfstream_open_rzip_file(save->path,
            RETRO_VFS_FILE_ACCESS_WRITE);
   else
      file = intfstream_open_file(save->path,
            RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if (!file)
      return false;

   written = intfstream_write(file, data, len) == (int64_t)len;
   if (intfstream_flush(file) != 0)
      written = false;
   if (intfstream_close(file) != 0)
      written = false;
   free(file);
   if (!written)
      return false;
   autosave_keep(save, data, len);
   save->rewrite = false;
   return true;
}

/* After a rename: the main thread wrote the file since the check
 * before it, so this rename may have landed over that newer image,
 * which is written again unless a newer one follows. */
static void autosave_reissue(autosave_t *save)
{
   int mark = save->fill_mark;
   for (;;)
   {
      char *tmp;
      autosave_image_t *image;
      int now = retro_atomic_load_acquire_int(&save->superseded);
      if (now == mark || !(image = (autosave_image_t*)
            retro_atomic_exchange_ptr(&save->reissue, NULL)))
         return;
      mark = now;
      if ((tmp = save_item_write_temp(save->path, SAVE_ITEM_AUTO_TMP,
            image + 1, image->size, save->compress)))
      {
         if (retro_atomic_load_acquire_int(&save->superseded) != mark)
         {
            filestream_delete(tmp);
            free(tmp);
         }
         else if (save_item_rename_temp(tmp, save->path))
         {
            retro_atomic_store_release_int(&save->written, mark);
            save->reissued = true;
         }
      }
      free(image);
   }
}

/* An item's write, whole or not at all, after the safety copy it owes.
 * Without the copy a refused file is left alone and a longer one keeps
 * its end this time. */
static bool autosave_write_item(autosave_t *save, const void *data,
      size_t len)
{
   char *tmp;
   int owed = retro_atomic_load_acquire_int(&save->copy_first);
   if (owed)
   {
      unsigned n;
      enum save_item_copy copy = save->copy_image && !save->image_copied
         ? save_item_copy_image(save->path, save->undo_dir,
               SAVE_ITEM_AUTO_TMP, save->copy_image, save->copy_size, &n,
               !autosave_quitting(save))
         : save_item_copy_file(save->path, save->undo_dir,
               SAVE_ITEM_AUTO_TMP, &n, !autosave_quitting(save));
      if (copy == SAVE_ITEM_COPY_BUSY)
      {
         /* Made at the next write. */
         save->copy_made = false;
         if (retro_atomic_load_acquire_int(&save->keep_file))
         {
            save->held_back = true;
            return false;
         }
      }
      else if ((save->copy_made = copy != SAVE_ITEM_COPY_FAILED))
      {
         save->image_copied = save->copy_image != NULL;
         save->copy_drop    = save->drop_size;
         retro_atomic_store_release_int(&save->keep_file, 0);
         /* A copy owed since this one began stays owed. */
         retro_atomic_cas_int(&save->copy_first, owed, 0);
         retro_atomic_store_release_int(&save->copy_number, (int)n);
         retro_atomic_fetch_or_int(&save->copied, copy);
      }
      else if (retro_atomic_load_acquire_int(&save->keep_file))
         return false;
   }
   if (save->copy_made)
      len -= save->drop_size;
   if (!(tmp = save_item_write_temp(save->path, SAVE_ITEM_AUTO_TMP, data,
         len, save->compress)))
      return false;
   if (retro_atomic_load_acquire_int(&save->superseded) != save->fill_mark)
   {
      filestream_delete(tmp);
      free(tmp);
      save->rewrite = true;
      return true;
   }
   if (!save_item_rename_temp(tmp, save->path))
      return false;
   retro_atomic_store_release_int(&save->written, save->fill_mark);
   save->reissued = false;
   autosave_reissue(save);
   /* A newer write may have landed just before this one. */
   save->rewrite = retro_atomic_load_acquire_int(&save->superseded)
      != save->fill_mark;
   save->over_newer = save->rewrite && !save->reissued;
   autosave_keep(save, data, len);
   return true;
}

static void autosave_thread(void *data)
{
   autosave_t *save = (autosave_t*)data;
   bool retry_write = false;
   /* Failed writes in a row: the second is told. */
   int fails        = 0;

   while (!autosave_quitting(save))
   {
      /* Ask for a copy and wait until the main thread has made it. */
      retro_atomic_store_release_int(&save->snapshot_requested, 1);
      for (;;)
      {
         int key;
         if (     autosave_quitting(save)
               || !retro_atomic_load_acquire_int(&save->snapshot_requested))
            break;
         key = retro_eventcount_prepare_wait(&save->ec);
         if (     autosave_quitting(save)
               || !retro_atomic_load_acquire_int(&save->snapshot_requested))
         {
            retro_eventcount_cancel_wait(&save->ec);
            break;
         }
         retro_eventcount_commit_wait(&save->ec, key);
      }

      /* A settled snapshot taken before quit is the one a close
       * keeps: written still. */
      if (     autosave_quitting(save)
            && (   retro_atomic_load_acquire_int(&save->snapshot_requested)
                || !save->settled))
         break;

      /* The snapshot remains ours until the next request. Keep the
       * last written image unchanged if opening or writing fails. */
      if (     retry_write
            || save->rewrite
            || save->snapshot_size != save->bufsize
            || memcmp(save->buffer, save->snapshot, save->bufsize) != 0)
      {
         save->held_back = false;
         retry_write     = !save->write(save, save->snapshot,
               save->snapshot_size);
         if (!retry_write)
            fails = 0;
         else if (!save->held_back && fails < 2)
            fails++;
         if (!save->held_back)
            retro_atomic_store_release_int(&save->failed, fails);
      }
      save->landed = !retry_write && !save->rewrite;

      /* Rest an interval. Nothing but quit notifies while no copy is
       * asked for, so a wake before it runs out is quit's. */
      if (save->interval)
      {
         int key = retro_eventcount_prepare_wait(&save->ec);
         if (autosave_quitting(save))
            retro_eventcount_cancel_wait(&save->ec);
         else
            retro_eventcount_commit_wait_timeout(&save->ec, key,
#if defined(_MSC_VER) && _MSC_VER <= 1200
                  save->interval * 1000000
#else
                  save->interval * 1000000LL
#endif
                  );
      }
   }

   save->drop_size = save->final_drop;
   save->fill_mark = save->final_mark;
   /* Written while a copy is owed, changed or not. A failed last
    * write is the main thread's to recover. */
   if (     save->final
         && retro_atomic_load_acquire_int(&save->superseded)
            == save->final_mark
         && (   retro_atomic_load_acquire_int(&save->copy_first)
             || save->rewrite
             || save->final_size != save->bufsize
             || memcmp(save->buffer, save->final, save->bufsize) != 0)
         && !save->write(save, save->final, save->final_size))
      retro_atomic_store_release_int(&save->final_failed, 1);
   retro_atomic_store_release_int(&save->exited, 1);
}

/* What autosave_new() does not take from its arguments: nothing
 * written, handed or owed yet. */
static void autosave_init_state(autosave_t *handle)
{
   retro_atomic_int_init(&handle->exited, 0);
   retro_atomic_int_init(&handle->failed, 0);
   retro_atomic_int_init(&handle->final_failed, 0);
   handle->write                 = autosave_write;
   handle->generation            = 0;
   handle->has_generation        = false;
   handle->refused_at            = 0;
   handle->refusing              = false;
   handle->settled               = false;
   handle->landed                = false;
   handle->reissued              = false;
   handle->over_newer            = false;
   handle->wrote_mark            = 0;
   handle->wrote_settled         = false;
   handle->item                  = SAVE_ITEMS_NONE;
   handle->type                  = 0;
   handle->final                 = NULL;
   handle->untold                = NULL;
   handle->final_size            = 0;
   retro_atomic_int_init(&handle->copy_first, 0);
   retro_atomic_int_init(&handle->copied, 0);
   retro_atomic_int_init(&handle->copy_number, 1);
   retro_atomic_int_init(&handle->keep_file, 0);
   retro_atomic_int_init(&handle->superseded, 0);
   retro_atomic_int_init(&handle->written, 0);
   retro_atomic_ptr_init(&handle->reissue, NULL);
   handle->fill_mark             = 0;
   handle->final_mark            = 0;
   handle->undo_dir              = NULL;
   handle->drop_size             = 0;
   handle->final_drop            = 0;
   handle->copy_drop             = 0;
   handle->copy_image            = NULL;
   handle->copy_size             = 0;
   handle->image_copied          = false;
   handle->copy_made             = false;
   handle->rewrite               = false;
   handle->held_back             = false;
   handle->tail_kept             = false;
   handle->type_changed          = false;
   handle->copy_told             = false;
}

/**
 * autosave_new:
 * @path            : path to autosave file
 * @data            : pointer to buffer
 * @len             : size of @data buffer
 * @interval        : interval at which saves should be performed.
 *
 * Create and initialize autosave object.
 *
 * @return Pointer to new autosave_t object if successful, otherwise
 * NULL.
 **/
static autosave_t *autosave_new(const char *path,
      const void *data, size_t len,
      unsigned interval, bool compress)
{
   void       *buf               = NULL;
   autosave_t *handle            = (autosave_t*)malloc(sizeof(*handle));
   if (!handle)
      return NULL;

   handle->compress              = compress;
   handle->ec_inited             = false;
   retro_atomic_int_init(&handle->quit, 0);
   retro_atomic_int_init(&handle->snapshot_requested, 0);
   handle->bufsize               = len;
   handle->snapshot_size         = len;
   handle->snapshot_cap          = len;
   handle->interval              = interval;
   handle->buffer                = NULL;
   handle->snapshot              = NULL;
   handle->thread                = NULL;
   autosave_init_state(handle);

   handle->retro_buffer          = data;
   /* Own the path string rather than borrowing it. The caller's
    * path comes from task_save_files->elems[i].data, freed by
    * path_deinit_savefile() during the deinit chain. The worker
    * thread reads handle->path via intfstream_open_*(). Owning
    * the string keeps the lifetime contract local to
    * autosave_new/autosave_free rather than depending on top-
    * level deinit ordering at every call site. */
   if (!(handle->path = strdup(path)))
   {
      free(handle);
      return NULL;
   }

   if (!(buf = malloc(len)))
   {
      free(handle->path);
      free(handle);
      return NULL;
   }

   handle->buffer                = buf;

   memcpy(handle->buffer, handle->retro_buffer, handle->bufsize);

   handle->snapshot              = malloc(len);
   if (handle->snapshot)
      handle->ec_inited          = retro_eventcount_init(&handle->ec);

   if (!handle->snapshot || !handle->ec_inited)
   {
      RARCH_ERR("[SRAM] Failed to initialize autosave synchronization primitives.\n");
      free(handle->snapshot);
      if (handle->ec_inited)
         retro_eventcount_free(&handle->ec);
      free(handle->path);
      free(handle->buffer);
      free(handle);
      return NULL;
   }

   handle->thread                = sthread_create(autosave_thread, handle);

   if (!handle->thread)
   {
      RARCH_ERR("[SRAM] Failed to create autosave thread.\n");
      free(handle->snapshot);
      retro_eventcount_free(&handle->ec);
      free(handle->path);
      free(handle->buffer);
      free(handle);
      return NULL;
   }

   return handle;
}

/**
 * autosave_free:
 * @handle          : pointer to autosave object
 *
 * Frees autosave object and all associated resources.
 **/
static void autosave_free(autosave_t *handle)
{
   retro_atomic_store_release_int(&handle->quit, 1);
   retro_eventcount_notify(&handle->ec);
   sthread_join(handle->thread);
   save_item_untold_drop(&handle->untold);

   free(retro_atomic_load_acquire_ptr(&handle->reissue));
   free(handle->snapshot);
   free(handle->final);
   free(handle->copy_image);
   free(handle->undo_dir);
   retro_eventcount_free(&handle->ec);

   if (handle->buffer)
      free(handle->buffer);
   handle->buffer = NULL;

   if (handle->path)
      free(handle->path);
   handle->path = NULL;

   free(handle);
}

/* Copies a registered item into its handle's snapshot. False leaves
 * the request pending: the item has nothing to copy right now. */
/* At a save of the files: an item whose generation stood still since
 * its last autosave copy while its bytes changed is copied every
 * interval from now on. Logged once. */
static void save_item_check_generation(const save_items_set_t *set,
      unsigned i, const void *data, size_t len)
{
   char who[256];
   save_item_file_t *f = &save_item_files.files[i];
   autosave_t *h       = f->autosave;
   if (     !h || !h->has_generation || f->generation_stuck
         || save_items_generation(set, i) != h->generation
         || (   len == f->copy_crc_size
             && encoding_crc32(0, (const uint8_t*)data, len) == f->copy_crc))
      return;
   f->generation_stuck = true;
   h->has_generation   = false;
   save_items_who(who, sizeof(who), set, i);
   RARCH_WARN("[SRAM] %s: get_generation = %lu: it did not change while "
         "the bytes did. See libretro.h, retro_save_get_generation_t. "
         "Autosave now copies the item every interval.\n", who,
         (unsigned long)h->generation);
}

static bool autosave_fill_item(autosave_t *handle)
{
   size_t len;
   size_t tail;
   save_item_file_t *f;
   uint64_t generation         = 0;
   bool settled                = false;
   const save_items_set_t *set = save_items_state_get_ptr()->current;

   if (!set || handle->item >= set->num_items)
      return false;
   f = &save_item_files.files[handle->item];
   /* An unchanged generation stands for an unchanged image, unless the
    * last write failed. */
   if (set->iface.get_generation && !f->generation_stuck)
   {
      generation = save_items_generation(set, handle->item);
      if (     handle->has_generation
            && generation == handle->generation
            && !retro_atomic_load_acquire_int(&handle->failed))
         return false;
   }
   /* A grow waits for its fill from the tail at the frame start. */
   if (save_item_files.files[handle->item].grow_from)
      return false;
   /* Mid-save: asked again each frame, copied anyway at the cap. */
   if (set->iface.can_snapshot)
   {
      settled = save_items_can_snapshot(set, handle->item);
      if (settled)
         handle->refusing = false;
      else
      {
         if (!handle->refusing)
         {
            handle->refusing   = true;
            handle->refused_at = autosave_state.frames;
         }
         if (autosave_state.frames - handle->refused_at
               < (autosave_state.fps > 0.0 ? autosave_state.fps : 60.0)
               * AUTOSAVE_SETTLE_SECONDS)
            return false;
      }
   }
   /* blob_size() once a tick, over max_size or not. */
   if (     set->items[handle->item].item.storage
         == RETRO_SAVE_STORAGE_FRONTEND_BLOB)
   {
      size_t max = set->items[handle->item].item.max_size;
      len        = save_items_blob_size(set, handle->item);
      if (max && len > max)
      {
         save_item_blob_unsaved(set, handle->item, len, true);
         return false;
      }
   }
   else
      len = save_items_size(set, handle->item);
   if (!len)
      return false;
   tail = set->items[handle->item].tail_size;
   if (len > handle->snapshot_cap)
   {
      void *grown = realloc(handle->snapshot, len);
      if (!grown)
         return false;
      handle->snapshot     = grown;
      handle->snapshot_cap = len;
      handle->tail_kept    = false;
   }
   /* The tail goes into the snapshot once, the segments every time. */
   if (handle->tail_kept && tail && len == handle->snapshot_size)
   {
      if (!save_items_copy_out_head(set, handle->item, handle->snapshot,
               len - tail))
         return false;
   }
   else
   {
      /* A short blob_save() leaves zeros, not an older image. */
      if (     set->items[handle->item].item.storage
            == RETRO_SAVE_STORAGE_FRONTEND_BLOB)
         memset(handle->snapshot, 0, len);
      if (!save_items_copy_out(set, handle->item, handle->snapshot, len))
      {
         save_item_blob_unsaved(set, handle->item, len, false);
         return false;
      }
   }
   if (set->iface.get_generation)
   {
      f->copy_crc      = encoding_crc32(0, (const uint8_t*)handle->snapshot,
            len);
      f->copy_crc_size = len;
   }
   handle->tail_kept      = tail != 0;
   handle->drop_size      = (set->items[handle->item].item.flags
         & RETRO_SAVE_FLAG_EXACT_SIZE) ? tail : 0;
   handle->snapshot_size  = len;
   handle->generation     = generation;
   handle->has_generation =    set->iface.get_generation != NULL
                            && !f->generation_stuck;
   handle->refusing       = false;
   handle->settled        = settled;
   handle->fill_mark      = retro_atomic_load_acquire_int(
         &handle->superseded);
   if (     set->iface.can_snapshot && !settled
         && !save_item_files.files[handle->item].cap_told)
   {
      save_item_files.files[handle->item].cap_told = true;
      RARCH_WARN("[SRAM] Autosave copied \"%s\" while the game was still "
            "saving it.\n", set->items[handle->item].path);
   }
   return true;
}

static autosave_t *autosave_new_item(const save_items_set_t *set,
      unsigned i)
{
   autosave_t *handle  = NULL;
   save_item_file_t *f = &save_item_files.files[i];
   uint64_t generation = save_items_generation(set, i);
   size_t len          = save_items_size(set, i);
   void *image         = len ? malloc(len) : NULL;

   /* Until it has a file, the worker compares against what the item
    * held before an earlier name filled it; the copy still shows that
    * the item can be read. */
   if (image && save_items_copy_out(set, i, image, len))
   {
      f->copy_crc      = encoding_crc32(0, (const uint8_t*)image, len);
      f->copy_crc_size = len;
      handle = autosave_new(set->items[i].path,
            f->initial ? f->initial      : image,
            f->initial ? f->initial_size : len,
            autosave_state.interval,
               autosave_state.compress
            && (set->items[i].item.flags
               & RETRO_SAVE_FLAG_FRONTEND_MAY_TRANSFORM));
   }
   free(image);
   if (handle)
   {
      /* Read by the worker only after the first snapshot publishes. */
      save_item_guard_t *guard = &save_item_files.files[i].guard;
      handle->write          = autosave_write_item;
      handle->retro_buffer   = NULL;
      handle->item           = i;
      handle->generation     = generation;
      handle->has_generation =    set->iface.get_generation != NULL
                               && !f->generation_stuck;
      handle->undo_dir       = save_undo_dir ? strdup(save_undo_dir) : NULL;
      handle->copy_image     = guard->copy_image;
      handle->copy_size      = guard->copy_size;
      guard->copy_image      = NULL;
      retro_atomic_store_release_int(&handle->keep_file, guard->keep_file);
      retro_atomic_store_release_int(&handle->copy_first,
            guard->copy_first);
   }
   else if (len || set->iface.get_generation)
   {
      f->start_failed     = true;
      f->start_generation = generation;
      if (len && !f->start_told)
      {
         f->start_told = true;
         RARCH_WARN("[SRAM] %s\n", msg_hash_to_str(MSG_AUTOSAVE_FAILED));
      }
   }
   return handle;
}

/* A retired handle of @path is not reaped yet: it may still write, or
 * owe a copy the reap passes to the item at the file. */
static bool autosave_retired_writes(const char *path)
{
   unsigned i;
   for (i = 0; i < autosave_state.num_retired; i++)
      if (string_is_equal_noncase(autosave_state.retired[i]->path, path))
         return true;
   return false;
}

/* An orphan's image is still to be written to @path. */
static bool save_item_orphan_waits(const char *path)
{
   unsigned i;
   for (i = 0; i < save_item_files.num_orphans; i++)
      if (     save_item_files.orphans[i].data
            && string_is_equal_noncase(save_item_files.orphans[i].path,
               path))
         return true;
   return false;
}

/* Starts a handle for each frontend-owned item without one, once it
 * has been filled and no retired worker or orphan writes its file: one
 * worker per file. It calls the core: never inside a registration. */
static void autosave_items_start(void)
{
   unsigned i;
   unsigned live               = 0;
   const save_items_set_t *set = save_items_state_get_ptr()->current;

   if (!autosave_state.interval || !save_item_files.loaded || !set)
      return;
   for (i = 0; i < save_item_files.num_files; i++)
      if (save_item_files.files[i].autosave)
         live++;
   for (i = 0; i < save_item_files.num_files; i++)
   {
      save_item_file_t *f = &save_item_files.files[i];
      if (     f->autosave
            || !f->taken_up
            || !save_items_frontend_owned(&set->items[i])
            || autosave_retired_writes(set->items[i].path)
            || save_item_orphan_waits(set->items[i].path))
         continue;
      if (     f->start_failed
            && (     !set->iface.get_generation
                  || save_items_generation(set, i) == f->start_generation))
         continue;
      /* Its slot in retired first. */
      if (autosave_state.num_retired + live >= autosave_state.retired_cap)
      {
         unsigned cap      = autosave_state.num_retired + live + 1;
         autosave_t **list = (autosave_t**)realloc(autosave_state.retired,
               cap * sizeof(*list));
         if (!list)
            return;
         autosave_state.retired     = list;
         autosave_state.retired_cap = cap;
      }
      if ((f->autosave = autosave_new_item(set, i)))
      {
         f->start_failed = false;
         live++;
      }
   }
}

/* @h's worker is to write @data again after a rename that may land
 * over it; NULL: nothing. Then superseded moves. */
static void autosave_bump(autosave_t *h, const void *data, size_t len)
{
   autosave_image_t *image = data
      ? (autosave_image_t*)malloc(sizeof(*image) + len) : NULL;
   if (image)
   {
      image->size = len;
      memcpy(image + 1, data, len);
   }
   free(retro_atomic_exchange_ptr(&h->reissue, image));
   retro_atomic_inc_int(&h->superseded);
}

/* Stops an item handle without waiting for it. Its worker writes
 * @final if that differs from what it wrote last or a copy is still
 * owed, then exits, and a later autosave_check() frees it, telling a
 * copy it made (the type change's notice too when @type_changed) and
 * recovering @final as a @type save when that write failed. Takes
 * @final. */
static void autosave_retire(autosave_t *handle, void *final,
      size_t final_size, size_t final_drop, unsigned type,
      bool type_changed)
{
   unsigned i;

   /* At most one last image waits for a file: the newest. */
   for (i = 0; final && i < autosave_state.num_retired; i++)
      if (string_is_equal_noncase(autosave_state.retired[i]->path,
               handle->path))
         autosave_bump(autosave_state.retired[i], NULL, 0);

   handle->final        = final;
   handle->final_size   = final_size;
   handle->final_drop   = final_drop;
   handle->type         = type;
   handle->type_changed = type_changed;
   handle->final_mark = retro_atomic_load_acquire_int(&handle->superseded);
   retro_atomic_store_release_int(&handle->quit, 1);
   retro_eventcount_notify(&handle->ec);
   /* Its slot was kept when it started. */
   autosave_state.retired[autosave_state.num_retired++] = handle;
}

/* A retired worker of @path has not made the copy it owes yet: made
 * here before a newer image lands, and told once whichever thread
 * makes it (a slot that holds the bytes is not written again). False
 * when it cannot be made and the file is kept. */
static bool autosave_retired_copy(const char *path)
{
   unsigned i;
   bool ok = true;
   for (i = 0; i < autosave_state.num_retired; i++)
   {
      unsigned n;
      enum save_item_copy copy;
      autosave_t *h = autosave_state.retired[i];
      int owed      = retro_atomic_load_acquire_int(&h->copy_first);
      if (!owed || !string_is_equal_noncase(h->path, path))
         continue;
      copy = h->copy_image
         ? save_item_copy_image(h->path, save_undo_dir, SAVE_ITEM_TMP,
               h->copy_image, h->copy_size, &n, false)
         : save_item_copy_file(h->path, save_undo_dir, SAVE_ITEM_TMP, &n,
               false);
      if (copy == SAVE_ITEM_COPY_FAILED)
      {
         if (retro_atomic_load_acquire_int(&h->keep_file))
            ok = false;
         continue;
      }
      retro_atomic_store_release_int(&h->keep_file, 0);
      retro_atomic_cas_int(&h->copy_first, owed, 0);
      if (copy == SAVE_ITEM_COPY_MADE && !h->copy_told)
      {
         h->copy_told = true;
         save_item_log_copy(h->path, save_undo_dir, n, 0);
         if (h->type_changed)
            save_item_warn_type_changed(h->path, save_undo_dir, n);
      }
   }
   return ok;
}

/* @path is to hold a newer image, written or already there: a worker's
 * older one is not renamed over it, whatever the case of its path, and
 * one renamed after @data is written goes under it again. */
static void autosave_supersede(const char *path, const void *data,
      size_t len)
{
   unsigned i;
   for (i = 0; i < save_item_files.num_files; i++)
   {
      autosave_t *handle = save_item_files.files[i].autosave;
      if (handle && string_is_equal_noncase(handle->path, path))
         autosave_bump(handle, data, len);
   }
   for (i = 0; i < autosave_state.num_retired; i++)
      if (string_is_equal_noncase(autosave_state.retired[i]->path, path))
         autosave_bump(autosave_state.retired[i], data, len);
}

/* A copy a retired worker made, told as save_item_file_collect() tells
 * a live worker's, with the type change's notice when the item owed it.
 * Main thread, once the worker has exited. */
static void autosave_report_copy(autosave_t *handle)
{
   unsigned n;
   if (     handle->copy_told
         || !(   retro_atomic_load_acquire_int(&handle->copied)
               & SAVE_ITEM_COPY_MADE))
      return;
   n = (unsigned)retro_atomic_load_acquire_int(&handle->copy_number);
   save_item_log_copy(handle->path, handle->undo_dir, n,
         handle->copy_drop);
   if (handle->type_changed)
      save_item_warn_type_changed(handle->path, handle->undo_dir, n);
}

/* What a retired worker that has exited left undone, on the main
 * thread: a failed last write goes to the recovery file, and a type
 * change's copy it still owed to a second one. A copy still owed
 * passes to the item now at the file, which keeps the file as its own
 * debt would; when its fill took that image, its recovery file starts
 * as the one the image went to, so the image is recovered once. */
static void autosave_settle(autosave_t *handle)
{
   unsigned i;
   save_item_file_t *f         = NULL;
   save_item_guard_t *guard    = NULL;
   const save_items_set_t *set = save_items_state_get_ptr()->current;
   bool owed = retro_atomic_load_acquire_int(&handle->copy_first) != 0;
   bool took = false;

   for (i = 0; set && !f && i < save_item_files.num_files; i++)
      if (     string_is_equal_noncase(set->items[i].path, handle->path)
            && save_items_frontend_owned(&set->items[i]))
         f = &save_item_files.files[i];
   if (f && f->took == handle)
   {
      f->took = NULL;
      took    = true;
   }
   if (f && owed)
      guard = &f->guard;
   if (     retro_atomic_load_acquire_int(&handle->final_failed)
         && !(   guard && guard->recovered
              && filestream_matches_buf(guard->recovered, handle->final,
                 handle->final_size)))
      save_item_recover(handle->path, handle->final, handle->final_size,
            handle->type,
            guard && took && !guard->recovered ? &guard->recovered : NULL);
   if (owed && handle->copy_image && !handle->image_copied)
      save_item_recover(handle->path, handle->copy_image,
            handle->copy_size, handle->type, NULL);
   if (guard)
   {
      guard->copy_first = true;
      if (retro_atomic_load_acquire_int(&handle->keep_file))
         guard->keep_file = true;
      if (handle->type_changed)
         guard->type_changed = true;
   }
}

/* Frees the retired handles whose worker has exited, telling a copy
 * each made and settling what it left undone first. Their join returns
 * at once: the thread has nothing left to do. A worker still writing
 * waits for a later call. */
static void autosave_reap(void)
{
   unsigned i = 0;
   while (i < autosave_state.num_retired)
   {
      autosave_t *handle = autosave_state.retired[i];
      if (!retro_atomic_load_acquire_int(&handle->exited))
      {
         i++;
         continue;
      }
      autosave_report_copy(handle);
      autosave_settle(handle);
      autosave_free(handle);
      autosave_state.retired[i] =
         autosave_state.retired[--autosave_state.num_retired];
   }
}

/* What item @i's worker left for the main thread: a copy made, to log,
 * and a failed write, to show. The worker logs nothing. */
static void save_item_file_collect(const save_items_set_t *set,
      unsigned i)
{
   save_item_file_t *f = &save_item_files.files[i];
   autosave_t *handle  = f->autosave;
   int copied          = retro_atomic_exchange_int(&handle->copied, 0);
   int failed          = retro_atomic_load_acquire_int(&handle->failed);
   if (copied)
   {
      if (copied & SAVE_ITEM_COPY_MADE)
      {
         unsigned n = (unsigned)retro_atomic_load_acquire_int(
               &handle->copy_number);
         save_item_log_copy(set->items[i].path, handle->undo_dir, n,
               (set->items[i].item.flags & RETRO_SAVE_FLAG_EXACT_SIZE)
               ? set->items[i].tail_size : 0);
         if (f->guard.type_changed)
            save_item_warn_type_changed(set->items[i].path,
                  handle->undo_dir, n);
      }
      f->guard.keep_file    = false;
      f->guard.type_changed = false;
      /* A re-registration since may owe another copy. */
      if (!retro_atomic_load_acquire_int(&handle->copy_first))
         save_item_copy_done(set, i);
   }
   if (f->guard.untold)
   {
      /* Its copy failed. */
      if (failed && f->guard.copy_first && f->guard.keep_file)
         save_item_untold_tell(set->items[i].path, &f->guard.untold,
               &f->guard.recovered);
      else if (autosave_mark_reached(
               retro_atomic_load_acquire_int(&handle->written),
               f->guard.untold_mark))
         save_item_untold_drop(&f->guard.untold);
   }
   if (!failed)
      f->failed_told = false;
   else if (failed > 1 && !f->failed_told)
   {
      f->failed_told = true;
      save_item_warn_failed(set->items[i].path);
   }
}

/* Stops every item handle, retired ones too, waiting for their
 * workers: only closing the content and changing the autosave
 * settings get here, as for save RAM. */
static void autosave_items_free(void)
{
   unsigned i;
   const save_items_set_t *set = save_items_state_get_ptr()->current;
   for (i = 0; i < save_item_files.num_files; i++)
   {
      autosave_t *handle = save_item_files.files[i].autosave;
      if (!handle)
         continue;
      /* Joined first: a copy the worker made is collected before the
       * main thread could take another. */
      retro_atomic_store_release_int(&handle->quit, 1);
      retro_eventcount_notify(&handle->ec);
      sthread_join(handle->thread);
      handle->thread = NULL;
      /* The main thread's image is at the file when the worker's last
       * was taken before it, or was that image again. */
      if (handle->over_newer)
         save_item_files.files[i].settled = false;
      else if (handle->reissued
            || !autosave_mark_reached(
               retro_atomic_load_acquire_int(&handle->written),
               handle->wrote_mark))
         save_item_files.files[i].settled = handle->wrote_settled;
      else
         save_item_files.files[i].settled = handle->settled
            && handle->landed;
      if (set)
         save_item_file_collect(set, i);
      /* Not copied yet: the final write copies it. */
      if (handle->copy_image && !handle->image_copied)
      {
         save_item_files.files[i].guard.copy_image = handle->copy_image;
         save_item_files.files[i].guard.copy_size  = handle->copy_size;
         handle->copy_image                        = NULL;
      }
      autosave_free(handle);
      save_item_files.files[i].autosave = NULL;
   }
   for (i = 0; i < autosave_state.num_retired; i++)
   {
      autosave_t *handle = autosave_state.retired[i];
      sthread_join(handle->thread);
      handle->thread = NULL;
      autosave_report_copy(handle);
      autosave_settle(handle);
      autosave_free(handle);
   }
   free(autosave_state.retired);
   autosave_state.retired     = NULL;
   autosave_state.num_retired = 0;
   autosave_state.retired_cap = 0;
}

/* Starts save RAM slot @i's worker; with @rewrite, its first snapshot
 * is written, changed or not. */
static void autosave_ram_start(unsigned i, bool rewrite)
{
   retro_ctx_memory_info_t mem_info;
   autosave_t *auto_st = NULL;

   mem_info.id         = task_save_files->elems[i].attr.i;

   core_get_memory(&mem_info);

   if (mem_info.size == 0)
      return;

   if (!(auto_st = autosave_new(task_save_files->elems[i].data,
         mem_info.data,
         mem_info.size,
         autosave_state.interval,
         autosave_state.compress)))
   {
      RARCH_WARN("[SRAM] %s\n", msg_hash_to_str(MSG_AUTOSAVE_FAILED));
      return;
   }

   /* The first snapshot's publish orders it before the worker reads
    * it. */
   auto_st->rewrite       = rewrite;
   autosave_state.list[i] = auto_st;
}

/* Save RAM slot @i's file is an item's, or one that left owes it a
 * write. */
static bool autosave_ram_held(unsigned i)
{
   return task_save_files && i < task_save_files->size
      && (   save_items_legacy_retired(task_save_files->elems[i].attr.i)
          || save_item_write_pending(task_save_files->elems[i].data));
}

bool autosave_init(bool compress_files, unsigned autosave_interval)
{
   unsigned i;
   autosave_t **list          = NULL;

   if (autosave_interval < 1 || !task_save_files)
      return false;

   if (!(list = (autosave_t**)
      calloc(task_save_files->size,
            sizeof(*autosave_state.list))))
      return false;
   if (!(autosave_state.waits = (bool*)calloc(task_save_files->size,
            sizeof(*autosave_state.waits))))
   {
      free(list);
      return false;
   }

   autosave_state.list     = list;
   autosave_state.num      = (unsigned)task_save_files->size;
   autosave_state.interval = autosave_interval;
   autosave_state.compress = compress_files;

   for (i = 0; i < task_save_files->size; i++)
   {
      unsigned type = task_save_files->elems[i].attr.i;
      if (save_items_legacy_retired(type))
         autosave_state.waits[i] = true;
      else
         autosave_ram_start(i, type <= RETRO_MEMORY_RTC
               && (save_item_files.given_back & (1 << type)));
   }

   for (i = 0; i < save_item_files.num_files; i++)
      save_item_files.files[i].start_failed = false;
   autosave_items_start();
   return true;
}

void autosave_deinit(void)
{
   unsigned i;

   for (i = 0; i < autosave_state.num; i++)
   {
      autosave_t *handle = autosave_state.list[i];
      if (handle)
         autosave_free(handle);
      autosave_state.list[i] = NULL;
   }

   free(autosave_state.list);
   free(autosave_state.waits);

   autosave_state.list     = NULL;
   autosave_state.waits    = NULL;
   autosave_state.num      = 0;
   autosave_state.interval = 0;
   autosave_items_free();
}

void autosave_lock(void)
{
   autosave_state.depth++;
}

void autosave_check(void)
{
   unsigned i;

   if (autosave_state.depth)
      return;

   autosave_reap();

   for (i = 0; i < autosave_state.num; i++)
   {
      autosave_t *handle = autosave_state.list[i];
      if (!handle)
         continue;
      /* The worker asked and is waiting: the snapshot is ours to fill
       * until the flag clears. Not while an item holds the file, or
       * one that left owes it a write. */
      if (     !retro_atomic_load_acquire_int(&handle->snapshot_requested)
            || autosave_ram_held(i))
         continue;
      memcpy(handle->snapshot, handle->retro_buffer, handle->bufsize);
      retro_atomic_store_release_int(&handle->snapshot_requested, 0);
      retro_eventcount_notify(&handle->ec);
   }

   for (i = 0; i < save_item_files.num_files; i++)
   {
      autosave_t *handle = save_item_files.files[i].autosave;
      if (     !handle
            || !retro_atomic_load_acquire_int(&handle->snapshot_requested)
            || !autosave_fill_item(handle))
         continue;
      retro_atomic_store_release_int(&handle->snapshot_requested, 0);
      retro_eventcount_notify(&handle->ec);
   }

   /* Save RAM an item gave back by leaving: its file holds that item's
    * last image, so the first snapshot is written. */
   for (i = 0; autosave_state.waits && i < autosave_state.num; i++)
      if (autosave_state.waits[i] && !autosave_ram_held(i))
      {
         autosave_state.waits[i] = false;
         autosave_ram_start(i, true);
      }
}

void autosave_unlock(void)
{
   if (autosave_state.depth && !--autosave_state.depth)
      autosave_check();
}

#endif

static bool content_get_memory(retro_ctx_memory_info_t *mem_info,
      struct ram_type *ram, unsigned slot)
{
   ram->type     = task_save_files->elems[slot].attr.i;
   ram->path     = task_save_files->elems[slot].data;
   mem_info->id  = ram->type;

   core_get_memory(mem_info);

   if (!mem_info->data || mem_info->size == 0)
      return false;

   return true;
}

/**
 * content_load_ram_file:
 * @slot             : index into task_save_files
 *
 * Load a RAM state from disk to memory.
 */
static bool content_load_ram_file(unsigned slot)
{
   int64_t rc;
   struct ram_type ram;
   retro_ctx_memory_info_t mem_info;
   void *buf        = NULL;
   bool success     = false;

   if (!content_get_memory(&mem_info, &ram, slot))
      return false;

   /* On first run of content, SRAM file will
    * not exist. This is a common enough occurrence
    * that we should check before attempting to
    * invoke the relevant read_file() function */
   if (    (!ram.path || !*ram.path)
       || !path_is_valid(ram.path))
      return false;

#if defined(HAVE_COMPRESSION)
   /* Always use RZIP interface when reading SRAM
    * files - this will automatically handle uncompressed
    * data */
   if (!rzipstream_read_file(ram.path, &buf, &rc))
#else
   if (!filestream_read_file(ram.path, &buf, &rc))
#endif
      return false;

   if (rc > 0)
   {
      if (rc > (ssize_t)mem_info.size)
      {
         RARCH_WARN("[SRAM] SRAM is larger than implementation expects, "
               "doing partial load (truncating %u %s %s %u).\n",
               (unsigned)rc,
               msg_hash_to_str(MSG_BYTES),
               msg_hash_to_str(MSG_TO),
               (unsigned)mem_info.size);
         save_ram_keep_copy(ram.path, (unsigned)ram.type, buf, (size_t)rc);
         rc = mem_info.size;
      }
      memcpy(mem_info.data, buf, (size_t)rc);
      success = true;
   }

   if (buf)
      free(buf);

   return success;
}

/**
 * dump_to_file_desperate:
 * @data         : pointer to data buffer.
 * @size         : size of @data.
 * @type         : type of file to be saved.
 *
 * Attempt to save valuable RAM data somewhere.
 **/
static bool dump_to_file_desperate(const void *data,
      size_t len, unsigned type, char *s, size_t s_len)
{
   size_t _len;
   char path[PATH_MAX_LENGTH + 256 + 32];
   path[0] = '\0';
   _len    = fill_pathname_application_data(path, sizeof(path))
         ? fill_pathname_slash(path, sizeof(path)) : 0;

   if (_len)
   {
      time_t time_;
      struct tm tm_;

      /* rtime_localtime leaves tm_ unwritten when localtime() fails */
      memset(&tm_, 0, sizeof(tm_));
      time(&time_);
      rtime_localtime(&time_, &tm_);
      _len += strlcpy_lit(path  + _len, "RetroArch-recovery-", sizeof(path) - _len);
      if (type != RETRO_SAVE_NO_LEGACY_MEMORY)
         _len += snprintf(path + _len, sizeof(path) - _len, "%u-", type);
      _len += strftime(path + _len, sizeof(path) - _len,
            "%Y-%m-%d-%H-%M-%S", &tm_);
      /* Never over an earlier recovery of the same second. */
      save_name_free(path, sizeof(path), _len, "", NULL, 0);

      /* Fallback (emergency) saves are always
       * uncompressed
       * > If a regular save fails, then the host
       *   system is experiencing serious technical
       *   difficulties (most likely some kind of
       *   hardware failure)
       * > In this case, we don't want to further
       *   complicate matters by introducing zlib
       *   compression overheads */
      if (filestream_write_file_atomic(path, data, len))
      {
         RARCH_WARN("[SRAM] Succeeded in saving RAM data to \"%s\".\n", path);
         if (s)
            strlcpy(s, path, s_len);
         return true;
      }
   }

   return false;
}

/**
 * content_save_ram_file:
 * @slot             : index into task_save_files
 * @compress         : whether to use rzip compression
 *
 * Save a RAM state from memory to disk.
 * Skips the write if the on-disk content already
 * matches memory (common when autosave has been active).
 */
static bool content_save_ram_file(unsigned slot, bool compress)
{
   struct ram_type ram;
   retro_ctx_memory_info_t mem_info;

   if (!content_get_memory(&mem_info, &ram, slot))
      return false;

   /* Quick check: if the file already exists and matches
    * current memory contents, skip the write entirely.
    * This is the common case when autosave has been running. */
   if (   ram.path && *ram.path
       &&  path_is_valid(ram.path))
   {
      /* Compared in place rather than slurped, in both lanes.  The
       * old path allocated a second copy of the whole save file
       * purely to memcmp it and free it, and checked the size only
       * after the read had already happened - so a save that had
       * changed size, the one case where the answer is knowable for
       * free, still paid a full-size allocation and a full-file read
       * (a full decompress, in the compressed lane).  Cores with
       * megabytes of save RAM paid that on every save, against a
       * memory budget that on the handheld targets is the scarce
       * resource.
       *
       * Size first, then compare without owning a copy, stopping at
       * the first differing byte - which is the case that goes on to
       * write. */
#if defined(HAVE_COMPRESSION)
      if (rzipstream_matches_buf(ram.path, mem_info.data,
               mem_info.size))
#else
      if (filestream_matches_buf(ram.path, mem_info.data,
               mem_info.size))
#endif
      {
         RARCH_LOG("[SRAM] %s \"%s\" (unchanged, skipping write).\n",
               msg_hash_to_str(MSG_SAVED_SUCCESSFULLY_TO),
               ram.path);
         return true;
      }
   }

   RARCH_LOG("[SRAM] %s #%u %s \"%s\".\n",
         msg_hash_to_str(MSG_SAVING_RAM_TYPE),
         ram.type,
         msg_hash_to_str(MSG_TO),
         ram.path);

   /* Write via a temporary file, so a crash or power loss mid-save
    * leaves the previous save intact instead of a truncated one. */
#if defined(HAVE_COMPRESSION)
   if (compress)
   {
      char tmp_path[PATH_MAX_LENGTH];
      if (     !filestream_atomic_temp_name(tmp_path, sizeof(tmp_path), ram.path)
            || !content_finish_tmp(ram.path, rzipstream_write_file(
                  tmp_path, mem_info.data, mem_info.size)))
         goto fail;
   }
   else
#endif
   {
      if (!filestream_write_file_atomic(
            ram.path, mem_info.data, mem_info.size))
         goto fail;
   }

   RARCH_LOG("[SRAM] %s \"%s\".\n",
         msg_hash_to_str(MSG_SAVED_SUCCESSFULLY_TO),
         ram.path);

   return true;

fail:
   RARCH_ERR("[SRAM] %s.\n",
         msg_hash_to_str(MSG_FAILED_TO_SAVE_SRAM));
   RARCH_WARN("[SRAM] Attempting to recover...\n");

   /* In case the file could not be written to,
    * the fallback function 'dump_to_file_desperate'
    * will be called. */
   if (!dump_to_file_desperate(
            mem_info.data, mem_info.size, ram.type, NULL, 0))
      RARCH_WARN("[SRAM] Failed. Cannot recover save file.\n");
   return false;
}

/* Keeps what an item holds now as the bytes it was taken up with. */
static void save_item_file_take_up(const save_items_set_t *set,
      unsigned i)
{
   save_item_file_t *f = &save_item_files.files[i];
   size_t len          = save_items_size(set, i);

   free(f->initial);
   f->initial      = NULL;
   f->initial_size = 0;
   f->taken_up     = true;
   if (len && (f->initial = malloc(len)))
   {
      if (save_items_copy_out(set, i, f->initial, len))
         f->initial_size = len;
      else
      {
         free(f->initial);
         f->initial = NULL;
      }
   }
}

/* Item @i's file was copied: the guard is met, and an EXACT_SIZE item
 * drops its tail. */
static void save_item_copy_done(const save_items_set_t *set, unsigned i)
{
   save_item_file_t *f = &save_item_files.files[i];
   save_item_guard_met(&f->guard);
   if (     (set->items[i].item.flags & RETRO_SAVE_FLAG_EXACT_SIZE)
         && set->items[i].tail_size)
   {
      save_items_drop_tail(i);
#ifdef HAVE_THREADS
      if (f->autosave)
         f->autosave->tail_kept = false;
#endif
   }
}

/* Reads a whole item file. rzip reads raw files too, so a compressed
 * .srm from before the core registered loads whatever the flags say. */
static bool save_item_read_file(const char *path, void **buf,
      int64_t *len)
{
#if defined(HAVE_COMPRESSION)
   return rzipstream_read_file(path, buf, len);
#else
   return filestream_read_file(path, buf, len) != 0;
#endif
}

/* A buffer item keeps what its file holds past it; an EXACT_SIZE one
 * only until the copy is made. */
static void save_item_file_keep_tail(const save_items_set_t *set,
      unsigned i, const char *path, const void *buf, size_t rc,
      size_t len)
{
   save_items_keep_tail(i, buf, rc);
   if (rc > len)
   {
      RARCH_LOG("[SRAM] \"%s\" is longer than the item: keeping the %u "
            "bytes past it.\n", path, (unsigned)(rc - len));
      /* No memory for them: the file is kept until it is copied. */
      if (!set->items[i].tail_size)
      {
         save_item_files.files[i].guard.copy_first = true;
         save_item_files.files[i].guard.keep_file  = true;
      }
      else if (set->items[i].item.flags & RETRO_SAVE_FLAG_EXACT_SIZE)
         save_item_files.files[i].guard.copy_first = true;
   }
   else if (rc < len)
      RARCH_LOG("[SRAM] \"%s\" is shorter than the item: loading its %u "
            "bytes.\n", path, (unsigned)rc);
}

/* An earlier name newer than the item's own file may hold a save
 * another version of the core made since. */
static void save_item_check_forks(const save_item_t *it)
{
   unsigned n;
   int64_t own, old;
   if (!it->legacy || !path_get_mtime(it->path, &own))
      return;
   for (n = 0; it->legacy[n]; n++)
      if (path_get_mtime(it->legacy[n], &old) && old > own)
      {
         save_item_warn(MSG_SAVE_ITEM_LEGACY_NEWER, it->legacy[n], NULL);
         return;
      }
}

/* Earlier name @n of item @i, read whole into @buf: to legacy_load()
 * when the core converts, else into the item as its own file would
 * be, a buffer's tail following it to the new name. False
 * when the core refused it. A callback may register again, so nothing
 * of @set is read after one. */
static bool content_load_item_legacy(const save_items_set_t *set,
      unsigned i, unsigned n, const void *buf, size_t rc)
{
   size_t len;
   const save_item_t *it = &set->items[i];
   const char *path      = it->legacy[n];

   if (save_items_can_legacy_load())
   {
      bool ok;
      char *old = strdup(path);
      char *own = strdup(it->path);
      if ((ok = set->iface.legacy_load(i, n, NULL, buf, rc)) && old && own)
         RARCH_LOG("[SRAM] Converted \"%s\" to \"%s\"; the old file is "
               "kept.\n", old, own);
      free(old);
      free(own);
      return ok;
   }
   if (it->item.storage == RETRO_SAVE_STORAGE_FRONTEND_BLOB)
      return save_items_copy_in(set, i, buf, rc);
   len = save_items_size(set, i);
   save_items_copy_in(set, i, buf, rc);
   save_item_file_keep_tail(set, i, path, buf, rc, len);
   if (rc != len)
      save_item_warn(MSG_SAVE_ITEM_LEGACY_SIZE, path, NULL);
   return true;
}

#ifdef HAVE_THREADS
/* @h's last image is the newest waiting for @path. final stays until
 * autosave_reap() frees it on this thread; the worker only reads it. A
 * newer image superseded it once superseded moves past final_mark. */
static bool autosave_final_waits(autosave_t *h, const char *path)
{
   return   h->final
         && h->final_size
         &&    retro_atomic_load_acquire_int(&h->superseded)
            == h->final_mark
         && string_is_equal_case_insensitive(h->path, path);
}
#endif

/* The newest image waiting for @path, left waiting until the fill has
 * accepted it (save_item_take_waiting()): an orphan's, lent (@lent),
 * as a registration adds orphans and frees none, else a copy of a
 * retired worker's last one. NULL when nothing waits. */
static void *save_item_find_waiting(const char *path, size_t *len,
      save_item_file_t *f, bool *lent)
{
   unsigned i;
   void *image         = NULL;
   const void *initial = f->initial;
   size_t initial_size = f->initial_size;

   for (i = save_item_files.num_orphans; !image && i-- > 0; )
   {
      const save_item_orphan_t *o = &save_item_files.orphans[i];
      if (!o->data || !string_is_equal_case_insensitive(o->path, path))
         continue;
      image        = o->data;
      *len         = o->size;
      *lent        = true;
      initial      = o->initial;
      initial_size = o->initial_size;
   }
#ifdef HAVE_THREADS
   for (i = 0; !image && i < autosave_state.num_retired; i++)
   {
      autosave_t *h = autosave_state.retired[i];
      if (     !autosave_final_waits(h, path)
            || !(image = malloc(h->final_size)))
         continue;
      memcpy(image, h->final, h->final_size);
      *len = h->final_size;
   }
#endif
   /* Its own type change copies the image, not the file, whether the
    * core takes it or not. Nothing of the old type was saved when there
    * is no file and the image is still what the item was taken up
    * with. */
   if (     f->guard.type_changed && image
         && (     path_is_valid(path)
               || !initial
               || initial_size != *len
               || memcmp(initial, image, *len)))
   {
      /* Without it an orphan's image is not taken: its write comes
       * first, and the copy is of the file it leaves. */
      if (!(f->guard.copy_image = malloc(*len)))
      {
         if (!*lent)
            return image;
         *lent = false;
         *len  = 0;
         return NULL;
      }
      memcpy(f->guard.copy_image, image, *len);
      f->guard.copy_size = *len;
   }
   return image;
}

/* The fill accepted the image save_item_find_waiting() found: an
 * orphan's write is dropped with every older one and record @f takes
 * over its debts; a retired worker still writes its own after the copy
 * it owes, and @f shares its recovery file. */
static void save_item_take_waiting(const char *path, save_item_file_t *f)
{
   unsigned i;
   bool taken = false;
   /* Its own type change copies the image it takes, not the file. */
   bool own   = f->guard.type_changed;

   for (i = save_item_files.num_orphans; i-- > 0; )
   {
      save_item_orphan_t *o = &save_item_files.orphans[i];
      if (!o->data || !string_is_equal_case_insensitive(o->path, path))
         continue;
      if (!taken)
      {
         taken           = true;
         free(f->initial);
         f->initial      = o->initial;
         f->initial_size = o->initial_size;
         o->initial      = NULL;
         if (o->guard.copy_first)
            f->guard.copy_first   = true;
         if (o->guard.keep_file)
            f->guard.keep_file    = true;
         if (o->guard.type_changed)
            f->guard.type_changed = true;
         /* The recovery file named for the kept file stays the one. */
         if (!f->guard.recovered)
         {
            f->guard.recovered = o->guard.recovered;
            o->guard.recovered = NULL;
         }
         if (!own)
         {
            f->guard.copy_image = o->guard.copy_image;
            f->guard.copy_size  = o->guard.copy_size;
            o->guard.copy_image = NULL;
         }
      }
      free(o->data);
      o->data = NULL;
      free(o->initial);
      o->initial = NULL;
      /* The type change's copy it owes stays owed. */
      if (o->guard.copy_image)
         continue;
      free(o->path);
      free(o->guard.recovered);
      save_item_files.num_orphans--;
      memmove(o, o + 1, (save_item_files.num_orphans - i) * sizeof(*o));
   }
#ifdef HAVE_THREADS
   for (i = 0; !taken && i < autosave_state.num_retired; i++)
      if (autosave_final_waits(autosave_state.retired[i], path))
      {
         f->took = autosave_state.retired[i];
         taken   = true;
      }
#endif
}

/* Blob item @i, of @id at @path, took its file: it owes no copy for
 * it but a type change's. When blob_load() registered again since
 * @seen, its record is the one carried: the same id at the same file. */
static void save_item_blob_taken(const char *id, const char *path,
      unsigned seen, unsigned i)
{
   save_item_file_t *f         = NULL;
   const save_items_set_t *set = save_items_state_get_ptr()->current;
   if (seen == save_item_files.registrations)
      f = &save_item_files.files[i];
   else
      for (i = 0; set && !f && i < save_item_files.num_files; i++)
         if (     string_is_equal(set->items[i].item.id, id)
               && string_is_equal(set->items[i].path, path))
            f = &save_item_files.files[i];
   if (f && !f->guard.type_changed)
   {
      f->guard.copy_first = false;
      f->guard.keep_file  = false;
   }
}

/* Item @i starts empty: the file its legacy memory was saved to
 * before, if it is there, is named in the log. */
static void save_item_check_old_file(const save_items_set_t *set,
      unsigned i)
{
   unsigned k, n;
   char who[256];
   const save_item_t *it = &set->items[i];
   for (k = 0; task_save_files && k < task_save_files->size; k++)
   {
      const char *old = task_save_files->elems[k].data;
      if (     (unsigned)task_save_files->elems[k].attr.i
               != it->item.legacy_memory
            || string_is_equal(old, it->path)
            || !path_is_valid(old))
         continue;
      for (n = 0; it->legacy && it->legacy[n]; n++)
         if (string_is_equal(it->legacy[n], old))
            return;
      save_items_who(who, sizeof(who), set, i);
      RARCH_WARN("[SRAM] %s: legacy_paths = %s: \"%s\", where the "
            "frontend saved this memory before, is not among them. See "
            "libretro.h, SET_SAVE_INTERFACE, \"Cross-core compatibility\". "
            "It was not loaded.\n", who, it->legacy ? "set" : "NULL", old);
   }
}

/* content_load_item_file() for an item with no file of its own. */
static bool content_load_item_legacy_names(const save_items_set_t *set,
      unsigned i)
{
   unsigned n;
   int64_t rc;
   unsigned seen;
   void *buf             = NULL;
   char *refused         = NULL;
   bool ok               = false;
   unsigned regs         = save_item_files.registrations;
   const save_item_t *it = &set->items[i];

   /* The baseline is what the item held before an earlier name filled
    * it, so the first write creates the item's own file. */
   save_item_file_take_up(set, i);
   for (n = 0; !ok && it->legacy && it->legacy[n]; n++)
   {
      char *name;
      if (     !path_is_valid(it->legacy[n])
            || !save_item_read_file(it->legacy[n], &buf, &rc)
            || rc <= 0)
      {
         free(buf);
         buf = NULL;
         continue;
      }
      name = strdup(it->legacy[n]);
      seen = save_item_files.registrations;
      ok   = content_load_item_legacy(set, i, n, buf, (size_t)rc);
      free(buf);
      buf  = NULL;
      if (!ok && !refused)
         refused = name;
      else
         free(name);
      /* Registered again: the record was carried as it stands. */
      if (seen != save_item_files.registrations)
         break;
   }
   if (!ok && refused)
      save_item_warn(MSG_SAVE_ITEM_LEGACY_REFUSED, refused, NULL);
   free(refused);
   if (!ok && regs == save_item_files.registrations)
      save_item_check_old_file(set, i);
   return ok;
}

/* Fills item @i from its own file, or else from the first earlier name
 * the core takes; the earlier names are never written again. */
static bool content_load_item_file(const save_items_set_t *set,
      unsigned i)
{
   int64_t rc;
   unsigned seen;
   size_t waiting        = 0;
   void *buf             = NULL;
   bool lent             = false;
   bool ok               = false;
   const save_item_t *it = &set->items[i];
   const char *path      = it->path;
   save_item_file_t *f   = &save_item_files.files[i];

   /* An image still waiting for the file is newer than the file. */
   if ((buf = save_item_find_waiting(path, &waiting, f, &lent)))
      rc = (int64_t)waiting;
   if (buf || path_is_valid(path))
   {
      bool got;
      save_item_check_forks(it);
      if ((got = buf || save_item_read_file(path, &buf, &rc)) && rc > 0)
      {
         size_t len = save_items_size(set, i);
         seen       = save_item_files.registrations;
         if (it->item.storage == RETRO_SAVE_STORAGE_FRONTEND_BLOB)
         {
            char *name          = strdup(path);
            char *id            = strdup(it->item.id);
            /* Kept as it is until it has been copied. */
            f->guard.copy_first = true;
            f->guard.keep_file  = true;
            if ((ok = save_items_copy_in(set, i, buf, (size_t)rc)))
            {
               if (name && id)
                  save_item_blob_taken(id, name, seen, i);
            }
            else if (name)
               save_item_warn(MSG_SAVE_ITEM_FILE_REFUSED, name, NULL);
            free(name);
            free(id);
         }
         else
         {
            ok = save_items_copy_in(set, i, buf, (size_t)rc);
            save_item_file_keep_tail(set, i, path, buf, (size_t)rc, len);
         }
         /* A refused image keeps its write; a registration from
          * blob_load() carries the record without the take. */
         if (waiting && ok && seen == save_item_files.registrations)
            save_item_take_waiting(path, f);
      }
      else
      {
         /* Unread: kept until it has been copied. */
         if (!got)
         {
            RARCH_ERR("[SRAM] Could not read \"%s\".\n", path);
            f->guard.copy_first = true;
            f->guard.keep_file  = true;
         }
         save_item_file_take_up(set, i);
      }
      if (!lent)
         free(buf);
      return ok;
   }

   return content_load_item_legacy_names(set, i);
}

/* Fills the bytes a re-registration added to item @i from its tail's
 * front. Between core calls. */
static void save_item_fill_grow(unsigned i)
{
   save_item_file_t *f = &save_item_files.files[i];
   if (save_items_grow_from_tail(i, f->grow_from))
      f->tell = true;
   f->grow_from = 0;
#ifdef HAVE_THREADS
   if (f->autosave)
      f->autosave->tail_kept = false;
#endif
}

/* A buffer replacing a legacy memory keeps its bytes in that memory
 * too, for netplay's save sync: a size that differs is logged once. */
static void save_item_check_memory_size(const save_items_set_t *set,
      unsigned i)
{
   retro_ctx_memory_info_t mem;
   size_t len;
   char who[256];
   const save_item_t *it = &set->items[i];
   save_item_file_t *f   = &save_item_files.files[i];
   if (     f->size_told
         || it->item.storage != RETRO_SAVE_STORAGE_FRONTEND_BUFFER
         || it->item.legacy_memory == RETRO_SAVE_NO_LEGACY_MEMORY)
      return;
   len      = save_items_size(set, i) - it->tail_size;
   mem.id   = it->item.legacy_memory;
   mem.data = NULL;
   mem.size = 0;
   core_get_memory(&mem);
   if (mem.size == len)
      return;
   f->size_told = true;
   save_items_who(who, sizeof(who), set, i);
   RARCH_WARN("[SRAM] %s: legacy_memory = %u: that memory is %lu bytes "
         "and the item %lu, and an item that replaces a memory keeps its "
         "bytes there too, for netplay's save sync. See libretro.h, "
         "SET_SAVE_INTERFACE, \"Layout changes\".\n", who,
         it->item.legacy_memory, (unsigned long)mem.size,
         (unsigned long)len);
}

/* Before an item's first fill, the legacy memories the core reports
 * now may leave it out, as at the call. */
static void content_check_items_reported(void)
{
   unsigned i, t;
   bool live[2]                = { false, false };
   bool asked                  = false;
   bool left                   = false;
   const save_items_set_t *set = save_items_state_get_ptr()->current;
   const char *name;
   for (i = 0; set && i < save_item_files.num_files; i++)
   {
      if (save_item_files.files[i].checked)
         continue;
      save_item_files.files[i].checked = true;
      if (!set->items[i].legacy_unreported)
         continue;
      for (t = 0; !asked && t < 2; t++)
      {
         retro_ctx_memory_info_t mem;
         mem.id   = t;
         mem.data = NULL;
         mem.size = 0;
         live[t]  = core_get_memory(&mem) && mem.data && mem.size;
      }
      asked = true;
      if (save_items_check_reported(i, live))
         left = true;
   }
   while (left && (name = save_items_take_left_out()))
      runloop_save_items_notice(name);
}

/* Fills each frontend-owned item not filled yet from its file, then
 * starts its autosave: the image read is the one compared against. */
static bool content_fill_items(bool is_sram_load_disabled)
{
   unsigned i;
   unsigned seen;
   bool ret = false;
   const save_items_set_t *set;

   save_item_files.fill_pending = false;
   content_check_items_reported();
   /* legacy_load(), blob_load() and loaded() may register again: the
    * set is read anew for each item, and a record carried across (same
    * id, same file) is not filled twice. */
   for (i = 0; (set = save_items_state_get_ptr()->current)
         && i < save_item_files.num_files; i++)
   {
      save_item_file_t *f = &save_item_files.files[i];
      if (f->grow_from)
      {
         save_item_fill_grow(i);
         continue;
      }
      if (f->taken_up || !save_items_frontend_owned(&set->items[i]))
         continue;
      f->tell     = true;
      f->taken_up = true;
      save_item_check_memory_size(set, i);
      if (is_sram_load_disabled)
         save_item_file_take_up(set, i);
      else
      {
         if (set->items[i].item.storage == RETRO_SAVE_STORAGE_FRONTEND_BLOB)
            save_item_files.filling = f;
         if (content_load_item_file(set, i))
            ret = true;
         save_item_files.filling = NULL;
      }
   }
   /* Each is told, filled or not, before the core runs again. A core
    * may register again from loaded(), so the set is read anew, and
    * the pass starts over: an item not told yet may have moved to an
    * index already passed. */
   do
   {
      seen = save_item_files.registrations;
      for (i = 0; seen == save_item_files.registrations
            && i < save_item_files.num_files; i++)
      {
         /* A grow a callback registered is told once it is filled. */
         if (     !save_item_files.files[i].tell
               || save_item_files.files[i].grow_from)
            continue;
         save_item_files.files[i].tell = false;
         set = save_items_state_get_ptr()->current;
         if (set && i < set->num_items && set->iface.loaded)
            set->iface.loaded(i);
      }
   } while (seen != save_item_files.registrations);
#ifdef HAVE_THREADS
   autosave_items_start();
#endif
   return ret;
}

/* What legacy_load() or loaded() registered during a fill is filled
 * and told once that callback has returned, before the core runs: a
 * few passes here, the rest at the next frame start. */
static bool content_fill_items_all(bool is_sram_load_disabled)
{
   unsigned n;
   bool ret = content_fill_items(is_sram_load_disabled);
   for (n = 0; n < 3 && save_item_files.fill_pending; n++)
      if (content_fill_items(is_sram_load_disabled))
         ret = true;
   return ret;
}

/* Once per load, after retro_load_game() has returned. */
static bool content_load_items(bool is_sram_load_disabled)
{
   if (save_item_files.loaded)
      return false;
   save_item_files.loaded = true;
   return content_fill_items_all(is_sram_load_disabled);
}

/* After a frame in which the core's retro_run() ran. */
void event_save_items_ran(double fps)
{
#ifdef HAVE_THREADS
   autosave_state.fps = fps;
   autosave_state.frames++;
#endif
}

void event_save_items_fill(bool is_sram_load_disabled)
{
   unsigned i;
   const save_items_set_t *set = save_items_state_get_ptr()->current;
   for (i = 0; set && i < save_item_files.num_files; i++)
   {
      save_item_files.files[i].kept_told = false;
#ifdef HAVE_THREADS
      if (save_item_files.files[i].autosave)
         save_item_file_collect(set, i);
      else
#endif
         save_item_files.files[i].settled = false;
   }
   if (save_item_files.fill_pending)
      content_fill_items_all(is_sram_load_disabled);
#ifdef HAVE_THREADS
   /* Again for an item empty when filled, or changed since its start
    * failed. */
   else
      autosave_items_start();
#endif
}

void event_save_items_set_undo_dir(const char *dir)
{
   unsigned i;
   free(save_undo_dir);
   save_undo_dir     = string_is_empty(dir) ? NULL : strdup(dir);
   save_ram_cut_told = false;
   for (i = 0; i <= RETRO_MEMORY_RTC; i++)
   {
      free(save_ram_cut_dumps[i]);
      save_ram_cut_dumps[i] = NULL;
      save_ram_cut_bare[i]  = false;
   }
}

/* A refused file is never written over without its copy: the image
 * goes to the recovery file, and both are named on screen. With
 * @recovered they are named once: a later image replaces the one in
 * that file, and the same image is not written again. */
static void save_item_recover(const char *path, const void *data,
      size_t len, unsigned type, char **recovered)
{
   char *s;
   if (recovered && *recovered)
   {
      if (filestream_matches_buf(*recovered, data, len))
         return;
      if (filestream_write_file_atomic(*recovered, data, len))
      {
         RARCH_LOG("[SRAM] %s \"%s\".\n",
               msg_hash_to_str(MSG_SAVED_SUCCESSFULLY_TO), *recovered);
         return;
      }
   }
   s = (char*)malloc(PATH_MAX_LENGTH + 256 + 32);
   if (s && dump_to_file_desperate(data, len, type, s,
            PATH_MAX_LENGTH + 256 + 32))
   {
      if (!recovered || !*recovered)
         save_item_warn(MSG_SAVE_ITEM_FILE_RECOVERED, path, s);
      if (recovered)
      {
         free(*recovered);
         *recovered = strdup(s);
      }
   }
   else
      RARCH_WARN("[SRAM] Failed. Cannot recover save file.\n");
   free(s);
}

/* The write itself, for content_save_item_image(). */
static bool content_save_item_write(const char *path, const void *data,
      size_t len, const void *initial, size_t initial_size,
      bool transform, unsigned type, char **recovered)
{
#ifdef HAVE_THREADS
   autosave_supersede(path, data, len);
#endif
   if (path_is_valid(path))
   {
#if defined(HAVE_COMPRESSION)
      if (transform
            ? rzipstream_matches_buf(path, data, len)
            : filestream_matches_buf(path, data, len))
#else
      if (filestream_matches_buf(path, data, len))
#endif
         return true;
   }
   else if (initial && initial_size == len && !memcmp(initial, data, len))
      return true;

   if (save_item_write_file(path, SAVE_ITEM_TMP, data, len, transform))
   {
      RARCH_LOG("[SRAM] %s \"%s\".\n",
            msg_hash_to_str(MSG_SAVED_SUCCESSFULLY_TO), path);
      return true;
   }
   RARCH_ERR("[SRAM] %s.\n", msg_hash_to_str(MSG_FAILED_TO_SAVE_SRAM));
   save_item_recover(path, data, len, type, recovered);
   return false;
}

/* Writes an item's image unless its file already holds it, or it has
 * no file and still holds what it was taken up with. Raw unless
 * @transform. @guard, when given, takes the copy it owes first; when
 * that fails a refused file is left as it is and its image goes to the
 * recovery file, and a longer one keeps its end this time. A failed
 * write sends the image to the recovery file *@recovered names. */
static bool content_save_item_image(const char *path, const void *data,
      size_t len, const void *initial, size_t initial_size,
      bool transform, unsigned type, save_item_guard_t *guard,
      char **recovered)
{
#ifdef HAVE_THREADS
   /* A retired writer's owed copy comes before this newer image. */
   if (guard && !autosave_retired_copy(path))
   {
      save_item_recover(path, data, len, type, &guard->recovered);
      return false;
   }
#endif
   if (guard && guard->copy_first)
   {
      unsigned n;
      enum save_item_copy copy = guard->copy_image
         ? save_item_copy_image(path, save_undo_dir, SAVE_ITEM_TMP,
               guard->copy_image, guard->copy_size, &n, false)
         : save_item_copy_file(path, save_undo_dir, SAVE_ITEM_TMP, &n,
               false);
      if (copy != SAVE_ITEM_COPY_FAILED)
      {
         if (copy == SAVE_ITEM_COPY_MADE)
         {
            save_item_log_copy(path, save_undo_dir, n, guard->drop_size);
            if (guard->type_changed)
               save_item_warn_type_changed(path, save_undo_dir, n);
         }
         len -= guard->drop_size;
         save_item_guard_met(guard);
      }
      else if (guard->keep_file)
      {
         save_item_recover(path, data, len, type, &guard->recovered);
         return false;
      }
      else
         RARCH_WARN("[SRAM] No copy of \"%s\" could be kept; its last %u "
               "bytes stay.\n", path, (unsigned)guard->drop_size);
   }

   return content_save_item_write(path, data, len, initial, initial_size,
         transform, type, recovered);
}

/* @f's file, or its worker's next write, holds a snapshot taken while
 * can_snapshot() said true, and owes nothing before a newer image. */
static bool save_item_file_settled(const save_item_file_t *f)
{
#ifdef HAVE_THREADS
   if (f->autosave)
      return    f->autosave->settled
             && !retro_atomic_load_acquire_int(&f->autosave->failed);
#endif
   return f->settled && !f->guard.untold && !f->guard.copy_first;
}

static bool content_save_item_file(const save_items_set_t *set,
      unsigned i, bool compress)
{
   size_t len;
   void *data;
   bool ret                  = false;
   bool stuck                = false;
   bool untold               = false;
   bool ready                = false;
   const save_item_t *it     = &set->items[i];
   save_item_file_t *f       = &save_item_files.files[i];
   save_item_guard_t *guard  = &f->guard;
   bool blob                 =
      it->item.storage == RETRO_SAVE_STORAGE_FRONTEND_BLOB;

   if (f->grow_from)
      save_item_fill_grow(i);
#ifdef HAVE_THREADS
   /* An item's worker takes its copies: one copy per drop. */
   if (f->autosave)
   {
      save_item_file_collect(set, i);
      if (guard->copy_first && guard->keep_file)
      {
         if (!retro_atomic_load_acquire_int(&f->autosave->failed))
            untold = true;
         else
         {
            /* Its copy failed: it writes nothing. */
            save_item_untold_tell(it->path, &guard->untold,
                  &guard->recovered);
            stuck = true;
         }
      }
      guard = NULL;
   }
#endif
   /* The game is still saving it: the file, or its worker's next
    * write, holds the last snapshot taken while it said true. */
   if (     !untold && !stuck
         && !(ready = save_items_can_snapshot(set, i))
         && save_item_file_settled(f))
   {
      if (!f->kept_told)
         RARCH_LOG("[SRAM] Kept the last finished save of \"%s\": the "
               "game was still saving it.\n", it->path);
      f->kept_told = true;
      return true;
   }
   len  = blob ? save_items_blob_size(set, i) : save_items_size(set, i);
   /* A blob over its max_size is never written. */
   data = len && !(blob && it->item.max_size && len > it->item.max_size)
      ? malloc(len) : NULL;
   if (!data)
   {
      if (!len)
         return true;
      save_item_blob_unsaved(set, i, len, true);
      save_item_write_failed(&f->guard, it->path);
      return false;
   }
   /* A short blob_save() leaves zeros, not heap. */
   if (blob)
      memset(data, 0, len);
   if (guard)
      guard->drop_size = (it->item.flags & RETRO_SAVE_FLAG_EXACT_SIZE)
         ? it->tail_size : 0;
   if (stuck)
   {
      if (save_items_copy_out(set, i, data, len))
         save_item_recover(it->path, data, len, it->item.legacy_memory,
               &f->guard.recovered);
   }
#ifdef HAVE_THREADS
   else if (untold)
   {
      if (save_items_copy_out(set, i, data, len))
      {
         ret = save_item_untold_write(&f->guard.untold, data, len,
               it->item.legacy_memory);
         if (!ret)
            save_item_write_failed(&f->guard, it->path);
         /* The next write from a snapshot taken from now on holds it. */
         autosave_supersede(it->path, NULL, 0);
         f->guard.untold_mark = retro_atomic_load_acquire_int(
               &f->autosave->superseded);
      }
   }
#endif
   else if (save_items_copy_out(set, i, data, len))
   {
      bool copying = guard && guard->copy_first;
#ifdef HAVE_THREADS
      save_item_check_generation(set, i, data, len);
#endif
      ret = content_save_item_image(it->path, data, len,
            f->initial, f->initial_size,
               compress
            && (it->item.flags & RETRO_SAVE_FLAG_FRONTEND_MAY_TRANSFORM),
            it->item.legacy_memory, guard, &f->guard.recovered);
      if (copying && !guard->copy_first)
         save_item_copy_done(set, i);
      if (guard)
      {
         if (ret || f->guard.recovered)
            save_item_untold_drop(&f->guard.untold);
         else
            save_item_untold_tell(it->path, &f->guard.untold,
                  &f->guard.recovered);
      }
#ifdef HAVE_THREADS
      else
      {
         f->autosave->wrote_mark    = retro_atomic_load_acquire_int(
               &f->autosave->superseded);
         f->autosave->wrote_settled = ready && ret;
      }
#endif
   }
   else
   {
      save_item_blob_unsaved(set, i, len, false);
      RARCH_ERR("[SRAM] %s \"%s\".\n",
            msg_hash_to_str(MSG_FAILED_TO_SAVE_SRAM), it->path);
   }
   /* The file holds a settled image only if this one is. */
   if (guard && ret)
      f->settled = ready;
   if (ret)
      f->guard.save_fails = 0;
   free(data);
   return ret;
}

/* Writes the orphans, then every frontend-owned item that has been
 * filled: one still waiting for its fill would overwrite its file. */
static bool content_save_items(bool compress)
{
   unsigned i;
   bool ret                    = true;
   const save_items_set_t *set = save_items_state_get_ptr()->current;

   for (i = 0; i < save_item_files.num_orphans; i++)
   {
      unsigned n;
      save_item_orphan_t *o = &save_item_files.orphans[i];
      if (o->data)
      {
         if (!content_save_item_image(o->path, o->data, o->size,
                  o->initial, o->initial_size, compress && o->transform,
                  o->type, &o->guard, &o->guard.recovered))
            ret = false;
      }
      else if (save_item_copy_image(o->path, save_undo_dir, SAVE_ITEM_TMP,
               o->guard.copy_image, o->guard.copy_size, &n, false)
            != SAVE_ITEM_COPY_FAILED)
      {
         save_item_log_copy(o->path, save_undo_dir, n, 0);
         if (o->guard.type_changed)
            save_item_warn_type_changed(o->path, save_undo_dir, n);
         free(o->guard.copy_image);
         o->guard.copy_image = NULL;
      }
      else
         ret = false;
      /* Its last write: a type change's copy never made keeps its
       * image in a second recovery file. */
      if (o->guard.copy_image)
         save_item_recover(o->path, o->guard.copy_image,
               o->guard.copy_size, o->type, NULL);
      free(o->path);
      free(o->data);
      free(o->initial);
      free(o->guard.recovered);
      free(o->guard.copy_image);
   }
   free(save_item_files.orphans);
   save_item_files.orphans     = NULL;
   save_item_files.num_orphans = 0;

   if (!set || !save_item_files.loaded)
      return ret;
   for (i = 0; i < save_item_files.num_files; i++)
      if (     save_item_files.files[i].taken_up
            && save_items_frontend_owned(&set->items[i])
            && !content_save_item_file(set, i, compress))
         ret = false;
   return ret;
}

bool event_save_files(bool is_sram_used, bool compress_files,
      const char *path_cheat_database)
{
   unsigned i;
   bool ret = true;
#ifdef HAVE_CHEATS
   cheat_manager_save_game_specific_cheats(path_cheat_database);
#endif
   if (!task_save_files || !is_sram_used)
      return false;
   if (!content_save_items(compress_files))
      ret = false;
   /* Held while an item that left still owes the file its last image:
    * written at a later save once that has landed. */
   for (i = 0; i < task_save_files->size; i++)
      if (     !save_items_legacy_retired(task_save_files->elems[i].attr.i)
            && !save_item_write_pending(task_save_files->elems[i].data)
            && !content_save_ram_file(i, compress_files))
         ret = false;
   return ret;
}

bool event_load_save_files(bool is_sram_load_disabled)
{
   unsigned i;
   bool ret = content_load_items(is_sram_load_disabled);

   if (!task_save_files || is_sram_load_disabled)
      return ret;

   /* Report a successful load operation if
    * any type of RAM file is found and
    * processed correctly */
   for (i = 0; i < task_save_files->size; i++)
      if (!save_items_legacy_retired(task_save_files->elems[i].attr.i))
         ret |= content_load_ram_file(i);

   return ret;
}

/* An item that left the set still owes @path its last image: an
 * orphan, or a retired worker not freed yet. */
static bool save_item_write_pending(const char *path)
{
   unsigned i;
   for (i = 0; i < save_item_files.num_orphans; i++)
      if (string_is_equal_noncase(save_item_files.orphans[i].path, path))
         return true;
#ifdef HAVE_THREADS
   for (i = 0; i < autosave_state.num_retired; i++)
      if (string_is_equal_noncase(autosave_state.retired[i]->path, path))
         return true;
#endif
   return false;
}

/**
 * content_savefile_is_live:
 * @path             : absolute path to a file on disk
 *
 * Answers whether a loaded core owns @path. Ownership runs from the
 * point the save file list is built for the content up to the point
 * the deinit chain has written save RAM back out and torn the list
 * down again, and it is the window in which core memory rather than
 * the file on disk holds the authoritative copy.
 *
 * Anything that would replace or remove such a file needs to ask:
 * whatever it puts on disk is overwritten from core memory when the
 * content closes.
 *
 * A registered save item's file, or a file inside a core folder it
 * names that its patterns select, is owned while the item is
 * registered, and after it leaves until its last image is written.
 *
 * Main thread only: it reads save item state without a lock.
 *
 * The comparison ignores case on every platform. A false positive
 * costs nothing beyond leaving a file alone for one more round, while
 * a false negative is the data loss this exists to prevent.
 *
 * Returns: true if a loaded core owns @path.
 **/
bool content_savefile_is_live(const char *path)
{
   size_t i;

   if (string_is_empty(path))
      return false;

   if (task_save_files)
      for (i = 0; i < task_save_files->size; i++)
         if (string_is_equal_noncase(task_save_files->elems[i].data, path))
            return true;

   return save_items_path_is_live(path) || save_item_write_pending(path);
}

void path_init_savefile_rtc(const char *savefile_path)
{
   union string_list_elem_attr attr;
   char savefile_name_rtc[PATH_MAX_LENGTH];

   attr.i = RETRO_MEMORY_SAVE_RAM;
   string_list_append(task_save_files, savefile_path, attr);

   /* Infer .rtc save path from save RAM path. */
   attr.i = RETRO_MEMORY_RTC;
   fill_pathname(savefile_name_rtc,
         savefile_path, ".rtc",
         sizeof(savefile_name_rtc));
   string_list_append(task_save_files, savefile_name_rtc, attr);
}

void path_deinit_savefile(void)
{
   if (task_save_files)
      string_list_free(task_save_files);
   task_save_files = NULL;
}

void path_init_savefile_new(void)
{
   task_save_files = string_list_new();
}

void *savefile_ptr_get(void)
{
   return task_save_files;
}

static bool save_item_same_file(const save_item_t *a,
      const save_item_t *b)
{
   return   save_items_frontend_owned(a)
         && save_items_frontend_owned(b)
         && a->item.storage == b->item.storage
         && string_is_equal(a->path, b->path)
         &&    (a->item.flags & RETRO_SAVE_FLAG_FRONTEND_MAY_TRANSFORM)
            == (b->item.flags & RETRO_SAVE_FLAG_FRONTEND_MAY_TRANSFORM);
}

/* No memory to keep item file @f of @it that left the set: its images
 * go to recovery files now. Frees @image. */
static void save_item_orphan_lost(const save_item_t *it, void *image,
      size_t len, save_item_file_t *f)
{
   if (image)
      save_item_recover(it->path, image, len, it->item.legacy_memory,
            &f->guard.recovered);
   if (f->guard.copy_image)
      save_item_recover(it->path, f->guard.copy_image,
            f->guard.copy_size, it->item.legacy_memory, NULL);
   free(image);
}

/* Takes @image, NULL for a copy owed only, and the record's initial
 * bytes. */
static void save_item_orphan_add(const save_item_t *it, void *image,
      size_t len, size_t drop, save_item_file_t *f)
{
   save_item_orphan_t *o;
   save_item_orphan_t *list = (save_item_orphan_t*)realloc(
         save_item_files.orphans,
         (save_item_files.num_orphans + 1) * sizeof(*list));

   if (!list)
   {
      save_item_orphan_lost(it, image, len, f);
      return;
   }
   save_item_files.orphans = list;
   o                       = &list[save_item_files.num_orphans];
   if (!(o->path = strdup(it->path)))
   {
      save_item_orphan_lost(it, image, len, f);
      return;
   }
   o->data         = image;
   o->size         = len;
   o->initial      = f->initial;
   o->initial_size = f->initial_size;
   o->type         = it->item.legacy_memory;
   o->transform    =
      (it->item.flags & RETRO_SAVE_FLAG_FRONTEND_MAY_TRANSFORM) != 0;
   o->guard        = f->guard;
   o->guard.drop_size = drop;
   f->initial      = NULL;
   f->guard.recovered = NULL;
   f->guard.copy_image = NULL;
   save_item_files.num_orphans++;
}

/* Item @i of @old left the set or its file changed. Its last image is
 * written later, by its worker when it has one, even when a new item
 * names that file. Nothing is written for an item not filled yet, a
 * blob whose own fill runs, or a file a core-owned item of @set now
 * names.
 * A buffer of @set at the same file that took its tail without its
 * record (save_items_register()) gives that tail back to the image. */
static void save_item_file_close(const save_items_set_t *old,
      unsigned i, const save_items_set_t *set)
{
   unsigned k;
   void *image                = NULL;
   const unsigned char *taken = NULL;
   size_t len                 = 0;
   size_t took                = 0;
   size_t drop                = 0;
   const save_item_t *it      = &old->items[i];
   save_item_file_t *f        = &save_item_files.files[i];
   bool keep                  = f->taken_up
      && save_items_frontend_owned(it);

   if (f == save_item_files.filling)
   {
      keep                    = false;
      save_item_files.filling = NULL;
   }
   for (k = 0; keep && set && k < set->num_items; k++)
      if (     string_is_equal(set->items[k].path, it->path)
            && !save_items_frontend_owned(&set->items[k]))
         keep = false;
   if (keep && set && it->next != SAVE_ITEMS_NONE && !it->tail_size)
   {
      const save_item_t *n = &set->items[it->next];
      if (     n->item.storage == RETRO_SAVE_STORAGE_FRONTEND_BUFFER
            && string_is_equal(n->path, it->path))
      {
         /* Behind the bytes a cut put in front of it. */
         size_t had = save_items_size(old, i);
         size_t now = save_items_size(set, it->next) - n->tail_size;
         size_t cut = had > now ? had - now : 0;
         if (n->tail_size > cut)
         {
            taken = (const unsigned char*)n->tail + cut;
            took  = n->tail_size - cut;
         }
      }
   }
   /* Its segments are readable, and a blob is saved, only during this
    * SET_SAVE_INTERFACE. */
   if (     keep
         && (len = save_items_size(old, i) + took)
         && (image = malloc(len))
         && !save_items_copy_out(old, i, image, len - took))
   {
      free(image);
      image = NULL;
   }
   if (image)
   {
      size_t own = len - took - it->tail_size;
      if (took)
         memcpy((unsigned char*)image + own, taken, took);
      /* A grow not filled yet: the file as it was, with the current
       * head. */
      if (f->grow_from && f->grow_from < own)
      {
         memmove((unsigned char*)image + f->grow_from,
               (unsigned char*)image + own, len - own);
         len = f->grow_from + len - own;
      }
      if ((it->item.flags & RETRO_SAVE_FLAG_EXACT_SIZE) && len > own)
         drop = len - own;
   }
#ifdef HAVE_THREADS
   /* Its worker writes the newest image last. */
   if (image && f->autosave)
   {
      f->autosave->untold = f->guard.untold;
      f->guard.untold     = NULL;
   }
#endif
   save_item_untold_tell(it->path, &f->guard.untold, &f->guard.recovered);
#ifdef HAVE_THREADS
   if (     it->item.legacy_memory <= RETRO_MEMORY_RTC
         && !save_items_legacy_retired(it->item.legacy_memory))
      save_item_files.given_back |= 1 << it->item.legacy_memory;
   if (f->autosave)
   {
      autosave_retire(f->autosave, image, len, drop,
            it->item.legacy_memory, f->guard.type_changed);
      f->autosave = NULL;
      image       = NULL;
   }
#endif
   if (image || f->guard.copy_image)
      save_item_orphan_add(it, image, len, drop, f);
   free(f->initial);
   f->initial = NULL;
   free(f->guard.recovered);
   f->guard.recovered = NULL;
   free(f->guard.copy_image);
   f->guard.copy_image = NULL;
}

/* Item @j of @old kept its file as item @i of @set. A cut put the cut
 * bytes in front of the tail (save_items_register()), less any a grow
 * not filled yet added; a grow is filled from the tail at the frame
 * start, never inside this call; an EXACT_SIZE item that was cut owes
 * a copy. */
static void save_item_file_reshape(const save_items_set_t *old,
      unsigned j, const save_items_set_t *set, unsigned i,
      save_item_file_t *f)
{
   size_t had, own;
   bool lost;
   const save_item_t *it = &set->items[i];

   if (     it->item.storage != RETRO_SAVE_STORAGE_FRONTEND_BUFFER
         || !f->taken_up)
      return;
   had = save_items_size(old, j) - old->items[j].tail_size;
   own = save_items_size(set, i) - it->tail_size;
   /* With no memory for the cut bytes the tail stayed with the replaced
    * item (save_items_register()): the file is kept until copied. */
   lost = had > own && it->tail_size < had - own;
   /* The bytes a pending grow added were never read from the file. */
   if (f->grow_from && had > own)
   {
      size_t from = own > f->grow_from ? own : f->grow_from;
      save_items_cut_tail(i, from - own, had - from);
      if (own <= f->grow_from)
         f->grow_from = 0;
   }
   /* A grow already pending keeps where it began. */
   if (own > had && it->tail_size && !f->grow_from)
   {
      f->grow_from                 = had;
      save_item_files.fill_pending = true;
   }
   if (lost || (had > own && (it->item.flags & RETRO_SAVE_FLAG_EXACT_SIZE)))
   {
      f->guard.copy_first = true;
      if (lost)
         f->guard.keep_file = true;
#ifdef HAVE_THREADS
      if (f->autosave)
      {
         if (lost)
            retro_atomic_store_release_int(&f->autosave->keep_file, 1);
         retro_atomic_inc_int(&f->autosave->copy_first);
      }
#endif
   }
#ifdef HAVE_THREADS
   if (f->autosave)
      f->autosave->tail_kept = false;
#endif
}

void event_save_items_registered(void)
{
   unsigned i;
   save_items_state_t *st      = save_items_state_get_ptr();
   const save_items_set_t *set = st->current;
   const save_items_set_t *old = st->previous;
   save_item_file_t *files     = NULL;

   save_item_files.registrations++;
   if (     set
         && set->num_items
         && !(files = (save_item_file_t*)calloc(set->num_items,
               sizeof(*files))))
   {
      const char *msg = msg_hash_to_str(MSG_FAILED_TO_SAVE_SRAM);
      RARCH_ERR("[SRAM] No memory for the save items' files.\n");
      runloop_msg_queue_push(msg, strlen(msg), 1, 180, false, NULL,
            MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);
   }

   for (i = 0; old && i < save_item_files.num_files; i++)
   {
      unsigned next = old->items[i].next;
      if (     files
            && next != SAVE_ITEMS_NONE
            && save_item_same_file(&old->items[i], &set->items[next]))
      {
         files[next]              = save_item_files.files[i];
         files[next].start_failed = false;
         if (save_item_files.filling == &save_item_files.files[i])
            save_item_files.filling = &files[next];
         save_item_file_reshape(old, i, set, next, &files[next]);
#ifdef HAVE_THREADS
         if (files[next].autosave)
            files[next].autosave->item = next;
#endif
      }
      else
         save_item_file_close(old, i, set);
   }
   free(save_item_files.files);
   save_item_files.files     = files;
   save_item_files.num_files = files ? set->num_items : 0;

   /* A new or moved id at a file another replaced item kept another
    * way: the file is copied before the first write and never written
    * over without it. Its record is new, so nothing else owes a copy
    * yet. */
   for (i = 0; i < save_item_files.num_files; i++)
      if (set->items[i].type_changed)
      {
         save_item_files.files[i].guard.copy_first   = true;
         save_item_files.files[i].guard.keep_file    = true;
         save_item_files.files[i].guard.type_changed = true;
      }

   /* An item added after the load, or moved to another file, is filled
    * before the core runs again, never inside this call. */
   for (i = 0; save_item_files.loaded && i < save_item_files.num_files; i++)
      if (     (   !save_item_files.files[i].taken_up
                && save_items_frontend_owned(&set->items[i]))
            || (   !save_item_files.files[i].checked
                && set->items[i].legacy_unreported))
         save_item_files.fill_pending = true;
}

/* After retro_unload_game() and before the set is cleared, so what the
 * core writes while unloading reaches the files. The workers are
 * stopped first: closing waits for them, as it does for save RAM. */
void event_save_items_final(bool is_sram_used, bool compress_files)
{
   unsigned i;
   const save_items_set_t *set = save_items_state_get_ptr()->current;
#ifdef HAVE_THREADS
   autosave_items_free();
#endif
   if (is_sram_used)
   {
      content_save_items(compress_files);
      /* The last write: a type change's copy never made keeps its
       * image in a second recovery file. */
      for (i = 0; set && i < save_item_files.num_files; i++)
         if (save_item_files.files[i].guard.copy_image)
            save_item_recover(set->items[i].path,
                  save_item_files.files[i].guard.copy_image,
                  save_item_files.files[i].guard.copy_size,
                  set->items[i].item.legacy_memory, NULL);
   }
   event_save_items_deinit();
}

void event_save_items_deinit(void)
{
   unsigned i;
#ifdef HAVE_THREADS
   autosave_items_free();
#endif
   for (i = 0; i < save_item_files.num_files; i++)
   {
      free(save_item_files.files[i].initial);
      free(save_item_files.files[i].guard.recovered);
      free(save_item_files.files[i].guard.untold);
      free(save_item_files.files[i].guard.copy_image);
   }
   free(save_item_files.files);
   for (i = 0; i < save_item_files.num_orphans; i++)
   {
      free(save_item_files.orphans[i].path);
      free(save_item_files.orphans[i].data);
      free(save_item_files.orphans[i].initial);
      free(save_item_files.orphans[i].guard.recovered);
      free(save_item_files.orphans[i].guard.copy_image);
   }
   free(save_item_files.orphans);
   memset(&save_item_files, 0, sizeof(save_item_files));
}

