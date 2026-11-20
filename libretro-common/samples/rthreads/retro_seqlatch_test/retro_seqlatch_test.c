/* rthreads/retro_seqlatch.h: a record one thread publishes while others
 * read it, neither ever waiting on the other.
 *
 * What is asserted:
 *  - a record comes back as it went in, a size that is not a multiple of
 *    int included, the last word zero-padded, and nothing written past
 *    the two copies or past the record's bytes;
 *  - a writer stopped after its odd stamp, part way into copy 0: a read
 *    returns the last whole record, from copy 1, and does not loop;
 *  - a writer stopped after its even stamp, part way into copy 1: a read
 *    returns copy 0;
 *  - one writer, three readers: no copy torn, none going back, each the
 *    update its stamp says (stamp / 2), and the last copy taken after the
 *    writer is done is its last update. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>

#include <rthreads/retro_seqlatch.h>

#define WORDS   16
#define UPDATES 1000000
#define READERS 3

#define WHOLE_LEN (8 * sizeof(int))
#define ODD_LEN   (2 * sizeof(int) + 3)
#define MAX_WORDS RETRO_SEQLATCH_WORDS(WHOLE_LEN)
#define GUARD     0x7EADBEEF
#define GARBAGE   0x5A5A5A5A

static unsigned failures;
#define CHECK(cond, ...) \
   do { if (!(cond)) { printf("FAIL line %d: ", __LINE__); \
        printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

typedef struct
{
   retro_atomic_int_t seq;
   /* The two copies, then guard words no call may write. */
   retro_atomic_int_t words[2 * MAX_WORDS + 2];
} latch_t;

#define LATCH_WORDS (sizeof(((latch_t*)0)->words) / sizeof(retro_atomic_int_t))

static void latch_init(latch_t *l)
{
   unsigned i;
   retro_atomic_int_init(&l->seq, 0);
   for (i = 0; i < LATCH_WORDS; i++)
      retro_atomic_int_init(&l->words[i], GUARD);
}

static void check_guards(latch_t *l, size_t len, const char *name)
{
   size_t i;
   for (i = 2 * RETRO_SEQLATCH_WORDS(len); i < LATCH_WORDS; i++)
      CHECK(retro_atomic_load_relaxed_int(&l->words[i]) == GUARD,
            "%s: word %u past the two copies written", name, (unsigned)i);
}

static void fill(unsigned char *rec, size_t len, unsigned seed)
{
   size_t i;
   for (i = 0; i < len; i++)
      rec[i] = (unsigned char)(seed + i * 7);
}

static void garble(retro_atomic_int_t *copy, size_t n)
{
   size_t i;
   for (i = 0; i < n; i++)
      retro_atomic_store_relaxed_int(&copy[i], GARBAGE);
}

static void start_thread(pthread_t *thread, void *(*fn)(void*), void *arg,
      const char *name)
{
   if (pthread_create(thread, NULL, fn, arg) != 0)
   {
      printf("FAIL: %s: no thread\n", name);
      exit(1);
   }
}

typedef struct
{
   latch_t *latch;
   void *dst;
   size_t len;
   int stamp;
   retro_atomic_int_t done;
} read_job_t;

static void *read_main(void *arg)
{
   read_job_t *job = (read_job_t*)arg;
   job->stamp = retro_seqlatch_read(&job->latch->seq, job->latch->words,
         job->dst, job->len);
   retro_atomic_store_release_int(&job->done, 1);
   return NULL;
}

/* A read on a thread of its own, given 10 s: one that waits for a
 * stopped writer never returns. */
static int read_bounded(latch_t *l, void *dst, size_t len, const char *name)
{
   pthread_t thread;
   read_job_t job;
   time_t deadline = time(NULL) + 10;

   job.latch = l;
   job.dst   = dst;
   job.len   = len;
   job.stamp = 0;
   retro_atomic_int_init(&job.done, 0);
   start_thread(&thread, read_main, &job, name);
   while (!retro_atomic_load_acquire_int(&job.done))
   {
      if (time(NULL) > deadline)
      {
         printf("FAIL: %s: the read is still looping after 10 s\n", name);
         exit(1); /* the reader cannot be joined */
      }
      sched_yield();
   }
   pthread_join(thread, NULL);
   return job.stamp;
}

static void test_round_trip(void)
{
   latch_t l;
   unsigned char rec[WHOLE_LEN], out[WHOLE_LEN];
   int stamp;

   latch_init(&l);
   fill(rec, sizeof(rec), 1);
   retro_seqlatch_publish(&l.seq, l.words, rec, sizeof(rec));
   memset(out, 0, sizeof(out));
   stamp = retro_seqlatch_read(&l.seq, l.words, out, sizeof(out));
   CHECK(!memcmp(out, rec, sizeof(rec)), "round trip: first record differs");
   CHECK(stamp == 2, "round trip: stamp %d after one publish, not 2", stamp);

   fill(rec, sizeof(rec), 100);
   retro_seqlatch_publish(&l.seq, l.words, rec, sizeof(rec));
   memset(out, 0, sizeof(out));
   stamp = retro_seqlatch_read(&l.seq, l.words, out, sizeof(out));
   CHECK(!memcmp(out, rec, sizeof(rec)), "round trip: second record differs");
   CHECK(stamp == 4, "round trip: stamp %d after two publishes, not 4", stamp);
   check_guards(&l, sizeof(rec), "round trip");
}

static void test_odd_size(void)
{
   latch_t l;
   unsigned char rec[ODD_LEN], out[ODD_LEN + sizeof(int)];
   size_t n    = RETRO_SEQLATCH_WORDS(ODD_LEN);
   size_t tail = ODD_LEN - (n - 1) * sizeof(int);
   int last    = 0;
   size_t i;

   latch_init(&l);
   fill(rec, sizeof(rec), 3);
   retro_seqlatch_publish(&l.seq, l.words, rec, sizeof(rec));
   memcpy(&last, rec + (n - 1) * sizeof(int), tail);
   CHECK(retro_atomic_load_relaxed_int(&l.words[n - 1]) == last,
         "odd size: copy 0's last word is not the tail zero-padded");
   CHECK(retro_atomic_load_relaxed_int(&l.words[2 * n - 1]) == last,
         "odd size: copy 1's last word is not the tail zero-padded");
   check_guards(&l, sizeof(rec), "odd size");

   memset(out, 0xEE, sizeof(out));
   retro_seqlatch_read(&l.seq, l.words, out, ODD_LEN);
   CHECK(!memcmp(out, rec, ODD_LEN), "odd size: record differs");
   for (i = ODD_LEN; i < sizeof(out); i++)
      CHECK(out[i] == 0xEE, "odd size: byte %u past the record written",
            (unsigned)i);
}

static void test_stopped_after_odd(void)
{
   latch_t l;
   unsigned char a[WHOLE_LEN], out[WHOLE_LEN];
   int stamp;

   latch_init(&l);
   fill(a, sizeof(a), 5);
   retro_seqlatch_publish(&l.seq, l.words, a, sizeof(a));
   /* The next publish stamps odd and stops part way into copy 0. */
   retro_atomic_store_release_int(&l.seq, 3);
   garble(l.words, MAX_WORDS / 2);

   memset(out, 0, sizeof(out));
   stamp = read_bounded(&l, out, sizeof(out), "stopped after the odd stamp");
   CHECK(!memcmp(out, a, sizeof(a)),
         "stopped after the odd stamp: not the last whole record");
   CHECK(stamp == 3, "stopped after the odd stamp: stamp %d, not 3", stamp);
}

static void test_stopped_after_even(void)
{
   latch_t l;
   unsigned char a[WHOLE_LEN], b[WHOLE_LEN], out[WHOLE_LEN];
   int stamp;

   latch_init(&l);
   fill(a, sizeof(a), 5);
   fill(b, sizeof(b), 9);
   retro_seqlatch_publish(&l.seq, l.words, a, sizeof(a));
   retro_seqlatch_publish(&l.seq, l.words, b, sizeof(b));
   /* That publish as if stopped after its even stamp, part way into
    * copy 1. */
   garble(l.words + MAX_WORDS, MAX_WORDS / 2);

   memset(out, 0, sizeof(out));
   stamp = read_bounded(&l, out, sizeof(out), "stopped after the even stamp");
   CHECK(!memcmp(out, b, sizeof(b)),
         "stopped after the even stamp: not copy 0's record");
   CHECK(stamp == 4, "stopped after the even stamp: stamp %d, not 4", stamp);
}

static retro_atomic_int_t seq;
static retro_atomic_int_t words[2 * RETRO_SEQLATCH_WORDS(WORDS * sizeof(int))];
static retro_atomic_int_t running;

typedef struct
{
   unsigned long copies;
   unsigned long torn;
   unsigned long backwards;
   unsigned long off_stamp;
} reader_result_t;

static void *reader_main(void *arg)
{
   reader_result_t *result = (reader_result_t*)arg;
   int last                = 0;
   /* At least one copy, even when the writer is done before this starts. */
   do
   {
      int copy[WORDS];
      unsigned i;
      int stamp = retro_seqlatch_read(&seq, words, copy, sizeof(copy));
      for (i = 1; i < WORDS; i++)
         if (copy[i] != copy[0])
         {
            result->torn++;
            break;
         }
      if (copy[0] < last)
         result->backwards++;
      if (copy[0] != (int)((unsigned)stamp / 2))
         result->off_stamp++;
      last = copy[0];
      result->copies++;
   } while (retro_atomic_load_acquire_int(&running));
   return NULL;
}

static void test_threads(void)
{
   pthread_t readers[READERS];
   reader_result_t results[READERS];
   int rec[WORDS];
   int final_copy[WORDS];
   int stamp;
   int n;
   unsigned i;

   memset(results, 0, sizeof(results));
   retro_atomic_int_init(&seq, 0);
   for (i = 0; i < sizeof(words) / sizeof(words[0]); i++)
      retro_atomic_int_init(&words[i], 0);
   retro_atomic_int_init(&running, 1);
   for (i = 0; i < READERS; i++)
      start_thread(&readers[i], reader_main, &results[i], "threads");

   for (n = 1; n <= UPDATES; n++)
   {
      for (i = 0; i < WORDS; i++)
         rec[i] = n;
      retro_seqlatch_publish(&seq, words, rec, sizeof(rec));
   }

   retro_atomic_store_release_int(&running, 0);
   for (i = 0; i < READERS; i++)
   {
      pthread_join(readers[i], NULL);
      printf("reader %u: %lu copies\n", i, results[i].copies);
      CHECK(results[i].torn == 0, "reader %u: %lu torn copies", i,
            results[i].torn);
      CHECK(results[i].backwards == 0, "reader %u: went back %lu times", i,
            results[i].backwards);
      CHECK(results[i].off_stamp == 0,
            "reader %u: %lu copies not the update their stamp says", i,
            results[i].off_stamp);
   }

   stamp = retro_seqlatch_read(&seq, words, final_copy, sizeof(final_copy));
   CHECK(final_copy[0] == UPDATES && final_copy[WORDS - 1] == UPDATES,
         "last copy is update %d, not %d", final_copy[0], UPDATES);
   CHECK(stamp == 2 * UPDATES, "last stamp %d, not %d", stamp, 2 * UPDATES);
}

int main(void)
{
   test_round_trip();
   test_odd_size();
   test_stopped_after_odd();
   test_stopped_after_even();
   test_threads();

   if (failures)
   {
      printf("%u check(s) failed\n", failures);
      return 1;
   }
   printf("all checks passed\n");
   return 0;
}
