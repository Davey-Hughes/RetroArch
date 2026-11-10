/* Oracle for the replace-on-rename branch of retro_vfs_file_rename_impl
 * taken by the targets whose filesystem refuses to rename onto an
 * existing entry (libfat on GameCube/Wii, FatFs on PS2, FS on 3DS and
 * Switch, Wii U, PS3).  The VFS is compiled here with -DGEKKO, and
 * rename(), remove() and stat() are wrapped to behave as those
 * filesystems do - rename onto an existing name fails with EEXIST -
 * and to count every call.
 *
 * vfs_rename_replace_test_3ds and _wiiu (-DRENAME_REPLACES) run the
 * same lanes on a VFS compiled with -D_3DS or -DWIIU, whose rename()
 * may replace an existing name (libctru's deletes it first), so the
 * wrapped rename replaces one; those look for a free aside name first.
 *
 * What these lanes pin, through filestream_write_file_atomic():
 *
 *   fresh     - a new file lands with one stat and one rename.
 *   overwrite - the old file is replaced with one stat, two renames
 *               and one remove, nothing is left beside it, and no name
 *               that does not exist is removed: on these filesystems
 *               that is a lookup scanning the whole directory, paid
 *               per file of a bulk extraction.
 *   leftover  - an aside file left by an interrupted run does not
 *               stop the replacement and is kept: the target goes
 *               aside as "<target>.old2" instead.
 *   refused   - when the replacement cannot be renamed into place,
 *               the old file is put back unchanged.
 *   interrupted - with no target and the last good copy at
 *               "<target>.old", as a replace cut short between its
 *               renames leaves them, two more writes keep that copy.
 *   user_old  - the user's own "<target>.old" and "<target>.old2" are
 *               kept; with every name to "<target>.old99" taken the
 *               replace fails and the target is kept.
 *   restore   - renaming "<target>.old" onto "<target>", as when a
 *               backup is restored, does not delete the source by
 *               taking it for a leftover aside.
 *   restore_case - the same when only the letter case differs, as
 *               FAT volumes treat those as one name; a leftover aside
 *               is not touched.
 *   frontend  - under a frontend VFS whose rename refuses an existing
 *               destination, as the libretro VFS API allows, an
 *               existing file is still replaced.
 */

/* The *_impl functions and the VFS interface's callbacks share a
 * handle type only under VFS_FRONTEND, as in file_stream.c. */
#define VFS_FRONTEND

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

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

#define TARGET   "rename_replace.bin"
#define ASIDE    TARGET ".old"
#define TEMP     TARGET ".tmp"
#define ASIDE2   TARGET ".old2"
#define ASIDE3   TARGET ".old3"
#define UPPER    "RENAME_REPLACE.BIN.OLD"

/* Wrapped libc */

int __real_rename(const char *old_path, const char *new_path);
int __real_remove(const char *path);
int __real_stat(const char *path, struct stat *st);

static unsigned n_rename;
static unsigned n_rename_refused;
static unsigned n_remove;
static unsigned n_remove_missing;
static unsigned n_stat;
static bool refuse_temp;           /* fail renaming the temporary in */

static bool exists(const char *path)
{
   struct stat st;
   return __real_stat(path, &st) == 0;
}

int __wrap_rename(const char *old_path, const char *new_path)
{
   n_rename++;
#ifdef RENAME_REPLACES
   if (refuse_temp && !strcmp(old_path, TEMP))
#else
   if (     exists(new_path)
         || (refuse_temp && !strcmp(old_path, TEMP)))
#endif
   {
      n_rename_refused++;
      errno = EEXIST;
      return -1;
   }
   return __real_rename(old_path, new_path);
}

int __wrap_remove(const char *path)
{
   n_remove++;
   if (!exists(path))
      n_remove_missing++;
   return __real_remove(path);
}

int __wrap_stat(const char *path, struct stat *st)
{
   n_stat++;
   return __real_stat(path, st);
}

static void reset_counts(void)
{
   n_rename         = 0;
   n_rename_refused = 0;
   n_remove         = 0;
   n_remove_missing = 0;
   n_stat           = 0;
}

static void print_counts(const char *lane)
{
   printf("[%s] stat %u, rename %u (%u refused), remove %u (%u missing)\n",
         lane, n_stat, n_rename, n_rename_refused, n_remove,
         n_remove_missing);
}

/* Fixture */

static bool content_is(const char *path, const char *want)
{
   void   *buf = NULL;
   int64_t len = 0;
   bool    ok;

   if (!filestream_read_file(path, &buf, &len))
      return false;
   ok = (len == (int64_t)strlen(want) && !memcmp(buf, want, (size_t)len));
   free(buf);
   return ok;
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

/* A frontend VFS: the built-in one, except that rename refuses an
 * existing destination. */
static struct retro_vfs_interface frontend_iface;

static int RETRO_CALLCONV frontend_rename(const char *old_path,
      const char *new_path)
{
   if (exists(new_path))
      return -1;
   return retro_vfs_file_rename_impl(old_path, new_path);
}

static void install_frontend_vfs(void)
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
   info.iface                      = &frontend_iface;
   filestream_vfs_init(&info);
}

static void aside_name(char *s, unsigned n)
{
   if (n > 1)
      sprintf(s, "%s.old%u", TARGET, n);
   else
      sprintf(s, "%s.old", TARGET);
}

static void clean(void)
{
   char     name[64];
   unsigned n;

   __real_remove(TARGET);
   __real_remove(TEMP);
   __real_remove(UPPER);
   for (n = 1; n <= 99; n++)
   {
      aside_name(name, n);
      __real_remove(name);
   }
}

int main(void)
{
   const char *v1 = "first version";
   const char *v2 = "second version, longer";
   const char *v3 = "third";
   char        name[64];
   unsigned    n;

   clean();

   /* fresh */
   reset_counts();
   CHECK(filestream_write_file_atomic(TARGET, v1, strlen(v1)),
         "fresh write failed");
   print_counts("fresh");
   CHECK(content_is(TARGET, v1), "fresh content wrong");
   CHECK(n_stat == 1 && n_rename == 1 && n_rename_refused == 0,
         "fresh write took more than one stat and one rename");
   CHECK(n_remove == 0, "fresh write removed something");

   /* overwrite */
   reset_counts();
   CHECK(filestream_write_file_atomic(TARGET, v2, strlen(v2)),
         "overwrite failed");
   print_counts("overwrite");
   CHECK(content_is(TARGET, v2), "overwrite content wrong");
   CHECK(!exists(ASIDE) && !exists(TEMP),
         "overwrite left a file beside the target");
   CHECK(n_remove_missing == 0,
         "overwrite removed a name that did not exist");
#ifdef RENAME_REPLACES
   /* and one for the aside name */
   CHECK(n_stat == 2 && n_rename == 2 && n_remove == 1,
         "overwrite took more than two stats, two renames and one remove");
#else
   CHECK(n_stat == 1 && n_rename == 2 && n_remove == 1,
         "overwrite took more than one stat, two renames and one remove");
#endif

   /* leftover aside from an interrupted run */
   put(ASIDE, "stale");
   reset_counts();
   CHECK(filestream_write_file_atomic(TARGET, v3, strlen(v3)),
         "overwrite with a leftover aside failed");
   print_counts("leftover");
   CHECK(content_is(TARGET, v3), "leftover lane content wrong");
   CHECK(content_is(ASIDE, "stale"), "the leftover aside was not kept");
   CHECK(!exists(ASIDE2) && !exists(TEMP),
         "leftover lane left a file beside the target");
   __real_remove(ASIDE);

   /* refused: the replacement cannot be renamed into place */
   refuse_temp = true;
   reset_counts();
   CHECK(!filestream_write_file_atomic(TARGET, v1, strlen(v1)),
         "a refused replacement reported success");
   refuse_temp = false;
   print_counts("refused");
   CHECK(content_is(TARGET, v3), "a refused replacement lost the old file");
   CHECK(!exists(ASIDE) && !exists(TEMP),
         "a refused replacement left a file beside the target");

   /* interrupted: cut short between "target -> .old" and "temp ->
    * target"; the game then starts empty and saves twice */
   __real_rename(TARGET, ASIDE);
   reset_counts();
   CHECK(filestream_write_file_atomic(TARGET, v1, strlen(v1)),
         "the first write after an interrupted replace failed");
   CHECK(filestream_write_file_atomic(TARGET, v2, strlen(v2)),
         "the second write after an interrupted replace failed");
   print_counts("interrupted");
   CHECK(content_is(TARGET, v2), "interrupted lane content wrong");
   CHECK(content_is(ASIDE, v3), "the last good copy at .old was lost");
   CHECK(!exists(ASIDE2) && !exists(TEMP),
         "interrupted lane left a file beside the target");
   __real_remove(ASIDE);

   /* user_old */
   put(ASIDE, "mine");
   put(ASIDE2, "mine too");
   reset_counts();
   CHECK(filestream_write_file_atomic(TARGET, v1, strlen(v1)),
         "overwrite beside the user's .old copies failed");
   print_counts("user_old");
   CHECK(content_is(TARGET, v1), "user_old lane content wrong");
   CHECK(content_is(ASIDE, "mine") && content_is(ASIDE2, "mine too"),
         "a user's own .old copy was not kept");
   CHECK(!exists(ASIDE3) && !exists(TEMP),
         "user_old lane left a file beside the target");
   for (n = 3; n <= 99; n++)
   {
      aside_name(name, n);
      put(name, "mine");
   }
   CHECK(!filestream_write_file_atomic(TARGET, v2, strlen(v2)),
         "a replace with every aside name taken reported success");
   CHECK(content_is(TARGET, v1),
         "a replace with every aside name taken lost the target");
   CHECK(content_is(ASIDE, "mine") && content_is(name, "mine")
         && !exists(TEMP),
         "a replace with every aside name taken changed a file beside it");
   for (n = 1; n <= 99; n++)
   {
      aside_name(name, n);
      __real_remove(name);
   }

   /* restore: the source is named as the aside would be */
   put(TARGET, "current");
   put(ASIDE, "backup");
   reset_counts();
   CHECK(retro_vfs_file_rename_impl(ASIDE, TARGET) == 0,
         "restoring a .old file failed");
   print_counts("restore");
   CHECK(content_is(TARGET, "backup"), "restore lane content wrong");
   CHECK(!exists(ASIDE) && !exists(ASIDE2),
         "restore lane left a file beside the target");

   /* restore_case */
   put(TARGET, "current");
   put(ASIDE, "stale");
   put(UPPER, "backup");
   CHECK(retro_vfs_file_rename_impl(UPPER, TARGET) == 0,
         "restoring a .OLD file failed");
   CHECK(content_is(TARGET, "backup"), "restore_case lane content wrong");
   CHECK(content_is(ASIDE, "stale"), "restore_case lane touched the aside");
   CHECK(!exists(UPPER) && !exists(ASIDE2),
         "restore_case lane left a file beside the target");
   printf("[restore_case] restored, the leftover aside kept\n");
   __real_remove(ASIDE);

   /* frontend */
   install_frontend_vfs();
   put(TARGET, v1);
   CHECK(filestream_write_file_atomic(TARGET, v2, strlen(v2)),
         "overwrite under a refusing frontend rename failed");
   CHECK(content_is(TARGET, v2), "frontend lane content wrong");
   CHECK(!exists(TEMP), "frontend lane left the temporary behind");
   printf("[frontend] replaced through a refusing rename\n");

   clean();

   if (failures)
   {
      fprintf(stderr, "%u check(s) failed\n", failures);
      return 1;
   }
   printf("[pass] vfs_rename_replace_test\n");
   return 0;
}
