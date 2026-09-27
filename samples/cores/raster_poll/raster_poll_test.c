/* The raster poll interface: RetroArch's contract checker
 * (gfx/video_raster.c) against synthetic call sequences, then the test
 * core (raster_poll_core.c, linked in) driven through it. */

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

/* The test core, driven as a frontend would, through the checker */
static video_raster_t core_raster;
static const char    *core_rows_option;
static bool           core_offer_interface;
static unsigned       core_frame_polls;
static unsigned       core_frames;
static unsigned       core_shadow_rows;
static uint32_t       core_shadow[256 * 240];
static uint32_t       core_last[256 * 240];

static void RETRO_CALLCONV core_raster_poll(const void *data,
      unsigned width, unsigned height, size_t pitch, unsigned row)
{
   unsigned y;
   CHECK(video_raster_poll(&core_raster, data, width, height, row)
         == VIDEO_RASTER_OK);
   CHECK(pitch == width * sizeof(uint32_t));
   /* Keep each row as it is first reported final */
   for (y = core_shadow_rows; y <= row && y < 240; y++)
      memcpy(core_shadow + y * 256, (const uint8_t*)data + y * pitch,
            256 * sizeof(uint32_t));
   core_shadow_rows = row + 1;
   core_frame_polls++;
}

static void RETRO_CALLCONV core_video_refresh(const void *data,
      unsigned width, unsigned height, size_t pitch)
{
   unsigned y;
   bool same = true;
   CHECK(video_raster_frame_end(&core_raster, data, width, height)
         == VIDEO_RASTER_OK);
   for (y = 0; y < core_shadow_rows; y++)
      if (memcmp(core_shadow + y * 256, (const uint8_t*)data + y * pitch,
               256 * sizeof(uint32_t)))
         same = false;
   CHECK(same);
   for (y = 0; y < 240; y++)
      memcpy(core_last + y * 256, (const uint8_t*)data + y * pitch,
            256 * sizeof(uint32_t));
   core_shadow_rows = 0;
   core_frames++;
}

static bool RETRO_CALLCONV core_environment(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_RASTER_POLL_INTERFACE:
      {
         struct retro_raster_poll_interface *iface =
               (struct retro_raster_poll_interface*)data;
         if (     !core_offer_interface
               || !iface
               || iface->interface_version != RETRO_RASTER_POLL_INTERFACE_VERSION)
            return false;
         iface->raster_poll = core_raster_poll;
         return true;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = (struct retro_variable*)data;
         if (strcmp(var->key, "raster_poll_rows_per_call"))
            return false;
         var->value = core_rows_option;
         return true;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool*)data = false;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return *(const enum retro_pixel_format*)data
               == RETRO_PIXEL_FORMAT_XRGB8888;
      case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      case RETRO_ENVIRONMENT_SET_VARIABLES:
         return true;
      default:
         break;
   }
   return false;
}

static void RETRO_CALLCONV core_input_poll(void) { }

static int16_t RETRO_CALLCONV core_input_state(unsigned port,
      unsigned device, unsigned index, unsigned id)
{
   return 0;
}

static void core_load(const char *rows_option, bool offer)
{
   core_rows_option     = rows_option;
   core_offer_interface = offer;
   core_frames          = 0;
   core_shadow_rows     = 0;
   video_raster_reset(&core_raster);
   counts_reset();

   retro_set_environment(core_environment);
   retro_set_video_refresh(core_video_refresh);
   retro_set_input_poll(core_input_poll);
   retro_set_input_state(core_input_state);
   retro_init();
   CHECK(retro_load_game(NULL));
}

static void core_unload(void)
{
   retro_unload_game();
   retro_deinit();
}

static void test_core(const char *rows_option, bool offer,
      unsigned polls_per_frame)
{
   unsigned i;

   core_load(rows_option, offer);
   for (i = 0; i < 3; i++)
   {
      core_frame_polls = 0;
      retro_run();
      CHECK(core_frame_polls == polls_per_frame);
   }
   CHECK(core_frames == 3);
   CHECK(warn_count == 0);
   if (polls_per_frame)
   {
      CHECK(log_count == 1);
      CHECK(strstr(last_msg, "-239.") != NULL);
   }
   else
      CHECK(log_count == 0);
   core_unload();
}

/* Run-ahead rolls the core back: a restored state replays the frame */
static void test_core_state(void)
{
   static uint32_t after[256 * 240];
   uint8_t state[64];
   size_t size;

   core_load("1", true);
   retro_run();
   retro_run();
   size = retro_serialize_size();
   CHECK(size > 0 && size <= sizeof(state));
   CHECK(retro_serialize(state, size));
   retro_run();
   memcpy(after, core_last, sizeof(after));
   retro_run();
   CHECK(retro_unserialize(state, size));
   retro_run();
   CHECK(!memcmp(after, core_last, sizeof(after)));
   core_unload();
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

   test_core("1",   true,  240);
   test_core("8",   true,  30);
   test_core("240", true,  1);
   test_core("1",   false, 0);
   test_core_state();

   if (failures)
   {
      fprintf(stderr, "raster_poll_test: %d check(s) failed\n", failures);
      return 1;
   }
   printf("raster_poll_test: all checks passed\n");
   return 0;
}
