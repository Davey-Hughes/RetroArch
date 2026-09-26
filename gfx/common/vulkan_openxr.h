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
#include <rthreads/rthreads.h>

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

/* What the headset input needs from a session, valid between the hooks'
 * session_created and session_destroying. */
typedef struct vulkan_openxr_handles
{
   vulkan_openxr_t *xr;
   XrInstance instance;
   XrSystemId system;
   XrSession session;
   XrSpace local_space;
   XrSpace view_space;
   PFN_xrGetInstanceProcAddr get_proc;
} vulkan_openxr_handles_t;

#define VULKAN_OPENXR_MAX_EXTRA_LAYERS 4

typedef struct vulkan_openxr_hooks
{
   /* The session and its spaces exist and its frame loop has not
    * started: suggest bindings and attach action sets here. */
   void (*session_created)(void *user, const vulkan_openxr_handles_t *handles);
   /* The frame loop has stopped; the session is destroyed next. */
   void (*session_destroying)(void *user, vulkan_openxr_t *xr);
   /* The XR thread, each headset frame: up to cap layers drawn over
    * RetroArch's. Returns how many were written. */
   unsigned (*frame_layers)(void *user, XrTime display_time,
         const XrCompositionLayerBaseHeader **layers, unsigned cap);
   void *user;
} vulkan_openxr_hooks_t;

/* Main thread, while no session exists. NULL clears. */
void vulkan_openxr_set_hooks(const vulkan_openxr_hooks_t *hooks);

/* Makes the session on the first call, then (again after a stop)
 * starts the XR thread. Every OpenXR call that may use the queue holds
 * queue_lock. False when the session could not be made. */
bool vulkan_openxr_start(vulkan_openxr_t *xr, VkInstance instance,
      VkPhysicalDevice gpu, VkDevice device, uint32_t queue_family,
      slock_t *queue_lock);

/* Stops the XR thread; the session stays. NULL is fine. */
void vulkan_openxr_stop(vulkan_openxr_t *xr);

/* The session failed on a device made for it: frees, tells the user,
 * and has the next video init skip the runtime. */
void vulkan_openxr_drop_and_reinit(vulkan_openxr_t *xr);

/* Until the runtime ends the session: the headset shows both eyes. */
bool vulkan_openxr_alive(vulkan_openxr_t *xr);

/* The session's focus as the XR thread last saw it, and its latest
 * predicted display time. */
bool vulkan_openxr_focused(vulkan_openxr_t *xr);
XrTime vulkan_openxr_predicted_time(vulkan_openxr_t *xr);

RETRO_END_DECLS

#endif
