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

#ifndef __VULKAN_OPENXR_H
#define __VULKAN_OPENXR_H

#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

#include "../include/vulkan/vulkan.h"

#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include "../include/openxr/openxr.h"
#include "../include/openxr/openxr_platform.h"

RETRO_BEGIN_DECLS

/* Headset output through OpenXR: the runtime's instance, the Vulkan
 * instance and device it asks for, its session, and the thread that
 * runs the headset's frame loop. The loader is opened at run time. */
typedef struct vulkan_openxr vulkan_openxr_t;

#define VULKAN_OPENXR_MAX_EXTS 16

/* The runtime's instance and headset, or NULL after a notification.
 * enable1 is for a core that makes its own device with create_device
 * (v1): the runtime then only lists what the instance and device
 * need. api_version is the Vulkan version the instance will ask for. */
vulkan_openxr_t *vulkan_openxr_new(bool enable1, uint32_t api_version);

/* Before the Vulkan device is destroyed. NULL is fine. */
void vulkan_openxr_free(vulkan_openxr_t *xr);

/* OpenXR failed before the device existed: log, tell the user, free. */
void vulkan_openxr_drop(vulkan_openxr_t *xr);

/* A kept Vulkan context cannot take the runtime in: tell the user. */
void vulkan_openxr_needs_reload(void);

bool vulkan_openxr_uses_enable2(const vulkan_openxr_t *xr);

/* XR_KHR_vulkan_enable: the extensions the runtime needs, valid until
 * vulkan_openxr_free(). Returns how many were written. */
unsigned vulkan_openxr_instance_extensions(const vulkan_openxr_t *xr,
      const char **out, unsigned cap);
unsigned vulkan_openxr_device_extensions(const vulkan_openxr_t *xr,
      const char **out, unsigned cap);

/* XR_KHR_vulkan_enable2: vkCreateInstance and vkCreateDevice through
 * the runtime, which adds what it needs. */
VkResult vulkan_openxr_create_instance(vulkan_openxr_t *xr,
      PFN_vkGetInstanceProcAddr gipa, const VkInstanceCreateInfo *info,
      VkInstance *instance);
VkResult vulkan_openxr_create_device(vulkan_openxr_t *xr,
      PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice gpu,
      const VkDeviceCreateInfo *info, VkDevice *device);

/* The GPU the headset is attached to, or VK_NULL_HANDLE. */
VkPhysicalDevice vulkan_openxr_gpu(vulkan_openxr_t *xr, VkInstance instance);

RETRO_END_DECLS

#endif
