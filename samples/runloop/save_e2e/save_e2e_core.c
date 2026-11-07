/* The E2E harness's core: save RAM of SAVE_E2E_SRAM bytes (default
 * 8192), loaded and written by the frontend.
 *
 * SAVE_E2E_TOUCH_FRAME=n: at frame n, fills save RAM with the bytes
 *    run.py expects (seed SAVE_E2E_SEED) and creates SAVE_E2E_MARK.
 * SAVE_E2E_FRAME_MS: sleeps that long each frame, so frames take time
 *    with the null drivers.
 * SAVE_E2E_LATE=n: reports no save RAM before frame n (A1-5). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <retro_timers.h>

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t poll_cb;
static unsigned char *sram;
static size_t sram_size;
static long frame, touch_frame = -1, late_frame;
static unsigned frame_ms, seed;
static unsigned fb[16 * 16];
/* SAVE_E2E_TOUCHES: "frame:seed,..."; each makes <mark>-<seed> */
#define MAX_TOUCHES 8
static long touches[MAX_TOUCHES][2];
static unsigned touch_num;

static long env_long(const char *name, long fallback)
{
   const char *v = getenv(name);
   return v && *v ? atol(v) : fallback;
}

/* The same generator as run.py's pattern(). */
static void fill(unsigned char *p, size_t len, unsigned s)
{
   size_t i;
   for (i = 0; i < len; i++)
   {
      s    = s * 1103515245u + 12345u;
      p[i] = (unsigned char)(s >> 16);
   }
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
   bool no_game = false;
   env_cb       = cb;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { (void)cb; }
RETRO_API void retro_init(void) { }
RETRO_API void retro_deinit(void) { }
RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "save_e2e";
   info->library_version  = "1";
   info->valid_extensions = "bin";
   info->need_fullpath    = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->timing.fps            = 60.0;
   info->timing.sample_rate    = 48000.0;
   info->geometry.base_width   = 16;
   info->geometry.base_height  = 16;
   info->geometry.max_width    = 16;
   info->geometry.max_height   = 16;
   info->geometry.aspect_ratio = 1.0f;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}
RETRO_API void retro_reset(void) { }

static void touch_list(void)
{
   unsigned i;
   for (i = 0; i < touch_num; i++)
   {
      if (frame == touches[i][0])
      {
         char path[1024];
         const char *mark = getenv("SAVE_E2E_MARK");
         FILE *f;
         fill(sram, sram_size, (unsigned)touches[i][1]);
         if (!mark || !*mark)
            continue;
         snprintf(path, sizeof(path), "%s-%ld", mark, touches[i][1]);
         if ((f = fopen(path, "wb")))
            fclose(f);
      }
   }
}

RETRO_API void retro_run(void)
{
   touch_list();
   if (frame == touch_frame)
   {
      const char *mark = getenv("SAVE_E2E_MARK");
      FILE *f;
      fill(sram, sram_size, seed);
      if (mark && *mark && (f = fopen(mark, "wb")))
         fclose(f);
   }
   frame++;
   poll_cb();
   video_cb(fb, 16, 16, 16 * sizeof(*fb));
   if (frame_ms)
      retro_sleep(frame_ms);
}

/* A state is save RAM alone, so a load rolls it back. */
RETRO_API size_t retro_serialize_size(void) { return sram_size; }
RETRO_API bool retro_serialize(void *data, size_t len)
{
   if (!sram || len < sram_size)
      return false;
   memcpy(data, sram, sram_size);
   return true;
}
RETRO_API bool retro_unserialize(const void *data, size_t len)
{
   if (!sram || len < sram_size)
      return false;
   memcpy(sram, data, sram_size);
   return true;
}
RETRO_API void retro_cheat_reset(void) { }
RETRO_API void retro_cheat_set(unsigned i, bool on, const char *code)
{
   (void)i;
   (void)on;
   (void)code;
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   (void)game;
   if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      return false;
   sram_size   = (size_t)env_long("SAVE_E2E_SRAM", 8192);
   touch_frame = env_long("SAVE_E2E_TOUCH_FRAME", -1);
   frame_ms    = (unsigned)env_long("SAVE_E2E_FRAME_MS", 0);
   seed        = (unsigned)env_long("SAVE_E2E_SEED", 1);
   late_frame  = env_long("SAVE_E2E_LATE", 0);
   {
      const char *t = getenv("SAVE_E2E_TOUCHES");
      char *end;
      while (t && *t && touch_num < MAX_TOUCHES)
      {
         touches[touch_num][0] = strtol(t, &end, 10);
         if (*end != ':')
            break;
         touches[touch_num][1] = strtol(end + 1, &end, 10);
         touch_num++;
         if (*end != ',')
            break;
         t = end + 1;
      }
   }
   return (sram = (unsigned char*)calloc(1, sram_size)) != NULL;
}

RETRO_API bool retro_load_game_special(unsigned type,
      const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

RETRO_API void retro_unload_game(void)
{
   free(sram);
   sram = NULL;
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API void *retro_get_memory_data(unsigned id)
{
   return id == RETRO_MEMORY_SAVE_RAM && frame >= late_frame ? sram : NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
   return id == RETRO_MEMORY_SAVE_RAM && frame >= late_frame ? sram_size : 0;
}
