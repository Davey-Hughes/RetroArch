/* The raster poll interface: RetroArch's contract checker
 * (gfx/video_raster.c) against synthetic call sequences. */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libretro.h>

#include "../../../gfx/video_raster.h"

static unsigned log_count;
static unsigned warn_count;
static char     last_msg[256];
static int      failures;

/* verbosity.h's non-logger build declares these as functions */
void RARCH_LOG(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(last_msg, sizeof(last_msg), fmt, ap);
   va_end(ap);
   log_count++;
}

void RARCH_WARN(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(last_msg, sizeof(last_msg), fmt, ap);
   va_end(ap);
   warn_count++;
}

#define CHECK(cond) do { \
   if (!(cond)) { \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++; \
   } \
} while (0)

static uint32_t frame[256 * 240];

static void counts_reset(void)
{
   log_count   = 0;
   warn_count  = 0;
   last_msg[0] = '\0';
}

/* Polls rows step-1, 2*step-1, ... and always the last row */
static void poll_frame(video_raster_t *r, unsigned step)
{
   unsigned y;
   for (y = 0; y < 240; y++)
      if ((y + 1) % step == 0 || y + 1 == 240)
         CHECK(video_raster_poll(r, frame, 256, 240, y) == VIDEO_RASTER_OK);
}

static void test_clean_rows(void)
{
   video_raster_t r;
   video_raster_reset(&r);
   counts_reset();

   poll_frame(&r, 1);
   CHECK(r.calls == 240);
   CHECK(r.first_row == 0 && r.last_row == 239);
   CHECK(video_raster_frame_end(&r, frame, 256, 240) == VIDEO_RASTER_OK);
   CHECK(log_count == 1);
   CHECK(strstr(last_msg, "240 calls for 256x240, rows 0-239.") != NULL);
   CHECK(r.calls == 0);

   /* The summary is once per core */
   poll_frame(&r, 1);
   CHECK(video_raster_frame_end(&r, frame, 256, 240) == VIDEO_RASTER_OK);
   CHECK(log_count == 1);
   CHECK(warn_count == 0);
}

static void test_batches(void)
{
   video_raster_t r;
   video_raster_reset(&r);
   counts_reset();

   poll_frame(&r, 8);
   CHECK(r.calls == 30);
   CHECK(r.first_row == 7 && r.last_row == 239);
   CHECK(video_raster_frame_end(&r, frame, 256, 240) == VIDEO_RASTER_OK);
   CHECK(strstr(last_msg, "30 calls for 256x240, rows 7-239.") != NULL);
   CHECK(warn_count == 0);
}

static void test_order(void)
{
   video_raster_t r;
   video_raster_reset(&r);
   counts_reset();

   CHECK(video_raster_poll(&r, frame, 256, 240, 0) == VIDEO_RASTER_OK);
   CHECK(video_raster_poll(&r, frame, 256, 240, 1) == VIDEO_RASTER_OK);
   CHECK(video_raster_poll(&r, frame, 256, 240, 1) == VIDEO_RASTER_BAD_ORDER);
   CHECK(r.flags & VIDEO_RASTER_FLAG_BROKEN);
   CHECK(warn_count == 1);
   CHECK(strstr(last_msg, "row 1 after row 1.") != NULL);

   /* The rest of a broken frame is ignored... */
   CHECK(video_raster_poll(&r, frame, 256, 240, 2) == VIDEO_RASTER_OK);
   CHECK(r.calls == 2);

   /* ...and it is not summarised */
   CHECK(video_raster_frame_end(&r, frame, 256, 240) == VIDEO_RASTER_OK);
   CHECK(log_count == 0);
   CHECK(!(r.flags & VIDEO_RASTER_FLAG_BROKEN));

   /* The same kind of break is reported once */
   CHECK(video_raster_poll(&r, frame, 256, 240, 5) == VIDEO_RASTER_OK);
   CHECK(video_raster_poll(&r, frame, 256, 240, 4) == VIDEO_RASTER_BAD_ORDER);
   CHECK(warn_count == 1);
   video_raster_frame_end(&r, frame, 256, 240);

   /* A clean frame after a broken one is summarised */
   poll_frame(&r, 1);
   CHECK(video_raster_frame_end(&r, frame, 256, 240) == VIDEO_RASTER_OK);
   CHECK(log_count == 1);
}

static void test_row_range(void)
{
   video_raster_t r;
   video_raster_reset(&r);
   counts_reset();

   CHECK(video_raster_poll(&r, frame, 256, 240, 240) == VIDEO_RASTER_BAD_ROW);
   CHECK(warn_count == 1);
   CHECK(strstr(last_msg, "row 240 of a frame 240 rows tall.") != NULL);
   video_raster_frame_end(&r, frame, 256, 240);
   CHECK(log_count == 0);
}

static void test_size_change(void)
{
   video_raster_t r;
   video_raster_reset(&r);
   counts_reset();

   CHECK(video_raster_poll(&r, frame, 256, 240, 0) == VIDEO_RASTER_OK);
   CHECK(video_raster_poll(&r, frame, 256, 224, 1) == VIDEO_RASTER_BAD_SIZE);
   CHECK(strstr(last_msg, "256x224 within a 256x240 frame.") != NULL);
   video_raster_frame_end(&r, frame, 256, 240);
   CHECK(warn_count == 1);
   CHECK(log_count == 0);
}

static void test_size_at_end(void)
{
   video_raster_t r;
   video_raster_reset(&r);
   counts_reset();

   poll_frame(&r, 1);
   CHECK(video_raster_frame_end(&r, frame, 256, 224) == VIDEO_RASTER_BAD_SIZE);
   CHECK(warn_count == 1);
   CHECK(strstr(last_msg, "256x240, frame presented at 256x224.") != NULL);
   CHECK(log_count == 0);
   CHECK(r.calls == 0);
}

static void test_data(void)
{
   video_raster_t r;
   video_raster_reset(&r);
   counts_reset();

   CHECK(video_raster_poll(&r, NULL, 256, 240, 0) == VIDEO_RASTER_BAD_DATA);
   video_raster_frame_end(&r, frame, 256, 240);
   CHECK(video_raster_poll(&r, RETRO_HW_FRAME_BUFFER_VALID, 256, 240, 0)
         == VIDEO_RASTER_BAD_DATA);
   video_raster_frame_end(&r, frame, 256, 240);
   CHECK(warn_count == 1);
   CHECK(strstr(last_msg, "without a software frame.") != NULL);

   /* A hardware frame presented after software polls */
   poll_frame(&r, 1);
   CHECK(video_raster_frame_end(&r, RETRO_HW_FRAME_BUFFER_VALID, 256, 240)
         == VIDEO_RASTER_BAD_DATA);
   CHECK(log_count == 0);
}

static void test_dupe_and_empty(void)
{
   video_raster_t r;
   video_raster_reset(&r);
   counts_reset();

   /* A duped frame ends the frame without judging it... */
   poll_frame(&r, 1);
   CHECK(video_raster_frame_end(&r, NULL, 256, 240) == VIDEO_RASTER_OK);
   CHECK(r.calls == 0);
   CHECK(log_count == 0 && warn_count == 0);

   /* ...so the next frame starts over at row 0 */
   poll_frame(&r, 1);
   CHECK(video_raster_frame_end(&r, frame, 256, 240) == VIDEO_RASTER_OK);
   CHECK(log_count == 1 && warn_count == 0);

   /* A frame without polls says nothing */
   counts_reset();
   CHECK(video_raster_frame_end(&r, frame, 256, 240) == VIDEO_RASTER_OK);
   CHECK(log_count == 0 && warn_count == 0);
}

static void test_reset(void)
{
   video_raster_t r;
   video_raster_reset(&r);
   counts_reset();

   CHECK(video_raster_poll(&r, frame, 256, 240, 240) == VIDEO_RASTER_BAD_ROW);
   video_raster_frame_end(&r, frame, 256, 240);
   poll_frame(&r, 1);
   video_raster_frame_end(&r, frame, 256, 240);
   CHECK(warn_count == 1 && log_count == 1);

   /* A new core hears about both again */
   video_raster_reset(&r);
   CHECK(video_raster_poll(&r, frame, 256, 240, 240) == VIDEO_RASTER_BAD_ROW);
   video_raster_frame_end(&r, frame, 256, 240);
   poll_frame(&r, 1);
   video_raster_frame_end(&r, frame, 256, 240);
   CHECK(warn_count == 2 && log_count == 2);
}

int main(void)
{
   test_clean_rows();
   test_batches();
   test_order();
   test_row_range();
   test_size_change();
   test_size_at_end();
   test_data();
   test_dupe_and_empty();
   test_reset();

   if (failures)
   {
      fprintf(stderr, "raster_poll_test: %d check(s) failed\n", failures);
      return 1;
   }
   printf("raster_poll_test: all checks passed\n");
   return 0;
}
