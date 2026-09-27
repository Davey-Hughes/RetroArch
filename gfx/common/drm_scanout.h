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

#ifndef __DRM_SCANOUT_H
#define __DRM_SCANOUT_H

#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct drm_scanout
{
   uint64_t frame_ns; /* htotal * vtotal * 1000000 / clock */
   uint32_t connector_id;
   uint32_t crtc_id;
   unsigned vtotal;
   unsigned vdisplay;
   int      card;
   bool     vrr;      /* the CRTC's VRR_ENABLED */
} drm_scanout_t;

/* The scanout timing of the connector named name ("DP-1", as
 * wl_output v4 names it), read through a non-master descriptor beside
 * the compositor. sysfs_root is normally "/sys/class/drm" and dev_root
 * "/dev/dri". Returns that descriptor, which the caller closes, or -1
 * with nothing left open when the name is on no card or on more than
 * one, no compositor holds the card, the connector is idle, a read
 * fails, or the mode is interlaced or doublescan. */
int drm_scanout_open(const char *sysfs_root, const char *dev_root,
      const char *name, drm_scanout_t *out);

/* Reads s->crtc_id's mode and VRR state again through fd, the
 * descriptor drm_scanout_open() returned. False, leaving s as it was,
 * when the connector no longer drives that CRTC, or the CRTC is off
 * or its mode is untimed, interlaced or doublescan. */
bool drm_scanout_update(int fd, drm_scanout_t *s);

RETRO_END_DECLS

#endif
