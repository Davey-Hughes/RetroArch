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

/* A libretro frontend that checks a core's raster polls. Each poll
 * goes through RetroArch's contract checker (gfx/video_raster.c), and
 * each row reported final is compared with the frame then presented.
 *
 * raster_poll_conform [options] CORE [CONTENT]
 *   --frames N                   frames to run (600)
 *   --dir DIR                    system and save directory (.)
 *   --set KEY=VALUE              a core option
 *   --press BUTTON@FRAME[+N]     hold a port 0 joypad button for N
 *                                frames (4): a b x y select start
 *                                up down left right
 *   --port-device PORT=ID        retro_set_controller_port_device
 *   --state-at FRAME             serialize before FRAME, unserialize
 *                                60 frames later
 *   --reset-at FRAME             retro_reset before FRAME
 *   --option-at FRAME:KEY=VALUE  change a core option before FRAME
 *   --dump FRAME:FILE            write that frame as a PPM
 *   --hash-out FILE              a line per presented frame: number,
 *                                size and a hash of its rows
 *   --hash-in FILE               fail on a presented frame that differs from FILE, the --hash-out of a --no-interface run with the same inputs
 *   --no-interface               refuse GET_RASTER_POLL_INTERFACE
 *   --null-poll                  a callback that does nothing
 *   --expect-polls               fail unless some frame polls
 *   --expect-no-polls            fail if any frame polls
 *   --verbose                    all of the core's log
 *
 * Exits 0 on PASS, 1 on FAIL, 2 when it cannot run. */

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libretro.h>

#include "../../../gfx/video_raster.h"

#define RC_MAX_OPTIONS   512
#define RC_MAX_EVENTS    64
#define RC_MAX_PORTS     8
#define RC_STATE_FRAMES  60
#define RC_HASH_LINE_MAX 128

enum rc_event_type
{
   RC_EVENT_PRESS = 0,
   RC_EVENT_STATE,
   RC_EVENT_RESET,
   RC_EVENT_OPTION,
   RC_EVENT_DUMP
};

struct rc_event
{
   enum rc_event_type type;
   unsigned           frame;
   unsigned           frames; /* PRESS: frames held */
   unsigned           id;     /* PRESS: joypad button */
   const char        *arg;    /* OPTION: KEY=VALUE, DUMP: file */
};

struct rc_option
{
   char key[128];
   char value[256];
   bool declared;
};

struct rc_core
{
   unsigned (*api_version)(void);
   void     (*set_environment)(retro_environment_t);
   void     (*set_video_refresh)(retro_video_refresh_t);
   void     (*set_audio_sample)(retro_audio_sample_t);
   void     (*set_audio_sample_batch)(retro_audio_sample_batch_t);
   void     (*set_input_poll)(retro_input_poll_t);
   void     (*set_input_state)(retro_input_state_t);
   void     (*init)(void);
   void     (*deinit)(void);
   void     (*get_system_info)(struct retro_system_info*);
   void     (*get_system_av_info)(struct retro_system_av_info*);
   void     (*set_controller_port_device)(unsigned, unsigned);
   void     (*reset)(void);
   void     (*run)(void);
   size_t   (*serialize_size)(void);
   bool     (*serialize)(void*, size_t);
   bool     (*unserialize)(const void*, size_t);
   bool     (*load_game)(const struct retro_game_info*);
   void     (*unload_game)(void);
};

typedef void (*rc_func_t)(void);

static const struct
{
   const char *name;
   unsigned    id;
} rc_button_names[] = {
   { "b",      RETRO_DEVICE_ID_JOYPAD_B },
   { "y",      RETRO_DEVICE_ID_JOYPAD_Y },
   { "select", RETRO_DEVICE_ID_JOYPAD_SELECT },
   { "start",  RETRO_DEVICE_ID_JOYPAD_START },
   { "up",     RETRO_DEVICE_ID_JOYPAD_UP },
   { "down",   RETRO_DEVICE_ID_JOYPAD_DOWN },
   { "left",   RETRO_DEVICE_ID_JOYPAD_LEFT },
   { "right",  RETRO_DEVICE_ID_JOYPAD_RIGHT },
   { "a",      RETRO_DEVICE_ID_JOYPAD_A },
   { "x",      RETRO_DEVICE_ID_JOYPAD_X }
};

#define RC_BUTTON_COUNT (sizeof(rc_button_names) / sizeof(rc_button_names[0]))

static struct rc_core          rc_core;
static struct rc_option        rc_options[RC_MAX_OPTIONS];
static unsigned                rc_option_count;
static bool                    rc_options_updated;
static struct rc_event         rc_events[RC_MAX_EVENTS];
static unsigned                rc_event_count;
static const char             *rc_dir = ".";
static bool                    rc_offer_interface = true;
static bool                    rc_null_poll;
static bool                    rc_verbose;
static enum retro_pixel_format rc_format = RETRO_PIXEL_FORMAT_0RGB1555;
static unsigned                rc_bpp = 2;
static unsigned                rc_joypad;
static unsigned                rc_frame;
static FILE                   *rc_hash_file;
static FILE                   *rc_hash_in_file;
static unsigned                rc_hash_in_diffs;
static unsigned                rc_hash_in_first_frame;
static char                    rc_hash_in_first_expected[RC_HASH_LINE_MAX];
static char                    rc_hash_in_first_presented[RC_HASH_LINE_MAX];
static bool                    rc_in_run;
static pthread_t               rc_run_thread;

/* The frame being built */
static video_raster_t          rc_raster;
static uint8_t                *rc_shadow;
static size_t                  rc_shadow_size;
static unsigned                rc_shadow_width;
static unsigned                rc_shadow_height;
static unsigned                rc_shadow_rows;
static unsigned                rc_frame_polls;
static bool                    rc_frame_presented;
static bool                    rc_bad_pitch_seen;
static bool                    rc_bad_refresh_pitch_seen;
static bool                    rc_poll_outside_run_seen;
static bool                    rc_poll_other_thread_seen;

static unsigned                rc_presented;
static unsigned                rc_polled_frames;
static unsigned long           rc_polls;
static unsigned long           rc_rows_compared;
static unsigned long           rc_mismatched_rows;
static unsigned                rc_first_mismatch_frame;
static unsigned                rc_first_mismatch_row;
static unsigned                rc_violations;

/* verbosity.h's non-logger build declares these as functions */
void RARCH_LOG(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

void RARCH_WARN(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

static void rc_cannot_run(const char *what, const char *detail)
{
   fprintf(stderr, "raster_poll_conform: %s%s%s\n", what,
         detail ? ": " : "", detail ? detail : "");
   exit(2);
}

static struct rc_option *rc_option_find(const char *key)
{
   unsigned i;
   for (i = 0; i < rc_option_count; i++)
      if (!strcmp(rc_options[i].key, key))
         return &rc_options[i];
   return NULL;
}

/* KEY=VALUE */
static struct rc_option *rc_option_set(const char *pair)
{
   const char       *eq = strchr(pair, '=');
   size_t            key_len;
   char              key[sizeof(rc_options[0].key)];
   struct rc_option *opt;

   if (     !eq
         || eq == pair
         || (size_t)(eq - pair) >= sizeof(key)
         || strlen(eq + 1) >= sizeof(rc_options[0].value))
      return NULL;
   key_len = (size_t)(eq - pair);
   memcpy(key, pair, key_len);
   key[key_len] = '\0';
   if (!(opt = rc_option_find(key)))
   {
      if (rc_option_count == RC_MAX_OPTIONS)
         return NULL;
      opt = &rc_options[rc_option_count++];
      strcpy(opt->key, key);
   }
   strcpy(opt->value, eq + 1);
   return opt;
}

/* "Description; default|other|..." unless the option is already set */
static void rc_option_default(const struct retro_variable *var)
{
   const char       *start = var->value ? strstr(var->value, "; ") : NULL;
   size_t            len;
   struct rc_option *opt;

   if (     !start
         || rc_option_find(var->key)
         || rc_option_count == RC_MAX_OPTIONS
         || strlen(var->key) >= sizeof(rc_options[0].key))
      return;
   start += 2;
   len    = strcspn(start, "|");
   if (len >= sizeof(rc_options[0].value))
      return;
   opt = &rc_options[rc_option_count++];
   strcpy(opt->key, var->key);
   memcpy(opt->value, start, len);
   opt->value[len] = '\0';
}

static void RETRO_CALLCONV rc_core_log(enum retro_log_level level,
      const char *fmt, ...)
{
   va_list ap;
   if (!rc_verbose && level < RETRO_LOG_WARN)
      return;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

static void RETRO_CALLCONV rc_raster_poll(const void *data,
      unsigned width, unsigned height, size_t pitch, unsigned row)
{
   unsigned y;
   size_t   row_bytes = (size_t)width * rc_bpp;

   if (!rc_in_run)
   {
      if (!rc_poll_outside_run_seen)
         fprintf(stderr, "frame %u: raster poll outside retro_run\n", rc_frame);
      rc_poll_outside_run_seen = true;
      rc_violations++;
   }
   else if (!pthread_equal(pthread_self(), rc_run_thread))
   {
      if (!rc_poll_other_thread_seen)
         fprintf(stderr, "frame %u: raster poll on another thread\n", rc_frame);
      rc_poll_other_thread_seen = true;
      rc_violations++;
   }

   rc_polls++;
   rc_frame_polls++;
   if (rc_null_poll)
      return;
   if (video_raster_poll(&rc_raster, data, width, height, row)
         != VIDEO_RASTER_OK)
      rc_violations++;

   if (     !data
         || data == RETRO_HW_FRAME_BUFFER_VALID
         || row  >= height)
      return;
   if (pitch < row_bytes)
   {
      if (!rc_bad_pitch_seen)
         fprintf(stderr, "frame %u: pitch %lu is under %u pixels\n",
               rc_frame, (unsigned long)pitch, width);
      rc_bad_pitch_seen = true;
      rc_violations++;
      return;
   }
   if (!rc_shadow_rows)
   {
      rc_shadow_width  = width;
      rc_shadow_height = height;
      if (row_bytes * height > rc_shadow_size)
      {
         rc_shadow_size = row_bytes * height;
         rc_shadow      = (uint8_t*)realloc(rc_shadow, rc_shadow_size);
         if (!rc_shadow)
            rc_cannot_run("out of memory", NULL);
      }
   }
   else if (width != rc_shadow_width || height != rc_shadow_height)
      return;

   /* Each row as it was first reported final */
   for (y = rc_shadow_rows; y <= row; y++)
      memcpy(rc_shadow + y * row_bytes,
            (const uint8_t*)data + y * pitch, row_bytes);
   if (row >= rc_shadow_rows)
      rc_shadow_rows = row + 1;
}

static uint32_t rc_hash(uint32_t hash, const uint8_t *bytes, size_t len)
{
   size_t i;
   for (i = 0; i < len; i++)
      hash = (hash ^ bytes[i]) * 16777619u;
   return hash;
}

/* "<frame> <w>x<h> <hash>" or "<frame> dupe": what --hash-out writes
 * and --hash-in compares against */
static void rc_hash_line(char *buf, unsigned frame, const void *data,
      unsigned width, unsigned height, size_t pitch, bool software)
{
   if (software)
   {
      unsigned y;
      size_t   row_bytes = (size_t)width * rc_bpp;
      uint32_t hash      = 2166136261u;
      for (y = 0; y < height; y++)
         hash = rc_hash(hash, (const uint8_t*)data + y * pitch, row_bytes);
      sprintf(buf, "%u %ux%u %08lx", frame, width, height,
            (unsigned long)hash);
   }
   else
      sprintf(buf, "%u dupe", frame);
}

/* Reads FILE's next line, or "" past its end */
static void rc_hash_in_next(char *buf)
{
   if (!fgets(buf, RC_HASH_LINE_MAX, rc_hash_in_file))
   {
      buf[0] = '\0';
      return;
   }
   buf[strcspn(buf, "\n")] = '\0';
}

static void rc_hash_in_check(const char *line)
{
   char expected[RC_HASH_LINE_MAX];
   rc_hash_in_next(expected);
   if (strcmp(expected, line))
   {
      if (!rc_hash_in_diffs)
      {
         rc_hash_in_first_frame = rc_frame;
         strcpy(rc_hash_in_first_expected, expected);
         strcpy(rc_hash_in_first_presented, line);
      }
      rc_hash_in_diffs++;
   }
}

/* Lines FILE still has once the run is done comparing */
static unsigned rc_hash_in_leftover(void)
{
   char     line[RC_HASH_LINE_MAX];
   unsigned count = 0;
   while (fgets(line, sizeof(line), rc_hash_in_file))
      count++;
   return count;
}

static void rc_dump(const char *path, const uint8_t *data,
      unsigned width, unsigned height, size_t pitch)
{
   unsigned x, y;
   FILE    *file = fopen(path, "wb");

   if (!file)
   {
      fprintf(stderr, "cannot write %s\n", path);
      return;
   }
   fprintf(file, "P6\n%u %u\n255\n", width, height);
   for (y = 0; y < height; y++)
   {
      const uint8_t *row = data + y * pitch;
      for (x = 0; x < width; x++)
      {
         unsigned r, g, b;
         if (rc_bpp == 4)
         {
            uint32_t p = ((const uint32_t*)row)[x];
            r = (p >> 16) & 0xFF;
            g = (p >>  8) & 0xFF;
            b =  p        & 0xFF;
         }
         else
         {
            unsigned p = ((const uint16_t*)row)[x];
            if (rc_format == RETRO_PIXEL_FORMAT_RGB565)
            {
               r = ((p >> 11) & 0x1F) << 3;
               g = ((p >>  5) & 0x3F) << 2;
               b = ( p        & 0x1F) << 3;
            }
            else
            {
               r = ((p >> 10) & 0x1F) << 3;
               g = ((p >>  5) & 0x1F) << 3;
               b = ( p        & 0x1F) << 3;
            }
         }
         fputc((int)r, file);
         fputc((int)g, file);
         fputc((int)b, file);
      }
   }
   fclose(file);
}

static void RETRO_CALLCONV rc_video_refresh(const void *data,
      unsigned width, unsigned height, size_t pitch)
{
   unsigned y, e;
   size_t   row_bytes = (size_t)width * rc_bpp;
   bool     software  = data && data != RETRO_HW_FRAME_BUFFER_VALID
         && pitch >= row_bytes;

   rc_presented++;
   rc_frame_presented = true;
   if (     !rc_null_poll
         && video_raster_frame_end(&rc_raster, data, width, height)
            != VIDEO_RASTER_OK)
      rc_violations++;

   if (     data
         && data != RETRO_HW_FRAME_BUFFER_VALID
         && pitch < row_bytes)
   {
      if (!rc_bad_refresh_pitch_seen)
         fprintf(stderr, "frame %u: final pitch %lu is under %u pixels\n",
               rc_frame, (unsigned long)pitch, width);
      rc_bad_refresh_pitch_seen = true;
      rc_violations++;
   }

   if (     software
         && width  == rc_shadow_width
         && height == rc_shadow_height)
   {
      for (y = 0; y < rc_shadow_rows; y++)
      {
         rc_rows_compared++;
         if (memcmp(rc_shadow + y * row_bytes,
                  (const uint8_t*)data + y * pitch, row_bytes))
         {
            if (!rc_mismatched_rows)
            {
               rc_first_mismatch_frame = rc_frame;
               rc_first_mismatch_row   = y;
            }
            rc_mismatched_rows++;
         }
      }
   }
   rc_shadow_rows = 0;

   if (rc_hash_file || rc_hash_in_file)
   {
      char line[RC_HASH_LINE_MAX];
      rc_hash_line(line, rc_frame, data, width, height, pitch, software);
      if (rc_hash_file)
         fprintf(rc_hash_file, "%s\n", line);
      if (rc_hash_in_file)
         rc_hash_in_check(line);
   }

   for (e = 0; e < rc_event_count; e++)
      if (     software
            && rc_events[e].type  == RC_EVENT_DUMP
            && rc_events[e].frame == rc_frame)
         rc_dump(rc_events[e].arg, (const uint8_t*)data, width, height,
               pitch);
}

static bool RETRO_CALLCONV rc_environment(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_RASTER_POLL_INTERFACE:
      {
         struct retro_raster_poll_interface *iface =
               (struct retro_raster_poll_interface*)data;
         if (     !rc_offer_interface
               || !iface
               || iface->interface_version
                  != RETRO_RASTER_POLL_INTERFACE_VERSION)
            return false;
         iface->raster_poll = rc_raster_poll;
         video_raster_reset(&rc_raster);
         return true;
      }
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         rc_format = *(const enum retro_pixel_format*)data;
         switch (rc_format)
         {
            case RETRO_PIXEL_FORMAT_XRGB8888:
               rc_bpp = 4;
               return true;
            case RETRO_PIXEL_FORMAT_RGB565:
            case RETRO_PIXEL_FORMAT_0RGB1555:
               rc_bpp = 2;
               return true;
            default:
               break;
         }
         return false;
      case RETRO_ENVIRONMENT_SET_VARIABLES:
      {
         const struct retro_variable *var =
               (const struct retro_variable*)data;
         struct rc_option            *opt;
         for (; var && var->key; var++)
         {
            rc_option_default(var);
            if ((opt = rc_option_find(var->key)))
               opt->declared = true;
         }
         return true;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = (struct retro_variable*)data;
         struct rc_option      *opt = rc_option_find(var->key);
         var->value = opt ? opt->value : NULL;
         return opt != NULL;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool*)data       = rc_options_updated;
         rc_options_updated = false;
         return true;
      case RETRO_ENVIRONMENT_GET_CAN_DUPE:
         *(bool*)data = true;
         return true;
      case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char**)data = rc_dir;
         return true;
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback*)data)->log = rc_core_log;
         return true;
      case RETRO_ENVIRONMENT_SET_GEOMETRY:
      case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
      case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
      case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
      case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
         return true;
      default:
         break;
   }
   return false;
}

static void RETRO_CALLCONV rc_input_poll(void) { }

static int16_t RETRO_CALLCONV rc_input_state(unsigned port,
      unsigned device, unsigned index, unsigned id)
{
   (void)index;
   if (port == 0 && (device & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD)
   {
      if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
         return (int16_t)rc_joypad;
      if (id < 16)
         return (int16_t)((rc_joypad >> id) & 1);
   }
   return 0;
}

static void RETRO_CALLCONV rc_audio_sample(int16_t left, int16_t right)
{
   (void)left;
   (void)right;
}

static size_t RETRO_CALLCONV rc_audio_sample_batch(const int16_t *data,
      size_t frames)
{
   (void)data;
   return frames;
}

static rc_func_t rc_sym(void *lib, const char *name)
{
   union
   {
      void     *object;
      rc_func_t func;
   } sym;
   sym.object = dlsym(lib, name);
   if (!sym.object)
      rc_cannot_run("the core lacks", name);
   return sym.func;
}

static void rc_core_open(const char *path)
{
   void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
   if (!lib)
      rc_cannot_run("cannot load the core", dlerror());
   rc_core.api_version = (unsigned (*)(void))
         rc_sym(lib, "retro_api_version");
   rc_core.set_environment = (void (*)(retro_environment_t))
         rc_sym(lib, "retro_set_environment");
   rc_core.set_video_refresh = (void (*)(retro_video_refresh_t))
         rc_sym(lib, "retro_set_video_refresh");
   rc_core.set_audio_sample = (void (*)(retro_audio_sample_t))
         rc_sym(lib, "retro_set_audio_sample");
   rc_core.set_audio_sample_batch = (void (*)(retro_audio_sample_batch_t))
         rc_sym(lib, "retro_set_audio_sample_batch");
   rc_core.set_input_poll = (void (*)(retro_input_poll_t))
         rc_sym(lib, "retro_set_input_poll");
   rc_core.set_input_state = (void (*)(retro_input_state_t))
         rc_sym(lib, "retro_set_input_state");
   rc_core.init = (void (*)(void))rc_sym(lib, "retro_init");
   rc_core.deinit = (void (*)(void))rc_sym(lib, "retro_deinit");
   rc_core.get_system_info = (void (*)(struct retro_system_info*))
         rc_sym(lib, "retro_get_system_info");
   rc_core.get_system_av_info = (void (*)(struct retro_system_av_info*))
         rc_sym(lib, "retro_get_system_av_info");
   rc_core.set_controller_port_device = (void (*)(unsigned, unsigned))
         rc_sym(lib, "retro_set_controller_port_device");
   rc_core.reset = (void (*)(void))rc_sym(lib, "retro_reset");
   rc_core.run = (void (*)(void))rc_sym(lib, "retro_run");
   rc_core.serialize_size = (size_t (*)(void))
         rc_sym(lib, "retro_serialize_size");
   rc_core.serialize = (bool (*)(void*, size_t))
         rc_sym(lib, "retro_serialize");
   rc_core.unserialize = (bool (*)(const void*, size_t))
         rc_sym(lib, "retro_unserialize");
   rc_core.load_game = (bool (*)(const struct retro_game_info*))
         rc_sym(lib, "retro_load_game");
   rc_core.unload_game = (void (*)(void))rc_sym(lib, "retro_unload_game");
}

static void *rc_read_file(const char *path, size_t *size)
{
   long  len;
   void *buf;
   FILE *file = fopen(path, "rb");

   if (!file)
      rc_cannot_run("cannot read", path);
   fseek(file, 0, SEEK_END);
   len = ftell(file);
   fseek(file, 0, SEEK_SET);
   buf = malloc(len > 0 ? (size_t)len : 1);
   if (!buf || len < 0 || fread(buf, 1, (size_t)len, file) != (size_t)len)
      rc_cannot_run("cannot read", path);
   fclose(file);
   *size = (size_t)len;
   return buf;
}

static const char *rc_next_arg(int argc, char **argv, int *i)
{
   if (*i + 1 >= argc)
      rc_cannot_run("a value is missing after", argv[*i]);
   return argv[++*i];
}

/* A frame number ending at `sep`; returns what follows, or NULL */
static const char *rc_parse_frame(const char *arg, char sep,
      unsigned *frame)
{
   char *end;
   *frame = (unsigned)strtoul(arg, &end, 10);
   if (end == arg || *end != sep)
      return NULL;
   return sep ? end + 1 : end;
}

static bool rc_parse_press(const char *arg, struct rc_event *ev)
{
   const char *at = strchr(arg, '@');
   char       *end;
   size_t      i;

   if (!at)
      return false;
   for (i = 0; i < RC_BUTTON_COUNT; i++)
      if (     strlen(rc_button_names[i].name) == (size_t)(at - arg)
            && !strncmp(rc_button_names[i].name, arg, (size_t)(at - arg)))
         break;
   if (i == RC_BUTTON_COUNT)
      return false;
   ev->type   = RC_EVENT_PRESS;
   ev->id     = rc_button_names[i].id;
   ev->frames = 4;
   ev->frame  = (unsigned)strtoul(at + 1, &end, 10);
   if (end == at + 1)
      return false;
   if (*end == '+')
   {
      const char *count = end + 1;
      ev->frames = (unsigned)strtoul(count, &end, 10);
      if (end == count || !ev->frames)
         return false;
   }
   return *end == '\0';
}

static void rc_parse_event(const char *opt, const char *value)
{
   struct rc_event *ev = &rc_events[rc_event_count];

   if (rc_event_count == RC_MAX_EVENTS)
      rc_cannot_run("too many events at", opt);
   memset(ev, 0, sizeof(*ev));
   if (!strcmp(opt, "--press"))
   {
      if (!rc_parse_press(value, ev))
         rc_cannot_run("bad --press", value);
   }
   else if (!strcmp(opt, "--state-at") || !strcmp(opt, "--reset-at"))
   {
      ev->type = strcmp(opt, "--state-at") ? RC_EVENT_RESET : RC_EVENT_STATE;
      if (!rc_parse_frame(value, '\0', &ev->frame))
         rc_cannot_run("bad frame", value);
   }
   else if (!strcmp(opt, "--option-at") || !strcmp(opt, "--dump"))
   {
      ev->type = strcmp(opt, "--dump") ? RC_EVENT_OPTION : RC_EVENT_DUMP;
      ev->arg  = rc_parse_frame(value, ':', &ev->frame);
      if (     !ev->arg
            || !*ev->arg
            || (ev->type == RC_EVENT_OPTION && !strchr(ev->arg, '=')))
         rc_cannot_run("bad value", value);
   }
   else
      rc_cannot_run("unknown option", opt);
   rc_event_count++;
}

int main(int argc, char **argv)
{
   int                         i;
   unsigned                    e;
   unsigned                    frames          = 600;
   const char                 *core_path       = NULL;
   const char                 *content_path    = NULL;
   bool                        expect_polls    = false;
   bool                        expect_no_polls = false;
   bool                        ok              = true;
   unsigned                    port_device[RC_MAX_PORTS];
   bool                        port_set[RC_MAX_PORTS];
   void                       *content         = NULL;
   void                       *state           = NULL;
   size_t                      state_size      = 0;
   clock_t                     start;
   double                      seconds;
   struct retro_system_info    sys;
   struct retro_system_av_info av;
   struct retro_game_info      game;

   memset(port_set, 0, sizeof(port_set));

   for (i = 1; i < argc; i++)
   {
      const char *a = argv[i];

      if (!strcmp(a, "--frames"))
         frames = (unsigned)strtoul(rc_next_arg(argc, argv, &i), NULL, 10);
      else if (!strcmp(a, "--dir"))
         rc_dir = rc_next_arg(argc, argv, &i);
      else if (!strcmp(a, "--set"))
      {
         if (!rc_option_set(rc_next_arg(argc, argv, &i)))
            rc_cannot_run("bad --set", argv[i]);
      }
      else if (!strcmp(a, "--port-device"))
      {
         unsigned    port;
         const char *id = rc_parse_frame(rc_next_arg(argc, argv, &i), '=',
               &port);
         if (!id || port >= RC_MAX_PORTS)
            rc_cannot_run("bad --port-device", argv[i]);
         port_device[port] = (unsigned)strtoul(id, NULL, 10);
         port_set[port]    = true;
      }
      else if (!strcmp(a, "--hash-out"))
      {
         if (!(rc_hash_file = fopen(rc_next_arg(argc, argv, &i), "w")))
            rc_cannot_run("cannot write", argv[i]);
      }
      else if (!strcmp(a, "--hash-in"))
      {
         if (!(rc_hash_in_file = fopen(rc_next_arg(argc, argv, &i), "r")))
            rc_cannot_run("cannot read", argv[i]);
      }
      else if (!strcmp(a, "--no-interface"))
         rc_offer_interface = false;
      else if (!strcmp(a, "--null-poll"))
         rc_null_poll = true;
      else if (!strcmp(a, "--expect-polls"))
         expect_polls = true;
      else if (!strcmp(a, "--expect-no-polls"))
         expect_no_polls = true;
      else if (!strcmp(a, "--verbose"))
         rc_verbose = true;
      else if (!strncmp(a, "--", 2))
         rc_parse_event(a, rc_next_arg(argc, argv, &i));
      else if (!core_path)
         core_path = a;
      else if (!content_path)
         content_path = a;
      else
         rc_cannot_run("unexpected argument", a);
   }
   if (!core_path)
   {
      fprintf(stderr, "usage: raster_poll_conform [options] CORE [CONTENT]\n");
      return 2;
   }

   rc_core_open(core_path);
   rc_core.set_environment(rc_environment);
   rc_core.set_video_refresh(rc_video_refresh);
   rc_core.set_audio_sample(rc_audio_sample);
   rc_core.set_audio_sample_batch(rc_audio_sample_batch);
   rc_core.set_input_poll(rc_input_poll);
   rc_core.set_input_state(rc_input_state);
   if (rc_core.api_version() != RETRO_API_VERSION)
      rc_cannot_run("the core's API version does not match ours", NULL);
   rc_core.init();

   memset(&sys, 0, sizeof(sys));
   rc_core.get_system_info(&sys);
   memset(&game, 0, sizeof(game));
   game.path = content_path;
   if (content_path && !sys.need_fullpath)
   {
      content   = rc_read_file(content_path, &game.size);
      game.data = content;
   }
   if (!rc_core.load_game(content_path ? &game : NULL))
      rc_cannot_run("the core did not load", content_path);
   memset(&av, 0, sizeof(av));
   rc_core.get_system_av_info(&av);
   for (e = 0; e < RC_MAX_PORTS; e++)
      if (port_set[e])
         rc_core.set_controller_port_device(e, port_device[e]);
      else if (e < 2)
         rc_core.set_controller_port_device(e, RETRO_DEVICE_JOYPAD);
   for (e = 0; e < rc_option_count; e++)
      if (!rc_options[e].declared)
         fprintf(stderr,
               "raster_poll_conform: warning: option %s is not declared by the core\n",
               rc_options[e].key);

   start = clock();
   for (rc_frame = 0; rc_frame < frames; rc_frame++)
   {
      rc_joypad = 0;
      for (e = 0; e < rc_event_count; e++)
      {
         const struct rc_event *ev = &rc_events[e];
         switch (ev->type)
         {
            case RC_EVENT_PRESS:
               if (     rc_frame >= ev->frame
                     && rc_frame - ev->frame < ev->frames)
                  rc_joypad |= 1u << ev->id;
               break;
            case RC_EVENT_RESET:
               if (rc_frame == ev->frame)
                  rc_core.reset();
               break;
            case RC_EVENT_OPTION:
               if (rc_frame == ev->frame)
               {
                  struct rc_option *opt = rc_option_set(ev->arg);
                  rc_options_updated = true;
                  if (opt && !opt->declared)
                     fprintf(stderr,
                           "raster_poll_conform: warning: option %s is not "
                           "declared by the core\n", opt->key);
               }
               break;
            case RC_EVENT_STATE:
               if (rc_frame == ev->frame)
               {
                  free(state);
                  state_size = rc_core.serialize_size();
                  state      = malloc(state_size ? state_size : 1);
                  if (state && !rc_core.serialize(state, state_size))
                  {
                     free(state);
                     state = NULL;
                  }
                  if (!state)
                  {
                     printf("frame %u: serialize failed\n", rc_frame);
                     ok = false;
                  }
               }
               else if (rc_frame == ev->frame + RC_STATE_FRAMES
                     && state
                     && !rc_core.unserialize(state, state_size))
               {
                  printf("frame %u: unserialize failed\n", rc_frame);
                  ok = false;
               }
               break;
            case RC_EVENT_DUMP:
               break;
         }
      }
      rc_frame_polls     = 0;
      rc_frame_presented = false;
      rc_in_run          = true;
      rc_run_thread      = pthread_self();
      rc_core.run();
      rc_in_run = false;
      if (rc_frame_polls)
         rc_polled_frames++;
   }
   seconds = (double)(clock() - start) / CLOCKS_PER_SEC;

   if (frames && rc_frame_polls && !rc_frame_presented)
   {
      fprintf(stderr, "frame %u: raster poll never presented\n", frames - 1);
      rc_violations++;
   }

   rc_core.unload_game();
   rc_core.deinit();
   free(content);
   free(state);
   free(rc_shadow);
   if (rc_hash_file)
      fclose(rc_hash_file);

   printf("raster_poll_conform: %u frames, %u presented, %u polled, "
         "%lu polls, %lu rows compared, %lu mismatched rows, "
         "%u violations, %.2f s (%.0f fps)\n",
         frames, rc_presented, rc_polled_frames, rc_polls,
         rc_rows_compared, rc_mismatched_rows, rc_violations,
         seconds, seconds > 0.0 ? frames / seconds : 0.0);
   if (rc_mismatched_rows)
   {
      printf("first mismatch: frame %u, row %u\n",
            rc_first_mismatch_frame, rc_first_mismatch_row);
      ok = false;
   }
   if (rc_violations)
      ok = false;
   if (expect_polls && !rc_polled_frames)
   {
      printf("expected polls, saw none\n");
      ok = false;
   }
   if (expect_no_polls && rc_polled_frames)
   {
      printf("expected no polls, saw %u polled frames\n", rc_polled_frames);
      ok = false;
   }
   if (rc_hash_in_file)
   {
      unsigned leftover = rc_hash_in_leftover();
      printf("hash-in: %u of %u presented frames differ\n",
            rc_hash_in_diffs, rc_presented);
      if (rc_hash_in_diffs)
      {
         printf("first difference: frame %u: expected \"%s\", presented \"%s\"\n",
               rc_hash_in_first_frame, rc_hash_in_first_expected,
               rc_hash_in_first_presented);
         ok = false;
      }
      if (leftover)
      {
         printf("hash-in: %u lines left over\n", leftover);
         ok = false;
      }
      fclose(rc_hash_in_file);
   }
   printf("%s\n", ok ? "PASS" : "FAIL");
   return ok ? 0 : 1;
}
