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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drm_scanout.h"

/* The kernel's connector names (drm_connector_enum_list), which
 * compositors name outputs by; drmModeGetConnectorTypeName() needs
 * libdrm 2.4.112 */
static const struct
{
   uint32_t type;
   const char *name;
} drm_scanout_types[] = {
   { DRM_MODE_CONNECTOR_Unknown,     "Unknown"   },
   { DRM_MODE_CONNECTOR_VGA,         "VGA"       },
   { DRM_MODE_CONNECTOR_DVII,        "DVI-I"     },
   { DRM_MODE_CONNECTOR_DVID,        "DVI-D"     },
   { DRM_MODE_CONNECTOR_DVIA,        "DVI-A"     },
   { DRM_MODE_CONNECTOR_Composite,   "Composite" },
   { DRM_MODE_CONNECTOR_SVIDEO,      "SVIDEO"    },
   { DRM_MODE_CONNECTOR_LVDS,        "LVDS"      },
   { DRM_MODE_CONNECTOR_Component,   "Component" },
   { DRM_MODE_CONNECTOR_9PinDIN,     "DIN"       },
   { DRM_MODE_CONNECTOR_DisplayPort, "DP"        },
   { DRM_MODE_CONNECTOR_HDMIA,       "HDMI-A"    },
   { DRM_MODE_CONNECTOR_HDMIB,       "HDMI-B"    },
   { DRM_MODE_CONNECTOR_TV,          "TV"        },
   { DRM_MODE_CONNECTOR_eDP,         "eDP"       },
#ifdef DRM_MODE_CONNECTOR_VIRTUAL
   { DRM_MODE_CONNECTOR_VIRTUAL,     "Virtual"   },
#endif
#ifdef DRM_MODE_CONNECTOR_DSI
   { DRM_MODE_CONNECTOR_DSI,         "DSI"       },
#endif
#ifdef DRM_MODE_CONNECTOR_DPI
   { DRM_MODE_CONNECTOR_DPI,         "DPI"       },
#endif
#ifdef DRM_MODE_CONNECTOR_WRITEBACK
   { DRM_MODE_CONNECTOR_WRITEBACK,   "Writeback" },
#endif
#ifdef DRM_MODE_CONNECTOR_SPI
   { DRM_MODE_CONNECTOR_SPI,         "SPI"       },
#endif
#ifdef DRM_MODE_CONNECTOR_USB
   { DRM_MODE_CONNECTOR_USB,         "USB"       },
#endif
};

static const char *drm_scanout_type_name(uint32_t type)
{
   size_t i;
   for (i = 0; i < sizeof(drm_scanout_types)
         / sizeof(drm_scanout_types[0]); i++)
      if (drm_scanout_types[i].type == type)
         return drm_scanout_types[i].name;
   return "Unknown";
}

/* The card whose sysfs node is card<N>-<name>: -1 none, -2 more than
 * one. The kernel numbers each connector type across all cards, so a
 * name should be on one card only; two are refused all the same. */
static int drm_scanout_card(const char *sysfs_root, const char *name)
{
   DIR *dir;
   struct dirent *ent;
   int card = -1;

   if (!(dir = opendir(sysfs_root)))
      return -1;
   while ((ent = readdir(dir)))
   {
      const char *dash;
      char *end;
      long n;

      if (     strncmp(ent->d_name, "card", 4)
            || !(dash = strchr(ent->d_name, '-'))
            || strcmp(dash + 1, name))
         continue;
      n = strtol(ent->d_name + 4, &end, 10);
      if (end != dash || n < 0 || n > 255)
         continue;
      if (card != -1)
      {
         card = -2;
         break;
      }
      card = (int)n;
   }
   closedir(dir);
   return card;
}

static bool drm_scanout_vrr(int fd, uint32_t crtc_id)
{
   uint32_t i;
   bool vrr = false;
   drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd,
         crtc_id, DRM_MODE_OBJECT_CRTC);

   if (!props)
      return false;
   for (i = 0; i < props->count_props; i++)
   {
      drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
      if (!prop)
         continue;
      if (!strcmp(prop->name, "VRR_ENABLED"))
         vrr = props->prop_values[i] != 0;
      drmModeFreeProperty(prop);
   }
   drmModeFreeObjectProperties(props);
   return vrr;
}

/* crtc_id's mode and VRR state, where the beam can be worked out */
static bool drm_scanout_crtc(int fd, uint32_t crtc_id, drm_scanout_t *out)
{
   drmModeModeInfo *m;
   bool timed        = false;
   drmModeCrtc *crtc = drmModeGetCrtc(fd, crtc_id);

   if (!crtc)
      return false;
   m = &crtc->mode;
   if (     crtc->mode_valid
         && m->clock && m->htotal && m->vtotal
         && !(m->flags & (DRM_MODE_FLAG_INTERLACE
               | DRM_MODE_FLAG_DBLSCAN)))
   {
      out->frame_ns = (uint64_t)m->htotal * m->vtotal
            * 1000000 / m->clock;
      out->crtc_id  = crtc_id;
      out->vtotal   = m->vtotal;
      out->vdisplay = m->vdisplay;
      out->vrr      = drm_scanout_vrr(fd, crtc_id);
      timed         = true;
   }
   drmModeFreeCrtc(crtc);
   return timed;
}

int drm_scanout_open(const char *sysfs_root, const char *dev_root,
      const char *name, drm_scanout_t *out)
{
   char path[256];
   int card;
   int fd;
   int i;
   bool found      = false;
   drmModeRes *res = NULL;

   if (     !name || !*name
         || (card = drm_scanout_card(sysfs_root, name)) < 0)
      return -1;

   snprintf(path, sizeof(path), "%s/card%d", dev_root, card);
   if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
      return -1;

   /* Opened while no compositor held the card, this descriptor became
    * its master, and kept open it would lock the compositor out.
    * AUTH_MAGIC needs master: drmIsMaster(), which older libdrm lacks. */
   if (drmAuthMagic(fd, 0) != -EACCES)
   {
      close(fd);
      return -1;
   }

   if ((res = drmModeGetResources(fd)))
   {
      for (i = 0; i < res->count_connectors && !found; i++)
      {
         char conn_name[32];
         drmModeEncoder *enc;
         drmModeConnector *conn = drmModeGetConnectorCurrent(fd,
               res->connectors[i]);

         if (!conn)
            continue;
         snprintf(conn_name, sizeof(conn_name), "%s-%u",
               drm_scanout_type_name(conn->connector_type),
               conn->connector_type_id);
         if (     strcmp(conn_name, name)
               || !conn->encoder_id
               || !(enc = drmModeGetEncoder(fd, conn->encoder_id)))
         {
            drmModeFreeConnector(conn);
            continue;
         }
         if (enc->crtc_id && drm_scanout_crtc(fd, enc->crtc_id, out))
         {
            out->connector_id = conn->connector_id;
            out->card         = card;
            found             = true;
         }
         drmModeFreeEncoder(enc);
         drmModeFreeConnector(conn);
      }
      drmModeFreeResources(res);
   }
   if (!found)
   {
      close(fd);
      return -1;
   }
   return fd;
}

bool drm_scanout_update(int fd, drm_scanout_t *s)
{
   bool routed            = false;
   drmModeConnector *conn = drmModeGetConnectorCurrent(fd,
         s->connector_id);

   /* The compositor can move an output to another CRTC */
   if (conn)
   {
      drmModeEncoder *enc = conn->encoder_id
            ? drmModeGetEncoder(fd, conn->encoder_id) : NULL;
      if (enc)
      {
         routed = enc->crtc_id == s->crtc_id;
         drmModeFreeEncoder(enc);
      }
      drmModeFreeConnector(conn);
   }
   return routed && drm_scanout_crtc(fd, s->crtc_id, s);
}
