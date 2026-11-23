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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <gfx/scaler/pixconv.h>

#include "video_screen_images.h"
#include "gfx_surface.h"
#include "video_defines.h"
#include "../verbosity.h"

#define VIDEO_SCREEN_IMAGES_KEPT   (1u << 0)  /* the last picture taken */
#define VIDEO_SCREEN_IMAGES_LENT   (1u << 1)
#define VIDEO_SCREEN_IMAGES_PINNED (1u << 2)  /* read by an upload in flight */
#define VIDEO_SCREEN_IMAGES_RGBA   (1u << 3)  /* kept in memory order R,G,B,X */

/* One screen of one core instance */
typedef struct video_screen_images_screen
{
   uint32_t *slot[VIDEO_SCREEN_IMAGES_SLOTS];      /* XRGB8888, packed */
   unsigned slot_dims[VIDEO_SCREEN_IMAGES_SLOTS];  /* VIDEO_SCALE_PACK */
   uint8_t slot_flags[VIDEO_SCREEN_IMAGES_SLOTS];
   unsigned serial;        /* the kept picture's */
   unsigned format;        /* the kept picture's, as the core sent it */
   unsigned kept_gen;
   unsigned uploaded_gen;
   unsigned sent_gen;      /* the upload in flight's */
   unsigned failed_gen;    /* not tried again */
   gfx_surface_t *surface;
   gfx_surface_t *old;     /* drawn until surface has a texture */
   unsigned surface_dims;
} video_screen_images_screen_t;

static video_screen_images_screen_t video_screen_images_store
      [VIDEO_SCREEN_IMAGES_OWNERS][RETRO_VIDEO_SCREENS_MAX];
static unsigned video_screen_images_caller;
static unsigned video_screen_images_shown;

/* The slot of @scr with @flag, or -1 */
static int video_screen_images_with(const video_screen_images_screen_t *scr,
      unsigned flag)
{
   int i;
   for (i = 0; i < VIDEO_SCREEN_IMAGES_SLOTS; i++)
      if (scr->slot_flags[i] & flag)
         return i;
   return -1;
}

/* A slot of @scr with none of @busy: one of @dims first, then an empty
 * one, then any; -1 when every slot is busy */
static int video_screen_images_pick(const video_screen_images_screen_t *scr,
      unsigned dims, unsigned busy)
{
   int i;
   int pick = -1;
   for (i = 0; i < VIDEO_SCREEN_IMAGES_SLOTS; i++)
   {
      if (scr->slot_flags[i] & busy)
         continue;
      if (scr->slot[i] && scr->slot_dims[i] == dims)
         return i;
      if (pick < 0 || (scr->slot[pick] && !scr->slot[i]))
         pick = i;
   }
   return pick;
}

static uint32_t *video_screen_images_alloc(unsigned dims)
{
   return (uint32_t*)malloc((size_t)VIDEO_SCALE_W(dims)
         * VIDEO_SCALE_H(dims) * sizeof(uint32_t));
}

/* Whether @data points into a slot of either store; its screen and slot */
static bool video_screen_images_find(const void *data, unsigned *screen,
      int *slot)
{
   unsigned o, s;
   int i;
   for (o = 0; o < VIDEO_SCREEN_IMAGES_OWNERS; o++)
   {
      const video_screen_images_screen_t *store =
            video_screen_images_store[o];
      for (s = 0; s < RETRO_VIDEO_SCREENS_MAX; s++)
      {
         for (i = 0; i < VIDEO_SCREEN_IMAGES_SLOTS; i++)
         {
            unsigned dims = store[s].slot_dims[i];
            size_t len    = (size_t)VIDEO_SCALE_W(dims)
                  * VIDEO_SCALE_H(dims) * sizeof(uint32_t);
            if (     store[s].slot[i]
                  && (uintptr_t)data - (uintptr_t)store[s].slot[i] < len)
            {
               *screen = s;
               *slot   = i;
               return true;
            }
         }
      }
   }
   return false;
}

/* @im's pixels into @dst packed, as XRGB8888 or with @rgba as XBGR8888 */
static void video_screen_images_copy(uint32_t *dst,
      const struct retro_video_screen_image *im, bool rgba)
{
   unsigned x, y;
   const uint8_t *src = (const uint8_t*)im->data;
   for (y = 0; y < im->height; y++, src += im->pitch, dst += im->width)
   {
      if (im->format == RETRO_PIXEL_FORMAT_RGB565)
      {
         const uint16_t *in = (const uint16_t*)src;
         if (rgba)
            for (x = 0; x < im->width; x++)
               dst[x] = pixconv_rgb565_to_xbgr8888(in[x]);
         else
            for (x = 0; x < im->width; x++)
               dst[x] = pixconv_rgb565_to_xrgb8888(in[x]);
      }
      else if (rgba)
         conv_argb8888_abgr8888(dst, src, (int)im->width, 1, 0, 0);
      else
         memcpy(dst, src, im->width * sizeof(uint32_t));
   }
}

/* A packed picture of @dims into the other channel order, in place */
static void video_screen_images_swap(uint32_t *pixels, unsigned dims)
{
   conv_argb8888_abgr8888(pixels, pixels,
         (int)(VIDEO_SCALE_W(dims) * VIDEO_SCALE_H(dims)), 1, 0, 0);
}

/* @scr's surfaces go; a slot an upload still reads goes with its
 * surface */
static void video_screen_images_unsurface(video_screen_images_screen_t *scr)
{
   int pinned = video_screen_images_with(scr, VIDEO_SCREEN_IMAGES_PINNED);
   if (pinned < 0)
      gfx_surface_free(scr->surface);
   else if (gfx_surface_free_adopt(scr->surface, scr->slot[pinned]))
   {
      scr->slot[pinned]       = NULL;
      scr->slot_dims[pinned]  = 0;
      scr->slot_flags[pinned] = 0;
   }
   else
      scr->slot_flags[pinned] &= ~VIDEO_SCREEN_IMAGES_PINNED;
   gfx_surface_free(scr->old);
   scr->surface      = NULL;
   scr->old          = NULL;
   scr->surface_dims = 0;
}

/* @scr loses its picture and textures. A lent slot stays the core's. */
static void video_screen_images_drop(video_screen_images_screen_t *scr)
{
   int i;
   video_screen_images_unsurface(scr);
   for (i = 0; i < VIDEO_SCREEN_IMAGES_SLOTS; i++)
   {
      if (scr->slot_flags[i] & VIDEO_SCREEN_IMAGES_LENT)
         scr->slot_flags[i] &= ~VIDEO_SCREEN_IMAGES_KEPT;
      else if (scr->slot[i])
      {
         free(scr->slot[i]);
         scr->slot[i]       = NULL;
         scr->slot_dims[i]  = 0;
         scr->slot_flags[i] = 0;
      }
   }
   scr->serial = 0;
   scr->format = 0;
}

void video_screen_images_set_caller(unsigned owner)
{
   if (owner < VIDEO_SCREEN_IMAGES_OWNERS)
      video_screen_images_caller = owner;
}

bool video_screen_images_lend(struct retro_video_screen_buffer *buf,
      bool images)
{
   video_screen_images_screen_t *scr;
   unsigned dims;
   int lent;
   int pick;

   if (     !images
         || buf->screen >= RETRO_VIDEO_SCREENS_MAX
         || !buf->width  || buf->width  > VIDEO_VIEWS_IMAGE_MAX
         || !buf->height || buf->height > VIDEO_VIEWS_IMAGE_MAX
         || buf->format != RETRO_PIXEL_FORMAT_XRGB8888)
      return false;
   scr  = &video_screen_images_store[video_screen_images_caller][buf->screen];
   dims = VIDEO_SCALE_PACK(buf->width, buf->height);
   /* A free slot or the lend this call takes back */
   pick = video_screen_images_pick(scr, dims,
         VIDEO_SCREEN_IMAGES_KEPT | VIDEO_SCREEN_IMAGES_PINNED);
   if (pick < 0)
      return false;
   if (!scr->slot[pick] || scr->slot_dims[pick] != dims)
   {
      uint32_t *made = video_screen_images_alloc(dims);
      if (!made)
         return false;
      free(scr->slot[pick]);
      scr->slot[pick]      = made;
      scr->slot_dims[pick] = dims;
   }
   lent = video_screen_images_with(scr, VIDEO_SCREEN_IMAGES_LENT);
   if (lent >= 0)
      scr->slot_flags[lent] &= ~VIDEO_SCREEN_IMAGES_LENT;
   scr->slot_flags[pick] |= VIDEO_SCREEN_IMAGES_LENT;
   buf->data  = scr->slot[pick];
   buf->pitch = buf->width * sizeof(uint32_t);
   return true;
}

bool video_screen_images_set(const struct retro_video_views *in,
      const video_views_map_t *map)
{
   video_screen_images_screen_t *store =
         video_screen_images_store[video_screen_images_caller];
   int take[RETRO_VIDEO_SCREENS_MAX];         /* slot to keep; -1 none */
   uint32_t *made[RETRO_VIDEO_SCREENS_MAX];   /* that slot remade */
   unsigned s;

   for (s = 0; s < RETRO_VIDEO_SCREENS_MAX; s++)
   {
      take[s] = -1;
      made[s] = NULL;
   }

   for (s = 0; s < RETRO_VIDEO_SCREENS_MAX; s++)
   {
      const struct retro_video_screen_image *im;
      video_screen_images_screen_t *scr = &store[s];
      unsigned dims;
      unsigned found;
      int kept;
      int slot;
      if (!(map->image & (1u << s)))
         continue;
      im   = in->screens[s].image;
      dims = VIDEO_SCALE_PACK(im->width, im->height);
      kept = video_screen_images_with(scr, VIDEO_SCREEN_IMAGES_KEPT);
      if (kept >= 0 && im->serial == scr->serial)
      {
         if (dims != scr->slot_dims[kept] || im->format != scr->format)
            goto refuse;
         continue;
      }
      if (!im->data)
         goto refuse;
      if (video_screen_images_find(im->data, &found, &slot))
      {
         /* Only the caller's lend for this screen, as it was lent */
         if (     found != s
               || !(scr->slot_flags[slot] & VIDEO_SCREEN_IMAGES_LENT)
               || im->data   != scr->slot[slot]
               || dims       != scr->slot_dims[slot]
               || im->format != RETRO_PIXEL_FORMAT_XRGB8888
               || im->pitch  != im->width * sizeof(uint32_t))
            goto refuse;
         take[s] = slot;
         continue;
      }
      /* The core's own pixels: a copy into a free slot, else the kept
       * one */
      slot = video_screen_images_pick(scr, dims, VIDEO_SCREEN_IMAGES_KEPT
            | VIDEO_SCREEN_IMAGES_LENT | VIDEO_SCREEN_IMAGES_PINNED);
      if (     slot < 0 && kept >= 0
            && !(scr->slot_flags[kept] & VIDEO_SCREEN_IMAGES_PINNED))
         slot = kept;
      if (slot < 0)
         goto refuse;
      if (!scr->slot[slot] || scr->slot_dims[slot] != dims)
      {
         if (!(made[s] = video_screen_images_alloc(dims)))
            goto refuse;
      }
      take[s] = slot;
   }

   for (s = 0; s < RETRO_VIDEO_SCREENS_MAX; s++)
   {
      const struct retro_video_screen_image *im;
      video_screen_images_screen_t *scr = &store[s];
      unsigned dims;
      unsigned held;
      bool rgba;
      int kept;
      int slot = take[s];
      if (!(map->image & (1u << s)))
      {
         video_screen_images_drop(scr);
         continue;
      }
      if (slot < 0)
         continue;
      im   = in->screens[s].image;
      dims = VIDEO_SCALE_PACK(im->width, im->height);
      kept = video_screen_images_with(scr, VIDEO_SCREEN_IMAGES_KEPT);
      held = kept >= 0 ? scr->slot_dims[kept] : 0;
      if (made[s])
      {
         free(scr->slot[slot]);
         scr->slot[slot]      = made[s];
         scr->slot_dims[slot] = dims;
      }
      rgba = gfx_surface_wants_rgba();
      if (!(scr->slot_flags[slot] & VIDEO_SCREEN_IMAGES_LENT))
         video_screen_images_copy(scr->slot[slot], im, rgba);
      else if (rgba)
         video_screen_images_swap(scr->slot[slot], dims);
      if (held != dims)
         RARCH_LOG("[Video] Screen %u image: %ux%u.\n",
               s, im->width, im->height);
      if (kept >= 0)
         scr->slot_flags[kept] &= ~VIDEO_SCREEN_IMAGES_KEPT;
      scr->slot_flags[slot] = VIDEO_SCREEN_IMAGES_KEPT
            | (rgba ? VIDEO_SCREEN_IMAGES_RGBA : 0);
      scr->serial           = im->serial;
      scr->format           = im->format;
      scr->kept_gen++;
   }
   video_screen_images_shown = video_screen_images_caller;
   return true;

refuse:
   for (s = 0; s < RETRO_VIDEO_SCREENS_MAX; s++)
      free(made[s]);
   return false;
}

/* @scr's upload of generation @gen ended, on its surface's texture or
 * not */
static void video_screen_images_settle(video_screen_images_screen_t *scr,
      bool landed, unsigned gen)
{
   if (!landed)
   {
      scr->failed_gen = gen;
      return;
   }
   scr->uploaded_gen = gen;
   gfx_surface_free(scr->old);
   scr->old = NULL;
}

/* Main thread, from video_thread_async_poll: a queued upload landed or
 * its load failed */
static void video_screen_images_released(void *user, gfx_surface_t *s,
      unsigned slot)
{
   video_screen_images_screen_t *scr = (video_screen_images_screen_t*)user;
   int pinned = video_screen_images_with(scr, VIDEO_SCREEN_IMAGES_PINNED);
   (void)slot;
   if (pinned >= 0)
      scr->slot_flags[pinned] &= ~VIDEO_SCREEN_IMAGES_PINNED;
   video_screen_images_settle(scr, !s->dropped, scr->sent_gen);
}

/* @scr's kept picture up, when it is newer than the one up and no upload
 * is in flight */
static void video_screen_images_upload(video_screen_images_screen_t *scr)
{
   gfx_surface_src_t src;
   unsigned dims;
   bool rgba;
   int kept = video_screen_images_with(scr, VIDEO_SCREEN_IMAGES_KEPT);
   if (     kept < 0
         || scr->kept_gen == scr->uploaded_gen
         || scr->kept_gen == scr->failed_gen
         || video_screen_images_with(scr, VIDEO_SCREEN_IMAGES_PINNED) >= 0)
      return;
   dims = scr->slot_dims[kept];
   if (!scr->surface || scr->surface_dims != dims)
   {
      gfx_surface_t *made = gfx_surface_new_static(dims,
            TEXTURE_FILTER_NEAREST);
      if (!made)
      {
         scr->failed_gen = scr->kept_gen;
         return;
      }
      if (GFX_SURFACE_HANDLE(scr->surface))
      {
         gfx_surface_free(scr->old);
         scr->old = scr->surface;
      }
      else
         gfx_surface_free(scr->surface);
      scr->surface      = made;
      scr->surface_dims = dims;
   }
   /* Taken under a driver that wanted the other order */
   rgba = gfx_surface_wants_rgba();
   if (rgba != !!(scr->slot_flags[kept] & VIDEO_SCREEN_IMAGES_RGBA))
   {
      video_screen_images_swap(scr->slot[kept], dims);
      scr->slot_flags[kept] ^= VIDEO_SCREEN_IMAGES_RGBA;
   }
   memset(&src, 0, sizeof(src));
   src.pixels = scr->slot[kept];
   src.pixfmt = IMAGE_PIXFMT_8888;
   src.rgba   = rgba;
   switch (gfx_surface_submit_external(scr->surface, &src,
            video_screen_images_released, scr))
   {
      case GFX_SURFACE_SUBMIT_DONE:
         video_screen_images_settle(scr, true, scr->kept_gen);
         break;
      case GFX_SURFACE_SUBMIT_QUEUED:
         scr->slot_flags[kept] |= VIDEO_SCREEN_IMAGES_PINNED;
         scr->sent_gen          = scr->kept_gen;
         break;
      case GFX_SURFACE_SUBMIT_FAILED:
         scr->failed_gen = scr->kept_gen;
         break;
      default: /* BUSY or DROPPED: the next shown frame */
         break;
   }
}

void video_screen_images_frame(video_views_map_t *map, unsigned draw,
      unsigned nearest)
{
   video_screen_images_screen_t *store =
         video_screen_images_store[video_screen_images_shown];
   unsigned s;
   for (s = 0; s < RETRO_VIDEO_SCREENS_MAX; s++)
   {
      video_screen_images_screen_t *scr = &store[s];
      uintptr_t tex                     = 0;
      if (draw & (1u << s))
      {
         video_screen_images_upload(scr);
         if (!(tex = GFX_SURFACE_HANDLE(scr->surface)))
            tex = GFX_SURFACE_HANDLE(scr->old);
      }
      map->image_tex[s] = tex;
      map->image_gen[s] = tex ? (uint8_t)scr->uploaded_gen : 0;
   }
   map->nearest = nearest & map->image;
}

void video_screen_images_driver_free(void)
{
   unsigned o, s;
   int i;
   for (o = 0; o < VIDEO_SCREEN_IMAGES_OWNERS; o++)
      for (s = 0; s < RETRO_VIDEO_SCREENS_MAX; s++)
      {
         video_screen_images_screen_t *scr = &video_screen_images_store[o][s];
         /* Nothing writes a slot before the driver and its thread are
          * gone */
         for (i = 0; i < VIDEO_SCREEN_IMAGES_SLOTS; i++)
            scr->slot_flags[i] &= ~VIDEO_SCREEN_IMAGES_PINNED;
         video_screen_images_unsurface(scr);
         /* Neither up nor refused on the next driver */
         scr->uploaded_gen = scr->kept_gen - 1;
         scr->failed_gen   = scr->uploaded_gen;
      }
}

void video_screen_images_free_owner(unsigned owner)
{
   unsigned s;
   int i;
   if (owner >= VIDEO_SCREEN_IMAGES_OWNERS)
      return;
   for (s = 0; s < RETRO_VIDEO_SCREENS_MAX; s++)
   {
      video_screen_images_screen_t *scr =
            &video_screen_images_store[owner][s];
      video_screen_images_unsurface(scr);
      for (i = 0; i < VIDEO_SCREEN_IMAGES_SLOTS; i++)
         if (scr->slot[i])
            free(scr->slot[i]);
   }
   memset(video_screen_images_store[owner], 0,
         sizeof(video_screen_images_store[owner]));
   /* Main's pictures drawn until a map is taken */
   if (video_screen_images_shown == owner)
      video_screen_images_shown = VIDEO_SCREEN_IMAGES_MAIN;
}

void video_screen_images_clear(void)
{
   unsigned o;
   for (o = 0; o < VIDEO_SCREEN_IMAGES_OWNERS; o++)
      video_screen_images_free_owner(o);
}
