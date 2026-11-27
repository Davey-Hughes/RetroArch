/* Runs the production stock view draw (vulkan_views_draw_direct and
 * the vulkan_views_draw_stock it calls), the blank inset fill, the
 * pick of a second output's pipelines (vulkan_views_target_use) and a
 * screen window's draw with them (vulkan_window_draw_screens), the
 * HDR pass encode the draw borrows (vulkan_hdr_ubo_mode), the
 * decisions that send stock views straight into an HDR swapchain, with
 * no FP16 image and HDR game pass between, unless a placement is
 * translucent (vulkan_views_translucent), and a window's FP16 image
 * kept for that (vulkan_window_fp16_ensure), and an image screen's draw
 * (vulkan_views_draw_image) encoded where the target holds encoded
 * values, against a recording fake. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <boolean.h>

#define VULKAN_HDR_SWAPCHAIN 1
#define VKALIGN(x)

typedef uintptr_t VkPipeline;
typedef uintptr_t VkSampler;
typedef uintptr_t VkBuffer;
typedef uintptr_t VkCommandBuffer;
typedef uintptr_t VkImageView;
typedef uintptr_t VkRenderPass;
typedef uint64_t VkDeviceSize;
typedef int VkFormat;
typedef int VkColorSpaceKHR;
#define VK_NULL_HANDLE 0
#define VK_INDEX_TYPE_UINT16 0
#define VK_IMAGE_ASPECT_COLOR_BIT 1
#define VK_FORMAT_UNDEFINED 0
#define VK_FORMAT_R8G8B8A8_UNORM 37
#define VK_FORMAT_A2B10G10R10_UNORM_PACK32 64
#define VK_FORMAT_R16G16B16A16_SFLOAT 97
#define VK_FORMAT_B8G8R8A8_UNORM 44
#define VK_COLOR_SPACE_SRGB_NONLINEAR_KHR 0
#define VK_COLOR_SPACE_HDR10_ST2084_EXT 1000104008
#define VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT 1000104002
#define VK_CTX_FLAG_HDR_ENABLE (1u << 3)
#define VK_CTX_FLAG_HDR_SCRGB (1u << 4)
#define VK_FLAG_SDR_PIPELINE (1u << 5)
#define VK_FLAG_TRACKER_USE_SCISSOR (1u << 6)
#define VULKAN_DIRTY_DYNAMIC_BIT 1u
#define RGBA16_WHITE 0xffffffffffffffffull
#define VIDEO_SCALE_W(d) ((d) & 0xffffu)
#define VIDEO_SCALE_H(d) ((d) >> 16)
#define VIDEO_SCALE_PACK(w, h) ((unsigned)(w) | ((unsigned)(h) << 16))
#define VIDEO_POS_X(p) ((int)(int16_t)(((unsigned)(p) >> 16) & 0xffffu))
#define VIDEO_POS_Y(p) ((int)(int16_t)( (unsigned)(p)        & 0xffffu))

typedef struct { float data[16]; } math_matrix_4x4;
typedef struct { float x, y, width, height, minDepth, maxDepth; } VkViewport;
typedef struct { struct { int x, y; } offset;
   struct { unsigned width, height; } extent; } VkRect2D;
typedef union { float float32[4]; } VkClearColorValue;
typedef union { VkClearColorValue color; } VkClearValue;
typedef struct { unsigned aspectMask, colorAttachment;
   VkClearValue clearValue; } VkClearAttachment;
typedef struct { VkRect2D rect; unsigned baseArrayLayer, layerCount; }
   VkClearRect;
struct vk_texture { unsigned dims; };
struct retro_video_view { unsigned x, y, width, height, screen, eye;
   float aspect_ratio; };
typedef struct { unsigned pos, dims; } video_views_rect_t;
typedef struct { struct { struct retro_video_view views[1]; } views; }
   video_frame_info_t;
struct vk_vertex { float x, y; float tex_x, tex_y; uint64_t color; };
struct vk_buffer_range { uint8_t *data; VkBuffer buffer;
   VkDeviceSize offset; };
struct vk_draw_triangles
{
   const void *uniform;
   const struct vk_buffer_range *vbo;
   struct vk_texture *texture;
   VkPipeline pipeline;
   VkSampler sampler;
   size_t uniform_size;
   unsigned vertices;
};
struct vk_draw_quad
{
   struct vk_texture *texture;
   const math_matrix_4x4 *mvp;
   VkPipeline pipeline;
   VkSampler sampler;
   uint64_t color;
};
typedef struct { VkRenderPass render_pass; VkFormat format;
   VkColorSpaceKHR color_space; unsigned dims; } vulkan_output_t;

#define VULKAN_WRITE_QUAD_VBO(pv, _x, _y, _w, _h, _tx, _ty, _tw, _th, c) \
{ \
   pv[0].x = (_x); pv[0].y = (_y); pv[0].tex_x = (_tx); \
   pv[0].tex_y = (_ty); pv[0].color = (c); \
   pv[3].x = (_x) + (_w); pv[3].y = (_y) + (_h); \
   pv[3].tex_x = (_tx) + (_tw); pv[3].tex_y = (_ty) + (_th); \
   pv[3].color = (c); \
}

struct vk_context { unsigned flags; VkFormat swapchain_format; };
struct vk_chain { int vbo; };

#include "production_types.h"

struct vk_target_pass
{
   VkRenderPass pass;
   VkPipeline premult;
   VkPipeline stock_rect;
   VkPipeline stock_rect_blend;
   VkFormat format;
};
#define VK_VIEWS_TARGETS 4

typedef struct vk
{
   math_matrix_4x4 mvp, mvp_no_rot;
   VkViewport vk_vp;
   struct { VkRect2D scissor; unsigned dirty; VkPipeline pipeline;
      VkImageView view; } tracker;
   struct { VkPipeline stock_rect, stock_rect_blend, stock_rect_sdr,
      stock_rect_sdr_blend, stock_rect_hdr, stock_rect_hdr_sdr, alpha_blend,
      alpha_blend_sdr, alpha_premult; } pipelines;
   struct { VkSampler linear, nearest; } samplers;
   struct { unsigned hdr_stock, encoded; VkPipeline stock_pipeline,
      blend_pipeline, fill_pipeline; bool target; unsigned alpha; bool fp16;
      struct vk_target_pass targets[VK_VIEWS_TARGETS]; } views;
   struct { struct vk_texture blank_texture; } display;
   struct { vulkan_hdr_uniform_t ubo_values; } hdr;
   struct { bool smooth; } video;
   struct { VkBuffer buffer; unsigned num_quads; } quad_ibo;
   struct vk_context context_s;
   struct vk_context *context;
   struct vk_chain chain_s;
   struct vk_chain *chain;
   VkCommandBuffer cmd;
   unsigned flags;
} vk_t;

struct vulkan_views_draw_ctx
{
   const video_frame_info_t *video_info;
   struct vk_texture *direct;
};

/* libretro.h's */
#define RETRO_VIDEO_VIEWS_MAX 8

/* gfx/video_views.h's */
#define VIDEO_VIEWS_MAX_PLACEMENTS (RETRO_VIDEO_VIEWS_MAX * 2)

#define VIDEO_VIEWS_PLACEMENT_BLANK (1u << 0)   /* drawn black */
#define VIDEO_VIEWS_PLACEMENT_INSET (1u << 1)   /* drawn over its base's */
#define VIDEO_VIEWS_PLACEMENT_IMAGE (1u << 2)   /* view holds the screen */
/* Bits 8-15 hold 255 minus the opacity, so flags 0 is solid. */
#define VIDEO_VIEWS_PLACEMENT_ALPHA_BITS(a) ((255u - (a)) << 8)
#define VIDEO_VIEWS_PLACEMENT_ALPHA(flags)  (255u - (((flags) >> 8) & 0xffu))

/* One view drawn into one eye's area. */
typedef struct video_views_placement
{
   video_views_rect_t dst;   /* canvas pixels, top-left origin */
   unsigned view;            /* the map's view, or an IMAGE's screen */
   unsigned area;            /* 0: left eye or the only area, 1: right eye */
   unsigned flags;           /* VIDEO_VIEWS_PLACEMENT_* */
} video_views_placement_t;

typedef struct video_views_layout
{
   video_views_placement_t placements[VIDEO_VIEWS_MAX_PLACEMENTS];
   video_views_rect_t areas[2];
   unsigned num_placements;
   unsigned num_areas;
   /* What the placements are drawn into: the window, or for anaglyph and
    * interlaced an offscreen image holding both eyes side by side, which
    * a final pass blends into the window. */
   unsigned canvas_dims;
   /* The size the menu and widgets lay out at. */
   unsigned ui_dims;
   /* The params' stereo mode, so drivers pick the blend without reading
    * settings. */
   unsigned stereo_mode;
   bool offscreen;
   /* The UI is drawn once and placed in each eye's area. */
   bool ui_per_eye;
} video_views_layout_t;

/* A render target the views draw into: the size it was made at, kept
 * when making it failed so it isn't retried every frame. */
typedef struct video_views_target
{
   unsigned dims;
   bool made;
} video_views_target_t;

typedef struct vulkan_filter_chain vulkan_filter_chain_t;
struct vk_image { VkImageView view; };
struct vk_window
{
   vulkan_output_t output;
   video_views_layout_t *layout;
   vulkan_filter_chain_t *chains[RETRO_VIDEO_VIEWS_MAX];
   struct vk_image hdr;
   video_views_target_t hdr_target;
};

/* The draw the fake took, the blend constants set after how many
 * binds, and the quads and clears a fill took */
static struct
{
   VkPipeline pipeline;
   VkSampler sampler;
   size_t uniform_size;
   unsigned char uniform[512];
   unsigned draws;
   unsigned binds;
   float blend[4];
   unsigned blend_sets, blend_binds;
   VkPipeline quad_pipeline;
   uint64_t quad_color;
   unsigned quads;
   VkRect2D clear_rect;
   unsigned clears;
} last;
static struct vk_vertex vbo_mem[4];
static unsigned failures;

/* What a window's screens drew: its placements, with the pass's
 * pipelines, direct and the chains' final target at the call, and its
 * FP16 image's quad */
static struct
{
   unsigned placements;
   bool target, direct;
   VkPipeline stock, blend, fill;
   VkRenderPass chains_pass, chains_target;
   unsigned quads;
   const struct vk_image *quad_img;
   VkFormat quad_format;
   VkPipeline quad_pipeline;
} drew;

static void vulkan_vv_draw_placements(vk_t *vk,
      const struct vulkan_views_draw_ctx *d,
      const video_views_layout_t *layout,
      vulkan_filter_chain_t *const *chains, bool all_areas, unsigned dims,
      bool direct)
{
   (void)d; (void)layout; (void)chains; (void)all_areas; (void)dims;
   drew.placements++;
   drew.target      = vk->views.target;
   drew.stock       = vk->views.stock_pipeline;
   drew.blend       = vk->views.blend_pipeline;
   drew.fill        = vk->views.fill_pipeline;
   drew.direct      = direct;
   drew.chains_pass = drew.chains_target;
}
static void vulkan_window_chains_target(vulkan_filter_chain_t **chains,
      VkRenderPass render_pass)
{
   (void)chains;
   drew.chains_target = render_pass;
}
static void vulkan_window_quad(vk_t *vk, const struct vk_image *img,
      unsigned img_dims, VkFormat format, unsigned dims, VkPipeline pipeline)
{
   (void)vk; (void)img_dims; (void)dims;
   drew.quads++;
   drew.quad_img      = img;
   drew.quad_format   = format;
   drew.quad_pipeline = pipeline;
}

/* A window FP16 image's makes and drops; a make gives fp16.made */
static struct { unsigned ensures, drops; bool made; } fp16;
static bool vulkan_vv_target_ensure(vk_t *vk, video_views_target_t *t,
      struct vk_image *target, unsigned dims)
{
   (void)vk; (void)target;
   fp16.ensures++;
   t->dims = dims;
   t->made = fp16.made;
   return t->made;
}
static void vulkan_vv_target_drop(vk_t *vk, video_views_target_t *t,
      struct vk_image *target)
{
   (void)vk; (void)target;
   fp16.drops++;
   t->dims = 0;
   t->made = false;
}

static bool vulkan_draw_bind(vk_t *vk, const struct vk_draw_triangles *c)
{
   (void)vk;
   last.binds++;
   last.pipeline     = c->pipeline;
   last.sampler      = c->sampler;
   last.uniform_size = c->uniform_size;
   memcpy(last.uniform, c->uniform, c->uniform_size);
   return true;
}
static bool vulkan_buffer_chain_alloc(const struct vk_context *ctx,
      int *chain, size_t size, struct vk_buffer_range *range)
{
   (void)ctx; (void)chain;
   if (size > sizeof(vbo_mem))
      return false;
   range->data   = (uint8_t*)vbo_mem;
   range->buffer = 1;
   range->offset = 0;
   return true;
}
static void vkCmdBindVertexBuffers(VkCommandBuffer c, unsigned f, unsigned n,
      const VkBuffer *b, const VkDeviceSize *o)
{ (void)c; (void)f; (void)n; (void)b; (void)o; }
static void vkCmdBindIndexBuffer(VkCommandBuffer c, VkBuffer b,
      VkDeviceSize o, int t)
{ (void)c; (void)b; (void)o; (void)t; }
static void vkCmdDrawIndexed(VkCommandBuffer c, unsigned n, unsigned i,
      unsigned f, int v, unsigned fi)
{ (void)c; (void)n; (void)i; (void)f; (void)v; (void)fi; last.draws++; }
static void vkCmdDraw(VkCommandBuffer c, unsigned n, unsigned i,
      unsigned f, unsigned fi)
{ (void)c; (void)n; (void)i; (void)f; (void)fi; last.draws++; }
static void vkCmdSetBlendConstants(VkCommandBuffer c, const float k[4])
{
   (void)c;
   memcpy(last.blend, k, sizeof(last.blend));
   last.blend_sets++;
   last.blend_binds = last.binds;
}
static void vulkan_draw_quad(vk_t *vk, const struct vk_draw_quad *quad)
{
   (void)vk;
   last.quad_pipeline = quad->pipeline;
   last.quad_color    = quad->color;
   last.quads++;
}
static void vkCmdClearAttachments(VkCommandBuffer c, unsigned n,
      const VkClearAttachment *a, unsigned rects, const VkClearRect *r)
{
   (void)c; (void)n; (void)a; (void)rects;
   last.clear_rect = r->rect;
   last.clears++;
}
/* What the view chains' preset writes */
static struct { bool hdr10, hdr16; } emits;
struct video_shader;
static bool vulkan_chain_emits_hdr10(vk_t *vk, vulkan_filter_chain_t *chain)
{
   (void)vk; (void)chain;
   return emits.hdr10;
}
static bool vulkan_chain_emits_hdr16(vk_t *vk, vulkan_filter_chain_t *chain)
{
   (void)vk; (void)chain;
   return emits.hdr16;
}

/* rgba16_pack.h's scalar path, little-endian */
static uint64_t rgba16_pack(const float *rgba)
{
   uint64_t r = 0;
   unsigned i;
   for (i = 4; i-- > 0; )
      r = (r << 16) | (uint64_t)(rgba[i] * 65535.0f + 0.5f);
   return r;
}

#include "production_stock_hdr.h"

static void expect(const char *t, int ok, const char *what, double got)
{
   if (ok)
      return;
   printf("[FAIL] %s: %s (got %g)\n", t, what, got);
   failures++;
}

static vk_t vk;
static struct vk_texture frame;
static struct retro_video_view view;

static void setup(void)
{
   unsigned i;
   memset(&vk, 0, sizeof(vk));
   memset(&last, 0, sizeof(last));
   memset(&emits, 0, sizeof(emits));
   vk.context                         = &vk.context_s;
   vk.chain                           = &vk.chain_s;
   vk.context->flags                  = VK_CTX_FLAG_HDR_ENABLE;
   vk.context->swapchain_format       = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
   vk.pipelines.stock_rect            = 11;
   vk.pipelines.stock_rect_blend      = 14;
   vk.pipelines.stock_rect_sdr        = 12;
   vk.pipelines.stock_rect_hdr        = 13;
   vk.pipelines.stock_rect_sdr_blend  = 15;
   vk.pipelines.alpha_blend           = 16;
   vk.pipelines.alpha_blend_sdr       = 17;
   vk.pipelines.alpha_premult         = 18;
   vk.pipelines.stock_rect_hdr_sdr    = 19;
   vk.samplers.linear                 = 21;
   vk.samplers.nearest                = 22;
   vk.video.smooth                    = true;
   vk.quad_ibo.buffer                 = 1;
   vk.quad_ibo.num_quads              = 1;
   vk.views.fp16                      = true;
   for (i = 0; i < 16; i++)
   {
      vk.mvp.data[i]                  = (float)(i + 1);
      vk.mvp_no_rot.data[i]           = (float)(i + 101);
   }
   vk.hdr.ubo_values.paper_white_nits = 203.0f;
   vk.hdr.ubo_values.expand_gamut     = 2;
   vk.hdr.ubo_values.subpixel_layout  = 1;
   vk.hdr.ubo_values.inverse_tonemap  = 0.5f;
   vk.hdr.ubo_values.hdr10            = 0.5f;
   vk.hdr.ubo_values.hdr_mode         = 7;
   frame.dims  = VIDEO_SCALE_PACK(400, 480);
   view.x      = 0;
   view.y      = 240;
   view.width  = 320;
   view.height = 240;
}

static void draw(void)
{
   video_frame_info_t info;
   struct vulkan_views_draw_ctx d;
   video_views_rect_t rect;
   memset(&rect, 0, sizeof(rect));
   info.views.views[0] = view;
   d.video_info        = &info;
   d.direct            = &frame;
   vulkan_views_draw_direct(&vk, &d, 0, &rect, &rect);
}

/* A blank inset at x 48, y 24, 16x8 */
static void fill(unsigned alpha)
{
   video_views_rect_t cut;
   cut.pos  = (48u << 16) | 24u;
   cut.dims = VIDEO_SCALE_PACK(16, 8);
   vulkan_views_fill(&vk, &cut, alpha);
}

/* A 48x32 picture over x 8, y 4, 48x32, nearest */
static void image(void)
{
   static struct vk_texture picture;
   video_views_rect_t dst;
   picture.dims = VIDEO_SCALE_PACK(48, 32);
   dst.pos      = (8u << 16) | 4u;
   dst.dims     = VIDEO_SCALE_PACK(48, 32);
   vulkan_views_draw_image(&vk, (uintptr_t)&picture, &dst, &dst, true);
}

/* The uniform shapes stock_rect_hdr.frag reads: SourceSize, where the
 * clamp goes, right after the MVP, as hdr_common.glsl lays its UBO out */
static void test_uniform_layout(void)
{
   static const char t[] = "test_uniform_layout";
   unsigned before = failures;
   expect(t, offsetof(vulkan_hdr_uniform_t, not_used1) == 64,
         "SourceSize at 64", (double)offsetof(vulkan_hdr_uniform_t, not_used1));
   expect(t, offsetof(vulkan_hdr_uniform_t, paper_white_nits) == 96,
         "BrightnessNits at 96",
         (double)offsetof(vulkan_hdr_uniform_t, paper_white_nits));
   expect(t, offsetof(vulkan_hdr_uniform_t, hdr_mode) == 120,
         "HDRMode at 120", (double)offsetof(vulkan_hdr_uniform_t, hdr_mode));
   if (failures == before)
      printf("[pass] %s\n", t);
}

/* Encoding: the HDR pipeline, the HDR pass's values with the mode's
 * encode, the view's mvp, and the clamp in the source size slot */
static void test_encoded_draw(void)
{
   static const char t[] = "test_encoded_draw";
   unsigned before = failures;
   unsigned mode;
   for (mode = 1; mode <= 2; mode++)
   {
      vulkan_hdr_uniform_t u;
      setup();
      vk.views.hdr_stock = mode + 1;
      draw();
      memcpy(&u, last.uniform, sizeof(u));
      expect(t, last.draws == 1, "one draw", last.draws);
      expect(t, last.pipeline == vk.pipelines.stock_rect_hdr,
            "the HDR pipeline", (double)last.pipeline);
      expect(t, last.sampler == vk.samplers.linear, "the frame's filter",
            (double)last.sampler);
      expect(t, last.uniform_size == sizeof(vulkan_hdr_uniform_t),
            "the HDR uniform", (double)last.uniform_size);
      expect(t, !memcmp(&u.mvp, &vk.mvp, sizeof(u.mvp)), "the views' mvp",
            u.mvp.data[0]);
      expect(t, u.not_used1.width == 0.5f / 400.0f
            && u.not_used1.height == (240.0f + 0.5f) / 480.0f
            && u.not_used1.pad0 == (320.0f - 0.5f) / 400.0f
            && u.not_used1.pad1 == (480.0f - 0.5f) / 480.0f,
            "the first and last texel centres", u.not_used1.width);
      expect(t, u.paper_white_nits == 203.0f && u.expand_gamut == 2
            && u.subpixel_layout == 1, "the HDR pass's values",
            u.paper_white_nits);
      expect(t, u.hdr_mode == mode, "the mode", u.hdr_mode);
      expect(t, (mode == 1)
            ? (u.inverse_tonemap == 1.0f && u.hdr10 == 1.0f)
            : (u.inverse_tonemap == 0.0f && u.hdr10 == 0.0f),
            "the mode's encode", u.inverse_tonemap);
      expect(t, vk.hdr.ubo_values.hdr_mode == 7
            && vk.hdr.ubo_values.inverse_tonemap == 0.5f,
            "the HDR pass's own values untouched",
            vk.hdr.ubo_values.hdr_mode);
   }
   if (failures == before)
      printf("[pass] %s\n", t);
}

/* Not encoding: the plain or FP16 pipeline, with the small uniform */
static void test_plain_draw(void)
{
   static const char t[] = "test_plain_draw";
   unsigned before = failures;
   setup();
   draw();
   expect(t, last.pipeline == vk.pipelines.stock_rect, "stock_rect",
         (double)last.pipeline);
   expect(t, last.uniform_size == sizeof(math_matrix_4x4) + 4 * sizeof(float),
         "mvp and clamp", (double)last.uniform_size);
   expect(t, !last.blend_sets, "no constants", last.blend_sets);
   setup();
   vk.flags |= VK_FLAG_SDR_PIPELINE;
   draw();
   expect(t, last.pipeline == vk.pipelines.stock_rect_sdr, "stock_rect_sdr",
         (double)last.pipeline);
   expect(t, !last.blend_sets, "no constants in FP16", last.blend_sets);
   if (failures == before)
      printf("[pass] %s\n", t);
}

/* A translucent inset blends at its alpha, the constants set after the
 * bind; with no blend pipeline it draws solid */
static void test_translucent_draw(void)
{
   static const char t[] = "test_translucent_draw";
   unsigned before = failures;
   setup();
   vk.views.alpha = 64;
   draw();
   expect(t, last.pipeline == vk.pipelines.stock_rect_blend,
         "stock_rect_blend", (double)last.pipeline);
   expect(t, last.blend_sets == 1 && last.blend_binds == 1,
         "the constants after the bind", last.blend_binds);
   expect(t, last.blend[0] == 0.0f && last.blend[1] == 0.0f
         && last.blend[2] == 0.0f && last.blend[3] == 64.0f / 255.0f,
         "the alpha", last.blend[3]);
   setup();
   vk.views.alpha                = 64;
   vk.pipelines.stock_rect_blend = VK_NULL_HANDLE;
   draw();
   expect(t, last.pipeline == vk.pipelines.stock_rect && !last.blend_sets,
         "solid without it", (double)last.pipeline);
   /* In FP16, with sdr_render_pass's twin */
   setup();
   vk.flags      |= VK_FLAG_SDR_PIPELINE;
   vk.views.alpha = 64;
   draw();
   expect(t, last.pipeline == vk.pipelines.stock_rect_sdr_blend,
         "stock_rect_sdr_blend", (double)last.pipeline);
   expect(t, last.binds == 1 && last.blend_sets == 1
         && last.blend_binds == 1, "the constants once after the one bind",
         last.blend_sets);
   setup();
   vk.flags                         |= VK_FLAG_SDR_PIPELINE;
   vk.views.alpha                    = 64;
   vk.pipelines.stock_rect_sdr_blend = VK_NULL_HANDLE;
   draw();
   expect(t, last.pipeline == vk.pipelines.stock_rect_sdr && !last.blend_sets,
         "solid in FP16 without it", (double)last.pipeline);
   /* In a second output's pass, with its own */
   setup();
   vk.views.target         = true;
   vk.views.stock_pipeline = 61;
   vk.views.blend_pipeline = 62;
   vk.views.alpha          = 64;
   draw();
   expect(t, last.pipeline == 62 && last.blend_sets == 1
         && last.blend_binds == 1 && last.blend[3] == 64.0f / 255.0f,
         "the output's twin, the constants after the bind",
         (double)last.pipeline);
   setup();
   vk.views.target         = true;
   vk.views.stock_pipeline = 61;
   vk.views.blend_pipeline = 62;
   draw();
   expect(t, last.pipeline == 61 && !last.blend_sets,
         "the output's stock_rect when solid", (double)last.pipeline);
   setup();
   vk.views.target         = true;
   vk.views.stock_pipeline = 61;
   vk.views.alpha          = 64;
   draw();
   expect(t, last.pipeline == 61 && !last.blend_sets,
         "the output's stock_rect without its twin", (double)last.pipeline);
   setup();
   vk.views.target = true;
   vk.views.alpha  = 64;
   draw();
   expect(t, !last.draws && !last.binds,
         "nothing without the output's pipelines", last.draws);
   /* Encoded, solid */
   setup();
   vk.views.hdr_stock = 2;
   vk.views.alpha     = 64;
   draw();
   expect(t, last.pipeline == vk.pipelines.stock_rect_hdr && !last.blend_sets,
         "stock_rect_hdr, solid", (double)last.pipeline);
   if (failures == before)
      printf("[pass] %s\n", t);
}

/* A second output in another format draws them with its own stock_rect */
static void test_target_pipeline_draw(void)
{
   static const char t[] = "test_target_pipeline_draw";
   unsigned before = failures;
   setup();
   vk.views.target         = true;
   vk.views.stock_pipeline = 31;
   draw();
   expect(t, last.pipeline == 31, "the output's stock_rect",
         (double)last.pipeline);
   expect(t, last.uniform_size == sizeof(math_matrix_4x4) + 4 * sizeof(float),
         "mvp and clamp", (double)last.uniform_size);
   if (failures == before)
      printf("[pass] %s\n", t);
}

/* Views in a second output's or the headset's pass in another format
 * draw with its pipelines, or none where it has no pass; in the
 * window's format, or with none given, with the window's own */
static void test_target_use(void)
{
   static const char t[] = "test_target_use";
   unsigned before = failures;
   static const VkFormat window[2] = {
      VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_UNDEFINED };
   unsigned i;
   setup();
   vk.views.targets[1].pass             = 41;
   vk.views.targets[1].stock_rect       = 42;
   vk.views.targets[1].premult          = 43;
   vk.views.targets[1].stock_rect_blend = 44;
   vk.views.targets[1].format           = VK_FORMAT_B8G8R8A8_UNORM;
   for (i = 0; i < 2; i++)
   {
      vk.views.target         = true;
      vk.views.stock_pipeline = 1;
      vk.views.blend_pipeline = 2;
      vk.views.fill_pipeline  = 3;
      vulkan_views_target_use(&vk, window[i]);
      expect(t, !vk.views.target && !vk.views.stock_pipeline
            && !vk.views.blend_pipeline && !vk.views.fill_pipeline,
            "the window's own", i);
   }
   vulkan_views_target_use(&vk, VK_FORMAT_B8G8R8A8_UNORM);
   expect(t, vk.views.target && vk.views.stock_pipeline == 42
         && vk.views.blend_pipeline == 44 && vk.views.fill_pipeline == 43,
         "the output's pass's", (double)vk.views.stock_pipeline);
   vulkan_views_target_use(&vk, VK_FORMAT_R8G8B8A8_UNORM);
   expect(t, vk.views.target && !vk.views.stock_pipeline
         && !vk.views.blend_pipeline && !vk.views.fill_pipeline,
         "none without a pass", (double)vk.views.stock_pipeline);
   /* As vulkan_views_targets_free leaves them */
   vulkan_views_target_use(&vk, VK_FORMAT_B8G8R8A8_UNORM);
   memset(vk.views.targets, 0, sizeof(vk.views.targets));
   vulkan_views_target_use(&vk, VK_FORMAT_B8G8R8A8_UNORM);
   expect(t, vk.views.target && !vk.views.stock_pipeline
         && !vk.views.blend_pipeline && !vk.views.fill_pipeline,
         "none once freed", (double)vk.views.stock_pipeline);
   if (failures == before)
      printf("[pass] %s\n", t);
}

/* A blank inset below 255 is black at its alpha through the pass's
 * alpha_blend, or the clear where the pass has none; at 255 the clear,
 * over the cut */
static void test_fill_pipeline(void)
{
   static const char t[] = "test_fill_pipeline";
   unsigned before = failures;
   setup();
   fill(128);
   expect(t, last.quads == 1 && last.quad_pipeline == vk.pipelines.alpha_blend,
         "alpha_blend", (double)last.quad_pipeline);
   expect(t, last.quad_color == ((uint64_t)(128 * 257) << 48),
         "black at its alpha", (double)(last.quad_color >> 48));
   expect(t, !last.clears, "no clear", last.clears);
   setup();
   vk.flags |= VK_FLAG_SDR_PIPELINE;
   fill(128);
   expect(t, last.quads == 1
         && last.quad_pipeline == vk.pipelines.alpha_blend_sdr,
         "alpha_blend_sdr in FP16", (double)last.quad_pipeline);
   setup();
   vk.views.target        = true;
   vk.views.fill_pipeline = 51;
   fill(128);
   expect(t, last.quads == 1 && last.quad_pipeline == 51,
         "the output's premult", (double)last.quad_pipeline);
   setup();
   vk.views.target = true;
   fill(128);
   expect(t, !last.quads && last.clears == 1,
         "the clear without the output's", last.quads);
   setup();
   fill(255);
   expect(t, !last.quads && last.clears == 1, "solid: the clear",
         last.quads);
   expect(t, last.clear_rect.offset.x == 48 && last.clear_rect.offset.y == 24
         && last.clear_rect.extent.width == 16
         && last.clear_rect.extent.height == 8, "over the cut",
         last.clear_rect.offset.x);
   if (failures == before)
      printf("[pass] %s\n", t);
}

/* What the main window's target holds: nothing encoded without HDR
 * output, or in FP16 that the HDR game pass encodes; the swapchain's
 * encoding where a views preset writes it straight in; PQ in FP16
 * under scRGB with a preset writing HDR10 */
static void test_encoded_mode(void)
{
   static const char t[] = "test_encoded_mode";
   unsigned before = failures;
   setup();
   vk.context->flags = 0;
   vk.views.fp16     = false;
   expect(t, vulkan_views_encoded(&vk, NULL) == 0, "no HDR",
         vulkan_views_encoded(&vk, NULL));
   setup();
   vk.views.fp16 = false;
   expect(t, vulkan_views_encoded(&vk, NULL) == 2, "HDR10 straight in",
         vulkan_views_encoded(&vk, NULL));
   vk.context->flags |= VK_CTX_FLAG_HDR_SCRGB;
   expect(t, vulkan_views_encoded(&vk, NULL) == 3, "scRGB straight in",
         vulkan_views_encoded(&vk, NULL));
   vk.views.fp16 = true;
   emits.hdr10   = true;
   expect(t, vulkan_views_encoded(&vk, NULL) == 2,
         "scRGB, PQ in FP16", vulkan_views_encoded(&vk, NULL));
   setup();
   expect(t, vulkan_views_encoded(&vk, NULL) == 0, "HDR10, an SDR preset",
         vulkan_views_encoded(&vk, NULL));
   vk.context->flags |= VK_CTX_FLAG_HDR_SCRGB;
   expect(t, vulkan_views_encoded(&vk, NULL) == 0, "scRGB, an SDR preset",
         vulkan_views_encoded(&vk, NULL));
   if (failures == before)
      printf("[pass] %s\n", t);
}

/* Where the target holds encoded values a picture is encoded as it
 * holds them, whole and never turned, with sdr_render_pass's pipeline
 * in FP16 and none without it; the stock views' direct encode wins */
static void test_image_encoded(void)
{
   static const char t[] = "test_image_encoded";
   unsigned before = failures;
   vulkan_hdr_uniform_t u;
   setup();
   vk.views.encoded = 2;
   image();
   memcpy(&u, last.uniform, sizeof(u));
   expect(t, last.draws == 1 && last.pipeline == vk.pipelines.stock_rect_hdr,
         "stock_rect_hdr", (double)last.pipeline);
   expect(t, last.uniform_size == sizeof(vulkan_hdr_uniform_t),
         "the HDR uniform", (double)last.uniform_size);
   expect(t, u.hdr_mode == 1 && u.inverse_tonemap == 1.0f && u.hdr10 == 1.0f,
         "mode 1, the HDR10 encode", u.hdr_mode);
   expect(t, !memcmp(&u.mvp, &vk.mvp_no_rot, sizeof(u.mvp)), "never turned",
         u.mvp.data[0]);
   expect(t, u.not_used1.width == 0.5f / 48.0f
         && u.not_used1.height == 0.5f / 32.0f
         && u.not_used1.pad0 == (48.0f - 0.5f) / 48.0f
         && u.not_used1.pad1 == (32.0f - 0.5f) / 32.0f, "the whole picture",
         u.not_used1.width);
   expect(t, last.sampler == vk.samplers.nearest, "nearest",
         (double)last.sampler);
   expect(t, !vk.views.hdr_stock, "hdr_stock restored", vk.views.hdr_stock);
   setup();
   vk.flags        |= VK_FLAG_SDR_PIPELINE;
   vk.views.encoded = 2;
   image();
   memcpy(&u, last.uniform, sizeof(u));
   expect(t, last.draws == 1
         && last.pipeline == vk.pipelines.stock_rect_hdr_sdr
         && u.hdr_mode == 1, "in FP16: stock_rect_hdr_sdr, mode 1",
         (double)last.pipeline);
   setup();
   vk.flags                       |= VK_FLAG_SDR_PIPELINE;
   vk.views.encoded                = 2;
   vk.pipelines.stock_rect_hdr_sdr = VK_NULL_HANDLE;
   image();
   expect(t, !last.binds && !last.draws, "in FP16 none without it",
         last.binds);
   setup();
   vk.views.encoded = 3;
   image();
   memcpy(&u, last.uniform, sizeof(u));
   expect(t, last.pipeline == vk.pipelines.stock_rect_hdr && u.hdr_mode == 2
         && u.inverse_tonemap == 0.0f && u.hdr10 == 0.0f, "scRGB: mode 2",
         u.hdr_mode);
   setup();
   image();
   expect(t, last.pipeline == vk.pipelines.stock_rect
         && last.uniform_size == sizeof(math_matrix_4x4) + 4 * sizeof(float),
         "not encoded: stock_rect, mvp and clamp", (double)last.pipeline);
   setup();
   vk.views.hdr_stock = 3;
   vk.views.encoded   = 2;
   image();
   memcpy(&u, last.uniform, sizeof(u));
   expect(t, last.pipeline == vk.pipelines.stock_rect_hdr && u.hdr_mode == 2
         && vk.views.hdr_stock == 3, "the stock direct encode wins",
         u.hdr_mode);
   if (failures == before)
      printf("[pass] %s\n", t);
}

/* Where the target holds encoded values a translucent picture or blank
 * inset draws solid: nothing blends encoded values */
static void test_encoded_solid(void)
{
   static const char t[] = "test_encoded_solid";
   unsigned before = failures;
   setup();
   vk.views.encoded = 2;
   vk.views.alpha   = 64;
   image();
   expect(t, last.pipeline == vk.pipelines.stock_rect_hdr && !last.blend_sets,
         "the picture solid", (double)last.pipeline);
   setup();
   vk.views.encoded = 2;
   fill(128);
   expect(t, !last.quads && last.clears == 1, "the inset cleared",
         last.quads);
   setup();
   vk.flags        |= VK_FLAG_SDR_PIPELINE;
   vk.views.encoded = 2;
   fill(128);
   expect(t, !last.quads && last.clears == 1, "in FP16 too", last.quads);
   if (failures == before)
      printf("[pass] %s\n", t);
}

/* Under HDR output an SDR window draws stock views straight in with the
 * stock_rect of its format's pass; never one in the main window's
 * encoding, nor without one, nor through chains, nor without HDR */
static void test_sdr_window_decision(void)
{
   static const char t[] = "test_sdr_window_decision";
   unsigned before = failures;
   vulkan_output_t out;
   setup();
   vk.views.targets[1].pass       = 41;
   vk.views.targets[1].format     = VK_FORMAT_B8G8R8A8_UNORM;
   vk.views.targets[1].stock_rect = 42;
   out.format      = VK_FORMAT_B8G8R8A8_UNORM;
   out.color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   expect(t, vulkan_window_stock_sdr(&vk, &out, true) == 42,
         "its format's stock_rect", 0);
   expect(t, vulkan_window_stock_sdr(&vk, &out, false) == VK_NULL_HANDLE,
         "not through chains", 1);
   /* Depth 30: the SDR window in the main window's format, sRGB */
   out.format      = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
   expect(t, vulkan_window_stock_sdr(&vk, &out, true)
         == vk.pipelines.stock_rect, "the window's own stock_rect", 2);
   out.color_space = VK_COLOR_SPACE_HDR10_ST2084_EXT;
   expect(t, vulkan_window_stock_sdr(&vk, &out, true) == VK_NULL_HANDLE,
         "not in the main window's encoding", 3);
   out.format      = VK_FORMAT_R16G16B16A16_SFLOAT;
   out.color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   expect(t, vulkan_window_stock_sdr(&vk, &out, true) == VK_NULL_HANDLE,
         "not without a pass for the format", 4);
   vk.views.targets[1].stock_rect = VK_NULL_HANDLE;
   out.format      = VK_FORMAT_B8G8R8A8_UNORM;
   expect(t, vulkan_window_stock_sdr(&vk, &out, true) == VK_NULL_HANDLE,
         "not without its stock_rect", 5);
   vk.views.targets[1].stock_rect = 42;
   vk.context->flags = 0;
   expect(t, vulkan_window_stock_sdr(&vk, &out, true) == VK_NULL_HANDLE,
         "not without HDR output", 6);
   if (failures == before)
      printf("[pass] %s\n", t);
}

/* Translucent: any placement, stock, image or blank, below full
 * opacity */
static void test_translucent_layout(void)
{
   static const char t[] = "test_translucent_layout";
   unsigned before = failures;
   video_views_layout_t layout;
   memset(&layout, 0, sizeof(layout));
   expect(t, !vulkan_views_translucent(&layout), "no placements", 0);
   layout.num_placements = 2;
   expect(t, !vulkan_views_translucent(&layout), "flags 0", 1);
   layout.placements[1].flags = VIDEO_VIEWS_PLACEMENT_BLANK
      | VIDEO_VIEWS_PLACEMENT_INSET;
   expect(t, !vulkan_views_translucent(&layout), "a solid blank inset", 2);
   layout.placements[1].flags = VIDEO_VIEWS_PLACEMENT_ALPHA_BITS(128);
   expect(t, vulkan_views_translucent(&layout), "a stock placement", 3);
   layout.placements[1].flags = VIDEO_VIEWS_PLACEMENT_BLANK
      | VIDEO_VIEWS_PLACEMENT_INSET | VIDEO_VIEWS_PLACEMENT_ALPHA_BITS(128);
   expect(t, vulkan_views_translucent(&layout), "a blank inset", 4);
   layout.placements[1].flags = VIDEO_VIEWS_PLACEMENT_IMAGE
      | VIDEO_VIEWS_PLACEMENT_ALPHA_BITS(128);
   expect(t, vulkan_views_translucent(&layout), "an image", 5);
   if (failures == before)
      printf("[pass] %s\n", t);
}

/* Stock views straight from the frame encode as they draw, unless the
 * blends read the canvas, the views are not in FP16 (no HDR, or a
 * preset's chains write the encoding), or the pipeline is missing */
static void test_main_window_decision(void)
{
   static const char t[] = "test_main_window_decision";
   unsigned before = failures;
   setup();
   expect(t, vulkan_views_stock_encodes(&vk, true, false, false),
         "encodes", 0);
   expect(t, !vulkan_views_stock_encodes(&vk, false, false, false),
         "not through chains", 1);
   expect(t, !vulkan_views_stock_encodes(&vk, true, true, false),
         "not into the canvas", 2);
   expect(t, vulkan_views_stock_encodes(&vk, true, false, false)
         && !vulkan_views_stock_encodes(&vk, true, false, true),
         "not when translucent", 5);
   vk.views.fp16 = false;
   expect(t, !vulkan_views_stock_encodes(&vk, true, false, false),
         "not without FP16 views", 3);
   setup();
   vk.pipelines.stock_rect_hdr = VK_NULL_HANDLE;
   expect(t, !vulkan_views_stock_encodes(&vk, true, false, false),
         "not without the pipeline", 4);
   if (failures == before)
      printf("[pass] %s\n", t);
}

/* A window takes them straight in only in the main window's encoding;
 * an SDR window keeps its FP16 image and copy */
static void test_window_decision(void)
{
   static const char t[] = "test_window_decision";
   unsigned before = failures;
   vulkan_output_t out;
   setup();
   out.format      = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
   out.color_space = VK_COLOR_SPACE_HDR10_ST2084_EXT;
   expect(t, vulkan_window_stock_encodes(&vk, &out, true, false), "HDR10", 0);
   expect(t, !vulkan_window_stock_encodes(&vk, &out, false, false),
         "not through chains", 1);
   expect(t, vulkan_window_stock_encodes(&vk, &out, true, false)
         && !vulkan_window_stock_encodes(&vk, &out, true, true),
         "not when translucent", 5);
   out.format      = VK_FORMAT_B8G8R8A8_UNORM;
   out.color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   expect(t, !vulkan_window_stock_encodes(&vk, &out, true, false),
         "not SDR", 2);
   vk.context->swapchain_format = VK_FORMAT_R16G16B16A16_SFLOAT;
   out.format      = VK_FORMAT_R16G16B16A16_SFLOAT;
   out.color_space = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
   expect(t, vulkan_window_stock_encodes(&vk, &out, true, false), "scRGB", 3);
   vk.context->flags = 0;
   expect(t, !vulkan_window_stock_encodes(&vk, &out, true, false),
         "not without HDR output", 4);
   if (failures == before)
      printf("[pass] %s\n", t);
}

/* A window in the main window's encoding keeps its FP16 image while its
 * stock views encode straight in, so a frame with a translucent
 * placement goes through it with nothing made then; an SDR window's
 * straight stock views let it go */
static void test_window_fp16(void)
{
   static const char t[] = "test_window_fp16";
   unsigned before = failures;
   struct vk_window win;
   video_views_layout_t layout;
   setup();
   memset(&fp16, 0, sizeof(fp16));
   memset(&win, 0, sizeof(win));
   memset(&layout, 0, sizeof(layout));
   win.layout             = &layout;
   win.output.format      = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
   win.output.color_space = VK_COLOR_SPACE_HDR10_ST2084_EXT;
   win.output.dims        = VIDEO_SCALE_PACK(640, 480);
   layout.num_placements  = 1;
   fp16.made              = true;
   expect(t, vulkan_window_fp16_ensure(&vk, &win, true)
         && fp16.ensures == 1 && !fp16.drops && win.hdr_target.made
         && win.hdr_target.dims == win.output.dims,
         "kept while encoding straight", fp16.drops);
   fp16.made = false;
   expect(t, vulkan_window_fp16_ensure(&vk, &win, true),
         "encodes straight without it", 1);
   layout.placements[0].flags = VIDEO_VIEWS_PLACEMENT_ALPHA_BITS(128);
   expect(t, !vulkan_window_fp16_ensure(&vk, &win, true),
         "not translucent without it", 2);
   fp16.made = true;
   expect(t, vulkan_window_fp16_ensure(&vk, &win, true),
         "translucent through it", 3);
   layout.placements[0].flags = 0;
   fp16.made = false;
   expect(t, !vulkan_window_fp16_ensure(&vk, &win, false),
         "not through chains without it", 4);
   /* An SDR window, its pass in targets[1] */
   memset(&fp16, 0, sizeof(fp16));
   vk.views.targets[1].pass       = 41;
   vk.views.targets[1].format     = VK_FORMAT_B8G8R8A8_UNORM;
   vk.views.targets[1].stock_rect = 42;
   vk.views.targets[1].premult    = 43;
   win.output.format      = VK_FORMAT_B8G8R8A8_UNORM;
   win.output.color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   win.hdr_target.made    = true;
   expect(t, vulkan_window_fp16_ensure(&vk, &win, true)
         && fp16.drops == 1 && !fp16.ensures && !win.hdr_target.made,
         "let go by an SDR window's straight stock views", fp16.ensures);
   if (failures == before)
      printf("[pass] %s\n", t);
}

/* An SDR window in B8G8R8A8 beside the A2B10G10R10 swapchain, its
 * pass's pipelines in targets[1] */
static void window_setup(struct vk_window *win, video_views_layout_t *layout)
{
   setup();
   memset(&drew, 0, sizeof(drew));
   memset(win, 0, sizeof(*win));
   memset(layout, 0, sizeof(*layout));
   vk.views.targets[1].pass             = 41;
   vk.views.targets[1].stock_rect       = 42;
   vk.views.targets[1].premult          = 43;
   vk.views.targets[1].stock_rect_blend = 44;
   vk.views.targets[1].format           = VK_FORMAT_B8G8R8A8_UNORM;
   win->output.render_pass              = 41;
   win->output.format                   = VK_FORMAT_B8G8R8A8_UNORM;
   win->output.dims                     = VIDEO_SCALE_PACK(320, 240);
   win->layout                          = layout;
   win->chains[0]                       = (vulkan_filter_chain_t*)layout;
}

/* A screen window draws its views with its own pass's pipelines, through
 * its chains or straight in, and leaves the window's own for the output
 * after; its FP16 image goes through premult. The plain draw takes them
 * in another format without HDR, and none in the swapchain's. */
static void test_window_screens_target(void)
{
   static const char t[] = "test_window_screens_target";
   unsigned before = failures;
   unsigned i;
   video_frame_info_t info;
   struct vulkan_views_draw_ctx d;
   video_views_layout_t layout;
   struct vk_window win;
   d.video_info = &info;
   for (i = 0; i < 2; i++)
   {
      /* chains_sdr, then stock_sdr */
      window_setup(&win, &layout);
      d.direct = i ? &frame : NULL;
      vulkan_window_draw_screens(&vk, &d, &win, i ? 42 : VK_NULL_HANDLE,
            !i, true, true, 43);
      expect(t, drew.placements == 1 && drew.target,
            "drawn with the target set", i);
      expect(t, drew.stock == 42 && drew.blend == 44 && drew.fill == 43,
            "with the window pass's pipelines", (double)drew.stock);
      expect(t, drew.direct == (i == 1), "direct only straight in", i);
      expect(t, !vk.views.target && !vk.views.stock_pipeline
            && !vk.views.blend_pipeline && !vk.views.fill_pipeline,
            "the window's own after", i);
      expect(t, !drew.quads, "no FP16 quad", drew.quads);
   }
   window_setup(&win, &layout);
   d.direct = NULL;
   vulkan_window_draw_screens(&vk, &d, &win, VK_NULL_HANDLE, true, true,
         true, 43);
   expect(t, drew.chains_pass == 41 && drew.chains_target == VK_NULL_HANDLE,
         "the chains into the window's pass, then their own",
         (double)drew.chains_pass);
   window_setup(&win, &layout);
   vulkan_window_draw_screens(&vk, &d, &win, VK_NULL_HANDLE, false, true,
         true, 43);
   expect(t, drew.quads == 1 && drew.quad_pipeline == 43
         && drew.quad_img == &win.hdr
         && drew.quad_format == VK_FORMAT_R16G16B16A16_SFLOAT,
         "the FP16 image through premult", (double)drew.quad_pipeline);
   expect(t, !drew.placements, "no placements in FP16", drew.placements);
   /* Without HDR, in another format */
   window_setup(&win, &layout);
   vk.context->flags = 0;
   d.direct          = &frame;
   vulkan_window_draw_screens(&vk, &d, &win, VK_NULL_HANDLE, false, true,
         false, 43);
   expect(t, drew.placements == 1 && drew.target && drew.stock == 42
         && drew.blend == 44 && drew.fill == 43 && drew.direct,
         "plain: the window pass's pipelines", (double)drew.stock);
   expect(t, !vk.views.target && !vk.views.stock_pipeline
         && !vk.views.blend_pipeline && !vk.views.fill_pipeline,
         "plain: the window's own after", 0);
   /* and in the swapchain's */
   window_setup(&win, &layout);
   vk.context->flags  = 0;
   win.output.format  = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
   vulkan_window_draw_screens(&vk, &d, &win, VK_NULL_HANDLE, false, true,
         false, VK_NULL_HANDLE);
   expect(t, drew.placements == 1 && !drew.target && !drew.stock
         && !drew.blend && !drew.fill && drew.direct,
         "plain in the swapchain's format: the window's own",
         (double)drew.stock);
   if (failures == before)
      printf("[pass] %s\n", t);
}

int main(void)
{
   test_uniform_layout();
   test_encoded_draw();
   test_plain_draw();
   test_translucent_draw();
   test_translucent_layout();
   test_main_window_decision();
   test_window_decision();
   test_window_fp16();
   test_target_pipeline_draw();
   test_sdr_window_decision();
   test_fill_pipeline();
   test_target_use();
   test_window_screens_target();
   test_encoded_mode();
   test_image_encoded();
   test_encoded_solid();
   return failures ? 1 : 0;
}
