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
#include <features/features_cpu.h>
#include <rthreads/retro_eventcount.h>
#endif
#include <retro_atomic.h>
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

#ifdef HAVE_THREADS
/* The worker's own temporary: a SAVE_FILES on the main thread uses
 * "<path>.tmp" and may run while the worker writes. */
#define AUTOSAVE_TMP_SUFFIX ".auto.tmp"

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
   char *path;
   char *tmp_path;
   /* Retired handles whose workers may not have ended yet */
   autosave_t *prev;
   sthread_t *thread;
   size_t bufsize;
   /* Main thread only: retired, when to try a replacement again */
   retro_time_t retry_time;
   /* What the worker sleeps on: the copy it asked for, the end of its
    * interval, or quit. */
   retro_eventcount_t ec;
   unsigned interval;
   unsigned type;
   /* Only the main thread reads live SRAM. The worker requests a copy
    * and waits until autosave_check() has made it and cleared this. */
   retro_atomic_int_t snapshot_requested;
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

static void autosave_thread(void *data)
{
   autosave_t *save = (autosave_t*)data;

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

      if (autosave_quitting(save))
         break;

      /* The snapshot remains ours until the next request. Keep the
       * last written image unchanged if opening, writing, syncing or
       * renaming fails; the file keeps its old contents. */
      if (save->unwritten ||
            memcmp(save->buffer, save->snapshot, save->bufsize) != 0)
      {
         int ret;
         intfstream_t *file = NULL;
         bool written       = false;
         save->unwritten    = true;

         if (save->compress)
            file = intfstream_open_rzip_file(save->tmp_path,
                  RETRO_VFS_FILE_ACCESS_WRITE);
         else
            file = intfstream_open_file(save->tmp_path,
                  RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);

         if (file)
         {
            written = intfstream_write(file, save->snapshot,
                  save->bufsize) == (int64_t)save->bufsize;
            if (intfstream_flush(file) != 0)
               written = false;
            if (intfstream_close(file) != 0)
               written = false;
            free(file);
         }

         ret = filestream_finish_atomic_suffix(save->path,
               AUTOSAVE_TMP_SUFFIX, written, true);
         if (ret == 0)
         {
            memcpy(save->buffer, save->snapshot, save->bufsize);
            save->unwritten = false;
         }
         else if (ret == 1)
            filestream_delete(save->tmp_path);
      }

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
 *
 * Create and initialize autosave object. Logs nothing: it is also
 * called from autosave_check().
 *
 * @return Pointer to new autosave_t object if successful, otherwise
 * NULL.
 **/
static autosave_t *autosave_new(const char *path,
      const void *data, size_t len,
      unsigned interval, bool compress, unsigned type, bool unwritten)
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
   handle->bufsize               = len;
   handle->interval              = interval;
   handle->type                  = type;
   handle->unwritten             = unwritten;
   handle->retry_time            = 0;
   handle->buffer                = NULL;
   handle->snapshot              = NULL;
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

   if (!(buf = malloc(len)))
   {
      free(handle->tmp_path);
      free(handle->path);
      free(handle);
      return NULL;
   }

   handle->buffer                = buf;

   memcpy(handle->buffer, data, handle->bufsize);

   handle->snapshot              = malloc(len);
   if (handle->snapshot)
      handle->ec_inited          = retro_eventcount_init(&handle->ec);

   if (!handle->snapshot || !handle->ec_inited)
   {
      free(handle->snapshot);
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
      retro_eventcount_free(&handle->ec);
      free(handle->tmp_path);
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

   free(handle->snapshot);
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

/* A handle and the retired ones it replaced. */
static void autosave_free_chain(autosave_t *handle)
{
   while (handle)
   {
      autosave_t *prev = handle->prev;
      autosave_free(handle);
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
         autosave_free(old);
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
   size_t      len    = old->bufsize < mem_info->size
      ? old->bufsize : mem_info->size;
   /* The old worker waits for a copy or has ended: what it had not
    * written goes to the new one. */
   bool    unwritten  = old->unwritten
      || memcmp(old->buffer, mem_info->data, len) != 0;
   autosave_t *handle = autosave_new(old->path, mem_info->data,
         mem_info->size, old->interval, old->compress, old->type,
         unwritten);
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

   if (autosave_interval < 1 || !task_save_files)
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

      if (mem_info.size == 0)
         continue;

      if (!(auto_st = autosave_new(path,
            mem_info.data,
            mem_info.size,
            autosave_interval,
            compress_files,
            type,
            false)))
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
         if (mem_info.data && mem_info.size)
            autosave_replace(i, &mem_info);
         continue;
      }
      /* The worker asked and is waiting: the snapshot is ours to fill
       * until the flag clears. */
      if (!retro_atomic_load_acquire_int(&handle->snapshot_requested))
         continue;
      /* A core may move or resize its memory at any time. */
      core_get_memory(&mem_info);
      if (!mem_info.data || mem_info.size != handle->bufsize)
      {
         /* Its worker is idle, waiting for this copy: it ends without
          * writing again and is joined once it has. */
         retro_atomic_store_release_int(&handle->quit, 1);
         retro_eventcount_notify(&handle->ec);
         if (mem_info.data && mem_info.size)
            autosave_replace(i, &mem_info);
         continue;
      }
      memcpy(handle->snapshot, mem_info.data, handle->bufsize);
      retro_atomic_store_release_int(&handle->snapshot_requested, 0);
      retro_eventcount_notify(&handle->ec);
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
      size_t len, unsigned type)
{
   size_t _len;
   char path[PATH_MAX_LENGTH + 256 + 32];
   path[0] = '\0';
   _len    = fill_pathname_application_data(path,
            sizeof(path));

   if (_len)
   {
      time_t time_;
      struct tm tm_;

      /* rtime_localtime leaves tm_ unwritten when localtime() fails */
      memset(&tm_, 0, sizeof(tm_));
      time(&time_);
      rtime_localtime(&time_, &tm_);
      _len += strlcpy_lit(path  + _len, "/RetroArch-recovery-", sizeof(path) - _len);
      _len += snprintf(path + _len, sizeof(path) - _len, "%u-", type);
      strftime(path + _len, sizeof(path) - _len,
            "%Y-%m-%d-%H-%M-%S", &tm_);

      /* Fallback (emergency) saves are always
       * uncompressed
       * > If a regular save fails, then the host
       *   system is experiencing serious technical
       *   difficulties (most likely some kind of
       *   hardware failure)
       * > In this case, we don't want to further
       *   complicate matters by introducing zlib
       *   compression overheads */
      if (filestream_write_file(path, data, len))
      {
         RARCH_WARN("[SRAM] Succeeded in saving RAM data to \"%s\".\n", path);
         return true;
      }
   }

   return false;
}

/**
 * content_save_ram_file:
 * @slot             : index into task_save_files
 * @compress         : whether to use rzip compression
 * @sync             : whether to wait for the disk before replacing
 * @rewrite          : write even if the file matches memory
 *
 * Save a RAM state from memory to disk.
 * Skips the write if the on-disk content already
 * matches memory (common when autosave has been active).
 */
static bool content_save_ram_file(unsigned slot, bool compress, bool sync,
      bool rewrite)
{
   struct ram_type ram;
   retro_ctx_memory_info_t mem_info;

   if (!content_get_memory(&mem_info, &ram, slot))
      return false;

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
         return true;
      }
   }

   if (!sync)
      save_files_unsynced = true;

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

   RARCH_LOG("[SRAM] %s \"%s\".\n",
         msg_hash_to_str(MSG_SAVED_SUCCESSFULLY_TO),
         ram.path);

   return true;

fail:
   if (rewrite)
      save_files_unsynced = true;
   RARCH_ERR("[SRAM] %s.\n",
         msg_hash_to_str(MSG_FAILED_TO_SAVE_SRAM));
   RARCH_WARN("[SRAM] Attempting to recover...\n");

   /* In case the file could not be written to,
    * the fallback function 'dump_to_file_desperate'
    * will be called. */
   if (!dump_to_file_desperate(
            mem_info.data, mem_info.size, ram.type))
      RARCH_WARN("[SRAM] Failed. Cannot recover save file.\n");
   return false;
}

bool event_save_files(bool is_sram_used, bool compress_files, bool sync,
      const char *path_cheat_database)
{
   unsigned i;
   bool ret     = true;
   bool rewrite = sync && save_files_unsynced;
#ifdef HAVE_CHEATS
   cheat_manager_save_game_specific_cheats(path_cheat_database);
#endif
   if (!task_save_files || !is_sram_used)
      return false;
   if (sync)
      save_files_unsynced = false;
   for (i = 0; i < task_save_files->size; i++)
      if (!content_save_ram_file(i, compress_files, sync, rewrite))
         ret = false;
   return ret;
}

bool event_load_save_files(bool is_sram_load_disabled)
{
   unsigned i;
   bool ret = false;

   if (!task_save_files || is_sram_load_disabled)
      return false;

   /* Report a successful load operation if
    * any type of RAM file is found and
    * processed correctly */
   for (i = 0; i < task_save_files->size; i++)
      ret |= content_load_ram_file(i);

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
 * Returns: true if a loaded core owns @path.
 **/
bool content_savefile_is_live(const char *path)
{
   size_t i;

   if (!task_save_files || string_is_empty(path))
      return false;

   for (i = 0; i < task_save_files->size; i++)
      if (string_is_equal_noncase(task_save_files->elems[i].data, path))
         return true;

   return false;
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

