/*  RetroArch - A frontend for libretro.
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* A software core that reports each finished row through
 * RETRO_ENVIRONMENT_GET_RASTER_POLL_INTERFACE. A bar moves 4 pixels a
 * frame over a vertical gradient, so a frame shown in slices shows any
 * seam between them. */

#include <stdint.h>
#include <string.h>

#include <libretro.h>

#define RP_WIDTH  256
#define RP_HEIGHT 240
#define RP_BAR    16
#define RP_STEP   4

static uint32_t rp_frame[RP_WIDTH * RP_HEIGHT];
static unsigned rp_bar_x;
static uint32_t rp_frame_count;
static unsigned rp_rows_per_call = 1;

static retro_environment_t   rp_environ_cb;
static retro_video_refresh_t rp_video_cb;
static retro_input_poll_t    rp_input_poll_cb;
static retro_raster_poll_t   rp_raster_poll_cb;

void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_variable vars[] = {
      { "raster_poll_rows_per_call", "Rows per raster poll; 1|8|240" },
      { NULL, NULL }
   };
   bool no_game = true;

   rp_environ_cb = cb;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { rp_video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
void retro_set_input_poll(retro_input_poll_t cb) { rp_input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { (void)cb; }

void retro_init(void) { }
void retro_deinit(void) { rp_raster_poll_cb = NULL; }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Raster Poll Test";
   info->library_version  = "1";
   info->need_fullpath    = false;
   info->valid_extensions = "";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->geometry.base_width   = RP_WIDTH;
   info->geometry.base_height  = RP_HEIGHT;
   info->geometry.max_width    = RP_WIDTH;
   info->geometry.max_height   = RP_HEIGHT;
   info->geometry.aspect_ratio = 4.0f / 3.0f;
   info->timing.fps            = 60.0;
   info->timing.sample_rate    = 48000.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_reset(void)
{
   rp_bar_x       = 0;
   rp_frame_count = 0;
}

static void rp_read_variables(void)
{
   struct retro_variable var;

   var.key          = "raster_poll_rows_per_call";
   var.value        = NULL;
   rp_rows_per_call = 1;
   if (rp_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "8"))
         rp_rows_per_call = 8;
      else if (!strcmp(var.value, "240"))
         rp_rows_per_call = 240;
   }
}

static void rp_draw_row(unsigned y)
{
   unsigned x;
   uint32_t *row = rp_frame + y * RP_WIDTH;
   uint32_t  bg  = (uint32_t)((y * 255) / (RP_HEIGHT - 1));

   for (x = 0; x < RP_WIDTH; x++)
      row[x] = ((x + RP_WIDTH - rp_bar_x) % RP_WIDTH < RP_BAR)
            ? 0x00FFFFFF : bg;
}

void retro_run(void)
{
   unsigned y;
   bool updated = false;

   rp_input_poll_cb();

   if (     rp_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated)
         && updated)
      rp_read_variables();

   for (y = 0; y < RP_HEIGHT; y++)
   {
      rp_draw_row(y);
      if (     rp_raster_poll_cb
            && ((y + 1) % rp_rows_per_call == 0 || y + 1 == RP_HEIGHT))
         rp_raster_poll_cb(rp_frame, RP_WIDTH, RP_HEIGHT,
               sizeof(rp_frame[0]) * RP_WIDTH, y);
   }

   rp_video_cb(rp_frame, RP_WIDTH, RP_HEIGHT,
         sizeof(rp_frame[0]) * RP_WIDTH);

   rp_bar_x = (rp_bar_x + RP_STEP) % RP_WIDTH;
   rp_frame_count++;
}

size_t retro_serialize_size(void)
{
   return 2 * sizeof(uint32_t);
}

bool retro_serialize(void *data, size_t size)
{
   uint32_t state[2];

   if (size < sizeof(state))
      return false;
   state[0] = (uint32_t)rp_bar_x;
   state[1] = rp_frame_count;
   memcpy(data, state, sizeof(state));
   return true;
}

bool retro_unserialize(const void *data, size_t size)
{
   uint32_t state[2];

   if (size < sizeof(state))
      return false;
   memcpy(state, data, sizeof(state));
   rp_bar_x       = state[0] % RP_WIDTH;
   rp_frame_count = state[1];
   return true;
}

void retro_cheat_reset(void) { }

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   struct retro_raster_poll_interface raster;

   (void)game;

   if (!rp_environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      return false;

   raster.interface_version = RETRO_RASTER_POLL_INTERFACE_VERSION;
   raster.raster_poll       = NULL;
   rp_raster_poll_cb        = NULL;
   if (rp_environ_cb(RETRO_ENVIRONMENT_GET_RASTER_POLL_INTERFACE, &raster))
      rp_raster_poll_cb = raster.raster_poll;

   rp_read_variables();
   retro_reset();
   return true;
}

bool retro_load_game_special(unsigned type,
      const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

void retro_unload_game(void) { rp_raster_poll_cb = NULL; }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}
