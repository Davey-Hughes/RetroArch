/* Copyright  (C) 2010-2026 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (fake_hdr_layer.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* A Vulkan layer that makes a 10-bit X11 surface (lavapipe on an Xvfb
 * of depth 30) look like an HDR10 display.
 *
 * Every 10-bit surface format the surface reports in sRGB is reported
 * a second time paired with VK_COLOR_SPACE_HDR10_ST2084_EXT, so the
 * Vulkan driver selects an HDR10 swapchain and runs its whole HDR
 * path: the offscreen buffer, the SDR UI layer, the PQ composite. A
 * swapchain created in that colour space is passed on in sRGB with the
 * same format and images, so nothing the driver records changes; only
 * the meaning the compositor would give the pixels does, and there is
 * no compositor here to give it.
 *
 * Whichever side of the validation layer this one lands on, the two
 * agree: below it, the validation layer reads the HDR10 pairs through
 * this layer and accepts the swapchain; above it, it sees the sRGB
 * swapchain the surface really has.
 *
 * Each swapchain it rewrote is logged, so a run can tell that the HDR
 * path was taken rather than silently falling back to SDR.
 *
 * With FAKE_HDR_SCRGB_ONLY=1 the surface offers scRGB and no HDR10
 * instead: one R16G16B16A16_SFLOAT pair in the extended linear sRGB
 * colour space, which the X11 WSI cannot present. A swapchain asked in
 * it is a stand-in: the real one is made 10-bit sRGB, the driver draws
 * into FP16 images of the layer's own, and each present first blits
 * the presented image across into the real one. Each stand-in is
 * logged.
 *
 * One instance and one device at a time, which is how the frontend
 * runs the driver; the next layer's entry points are kept in globals. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../gfx/include/vulkan/vulkan.h"
#include "../../../gfx/include/vulkan/vk_layer.h"

#if defined(_WIN32)
#define FH_EXPORT __declspec(dllexport)
#else
#define FH_EXPORT __attribute__((visibility("default")))
#endif

/* Far more than any surface reports. */
#define FH_FORMATS 64
/* Far more images, live swapchains and present waits than any run has. */
#define FH_IMAGES   16
#define FH_STANDINS 8
#define FH_WAITS    16

#define FH_NEXT(n) ((PFN_vk##n)s_next_gdpa(s_device, "vk" #n))

/* An scRGB swapchain: the real one, keyed by its handle, and the FP16
 * images handed out in place of its own. */
struct fh_standin
{
   VkSwapchainKHR  swapchain;
   uint32_t        count;
   VkImage         images[FH_IMAGES];
   VkDeviceMemory  memory[FH_IMAGES];
   VkImage         bridges[FH_IMAGES];
   VkDeviceMemory  bridge_memory[FH_IMAGES];
   VkCommandBuffer cmds[FH_IMAGES];
   VkFence         fences[FH_IMAGES];
   VkCommandPool   pool;
};

static PFN_vkGetInstanceProcAddr s_next_gipa;
static PFN_vkGetDeviceProcAddr   s_next_gdpa;
static PFN_vkSetDeviceLoaderData s_set_loader_data;
static VkInstance                s_instance;
static VkPhysicalDevice          s_gpu;
static VkDevice                  s_device;
static uint32_t                  s_family;
static int                       s_scrgb_only;
static unsigned long             s_hdr10_swapchains;
static unsigned long             s_scrgb_swapchains;
static struct fh_standin         s_standins[FH_STANDINS];

static PFN_vkVoidFunction fh_next_instance(const char *name)
{
   return s_next_gipa ? s_next_gipa(s_instance, name) : NULL;
}

static int fh_is_10bit(VkFormat format)
{
   return format == VK_FORMAT_A2B10G10R10_UNORM_PACK32
       || format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
}

static int fh_first_10bit(const VkSurfaceFormatKHR *list, uint32_t n)
{
   uint32_t i;
   for (i = 0; i < n; i++)
      if (     fh_is_10bit(list[i].format)
            && list[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
         return (int)i;
   return -1;
}

/* The real list in real[0..n), the HDR10 twins (or the one scRGB pair)
 * appended after it. Returns the length of the whole list. */
static uint32_t fh_extend(VkSurfaceFormatKHR *real, uint32_t n)
{
   uint32_t i, total = n;
   if (s_scrgb_only)
   {
      if (fh_first_10bit(real, n) < 0 || n >= FH_FORMATS)
         return n;
      real[n].format     = VK_FORMAT_R16G16B16A16_SFLOAT;
      real[n].colorSpace = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
      return n + 1;
   }
   for (i = 0; i < n && total < FH_FORMATS; i++)
   {
      if (     fh_is_10bit(real[i].format)
            && real[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      {
         real[total].format     = real[i].format;
         real[total].colorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT;
         total++;
      }
   }
   return total;
}

/* The count/fill contract of every vkGet...KHR enumerator. */
static VkResult fh_hand_out(uint32_t total, uint32_t *count, int filling)
{
   if (!filling)
   {
      *count = total;
      return VK_SUCCESS;
   }
   if (*count < total)
      return VK_INCOMPLETE;
   *count = total;
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL fh_GetPhysicalDeviceSurfaceFormatsKHR(
      VkPhysicalDevice gpu, VkSurfaceKHR surface, uint32_t *count,
      VkSurfaceFormatKHR *formats)
{
   PFN_vkGetPhysicalDeviceSurfaceFormatsKHR next =
      (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)fh_next_instance(
            "vkGetPhysicalDeviceSurfaceFormatsKHR");
   VkSurfaceFormatKHR list[FH_FORMATS];
   uint32_t n = FH_FORMATS / 2, total, i;
   VkResult res;

   if (!next)
      return VK_ERROR_INITIALIZATION_FAILED;
   res = next(gpu, surface, &n, list);
   if (res != VK_SUCCESS && res != VK_INCOMPLETE)
      return res;
   total = fh_extend(list, n);
   res   = fh_hand_out(total, count, formats != NULL);
   if (formats)
      for (i = 0; i < *count && i < total; i++)
         formats[i] = list[i];
   return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL fh_GetPhysicalDeviceSurfaceFormats2KHR(
      VkPhysicalDevice gpu, const VkPhysicalDeviceSurfaceInfo2KHR *info,
      uint32_t *count, VkSurfaceFormat2KHR *formats)
{
   PFN_vkGetPhysicalDeviceSurfaceFormats2KHR next =
      (PFN_vkGetPhysicalDeviceSurfaceFormats2KHR)fh_next_instance(
            "vkGetPhysicalDeviceSurfaceFormats2KHR");
   VkSurfaceFormat2KHR real[FH_FORMATS / 2];
   VkSurfaceFormatKHR list[FH_FORMATS];
   uint32_t n = FH_FORMATS / 2, total, i;
   VkResult res;

   if (!next)
      return VK_ERROR_INITIALIZATION_FAILED;
   for (i = 0; i < n; i++)
   {
      real[i].sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
      real[i].pNext = NULL;
   }
   res = next(gpu, info, &n, real);
   if (res != VK_SUCCESS && res != VK_INCOMPLETE)
      return res;
   for (i = 0; i < n; i++)
      list[i] = real[i].surfaceFormat;
   total = fh_extend(list, n);
   res   = fh_hand_out(total, count, formats != NULL);
   /* The caller's sType and pNext stay; only the pair is written. */
   if (formats)
      for (i = 0; i < *count && i < total; i++)
         formats[i].surfaceFormat = list[i];
   return res;
}

static struct fh_standin *fh_standin_find(VkSwapchainKHR swapchain)
{
   unsigned i;
   if (swapchain == VK_NULL_HANDLE)
      return NULL;
   for (i = 0; i < FH_STANDINS; i++)
      if (s_standins[i].swapchain == swapchain)
         return &s_standins[i];
   return NULL;
}

/* Destroying a null handle is a no-op, so a half-built one frees too. */
static void fh_standin_free(struct fh_standin *s)
{
   uint32_t i;
   for (i = 0; i < FH_IMAGES; i++)
   {
      FH_NEXT(DestroyFence)(s_device, s->fences[i], NULL);
      FH_NEXT(DestroyImage)(s_device, s->images[i], NULL);
      FH_NEXT(FreeMemory)(s_device, s->memory[i], NULL);
      FH_NEXT(DestroyImage)(s_device, s->bridges[i], NULL);
      FH_NEXT(FreeMemory)(s_device, s->bridge_memory[i], NULL);
   }
   FH_NEXT(DestroyCommandPool)(s_device, s->pool, NULL);
   memset(s, 0, sizeof(*s));
}

/* An image in device-local memory where there is any. */
static VkResult fh_image_create(const VkImageCreateInfo *info,
      const VkPhysicalDeviceMemoryProperties *props, VkImage *image,
      VkDeviceMemory *memory)
{
   VkMemoryRequirements req;
   VkMemoryAllocateInfo alloc;
   uint32_t t;
   VkResult res;

   if ((res = FH_NEXT(CreateImage)(s_device, info, NULL, image))
         != VK_SUCCESS)
      return res;
   FH_NEXT(GetImageMemoryRequirements)(s_device, *image, &req);

   memset(&alloc, 0, sizeof(alloc));
   alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   alloc.allocationSize  = req.size;
   alloc.memoryTypeIndex = props->memoryTypeCount;
   for (t = 0; t < props->memoryTypeCount; t++)
   {
      if (!(req.memoryTypeBits & (1u << t)))
         continue;
      if (alloc.memoryTypeIndex == props->memoryTypeCount)
         alloc.memoryTypeIndex = t;
      if (props->memoryTypes[t].propertyFlags
            & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
      {
         alloc.memoryTypeIndex = t;
         break;
      }
   }
   if ((res = FH_NEXT(AllocateMemory)(s_device, &alloc, NULL, memory))
         != VK_SUCCESS)
      return res;
   return FH_NEXT(BindImageMemory)(s_device, *image, *memory, 0);
}

/* The FP16 image into the real one. Lavapipe cannot blit into the X11
 * surface's A2R10G10B10, so the blit goes into an A2B10G10R10 bridge
 * and a copy, which only needs the same texel size, moves it on. */
static VkResult fh_standin_record(VkCommandBuffer cmd, VkImage fp16,
      VkImage real, VkImage bridge, const VkSwapchainCreateInfoKHR *info)
{
   VkCommandBufferBeginInfo begin;
   VkImageMemoryBarrier b[3];
   VkImageBlit blit;
   VkImageCopy copy;
   VkResult res;

   memset(&begin, 0, sizeof(begin));
   begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   if ((res = FH_NEXT(BeginCommandBuffer)(cmd, &begin)) != VK_SUCCESS)
      return res;

   memset(b, 0, sizeof(b));
   b[0].sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   b[0].srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
   b[0].dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
   b[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   b[0].subresourceRange.levelCount = 1;
   b[0].subresourceRange.layerCount = info->imageArrayLayers;
   b[1]                             = b[0];
   b[2]                             = b[0];
   b[0].image                       = fp16;
   b[0].oldLayout                   = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   b[0].newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b[0].dstAccessMask               = VK_ACCESS_TRANSFER_READ_BIT;
   b[1].image                       = real;
   b[1].oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
   b[1].newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b[1].dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
   b[2].image                       = bridge;
   b[2].oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
   b[2].newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b[2].dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
   FH_NEXT(CmdPipelineBarrier)(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 3, b);

   memset(&blit, 0, sizeof(blit));
   blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   blit.srcSubresource.layerCount = info->imageArrayLayers;
   blit.srcOffsets[1].x           = (int32_t)info->imageExtent.width;
   blit.srcOffsets[1].y           = (int32_t)info->imageExtent.height;
   blit.srcOffsets[1].z           = 1;
   blit.dstSubresource            = blit.srcSubresource;
   blit.dstOffsets[1]             = blit.srcOffsets[1];
   FH_NEXT(CmdBlitImage)(cmd, fp16, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         bridge, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
         VK_FILTER_NEAREST);

   b[2].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b[2].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b[2].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   b[2].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   FH_NEXT(CmdPipelineBarrier)(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b[2]);

   memset(&copy, 0, sizeof(copy));
   copy.srcSubresource = blit.srcSubresource;
   copy.dstSubresource = blit.srcSubresource;
   copy.extent.width   = info->imageExtent.width;
   copy.extent.height  = info->imageExtent.height;
   copy.extent.depth   = 1;
   FH_NEXT(CmdCopyImage)(cmd, bridge, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         real, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

   b[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b[0].newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   b[0].dstAccessMask = 0;
   b[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b[1].newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   b[1].dstAccessMask = 0;
   FH_NEXT(CmdPipelineBarrier)(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 2, b);

   return FH_NEXT(EndCommandBuffer)(cmd);
}

/* For each of the real swapchain's images: an FP16 image, its bridge,
 * a fence and the command buffer that moves the one into the other. */
static VkResult fh_standin_build(struct fh_standin *s, const VkImage *real,
      const VkSwapchainCreateInfoKHR *info)
{
   VkPhysicalDeviceMemoryProperties props;
   VkCommandPoolCreateInfo pool_info;
   VkCommandBufferAllocateInfo cmd_info;
   VkFenceCreateInfo fence_info;
   VkImageCreateInfo image_info, bridge_info;
   PFN_vkGetPhysicalDeviceMemoryProperties get_props =
      (PFN_vkGetPhysicalDeviceMemoryProperties)fh_next_instance(
            "vkGetPhysicalDeviceMemoryProperties");
   uint32_t i;
   VkResult res;

   if (!get_props || !s_set_loader_data)
      return VK_ERROR_INITIALIZATION_FAILED;
   get_props(s_gpu, &props);

   memset(&pool_info, 0, sizeof(pool_info));
   pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   pool_info.queueFamilyIndex = s_family;
   if ((res = FH_NEXT(CreateCommandPool)(s_device, &pool_info, NULL,
               &s->pool)) != VK_SUCCESS)
      return res;

   memset(&cmd_info, 0, sizeof(cmd_info));
   cmd_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   cmd_info.commandPool        = s->pool;
   cmd_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cmd_info.commandBufferCount = s->count;
   if ((res = FH_NEXT(AllocateCommandBuffers)(s_device, &cmd_info,
               s->cmds)) != VK_SUCCESS)
      return res;

   memset(&fence_info, 0, sizeof(fence_info));
   fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

   memset(&image_info, 0, sizeof(image_info));
   image_info.sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
   image_info.imageType             = VK_IMAGE_TYPE_2D;
   image_info.format                = info->imageFormat;
   image_info.extent.width          = info->imageExtent.width;
   image_info.extent.height         = info->imageExtent.height;
   image_info.extent.depth          = 1;
   image_info.mipLevels             = 1;
   image_info.arrayLayers           = info->imageArrayLayers;
   image_info.samples               = VK_SAMPLE_COUNT_1_BIT;
   image_info.tiling                = VK_IMAGE_TILING_OPTIMAL;
   image_info.usage                 = info->imageUsage
                                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   image_info.sharingMode           = info->imageSharingMode;
   image_info.queueFamilyIndexCount = info->queueFamilyIndexCount;
   image_info.pQueueFamilyIndices   = info->pQueueFamilyIndices;
   image_info.initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED;

   bridge_info                       = image_info;
   bridge_info.format                = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
   bridge_info.usage                 = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                     | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   bridge_info.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
   bridge_info.queueFamilyIndexCount = 0;
   bridge_info.pQueueFamilyIndices   = NULL;

   for (i = 0; i < s->count; i++)
   {
      /* The layer's own dispatchable objects need the loader's table. */
      if (     (res = s_set_loader_data(s_device, s->cmds[i])) != VK_SUCCESS
            || (res = fh_image_create(&image_info, &props, &s->images[i],
                  &s->memory[i])) != VK_SUCCESS
            || (res = fh_image_create(&bridge_info, &props, &s->bridges[i],
                  &s->bridge_memory[i])) != VK_SUCCESS
            || (res = FH_NEXT(CreateFence)(s_device, &fence_info, NULL,
                  &s->fences[i])) != VK_SUCCESS
            || (res = fh_standin_record(s->cmds[i], s->images[i], real[i],
                  s->bridges[i], info)) != VK_SUCCESS)
         return res;
   }
   return VK_SUCCESS;
}

/* The real swapchain in the surface's first 10-bit sRGB format, behind
 * a stand-in that hands out FP16 images in its place. */
static VkResult fh_standin_create(VkDevice device,
      const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *cb,
      VkSwapchainKHR *swapchain)
{
   PFN_vkGetPhysicalDeviceSurfaceFormatsKHR get_formats =
      (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)fh_next_instance(
            "vkGetPhysicalDeviceSurfaceFormatsKHR");
   VkSurfaceFormatKHR list[FH_FORMATS];
   VkImage real[FH_IMAGES];
   VkSwapchainCreateInfoKHR real_info = *info;
   struct fh_standin *s = NULL;
   uint32_t n = FH_FORMATS, i;
   int first;
   VkResult res;

   for (i = 0; i < FH_STANDINS && !s; i++)
      if (s_standins[i].swapchain == VK_NULL_HANDLE)
         s = &s_standins[i];
   if (!s || !get_formats)
      return VK_ERROR_INITIALIZATION_FAILED;
   res = get_formats(s_gpu, info->surface, &n, list);
   if (res != VK_SUCCESS && res != VK_INCOMPLETE)
      return res;
   if ((first = fh_first_10bit(list, n)) < 0)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   real_info.imageFormat      = list[first].format;
   real_info.imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   real_info.imageUsage      |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   if ((res = FH_NEXT(CreateSwapchainKHR)(device, &real_info, cb,
               swapchain)) != VK_SUCCESS)
      return res;

   n   = FH_IMAGES;
   res = FH_NEXT(GetSwapchainImagesKHR)(device, *swapchain, &n, real);
   if (res == VK_SUCCESS)
   {
      s->swapchain = *swapchain;
      s->count     = n;
      res          = fh_standin_build(s, real, info);
   }
   if (res != VK_SUCCESS)
   {
      fh_standin_free(s);
      FH_NEXT(DestroySwapchainKHR)(device, *swapchain, cb);
      *swapchain = VK_NULL_HANDLE;
      return res;
   }

   s_scrgb_swapchains++;
   printf("[info] fake-hdr: scRGB swapchain %lu, format %d, %ux%u\n",
         s_scrgb_swapchains, (int)info->imageFormat,
         info->imageExtent.width, info->imageExtent.height);
   fflush(stdout);
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL fh_CreateSwapchainKHR(VkDevice device,
      const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *cb,
      VkSwapchainKHR *swapchain)
{
   PFN_vkCreateSwapchainKHR next = (PFN_vkCreateSwapchainKHR)
      s_next_gdpa(device, "vkCreateSwapchainKHR");
   VkSwapchainCreateInfoKHR real = *info;

   if (     s_scrgb_only
         && real.imageFormat     == VK_FORMAT_R16G16B16A16_SFLOAT
         && real.imageColorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)
      return fh_standin_create(device, info, cb, swapchain);
   if (real.imageColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
   {
      real.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      s_hdr10_swapchains++;
      printf("[info] fake-hdr: HDR10 swapchain %lu, format %d, %ux%u\n",
            s_hdr10_swapchains, (int)real.imageFormat,
            real.imageExtent.width, real.imageExtent.height);
      fflush(stdout);
   }
   return next(device, &real, cb, swapchain);
}

static VKAPI_ATTR VkResult VKAPI_CALL fh_GetSwapchainImagesKHR(
      VkDevice device, VkSwapchainKHR swapchain, uint32_t *count,
      VkImage *images)
{
   struct fh_standin *s = fh_standin_find(swapchain);
   uint32_t i;
   VkResult res;

   if (!s)
      return FH_NEXT(GetSwapchainImagesKHR)(device, swapchain, count, images);
   res = fh_hand_out(s->count, count, images != NULL);
   if (images)
      for (i = 0; i < *count && i < s->count; i++)
         images[i] = s->images[i];
   return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL fh_QueuePresentKHR(VkQueue queue,
      const VkPresentInfoKHR *info)
{
   VkPipelineStageFlags stages[FH_WAITS];
   VkCommandBuffer cmds[FH_STANDINS];
   VkPresentInfoKHR present = *info;
   VkSubmitInfo submit;
   VkFence fence = VK_NULL_HANDLE;
   uint32_t i, n = 0;
   VkResult res;

   for (i = 0; i < info->swapchainCount && n < FH_STANDINS; i++)
   {
      struct fh_standin *s = fh_standin_find(info->pSwapchains[i]);
      if (!s)
         continue;
      cmds[n++] = s->cmds[info->pImageIndices[i]];
      if (fence == VK_NULL_HANDLE)
         fence = s->fences[info->pImageIndices[i]];
   }
   if (!n)
      return FH_NEXT(QueuePresentKHR)(queue, info);
   if (info->waitSemaphoreCount > FH_WAITS)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   for (i = 0; i < info->waitSemaphoreCount; i++)
      stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
   memset(&submit, 0, sizeof(submit));
   submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   submit.waitSemaphoreCount = info->waitSemaphoreCount;
   submit.pWaitSemaphores    = info->pWaitSemaphores;
   submit.pWaitDstStageMask  = stages;
   submit.commandBufferCount = n;
   submit.pCommandBuffers    = cmds;
   if (     (res = FH_NEXT(ResetFences)(s_device, 1, &fence)) != VK_SUCCESS
         || (res = FH_NEXT(QueueSubmit)(queue, 1, &submit, fence))
               != VK_SUCCESS)
      return res;
   /* Test tooling on lavapipe: the blit ends before the image is reused. */
   if ((res = FH_NEXT(WaitForFences)(s_device, 1, &fence, VK_TRUE,
               UINT64_MAX)) != VK_SUCCESS)
      return res;

   present.waitSemaphoreCount = 0;
   present.pWaitSemaphores    = NULL;
   return FH_NEXT(QueuePresentKHR)(queue, &present);
}

static VKAPI_ATTR void VKAPI_CALL fh_DestroySwapchainKHR(VkDevice device,
      VkSwapchainKHR swapchain, const VkAllocationCallbacks *cb)
{
   struct fh_standin *s = fh_standin_find(swapchain);
   if (s)
      fh_standin_free(s);
   FH_NEXT(DestroySwapchainKHR)(device, swapchain, cb);
}

static VKAPI_ATTR VkResult VKAPI_CALL fh_CreateDevice(VkPhysicalDevice gpu,
      const VkDeviceCreateInfo *info, const VkAllocationCallbacks *cb,
      VkDevice *device)
{
   VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo*)info->pNext;
   VkLayerDeviceCreateInfo *data  = chain;
   PFN_vkGetInstanceProcAddr gipa;
   PFN_vkCreateDevice create;
   VkResult res;

   while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
            && chain->function == VK_LAYER_LINK_INFO))
      chain = (VkLayerDeviceCreateInfo*)chain->pNext;
   if (!chain)
      return VK_ERROR_INITIALIZATION_FAILED;
   while (data && !(data->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
            && data->function == VK_LOADER_DATA_CALLBACK))
      data = (VkLayerDeviceCreateInfo*)data->pNext;

   gipa        = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   s_next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
   chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
   s_set_loader_data   = data ? data->u.pfnSetDeviceLoaderData : NULL;
   s_gpu               = gpu;
   s_family            = info->queueCreateInfoCount
      ? info->pQueueCreateInfos[0].queueFamilyIndex : 0;

   create = (PFN_vkCreateDevice)gipa(s_instance, "vkCreateDevice");
   res    = create(gpu, info, cb, device);
   if (res == VK_SUCCESS)
      s_device = *device;
   return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL fh_CreateInstance(
      const VkInstanceCreateInfo *info, const VkAllocationCallbacks *cb,
      VkInstance *instance)
{
   VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo*)info->pNext;
   const char *scrgb_only = getenv("FAKE_HDR_SCRGB_ONLY");
   PFN_vkCreateInstance create;
   VkResult res;

   while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
            && chain->function == VK_LAYER_LINK_INFO))
      chain = (VkLayerInstanceCreateInfo*)chain->pNext;
   if (!chain)
      return VK_ERROR_INITIALIZATION_FAILED;

   s_next_gipa         = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
   s_scrgb_only        = scrgb_only && !strcmp(scrgb_only, "1");

   create = (PFN_vkCreateInstance)s_next_gipa(NULL, "vkCreateInstance");
   res    = create(info, cb, instance);
   if (res == VK_SUCCESS)
      s_instance = *instance;
   return res;
}

FH_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fh_GetDeviceProcAddr(
      VkDevice device, const char *name)
{
   if (!strcmp(name, "vkCreateSwapchainKHR"))
      return (PFN_vkVoidFunction)fh_CreateSwapchainKHR;
   if (!strcmp(name, "vkGetSwapchainImagesKHR"))
      return (PFN_vkVoidFunction)fh_GetSwapchainImagesKHR;
   if (!strcmp(name, "vkQueuePresentKHR"))
      return (PFN_vkVoidFunction)fh_QueuePresentKHR;
   if (!strcmp(name, "vkDestroySwapchainKHR"))
      return (PFN_vkVoidFunction)fh_DestroySwapchainKHR;
   if (!strcmp(name, "vkGetDeviceProcAddr"))
      return (PFN_vkVoidFunction)fh_GetDeviceProcAddr;
   return s_next_gdpa ? s_next_gdpa(device, name) : NULL;
}

FH_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fh_GetInstanceProcAddr(
      VkInstance instance, const char *name)
{
#define FH_HOOK(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)fh_##n
   FH_HOOK(CreateInstance);
   FH_HOOK(CreateDevice);
   FH_HOOK(CreateSwapchainKHR);
   FH_HOOK(GetSwapchainImagesKHR);
   FH_HOOK(QueuePresentKHR);
   FH_HOOK(DestroySwapchainKHR);
   FH_HOOK(GetPhysicalDeviceSurfaceFormatsKHR);
   FH_HOOK(GetPhysicalDeviceSurfaceFormats2KHR);
   FH_HOOK(GetInstanceProcAddr);
   FH_HOOK(GetDeviceProcAddr);
#undef FH_HOOK
   return s_next_gipa ? s_next_gipa(instance, name) : NULL;
}

FH_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
      VkNegotiateLayerInterface *v)
{
   if (v->loaderLayerInterfaceVersion > 2)
      v->loaderLayerInterfaceVersion = 2;
   v->pfnGetInstanceProcAddr       = fh_GetInstanceProcAddr;
   v->pfnGetDeviceProcAddr         = fh_GetDeviceProcAddr;
   v->pfnGetPhysicalDeviceProcAddr = NULL;
   return VK_SUCCESS;
}
