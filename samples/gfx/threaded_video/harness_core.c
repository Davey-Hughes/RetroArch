/* Minimal libretro core for the threaded video harness: supports
 * running without content, so the frontend treats it as a real core
 * (the menu can be closed over it, unlike the dummy core), and hands
 * the frontend a frame per retro_run, sometimes duplicated and
 * sometimes taller than the geometry it declared. Loaded after
 * harness_core_use_vulkan(1), it renders into Vulkan images of its own
 * instead and hands them over with set_image, behind a semaphore. */
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <libretro.h>
#define VK_NO_PROTOTYPES
#include <libretro_vulkan.h>

#define W 320
#define H 240
#define H_OVERSIZE 600

static retro_video_refresh_t video_cb;
static retro_environment_t   environ_cb;
static uint16_t frame[W * H_OVERSIZE];
static unsigned runs;

/* Vulkan mode: one image, command buffer, semaphore and fence per sync
 * index, cleared to a new colour each frame. */
#define VK_SLOTS 8
static int      harness_vk;
static unsigned harness_vk_frames;
static struct retro_hw_render_callback hw_render;
static const struct retro_hw_render_interface_vulkan *vk;
static VkCommandPool vk_pool;
static int      vk_ready;
static struct
{
   PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
   PFN_vkCreateImage                  vkCreateImage;
   PFN_vkDestroyImage                 vkDestroyImage;
   PFN_vkGetImageMemoryRequirements   vkGetImageMemoryRequirements;
   PFN_vkAllocateMemory               vkAllocateMemory;
   PFN_vkFreeMemory                   vkFreeMemory;
   PFN_vkBindImageMemory              vkBindImageMemory;
   PFN_vkCreateImageView              vkCreateImageView;
   PFN_vkDestroyImageView             vkDestroyImageView;
   PFN_vkCreateCommandPool            vkCreateCommandPool;
   PFN_vkDestroyCommandPool           vkDestroyCommandPool;
   PFN_vkAllocateCommandBuffers       vkAllocateCommandBuffers;
   PFN_vkBeginCommandBuffer           vkBeginCommandBuffer;
   PFN_vkEndCommandBuffer             vkEndCommandBuffer;
   PFN_vkCmdPipelineBarrier           vkCmdPipelineBarrier;
   PFN_vkCmdClearColorImage           vkCmdClearColorImage;
   PFN_vkQueueSubmit                  vkQueueSubmit;
   PFN_vkQueueWaitIdle                vkQueueWaitIdle;
   PFN_vkCreateSemaphore              vkCreateSemaphore;
   PFN_vkDestroySemaphore             vkDestroySemaphore;
   PFN_vkCreateFence                  vkCreateFence;
   PFN_vkDestroyFence                 vkDestroyFence;
   PFN_vkWaitForFences                vkWaitForFences;
   PFN_vkResetFences                  vkResetFences;
} vkf;
static struct vk_slot
{
   struct retro_vulkan_image image;
   VkDeviceMemory  memory;
   VkCommandBuffer cmd;
   VkSemaphore     semaphore;
   VkFence         fence;
   int             pending;
} vk_slots[VK_SLOTS];

RETRO_API void harness_core_use_vulkan(int on)      { harness_vk = on; }
RETRO_API unsigned harness_core_vk_frames(void)     { return harness_vk_frames; }

static void vk_slot_free(struct vk_slot *s)
{
   if (s->image.image_view)
      vkf.vkDestroyImageView(vk->device, s->image.image_view, NULL);
   if (s->image.create_info.image)
      vkf.vkDestroyImage(vk->device, s->image.create_info.image, NULL);
   if (s->memory)
      vkf.vkFreeMemory(vk->device, s->memory, NULL);
   if (s->semaphore)
      vkf.vkDestroySemaphore(vk->device, s->semaphore, NULL);
   if (s->fence)
      vkf.vkDestroyFence(vk->device, s->fence, NULL);
   memset(s, 0, sizeof(*s));
}

static int vk_slot_init(struct vk_slot *s)
{
   VkImageCreateInfo image;
   VkMemoryRequirements req;
   VkMemoryAllocateInfo alloc;
   VkPhysicalDeviceMemoryProperties props;
   VkCommandBufferAllocateInfo cmd;
   VkSemaphoreCreateInfo sem;
   VkFenceCreateInfo fence;
   VkImage img = VK_NULL_HANDLE;
   uint32_t i;

   memset(&image, 0, sizeof(image));
   image.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
   image.flags         = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
   image.imageType     = VK_IMAGE_TYPE_2D;
   image.format        = VK_FORMAT_R8G8B8A8_UNORM;
   image.extent.width  = W;
   image.extent.height = H;
   image.extent.depth  = 1;
   image.mipLevels     = 1;
   image.arrayLayers   = 1;
   image.samples       = VK_SAMPLE_COUNT_1_BIT;
   image.tiling        = VK_IMAGE_TILING_OPTIMAL;
   image.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   if (vkf.vkCreateImage(vk->device, &image, NULL, &img) != VK_SUCCESS)
      return 0;
   s->image.create_info.image = img;

   vkf.vkGetImageMemoryRequirements(vk->device, img, &req);
   vkf.vkGetPhysicalDeviceMemoryProperties(vk->gpu, &props);
   memset(&alloc, 0, sizeof(alloc));
   alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   alloc.allocationSize  = req.size;
   alloc.memoryTypeIndex = props.memoryTypeCount;
   for (i = 0; i < props.memoryTypeCount; i++)
      if (     (req.memoryTypeBits & (1u << i))
            && (props.memoryTypes[i].propertyFlags
               & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      {
         alloc.memoryTypeIndex = i;
         break;
      }
   if (alloc.memoryTypeIndex == props.memoryTypeCount)
      for (i = 0; i < props.memoryTypeCount; i++)
         if (req.memoryTypeBits & (1u << i))
         {
            alloc.memoryTypeIndex = i;
            break;
         }
   if (     vkf.vkAllocateMemory(vk->device, &alloc, NULL, &s->memory) != VK_SUCCESS
         || vkf.vkBindImageMemory(vk->device, img, s->memory, 0) != VK_SUCCESS)
      return 0;

   s->image.create_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
   s->image.create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
   s->image.create_info.format   = VK_FORMAT_R8G8B8A8_UNORM;
   s->image.create_info.components.r = VK_COMPONENT_SWIZZLE_R;
   s->image.create_info.components.g = VK_COMPONENT_SWIZZLE_G;
   s->image.create_info.components.b = VK_COMPONENT_SWIZZLE_B;
   s->image.create_info.components.a = VK_COMPONENT_SWIZZLE_A;
   s->image.create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   s->image.create_info.subresourceRange.levelCount = 1;
   s->image.create_info.subresourceRange.layerCount = 1;
   s->image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   if (vkf.vkCreateImageView(vk->device, &s->image.create_info, NULL,
            &s->image.image_view) != VK_SUCCESS)
      return 0;

   memset(&cmd, 0, sizeof(cmd));
   cmd.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   cmd.commandPool        = vk_pool;
   cmd.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cmd.commandBufferCount = 1;
   memset(&sem, 0, sizeof(sem));
   sem.sType   = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
   memset(&fence, 0, sizeof(fence));
   fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
   return vkf.vkAllocateCommandBuffers(vk->device, &cmd, &s->cmd) == VK_SUCCESS
       && vkf.vkCreateSemaphore(vk->device, &sem, NULL, &s->semaphore) == VK_SUCCESS
       && vkf.vkCreateFence(vk->device, &fence, NULL, &s->fence) == VK_SUCCESS;
}

static void vk_context_reset(void)
{
   VkCommandPoolCreateInfo pool;
   vk_ready = 0;
   if (     !environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void*)&vk)
         || !vk || vk->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN)
      return;
   vkf.vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)
      vk->get_instance_proc_addr(vk->instance, "vkGetPhysicalDeviceMemoryProperties");
#define VK_LOAD(name) vkf.name = (PFN_##name)vk->get_device_proc_addr(vk->device, #name)
   VK_LOAD(vkCreateImage);
   VK_LOAD(vkDestroyImage);
   VK_LOAD(vkGetImageMemoryRequirements);
   VK_LOAD(vkAllocateMemory);
   VK_LOAD(vkFreeMemory);
   VK_LOAD(vkBindImageMemory);
   VK_LOAD(vkCreateImageView);
   VK_LOAD(vkDestroyImageView);
   VK_LOAD(vkCreateCommandPool);
   VK_LOAD(vkDestroyCommandPool);
   VK_LOAD(vkAllocateCommandBuffers);
   VK_LOAD(vkBeginCommandBuffer);
   VK_LOAD(vkEndCommandBuffer);
   VK_LOAD(vkCmdPipelineBarrier);
   VK_LOAD(vkCmdClearColorImage);
   VK_LOAD(vkQueueSubmit);
   VK_LOAD(vkQueueWaitIdle);
   VK_LOAD(vkCreateSemaphore);
   VK_LOAD(vkDestroySemaphore);
   VK_LOAD(vkCreateFence);
   VK_LOAD(vkDestroyFence);
   VK_LOAD(vkWaitForFences);
   VK_LOAD(vkResetFences);
#undef VK_LOAD
   memset(&pool, 0, sizeof(pool));
   pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   pool.queueFamilyIndex = vk->queue_index;
   if (vkf.vkCreateCommandPool(vk->device, &pool, NULL, &vk_pool) != VK_SUCCESS)
      return;
   vk_ready = 1;
}

static void vk_context_destroy(void)
{
   unsigned i;
   if (!vk_ready)
      return;
   /* The frontend's reads of the images are on the queue before this. */
   vk->lock_queue(vk->handle);
   vkf.vkQueueWaitIdle(vk->queue);
   vk->unlock_queue(vk->handle);
   for (i = 0; i < VK_SLOTS; i++)
      vk_slot_free(&vk_slots[i]);
   vkf.vkDestroyCommandPool(vk->device, vk_pool, NULL);
   vk_pool  = VK_NULL_HANDLE;
   vk_ready = 0;
}

/* The core's side of the sync index protocol: wait for the index, then
 * reuse its image, which the frontend must no longer read. */
static int vk_send(void)
{
   VkCommandBufferBeginInfo begin;
   VkImageMemoryBarrier barrier;
   VkClearColorValue color;
   VkSubmitInfo submit;
   VkResult res;
   struct vk_slot *s;
   uint32_t index;

   vk->wait_sync_index(vk->handle);
   index = vk->get_sync_index(vk->handle);
   if (index >= VK_SLOTS)
      return 0;
   s = &vk_slots[index];
   if (!s->semaphore && !vk_slot_init(s))
   {
      vk_slot_free(s);
      return 0;
   }
   if (s->pending)
   {
      vkf.vkWaitForFences(vk->device, 1, &s->fence, VK_TRUE, UINT64_MAX);
      vkf.vkResetFences(vk->device, 1, &s->fence);
      s->pending = 0;
   }

   memset(&begin, 0, sizeof(begin));
   begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   vkf.vkBeginCommandBuffer(s->cmd, &begin);
   memset(&barrier, 0, sizeof(barrier));
   barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
   barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
   barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   barrier.image               = s->image.create_info.image;
   barrier.subresourceRange    = s->image.create_info.subresourceRange;
   vkf.vkCmdPipelineBarrier(s->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
   color.float32[0] = (float)(runs & 0xff) / 255.0f;
   color.float32[1] = 0.5f;
   color.float32[2] = 0.25f;
   color.float32[3] = 1.0f;
   vkf.vkCmdClearColorImage(s->cmd, barrier.image,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
         &barrier.subresourceRange);
   barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
   barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   barrier.newLayout     = s->image.image_layout;
   vkf.vkCmdPipelineBarrier(s->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL,
         1, &barrier);
   vkf.vkEndCommandBuffer(s->cmd);

   memset(&submit, 0, sizeof(submit));
   submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   submit.commandBufferCount   = 1;
   submit.pCommandBuffers      = &s->cmd;
   submit.signalSemaphoreCount = 1;
   submit.pSignalSemaphores    = &s->semaphore;
   vk->lock_queue(vk->handle);
   res = vkf.vkQueueSubmit(vk->queue, 1, &submit, s->fence);
   vk->unlock_queue(vk->handle);
   if (res != VK_SUCCESS)
      return 0;
   s->pending = 1;
   vk->set_image(vk->handle, &s->image, 1, &s->semaphore,
         VK_QUEUE_FAMILY_IGNORED);
   harness_vk_frames++;
   return 1;
}

void retro_set_environment(retro_environment_t cb)
{
   bool no_content = true;
   environ_cb = cb;
   {
      /* The frames below are RGB565; say so, as a real core must. */
      enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
      cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
   }
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
void retro_set_input_poll(retro_input_poll_t cb) { (void)cb; }
void retro_set_input_state(retro_input_state_t cb) { (void)cb; }
void retro_init(void) { }
void retro_deinit(void) { }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name    = "threaded_video_harness";
   info->library_version = "1";
   info->valid_extensions = "";
}
void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->timing.fps         = 60.0;
   info->timing.sample_rate = 48000.0;
   info->geometry.base_width   = W;
   info->geometry.base_height  = H;
   info->geometry.max_width    = W;
   info->geometry.max_height   = H;
   info->geometry.aspect_ratio = 4.0f / 3.0f;
}
void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
void retro_reset(void) { }
/* Set by the harness through the core's own export below: when on,
 * retro_run asks the frontend for a framebuffer and renders into it,
 * exercising the wrapper's zero-copy lend. Counts how often the ask
 * was granted so the harness can check the lend actually happened.
 * Mode 2 renders the whole loan but pushes a cropped window into it -
 * a pointer past the start with the loan's pitch, the way a core that
 * crops overscan by offset does - which must still be a lend. */
#define CROP_X 8
#define CROP_Y 4
static int      harness_use_fb;
static unsigned harness_fb_granted;

/* RETRO_API, like the core's own entry points: a Windows DLL exports
 * only what is marked, and the harness looks these two up by name. */
RETRO_API void harness_core_use_framebuffer(int on) { harness_use_fb = on; }
RETRO_API unsigned harness_core_fb_granted(void)   { return harness_fb_granted; }

void retro_run(void)
{
   unsigned h = (runs % 61 == 60) ? H_OVERSIZE : H;
   unsigned i;
   uint16_t *dst = frame;
   size_t   pitch = W * 2;
   runs++;

   unsigned out_w = W, out_h = h;
   const uint16_t *push = NULL;

   if (harness_vk)
   {
      /* Every third frame a dupe, which draws the last image again, or
       * with mode 2 a software frame. */
      if (runs % 3 == 0 && harness_vk == 2)
         video_cb(frame, W, H, W * 2);
      else if (vk_ready && runs % 3 != 0 && vk_send())
         video_cb(RETRO_HW_FRAME_BUFFER_VALID, W, H, 0);
      else
         video_cb(NULL, W, H, 0);
      return;
   }

   if (harness_use_fb && h == H)
   {
      struct retro_framebuffer fb;
      memset(&fb, 0, sizeof(fb));
      fb.width        = W;
      fb.height       = H;
      /* Read as well as write, as a core that snapshots its frame
       * for a wipe asks: the lend must not be refused for it. */
      fb.access_flags = RETRO_MEMORY_ACCESS_WRITE
                      | RETRO_MEMORY_ACCESS_READ;
      if (     environ_cb(RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER, &fb)
            && fb.format == RETRO_PIXEL_FORMAT_RGB565)
      {
         dst   = (uint16_t*)fb.data;
         pitch = fb.pitch;
         harness_fb_granted++;
      }
   }

   for (i = 0; i < W * h; i++)
      dst[i] = (uint16_t)(runs + i);
   push = dst;
   if (harness_use_fb == 2 && h == H)
   {
      /* Cropped window: CROP_Y rows down and CROP_X pixels in, at the
       * full pitch. Same for a loan and for the core's own buffer. */
      push  = dst + CROP_Y * (pitch / 2) + CROP_X;
      out_w = W - 2 * CROP_X;
      out_h = H - 2 * CROP_Y;
   }
   if (runs % 3 == 0)
      video_cb(NULL, out_w, out_h, pitch);
   else
      video_cb(push, out_w, out_h, pitch);
}
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
void retro_cheat_reset(void) { }
void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }
bool retro_load_game(const struct retro_game_info *game)
{
   (void)game;
   if (!harness_vk)
      return true;
   memset(&hw_render, 0, sizeof(hw_render));
   hw_render.context_type    = RETRO_HW_CONTEXT_VULKAN;
   hw_render.version_major   = VK_API_VERSION_1_0;
   hw_render.context_reset   = vk_context_reset;
   hw_render.context_destroy = vk_context_destroy;
   return environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render);
}
bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) { (void)type; (void)info; (void)num; return false; }
void retro_unload_game(void) { }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
