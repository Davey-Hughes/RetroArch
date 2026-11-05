/* Exercise each backend's diagnostic publication without a server. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <compat/strl.h>
#include <retro_atomic.h>
#include <rthreads/rthreads.h>

static size_t plain_copy(char *dst, const char *src, size_t len)
{
   return strlcpy(dst, src, len);
}

static size_t interleaved_copy(char *dst, const char *src, size_t len);
#undef strlcpy
#define strlcpy interleaved_copy
#ifdef TEST_SMB
#include "../../../libretro-common/vfs/vfs_implementation_smb.c"
#define note_error(msg) smb_note_error(msg, NULL)
#define last_error(buf, len) smb_get_last_error(buf, len)
#define BACKEND "SMB"
#else
#include "../../../libretro-common/vfs/vfs_implementation_nfs.c"
#define note_error(msg) nfs_note_error(msg)
#define last_error(buf, len) nfs_get_last_error(buf, len)
#define BACKEND "NFS"
#endif
#undef strlcpy

#define WRITERS 12
#define ROUNDS 2000

static int failures;
static int interleave;
static int saturate;
static retro_atomic_int_t start;
static retro_atomic_int_t finished;
static retro_atomic_int_t bad_messages;

#define CHECK(cond, what) do { if (!(cond)) { \
   fprintf(stderr, "FAIL: %s: %s\n", BACKEND, what); failures++; \
} } while (0)

/* Re-enter publication while the outer writer still owns its copy. */
static size_t interleaved_copy(char *dst, const char *src, size_t len)
{
   if (interleave)
   {
      int mode = interleave;
      unsigned i;
      char snapshot[256];
      char saved[256];
      interleave = 0;
      plain_copy(saved, src, sizeof(saved));
      plain_copy(dst, "unfinished", len);
      if (mode == 1)
         CHECK(!*last_error(snapshot, sizeof(snapshot)),
               "an unfinished first error is not published");
      else
      {
         for (i = 0; i < 16; i++)
            note_error("wrapped");
         if (mode == 2)
            CHECK(!strcmp(dst, "unfinished"),
                  "wraparound does not reuse an unfinished writer's storage");
         else
            CHECK(!strcmp(src, saved), "writers do not reuse a reader's storage");
      }
   }
   if (saturate)
   {
      char snapshot[256];
      int before;
      saturate--;
      before = saturate;
      plain_copy(dst, "unfinished", len);
      note_error("nested error");
      CHECK(!strcmp(dst, "unfinished"), "saturated writers retain their storage");
      if (saturate == before)
         CHECK(!*last_error(snapshot, sizeof(snapshot)),
               "a reader returns immediately when its slot is reserved");
   }
   return plain_copy(dst, src, len);
}

static void writer(void *arg)
{
   unsigned i;
   char msg[101];
   memset(msg, 'A' + (int)(uintptr_t)arg, sizeof(msg) - 1);
   msg[sizeof(msg) - 1] = '\0';
   while (!retro_atomic_load_acquire_int(&start))
      sthread_yield();
   for (i = 0; i < ROUNDS; i++)
      note_error(msg);
   retro_atomic_fetch_add_int(&finished, 1);
}

static void reader(void *arg)
{
   char snapshot[256];
   (void)arg;
   while (!retro_atomic_load_acquire_int(&start))
      sthread_yield();
   do
   {
      const char *msg = last_error(snapshot, sizeof(snapshot));
      size_t i;
      if (!*msg)
         continue;
      if (strlen(msg) != 100 || msg[0] < 'A' || msg[0] >= 'A' + WRITERS)
      {
         retro_atomic_fetch_add_int(&bad_messages, 1);
         continue;
      }
      for (i = 1; i < 100; i++)
         if (msg[i] != msg[0])
         {
            retro_atomic_fetch_add_int(&bad_messages, 1);
            break;
         }
   } while (retro_atomic_load_acquire_int(&finished) < WRITERS);
}

int main(void)
{
   sthread_t *threads[WRITERS + 2];
   char snapshot[256];
   const char *saved;
   unsigned i;

   CHECK(!*last_error(snapshot, sizeof(snapshot)), "initial error is empty");
   interleave = 1;
   note_error("first error");
   CHECK(!strcmp(last_error(snapshot, sizeof(snapshot)), "first error"),
         "completed error is published");
   interleave = 2;
   note_error("outer error");
   CHECK(!strcmp(last_error(snapshot, sizeof(snapshot)), "outer error"),
         "delayed writer publishes its completed error");
   {
      char short_buf[5];
      char untouched = 'X';
      last_error(short_buf, sizeof(short_buf));
      CHECK(!strcmp(short_buf, "oute"), "a short buffer is terminated");
      last_error(&untouched, 0);
      CHECK(untouched == 'X', "a zero-size buffer is untouched");
      last_error(short_buf, 1);
      CHECK(!*short_buf, "a one-byte buffer is terminated");
   }
   saved = last_error(snapshot, sizeof(snapshot));
   for (i = 0; i < 16; i++)
      note_error("later error");
   CHECK(!strcmp(saved, "outer error"), "a retrieved error survives later writes");
   note_error("reader snapshot");
   interleave = 3;
   last_error(snapshot, sizeof(snapshot));
   CHECK(!strcmp(snapshot, "reader snapshot"), "a reader copies its reserved message");
   saturate = 16;
   note_error("saturated error");
   saturate = 0;
   CHECK(!strcmp(last_error(snapshot, sizeof(snapshot)), "saturated error"),
         "publication resumes after saturation");
#ifdef TEST_SMB
   smb_note_error("connect", "reason");
   CHECK(!strcmp(last_error(snapshot, sizeof(snapshot)), "connect: reason"),
         "SMB diagnostics retain the client reason");
   smb_note_error("connect", "");
   CHECK(!strcmp(last_error(snapshot, sizeof(snapshot)), "connect"),
         "an empty client reason adds no separator");
#endif

   {
      char msg[101];
      memset(msg, 'A', sizeof(msg) - 1);
      msg[sizeof(msg) - 1] = '\0';
      note_error(msg);
   }
   for (i = 0; i < WRITERS + 2; i++)
      if (!(threads[i] = sthread_create(i < WRITERS ? writer : reader,
                  (void*)(uintptr_t)i)))
      {
         fprintf(stderr, "FAIL: thread creation\n");
         exit(1);
      }
   retro_atomic_store_release_int(&start, 1);
   for (i = 0; i < WRITERS + 2; i++)
      sthread_join(threads[i]);
   CHECK(!retro_atomic_load_acquire_int(&bad_messages),
         "concurrent readers see only complete messages");
   CHECK(strlen(last_error(snapshot, sizeof(snapshot))) == 100,
         "the final writer leaves a complete diagnostic");
   if (!failures)
      printf("[pass] %s diagnostic publication\n", BACKEND);
   return failures ? 1 : 0;
}
