/* Oracle for what an atomic write leaves behind when its rename into
 * place is refused.
 *
 *   rename_cb_keeps_dest  - under a frontend VFS whose rename refuses an
 *                           existing destination, a failed replacement
 *                           leaves the old file as it was; an aside
 *                           copy already there is left untouched and
 *                           the next free aside name is used.
 *   windows_locked_rename - (Windows) a target held open without
 *                           FILE_SHARE_DELETE is kept and the temporary
 *                           removed.
 */

/* The *_impl functions and the VFS interface's callbacks share a
 * handle type only under VFS_FRONTEND, as in file_stream.c. */
#define VFS_FRONTEND

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <boolean.h>
#include <libretro.h>
#include <streams/file_stream.h>
#include <vfs/vfs_implementation.h>

static unsigned failures = 0;

#define CHECK(cond, msg) \
   do { \
      if (!(cond)) \
      { \
         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
         failures++; \
      } \
   } while (0)

#define TARGET   "atomic.bin"
#define ASIDE    TARGET ".old"
#define ASIDE2   TARGET ".2.old"
#define TEMP     TARGET ".tmp"

static void numbered_aside(char *s, unsigned n)
{
   sprintf(s, TARGET ".%u.old", n);
}

static bool exists(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return false;
   fclose(f);
   return true;
}

static bool content_is(const char *path, const char *want)
{
   char   buf[64];
   size_t len;
   FILE  *f = fopen(path, "rb");
   if (!f)
      return false;
   len = fread(buf, 1, sizeof(buf), f);
   fclose(f);
   return len == strlen(want) && !memcmp(buf, want, len);
}

static void put(const char *path, const char *text)
{
   FILE *f = fopen(path, "wb");
   if (f)
   {
      fputs(text, f);
      fclose(f);
   }
}

static void clean(void)
{
   char     name[64];
   unsigned n;
   remove(TARGET);
   remove(ASIDE);
   remove(TEMP);
   for (n = 2; n <= 99; n++)
   {
      numbered_aside(name, n);
      remove(name);
   }
}

/* A frontend VFS: the built-in one, except that rename refuses an
 * existing destination, and the temporary when refuse_temp is set. */
static struct retro_vfs_interface frontend_iface;
static bool refuse_temp;

static int RETRO_CALLCONV frontend_rename(const char *old_path,
      const char *new_path)
{
   if (exists(new_path) || (refuse_temp && !strcmp(old_path, TEMP)))
      return -1;
   return retro_vfs_file_rename_impl(old_path, new_path);
}

static void use_frontend_vfs(bool on)
{
   struct retro_vfs_interface_info info;

   memset(&frontend_iface, 0, sizeof(frontend_iface));
   frontend_iface.get_path = retro_vfs_file_get_path_impl;
   frontend_iface.open     = retro_vfs_file_open_impl;
   frontend_iface.close    = retro_vfs_file_close_impl;
   frontend_iface.size     = retro_vfs_file_size_impl;
   frontend_iface.tell     = retro_vfs_file_tell_impl;
   frontend_iface.seek     = retro_vfs_file_seek_impl;
   frontend_iface.read     = retro_vfs_file_read_impl;
   frontend_iface.write    = retro_vfs_file_write_impl;
   frontend_iface.flush    = retro_vfs_file_flush_impl;
   frontend_iface.remove   = retro_vfs_file_remove_impl;
   frontend_iface.rename   = frontend_rename;
   frontend_iface.truncate = retro_vfs_file_truncate_impl;

   info.required_interface_version = FILESTREAM_REQUIRED_VFS_VERSION;
   info.iface                      = on ? &frontend_iface : NULL;
   filestream_vfs_init(&info);
}

static void lane_rename_cb_keeps_dest(void)
{
   char     name[64];
   unsigned n;
   RFILE   *f;

   clean();
   put(TARGET, "old");
   use_frontend_vfs(true);

   refuse_temp = true;
   CHECK((f = filestream_open_atomic(TARGET)) != NULL, "open_atomic failed");
   if (f)
   {
      filestream_write(f, "new", 3);
      CHECK(filestream_commit_atomic(f, TARGET, true) == 1,
            "a refused rename did not return 1");
   }
   CHECK(content_is(TARGET, "old"), "a refused rename lost the destination");
   CHECK(!exists(ASIDE), "a refused rename left the aside copy");
   CHECK(exists(TEMP), "commit_atomic removed the temporary it hands back");
   remove(TEMP);

   CHECK(!filestream_write_file_atomic(TARGET, "new", 3),
         "a refused rename reported success");
   CHECK(content_is(TARGET, "old"), "write_file_atomic lost the destination");
   CHECK(!exists(ASIDE) && !exists(TEMP),
         "write_file_atomic left a file beside the destination");
   refuse_temp = false;

   CHECK(filestream_write_file_atomic(TARGET, "new", 3),
         "a replacement through the aside copy failed");
   CHECK(content_is(TARGET, "new"), "replacement content wrong");
   CHECK(!exists(ASIDE) && !exists(TEMP),
         "the replacement left a file beside the destination");

   put(ASIDE, "stale");
   CHECK(filestream_write_file_atomic(TARGET, "newer", 5),
         "a replacement with a stale aside copy there failed");
   CHECK(content_is(TARGET, "newer"), "replacement content wrong");
   CHECK(content_is(ASIDE, "stale"), "a stale aside copy was changed");
   CHECK(!exists(ASIDE2) && !exists(TEMP),
         "the replacement left its own aside copy or the temporary");

   refuse_temp = true;
   CHECK(!filestream_write_file_atomic(TARGET, "lost", 4),
         "a refused rename reported success");
   refuse_temp = false;
   CHECK(content_is(TARGET, "newer"),
         "a refused rename beside a stale aside lost the destination");
   CHECK(content_is(ASIDE, "stale"), "a stale aside copy was changed");
   CHECK(!exists(ASIDE2) && !exists(TEMP),
         "a refused rename left its own aside copy or the temporary");

   for (n = 2; n <= 99; n++)
   {
      numbered_aside(name, n);
      put(name, "taken");
   }
   CHECK(!filestream_write_file_atomic(TARGET, "lost", 4),
         "a replacement with every aside name taken succeeded");
   CHECK(content_is(TARGET, "newer"),
         "every aside name taken cost the destination");
   CHECK(content_is(ASIDE, "stale") && content_is(ASIDE2, "taken"),
         "an aside copy was changed");
   CHECK(!exists(TEMP), "the temporary was left behind");

   use_frontend_vfs(false);
   clean();
   printf("[rename_cb_keeps_dest] done\n");
}

static void lane_windows_locked_rename(void)
{
#if defined(_WIN32) && !defined(_XBOX)
   HANDLE h;

   clean();
   put(TARGET, "old");
   h = CreateFileA(TARGET, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
   CHECK(h != INVALID_HANDLE_VALUE, "could not hold the target open");
   CHECK(!filestream_write_file_atomic(TARGET, "new", 3),
         "a rename over a locked target reported success");
   CHECK(!exists(TEMP), "the temporary was left behind");
   if (h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
   CHECK(content_is(TARGET, "old"), "the locked target was changed");
   clean();
   printf("[windows_locked_rename] done\n");
#else
   printf("[windows_locked_rename] skipped: Windows only\n");
#endif
}

int main(void)
{
   lane_rename_cb_keeps_dest();
   lane_windows_locked_rename();

   if (failures)
   {
      fprintf(stderr, "%u check(s) failed\n", failures);
      return 1;
   }
   printf("[pass] filestream_atomic_test\n");
   return 0;
}
