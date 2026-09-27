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

#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <compat/strl.h>
#include <dynamic/dylib.h>
#include <retro_atomic.h>
#include <retro_timers.h>
#include <string/stdstring.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "vulkan_openxr.h"
#include "../video_defines.h"

#include "../../msg_hash.h"
#include "../../runloop.h"
#include "../../verbosity.h"

#define VULKAN_OPENXR_EXT_BUF 1024
#define VULKAN_OPENXR_MAX_FORMATS 64

struct vulkan_openxr_slot
{
   XrSwapchain swapchain;       /* changed under the queue lock and lock */
   unsigned dims;
   unsigned layers;
   uint32_t index;              /* video thread */
   bool acquired;
   bool waited;
   retro_atomic_int_t content;  /* an image has been released */
};

struct vulkan_openxr
{
   dylib_t lib;
   XrInstance instance;
   XrSystemId system;
   XrEnvironmentBlendMode blend_mode;
   unsigned max_dim;
   uint32_t rec_width;
   bool enable2;
   /* The session is made on the thread that makes the device; its frame
    * loop runs on the XR thread. lock guards what both read. */
   XrSession session;
   XrSpace local_space;
   XrSpace view_space;
   VkDevice device;
   slock_t *lock;
   slock_t *queue_lock;
   sthread_t *thread;
   XrTime predicted_time;          /* lock */
   float px_per_rad;               /* lock; the XR thread writes it */
   retro_atomic_int_t quit;
   retro_atomic_int_t state;       /* XrSessionState */
   retro_atomic_int_t alive;
   bool running;                   /* XR thread */
   bool ended;                     /* XR thread, or while it is stopped */
   bool frame_failed;              /* XR thread */
   int64_t formats[VULKAN_OPENXR_MAX_FORMATS];
   uint32_t num_formats;
   struct vulkan_openxr_slot slots[VIDEO_XR_MAX_SLOTS];
   video_xr_quad_set_t quads;      /* lock */
   video_xr_pose_t anchor;         /* lock */

   /* XR_KHR_vulkan_enable's lists, split in place. */
   char inst_ext_buf[VULKAN_OPENXR_EXT_BUF];
   char dev_ext_buf[VULKAN_OPENXR_EXT_BUF];
   const char *inst_exts[VULKAN_OPENXR_MAX_EXTS];
   const char *dev_exts[VULKAN_OPENXR_MAX_EXTS];
   unsigned num_inst_exts;
   unsigned num_dev_exts;

   PFN_xrGetInstanceProcAddr GetInstanceProcAddr;
   PFN_xrDestroyInstance DestroyInstance;
   PFN_xrPollEvent PollEvent;
   PFN_xrCreateSession CreateSession;
   PFN_xrDestroySession DestroySession;
   PFN_xrBeginSession BeginSession;
   PFN_xrEndSession EndSession;
   PFN_xrCreateReferenceSpace CreateReferenceSpace;
   PFN_xrDestroySpace DestroySpace;
   PFN_xrLocateViews LocateViews;
   PFN_xrWaitFrame WaitFrame;
   PFN_xrBeginFrame BeginFrame;
   PFN_xrEndFrame EndFrame;
   PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats;
   PFN_xrCreateSwapchain CreateSwapchain;
   PFN_xrDestroySwapchain DestroySwapchain;
   PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages;
   PFN_xrAcquireSwapchainImage AcquireSwapchainImage;
   PFN_xrWaitSwapchainImage WaitSwapchainImage;
   PFN_xrReleaseSwapchainImage ReleaseSwapchainImage;
   PFN_xrGetVulkanGraphicsRequirements2KHR GetVulkanGraphicsRequirements2KHR;
   PFN_xrCreateVulkanInstanceKHR CreateVulkanInstanceKHR;
   PFN_xrCreateVulkanDeviceKHR CreateVulkanDeviceKHR;
   PFN_xrGetVulkanGraphicsDevice2KHR GetVulkanGraphicsDevice2KHR;
   PFN_xrGetVulkanGraphicsRequirementsKHR GetVulkanGraphicsRequirementsKHR;
   PFN_xrGetVulkanGraphicsDeviceKHR GetVulkanGraphicsDeviceKHR;
   PFN_xrGetVulkanInstanceExtensionsKHR GetVulkanInstanceExtensionsKHR;
   PFN_xrGetVulkanDeviceExtensionsKHR GetVulkanDeviceExtensionsKHR;
};

static PFN_xrVoidFunction vulkan_openxr_proc(const vulkan_openxr_t *xr,
      const char *name)
{
   PFN_xrVoidFunction fn = NULL;
   if (xr->GetInstanceProcAddr(xr->instance, name, &fn) != XR_SUCCESS)
      return NULL;
   return fn;
}

/* An instance function into the field named like it, without "xr". */
#define VULKAN_OPENXR_FN(xr, name) \
   ((xr)->name = (PFN_xr##name)vulkan_openxr_proc((xr), "xr" #name))

static vulkan_openxr_hooks_t vulkan_openxr_hooks;
/* Set after a session fails on a device made for it: the reinit that
 * follows builds the device without the runtime. */
static bool vulkan_openxr_skip_once;

static void vulkan_openxr_notify(enum msg_hash_enums msg)
{
   const char *s = msg_hash_to_str(msg);
   runloop_msg_queue_push(s, strlen(s), 2, 240, false, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_WARNING);
}

static bool vulkan_openxr_has_extension(
      PFN_xrEnumerateInstanceExtensionProperties enum_exts,
      uint32_t count, const char *name)
{
   uint32_t i;
   bool found                   = false;
   XrExtensionProperties *props = (XrExtensionProperties*)
      calloc(count, sizeof(*props));
   if (!props)
      return false;
   for (i = 0; i < count; i++)
      props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
   if (XR_SUCCEEDED(enum_exts(NULL, count, &count, props)))
      for (i = 0; i < count && !found; i++)
         found = string_is_equal(props[i].extensionName, name);
   free(props);
   return found;
}

/* Splits a space-separated list in buf into names; false when there
 * are more than VULKAN_OPENXR_MAX_EXTS. */
static bool vulkan_openxr_split(char *buf, const char **names,
      unsigned *count)
{
   unsigned n = 0;
   char *p    = buf;
   for (;;)
   {
      while (*p == ' ')
         *p++ = '\0';
      if (!*p)
         break;
      if (n == VULKAN_OPENXR_MAX_EXTS)
         return false;
      names[n++] = p;
      while (*p && *p != ' ')
         p++;
   }
   *count = n;
   return true;
}

/* False rather than a list cut short, which would leave the runtime
 * without extensions it needs. */
static bool vulkan_openxr_list(vulkan_openxr_t *xr,
      PFN_xrGetVulkanInstanceExtensionsKHR get, char *buf,
      const char **names, unsigned *count)
{
   uint32_t len = 0;
   *count       = 0;
   if (     XR_FAILED(get(xr->instance, xr->system, 0, &len, NULL))
         || len > VULKAN_OPENXR_EXT_BUF)
      return false;
   if (!len)
      return true;
   if (XR_FAILED(get(xr->instance, xr->system, len, &len, buf)))
      return false;
   buf[VULKAN_OPENXR_EXT_BUF - 1] = '\0';
   return vulkan_openxr_split(buf, names, count);
}

vulkan_openxr_t *vulkan_openxr_new(bool enable1, uint32_t api_version)
{
   XrResult res;
   XrVersion api;
   uint32_t count = 0;
   XrInstanceCreateInfo ici;
   XrSystemGetInfo sgi;
   XrSystemProperties props;
   XrViewConfigurationView views[2];
   XrEnvironmentBlendMode modes[8];
   XrGraphicsRequirementsVulkanKHR reqs;
   const char *ext;
   PFN_xrEnumerateInstanceExtensionProperties enum_exts;
   PFN_xrCreateInstance create_instance;
   PFN_xrGetSystem get_system;
   PFN_xrGetSystemProperties get_system_properties;
   PFN_xrEnumerateViewConfigurationViews enum_views;
   PFN_xrEnumerateEnvironmentBlendModes enum_modes;
   vulkan_openxr_t *xr;

   if (vulkan_openxr_skip_once)
   {
      vulkan_openxr_skip_once = false;
      RARCH_LOG("[OpenXR] Starting once without headset output after a failure.\n");
      return NULL;
   }
   if (!(xr = (vulkan_openxr_t*)calloc(1, sizeof(*xr))))
      return NULL;
   xr->enable2 = !enable1;
   ext         = enable1 ? XR_KHR_VULKAN_ENABLE_EXTENSION_NAME
                         : XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;

   if (!(xr->lib = dylib_load("libopenxr_loader.so.1")))
   {
      RARCH_WARN("[OpenXR] No runtime (no libopenxr_loader.so.1).\n");
      goto unavailable;
   }
   xr->GetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)
      dylib_proc(xr->lib, "xrGetInstanceProcAddr");
   if (!xr->GetInstanceProcAddr)
   {
      RARCH_WARN("[OpenXR] No runtime (the loader lacks xrGetInstanceProcAddr).\n");
      goto unavailable;
   }
   enum_exts       = (PFN_xrEnumerateInstanceExtensionProperties)
      vulkan_openxr_proc(xr, "xrEnumerateInstanceExtensionProperties");
   create_instance = (PFN_xrCreateInstance)
      vulkan_openxr_proc(xr, "xrCreateInstance");
   if (!enum_exts || !create_instance)
   {
      RARCH_WARN("[OpenXR] No runtime (the loader lacks xrCreateInstance or xrEnumerateInstanceExtensionProperties).\n");
      goto unavailable;
   }

   if (XR_FAILED(res = enum_exts(NULL, 0, &count, NULL)))
   {
      RARCH_WARN("[OpenXR] No runtime (xrEnumerateInstanceExtensionProperties: %d).\n",
            (int)res);
      goto unavailable;
   }
   if (!vulkan_openxr_has_extension(enum_exts, count, ext))
   {
      RARCH_WARN("[OpenXR] The runtime lacks %s.\n", ext);
      goto unavailable;
   }

   memset(&ici, 0, sizeof(ici));
   ici.type                       = XR_TYPE_INSTANCE_CREATE_INFO;
   strlcpy(ici.applicationInfo.applicationName, "RetroArch",
         sizeof(ici.applicationInfo.applicationName));
   strlcpy(ici.applicationInfo.engineName, "RetroArch",
         sizeof(ici.applicationInfo.engineName));
   ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
   ici.enabledExtensionCount      = 1;
   ici.enabledExtensionNames      = &ext;
   if (XR_FAILED(res = create_instance(&ici, &xr->instance)))
   {
      RARCH_WARN("[OpenXR] No runtime (xrCreateInstance: %d).\n", (int)res);
      xr->instance = XR_NULL_HANDLE;
      goto unavailable;
   }

   get_system            = (PFN_xrGetSystem)
      vulkan_openxr_proc(xr, "xrGetSystem");
   get_system_properties = (PFN_xrGetSystemProperties)
      vulkan_openxr_proc(xr, "xrGetSystemProperties");
   enum_views            = (PFN_xrEnumerateViewConfigurationViews)
      vulkan_openxr_proc(xr, "xrEnumerateViewConfigurationViews");
   enum_modes            = (PFN_xrEnumerateEnvironmentBlendModes)
      vulkan_openxr_proc(xr, "xrEnumerateEnvironmentBlendModes");
   if (     !VULKAN_OPENXR_FN(xr, DestroyInstance)
         || !get_system || !get_system_properties
         || !enum_views || !enum_modes)
      goto missing;
   if (enable1)
   {
      if (     !VULKAN_OPENXR_FN(xr, GetVulkanGraphicsRequirementsKHR)
            || !VULKAN_OPENXR_FN(xr, GetVulkanGraphicsDeviceKHR)
            || !VULKAN_OPENXR_FN(xr, GetVulkanInstanceExtensionsKHR)
            || !VULKAN_OPENXR_FN(xr, GetVulkanDeviceExtensionsKHR))
         goto missing;
   }
   else if (!VULKAN_OPENXR_FN(xr, GetVulkanGraphicsRequirements2KHR)
         || !VULKAN_OPENXR_FN(xr, CreateVulkanInstanceKHR)
         || !VULKAN_OPENXR_FN(xr, CreateVulkanDeviceKHR)
         || !VULKAN_OPENXR_FN(xr, GetVulkanGraphicsDevice2KHR))
      goto missing;

   memset(&sgi, 0, sizeof(sgi));
   sgi.type       = XR_TYPE_SYSTEM_GET_INFO;
   sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
   if (XR_FAILED(res = get_system(xr->instance, &sgi, &xr->system)))
   {
      RARCH_WARN("[OpenXR] No headset (xrGetSystem: %d).\n", (int)res);
      goto unavailable;
   }

   xr->max_dim = 4096;
   memset(&props, 0, sizeof(props));
   props.type  = XR_TYPE_SYSTEM_PROPERTIES;
   if (XR_SUCCEEDED(get_system_properties(xr->instance, xr->system, &props)))
   {
      uint32_t mw = props.graphicsProperties.maxSwapchainImageWidth;
      uint32_t mh = props.graphicsProperties.maxSwapchainImageHeight;
      if (mw && mw < xr->max_dim)
         xr->max_dim = mw;
      if (mh && mh < xr->max_dim)
         xr->max_dim = mh;
      RARCH_LOG("[OpenXR] Headset: %s.\n", props.systemName);
   }

   memset(views, 0, sizeof(views));
   views[0].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
   views[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
   count         = 0;
   if (     XR_FAILED(enum_views(xr->instance, xr->system,
               XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views))
         || count < 1)
   {
      RARCH_WARN("[OpenXR] No headset (no stereo view configuration).\n");
      goto unavailable;
   }
   xr->rec_width  = views[0].recommendedImageRectWidth;

   count          = 0;
   xr->blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
   if (     XR_SUCCEEDED(enum_modes(xr->instance, xr->system,
               XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 8, &count, modes))
         && count)
      xr->blend_mode = modes[0];

   /* The runtime must be asked before any Vulkan object is made. */
   memset(&reqs, 0, sizeof(reqs));
   reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
   res       = enable1
      ? xr->GetVulkanGraphicsRequirementsKHR(xr->instance, xr->system, &reqs)
      : xr->GetVulkanGraphicsRequirements2KHR(xr->instance, xr->system, &reqs);
   if (XR_FAILED(res))
   {
      RARCH_ERR("[OpenXR] xrGetVulkanGraphicsRequirements failed (%d).\n",
            (int)res);
      goto failed;
   }
   RARCH_LOG("[OpenXR] Vulkan through %s, %u.%u to %u.%u.\n", ext,
         (unsigned)XR_VERSION_MAJOR(reqs.minApiVersionSupported),
         (unsigned)XR_VERSION_MINOR(reqs.minApiVersionSupported),
         (unsigned)XR_VERSION_MAJOR(reqs.maxApiVersionSupported),
         (unsigned)XR_VERSION_MINOR(reqs.maxApiVersionSupported));

   /* Major and minor only. The maximum is what the runtime was tested
    * with, so only a newer major is refused. */
   api = XR_MAKE_VERSION(VK_VERSION_MAJOR(api_version),
         VK_VERSION_MINOR(api_version), 0);
   if (     api < XR_MAKE_VERSION(
               XR_VERSION_MAJOR(reqs.minApiVersionSupported),
               XR_VERSION_MINOR(reqs.minApiVersionSupported), 0)
         || VK_VERSION_MAJOR(api_version)
            > XR_VERSION_MAJOR(reqs.maxApiVersionSupported))
   {
      RARCH_WARN("[OpenXR] The runtime does not take Vulkan %u.%u.\n",
            (unsigned)VK_VERSION_MAJOR(api_version),
            (unsigned)VK_VERSION_MINOR(api_version));
      goto failed;
   }

   if (     enable1
         && (  !vulkan_openxr_list(xr, xr->GetVulkanInstanceExtensionsKHR,
                  xr->inst_ext_buf, xr->inst_exts, &xr->num_inst_exts)
            || !vulkan_openxr_list(xr, xr->GetVulkanDeviceExtensionsKHR,
                  xr->dev_ext_buf, xr->dev_exts, &xr->num_dev_exts)))
   {
      RARCH_ERR("[OpenXR] The runtime's Vulkan extension lists could not be read.\n");
      goto failed;
   }
   return xr;

unavailable:
   vulkan_openxr_notify(MSG_OPENXR_UNAVAILABLE);
   vulkan_openxr_free(xr);
   return NULL;
missing:
   RARCH_ERR("[OpenXR] The runtime lacks functions headset output needs.\n");
failed:
   vulkan_openxr_notify(MSG_OPENXR_FAILED);
   vulkan_openxr_free(xr);
   return NULL;
}

void vulkan_openxr_free(vulkan_openxr_t *xr)
{
   if (!xr)
      return;
   vulkan_openxr_stop(xr);
   if (xr->session && vulkan_openxr_hooks.session_destroying)
      vulkan_openxr_hooks.session_destroying(vulkan_openxr_hooks.user, xr);
   if (xr->view_space)
      xr->DestroySpace(xr->view_space);
   if (xr->local_space)
      xr->DestroySpace(xr->local_space);
   if (xr->session)
      xr->DestroySession(xr->session);
   if (xr->instance && xr->DestroyInstance)
      xr->DestroyInstance(xr->instance);
   if (xr->lock)
      slock_free(xr->lock);
   if (xr->lib)
      dylib_close(xr->lib);
   free(xr);
}

void vulkan_openxr_drop(vulkan_openxr_t *xr)
{
   RARCH_WARN("[OpenXR] Continuing without headset output.\n");
   vulkan_openxr_notify(MSG_OPENXR_FAILED);
   vulkan_openxr_free(xr);
}

void vulkan_openxr_needs_reload(void)
{
   RARCH_WARN("[OpenXR] The content keeps its Vulkan context; headset output starts when it is loaded again.\n");
   vulkan_openxr_notify(MSG_OPENXR_NEEDS_RELOAD);
}

bool vulkan_openxr_uses_enable2(const vulkan_openxr_t *xr)
{
   return xr->enable2;
}

unsigned vulkan_openxr_instance_extensions(const vulkan_openxr_t *xr,
      const char **out, unsigned cap)
{
   unsigned i;
   for (i = 0; i < xr->num_inst_exts && i < cap; i++)
      out[i] = xr->inst_exts[i];
   return i;
}

unsigned vulkan_openxr_device_extensions(const vulkan_openxr_t *xr,
      const char **out, unsigned cap)
{
   unsigned i;
   for (i = 0; i < xr->num_dev_exts && i < cap; i++)
      out[i] = xr->dev_exts[i];
   return i;
}

VkResult vulkan_openxr_create_instance(vulkan_openxr_t *xr,
      PFN_vkGetInstanceProcAddr gipa, const VkInstanceCreateInfo *info,
      VkInstance *instance)
{
   XrResult res;
   XrVulkanInstanceCreateInfoKHR ci;
   VkResult vk_res = VK_ERROR_INITIALIZATION_FAILED;

   memset(&ci, 0, sizeof(ci));
   ci.type                   = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
   ci.systemId               = xr->system;
   ci.pfnGetInstanceProcAddr = gipa;
   ci.vulkanCreateInfo       = info;
   res = xr->CreateVulkanInstanceKHR(xr->instance, &ci, instance, &vk_res);
   if (XR_FAILED(res) || vk_res != VK_SUCCESS)
   {
      RARCH_WARN("[OpenXR] The runtime could not create the Vulkan instance (%d, %d).\n",
            (int)res, (int)vk_res);
      return (vk_res != VK_SUCCESS) ? vk_res : VK_ERROR_INITIALIZATION_FAILED;
   }
   RARCH_LOG("[OpenXR] Vulkan instance created through the runtime.\n");
   return VK_SUCCESS;
}

VkResult vulkan_openxr_create_device(vulkan_openxr_t *xr,
      PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice gpu,
      const VkDeviceCreateInfo *info, VkDevice *device)
{
   XrResult res;
   XrVulkanDeviceCreateInfoKHR ci;
   VkResult vk_res = VK_ERROR_INITIALIZATION_FAILED;

   memset(&ci, 0, sizeof(ci));
   ci.type                   = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
   ci.systemId               = xr->system;
   ci.pfnGetInstanceProcAddr = gipa;
   ci.vulkanPhysicalDevice   = gpu;
   ci.vulkanCreateInfo       = info;
   res = xr->CreateVulkanDeviceKHR(xr->instance, &ci, device, &vk_res);
   if (XR_FAILED(res) || vk_res != VK_SUCCESS)
   {
      RARCH_WARN("[OpenXR] The runtime could not create the Vulkan device (%d, %d).\n",
            (int)res, (int)vk_res);
      return (vk_res != VK_SUCCESS) ? vk_res : VK_ERROR_INITIALIZATION_FAILED;
   }
   RARCH_LOG("[OpenXR] Vulkan device created through the runtime.\n");
   return VK_SUCCESS;
}

VkPhysicalDevice vulkan_openxr_gpu(vulkan_openxr_t *xr, VkInstance instance)
{
   XrResult res;
   VkPhysicalDevice gpu = VK_NULL_HANDLE;

   if (xr->enable2)
   {
      XrVulkanGraphicsDeviceGetInfoKHR gi;
      memset(&gi, 0, sizeof(gi));
      gi.type           = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
      gi.systemId       = xr->system;
      gi.vulkanInstance = instance;
      res = xr->GetVulkanGraphicsDevice2KHR(xr->instance, &gi, &gpu);
   }
   else
      res = xr->GetVulkanGraphicsDeviceKHR(xr->instance, xr->system,
            instance, &gpu);
   if (XR_FAILED(res) || gpu == VK_NULL_HANDLE)
   {
      RARCH_WARN("[OpenXR] The runtime named no GPU (%d).\n", (int)res);
      return VK_NULL_HANDLE;
   }
   return gpu;
}

void vulkan_openxr_set_hooks(const vulkan_openxr_hooks_t *hooks)
{
   if (hooks)
      vulkan_openxr_hooks = *hooks;
   else
      memset(&vulkan_openxr_hooks, 0, sizeof(vulkan_openxr_hooks));
}

static const char *vulkan_openxr_state_name(XrSessionState state)
{
   switch (state)
   {
      case XR_SESSION_STATE_IDLE:
         return "idle";
      case XR_SESSION_STATE_READY:
         return "ready";
      case XR_SESSION_STATE_SYNCHRONIZED:
         return "synchronized";
      case XR_SESSION_STATE_VISIBLE:
         return "visible";
      case XR_SESSION_STATE_FOCUSED:
         return "focused";
      case XR_SESSION_STATE_STOPPING:
         return "stopping";
      case XR_SESSION_STATE_LOSS_PENDING:
         return "loss pending";
      case XR_SESSION_STATE_EXITING:
         return "exiting";
      default:
         break;
   }
   return "unknown";
}

static void vulkan_openxr_ended(vulkan_openxr_t *xr)
{
   xr->running = false;
   if (xr->ended)
      return;
   xr->ended = true;
   retro_atomic_store_release_int(&xr->alive, 0);
   RARCH_WARN("[OpenXR] The headset session ended; the window keeps the output.\n");
   vulkan_openxr_notify(MSG_OPENXR_SESSION_ENDED);
}

static void vulkan_openxr_session_state(vulkan_openxr_t *xr,
      XrSessionState state)
{
   XrResult res;
   retro_atomic_store_release_int(&xr->state, (int)state);
   RARCH_LOG("[OpenXR] Session %s.\n", vulkan_openxr_state_name(state));
   switch (state)
   {
      case XR_SESSION_STATE_READY:
         {
            XrSessionBeginInfo bi;
            memset(&bi, 0, sizeof(bi));
            bi.type                         = XR_TYPE_SESSION_BEGIN_INFO;
            bi.primaryViewConfigurationType =
               XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            slock_lock(xr->queue_lock);
            res = xr->BeginSession(xr->session, &bi);
            slock_unlock(xr->queue_lock);
            if (XR_SUCCEEDED(res))
               xr->running = true;
            else
               RARCH_ERR("[OpenXR] xrBeginSession failed (%d).\n", (int)res);
         }
         break;
      case XR_SESSION_STATE_STOPPING:
         slock_lock(xr->queue_lock);
         xr->EndSession(xr->session);
         slock_unlock(xr->queue_lock);
         xr->running = false;
         break;
      case XR_SESSION_STATE_EXITING:
      case XR_SESSION_STATE_LOSS_PENDING:
         vulkan_openxr_ended(xr);
         break;
      default:
         break;
   }
}

static void vulkan_openxr_poll(vulkan_openxr_t *xr)
{
   XrResult res;
   XrEventDataBuffer ev;
   for (;;)
   {
      memset(&ev, 0, sizeof(ev));
      ev.type = XR_TYPE_EVENT_DATA_BUFFER;
      res     = xr->PollEvent(xr->instance, &ev);
      if (res == XR_ERROR_INSTANCE_LOST)
         vulkan_openxr_ended(xr);
      if (res != XR_SUCCESS)
         break;
      if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
         vulkan_openxr_session_state(xr,
               ((const XrEventDataSessionStateChanged*)&ev)->state);
      else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
         vulkan_openxr_ended(xr);
   }
}

/* Pixels per radian across one eye, from the first located views. */
static void vulkan_openxr_measure(vulkan_openxr_t *xr, XrTime time)
{
   float fov;
   uint32_t n = 0;
   XrViewLocateInfo li;
   XrViewState vs;
   XrView views[2];

   memset(&li, 0, sizeof(li));
   li.type                  = XR_TYPE_VIEW_LOCATE_INFO;
   li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
   li.displayTime           = time;
   li.space                 = xr->local_space;
   memset(&vs, 0, sizeof(vs));
   vs.type                  = XR_TYPE_VIEW_STATE;
   memset(views, 0, sizeof(views));
   views[0].type            = XR_TYPE_VIEW;
   views[1].type            = XR_TYPE_VIEW;
   if (     XR_FAILED(xr->LocateViews(xr->session, &li, &vs, 2, &n, views))
         || !n)
      return;
   fov = views[0].fov.angleRight - views[0].fov.angleLeft;
   if (fov < 0.1f || !xr->rec_width)
      return;
   slock_lock(xr->lock);
   xr->px_per_rad = (float)xr->rec_width / fov;
   slock_unlock(xr->lock);
   RARCH_LOG("[OpenXR] %u pixels across %.0f degrees per eye.\n",
         (unsigned)xr->rec_width, fov * 57.29578f);
}

static void vulkan_openxr_frame_error(vulkan_openxr_t *xr,
      const char *fn, XrResult res)
{
   if (!xr->frame_failed)
   {
      xr->frame_failed = true;
      RARCH_ERR("[OpenXR] %s failed (%d).\n", fn, (int)res);
   }
   /* A runtime may report the loss here without an event. */
   if (res == XR_ERROR_SESSION_LOST || res == XR_ERROR_INSTANCE_LOST)
      vulkan_openxr_ended(xr);
}

/* The published quads whose slot has released an image. The caller
 * holds the queue lock, so no slot changes under it. */
static unsigned vulkan_openxr_layers(vulkan_openxr_t *xr,
      XrCompositionLayerQuad *layers,
      const XrCompositionLayerBaseHeader **ptrs)
{
   unsigned i;
   unsigned n = 0;
   slock_lock(xr->lock);
   for (i = 0; i < xr->quads.num_quads; i++)
   {
      const video_xr_quad_t *q        = &xr->quads.quads[i];
      struct vulkan_openxr_slot *slot = &xr->slots[q->slot];
      XrCompositionLayerQuad *l       = &layers[n];
      if (     !slot->swapchain || q->layer >= slot->layers
            || !retro_atomic_load_acquire_int(&slot->content))
         continue;
      memset(l, 0, sizeof(*l));
      l->type          = XR_TYPE_COMPOSITION_LAYER_QUAD;
      /* The UI is drawn over transparent black: premultiplied. */
      l->layerFlags    = (q->kind == VIDEO_XR_QUAD_MENU)
         ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
      l->space         = xr->local_space;
      l->eyeVisibility = (q->eye == VIDEO_XR_EYE_LEFT)
         ? XR_EYE_VISIBILITY_LEFT
         : ((q->eye == VIDEO_XR_EYE_RIGHT)
               ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_BOTH);
      l->subImage.swapchain               = slot->swapchain;
      l->subImage.imageRect.extent.width  = (int32_t)VIDEO_SCALE_W(slot->dims);
      l->subImage.imageRect.extent.height = (int32_t)VIDEO_SCALE_H(slot->dims);
      l->subImage.imageArrayIndex         = q->layer;
      l->pose.orientation.x = q->pose.orientation.x;
      l->pose.orientation.y = q->pose.orientation.y;
      l->pose.orientation.z = q->pose.orientation.z;
      l->pose.orientation.w = q->pose.orientation.w;
      l->pose.position.x    = q->pose.position.x;
      l->pose.position.y    = q->pose.position.y;
      l->pose.position.z    = q->pose.position.z;
      l->size.width         = q->width;
      l->size.height        = q->height;
      ptrs[n++]             = (const XrCompositionLayerBaseHeader*)l;
   }
   slock_unlock(xr->lock);
   return n;
}

/* One headset frame. xrWaitFrame paces this thread at the headset's
 * rate; the core never waits on it. */
static void vulkan_openxr_frame(vulkan_openxr_t *xr)
{
   XrResult res;
   int session_state;
   const char *fn = NULL;
   XrFrameWaitInfo wait_info;
   XrFrameState state;
   XrFrameBeginInfo begin_info;
   XrFrameEndInfo end_info;
   XrCompositionLayerQuad layers[VIDEO_XR_MAX_QUADS];
   const XrCompositionLayerBaseHeader *ptrs[VIDEO_XR_MAX_QUADS
      + VULKAN_OPENXR_MAX_EXTRA_LAYERS];

   memset(&wait_info, 0, sizeof(wait_info));
   wait_info.type = XR_TYPE_FRAME_WAIT_INFO;
   memset(&state, 0, sizeof(state));
   state.type     = XR_TYPE_FRAME_STATE;
   if (XR_FAILED(res = xr->WaitFrame(xr->session, &wait_info, &state)))
   {
      vulkan_openxr_frame_error(xr, "xrWaitFrame", res);
      retro_sleep(1);
      return;
   }
   slock_lock(xr->lock);
   xr->predicted_time = state.predictedDisplayTime;
   slock_unlock(xr->lock);
   if (xr->px_per_rad <= 0.0f)
      vulkan_openxr_measure(xr, state.predictedDisplayTime);

   memset(&begin_info, 0, sizeof(begin_info));
   begin_info.type               = XR_TYPE_FRAME_BEGIN_INFO;
   memset(&end_info, 0, sizeof(end_info));
   end_info.type                 = XR_TYPE_FRAME_END_INFO;
   end_info.displayTime          = state.predictedDisplayTime;
   end_info.environmentBlendMode = xr->blend_mode;

   slock_lock(xr->queue_lock);
   if (XR_FAILED(res = xr->BeginFrame(xr->session, &begin_info)))
      fn = "xrBeginFrame";
   else
   {
      session_state = retro_atomic_load_acquire_int(&xr->state);
      /* Layers only while the headset shows the session. */
      if (     state.shouldRender
            && (   session_state == XR_SESSION_STATE_VISIBLE
                || session_state == XR_SESSION_STATE_FOCUSED))
      {
         end_info.layerCount = vulkan_openxr_layers(xr, layers, ptrs);
         if (vulkan_openxr_hooks.frame_layers)
            end_info.layerCount += vulkan_openxr_hooks.frame_layers(
                  vulkan_openxr_hooks.user, state.predictedDisplayTime,
                  ptrs + end_info.layerCount,
                  VULKAN_OPENXR_MAX_EXTRA_LAYERS);
      }
      end_info.layers = end_info.layerCount ? ptrs : NULL;
      if (XR_FAILED(res = xr->EndFrame(xr->session, &end_info)))
         fn = "xrEndFrame";
   }
   slock_unlock(xr->queue_lock);
   if (fn)
      vulkan_openxr_frame_error(xr, fn, res);
   else
      xr->frame_failed = false;
}

static void vulkan_openxr_thread(void *data)
{
   vulkan_openxr_t *xr = (vulkan_openxr_t*)data;
   while (!retro_atomic_load_acquire_int(&xr->quit))
   {
      vulkan_openxr_poll(xr);
      if (xr->running)
         vulkan_openxr_frame(xr);
      else
         retro_sleep(10);
   }
}

static bool vulkan_openxr_create_session(vulkan_openxr_t *xr,
      VkInstance instance, VkPhysicalDevice gpu, VkDevice device,
      uint32_t queue_family)
{
   XrResult res;
   uint32_t count = 0;
   XrGraphicsBindingVulkanKHR binding;
   XrSessionCreateInfo sci;
   XrReferenceSpaceCreateInfo rci;

   if (     !VULKAN_OPENXR_FN(xr, PollEvent)
         || !VULKAN_OPENXR_FN(xr, CreateSession)
         || !VULKAN_OPENXR_FN(xr, DestroySession)
         || !VULKAN_OPENXR_FN(xr, BeginSession)
         || !VULKAN_OPENXR_FN(xr, EndSession)
         || !VULKAN_OPENXR_FN(xr, CreateReferenceSpace)
         || !VULKAN_OPENXR_FN(xr, DestroySpace)
         || !VULKAN_OPENXR_FN(xr, LocateViews)
         || !VULKAN_OPENXR_FN(xr, WaitFrame)
         || !VULKAN_OPENXR_FN(xr, BeginFrame)
         || !VULKAN_OPENXR_FN(xr, EndFrame)
         || !VULKAN_OPENXR_FN(xr, EnumerateSwapchainFormats)
         || !VULKAN_OPENXR_FN(xr, CreateSwapchain)
         || !VULKAN_OPENXR_FN(xr, DestroySwapchain)
         || !VULKAN_OPENXR_FN(xr, EnumerateSwapchainImages)
         || !VULKAN_OPENXR_FN(xr, AcquireSwapchainImage)
         || !VULKAN_OPENXR_FN(xr, WaitSwapchainImage)
         || !VULKAN_OPENXR_FN(xr, ReleaseSwapchainImage))
   {
      RARCH_ERR("[OpenXR] The runtime lacks session functions.\n");
      return false;
   }

   memset(&binding, 0, sizeof(binding));
   binding.type             = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
   binding.instance         = instance;
   binding.physicalDevice   = gpu;
   binding.device           = device;
   binding.queueFamilyIndex = queue_family;
   binding.queueIndex       = 0;
   memset(&sci, 0, sizeof(sci));
   sci.type                 = XR_TYPE_SESSION_CREATE_INFO;
   sci.next                 = &binding;
   sci.systemId             = xr->system;
   if (XR_FAILED(res = xr->CreateSession(xr->instance, &sci, &xr->session)))
   {
      RARCH_ERR("[OpenXR] xrCreateSession failed (%d).\n", (int)res);
      xr->session = XR_NULL_HANDLE;
      return false;
   }
   xr->device = device;
   video_xr_pose_identity(&xr->anchor);

   memset(&rci, 0, sizeof(rci));
   rci.type                               = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
   rci.referenceSpaceType                 = XR_REFERENCE_SPACE_TYPE_LOCAL;
   rci.poseInReferenceSpace.orientation.w = 1.0f;
   if (XR_FAILED(res = xr->CreateReferenceSpace(xr->session, &rci,
               &xr->local_space)))
   {
      RARCH_ERR("[OpenXR] No LOCAL space (%d).\n", (int)res);
      xr->local_space = XR_NULL_HANDLE;
      goto error;
   }
   rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
   if (XR_FAILED(res = xr->CreateReferenceSpace(xr->session, &rci,
               &xr->view_space)))
   {
      RARCH_ERR("[OpenXR] No VIEW space (%d).\n", (int)res);
      xr->view_space = XR_NULL_HANDLE;
      goto error;
   }
   if (XR_FAILED(xr->EnumerateSwapchainFormats(xr->session,
               VULKAN_OPENXR_MAX_FORMATS, &count, xr->formats)))
      count = 0;
   xr->num_formats = count;
   RARCH_LOG("[OpenXR] Session created.\n");

   if (vulkan_openxr_hooks.session_created)
   {
      vulkan_openxr_handles_t h;
      h.xr          = xr;
      h.instance    = xr->instance;
      h.system      = xr->system;
      h.session     = xr->session;
      h.local_space = xr->local_space;
      h.view_space  = xr->view_space;
      h.get_proc    = xr->GetInstanceProcAddr;
      vulkan_openxr_hooks.session_created(vulkan_openxr_hooks.user, &h);
   }
   return true;

error:
   /* Freed here, so session_destroying only follows session_created. */
   if (xr->local_space)
      xr->DestroySpace(xr->local_space);
   xr->local_space = XR_NULL_HANDLE;
   xr->DestroySession(xr->session);
   xr->session     = XR_NULL_HANDLE;
   return false;
}

bool vulkan_openxr_start(vulkan_openxr_t *xr, VkInstance instance,
      VkPhysicalDevice gpu, VkDevice device, uint32_t queue_family,
      slock_t *queue_lock)
{
   if (!xr->lock && !(xr->lock = slock_new()))
      return false;
   if (     !xr->session
         && !vulkan_openxr_create_session(xr, instance, gpu, device,
            queue_family))
      return false;
   if (xr->device != device)
   {
      RARCH_ERR("[OpenXR] The session belongs to another Vulkan device.\n");
      return false;
   }
   xr->queue_lock = queue_lock;
   retro_atomic_store_release_int(&xr->quit, 0);
   retro_atomic_store_release_int(&xr->alive, xr->ended ? 0 : 1);
   if (!(xr->thread = sthread_create(vulkan_openxr_thread, xr)))
   {
      retro_atomic_store_release_int(&xr->alive, 0);
      return false;
   }
   return true;
}

void vulkan_openxr_stop(vulkan_openxr_t *xr)
{
   unsigned s;
   if (!xr)
      return;
   if (xr->thread)
   {
      retro_atomic_store_release_int(&xr->quit, 1);
      sthread_join(xr->thread);
      xr->thread = NULL;
   }
   /* A kept session outlives the driver's views of its images. */
   for (s = 0; s < VIDEO_XR_MAX_SLOTS; s++)
      vulkan_openxr_slot_destroy(xr, s);
   if (xr->lock)
   {
      slock_lock(xr->lock);
      xr->quads.num_quads = 0;
      slock_unlock(xr->lock);
   }
}

void vulkan_openxr_drop_and_reinit(vulkan_openxr_t *xr)
{
   RARCH_ERR("[OpenXR] Rebuilding video without headset output.\n");
   vulkan_openxr_notify(MSG_OPENXR_FAILED);
   vulkan_openxr_free(xr);
   vulkan_openxr_skip_once = true;
}

bool vulkan_openxr_alive(vulkan_openxr_t *xr)
{
   return retro_atomic_load_acquire_int(&xr->alive) != 0;
}

bool vulkan_openxr_focused(vulkan_openxr_t *xr)
{
   return retro_atomic_load_acquire_int(&xr->alive)
      && retro_atomic_load_acquire_int(&xr->state) == XR_SESSION_STATE_FOCUSED;
}

XrTime vulkan_openxr_predicted_time(vulkan_openxr_t *xr)
{
   XrTime t;
   slock_lock(xr->lock);
   t = xr->predicted_time;
   slock_unlock(xr->lock);
   return t;
}

bool vulkan_openxr_should_draw(vulkan_openxr_t *xr)
{
   int state = retro_atomic_load_acquire_int(&xr->state);
   return retro_atomic_load_acquire_int(&xr->alive)
      && (   state == XR_SESSION_STATE_VISIBLE
          || state == XR_SESSION_STATE_FOCUSED);
}

float vulkan_openxr_pixels_per_radian(vulkan_openxr_t *xr)
{
   float v;
   slock_lock(xr->lock);
   v = xr->px_per_rad;
   slock_unlock(xr->lock);
   return v;
}

unsigned vulkan_openxr_max_dim(const vulkan_openxr_t *xr)
{
   return xr->max_dim;
}

bool vulkan_openxr_supports_format(const vulkan_openxr_t *xr,
      VkFormat format)
{
   uint32_t i;
   for (i = 0; i < xr->num_formats; i++)
      if (xr->formats[i] == (int64_t)format)
         return true;
   return false;
}

void vulkan_openxr_slot_destroy(vulkan_openxr_t *xr, unsigned slot)
{
   XrSwapchain sc;
   struct vulkan_openxr_slot *s = &xr->slots[slot];
   if (!s->swapchain)
      return;
   slock_lock(xr->queue_lock);
   slock_lock(xr->lock);
   sc           = s->swapchain;
   s->swapchain = XR_NULL_HANDLE;
   s->dims      = 0;
   s->layers    = 0;
   s->acquired  = false;
   s->waited    = false;
   retro_atomic_store_release_int(&s->content, 0);
   slock_unlock(xr->lock);
   xr->DestroySwapchain(sc);
   slock_unlock(xr->queue_lock);
}

bool vulkan_openxr_slot_create(vulkan_openxr_t *xr, unsigned slot,
      VkFormat format, bool mutable_format, unsigned dims, unsigned layers,
      VkImage *images, unsigned *num_images)
{
   XrResult res;
   uint32_t i;
   uint32_t n = 0;
   XrSwapchainCreateInfo ci;
   XrSwapchainImageVulkanKHR imgs[VULKAN_OPENXR_MAX_IMAGES];
   XrSwapchain sc               = XR_NULL_HANDLE;
   struct vulkan_openxr_slot *s = &xr->slots[slot];

   vulkan_openxr_slot_destroy(xr, slot);
   memset(&ci, 0, sizeof(ci));
   ci.type        = XR_TYPE_SWAPCHAIN_CREATE_INFO;
   ci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
      | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
   /* The chains' final passes write through a view in the window's
    * format. */
   if (mutable_format)
      ci.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
   ci.format      = (int64_t)format;
   ci.sampleCount = 1;
   ci.width       = VIDEO_SCALE_W(dims);
   ci.height      = VIDEO_SCALE_H(dims);
   ci.faceCount   = 1;
   ci.arraySize   = layers;
   ci.mipCount    = 1;
   memset(imgs, 0, sizeof(imgs));
   for (i = 0; i < VULKAN_OPENXR_MAX_IMAGES; i++)
      imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;

   slock_lock(xr->queue_lock);
   res = xr->CreateSwapchain(xr->session, &ci, &sc);
   if (XR_SUCCEEDED(res))
      res = xr->EnumerateSwapchainImages(sc, 0, &n, NULL);
   if (XR_SUCCEEDED(res) && (!n || n > VULKAN_OPENXR_MAX_IMAGES))
      res = XR_ERROR_SIZE_INSUFFICIENT;
   if (XR_SUCCEEDED(res))
      res = xr->EnumerateSwapchainImages(sc, n, &n,
            (XrSwapchainImageBaseHeader*)imgs);
   if (XR_SUCCEEDED(res))
   {
      slock_lock(xr->lock);
      s->swapchain = sc;
      s->dims      = dims;
      s->layers    = layers;
      s->acquired  = false;
      s->waited    = false;
      retro_atomic_store_release_int(&s->content, 0);
      slock_unlock(xr->lock);
   }
   else if (sc != XR_NULL_HANDLE)
      xr->DestroySwapchain(sc);
   slock_unlock(xr->queue_lock);

   if (XR_FAILED(res))
   {
      RARCH_ERR("[OpenXR] No %ux%u swapchain with %u layer(s) (%d).\n",
            VIDEO_SCALE_W(dims), VIDEO_SCALE_H(dims), layers, (int)res);
      return false;
   }
   for (i = 0; i < n; i++)
      images[i] = imgs[i].image;
   *num_images = n;
   RARCH_LOG("[OpenXR] Slot %u: %ux%u, %u layer(s), %u images.\n", slot,
         VIDEO_SCALE_W(dims), VIDEO_SCALE_H(dims), layers, (unsigned)n);
   return true;
}

bool vulkan_openxr_slot_acquire(vulkan_openxr_t *xr, unsigned slot,
      unsigned *index)
{
   struct vulkan_openxr_slot *s = &xr->slots[slot];
   if (!s->swapchain)
      return false;
   if (!s->acquired)
   {
      XrSwapchainImageAcquireInfo ai;
      memset(&ai, 0, sizeof(ai));
      ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
      if (XR_FAILED(xr->AcquireSwapchainImage(s->swapchain, &ai, &s->index)))
         return false;
      s->acquired = true;
      s->waited   = false;
   }
   if (!s->waited)
   {
      XrSwapchainImageWaitInfo wi;
      memset(&wi, 0, sizeof(wi));
      wi.type    = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
      /* Never stall the core, nor the XR thread behind the queue lock:
       * an image the compositor still reads is tried again next frame. */
      wi.timeout = 0;
      if (xr->WaitSwapchainImage(s->swapchain, &wi) != XR_SUCCESS)
         return false;
      s->waited = true;
   }
   *index = s->index;
   return true;
}

void vulkan_openxr_slot_release(vulkan_openxr_t *xr, unsigned slot)
{
   XrSwapchainImageReleaseInfo ri;
   struct vulkan_openxr_slot *s = &xr->slots[slot];
   if (!s->swapchain || !s->acquired || !s->waited)
      return;
   memset(&ri, 0, sizeof(ri));
   ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
   if (XR_SUCCEEDED(xr->ReleaseSwapchainImage(s->swapchain, &ri)))
      retro_atomic_store_release_int(&s->content, 1);
   s->acquired = false;
   s->waited   = false;
}

void vulkan_openxr_publish(vulkan_openxr_t *xr,
      const video_xr_quad_set_t *set)
{
   slock_lock(xr->lock);
   xr->quads = *set;
   slock_unlock(xr->lock);
}

void vulkan_openxr_get_anchor(vulkan_openxr_t *xr, video_xr_pose_t *anchor)
{
   slock_lock(xr->lock);
   *anchor = xr->anchor;
   slock_unlock(xr->lock);
}

bool vulkan_openxr_get_quads(vulkan_openxr_t *xr, video_xr_quad_set_t *out)
{
   if (!xr || !xr->lock)
      return false;
   slock_lock(xr->lock);
   *out = xr->quads;
   slock_unlock(xr->lock);
   return true;
}
