/* LD_PRELOAD shim (Linux) for the E2E kill points: the first write
 * into a file opened for writing whose path starts with SAVE_E2E_STALL
 * puts half its bytes on disk, creates SAVE_E2E_STALL_MARK and stalls,
 * so run.py can kill the process in the middle of that write.  With
 * SAVE_E2E_STALL_RELEASE set, the write goes on once that file exists. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static FILE *stalled_fp;
static int   stalled_fd = -1;
static int   done;

static int matches(const char *path)
{
   const char *prefix = getenv("SAVE_E2E_STALL");
   return !done && path && prefix && *prefix
      && !strncmp(path, prefix, strlen(prefix));
}

static void stall(void)
{
   const char *mark    = getenv("SAVE_E2E_STALL_MARK");
   const char *release = getenv("SAVE_E2E_STALL_RELEASE");
   struct timespec ts;
   int fd;
   done = 1;
   if (mark && (fd = open(mark, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0)
      close(fd);
   ts.tv_sec  = 0;
   ts.tv_nsec = 10000000;
   while (!release || !*release || access(release, F_OK) != 0)
      nanosleep(&ts, NULL);
}

static FILE *open_stream(const char *sym, const char *path, const char *mode)
{
   FILE *(*real)(const char*, const char*) =
      (FILE *(*)(const char*, const char*))dlsym(RTLD_NEXT, sym);
   FILE *fp = real(path, mode);
   if (fp && strpbrk(mode, "wa+") && matches(path))
      stalled_fp = fp;
   return fp;
}

FILE *fopen(const char *path, const char *mode)
{
   return open_stream("fopen", path, mode);
}

FILE *fopen64(const char *path, const char *mode)
{
   return open_stream("fopen64", path, mode);
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *fp)
{
   size_t (*real)(const void*, size_t, size_t, FILE*) =
      (size_t (*)(const void*, size_t, size_t, FILE*))dlsym(RTLD_NEXT, "fwrite");
   if (!done && stalled_fp && fp == stalled_fp && size * n > 1)
   {
      size_t half = size * n / 2;
      real(p, 1, half, fp);
      fflush(fp);
      stall();
      return real((const char*)p + half, 1, size * n - half, fp)
         == size * n - half ? n : 0;
   }
   return real(p, size, n, fp);
}

static int open_fd(const char *sym, const char *path, int flags, mode_t mode)
{
   int (*real)(const char*, int, ...) =
      (int (*)(const char*, int, ...))dlsym(RTLD_NEXT, sym);
   int fd = real(path, flags, mode);
   if (fd >= 0 && (flags & (O_WRONLY | O_RDWR)) && matches(path))
      stalled_fd = fd;
   return fd;
}

int open(const char *path, int flags, ...)
{
   mode_t mode = 0;
   if (flags & O_CREAT)
   {
      va_list ap;
      va_start(ap, flags);
      mode = (mode_t)va_arg(ap, int);
      va_end(ap);
   }
   return open_fd("open", path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
   mode_t mode = 0;
   if (flags & O_CREAT)
   {
      va_list ap;
      va_start(ap, flags);
      mode = (mode_t)va_arg(ap, int);
      va_end(ap);
   }
   return open_fd("open64", path, flags, mode);
}

ssize_t write(int fd, const void *p, size_t len)
{
   ssize_t (*real)(int, const void*, size_t) =
      (ssize_t (*)(int, const void*, size_t))dlsym(RTLD_NEXT, "write");
   if (!done && fd >= 0 && fd == stalled_fd && len > 1)
   {
      ssize_t ret = real(fd, p, len / 2);
      stall();
      if (ret != (ssize_t)(len / 2))
         return ret;
      return (ret = real(fd, (const char*)p + len / 2, len - len / 2)) < 0
         ? ret : (ssize_t)(len / 2) + ret;
   }
   return real(fd, p, len);
}
