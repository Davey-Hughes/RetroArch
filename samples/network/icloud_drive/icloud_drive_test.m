/* Harness for the iCloud Drive cloud sync driver.
 *
 * Cloud sync counts its outstanding requests and waits for every one
 * to report back, so a driver must call the completion handler exactly
 * once per request.  The iCloud Drive driver broke that both ways:
 *
 * - a read whose local destination could not be created never called
 *   it at all, so the sync waited forever;
 * - a read whose cloud file existed but could not be loaded (a
 *   directory, an unreadable file), and an upload whose local file
 *   could not be read, reported failure and then carried on, reporting
 *   again - a second decrement of the outstanding count and a second
 *   use of state the first report had already released.
 *
 * It also treated an unavailable iCloud container as "file not
 * present", which cloud sync takes as the server not having the file
 * and a delete as done.  Without a container nothing was checked, so
 * those are failures.
 *
 * The container is redirected to a temporary directory by overriding
 * URLForUbiquityContainerIdentifier: in a category, so the driver's
 * own code runs unchanged; a nil answer stands for "iCloud
 * unavailable".  Each request waits for its first report and then a
 * little longer, to catch a second one.
 *
 * Includes the driver's translation unit so it is tested as shipped.
 */

#import <Foundation/Foundation.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#include <streams/file_stream.h>

#include "../../../configuration.h"

/* The driver's clock: the real one unless a check fixes it, so that
 * two keeps can fall in the same second. */
static time_t fixed_now = 0;

static time_t test_time(time_t *t)
{
   time_t now = fixed_now ? fixed_now : time(t);
   if (t)
      *t = now;
   return now;
}

#define time(t) test_time(t)
#include "../../../network/cloud_sync/icloud_drive.m"
#undef time

void RARCH_LOG(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
}

void RARCH_WARN(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   printf("WARN: ");
   vprintf(fmt, ap);
   va_end(ap);
}

void RARCH_ERR(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   printf("ERR: ");
   vprintf(fmt, ap);
   va_end(ap);
}

void RARCH_DBG(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
}

settings_t *config_get_ptr(void)
{
   static settings_t settings;
   return &settings;
}

static NSURL *test_container = nil;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"
@implementation NSFileManager (ICloudDriveTest)
- (NSURL *)URLForUbiquityContainerIdentifier:(NSString *)identifier
{
   (void)identifier;
   return test_container;
}
@end
#pragma clang diagnostic pop

static unsigned failures = 0;

#define CHECK(cond, ...) \
   do { \
      if (!(cond)) \
      { \
         printf("FAIL %s:%d: ", __FILE__, __LINE__); \
         printf(__VA_ARGS__); \
         printf("\n"); \
         failures++; \
      } \
   } while (0)

typedef struct
{
   volatile int calls;
   volatile int success;
   RFILE *volatile file;
} report_t;

static void on_report(void *user_data, const char *path, bool success,
      RFILE *file)
{
   report_t *r = (report_t*)user_data;
   (void)path;
   r->success = success;
   r->file    = file;
   __sync_synchronize();
   __sync_fetch_and_add(&r->calls, 1);
}

/* Up to 5 s for the first report, then 500 ms more for a second. */
static void wait_reports(report_t *r)
{
   int i;
   for (i = 0; i < 500 && !__sync_fetch_and_add(&r->calls, 0); i++)
      usleep(10000);
   usleep(500000);
   __sync_synchronize();
}

static char root[256];
static char docs[300];

static void path_in(char *s, size_t len, const char *base, const char *rel)
{
   snprintf(s, len, "%s/%s", base, rel);
}

static void write_file(const char *path, const char *text)
{
   FILE *f = fopen(path, "wb");
   if (f)
   {
      fputs(text, f);
      fclose(f);
   }
}

static bool file_is(const char *path, const char *text)
{
   char  buf[128] = {0};
   FILE *f        = fopen(path, "rb");
   if (!f)
      return false;
   fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   return !strcmp(buf, text);
}

static bool rfile_is(RFILE *f, const char *text)
{
   char buf[128] = {0};
   filestream_read(f, buf, sizeof(buf) - 1);
   return !strcmp(buf, text);
}

static void check_once(const char *what, report_t *r, bool success)
{
   CHECK(r->calls == 1, "%s: must report once, reported %d time(s)%s",
         what, r->calls, r->calls ? "" : " (the sync would wait forever)");
   if (r->calls >= 1)
      CHECK((bool)r->success == success, "%s: expected %s",
            what, success ? "success" : "failure");
}

static void test_read_present(void)
{
   report_t r = {0};
   char local[320];
   path_in(local, sizeof(local), root, "local_a.srm");

   icloud_drive_read("saves/a.srm", local, on_report, &r);
   wait_reports(&r);
   check_once("read present", &r, true);
   CHECK(r.file && rfile_is(r.file, "cloud-copy"),
         "read present: must hand back the cloud file's contents");
   if (r.file)
      filestream_close(r.file);
}

static void test_read_missing(void)
{
   report_t r = {0};
   char local[320];
   path_in(local, sizeof(local), root, "local_missing.srm");

   icloud_drive_read("saves/missing.srm", local, on_report, &r);
   wait_reports(&r);
   check_once("read missing", &r, true);
   CHECK(!r.file, "read missing: must hand back no file");
}

static void test_read_unwritable_local(void)
{
   report_t r = {0};
   char local[320];
   path_in(local, sizeof(local), root, "no/such/dir/local_a.srm");

   icloud_drive_read("saves/a.srm", local, on_report, &r);
   wait_reports(&r);
   check_once("read, local file cannot be created", &r, false);
   if (r.file)
      filestream_close(r.file);
}

static void test_read_unloadable_cloud_file(void)
{
   report_t r = {0};
   char local[320];
   path_in(local, sizeof(local), root, "local_dir.srm");

   /* saves/dir.srm is a directory: it exists, but has no contents. */
   icloud_drive_read("saves/dir.srm", local, on_report, &r);
   wait_reports(&r);
   check_once("read, cloud file cannot be loaded", &r, false);
   if (r.file)
      filestream_close(r.file);
}

static void test_update(void)
{
   report_t r = {0};
   char local[320], cloud[320];
   RFILE *rfile;

   path_in(local, sizeof(local), root, "upload.srm");
   path_in(cloud, sizeof(cloud), docs, "saves/new/upload.srm");
   write_file(local, "local-copy");
   rfile = filestream_open(local, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   icloud_drive_update("saves/new/upload.srm", rfile, on_report, &r);
   wait_reports(&r);
   check_once("update", &r, true);
   CHECK(file_is(cloud, "local-copy"),
         "update: cloud file must hold the upload");
   filestream_close(rfile);
}

static void test_update_unreadable_local(void)
{
   report_t r = {0};
   char local[320];
   RFILE *rfile;

   path_in(local, sizeof(local), root, "vanished.srm");
   write_file(local, "gone");
   rfile = filestream_open(local, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   remove(local);

   icloud_drive_update("saves/vanished.srm", rfile, on_report, &r);
   wait_reports(&r);
   check_once("update, local file cannot be read", &r, false);
   filestream_close(rfile);
}

static void test_delete(void)
{
   report_t r = {0};
   char cloud[320];
   path_in(cloud, sizeof(cloud), docs, "saves/old.srm");
   write_file(cloud, "old");

   icloud_drive_delete("saves/old.srm", on_report, &r);
   wait_reports(&r);
   check_once("delete", &r, true);
   CHECK(access(cloud, F_OK) != 0, "delete: cloud file must be gone");
}

/* A copy of @file kept as <docs>/deleted/<dir>/<file>-<yymmdd-hhmmss>
 * holds @text */
static bool kept_holds(const char *dir, const char *file, const char *text)
{
   char           kept[320], path[640];
   size_t         len   = strlen(file);
   bool           found = false;
   struct dirent *de;
   DIR           *d;

   snprintf(kept, sizeof(kept), "%s/deleted/%s", docs, dir);
   if (!(d = opendir(kept)))
      return false;
   while (!found && (de = readdir(d)))
      if (     !strncmp(de->d_name, file, len)
            && de->d_name[len] == '-'
            && strlen(de->d_name + len) == 14)
      {
         snprintf(path, sizeof(path), "%s/%s", kept, de->d_name);
         found = file_is(path, text);
      }
   closedir(d);
   return found;
}

/* With destructive sync off, the server's copy is kept before an upload
 * replaces it or a delete removes it, where WebDAV keeps it; with it
 * on, nothing is kept. */
static void test_backup(void)
{
   report_t ru = {0}, rd = {0}, rx = {0};
   char local[320], cloud[320];
   RFILE *rfile;

   config_get_ptr()->bools.cloud_sync_destructive = false;
   path_in(local, sizeof(local), root, "keep.srm");
   path_in(cloud, sizeof(cloud), docs, "saves/kb.srm");
   write_file(cloud, "server-old");
   write_file(local, "local-new");
   rfile = filestream_open(local, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   icloud_drive_update("saves/kb.srm", rfile, on_report, &ru);
   wait_reports(&ru);
   check_once("non-destructive update", &ru, true);
   CHECK(file_is(cloud, "local-new"),
         "non-destructive update must put the new data in place");
   CHECK(kept_holds("saves", "kb.srm", "server-old"),
         "non-destructive update must keep the old data as deleted/saves/kb.srm-<yymmdd-hhmmss>");
   filestream_close(rfile);

   path_in(cloud, sizeof(cloud), docs, "saves/kc.srm");
   write_file(cloud, "server-c");
   icloud_drive_delete("saves/kc.srm", on_report, &rd);
   wait_reports(&rd);
   check_once("non-destructive delete", &rd, true);
   CHECK(access(cloud, F_OK) != 0,
         "non-destructive delete: cloud file must be gone");
   CHECK(kept_holds("saves", "kc.srm", "server-c"),
         "non-destructive delete must keep the data as deleted/saves/kc.srm-<yymmdd-hhmmss>");

   config_get_ptr()->bools.cloud_sync_destructive = true;
   path_in(cloud, sizeof(cloud), docs, "saves/kd.srm");
   write_file(cloud, "server-d");
   icloud_drive_delete("saves/kd.srm", on_report, &rx);
   wait_reports(&rx);
   check_once("destructive delete", &rx, true);
   CHECK(access(cloud, F_OK) != 0,
         "destructive delete: cloud file must be gone");
   CHECK(!kept_holds("saves", "kd.srm", "server-d"),
         "destructive delete must keep nothing");
   config_get_ptr()->bools.cloud_sync_destructive = false;
}

/* With destructive sync on, an upload keeps nothing; the server
 * manifest is never kept. */
static void test_backup_not_kept(void)
{
   report_t ru = {0}, rm = {0};
   char local[320], cloud[320];
   RFILE *rfile;

   path_in(local, sizeof(local), root, "keep_f.srm");
   write_file(local, "local-f");
   rfile = filestream_open(local, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   config_get_ptr()->bools.cloud_sync_destructive = true;
   path_in(cloud, sizeof(cloud), docs, "saves/kf.srm");
   write_file(cloud, "server-f");
   icloud_drive_update("saves/kf.srm", rfile, on_report, &ru);
   wait_reports(&ru);
   check_once("destructive update", &ru, true);
   CHECK(file_is(cloud, "local-f"),
         "destructive update must put the new data in place");
   CHECK(!kept_holds("saves", "kf.srm", "server-f"),
         "destructive update must keep nothing");

   config_get_ptr()->bools.cloud_sync_destructive = false;
   path_in(cloud, sizeof(cloud), docs, CLOUD_SYNC_SERVER_MANIFEST);
   write_file(cloud, "server-m");
   icloud_drive_update(CLOUD_SYNC_SERVER_MANIFEST, rfile, on_report, &rm);
   wait_reports(&rm);
   check_once("manifest update", &rm, true);
   CHECK(file_is(cloud, "local-f"),
         "manifest update must put the new data in place");
   CHECK(!kept_holds(".", CLOUD_SYNC_SERVER_MANIFEST, "server-m"),
         "the server manifest must never be kept");
   filestream_close(rfile);
}

/* A non-ASCII name is kept under that name, not one read in another
 * encoding. */
static void test_backup_utf8(void)
{
   static const char name[] = "\xe6\x97\xa5\xe6\x9c\xac.srm"; /* UTF-8 */
   report_t r = {0};
   char rel[64], cloud[320];

   config_get_ptr()->bools.cloud_sync_destructive = false;
   snprintf(rel, sizeof(rel), "saves/%s", name);
   path_in(cloud, sizeof(cloud), docs, rel);
   write_file(cloud, "server-j");
   icloud_drive_delete(rel, on_report, &r);
   wait_reports(&r);
   check_once("non-destructive delete, UTF-8 name", &r, true);
   CHECK(kept_holds("saves", name, "server-j"),
         "a UTF-8 name must be kept under the same name in deleted/saves");
}

/* Two keeps of one file in the same second both succeed; each replaces
 * the copy kept before it, as WebDAV's COPY does. */
static void test_backup_same_second(void)
{
   report_t r1 = {0}, r2 = {0}, rd = {0};
   char local[320], cloud[320];
   RFILE *rfile;

   config_get_ptr()->bools.cloud_sync_destructive = false;
   fixed_now = time(NULL);
   path_in(cloud, sizeof(cloud), docs, "saves/kg.srm");
   write_file(cloud, "server-g");
   path_in(local, sizeof(local), root, "keep_g.srm");
   write_file(local, "local-g1");
   rfile = filestream_open(local, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   icloud_drive_update("saves/kg.srm", rfile, on_report, &r1);
   wait_reports(&r1);
   check_once("update, first keep in a second", &r1, true);

   write_file(local, "local-g2");
   icloud_drive_update("saves/kg.srm", rfile, on_report, &r2);
   wait_reports(&r2);
   check_once("update, second keep in the same second", &r2, true);
   CHECK(file_is(cloud, "local-g2"),
         "a second update in the same second must put the new data in place");
   CHECK(    kept_holds("saves", "kg.srm", "local-g1")
         && !kept_holds("saves", "kg.srm", "server-g"),
         "a second update in the same second must replace the copy kept before it");
   filestream_close(rfile);

   icloud_drive_delete("saves/kg.srm", on_report, &rd);
   wait_reports(&rd);
   check_once("delete, third keep in the same second", &rd, true);
   CHECK(access(cloud, F_OK) != 0,
         "a delete in the same second: cloud file must be gone");
   CHECK(kept_holds("saves", "kg.srm", "local-g2"),
         "a delete in the same second must replace the copy kept before it");
   fixed_now = 0;
}

/* A copy that cannot be kept stops the upload and the delete:
 * deleted/sb is a file, so deleted/sb/ cannot be made. */
static void test_backup_fails(void)
{
   report_t ru = {0}, rd = {0};
   char local[320], cloud[320], dir[320];
   RFILE *rfile;

   config_get_ptr()->bools.cloud_sync_destructive = false;
   path_in(dir, sizeof(dir), docs, "sb");
   mkdir(dir, 0755);
   path_in(dir, sizeof(dir), docs, "deleted");
   mkdir(dir, 0755);
   path_in(dir, sizeof(dir), docs, "deleted/sb");
   write_file(dir, "not a folder");
   path_in(cloud, sizeof(cloud), docs, "sb/ke.srm");
   write_file(cloud, "server-e");
   path_in(local, sizeof(local), root, "keep_e.srm");
   write_file(local, "local-e");
   rfile = filestream_open(local, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   icloud_drive_update("sb/ke.srm", rfile, on_report, &ru);
   wait_reports(&ru);
   check_once("update, copy cannot be kept", &ru, false);
   CHECK(file_is(cloud, "server-e"),
         "an update whose copy cannot be kept must upload nothing");
   filestream_close(rfile);

   icloud_drive_delete("sb/ke.srm", on_report, &rd);
   wait_reports(&rd);
   check_once("delete, copy cannot be kept", &rd, false);
   CHECK(file_is(cloud, "server-e"),
         "a delete whose copy cannot be kept must leave the file");
}

static void test_no_container(void)
{
   report_t rr = {0}, ru = {0}, rd = {0};
   char local[320];
   RFILE *rfile;
   NSURL *saved = test_container;

   path_in(local, sizeof(local), root, "upload.srm");
   write_file(local, "local-copy");
   rfile = filestream_open(local, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   test_container = nil;
   path_in(local, sizeof(local), root, "local_nc.srm");
   icloud_drive_read("saves/a.srm", local, on_report, &rr);
   wait_reports(&rr);
   check_once("read, no iCloud container", &rr, false);
   if (rr.file)
      filestream_close(rr.file);

   icloud_drive_update("saves/a.srm", rfile, on_report, &ru);
   wait_reports(&ru);
   check_once("update, no iCloud container", &ru, false);

   icloud_drive_delete("saves/a.srm", on_report, &rd);
   wait_reports(&rd);
   check_once("delete, no iCloud container", &rd, false);

   test_container = saved;
   filestream_close(rfile);
}

int main(void)
{
   @autoreleasepool
   {
      char dir[320];
      char cmd[400];

      snprintf(root, sizeof(root), "/tmp/icloud_drive_test_%ld",
            (long)getpid());
      snprintf(docs, sizeof(docs), "%s/container/Documents", root);
      snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
      system(cmd);
      path_in(dir, sizeof(dir), docs, "saves/dir.srm");
      [[NSFileManager defaultManager]
            createDirectoryAtPath:[NSString stringWithUTF8String:dir]
            withIntermediateDirectories:YES attributes:nil error:NULL];
      path_in(dir, sizeof(dir), docs, "saves/a.srm");
      write_file(dir, "cloud-copy");

      snprintf(dir, sizeof(dir), "%s/container", root);
      test_container = [NSURL fileURLWithPath:
            [NSString stringWithUTF8String:dir] isDirectory:YES];

      test_read_present();
      test_read_missing();
      test_read_unwritable_local();
      test_read_unloadable_cloud_file();
      test_update();
      test_update_unreadable_local();
      test_delete();
      test_backup();
      test_backup_not_kept();
      test_backup_utf8();
      test_backup_same_second();
      test_backup_fails();
      test_no_container();

      system(cmd);
   }

   if (failures)
   {
      printf("\n%u check(s) failed\n", failures);
      return 1;
   }
   printf("\nall iCloud Drive driver checks passed\n");
   return 0;
}
