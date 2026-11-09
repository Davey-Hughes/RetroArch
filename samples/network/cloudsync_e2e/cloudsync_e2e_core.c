/* A core with save RAM and states for cloudsync_e2e.
 *
 * CLOUDSYNC_E2E_LOG names a file the core appends to:
 *   "loaded <hex>"  at the first frame: the save RAM RetroArch loaded
 *   "filled <hex>"  CLOUDSYNC_E2E_FILL (one hex byte) written to the
 *                   whole save RAM at frame 10
 *   "state <hex>"   a state was loaded
 *   "run <n>"       every 60 frames
 * <hex> is the first 8 bytes of the save RAM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libretro.h>

#define SRAM_SIZE 256

static unsigned char sram[SRAM_SIZE];
static unsigned frame_buf[16 * 16];
static unsigned frames;
static retro_video_refresh_t video_cb;
static retro_environment_t   environ_cb;

static void note(const char *what)
{
   const char *path = getenv("CLOUDSYNC_E2E_LOG");
   FILE *f;
   int i;

   if (!path || !*path || !(f = fopen(path, "a")))
      return;
   fprintf(f, "%s ", what);
   for (i = 0; i < 8; i++)
      fprintf(f, "%02x", sram[i]);
   fprintf(f, "\n");
   fclose(f);
}

void retro_set_environment(retro_environment_t cb)
{
   bool no_game = false;
   environ_cb   = cb;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
void retro_set_input_poll(retro_input_poll_t cb) { (void)cb; }
void retro_set_input_state(retro_input_state_t cb) { (void)cb; }
void retro_init(void) { memset(sram, 0, sizeof(sram)); frames = 0; }
void retro_deinit(void) { }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "cloudsync_e2e";
   info->library_version  = "1";
   info->valid_extensions = "bin";
   info->need_fullpath    = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->geometry.base_width   = 16;
   info->geometry.base_height  = 16;
   info->geometry.max_width    = 16;
   info->geometry.max_height   = 16;
   info->timing.fps            = 60.0;
   info->timing.sample_rate    = 48000.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
void retro_reset(void) { }

void retro_run(void)
{
   const char *fill = getenv("CLOUDSYNC_E2E_FILL");
   char line[32];

   if (frames == 0)
      note("loaded");
   if (frames == 10 && fill && *fill)
   {
      memset(sram, (int)strtoul(fill, NULL, 16), sizeof(sram));
      note("filled");
   }
   frames++;
   if (frames % 60 == 0)
   {
      sprintf(line, "run %u", frames);
      note(line);
   }
   video_cb(frame_buf, 16, 16, 16 * sizeof(unsigned));
}

size_t retro_serialize_size(void) { return SRAM_SIZE; }
bool retro_serialize(void *data, size_t size)
{
   if (size < SRAM_SIZE)
      return false;
   memcpy(data, sram, SRAM_SIZE);
   return true;
}
bool retro_unserialize(const void *data, size_t size)
{
   if (size < SRAM_SIZE)
      return false;
   memcpy(sram, data, SRAM_SIZE);
   note("state");
   return true;
}
void retro_cheat_reset(void) { }
void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }

bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   (void)game;
   return environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}
bool retro_load_game_special(unsigned t, const struct retro_game_info *g, size_t n)
{ (void)t; (void)g; (void)n; return false; }
void retro_unload_game(void) { }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void *retro_get_memory_data(unsigned id)
{
   return id == RETRO_MEMORY_SAVE_RAM ? sram : NULL;
}
size_t retro_get_memory_size(unsigned id)
{
   return id == RETRO_MEMORY_SAVE_RAM ? SRAM_SIZE : 0;
}
