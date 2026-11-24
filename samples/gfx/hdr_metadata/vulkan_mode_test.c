#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <boolean.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

/* Distinct across groups, so a flag tested in the wrong word shows. */
#define VK_CTX_FLAG_INVALID_SWAPCHAIN (1u << 0)
#define VK_CTX_FLAG_HDR_ENABLE        (1u << 1)
#define VK_CTX_FLAG_HDR_SUPPORT       (1u << 2)
#define VK_CTX_FLAG_HDR_SCRGB         (1u << 3)
#define VK_FLAG_SHOULD_RESIZE         (1u << 4)
#define VIDEO_FLAG_HDR_SUPPORT        (1u << 5)
#define VIDEO_FLAG_HDR10_SUPPORT      (1u << 6)
#define VIDEO_FLAG_SCRGB_SUPPORT      (1u << 7)
#define RARCH_ERR(msg)                ((void)0)

typedef struct vulkan_filter_chain vulkan_filter_chain_t;
enum glslang_filter_chain_filter
{
   GLSLANG_FILTER_CHAIN_LINEAR = 0,
   GLSLANG_FILTER_CHAIN_NEAREST
};
struct vulkan_filter_chain_create_info
{
   bool hdr_enabled;
};

typedef struct
{
   unsigned hdr_mode;
   unsigned swapchain_bit_depth;
   unsigned dims;
} video_frame_info_t;

typedef struct
{
   uint32_t flags;
   VkFormat swapchain_format;
} test_context_t;

typedef struct
{
   bool (*set_resize)(void *data, unsigned dims);
} test_ctx_driver_t;

typedef struct
{
   test_context_t *context;
   const test_ctx_driver_t *ctx_driver;
   void *ctx_data;
   vulkan_filter_chain_t *filter_chain_default;
   struct
   {
      bool smooth;
   } video;
   struct
   {
      struct
      {
         float    paper_white_nits;
         unsigned subpixel_layout;
         float    scanlines;
         unsigned expand_gamut;
         float    inverse_tonemap;
         float    hdr10;
         unsigned hdr_mode;
      } ubo_values;
   } hdr;
   uint32_t flags;
   unsigned hdr_mode_latched;
   unsigned hdr_mode_asked;
   float    hdr_scanlines_latched;
   unsigned hdr_subpixel_latched;
} vk_t;

static vk_t *active;
static const video_frame_info_t *active_info;
static int surf_hdr10, surf_scrgb, pick_on;
static uint32_t disp_flags;
static unsigned resizes, buffers_inits, publishes;
static bool invalid_seen;
static float set_paper_white, set_expand_gamut, set_scanlines;
static float set_subpixel, set_inverse_tonemap, set_hdr10;
static int dummy_chain;

uint32_t video_driver_get_disp_flags(void) { return disp_flags; }

static void vulkan_hdr_buffers_init(vk_t *vk, unsigned dims)
{ (void)vk; (void)dims; buffers_inits++; }
static void vulkan_views_publish(vk_t *vk) { (void)vk; publishes++; }

static void vulkan_filter_chain_info_init(vk_t *vk,
      struct vulkan_filter_chain_create_info *info)
{ (void)vk; memset(info, 0, sizeof(*info)); }
static vulkan_filter_chain_t *vulkan_filter_chain_create_default(
      const struct vulkan_filter_chain_create_info *info,
      enum glslang_filter_chain_filter filter)
{ (void)info; (void)filter; return (vulkan_filter_chain_t*)&dummy_chain; }
static bool vulkan_chain_emits_hdr10(vk_t *vk, vulkan_filter_chain_t *chain)
{ (void)vk; (void)chain; return false; }
static bool vulkan_chain_emits_hdr16(vk_t *vk, vulkan_filter_chain_t *chain)
{ (void)vk; (void)chain; return false; }

static void vulkan_filter_chain_set_paper_white_nits(
      vulkan_filter_chain_t *chain, float v)
{ (void)chain; set_paper_white = v; }
static void vulkan_filter_chain_set_expand_gamut(
      vulkan_filter_chain_t *chain, unsigned v)
{ (void)chain; set_expand_gamut = (float)v; }
static void vulkan_filter_chain_set_scanlines(
      vulkan_filter_chain_t *chain, float v)
{ (void)chain; set_scanlines = v; }
static void vulkan_filter_chain_set_subpixel_layout(
      vulkan_filter_chain_t *chain, unsigned v)
{ (void)chain; set_subpixel = (float)v; }
static void vulkan_filter_chain_set_inverse_tonemap(
      vulkan_filter_chain_t *chain, float v)
{ (void)chain; set_inverse_tonemap = v; }
static void vulkan_filter_chain_set_hdr10(
      vulkan_filter_chain_t *chain, float v)
{ (void)chain; set_hdr10 = v; }

/* The context's set_resize. Only an invalidated swapchain is made
 * again, and with pick_on the format pick runs on the test's surface. */
static bool set_resize(void *data, unsigned dims)
{
   test_context_t *ctx = active->context;
   unsigned asked      = active_info->hdr_mode;
   unsigned mode       = 0;

   (void)data;
   (void)dims;
   resizes++;
   if (!(ctx->flags & VK_CTX_FLAG_INVALID_SWAPCHAIN))
      return true;
   invalid_seen = true;
   if (!pick_on)
      return true;

   if (asked == 1)
      mode = surf_hdr10 ? 1 : (surf_scrgb ? 2 : 0);
   else if (asked == 2)
      mode = surf_scrgb ? 2 : (surf_hdr10 ? 1 : 0);

   ctx->flags &= ~(VK_CTX_FLAG_HDR_ENABLE | VK_CTX_FLAG_HDR_SCRGB);
   if (mode > 0)
      ctx->flags |= VK_CTX_FLAG_HDR_ENABLE;
   if (mode == 2)
      ctx->flags |= VK_CTX_FLAG_HDR_SCRGB;
   ctx->swapchain_format = mode == 2 ? VK_FORMAT_R16G16B16A16_SFLOAT
      : (mode == 1 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32
      : VK_FORMAT_B8G8R8A8_UNORM);
   return true;
}

#include "vulkan_mode.inc"

static const test_ctx_driver_t ctx_driver = { set_resize };
static test_context_t ctx;
static vk_t vk;
static video_frame_info_t info;
static const char *test_name;
static int failures;

#define CHECK(x) do { if (!(x)) { \
   printf("[FAIL] %s: %s (line %d)\n", test_name, #x, __LINE__); \
   failures++; return; } } while (0)

static void begin(const char *name)
{
   test_name = name;
   memset(&ctx, 0, sizeof(ctx));
   memset(&vk, 0, sizeof(vk));
   memset(&info, 0, sizeof(info));
   ctx.swapchain_format                = VK_FORMAT_B8G8R8A8_UNORM;
   vk.context                          = &ctx;
   vk.ctx_driver                       = &ctx_driver;
   vk.hdr.ubo_values.paper_white_nits  = 200.0f;
   vk.hdr.ubo_values.inverse_tonemap   = 1.0f;
   vk.hdr.ubo_values.hdr10             = 1.0f;
   info.dims                           = (480u << 16) | 640u;
   active                              = &vk;
   active_info                         = &info;
   surf_hdr10 = surf_scrgb = pick_on   = 0;
   disp_flags                          = 0;
   resizes = buffers_inits = publishes = 0;
   invalid_seen                        = false;
   set_paper_white = set_expand_gamut = set_scanlines = -1.0f;
   set_subpixel = set_inverse_tonemap = set_hdr10     = -1.0f;
}

static void ok(void)
{
   printf("[ok]   %s\n", test_name);
}

/* As vulkan_check_swapchain() does after each. */
static void apply(unsigned calls)
{
   while (calls--)
   {
      vulkan_apply_pending_resize(&vk, &info);
      ctx.flags &= ~VK_CTX_FLAG_INVALID_SWAPCHAIN;
   }
}

/* HDR10 asked, the pick made an SDR swapchain: a resize keeps it SDR. */
static void test_resize_never_enables_hdr(void)
{
   begin("test_resize_never_enables_hdr");
   ctx.flags         = VK_CTX_FLAG_HDR_SUPPORT;
   disp_flags        = VIDEO_FLAG_HDR_SUPPORT | VIDEO_FLAG_SCRGB_SUPPORT;
   info.hdr_mode     = 1;
   vk.hdr_mode_asked = 1;
   vk.flags          = VK_FLAG_SHOULD_RESIZE;
   pick_on           = 1;
   apply(2);
   CHECK(!(ctx.flags & VK_CTX_FLAG_HDR_ENABLE));
   CHECK(buffers_inits == 0);
   CHECK(resizes == 1);
   ok();
}

/* A new mode asked makes the swapchain again, and the pick turns
 * HDR off. */
static void test_mode_change_rebuilds(void)
{
   begin("test_mode_change_rebuilds");
   ctx.flags                = VK_CTX_FLAG_HDR_SUPPORT | VK_CTX_FLAG_HDR_ENABLE;
   ctx.swapchain_format     = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
   disp_flags               = VIDEO_FLAG_HDR_SUPPORT
      | VIDEO_FLAG_HDR10_SUPPORT | VIDEO_FLAG_SCRGB_SUPPORT;
   info.swapchain_bit_depth = 2;
   surf_hdr10 = surf_scrgb  = 1;
   info.hdr_mode            = 0;
   vk.hdr_mode_asked        = 1;
   pick_on                  = 1;
   apply(2);
   CHECK(resizes == 1);
   CHECK(invalid_seen);
   CHECK(!(ctx.flags & VK_CTX_FLAG_HDR_ENABLE));
   CHECK(vk.hdr_mode_asked == 0);
   ok();
}

/* A context whose set_resize makes no swapchain is asked once. */
static void test_rebuild_asked_once(void)
{
   begin("test_rebuild_asked_once");
   info.hdr_mode     = 1;
   vk.hdr_mode_asked = 0;
   apply(4);
   CHECK(resizes == 1);
   CHECK(invalid_seen);
   CHECK(!(ctx.flags & VK_CTX_FLAG_HDR_ENABLE));
   CHECK(buffers_inits == 0);
   ok();
}

/* HDR10 asked, the pick fell back to scRGB: the stock chain encodes
 * scRGB. */
static void test_stock_chain_takes_scrgb(void)
{
   bool built;
   begin("test_stock_chain_takes_scrgb");
   ctx.flags           = VK_CTX_FLAG_HDR_ENABLE | VK_CTX_FLAG_HDR_SCRGB;
   vk.hdr_mode_latched = 1;
   built               = vulkan_init_default_filter_chain(&vk);
   CHECK(built);
   CHECK(ctx.flags & VK_CTX_FLAG_HDR_SCRGB);
   CHECK(set_hdr10 == 0.0f);
   CHECK(set_inverse_tonemap == 0.0f);
   CHECK(vk.hdr.ubo_values.hdr10 == 0.0f);
   CHECK(vk.flags & VK_FLAG_SHOULD_RESIZE);
   ok();
}

/* scRGB asked, the pick fell back to HDR10: the stock chain encodes
 * PQ. */
static void test_stock_chain_takes_hdr10(void)
{
   begin("test_stock_chain_takes_hdr10");
   ctx.flags           = VK_CTX_FLAG_HDR_ENABLE;
   vk.hdr_mode_latched = 2;
   vulkan_init_default_filter_chain(&vk);
   CHECK(!(ctx.flags & VK_CTX_FLAG_HDR_SCRGB));
   CHECK(set_hdr10 == 1.0f);
   CHECK(set_inverse_tonemap == 1.0f);
   ok();
}

/* HDR asked, the pick made an SDR swapchain: no HDR encode at all. */
static void test_stock_chain_sdr(void)
{
   bool built;
   begin("test_stock_chain_sdr");
   vk.hdr_mode_latched = 1;
   built               = vulkan_init_default_filter_chain(&vk);
   CHECK(built);
   CHECK(set_paper_white == -1.0f && set_expand_gamut == -1.0f
         && set_scanlines == -1.0f && set_subpixel == -1.0f
         && set_inverse_tonemap == -1.0f && set_hdr10 == -1.0f);
   CHECK(!(ctx.flags & VK_CTX_FLAG_HDR_SCRGB));
   ok();
}

int main(void)
{
   test_resize_never_enables_hdr();
   test_mode_change_rebuilds();
   test_rebuild_asked_once();
   test_stock_chain_takes_scrgb();
   test_stock_chain_takes_hdr10();
   test_stock_chain_sdr();
   return failures ? 1 : 0;
}
