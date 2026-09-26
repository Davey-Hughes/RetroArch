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
#include <string/stdstring.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "vulkan_openxr.h"

#include "../../msg_hash.h"
#include "../../runloop.h"
#include "../../verbosity.h"

#define VULKAN_OPENXR_EXT_BUF 1024

struct vulkan_openxr
{
   dylib_t lib;
   XrInstance instance;
   XrSystemId system;
   XrEnvironmentBlendMode blend_mode;
   unsigned max_dim;
   uint32_t rec_width;
   bool enable2;

   /* XR_KHR_vulkan_enable's lists, split in place. */
   char inst_ext_buf[VULKAN_OPENXR_EXT_BUF];
   char dev_ext_buf[VULKAN_OPENXR_EXT_BUF];
   const char *inst_exts[VULKAN_OPENXR_MAX_EXTS];
   const char *dev_exts[VULKAN_OPENXR_MAX_EXTS];
   unsigned num_inst_exts;
   unsigned num_dev_exts;

   PFN_xrGetInstanceProcAddr GetInstanceProcAddr;
   PFN_xrDestroyInstance DestroyInstance;
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
   if (xr->instance && xr->DestroyInstance)
      xr->DestroyInstance(xr->instance);
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
