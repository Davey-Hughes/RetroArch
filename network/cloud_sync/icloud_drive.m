#include <time.h>

#include "../cloud_sync_driver.h"
#include "../../configuration.h"
#include "../../verbosity.h"

#define ICLOUD_CONTAINER_NAME @"iCloud.com.libretro.dist.RetroArch"
#define DOCUMENTS_BASE_FOLDER @"Documents"

static NSURL *icloud_base_url(void)
{
   NSURL *base_url = [[NSFileManager defaultManager] URLForUbiquityContainerIdentifier:ICLOUD_CONTAINER_NAME];
   return [base_url URLByAppendingPathComponent: DOCUMENTS_BASE_FOLDER];
}

static NSURL *icloud_file_url(const char *path)
{
   NSURL *base = icloud_base_url();
   if (!base)
      return nil;

   NSString *relative_path = [NSString stringWithUTF8String:path];
   return [base URLByAppendingPathComponent:relative_path];
}

/* With destructive sync off, the server's copy of a file is kept, as
 * the other drivers keep it, at deleted/<path>-<yymmdd-hhmmss>: a
 * delete moves it there, and an upload that replaces it copies it there
 * first.  The server manifest is rewritten every sync and is not kept. */
static bool icloud_drive_wants_backup(const char *path)
{
   settings_t *settings = config_get_ptr();
   return settings
       && !settings->bools.cloud_sync_destructive
       && strcmp(path, CLOUD_SYNC_SERVER_MANIFEST) != 0;
}

static NSURL *icloud_drive_backup_url(const char *path)
{
   char      ts[32];
   time_t    now = time(NULL);
   struct tm tm_;

   localtime_r(&now, &tm_);
   strftime(ts, sizeof(ts), "-%y%m%d-%H%M%S", &tm_);
   return icloud_file_url([[NSString stringWithFormat:@"deleted/%@%s",
         [NSString stringWithUTF8String:path], ts] UTF8String]);
}

/* Moves, or copies, the file at @url to deleted/@path-<yymmdd-hhmmss>,
 * replacing a copy kept there in the same second, as WebDAV's COPY
 * does; nothing there is nothing to keep.  Called off the main thread,
 * as it looks up the container. */
static bool icloud_drive_backup(NSURL *url, const char *path, bool copy)
{
   NSFileManager *fm    = [NSFileManager defaultManager];
   NSError       *error = nil;
   NSURL         *backup;

   if (![fm fileExistsAtPath:url.path])
      return true;
   if (!(backup = icloud_drive_backup_url(path)))
   {
      RARCH_DBG("[iCloudDrive] iCloud is not available for %s\n", path);
      return false;
   }
   if (     ![fm createDirectoryAtURL:[backup URLByDeletingLastPathComponent]
               withIntermediateDirectories:YES attributes:nil error:&error]
         || (     [fm fileExistsAtPath:backup.path]
               && ![fm removeItemAtURL:backup error:&error])
         || !(copy
               ? [fm copyItemAtURL:url toURL:backup error:&error]
               : [fm moveItemAtURL:url toURL:backup error:&error]))
   {
      RARCH_DBG("[iCloudDrive] Could not keep %s: %s\n",
            [url.path UTF8String], [[error debugDescription] UTF8String]);
      return false;
   }
   return true;
}

static bool icloud_drive_sync_begin(cloud_sync_complete_handler_t cb, void *user_data)
{
   BOOL available = [[NSFileManager defaultManager] URLForUbiquityContainerIdentifier:ICLOUD_CONTAINER_NAME] != nil;
   cb(user_data, NULL, available, NULL);
   return true;
}

static bool icloud_drive_sync_end(cloud_sync_complete_handler_t cb, void *user_data)
{
   cb(user_data, NULL, true, NULL);
   return true;
}

/* Cloud sync counts its outstanding requests and waits for each one
 * to report, so every request below calls @cb exactly once, on every
 * path.  Without a container nothing could be checked, so that is a
 * failure too - never "not present", which cloud sync reads as the
 * server not having the file, or "deleted". */

static bool icloud_drive_read(const char *p, const char *f, cloud_sync_complete_handler_t cb, void *user_data)
{
    char *path = strdup(p);
    char *file = strdup(f);

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        NSURL  *url   = icloud_file_url(path);
        RFILE  *rfile = NULL;
        bool    ok    = false;

        if (!url)
            RARCH_DBG("[iCloudDrive] iCloud is not available for %s\n", path);
        else if (![[NSFileManager defaultManager] fileExistsAtPath: url.path])
        {
            RARCH_DBG("[iCloudDrive] File %s is not present\n", path);
            ok = true;
        }
        else
        {
            NSData *data = [NSData dataWithContentsOfURL:url];

            if (!data)
                RARCH_DBG("[iCloudDrive] Could not retrieve data for %s\n", path);
            else if (!(rfile = filestream_open(file,
                        RETRO_VFS_FILE_ACCESS_READ_WRITE,
                        RETRO_VFS_FILE_ACCESS_HINT_SEQUENTIAL_BULK)))
                RARCH_DBG("[iCloudDrive] Could not create RFILE for %s\n", path);
            else if (filestream_write(rfile, [data bytes], [data length])
                    != (int64_t)[data length])
            {
                RARCH_DBG("[iCloudDrive] Could not write %s\n", file);
                filestream_close(rfile);
                rfile = NULL;
            }
            else
            {
                filestream_seek(rfile, 0, SEEK_SET);
                ok = true;
            }
        }

        cb(user_data, path, ok, rfile);
        free(path);
        free(file);
    });
    return true;
}


static bool icloud_drive_update(const char *p, RFILE *rfile, cloud_sync_complete_handler_t cb, void *user_data)
{
    char     *path          = strdup(p);
    NSURL    *url           = icloud_file_url(path);
    bool      keep          = icloud_drive_wants_backup(path);
    NSURL    *directory_url = [url URLByDeletingLastPathComponent];
    NSString *file_string   = [NSString stringWithUTF8String:filestream_get_path(rfile)];
    NSData   *local_data    = [NSData dataWithContentsOfFile:file_string];

    if (!url || !local_data)
    {
        if (!url)
            RARCH_DBG("[iCloudDrive] iCloud is not available for %s\n", path);
        else
            RARCH_DBG("[iCloudDrive] Failed to read local file: %s\n", [file_string UTF8String]);
        cb(user_data, path, false, rfile);
        free(path);
        return true;
    }

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        NSError *error = nil;
        NSError *directory_error = nil;
        BOOL directory_is_present;

        if (keep && !icloud_drive_backup(url, path, true))
        {
            RARCH_DBG("[iCloudDrive] Not uploading %s: the copy on the server could not be kept\n", path);
            cb(user_data, path, false, rfile);
            free(path);
            return;
        }

        directory_is_present = [[NSFileManager defaultManager] fileExistsAtPath: directory_url.path];

        if (!directory_is_present)
        {
            [[NSFileManager defaultManager] createDirectoryAtPath:directory_url.path withIntermediateDirectories:YES attributes:nil error:&directory_error];
            if (directory_error)
            {
                RARCH_DBG("[iCloudDrive] Failed to create directory: %s\n", [[directory_error debugDescription] UTF8String]);
            }
        }

        [local_data writeToURL:url options:NSDataWritingAtomic error:&error];
        RARCH_DBG("[iCloudDrive] %s writing to %s\n", error == nil ? "succeeded" : "failed", [url.absoluteString UTF8String]);

        if (error)
        {
            RARCH_DBG("[iCloudDrive] error: %s\n", [[error debugDescription] UTF8String]);
        }

        cb(user_data, path, error == nil, rfile);
        free(path);
    });

   return true;
}

static bool icloud_drive_delete(const char *p, cloud_sync_complete_handler_t cb, void *user_data)
{
    char  *path = strdup(p);
    NSURL *url  = icloud_file_url(p);
    bool   keep = icloud_drive_wants_backup(p);

    if (!url)
    {
        RARCH_DBG("[iCloudDrive] iCloud is not available for %s\n", path);
        cb(user_data, path, false, NULL);
        free(path);
        return true;
    }

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        NSError *error = nil;
        bool     ok    = true;
        if (keep)
        {
            ok = icloud_drive_backup(url, path, false);
            RARCH_DBG("[iCloudDrive] delete %s (kept) %s\n", path, ok ? "succeeded" : "failed");
        }
        else if ([[NSFileManager defaultManager] fileExistsAtPath:url.path])
        {
            [[NSFileManager defaultManager] removeItemAtURL:url error:&error];
            RARCH_DBG("[iCloudDrive] delete %s %s\n", path, error == nil ? "succeeded" : "failed");
            if (error)
            {
                RARCH_DBG("[iCloudDrive] error: %s\n", [[error debugDescription] UTF8String]);
            }
            ok = error == nil;
        }
        cb(user_data, path, ok, NULL);
        free(path);
    });
    return true;
}

cloud_sync_driver_t cloud_sync_icloud_drive = {
    icloud_drive_sync_begin,
    icloud_drive_sync_end,
    icloud_drive_read,
    icloud_drive_update,
    icloud_drive_delete,
    "icloud_drive", /* ident */
    0 /* flags */
};