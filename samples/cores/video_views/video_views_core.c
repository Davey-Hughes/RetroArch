/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2026 - The RetroArch team
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

/* Core for checking RetroArch's view presentation: it sends a view map
 * (RETRO_ENVIRONMENT_SET_VIDEO_VIEWS), draws every view in its own solid
 * colour with a white marker at its top-left corner, follows
 * RETRO_ENVIRONMENT_GET_VIDEO_VIEWS_STATUS, and logs where touches land
 * in the packed frame. It sends its map even without
 * RETRO_VIDEO_VIEWS_STATUS_PRESENTS, so the frontend's answer there is
 * logged too. It draws in software, or with the video_views_test_hw
 * option through a GL core context, with either origin. e2e/run.py
 * checks the colours on screen and the status in the log. */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <libretro.h>

#define MAX_W 800
#define MAX_H 480

#define COL_BG     0x202020
#define COL_RED    0xFF0000
#define COL_BLUE   0x0000FF
#define COL_GREEN  0x00FF00
#define COL_YELLOW 0xFFFF00
#define COL_WHITE  0xFFFFFF

enum map_kind
{
   MAP_3DS = 0,
   MAP_3DS_FORCE,
   MAP_DS,
   MAP_VB,
   MAP_INVALID,
   MAP_NONE
};

enum hw_kind
{
   HW_OFF = 0,
   HW_GL,         /* bottom-left origin */
   HW_GL_TOPLEFT
};

/* The GL the hardware mode uses, loaded through the frontend's
 * get_proc_address: the core links no GL library. */
#ifndef APIENTRY
#ifdef _WIN32
#define APIENTRY __stdcall
#else
#define APIENTRY
#endif
#endif
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_SCISSOR_TEST     0x0C11
#define GL_FRAMEBUFFER      0x8D40
typedef void (APIENTRY *gl_bind_framebuffer_t)(unsigned, unsigned);
typedef void (APIENTRY *gl_enable_t)(unsigned);
typedef void (APIENTRY *gl_scissor_t)(int, int, int, int);
typedef void (APIENTRY *gl_clear_color_t)(float, float, float, float);
typedef void (APIENTRY *gl_clear_t)(unsigned);

static retro_environment_t   environ_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t    input_poll_cb;
static retro_input_state_t   input_state_cb;
static retro_log_printf_t    log_cb;

static uint32_t frame_buf[MAX_W * MAX_H];
static enum map_kind map_kind = MAP_3DS;
static enum hw_kind hw_kind   = HW_OFF;
static struct retro_hw_render_callback hw_render;
static gl_bind_framebuffer_t p_glBindFramebuffer;
static gl_enable_t           p_glEnable;
static gl_enable_t           p_glDisable;
static gl_scissor_t          p_glScissor;
static gl_clear_color_t      p_glClearColor;
static gl_clear_t            p_glClear;
static bool gl_ready;
static int last_status        = -1;
static int last_accepted      = -1;
static int last_pressed       = -1;
static int16_t last_px, last_py;

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list va;
   (void)level;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

static struct retro_video_view mkview(unsigned x, unsigned y,
      unsigned w, unsigned h, unsigned screen, unsigned eye)
{
   struct retro_video_view v;
   v.x            = x;
   v.y            = y;
   v.width        = w;
   v.height       = h;
   v.screen       = screen;
   v.eye          = eye;
   v.aspect_ratio = 0.0f;
   return v;
}

static void fill(unsigned x, unsigned y, unsigned w, unsigned h,
      unsigned fw, uint32_t c)
{
   unsigned i, j;
   for (j = y; j < y + h; j++)
      for (i = x; i < x + w; i++)
         frame_buf[j * fw + i] = c;
}

/* A top-left rectangle of the frame, cleared to c through the scissor.
 * With a bottom-left origin, GL counts rows from the frame's bottom. */
static void gl_rect(unsigned x, unsigned y, unsigned w, unsigned h,
      unsigned fh, uint32_t c)
{
   int gy = (hw_kind == HW_GL) ? (int)(fh - (y + h)) : (int)y;
   p_glScissor((int)x, gy, (int)w, (int)h);
   p_glClearColor(((c >> 16) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f,
         (c & 0xff) / 255.0f, 1.0f);
   p_glClear(GL_COLOR_BUFFER_BIT);
}

static void rect(unsigned x, unsigned y, unsigned w, unsigned h,
      unsigned fw, unsigned fh, uint32_t c)
{
   if (hw_kind == HW_OFF)
      fill(x, y, w, h, fw, c);
   else
      gl_rect(x, y, w, h, fh, c);
}

static void context_reset(void)
{
   p_glBindFramebuffer = (gl_bind_framebuffer_t)
      hw_render.get_proc_address("glBindFramebuffer");
   p_glEnable          = (gl_enable_t)hw_render.get_proc_address("glEnable");
   p_glDisable         = (gl_enable_t)hw_render.get_proc_address("glDisable");
   p_glScissor         = (gl_scissor_t)hw_render.get_proc_address("glScissor");
   p_glClearColor      = (gl_clear_color_t)
      hw_render.get_proc_address("glClearColor");
   p_glClear           = (gl_clear_t)hw_render.get_proc_address("glClear");
   gl_ready            = p_glBindFramebuffer && p_glEnable && p_glDisable
      && p_glScissor && p_glClearColor && p_glClear;
   if (!gl_ready)
      log_cb(RETRO_LOG_ERROR, "[video_views] GL entry points missing\n");
}

static void context_destroy(void)
{
   gl_ready = false;
}

static void frame_size(unsigned *fw, unsigned *fh)
{
   switch (map_kind)
   {
      case MAP_DS: *fw = 256; *fh = 384; break;
      case MAP_VB: *fw = 768; *fh = 224; break;
      default:     *fw = 800; *fh = 480; break;
   }
}

static unsigned build_map(struct retro_video_view *v, uint32_t *c,
      bool stereo)
{
   switch (map_kind)
   {
      case MAP_DS:
         v[0] = mkview(0,   0, 256, 192, 0, RETRO_VIDEO_VIEW_EYE_NONE);
         v[1] = mkview(0, 192, 256, 192, 1, RETRO_VIDEO_VIEW_EYE_NONE);
         c[0] = COL_GREEN;
         c[1] = COL_YELLOW;
         return 2;
      case MAP_VB:
         if (!stereo)
         {
            v[0] = mkview(0, 0, 384, 224, 0, RETRO_VIDEO_VIEW_EYE_NONE);
            c[0] = COL_GREEN;
            return 1;
         }
         v[0] = mkview(  0, 0, 384, 224, 0, RETRO_VIDEO_VIEW_EYE_LEFT);
         v[1] = mkview(384, 0, 384, 224, 0, RETRO_VIDEO_VIEW_EYE_RIGHT);
         c[0] = COL_RED;
         c[1] = COL_BLUE;
         return 2;
      case MAP_INVALID:
         /* A left eye with no right: the frontend must refuse it. */
         v[0] = mkview(0, 0, 400, 240, 0, RETRO_VIDEO_VIEW_EYE_LEFT);
         c[0] = COL_RED;
         return 1;
      default:
         if (stereo || map_kind == MAP_3DS_FORCE)
         {
            v[0] = mkview(  0,   0, 400, 240, 0, RETRO_VIDEO_VIEW_EYE_LEFT);
            v[1] = mkview(400,   0, 400, 240, 0, RETRO_VIDEO_VIEW_EYE_RIGHT);
            v[2] = mkview(240, 240, 320, 240, 1, RETRO_VIDEO_VIEW_EYE_NONE);
            c[0] = COL_RED;
            c[1] = COL_BLUE;
            c[2] = COL_YELLOW;
            return 3;
         }
         v[0] = mkview(  0,   0, 400, 240, 0, RETRO_VIDEO_VIEW_EYE_NONE);
         v[1] = mkview(240, 240, 320, 240, 1, RETRO_VIDEO_VIEW_EYE_NONE);
         c[0] = COL_GREEN;
         c[1] = COL_YELLOW;
         return 2;
   }
}

static void read_options(void)
{
   struct retro_variable var;
   var.key   = "video_views_test_hw";
   var.value = NULL;
   hw_kind   = HW_OFF;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "gl"))
         hw_kind = HW_GL;
      else if (!strcmp(var.value, "gl_topleft"))
         hw_kind = HW_GL_TOPLEFT;
   }

   var.key   = "video_views_test_map";
   var.value = NULL;
   map_kind  = MAP_3DS;
   if (!environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value)
      return;
   if (!strcmp(var.value, "3ds_force"))
      map_kind = MAP_3DS_FORCE;
   else if (!strcmp(var.value, "ds"))
      map_kind = MAP_DS;
   else if (!strcmp(var.value, "vb"))
      map_kind = MAP_VB;
   else if (!strcmp(var.value, "invalid"))
      map_kind = MAP_INVALID;
   else if (!strcmp(var.value, "none"))
      map_kind = MAP_NONE;
}

void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_variable vars[] = {
      { "video_views_test_map",
        "View map; 3ds|3ds_force|ds|vb|invalid|none" },
      { "video_views_test_hw",
        "Hardware rendering; off|gl|gl_topleft" },
      { NULL, NULL }
   };
   struct retro_log_callback logging;
   bool no_content = true;

   environ_cb = cb;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);
   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
   log_cb = fallback_log;
   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging) && logging.log)
      log_cb = logging.log;
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_init(void) { }
void retro_deinit(void) { }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "video_views test";
   info->library_version  = "1";
   info->valid_extensions = "";
   info->need_fullpath    = false;
   info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   unsigned fw, fh;
   frame_size(&fw, &fh);
   memset(info, 0, sizeof(*info));
   info->geometry.base_width   = fw;
   info->geometry.base_height  = fh;
   info->geometry.max_width    = MAX_W;
   info->geometry.max_height   = MAX_H;
   info->geometry.aspect_ratio = (float)fw / (float)fh;
   info->timing.fps            = 60.0;
   info->timing.sample_rate    = 48000.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_reset(void) { }

void retro_run(void)
{
   struct retro_video_view v[RETRO_VIDEO_VIEWS_MAX];
   uint32_t c[RETRO_VIDEO_VIEWS_MAX];
   struct retro_video_views views;
   unsigned i, n, fw, fh;
   int16_t px, py;
   int pressed;
   int accepted    = 0;
   unsigned status = 0;
   bool stereo;

   input_poll_cb();
   if (!environ_cb(RETRO_ENVIRONMENT_GET_VIDEO_VIEWS_STATUS, &status))
      status = 0;
   stereo = (status & RETRO_VIDEO_VIEWS_STATUS_STEREO) != 0;

   frame_size(&fw, &fh);
   n = build_map(v, c, stereo);
   if (hw_kind != HW_OFF)
   {
      if (!gl_ready)
      {
         video_cb(NULL, fw, fh, 0);
         return;
      }
      p_glBindFramebuffer(GL_FRAMEBUFFER,
            (unsigned)hw_render.get_current_framebuffer());
      p_glEnable(GL_SCISSOR_TEST);
   }
   rect(0, 0, fw, fh, fw, fh, COL_BG);
   for (i = 0; i < n; i++)
   {
      rect(v[i].x, v[i].y, v[i].width, v[i].height, fw, fh, c[i]);
      rect(v[i].x, v[i].y, 8, 8, fw, fh, COL_WHITE);
   }

   if (map_kind != MAP_NONE)
   {
      views.views     = v;
      views.num_views = n;
      accepted        = environ_cb(RETRO_ENVIRONMENT_SET_VIDEO_VIEWS,
            &views) ? 1 : 0;
   }
   if ((int)status != last_status || accepted != last_accepted)
   {
      log_cb(RETRO_LOG_INFO,
            "[video_views] presents=%d stereo=%d accepted=%d views=%u\n",
            (status & RETRO_VIDEO_VIEWS_STATUS_PRESENTS) ? 1 : 0,
            stereo ? 1 : 0, accepted, n);
      last_status   = (int)status;
      last_accepted = accepted;
   }

   px      = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
   py      = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
   pressed = input_state_cb(0, RETRO_DEVICE_POINTER, 0,
         RETRO_DEVICE_ID_POINTER_PRESSED) ? 1 : 0;
   if (pressed)
   {
      unsigned cx = (unsigned)(((long)px + 0x7fff) * (long)(fw - 1) / 0xfffe);
      unsigned cy = (unsigned)(((long)py + 0x7fff) * (long)(fh - 1) / 0xfffe);
      if (cx >= 4 && cx + 5 <= fw && cy >= 4 && cy + 5 <= fh)
      {
         rect(cx - 4, cy, 9, 1, fw, fh, COL_WHITE);
         rect(cx, cy - 4, 1, 9, fw, fh, COL_WHITE);
      }
      if (px != last_px || py != last_py || pressed != last_pressed)
         log_cb(RETRO_LOG_INFO,
               "[video_views] pointer x=%d y=%d pressed=1 packed=(%u,%u)\n",
               px, py, cx, cy);
   }
   else if (last_pressed == 1)
      log_cb(RETRO_LOG_INFO, "[video_views] pointer pressed=0\n");
   last_px      = px;
   last_py      = py;
   last_pressed = pressed;

   if (hw_kind != HW_OFF)
   {
      p_glDisable(GL_SCISSOR_TEST);
      video_cb(RETRO_HW_FRAME_BUFFER_VALID, fw, fh, 0);
   }
   else
      video_cb(frame_buf, fw, fh, fw * sizeof(uint32_t));
}

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
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
   (void)game;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      return false;
   read_options();
   if (hw_kind != HW_OFF)
   {
      memset(&hw_render, 0, sizeof(hw_render));
      hw_render.context_type       = RETRO_HW_CONTEXT_OPENGL_CORE;
      hw_render.version_major      = 3;
      hw_render.version_minor      = 2;
      hw_render.context_reset      = context_reset;
      hw_render.context_destroy    = context_destroy;
      hw_render.bottom_left_origin = (hw_kind == HW_GL);
      if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
         return false;
   }
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

void retro_unload_game(void) { }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
