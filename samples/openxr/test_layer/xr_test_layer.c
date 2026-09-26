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

/* Test-only OpenXR API layer for RetroArch's headset output.
 *
 * Records every xrEndFrame's layers as JSON lines, copies each swapchain
 * image as the application releases it, and writes the images a frame
 * shows to PNG every Nth frame. A script file overrides answers the
 * runtime gives: today the head pose. The headset input tests add
 * action states, aim poses and focus changes as more script commands.
 *
 *   RA_XR_LAYER_OUT         directory for frames.jsonl and snap_*.png
 *   RA_XR_LAYER_SNAP_EVERY  write images every Nth frame; 0 never
 *   RA_XR_LAYER_SCRIPT      script file, re-read when it changes
 *
 * Script lines:
 *   head <x> <y> <z> <yaw>  the VIEW space's pose in LOCAL, yaw in degrees
 *   head off                the runtime's own pose again
 *   fail session            xrCreateSession fails
 *   state <n>               the next xrPollEvent reports session state n
 */

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#define MAX_CHAINS 32
#define MAX_IMAGES 8
#define MAX_SPACES 64

struct chain
{
   XrSwapchain handle;
   int id;
   uint32_t width, height, layers;
   int64_t format;
   VkImage images[MAX_IMAGES];
   uint32_t num_images;
   uint32_t acquired[MAX_IMAGES];   /* indices, oldest first */
   uint32_t num_acquired;
   unsigned char *pixels;           /* last released image, all layers */
   bool has_pixels;
   VkBuffer buffer;
   VkDeviceMemory memory;
   VkDeviceSize size;
};

static struct
{
   pthread_mutex_t lock;
   FILE *out;
   char dir[1024];
   unsigned snap_every;
   char script[1024];
   struct timespec script_mtime;
   bool head_set;
   bool fail_session;
   int inject_state;
   XrSession session;
   XrPosef head;
   uint64_t frames;
   XrDuration period;

   VkPhysicalDevice gpu;
   VkDevice device;
   VkQueue queue;
   uint32_t queue_family;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   VkFence fence;

   struct chain chains[MAX_CHAINS];
   int next_id;
   XrSpace spaces[MAX_SPACES];
   XrReferenceSpaceType space_types[MAX_SPACES];
   unsigned num_spaces;

   PFN_xrGetInstanceProcAddr gipa;
   PFN_xrCreateSession CreateSession;
   PFN_xrDestroySession DestroySession;
   PFN_xrPollEvent PollEvent;
   PFN_xrCreateReferenceSpace CreateReferenceSpace;
   PFN_xrLocateSpace LocateSpace;
   PFN_xrCreateSwapchain CreateSwapchain;
   PFN_xrDestroySwapchain DestroySwapchain;
   PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages;
   PFN_xrAcquireSwapchainImage AcquireSwapchainImage;
   PFN_xrReleaseSwapchainImage ReleaseSwapchainImage;
   PFN_xrWaitFrame WaitFrame;
   PFN_xrEndFrame EndFrame;
} L = { .lock = PTHREAD_MUTEX_INITIALIZER };

static long long now_us(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void layer_open(void)
{
   const char *dir   = getenv("RA_XR_LAYER_OUT");
   const char *snap  = getenv("RA_XR_LAYER_SNAP_EVERY");
   const char *scr   = getenv("RA_XR_LAYER_SCRIPT");
   char path[1100];
   if (dir && *dir && !L.out)
   {
      snprintf(L.dir, sizeof(L.dir), "%s", dir);
      snprintf(path, sizeof(path), "%s/frames.jsonl", dir);
      L.out = fopen(path, "w");
   }
   L.snap_every = snap ? (unsigned)strtoul(snap, NULL, 10) : 0;
   if (scr)
      snprintf(L.script, sizeof(L.script), "%s", scr);
}

static struct chain *find_chain(XrSwapchain h)
{
   int i;
   for (i = 0; i < MAX_CHAINS; i++)
      if (L.chains[i].handle == h)
         return &L.chains[i];
   return NULL;
}

static int space_type(XrSpace s)
{
   unsigned i;
   for (i = 0; i < L.num_spaces; i++)
      if (L.spaces[i] == s)
         return (int)L.space_types[i];
   return -1;
}

/* ---- Script ---- */

static void script_head(const char *args)
{
   float x, y, z, yaw;
   if (!strncmp(args, "off", 3))
   {
      L.head_set = false;
      return;
   }
   if (sscanf(args, "%f %f %f %f", &x, &y, &z, &yaw) != 4)
      return;
   L.head.position.x    = x;
   L.head.position.y    = y;
   L.head.position.z    = z;
   L.head.orientation.x = 0.0f;
   L.head.orientation.y = sinf(yaw * (float)M_PI / 360.0f);
   L.head.orientation.z = 0.0f;
   L.head.orientation.w = cosf(yaw * (float)M_PI / 360.0f);
   L.head_set           = true;
}

static void script_fail(const char *args)
{
   L.fail_session = !strncmp(args, "session", 7);
}

static void script_state(const char *args)
{
   L.inject_state = atoi(args);
}

static const struct
{
   const char *name;
   void (*run)(const char *args);
} script_cmds[] = {
   { "head", script_head },
   { "fail", script_fail },
   { "state", script_state },
};

/* Caller holds L.lock. */
static void script_poll(void)
{
   size_t i;
   char line[256];
   struct stat st;
   FILE *f;
   if (!L.script[0] || stat(L.script, &st) != 0)
      return;
   if (     st.st_mtim.tv_sec  == L.script_mtime.tv_sec
         && st.st_mtim.tv_nsec == L.script_mtime.tv_nsec)
      return;
   L.script_mtime = st.st_mtim;
   if (!(f = fopen(L.script, "r")))
      return;
   while (fgets(line, sizeof(line), f))
      for (i = 0; i < sizeof(script_cmds) / sizeof(script_cmds[0]); i++)
      {
         size_t n = strlen(script_cmds[i].name);
         if (!strncmp(line, script_cmds[i].name, n)
               && (line[n] == ' ' || line[n] == '\n' || !line[n]))
            script_cmds[i].run(line + n + (line[n] == ' '));
      }
   fclose(f);
}

/* ---- Copies and PNGs ---- */

static bool rgba8(int64_t format, bool *bgra)
{
   switch (format)
   {
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_R8G8B8A8_SRGB:
         *bgra = false;
         return true;
      case VK_FORMAT_B8G8R8A8_UNORM:
      case VK_FORMAT_B8G8R8A8_SRGB:
         *bgra = true;
         return true;
      default:
         break;
   }
   return false;
}

static bool ensure_cmd(void)
{
   VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
   VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   if (L.cmd)
      return true;
   pi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   pi.queueFamilyIndex = L.queue_family;
   if (vkCreateCommandPool(L.device, &pi, NULL, &L.pool) != VK_SUCCESS)
      return false;
   ai.commandPool        = L.pool;
   ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   ai.commandBufferCount = 1;
   if (vkAllocateCommandBuffers(L.device, &ai, &L.cmd) != VK_SUCCESS)
      return false;
   return vkCreateFence(L.device, &fi, NULL, &L.fence) == VK_SUCCESS;
}

static bool ensure_buffer(struct chain *c, VkDeviceSize size)
{
   uint32_t i;
   VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
   VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
   VkMemoryRequirements req;
   VkPhysicalDeviceMemoryProperties props;
   VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   if (c->buffer && c->size == size)
      return true;
   if (c->buffer)
   {
      vkDestroyBuffer(L.device, c->buffer, NULL);
      vkFreeMemory(L.device, c->memory, NULL);
      c->buffer = VK_NULL_HANDLE;
   }
   free(c->pixels);
   if (!(c->pixels = malloc(size)))
      return false;
   bi.size        = size;
   bi.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   if (vkCreateBuffer(L.device, &bi, NULL, &c->buffer) != VK_SUCCESS)
      return false;
   vkGetBufferMemoryRequirements(L.device, c->buffer, &req);
   vkGetPhysicalDeviceMemoryProperties(L.gpu, &props);
   for (i = 0; i < props.memoryTypeCount; i++)
      if ((req.memoryTypeBits & (1u << i))
            && (props.memoryTypes[i].propertyFlags & want) == want)
         break;
   if (i == props.memoryTypeCount)
      return false;
   mi.allocationSize  = req.size;
   mi.memoryTypeIndex = i;
   if (vkAllocateMemory(L.device, &mi, NULL, &c->memory) != VK_SUCCESS)
      return false;
   vkBindBufferMemory(L.device, c->buffer, c->memory, 0);
   c->size = size;
   return true;
}

/* Caller holds L.lock. Submitting on the application's queue is safe
 * because RetroArch calls xrReleaseSwapchainImage under its queue lock,
 * and hello_xr is single-threaded. */
static void capture(struct chain *c, uint32_t index)
{
   bool bgra;
   void *map;
   VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
   VkBufferImageCopy region;
   VkDeviceSize size = (VkDeviceSize)c->width * c->height * 4 * c->layers;

   if (!L.device || !L.snap_every || index >= c->num_images
         || !rgba8(c->format, &bgra))
      return;
   if (!ensure_cmd() || !ensure_buffer(c, size))
      return;

   vkResetCommandBuffer(L.cmd, 0);
   bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   vkBeginCommandBuffer(L.cmd, &bi);
   b.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   b.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
   b.oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.image               = c->images[index];
   b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   b.subresourceRange.levelCount = 1;
   b.subresourceRange.layerCount = c->layers;
   vkCmdPipelineBarrier(L.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   memset(&region, 0, sizeof(region));
   region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   region.imageSubresource.layerCount = c->layers;
   region.imageExtent.width           = c->width;
   region.imageExtent.height          = c->height;
   region.imageExtent.depth           = 1;
   vkCmdCopyImageToBuffer(L.cmd, c->images[index],
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->buffer, 1, &region);
   b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   vkCmdPipelineBarrier(L.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL,
         1, &b);
   vkEndCommandBuffer(L.cmd);

   si.commandBufferCount = 1;
   si.pCommandBuffers    = &L.cmd;
   vkResetFences(L.device, 1, &L.fence);
   if (vkQueueSubmit(L.queue, 1, &si, L.fence) != VK_SUCCESS)
      return;
   vkWaitForFences(L.device, 1, &L.fence, VK_TRUE, UINT64_MAX);
   if (vkMapMemory(L.device, c->memory, 0, size, 0, &map) != VK_SUCCESS)
      return;
   memcpy(c->pixels, map, size);
   vkUnmapMemory(L.device, c->memory);
   c->has_pixels = true;
}

static void put_be32(unsigned char *p, uint32_t v)
{
   p[0] = (unsigned char)(v >> 24);
   p[1] = (unsigned char)(v >> 16);
   p[2] = (unsigned char)(v >> 8);
   p[3] = (unsigned char)v;
}

static void png_chunk(FILE *f, const char *type,
      const unsigned char *data, uint32_t len)
{
   unsigned char b[4];
   uLong crc = crc32(0L, (const Bytef*)type, 4);
   if (len)
      crc = crc32(crc, data, len);
   put_be32(b, len);
   fwrite(b, 1, 4, f);
   fwrite(type, 1, 4, f);
   if (len)
      fwrite(data, 1, len, f);
   put_be32(b, (uint32_t)crc);
   fwrite(b, 1, 4, f);
}

static void png_write(const char *path, const unsigned char *src,
      uint32_t w, uint32_t h, bool bgra)
{
   static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
   unsigned char ihdr[13];
   size_t stride = (size_t)w * 4 + 1;
   uLongf zlen   = compressBound(stride * h);
   unsigned char *raw = malloc(stride * h);
   unsigned char *z   = malloc(zlen);
   uint32_t x, y;
   FILE *f;
   if (!raw || !z)
      goto end;
   for (y = 0; y < h; y++)
   {
      unsigned char *row = raw + y * stride;
      row[0] = 0;
      for (x = 0; x < w; x++)
      {
         const unsigned char *p = src + ((size_t)y * w + x) * 4;
         row[1 + x * 4 + 0] = bgra ? p[2] : p[0];
         row[1 + x * 4 + 1] = p[1];
         row[1 + x * 4 + 2] = bgra ? p[0] : p[2];
         row[1 + x * 4 + 3] = p[3];
      }
   }
   if (compress2(z, &zlen, raw, stride * h, 6) != Z_OK)
      goto end;
   if (!(f = fopen(path, "wb")))
      goto end;
   put_be32(ihdr, w);
   put_be32(ihdr + 4, h);
   ihdr[8]  = 8;   /* bits */
   ihdr[9]  = 6;   /* RGBA */
   ihdr[10] = 0;
   ihdr[11] = 0;
   ihdr[12] = 0;
   fwrite(sig, 1, 8, f);
   png_chunk(f, "IHDR", ihdr, 13);
   png_chunk(f, "IDAT", z, (uint32_t)zlen);
   png_chunk(f, "IEND", NULL, 0);
   fclose(f);
end:
   free(raw);
   free(z);
}

/* Caller holds L.lock. */
static void snap(uint64_t n, XrSwapchain h, uint32_t layer)
{
   bool bgra;
   char path[1200];
   struct chain *c = find_chain(h);
   if (!c || !c->has_pixels || layer >= c->layers || !rgba8(c->format, &bgra))
      return;
   snprintf(path, sizeof(path), "%s/snap_%llu_sc%d_l%u.png", L.dir,
         (unsigned long long)n, c->id, layer);
   png_write(path, c->pixels + (size_t)c->width * c->height * 4 * layer,
         c->width, c->height, bgra);
}

/* ---- Hooks ---- */

static XRAPI_ATTR XrResult XRAPI_CALL layer_CreateSession(XrInstance instance,
      const XrSessionCreateInfo *info, XrSession *session)
{
   bool fail;
   XrResult res;
   const XrBaseInStructure *p;
   pthread_mutex_lock(&L.lock);
   script_poll();
   fail = L.fail_session;
   pthread_mutex_unlock(&L.lock);
   if (fail)
      return XR_ERROR_RUNTIME_FAILURE;
   if (XR_FAILED(res = L.CreateSession(instance, info, session)))
      return res;
   pthread_mutex_lock(&L.lock);
   for (p = (const XrBaseInStructure*)info->next; p; p = p->next)
      if (p->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR)
      {
         const XrGraphicsBindingVulkanKHR *b =
            (const XrGraphicsBindingVulkanKHR*)p;
         L.session      = *session;
         L.gpu          = b->physicalDevice;
         L.device       = b->device;
         L.queue_family = b->queueFamilyIndex;
         vkGetDeviceQueue(b->device, b->queueFamilyIndex, b->queueIndex,
               &L.queue);
      }
   pthread_mutex_unlock(&L.lock);
   return res;
}

static XRAPI_ATTR XrResult XRAPI_CALL layer_DestroySession(XrSession session)
{
   int i;
   pthread_mutex_lock(&L.lock);
   /* Before the application destroys its device. */
   if (L.device)
   {
      for (i = 0; i < MAX_CHAINS; i++)
         if (L.chains[i].buffer)
         {
            vkDestroyBuffer(L.device, L.chains[i].buffer, NULL);
            vkFreeMemory(L.device, L.chains[i].memory, NULL);
            L.chains[i].buffer = VK_NULL_HANDLE;
            L.chains[i].size   = 0;
         }
      if (L.fence)
         vkDestroyFence(L.device, L.fence, NULL);
      if (L.pool)
         vkDestroyCommandPool(L.device, L.pool, NULL);
      L.fence  = VK_NULL_HANDLE;
      L.pool   = VK_NULL_HANDLE;
      L.cmd    = VK_NULL_HANDLE;
      L.device = VK_NULL_HANDLE;
   }
   pthread_mutex_unlock(&L.lock);
   return L.DestroySession(session);
}

static XRAPI_ATTR XrResult XRAPI_CALL layer_PollEvent(XrInstance instance,
      XrEventDataBuffer *ev)
{
   int inject;
   XrResult res;
   pthread_mutex_lock(&L.lock);
   script_poll();
   inject         = L.session ? L.inject_state : 0;
   L.inject_state = 0;
   pthread_mutex_unlock(&L.lock);
   if (inject)
   {
      XrEventDataSessionStateChanged *s = (XrEventDataSessionStateChanged*)ev;
      memset(ev, 0, sizeof(*ev));
      s->type    = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
      s->session = L.session;
      s->state   = (XrSessionState)inject;
      res        = XR_SUCCESS;
   }
   else
      res = L.PollEvent(instance, ev);
   if (res == XR_SUCCESS && ev->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
   {
      const XrEventDataSessionStateChanged *s =
         (const XrEventDataSessionStateChanged*)ev;
      pthread_mutex_lock(&L.lock);
      if (L.out)
      {
         fprintf(L.out, "{\"ev\":\"state\",\"state\":%d,\"t_us\":%lld}\n",
               (int)s->state, now_us());
         fflush(L.out);
      }
      pthread_mutex_unlock(&L.lock);
   }
   return res;
}

static XRAPI_ATTR XrResult XRAPI_CALL layer_CreateReferenceSpace(XrSession session,
      const XrReferenceSpaceCreateInfo *info, XrSpace *space)
{
   XrResult res = L.CreateReferenceSpace(session, info, space);
   if (XR_SUCCEEDED(res))
   {
      pthread_mutex_lock(&L.lock);
      if (L.num_spaces < MAX_SPACES)
      {
         L.spaces[L.num_spaces]      = *space;
         L.space_types[L.num_spaces] = info->referenceSpaceType;
         L.num_spaces++;
      }
      pthread_mutex_unlock(&L.lock);
   }
   return res;
}

static XRAPI_ATTR XrResult XRAPI_CALL layer_LocateSpace(XrSpace space,
      XrSpace base, XrTime time, XrSpaceLocation *loc)
{
   XrResult res = L.LocateSpace(space, base, time, loc);
   pthread_mutex_lock(&L.lock);
   script_poll();
   if (     XR_SUCCEEDED(res) && L.head_set
         && space_type(space) == XR_REFERENCE_SPACE_TYPE_VIEW
         && space_type(base)  == XR_REFERENCE_SPACE_TYPE_LOCAL)
   {
      loc->pose          = L.head;
      loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT
         | XR_SPACE_LOCATION_POSITION_VALID_BIT
         | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT
         | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
   }
   pthread_mutex_unlock(&L.lock);
   return res;
}

static XRAPI_ATTR XrResult XRAPI_CALL layer_CreateSwapchain(XrSession session,
      const XrSwapchainCreateInfo *info, XrSwapchain *sc)
{
   int i;
   XrResult res;
   XrSwapchainCreateInfo ci = *info;
   /* So released images can be copied back. */
   ci.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
   res = L.CreateSwapchain(session, &ci, sc);
   if (XR_FAILED(res))
      return res;
   pthread_mutex_lock(&L.lock);
   for (i = 0; i < MAX_CHAINS; i++)
      if (!L.chains[i].handle)
      {
         struct chain *c = &L.chains[i];
         free(c->pixels);
         memset(c, 0, sizeof(*c));
         c->handle = *sc;
         c->id     = L.next_id++;
         c->width  = info->width;
         c->height = info->height;
         c->layers = info->arraySize;
         c->format = info->format;
         if (L.out)
         {
            fprintf(L.out, "{\"ev\":\"swapchain\",\"sc\":%d,\"w\":%u,\"h\":%u,"
                  "\"layers\":%u,\"format\":%lld}\n", c->id, c->width,
                  c->height, c->layers, (long long)c->format);
            fflush(L.out);
         }
         break;
      }
   pthread_mutex_unlock(&L.lock);
   return res;
}

static XRAPI_ATTR XrResult XRAPI_CALL layer_DestroySwapchain(XrSwapchain sc)
{
   struct chain *c;
   pthread_mutex_lock(&L.lock);
   if ((c = find_chain(sc)))
   {
      if (c->buffer && L.device)
      {
         vkDestroyBuffer(L.device, c->buffer, NULL);
         vkFreeMemory(L.device, c->memory, NULL);
      }
      free(c->pixels);
      memset(c, 0, sizeof(*c));
   }
   pthread_mutex_unlock(&L.lock);
   return L.DestroySwapchain(sc);
}

static XRAPI_ATTR XrResult XRAPI_CALL layer_EnumerateSwapchainImages(
      XrSwapchain sc, uint32_t cap, uint32_t *count,
      XrSwapchainImageBaseHeader *images)
{
   uint32_t i;
   struct chain *c;
   XrResult res = L.EnumerateSwapchainImages(sc, cap, count, images);
   if (XR_FAILED(res) || !images || images->type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR)
      return res;
   pthread_mutex_lock(&L.lock);
   if ((c = find_chain(sc)))
   {
      const XrSwapchainImageVulkanKHR *vk = (const XrSwapchainImageVulkanKHR*)images;
      c->num_images = *count < MAX_IMAGES ? *count : MAX_IMAGES;
      for (i = 0; i < c->num_images; i++)
         c->images[i] = vk[i].image;
   }
   pthread_mutex_unlock(&L.lock);
   return res;
}

static XRAPI_ATTR XrResult XRAPI_CALL layer_AcquireSwapchainImage(XrSwapchain sc,
      const XrSwapchainImageAcquireInfo *info, uint32_t *index)
{
   struct chain *c;
   XrResult res = L.AcquireSwapchainImage(sc, info, index);
   if (XR_FAILED(res))
      return res;
   pthread_mutex_lock(&L.lock);
   if ((c = find_chain(sc)) && c->num_acquired < MAX_IMAGES)
      c->acquired[c->num_acquired++] = *index;
   pthread_mutex_unlock(&L.lock);
   return res;
}

static XRAPI_ATTR XrResult XRAPI_CALL layer_ReleaseSwapchainImage(XrSwapchain sc,
      const XrSwapchainImageReleaseInfo *info)
{
   struct chain *c;
   pthread_mutex_lock(&L.lock);
   if ((c = find_chain(sc)) && c->num_acquired)
   {
      uint32_t index = c->acquired[0];
      memmove(c->acquired, c->acquired + 1,
            (c->num_acquired - 1) * sizeof(c->acquired[0]));
      c->num_acquired--;
      capture(c, index);
      if (L.out)
      {
         fprintf(L.out, "{\"ev\":\"release\",\"sc\":%d,\"t_us\":%lld}\n",
               c->id, now_us());
         fflush(L.out);
      }
   }
   pthread_mutex_unlock(&L.lock);
   return L.ReleaseSwapchainImage(sc, info);
}

static XRAPI_ATTR XrResult XRAPI_CALL layer_WaitFrame(XrSession session,
      const XrFrameWaitInfo *info, XrFrameState *state)
{
   XrResult res = L.WaitFrame(session, info, state);
   if (XR_SUCCEEDED(res))
   {
      pthread_mutex_lock(&L.lock);
      L.period = state->predictedDisplayPeriod;
      pthread_mutex_unlock(&L.lock);
   }
   return res;
}

static const char *eye_name(XrEyeVisibility e)
{
   return e == XR_EYE_VISIBILITY_LEFT ? "left"
      : (e == XR_EYE_VISIBILITY_RIGHT ? "right" : "both");
}

static const char *space_name(XrSpace s)
{
   int t = space_type(s);
   return t == XR_REFERENCE_SPACE_TYPE_LOCAL ? "local"
      : (t == XR_REFERENCE_SPACE_TYPE_VIEW ? "view" : "other");
}

static int chain_id(XrSwapchain h)
{
   struct chain *c = find_chain(h);
   return c ? c->id : -1;
}

static XRAPI_ATTR XrResult XRAPI_CALL layer_EndFrame(XrSession session,
      const XrFrameEndInfo *info)
{
   uint32_t i, v;
   uint64_t n;
   bool do_snap;
   pthread_mutex_lock(&L.lock);
   script_poll();
   n       = ++L.frames;
   do_snap = L.snap_every && !(n % L.snap_every);
   if (L.out)
   {
      fprintf(L.out, "{\"ev\":\"frame\",\"n\":%llu,\"t_us\":%lld,"
            "\"period_ns\":%lld,\"snap\":%s,\"layers\":[",
            (unsigned long long)n, now_us(), (long long)L.period,
            do_snap ? "true" : "false");
      for (i = 0; i < info->layerCount; i++)
      {
         const XrCompositionLayerBaseHeader *h = info->layers[i];
         if (i)
            fputc(',', L.out);
         if (h->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
         {
            const XrCompositionLayerQuad *q = (const XrCompositionLayerQuad*)h;
            fprintf(L.out, "{\"type\":\"quad\",\"sc\":%d,\"eye\":\"%s\","
                  "\"flags\":%llu,\"space\":\"%s\","
                  "\"pose\":[%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f],"
                  "\"size\":[%.5f,%.5f],\"rect\":[%d,%d,%d,%d],\"layer\":%u}",
                  chain_id(q->subImage.swapchain), eye_name(q->eyeVisibility),
                  (unsigned long long)q->layerFlags, space_name(q->space),
                  q->pose.position.x, q->pose.position.y, q->pose.position.z,
                  q->pose.orientation.x, q->pose.orientation.y,
                  q->pose.orientation.z, q->pose.orientation.w,
                  q->size.width, q->size.height,
                  q->subImage.imageRect.offset.x, q->subImage.imageRect.offset.y,
                  q->subImage.imageRect.extent.width,
                  q->subImage.imageRect.extent.height,
                  q->subImage.imageArrayIndex);
            if (do_snap)
               snap(n, q->subImage.swapchain, q->subImage.imageArrayIndex);
         }
         else if (h->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
         {
            const XrCompositionLayerProjection *p =
               (const XrCompositionLayerProjection*)h;
            fputs("{\"type\":\"projection\",\"views\":[", L.out);
            for (v = 0; v < p->viewCount; v++)
            {
               fprintf(L.out, "%s{\"sc\":%d,\"layer\":%u}", v ? "," : "",
                     chain_id(p->views[v].subImage.swapchain),
                     p->views[v].subImage.imageArrayIndex);
               if (do_snap)
                  snap(n, p->views[v].subImage.swapchain,
                        p->views[v].subImage.imageArrayIndex);
            }
            fputs("]}", L.out);
         }
         else
            fprintf(L.out, "{\"type\":\"other\",\"xr_type\":%d}", (int)h->type);
      }
      fputs("]}\n", L.out);
      fflush(L.out);
   }
   pthread_mutex_unlock(&L.lock);
   return L.EndFrame(session, info);
}

/* ---- Loader interface ---- */

static XRAPI_ATTR XrResult XRAPI_CALL layer_GetInstanceProcAddr(XrInstance instance,
      const char *name, PFN_xrVoidFunction *fn);

#define HOOK(n) if (!strcmp(name, "xr" #n)) \
   { *fn = (PFN_xrVoidFunction)layer_##n; return XR_SUCCESS; }

static XRAPI_ATTR XrResult XRAPI_CALL layer_GetInstanceProcAddr(XrInstance instance,
      const char *name, PFN_xrVoidFunction *fn)
{
   HOOK(GetInstanceProcAddr)
   HOOK(CreateSession)
   HOOK(DestroySession)
   HOOK(PollEvent)
   HOOK(CreateReferenceSpace)
   HOOK(LocateSpace)
   HOOK(CreateSwapchain)
   HOOK(DestroySwapchain)
   HOOK(EnumerateSwapchainImages)
   HOOK(AcquireSwapchainImage)
   HOOK(ReleaseSwapchainImage)
   HOOK(WaitFrame)
   HOOK(EndFrame)
   if (!L.gipa)
      return XR_ERROR_FUNCTION_UNSUPPORTED;
   return L.gipa(instance, name, fn);
}

#undef HOOK

static XRAPI_ATTR XrResult XRAPI_CALL layer_CreateApiLayerInstance(
      const XrInstanceCreateInfo *info, const XrApiLayerCreateInfo *li,
      XrInstance *instance)
{
   XrResult res;
   XrApiLayerCreateInfo next;
   if (!li || !li->nextInfo)
      return XR_ERROR_INITIALIZATION_FAILED;
   next          = *li;
   next.nextInfo = li->nextInfo->next;
   res = li->nextInfo->nextCreateApiLayerInstance(info, &next, instance);
   if (XR_FAILED(res))
      return res;
   L.gipa = li->nextInfo->nextGetInstanceProcAddr;
#define NEXT(n) L.gipa(*instance, "xr" #n, (PFN_xrVoidFunction*)&L.n)
   NEXT(CreateSession);
   NEXT(DestroySession);
   NEXT(PollEvent);
   NEXT(CreateReferenceSpace);
   NEXT(LocateSpace);
   NEXT(CreateSwapchain);
   NEXT(DestroySwapchain);
   NEXT(EnumerateSwapchainImages);
   NEXT(AcquireSwapchainImage);
   NEXT(ReleaseSwapchainImage);
   NEXT(WaitFrame);
   NEXT(EndFrame);
#undef NEXT
   pthread_mutex_lock(&L.lock);
   layer_open();
   pthread_mutex_unlock(&L.lock);
   return res;
}

__attribute__((visibility("default")))
XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
      const XrNegotiateLoaderInfo *li, const char *name,
      XrNegotiateApiLayerRequest *req)
{
   (void)name;
   if (     !li || !req
         || li->structType  != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO
         || req->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST
         || li->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION
         || li->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
      return XR_ERROR_INITIALIZATION_FAILED;
   req->layerInterfaceVersion  = XR_CURRENT_LOADER_API_LAYER_VERSION;
   req->layerApiVersion        = XR_CURRENT_API_VERSION;
   req->getInstanceProcAddr    = layer_GetInstanceProcAddr;
   req->createApiLayerInstance = layer_CreateApiLayerInstance;
   return XR_SUCCESS;
}
