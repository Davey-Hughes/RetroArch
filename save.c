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

#include <lists/string_list.h>
#include <streams/interface_stream.h>
#include <streams/file_stream.h>
#include <streams/rzip_stream.h>
#include <rthreads/rthreads.h>
#ifdef HAVE_THREADS
#include <encodings/crc32.h>
#include <features/features_cpu.h>
#include <rthreads/retro_eventcount.h>
#endif
#include <retro_atomic.h>
#include <file/file_path.h>
#include <string/stdstring.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "content.h"
#include "core.h"
#include "core_info.h"
#include "msg_hash.h"
#include "runloop.h"
#include "save_backups.h"
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
/* An unsynced write may have left bytes the disk does not hold yet: a
 * synced request writes them again even when they match. */
static bool save_files_unsynced = false;

/* Marks @slot's memory as having had a size this session: only then
 * does a drop to 0 bytes empty its save at close. */
static void content_mark_sized(unsigned slot,
      const retro_ctx_memory_info_t *mem_info)
{
   if (     mem_info->data && mem_info->size
         && task_save_files && slot < task_save_files->size
         && !task_save_files->elems[slot].userdata)
      task_save_files->elems[slot].userdata = malloc(1);
}

enum save_file_flags
{
   SAVE_FILE_LOADED_NONBLANK    = (1 << 0),
   SAVE_FILE_BLANKED_SEEN       = (1 << 1),
   SAVE_FILE_BACKUP_FAILED      = (1 << 2),
   SAVE_FILE_BACKUP_NOTICED     = (1 << 3),
   SAVE_FILE_UNREADABLE_NOTICED = (1 << 4),
   SAVE_FILE_CUT_SEEN           = (1 << 5),
   SAVE_FILE_WRITE_FAILED       = (1 << 6),
   SAVE_FILE_WRITE_NOTICED      = (1 << 7),
   SAVE_FILE_RECOVERY_NOTICED   = (1 << 8),
   SAVE_FILE_FLUSH_RECOVERED    = (1 << 9),
   SAVE_FILE_FLUSH_LOST         = (1 << 10),
   SAVE_FILE_CHANGED_ELSEWHERE  = (1 << 11),
   SAVE_FILE_CHANGED_NOTICED    = (1 << 12)
};

/* One per entry of task_save_files, for the session. Workers read and
 * raise these; the main thread makes them, frees them and turns the
 * raised flags into notices. */
struct save_file
{
   retro_atomic_size_t loaded; /* bytes read at load */
   retro_atomic_int_t owed;    /* 1 << enum save_backup_reason */
   retro_atomic_int_t flags;   /* enum save_file_flags */
   retro_atomic_int_t stat;    /* save_file_stat(), 0 until kept */
};

static struct save_file *save_files = NULL;
static size_t save_files_num        = 0;

/* Main thread: made at the first use after the list is built, kept
 * until path_deinit_savefile(). */
static struct save_file *save_file_get(unsigned slot)
{
   if (!save_files && task_save_files && task_save_files->size)
   {
      size_t i;
      if (!(save_files = (struct save_file*)malloc(
            task_save_files->size * sizeof(*save_files))))
         return NULL;
      save_files_num = task_save_files->size;
      for (i = 0; i < save_files_num; i++)
      {
         retro_atomic_size_init(&save_files[i].loaded, 0);
         retro_atomic_int_init(&save_files[i].owed, 0);
         retro_atomic_int_init(&save_files[i].flags, 0);
         retro_atomic_int_init(&save_files[i].stat, 0);
      }
   }
   return slot < save_files_num ? &save_files[slot] : NULL;
}

static void save_file_owe(struct save_file *f,
      enum save_backup_reason reason)
{
   if (f)
      retro_atomic_fetch_or_int(&f->owed, 1 << reason);
}

/* @bits: 1 << enum save_backup_reason each */
static void save_file_owe_bits(struct save_file *f, int bits)
{
   if (f && bits)
      retro_atomic_fetch_or_int(&f->owed, bits);
}

/* @path's size and mtime in one word, so either thread reads or keeps
 * it at once: never 0, and 1 when @path is missing. An unknown mtime
 * counts as 0. */
static int save_file_stat(const char *path)
{
   int64_t mtime = 0;
   int64_t size  = path_get_size(path);
   uint32_t key;
   if (size < 0)
      return 1;
   if (!path_get_mtime(path, &mtime))
      mtime = 0;
   key = ((uint32_t)size * 2654435761u ^ (uint32_t)mtime
         ^ (uint32_t)((uint64_t)mtime >> 32) * 40503u) & 0x7FFFFFFFu;
   return key > 1 ? (int)key : 2;
}

/* At load and after each own write: read back, not taken from the
 * clock, so coarse mtimes (FAT: 2 s) never look like a change. */
static void save_file_keep_stat(struct save_file *f, const char *path)
{
   if (f)
      retro_atomic_store_release_int(&f->stat, save_file_stat(path));
}

/* Another program wrote @path since it was loaded or last written
 * here. Not when it is gone: nothing to keep. */
static bool save_file_differs(struct save_file *f, const char *path)
{
   int stat, now;
   return   f
         && (stat = retro_atomic_load_acquire_int(&f->stat))
         && (now = save_file_stat(path)) != stat
         && now != 1;
}

/* As save_file_differs(), and then it owes a copy. */
static bool save_file_changed(struct save_file *f, const char *path)
{
   if (!save_file_differs(f, path))
      return false;
   save_file_owe(f, SAVE_BACKUP_CHANGED_ELSEWHERE);
   return true;
}

/* All 0x00 or all 0xFF */
static bool save_image_blank(const void *data, size_t len)
{
   size_t i;
   const unsigned char *p = (const unsigned char*)data;
   if (!len || (p[0] != 0x00 && p[0] != 0xFF))
      return false;
   for (i = 1; i < len; i++)
      if (p[i] != p[0])
         return false;
   return true;
}

/* Makes the copies @f owes before @path is written over with @data.
 * False when one is owed and not made: the file must stay as it is.
 * Without @copy (a write inside a frame) nothing is copied. */
static bool save_file_pay(struct save_file *f, const char *path,
      const void *data, size_t len, bool copy, bool sync)
{
   unsigned r;
   int owed;
   if (!f)
      return true;
   if (     save_image_blank(data, len)
         && (retro_atomic_load_acquire_int(&f->flags)
            & SAVE_FILE_LOADED_NONBLANK)
         && !(retro_atomic_fetch_or_int(&f->flags, SAVE_FILE_BLANKED_SEEN)
            & SAVE_FILE_BLANKED_SEEN))
      save_file_owe(f, SAVE_BACKUP_BLANKED);
   /* Shorter than the save as loaded: an old core's partial load, a
    * shrink or an erase. */
   if (     len < (size_t)retro_atomic_load_acquire_size(&f->loaded)
         && !(retro_atomic_fetch_or_int(&f->flags, SAVE_FILE_CUT_SEEN)
            & SAVE_FILE_CUT_SEEN))
      save_file_owe(f, SAVE_BACKUP_CUT);
   if (!(owed = retro_atomic_load_acquire_int(&f->owed)))
      return true;
   if (!copy)
      return false;
   for (r = 0; r < SAVE_BACKUP_REASON_LAST; r++)
   {
      enum save_backup_result res;
      if (!(owed & (1 << r)))
         continue;
      res = save_backups_copy_file(path, (enum save_backup_reason)r, sync);
      if (res == SAVE_BACKUP_FAILED)
      {
         retro_atomic_fetch_or_int(&f->flags, SAVE_FILE_BACKUP_FAILED);
         return false;
      }
      /* Once per reason a load, a state load or an outside change,
       * so a worker may log it. */
      if (res == SAVE_BACKUP_MADE)
         RARCH_LOG("[SRAM] Backed up \"%s\" (%s) before writing it.\n",
               path, save_backups_reason_name((enum save_backup_reason)r));
      retro_atomic_fetch_and_int(&f->owed, ~(1 << r));
   }
   return true;
}

/* On screen with the file's name; with @log, also in the log with
 * its path (never from autosave_check()). */
static void save_file_notice(enum msg_hash_enums msg, const char *path,
      bool log)
{
   char s[256];
   int _len = snprintf(s, sizeof(s), msg_hash_to_str(msg),
         path_basename_nocompression(path));
   if (_len < 0)
      return;
   if ((size_t)_len >= sizeof(s))
      _len = (int)sizeof(s) - 1;
   runloop_msg_queue_push(s, (size_t)_len, 1, 180, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_WARNING);
   if (log)
   {
      size_t len = PATH_MAX_LENGTH + sizeof(s);
      char *full = (char*)malloc(len);
      if (full)
      {
         snprintf(full, len, msg_hash_to_str(msg), path);
         RARCH_WARN("[SRAM] %s\n", full);
         free(full);
      }
   }
}

#ifdef HAVE_THREADS
/* The worker's own temporary: a SAVE_FILES on the main thread uses
 * "<path>.tmp" and may run while the worker writes. */
#define AUTOSAVE_TMP_SUFFIX ".auto.tmp"

/* Set in a slot of autosave.flush while its flush waits or is being
 * written; the bits below KEPT are owed once it is written. */
#define AUTOSAVE_FLUSH_FULL (1 << 30)
/* The worker is writing the slot: its image is no longer replaced. */
#define AUTOSAVE_FLUSH_OUT  (1 << 29)
/* Memory that stays as it is: a newer flush may replace the image
 * before the worker takes it, and a failed write is left to the next
 * check, not recovered. */
#define AUTOSAVE_FLUSH_KEPT (1 << 28)
/* The main thread made the image's copies before writing the file
 * itself: the image is dropped unless already being written, and the
 * worker owes nothing for it. */
#define AUTOSAVE_FLUSH_PAID (1 << 27)
#define AUTOSAVE_FLUSH_BITS \
   (AUTOSAVE_FLUSH_FULL | AUTOSAVE_FLUSH_OUT | AUTOSAVE_FLUSH_KEPT \
    | AUTOSAVE_FLUSH_PAID)

/* A save that changes at every check is written at most this often. */
#ifndef AUTOSAVE_BUSY_USEC
#define AUTOSAVE_BUSY_USEC 60000000
#endif

typedef struct autosave autosave_t;

/* Autosave support. */
struct autosave_st
{
   autosave_t **list;
   unsigned num;
   unsigned depth;
};

struct autosave
{
   void *buffer;
   void *snapshot;
   /* Copies the main thread made for flushes, written in turn before
    * any later image; flush_owed[i] is owed once flush_buf[i] is
    * written. Slot i is the main thread's while flush[i] is 0. */
   void *flush_buf[2];
   int flush_owed[2];
   /* main_gen when flush_buf[i] and the snapshot were filled */
   int flush_gen[2];
   int snapshot_gen;
   /* Worker only: main_gen of the last image written here */
   int put_gen;
   char *path;
   char *tmp_path;
   /* Its file's session state; outlives the handle */
   struct save_file *file;
   /* Retired handles whose workers may not have ended yet */
   autosave_t *prev;
   sthread_t *thread;
   size_t bufsize;
   /* Main thread only: retired, when to try a replacement again */
   retro_time_t retry_time;
   /* Worker only: when it last wrote the file */
   retro_time_t last_write;
   /* What the worker sleeps on: the copy it asked for, the end of its
    * interval, or quit. */
   retro_eventcount_t ec;
   unsigned interval;
   unsigned type;
   unsigned flush_fill; /* Main thread only: the slot filled next */
   unsigned flush_take; /* Worker only: the slot written next */
   /* Only the main thread reads live SRAM. The worker requests a copy
    * and waits until autosave_check() has made it and cleared this. */
   retro_atomic_int_t snapshot_requested;
   /* AUTOSAVE_FLUSH_FULL, plus what flushes that came while both were
    * set owe after it */
   retro_atomic_int_t flush[2];
   /* Counts the main thread's own writes of the file: an image taken
    * before the last one is older than the file. */
   retro_atomic_int_t main_gen;
   /* Set by the worker from its last check of main_gen until its
    * write is done. */
   retro_atomic_int_t committing;
   retro_atomic_int_t quit;
   /* The worker's last store: the handle is the main thread's after it */
   retro_atomic_int_t exited;
   bool compress;
   bool ec_inited;
   /* buffer is not on disk: written at the next copy whatever it
    * holds. Set before the worker starts, then the worker's; the main
    * thread reads it only while the worker waits for a copy or has
    * ended. */
   bool unwritten;
};

static struct autosave_st autosave_state;

static bool autosave_quitting(autosave_t *save)
{
   return retro_atomic_load_acquire_int(&save->quit) != 0;
}

static bool autosave_flush_waiting(autosave_t *save)
{
   return ((retro_atomic_load_acquire_int(&save->flush[0])
         | retro_atomic_load_acquire_int(&save->flush[1]))
         & AUTOSAVE_FLUSH_FULL) != 0;
}

/* Writes @image through the worker's own temporary, or in place where
 * the file can't be renamed (filestream_atomic_in_place()). False when
 * opening, writing, syncing or renaming failed: the file keeps its old
 * contents, unless written in place. */
static bool autosave_write(autosave_t *save, const void *image)
{
   int ret;
   intfstream_t *file = NULL;
   bool written       = false;
   bool in_place      = filestream_atomic_in_place(save->path);
   const char *out    = in_place ? save->path : save->tmp_path;

   if (save->compress)
      file = intfstream_open_rzip_file(out, RETRO_VFS_FILE_ACCESS_WRITE);
   else
      file = intfstream_open_file(out,
            RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_SEQUENTIAL_BULK);

   if (file)
   {
      written = intfstream_write(file, image,
            save->bufsize) == (int64_t)save->bufsize;
      if (intfstream_flush(file) != 0)
         written = false;
      if (intfstream_close(file) != 0)
         written = false;
      free(file);
   }

   if (in_place)
      return written;
   ret = filestream_finish_atomic_suffix(save->path,
         AUTOSAVE_TMP_SUFFIX, written, true);
   if (ret == 1)
      filestream_delete(save->tmp_path);
   return ret == 0;
}

/* A flush's image that could not be written: no later check has it. The
 * main thread shows the flag; a worker neither logs nor notifies. */
static void autosave_recover(autosave_t *save, const void *image)
{
   char *rec = NULL;
   bool made = false;
   if (     save->bufsize
         && (rec = (char*)malloc(PATH_MAX_LENGTH)))
      made = save_backups_recover(rec, PATH_MAX_LENGTH, save->path, image,
            save->bufsize, true) != 0;
   free(rec);
   if (save->file)
      retro_atomic_fetch_or_int(&save->file->flags,
            made ? SAVE_FILE_FLUSH_RECOVERED : SAVE_FILE_FLUSH_LOST);
}

/* Writes @image, taken at main_gen @gen, unless it is the last one
 * written, making the copies the file owes first. On failure the last
 * written image stays as it was and unwritten stays set, so the next
 * check writes; a @flush image the main thread has not backed up goes
 * to a recovery file. Any other image older than the main thread's
 * last write of the file is dropped, and so is a @flush image the main
 * thread backed up for that write. Memory erased to 0 bytes empties a
 * save, but makes none. */
static void autosave_put(autosave_t *save, const void *image, int gen,
      bool flush)
{
   bool changed;
   if (     save->unwritten && !save->bufsize
         && !path_is_valid(save->path))
   {
      save->unwritten = false;
      return;
   }
   /* The main thread wrote the file since: buffer is not what it holds. */
   if (retro_atomic_load_acquire_int(&save->main_gen) != save->put_gen)
      save->unwritten = true;
   if (!save->unwritten && !memcmp(save->buffer, image, save->bufsize))
      return;
   save->unwritten = true;
   changed         = save_file_differs(save->file, save->path);
   /* After the stat: the main thread's own write may be the change. */
   if (retro_atomic_load_acquire_int(&save->main_gen) != gen)
   {
      if (!flush)
         return;
      changed = false;
   }
   if (changed)
      save_file_owe(save->file, SAVE_BACKUP_CHANGED_ELSEWHERE);
   if (save_file_pay(save->file, save->path, image, save->bufsize,
            true, true))
   {
      bool dropped;
      bool written = false;
      /* Told once its copy is made */
      if (changed)
         retro_atomic_fetch_or_int(&save->file->flags,
               SAVE_FILE_CHANGED_ELSEWHERE);
      /* Against autosave_main_write(): it sees this write under way,
       * or this sees its count. A flush's slot is flush_take's. */
      retro_atomic_fetch_add_seq_cst_int(&save->committing, 1);
      dropped = flush
         ? (retro_atomic_load_acquire_int(&save->flush[save->flush_take])
            & AUTOSAVE_FLUSH_PAID) != 0
         : retro_atomic_load_seq_cst_int(&save->main_gen) != gen;
      if (!dropped)
         written = autosave_write(save, image);
      retro_atomic_store_release_int(&save->committing, 0);
      if (dropped)
         return;
      if (written)
      {
         memcpy(save->buffer, image, save->bufsize);
         save->unwritten  = false;
         save->put_gen    = gen;
         save->last_write = cpu_features_get_time_usec();
         save_file_keep_stat(save->file, save->path);
         return;
      }
      if (!flush && save->file)
         retro_atomic_fetch_or_int(&save->file->flags,
               SAVE_FILE_WRITE_FAILED);
   }
   if (flush && !(retro_atomic_load_acquire_int(
               &save->flush[save->flush_take]) & AUTOSAVE_FLUSH_PAID))
      autosave_recover(save, image);
}

/* Writes the next waiting flush, then owes what came with it. False
 * when none was waiting. */
static bool autosave_take_flush(autosave_t *save)
{
   int w, late;
   unsigned slot = save->flush_take;
   do
   {
      w = retro_atomic_load_acquire_int(&save->flush[slot]);
      if (!(w & AUTOSAVE_FLUSH_FULL))
         return false;
   } while (!retro_atomic_cas_int(&save->flush[slot], w,
            w | AUTOSAVE_FLUSH_OUT));
   /* Backed up already: the main thread writes the file itself. */
   if (!(w & AUTOSAVE_FLUSH_PAID))
      autosave_put(save, save->flush_buf[slot], save->flush_gen[slot],
            !(w & AUTOSAVE_FLUSH_KEPT));
   /* Owed before the slot empties: a main write that finds it empty
    * makes them from the file. */
   if (!(retro_atomic_load_acquire_int(&save->flush[slot])
         & AUTOSAVE_FLUSH_PAID))
      save_file_owe_bits(save->file, save->flush_owed[slot]);
   late = retro_atomic_exchange_int(&save->flush[slot], 0);
   save_file_owe_bits(save->file, late & ~AUTOSAVE_FLUSH_BITS);
   save->flush_take = !slot;
   return true;
}

/* Something for the worker: its copy, a flush, or quit. */
static bool autosave_woken(autosave_t *save)
{
   return   autosave_quitting(save)
         || !retro_atomic_load_acquire_int(&save->snapshot_requested)
         || autosave_flush_waiting(save);
}

static void autosave_thread(void *data)
{
   autosave_t *save  = (autosave_t*)data;
   /* Cadence: the previous check's copy, as a CRC */
   uint32_t prev_crc = encoding_crc32(0,
         (const uint8_t*)save->buffer, save->bufsize);
   bool prev_changed = false;
   retro_time_t busy = (retro_time_t)save->interval * 1000000;
   if (busy < AUTOSAVE_BUSY_USEC)
      busy = AUTOSAVE_BUSY_USEC;

   while (!autosave_quitting(save))
   {
      /* Ask for a copy and wait until the main thread has made it. */
      retro_atomic_store_release_int(&save->snapshot_requested, 1);
      for (;;)
      {
         int key;
         if (autosave_woken(save))
            break;
         key = retro_eventcount_prepare_wait(&save->ec);
         if (autosave_woken(save))
         {
            retro_eventcount_cancel_wait(&save->ec);
            break;
         }
         retro_eventcount_commit_wait(&save->ec, key);
      }

      /* A flush goes first. Any copy already made is older or no newer
       * than what the next one holds: ask again. */
      if (autosave_take_flush(save))
         continue;
      if (autosave_quitting(save))
         break;
      /* Woken by a flush the main thread took back to replace. */
      if (retro_atomic_load_acquire_int(&save->snapshot_requested))
         continue;

      /* The snapshot remains ours until the next request. Written at
       * the first check after a quiet one, once it settles, or once
       * per busy period while it keeps changing; one not yet on disk
       * (a failed or dropped write) at once. */
      {
         uint32_t crc = encoding_crc32(0,
               (const uint8_t*)save->snapshot, save->bufsize);
         bool changed = crc != prev_crc;
         if (     save->unwritten || !changed || !prev_changed
               || cpu_features_get_time_usec() - save->last_write >= busy)
            autosave_put(save, save->snapshot, save->snapshot_gen, false);
         prev_crc     = crc;
         prev_changed = changed;
      }
      /* The main thread wrote since the copy: a newer one, not a rest */
      if (retro_atomic_load_acquire_int(&save->main_gen) != save->snapshot_gen)
         continue;

      /* Rest an interval; a flush or quit ends it early. */
      if (save->interval)
      {
         int key = retro_eventcount_prepare_wait(&save->ec);
         if (autosave_quitting(save) || autosave_flush_waiting(save))
            retro_eventcount_cancel_wait(&save->ec);
         else
            retro_eventcount_commit_wait_timeout(&save->ec, key,
#if defined(_MSC_VER) && _MSC_VER <= 1200
                  save->interval * 1000000
#else
                  save->interval * 1000000LL
#endif
                  );
         autosave_take_flush(save);
      }
   }

   /* Handed over before the quit: no later check has these. */
   while (autosave_take_flush(save))
      ;

   retro_atomic_store_release_int(&save->exited, 1);
}

/**
 * autosave_new:
 * @path            : path to autosave file
 * @data            : pointer to buffer
 * @len             : size of @data buffer
 * @interval        : interval at which saves should be performed.
 * @type            : the RETRO_MEMORY_* id @data was read from
 * @unwritten       : @data is not on disk yet
 * @file            : its file's session state, or NULL
 *
 * Create and initialize autosave object. Logs nothing: it is also
 * called from autosave_check().
 *
 * @return Pointer to new autosave_t object if successful, otherwise
 * NULL.
 **/
static autosave_t *autosave_new(const char *path,
      const void *data, size_t len,
      unsigned interval, bool compress, unsigned type, bool unwritten,
      struct save_file *file)
{
   size_t      path_len;
   void       *buf               = NULL;
   autosave_t *handle            = (autosave_t*)malloc(sizeof(*handle));
   if (!handle)
      return NULL;

   handle->compress              = compress;
   handle->ec_inited             = false;
   retro_atomic_int_init(&handle->quit, 0);
   retro_atomic_int_init(&handle->exited, 0);
   retro_atomic_int_init(&handle->snapshot_requested, 0);
   retro_atomic_int_init(&handle->flush[0], 0);
   retro_atomic_int_init(&handle->flush[1], 0);
   retro_atomic_int_init(&handle->main_gen, 0);
   retro_atomic_int_init(&handle->committing, 0);
   handle->flush_owed[0]         = 0;
   handle->flush_owed[1]         = 0;
   handle->flush_gen[0]          = 0;
   handle->flush_gen[1]          = 0;
   handle->snapshot_gen          = 0;
   handle->put_gen               = 0;
   handle->last_write            = cpu_features_get_time_usec();
   handle->flush_fill            = 0;
   handle->flush_take            = 0;
   handle->bufsize               = len;
   handle->interval              = interval;
   handle->type                  = type;
   handle->unwritten             = unwritten;
   handle->file                  = file;
   handle->retry_time            = 0;
   handle->buffer                = NULL;
   handle->snapshot              = NULL;
   handle->flush_buf[0]          = NULL;
   handle->flush_buf[1]          = NULL;
   handle->prev                  = NULL;
   handle->thread                = NULL;

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

   path_len                      = strlen(path);
   if (!(handle->tmp_path = (char*)malloc(path_len
               + sizeof(AUTOSAVE_TMP_SUFFIX))))
   {
      free(handle->path);
      free(handle);
      return NULL;
   }
   memcpy(handle->tmp_path, path, path_len);
   memcpy(handle->tmp_path + path_len, AUTOSAVE_TMP_SUFFIX,
         sizeof(AUTOSAVE_TMP_SUFFIX));

   /* At least a byte: malloc(0) may return NULL. */
   if (!(buf = malloc(len ? len : 1)))
   {
      free(handle->tmp_path);
      free(handle->path);
      free(handle);
      return NULL;
   }

   handle->buffer                = buf;

   memcpy(handle->buffer, data, handle->bufsize);

   handle->snapshot              = malloc(len ? len : 1);
   handle->flush_buf[0]          = malloc(len ? len : 1);
   handle->flush_buf[1]          = malloc(len ? len : 1);
   if (handle->snapshot && handle->flush_buf[0] && handle->flush_buf[1])
      handle->ec_inited          = retro_eventcount_init(&handle->ec);

   if (     !handle->snapshot || !handle->flush_buf[0]
         || !handle->flush_buf[1] || !handle->ec_inited)
   {
      free(handle->snapshot);
      free(handle->flush_buf[0]);
      free(handle->flush_buf[1]);
      if (handle->ec_inited)
         retro_eventcount_free(&handle->ec);
      free(handle->tmp_path);
      free(handle->path);
      free(handle->buffer);
      free(handle);
      return NULL;
   }

   handle->thread                = sthread_create(autosave_thread, handle);

   if (!handle->thread)
   {
      free(handle->snapshot);
      free(handle->flush_buf[0]);
      free(handle->flush_buf[1]);
      retro_eventcount_free(&handle->ec);
      free(handle->tmp_path);
      free(handle->path);
      free(handle->buffer);
      free(handle);
      return NULL;
   }

   return handle;
}

/* The notices for what the worker raised: autosave's once per session,
 * a flush's each time, and with @log in the log too. */
static void save_file_check_notices(struct save_file *f, const char *path,
      bool log)
{
   int flags;
   const int raised = SAVE_FILE_BACKUP_FAILED | SAVE_FILE_WRITE_FAILED
      | SAVE_FILE_FLUSH_RECOVERED | SAVE_FILE_FLUSH_LOST
      | SAVE_FILE_CHANGED_ELSEWHERE;
   if (     !f
         || !(retro_atomic_load_acquire_int(&f->flags) & raised))
      return;
   flags = retro_atomic_fetch_and_int(&f->flags, ~raised);
   if (     (flags & SAVE_FILE_BACKUP_FAILED)
         && !(flags & SAVE_FILE_BACKUP_NOTICED))
   {
      retro_atomic_fetch_or_int(&f->flags, SAVE_FILE_BACKUP_NOTICED);
      save_file_notice(MSG_SAVE_BACKUP_FAILED, path, false);
   }
   if (     (flags & SAVE_FILE_WRITE_FAILED)
         && !(flags & SAVE_FILE_WRITE_NOTICED))
   {
      retro_atomic_fetch_or_int(&f->flags, SAVE_FILE_WRITE_NOTICED);
      save_file_notice(MSG_SAVE_WRITE_FAILED, path, false);
   }
   if (     (flags & SAVE_FILE_CHANGED_ELSEWHERE)
         && !(flags & SAVE_FILE_CHANGED_NOTICED))
   {
      retro_atomic_fetch_or_int(&f->flags, SAVE_FILE_CHANGED_NOTICED);
      save_file_notice(MSG_SAVE_CHANGED_ELSEWHERE, path, false);
   }
   if (flags & SAVE_FILE_FLUSH_RECOVERED)
      save_file_notice(MSG_SAVE_WRITE_RECOVERED, path, log);
   if (flags & SAVE_FILE_FLUSH_LOST)
      save_file_notice(MSG_SAVE_WRITE_FAILED, path, log);
}

/**
 * autosave_free:
 * @handle          : pointer to autosave object
 * @log             : its last flush's failure is logged as well as shown;
 *                    not from autosave_check()
 *
 * Frees autosave object and all associated resources.
 **/
static void autosave_free(autosave_t *handle, bool log)
{
   retro_atomic_store_release_int(&handle->quit, 1);
   retro_eventcount_notify(&handle->ec);
   sthread_join(handle->thread);

   /* No check runs after the worker's last flush. */
   save_file_check_notices(handle->file, handle->path, log);

   free(handle->snapshot);
   free(handle->flush_buf[0]);
   free(handle->flush_buf[1]);
   retro_eventcount_free(&handle->ec);

   if (handle->buffer)
      free(handle->buffer);
   handle->buffer = NULL;

   if (handle->path)
      free(handle->path);
   handle->path = NULL;
   free(handle->tmp_path);

   free(handle);
}

/* A handle and the retired ones it replaced, at the end of the session:
 * its notices may never be seen, so they are logged. */
static void autosave_free_chain(autosave_t *handle)
{
   while (handle)
   {
      autosave_t *prev = handle->prev;
      autosave_free(handle, true);
      handle = prev;
   }
}

/* Frees the retired handles behind @handle whose workers have ended.
 * A join after the worker's last store waits for no more than its
 * return. */
static void autosave_reap(autosave_t *handle)
{
   autosave_t **link = &handle->prev;
   while (*link)
   {
      autosave_t *old = *link;
      if (retro_atomic_load_acquire_int(&old->exited))
      {
         *link     = old->prev;
         old->prev = NULL;
         autosave_free(old, false);
      }
      else
         link = &old->prev;
   }
}

/* A new handle for slot @i over memory of a new size or place; the
 * retired one stays reachable from it until its worker has ended. */
static void autosave_replace(unsigned i,
      const retro_ctx_memory_info_t *mem_info)
{
   autosave_t *old    = autosave_state.list[i];
   /* The old worker waits for a copy or has ended: what it had not
    * written goes to the new one.  A new size is a change too: some
    * cores report only the extent in use. */
   bool    unwritten  = old->unwritten
      || old->bufsize != mem_info->size
      || memcmp(old->buffer, mem_info->data, old->bufsize) != 0;
   autosave_t *handle = autosave_new(old->path, mem_info->data,
         mem_info->size, old->interval, old->compress, old->type,
         unwritten, old->file);
   if (handle)
   {
      handle->prev           = old;
      autosave_state.list[i] = handle;
   }
   else
      old->retry_time = cpu_features_get_time_usec()
         + (retro_time_t)old->interval * 1000000;
}

bool autosave_init(bool compress_files, unsigned autosave_interval)
{
   unsigned i;
   autosave_t **list          = NULL;

   if (     autosave_interval < 1
         || !task_save_files
         || runloop_state_get_ptr()->save_load_hold)
      return false;

   if (!(list = (autosave_t**)
      calloc(task_save_files->size,
            sizeof(*autosave_state.list))))
      return false;

   autosave_state.list = list;
   autosave_state.num  = (unsigned)task_save_files->size;

   for (i = 0; i < task_save_files->size; i++)
   {
      retro_ctx_memory_info_t mem_info;
      autosave_t *auto_st = NULL;
      const char *path    = task_save_files->elems[i].data;
      unsigned    type    = task_save_files->elems[i].attr.i;

      mem_info.id         = type;

      core_get_memory(&mem_info);
      content_mark_sized(i, &mem_info);

      if (mem_info.size == 0)
         continue;

      if (!(auto_st = autosave_new(path,
            mem_info.data,
            mem_info.size,
            autosave_interval,
            compress_files,
            type,
            false,
            save_file_get(i))))
      {
         RARCH_WARN("[SRAM] %s\n", msg_hash_to_str(MSG_AUTOSAVE_FAILED));
         continue;
      }

      autosave_state.list[i] = auto_st;
   }

   return true;
}

void autosave_deinit(void)
{
   unsigned i;

   for (i = 0; i < autosave_state.num; i++)
   {
      autosave_t *handle = autosave_state.list[i];
      if (handle)
         autosave_free_chain(handle);
      autosave_state.list[i] = NULL;
   }

   free(autosave_state.list);

   autosave_state.list     = NULL;
   autosave_state.num      = 0;
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

   for (i = 0; i < autosave_state.num; i++)
   {
      retro_ctx_memory_info_t mem_info;
      autosave_t *handle = autosave_state.list[i];
      if (!handle)
         continue;
      save_file_check_notices(handle->file, handle->path, false);
      if (handle->prev)
         autosave_reap(handle);
      mem_info.id        = handle->type;
      /* Retired: replaced once the core reports its memory again. */
      if (autosave_quitting(handle))
      {
         if (     handle->retry_time
               && cpu_features_get_time_usec() < handle->retry_time)
            continue;
         core_get_memory(&mem_info);
         content_mark_sized(i, &mem_info);
         if (mem_info.data)
            autosave_replace(i, &mem_info);
         continue;
      }
      /* The worker asked and is waiting: the snapshot is ours to fill
       * until the flag clears. */
      if (!retro_atomic_load_acquire_int(&handle->snapshot_requested))
         continue;
      /* A core may move or resize its memory at any time. */
      core_get_memory(&mem_info);
      content_mark_sized(i, &mem_info);
      if (!mem_info.data || mem_info.size != handle->bufsize)
      {
         /* Retired only while its worker is idle. */
         if (autosave_flush_waiting(handle))
            continue;
         /* Its worker is idle, waiting for this copy: it ends without
          * writing again and is joined once it has. */
         retro_atomic_store_release_int(&handle->quit, 1);
         retro_eventcount_notify(&handle->ec);
         if (mem_info.data)
            autosave_replace(i, &mem_info);
         continue;
      }
      memcpy(handle->snapshot, mem_info.data, handle->bufsize);
      handle->snapshot_gen = retro_atomic_load_relaxed_int(&handle->main_gen);
      retro_atomic_store_release_int(&handle->snapshot_requested, 0);
      retro_eventcount_notify(&handle->ec);
   }
}

void autosave_unlock(void)
{
   if (autosave_state.depth && !--autosave_state.depth)
      autosave_check();
}

static void autosave_fill(autosave_t *handle, unsigned slot,
      const void *data, int owed, bool kept)
{
   memcpy(handle->flush_buf[slot], data, handle->bufsize);
   handle->flush_owed[slot] = owed;
   handle->flush_gen[slot]  = retro_atomic_load_relaxed_int(
         &handle->main_gen);
   retro_atomic_store_release_int(&handle->flush[slot],
         kept ? AUTOSAVE_FLUSH_FULL | AUTOSAVE_FLUSH_KEPT
              : AUTOSAVE_FLUSH_FULL);
}

/* Hands @data to the worker of @handle to write after any flush out
 * and before any later image, then @owed. A @kept image is of memory
 * that stays as it is. Either replaces a kept or backed up image the
 * worker has not taken from the slot filled last. With both slots out
 * otherwise, only @owed is added, to the one filled last; a kept image
 * is left out, as the worker asks for a copy once they are written.
 * Copies; never waits. */
static void autosave_flush(autosave_t *handle, const void *data, int owed,
      bool kept)
{
   unsigned fill = handle->flush_fill;
   for (;;)
   {
      int w = retro_atomic_load_acquire_int(&handle->flush[!fill]);
      if (     (w & (AUTOSAVE_FLUSH_KEPT | AUTOSAVE_FLUSH_PAID))
            && !(w & AUTOSAVE_FLUSH_OUT) && (w & AUTOSAVE_FLUSH_FULL))
      {
         /* Fails only when the worker has just taken it. */
         if (!retro_atomic_cas_int(&handle->flush[!fill], w, 0))
            continue;
         autosave_fill(handle, !fill, data,
               owed | ((w & AUTOSAVE_FLUSH_PAID)
                  ? 0 : handle->flush_owed[!fill])
               | (w & ~AUTOSAVE_FLUSH_BITS), kept);
         break;
      }
      if (!(retro_atomic_load_acquire_int(&handle->flush[fill])
            & AUTOSAVE_FLUSH_FULL))
      {
         autosave_fill(handle, fill, data, owed, kept);
         handle->flush_fill = !fill;
         break;
      }
      if (kept)
         break;
      /* Fails only when the worker has just finished it, and it finished
       * the other first: the next turn fills. */
      if (     (w & AUTOSAVE_FLUSH_FULL)
            && retro_atomic_cas_int(&handle->flush[!fill], w, w | owed))
         break;
   }
   retro_eventcount_notify(&handle->ec);
}

/* Main thread, before it writes slot @i itself: an image the worker
 * took before this is older than the file and is not written over it.
 * The handle when one may still land after this write, else NULL. */
static autosave_t *autosave_main_write(unsigned i)
{
   autosave_t *handle;
   if (     i >= autosave_state.num
         || !(handle = autosave_state.list[i])
         || autosave_quitting(handle))
      return NULL;
   retro_atomic_fetch_add_seq_cst_int(&handle->main_gen, 1);
   /* Under way, or a state load's image, which is never dropped */
   if (     retro_atomic_load_seq_cst_int(&handle->committing)
         || ((retro_atomic_load_acquire_int(&handle->flush[0])
            & (AUTOSAVE_FLUSH_FULL | AUTOSAVE_FLUSH_KEPT))
            == AUTOSAVE_FLUSH_FULL)
         || ((retro_atomic_load_acquire_int(&handle->flush[1])
            & (AUTOSAVE_FLUSH_FULL | AUTOSAVE_FLUSH_KEPT))
            == AUTOSAVE_FLUSH_FULL))
      return handle;
   return NULL;
}

/* Main thread, before it writes the file of @handle itself: a state
 * load's image still in a slot is backed up from the image, oldest
 * first, as the worker would once it had written it. The slot is then
 * marked, unless a copy failed. */
static void autosave_main_pay(autosave_t *handle, bool sync)
{
   unsigned n;
   for (n = 0; n < 2; n++)
   {
      unsigned r;
      unsigned slot = n ? !handle->flush_fill : handle->flush_fill;
      int w         = retro_atomic_load_acquire_int(&handle->flush[slot]);
      int owed      = handle->flush_owed[slot] | (w & ~AUTOSAVE_FLUSH_BITS);
      if ((w & (AUTOSAVE_FLUSH_FULL | AUTOSAVE_FLUSH_KEPT
            | AUTOSAVE_FLUSH_PAID)) != AUTOSAVE_FLUSH_FULL)
         continue;
      for (r = 0; r < SAVE_BACKUP_REASON_LAST; r++)
      {
         enum save_backup_result res;
         if (!(owed & (1 << r)))
            continue;
         res = save_backups_copy_image(handle->path,
               (enum save_backup_reason)r, handle->flush_buf[slot],
               handle->bufsize, sync);
         if (res == SAVE_BACKUP_FAILED)
            break;
         if (res == SAVE_BACKUP_MADE)
            RARCH_LOG("[SRAM] Backed up \"%s\" (%s) before writing it.\n",
                  handle->path,
                  save_backups_reason_name((enum save_backup_reason)r));
      }
      if (r < SAVE_BACKUP_REASON_LAST)
         continue;
      /* Retried when the worker takes the image; ends when it has
       * written it and owed the copies, which the file write makes. */
      while (     (w & AUTOSAVE_FLUSH_FULL)
            && !retro_atomic_cas_int(&handle->flush[slot], w,
                  (w & AUTOSAVE_FLUSH_BITS) | AUTOSAVE_FLUSH_PAID))
         w = retro_atomic_load_acquire_int(&handle->flush[slot]);
   }
}

/* The file holds @mem_info's memory now: the worker writes it again
 * after what may land later. */
static void autosave_main_written(autosave_t *handle,
      const retro_ctx_memory_info_t *mem_info)
{
   if (handle && mem_info->size == handle->bufsize)
      autosave_flush(handle, mem_info->data, 0, true);
}

/* Slot @i's live worker, or NULL; its memory in @mem_info. */
static autosave_t *autosave_flushable(unsigned i,
      retro_ctx_memory_info_t *mem_info)
{
   autosave_t *handle;
   if (     i >= autosave_state.num
         || !(handle = autosave_state.list[i])
         || autosave_quitting(handle))
      return NULL;
   mem_info->id = handle->type;
   core_get_memory(mem_info);
   if (!mem_info->data || mem_info->size != handle->bufsize)
      return NULL;
   return handle;
}

#endif

static bool content_get_memory(retro_ctx_memory_info_t *mem_info,
      struct ram_type *ram, unsigned slot)
{
   ram->type     = task_save_files->elems[slot].attr.i;
   ram->path     = task_save_files->elems[slot].data;
   mem_info->id  = ram->type;

   core_get_memory(mem_info);
   content_mark_sized(slot, mem_info);

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
   void *buf           = NULL;
   bool success        = false;
   bool have_memory    = content_get_memory(&mem_info, &ram, slot);
   struct save_file *f = save_file_get(slot);

   /* On first run of content, SRAM file will
    * not exist. This is a common enough occurrence
    * that we should check before attempting to
    * invoke the relevant read_file() function */
   if (    (!ram.path || !*ram.path)
       || !path_is_valid(ram.path))
      return false;

   /* Not read now, so copied before anything writes over it. */
   if (!have_memory)
   {
      const char *core = runloop_state_get_ptr()->system.info.library_name;
      save_file_owe(f, SAVE_BACKUP_NOT_LOADED);
      RARCH_LOG("[SRAM] %s reported no memory for \"%s\" at load; "
            "left unread.\n", core ? core : "", ram.path);
      return false;
   }

#if defined(HAVE_COMPRESSION)
   /* Always use RZIP interface when reading SRAM
    * files - this will automatically handle uncompressed
    * data */
   if (!rzipstream_read_file(ram.path, &buf, &rc))
#else
   if (!filestream_read_file(ram.path, &buf, &rc))
#endif
   {
      save_file_owe(f, SAVE_BACKUP_UNREADABLE);
      if (     f
            && !(retro_atomic_fetch_or_int(&f->flags,
                  SAVE_FILE_UNREADABLE_NOTICED)
               & SAVE_FILE_UNREADABLE_NOTICED))
      {
         save_file_notice(MSG_SAVE_FILE_UNREADABLE, ram.path, true);
      }
      return false;
   }

   if (rc > 0)
   {
      if (f)
         retro_atomic_store_release_size(&f->loaded, (size_t)rc);
      if (rc > (ssize_t)mem_info.size)
      {
         RARCH_WARN("[SRAM] SRAM is larger than implementation expects, "
               "doing partial load (truncating %u %s %s %u).\n",
               (unsigned)rc,
               msg_hash_to_str(MSG_BYTES),
               msg_hash_to_str(MSG_TO),
               (unsigned)mem_info.size);
         rc = mem_info.size;
      }
      memcpy(mem_info.data, buf, (size_t)rc);
      success = true;
      if (f && !save_image_blank(buf, (size_t)rc))
         retro_atomic_fetch_or_int(&f->flags, SAVE_FILE_LOADED_NONBLANK);
   }

   if (buf)
      free(buf);

   return success;
}

/* The image a close or flush write could not put in @path goes to a
 * recovery file, uncompressed; either way the user is told. An erase
 * leaves nothing to recover. */
static void save_file_recover(const char *path, const void *data,
      size_t len, bool sync)
{
   char *rec = NULL;
   if (     len
         && (rec = (char*)malloc(PATH_MAX_LENGTH))
         && save_backups_recover(rec, PATH_MAX_LENGTH, path, data, len,
               sync))
   {
      save_file_notice(MSG_SAVE_WRITE_RECOVERED, path, true);
      RARCH_WARN("[SRAM] Unsaved progress: \"%s\"\n", rec);
   }
   else
      save_file_notice(MSG_SAVE_WRITE_FAILED, path, true);
   free(rec);
}

/* Unsaved progress from an earlier session newer than the save, once a
 * session. */
static void save_file_check_recovery(unsigned slot)
{
   char *rec;
   int64_t mtime       = INT64_MIN;
   struct save_file *f = save_file_get(slot);
   const char *path    = task_save_files->elems[slot].data;
   if (     !f
         || string_is_empty(path)
         || (retro_atomic_load_acquire_int(&f->flags)
            & SAVE_FILE_RECOVERY_NOTICED)
         || !(rec = (char*)malloc(PATH_MAX_LENGTH)))
      return;
   if (!path_get_mtime(path, &mtime))
      mtime = INT64_MIN;
   if (save_backups_find_recovery(rec, PATH_MAX_LENGTH, path, mtime))
   {
      retro_atomic_fetch_or_int(&f->flags, SAVE_FILE_RECOVERY_NOTICED);
      save_file_notice(MSG_SAVE_RECOVERY_NEWER, path, true);
      RARCH_WARN("[SRAM] Unsaved progress: \"%s\"\n", rec);
   }
   free(rec);
}

/**
 * content_save_ram_file:
 * @slot             : index into task_save_files
 * @compress         : whether to use rzip compression
 * @sync             : whether to wait for the disk before replacing
 * @rewrite          : write even if the file matches memory
 * @copy             : whether the safety copies the file owes may be
 *                     made here
 * @flush            : a state load replaces the image next: if it cannot
 *                     be written, it goes to a recovery file
 * @kept             : the image stays in memory: if it cannot be
 *                     written, that is shown once a session
 *
 * Save a RAM state from memory to disk.
 * Skips the write if the on-disk content already
 * matches memory (common when autosave has been active).
 */
static bool content_save_ram_file(unsigned slot, bool compress, bool sync,
      bool rewrite, bool copy, bool flush, bool kept)
{
   struct ram_type ram;
   retro_ctx_memory_info_t mem_info;
   struct save_file *f;
   bool changed;
#ifdef HAVE_THREADS
   autosave_t *handle;
#endif

   /* Memory that had a size this session and reports 0 bytes, its
    * pointer kept, was erased: its save is emptied. */
   if (     !content_get_memory(&mem_info, &ram, slot)
         && (     !mem_info.data
               || !task_save_files->elems[slot].userdata
               || !path_is_valid(ram.path)))
      return false;
   f = save_file_get(slot);
#ifdef HAVE_THREADS
   handle = autosave_main_write(slot);
   if (handle && copy)
      autosave_main_pay(handle, sync);
#endif

   /* Quick check: if the file already exists and matches
    * current memory contents, skip the write entirely.
    * This is the common case when autosave has been running. */
   if (   ram.path && *ram.path
       && !rewrite
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
#ifdef HAVE_THREADS
         autosave_main_written(handle, &mem_info);
#endif
         return true;
      }
   }

   changed = save_file_changed(f, ram.path);
   if (!save_file_pay(f, ram.path, mem_info.data, mem_info.size,
            copy, sync))
   {
      if (rewrite)
         save_files_unsynced = true;
      if (!copy)
      {
         RARCH_LOG("[SRAM] \"%s\" is written once it is backed up.\n",
               ram.path);
         return false;
      }
      /* The image stays in memory: no recovery file. */
      if (kept || (!sync && !flush))
      {
         if (     f
               && !(retro_atomic_fetch_or_int(&f->flags,
                     SAVE_FILE_BACKUP_NOTICED)
                  & SAVE_FILE_BACKUP_NOTICED))
            save_file_notice(MSG_SAVE_BACKUP_FAILED, ram.path, true);
         return false;
      }
      if (!mem_info.size)
         RARCH_WARN("[SRAM] \"%s\" not emptied: it could not be "
               "backed up.\n", ram.path);
      save_file_recover(ram.path, mem_info.data, mem_info.size, sync);
      return false;
   }

   if (     changed
         && !(retro_atomic_fetch_or_int(&f->flags, SAVE_FILE_CHANGED_NOTICED)
            & SAVE_FILE_CHANGED_NOTICED))
      save_file_notice(MSG_SAVE_CHANGED_ELSEWHERE, ram.path, true);

   if (!sync)
      save_files_unsynced = true;

   RARCH_LOG("[SRAM] %s #%u %s \"%s\".\n",
         msg_hash_to_str(MSG_SAVING_RAM_TYPE),
         ram.type,
         msg_hash_to_str(MSG_TO),
         ram.path);

   /* Write via a temporary file, so a crash or power loss mid-save
    * leaves the previous save intact instead of a truncated one; in
    * place where there is no rename (filestream_atomic_in_place()). */
#if defined(HAVE_COMPRESSION)
   if (compress)
   {
      char tmp_path[PATH_MAX_LENGTH];
      if (filestream_atomic_in_place(ram.path))
      {
         if (!rzipstream_write_file(ram.path, mem_info.data, mem_info.size))
            goto fail;
      }
      else if (     !filestream_atomic_temp_name(tmp_path,
                        sizeof(tmp_path), ram.path)
            || !content_finish_tmp(ram.path, rzipstream_write_file(
                  tmp_path, mem_info.data, mem_info.size), sync))
         goto fail;
   }
   else
#endif
   {
      if (!(sync
            ? filestream_write_file_atomic_synced(
                  ram.path, mem_info.data, mem_info.size)
            : filestream_write_file_atomic(
                  ram.path, mem_info.data, mem_info.size)))
         goto fail;
   }

   save_file_keep_stat(f, ram.path);
   content_save_written();
#ifdef HAVE_THREADS
   autosave_main_written(handle, &mem_info);
#endif
   RARCH_LOG("[SRAM] %s \"%s\".\n",
         msg_hash_to_str(MSG_SAVED_SUCCESSFULLY_TO),
         ram.path);

   return true;

fail:
   if (rewrite)
      save_files_unsynced = true;
   RARCH_ERR("[SRAM] %s.\n",
         msg_hash_to_str(MSG_FAILED_TO_SAVE_SRAM));
   if (kept)
   {
      if (     f
            && !(retro_atomic_fetch_or_int(&f->flags,
                  SAVE_FILE_WRITE_NOTICED)
               & SAVE_FILE_WRITE_NOTICED))
         save_file_notice(MSG_SAVE_WRITE_FAILED, ram.path, true);
      return false;
   }
   save_file_recover(ram.path, mem_info.data, mem_info.size, sync);
   return false;
}

bool event_save_files(bool is_sram_used, bool compress_files, bool sync,
      bool copy, const char *path_cheat_database)
{
   unsigned i;
   bool ret     = true;
   bool rewrite = sync && save_files_unsynced;
#ifdef HAVE_CHEATS
   cheat_manager_save_game_specific_cheats(path_cheat_database);
#endif
   if (     !task_save_files
         || !is_sram_used
         || runloop_state_get_ptr()->save_load_hold)
      return false;
   if (sync)
      save_files_unsynced = false;
   for (i = 0; i < task_save_files->size; i++)
      if (!content_save_ram_file(i, compress_files, sync, rewrite, copy,
               false, false))
         ret = false;
   return ret;
}

static void save_files_flush(bool is_sram_used, bool compress_files,
      int owed, bool kept)
{
   unsigned i;
   if (     !task_save_files
         || !is_sram_used
         || runloop_state_get_ptr()->save_load_hold)
      return;
   for (i = 0; i < task_save_files->size; i++)
   {
      int bits = task_save_files->elems[i].attr.i == RETRO_MEMORY_SAVE_RAM
         ? owed : 0;
#ifdef HAVE_THREADS
      retro_ctx_memory_info_t mem_info;
      autosave_t *handle = autosave_flushable(i, &mem_info);
#endif
      if (bits)
         RARCH_LOG("[SRAM] \"%s\" is backed up before its next write.\n",
               task_save_files->elems[i].data);
#ifdef HAVE_THREADS
      if (handle)
      {
         autosave_flush(handle, mem_info.data, bits, kept);
         content_save_written();
         continue;
      }
#endif
      /* No worker: written here, owed copies made first; synced when
       * the game stops (menu, pause), not for a state load. */
      content_save_ram_file(i, compress_files, kept, false, true, !kept,
            kept);
      save_file_owe_bits(save_file_get(i), bits);
   }
}

void event_flush_save_files(bool is_sram_used, bool compress_files,
      int owed)
{
   save_files_flush(is_sram_used, compress_files, owed, false);
}

void event_flush_kept_save_files(bool is_sram_used, bool compress_files)
{
   save_files_flush(is_sram_used, compress_files, 0, true);
}

bool event_load_save_files(bool is_sram_load_disabled)
{
   unsigned i;
   bool ret = false;

   if (!task_save_files)
      return false;

   for (i = 0; i < task_save_files->size; i++)
      save_file_keep_stat(save_file_get(i), task_save_files->elems[i].data);

   /* Not loaded on request: copied before anything writes over it. */
   if (is_sram_load_disabled)
   {
      for (i = 0; i < task_save_files->size; i++)
      {
         if (path_is_valid(task_save_files->elems[i].data))
            save_file_owe(save_file_get(i), SAVE_BACKUP_NOT_LOADED);
         save_file_check_recovery(i);
      }
      return false;
   }

   /* Report a successful load operation if
    * any type of RAM file is found and
    * processed correctly */
   for (i = 0; i < task_save_files->size; i++)
   {
      ret |= content_load_ram_file(i);
      save_file_check_recovery(i);
   }

   return ret;
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
 * The comparison ignores case on every platform. A false positive
 * costs nothing beyond leaving a file alone for one more round, while
 * a false negative is the data loss this exists to prevent.
 *
 * The session's auto state (<state name>.auto) is owned in the same
 * window: the auto save at unload writes over it.
 *
 * Returns: true if a loaded core owns @path.
 **/
bool content_savefile_is_live(const char *path)
{
   size_t i, _len;
   const char *state;

   /* Held (save_load_hold): nothing was loaded, nothing writes them. */
   if (     !task_save_files
         || string_is_empty(path)
         || runloop_state_get_ptr()->save_load_hold)
      return false;

   for (i = 0; i < task_save_files->size; i++)
      if (string_is_equal_noncase(task_save_files->elems[i].data, path))
         return true;

   state = runloop_state_get_ptr()->name.savestate;
   _len  = strlen(state);
   return   _len
         && string_starts_with_case_insensitive(path, state)
         && string_is_equal_noncase(path + _len, ".auto");
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

/* Changes at every content load and unload. Main thread only. */
static unsigned content_savefile_lifetimes = 0;
static unsigned content_save_write_count   = 0;
static unsigned content_state_fetch_count  = 0;

void path_deinit_savefile(void)
{
   if (task_save_files)
      string_list_free(task_save_files);
   task_save_files = NULL;
   free(save_files);
   save_files      = NULL;
   save_files_num  = 0;
   content_savefile_lifetimes++;
}

void path_init_savefile_new(void)
{
   task_save_files = string_list_new();
   content_savefile_lifetimes++;
}

unsigned content_savefile_lifetime(void)
{
   return content_savefile_lifetimes;
}

void content_save_written(void)
{
   content_save_write_count++;
}

unsigned content_save_writes(void)
{
   return content_save_write_count;
}

void content_state_fetched(void)
{
   content_state_fetch_count++;
}

unsigned content_state_fetches(void)
{
   return content_state_fetch_count;
}

void *savefile_ptr_get(void)
{
   return task_save_files;
}

