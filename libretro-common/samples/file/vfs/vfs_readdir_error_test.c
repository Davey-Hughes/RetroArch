/* retro_dirent_error() once retro_readdir() has returned false: true
 * when the folder could not be read to its end, false at its end.
 *
 *   dir_list_midway_error - a folder whose read fails part-way is not
 *                           listed: dir_list_new() returns NULL.
 *
 * vfs_implementation.c is compiled into this file so that a test can
 * break a handle underneath the next read, as a failing disk would. */

/* The *_impl functions and the VFS interface's callbacks share a
 * handle type only under VFS_FRONTEND, as in file_stream.c. */
#define VFS_FRONTEND

#include <retro_posix_source.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <dirent.h>
#endif

/* dir_list reads through the wrapper below */
#define retro_vfs_readdir_impl test_real_readdir
#include "../../../vfs/vfs_implementation.c"
#undef retro_vfs_readdir_impl

#include <retro_dirent.h>
#include <file/file_path.h>
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <streams/file_stream.h>

static int failures;

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static const char *dir_name = "readdir_error_dir";
static const char *files[]  = {
   "readdir_error_dir/a.bin",
   "readdir_error_dir/b.bin",
   "readdir_error_dir/c.bin"
};

static void break_handle(struct RDIR *rdir)
{
   struct retro_vfs_dir_handle *h = (struct retro_vfs_dir_handle*)rdir;
#if defined(_WIN32)
   FindClose(h->directory);
   h->directory = INVALID_HANDLE_VALUE;
#else
   close(dirfd(h->directory));
#endif
}

/* The read before which the wrapper breaks the handle; -1 for none */
static int break_at = -1;
static int reads;

bool retro_vfs_readdir_impl(libretro_vfs_implementation_dir *rdir)
{
   if (reads++ == break_at)
      break_handle((struct RDIR*)rdir);
   return test_real_readdir(rdir);
}

static unsigned read_all(struct RDIR *rdir)
{
   unsigned n = 0;
   while (retro_readdir(rdir))
      n++;
   return n;
}

int main(void)
{
   struct RDIR        *rdir;
   struct string_list *list;
   struct string_list  init_list = {0};
   unsigned            i;

   path_mkdir(dir_name);
   for (i = 0; i < 3; i++)
      CHECK(filestream_write_file(files[i], "x", 1));

   /* read to its end */
   CHECK((rdir = retro_opendir_include_hidden(dir_name, true)) != NULL);
   if (rdir)
   {
      CHECK(!retro_dirent_error(rdir));
      CHECK(read_all(rdir) >= 3);
      CHECK(!retro_dirent_error(rdir));
      retro_closedir(rdir);
   }

   /* the handle fails under a read: after the entry FindFirstFile
    * holds on Win32; elsewhere before the first read, since a libc
    * that saw the end in its first refill (Apple's) reads no more */
   CHECK((rdir = retro_opendir_include_hidden(dir_name, true)) != NULL);
   if (rdir)
   {
#if defined(_WIN32)
      CHECK(retro_readdir(rdir));
#endif
      break_handle(rdir);
      read_all(rdir);
      CHECK(retro_dirent_error(rdir));
      retro_closedir(rdir);
   }

   /* dir_list_midway_error: as above, under dir_list */
#if defined(_WIN32)
   break_at = 1;
#else
   break_at = 0;
#endif
   reads = 0;
   list  = dir_list_new(dir_name, NULL, false, true, false, false);
   CHECK(!list);
   string_list_free(list);
   reads = 0;
   CHECK(!dir_list_initialize(&init_list, dir_name, NULL, false, true,
         false, false));
   string_list_deinitialize(&init_list);
   break_at = -1;
   list = dir_list_new(dir_name, NULL, false, true, false, false);
   CHECK(list && list->size == 3);
   string_list_free(list);

   for (i = 0; i < 3; i++)
      filestream_delete(files[i]);
   filestream_delete(dir_name);

   puts(failures ? "FAIL vfs_readdir_error_test" : "PASS vfs_readdir_error_test");
   return failures ? 1 : 0;
}
