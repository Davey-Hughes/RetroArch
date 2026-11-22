/* GL3's headset draw, copied out of gfx/drivers/gl3.c, with GL, the
 * runtime's textures and the chain's passes stubbed: each screen is
 * drawn straight into the texture the runtime gives, a new frame ends
 * the chain's frame, and a repeat draws a screen again only when it
 * moved or changed size, by replaying the last frame's final pass. A
 * blank screen's texture is only cleared, and so are the tracked eyes'
 * of a blank screen, whose views are not read. A slot's insets are
 * drawn into its texture after it, each in its rect. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_miscellaneous.h>
#include "gfx/video_views.h"
#include "gfx/video_defines.h"
#include "gfx/video_xr.h"

typedef unsigned GLuint;
typedef int GLint;
typedef unsigned GLenum;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef int GLsizei;
typedef unsigned GLbitfield;

#define GL_FRAMEBUFFER               0x8D40
#define GL_DRAW_FRAMEBUFFER_BINDING  0x8CA6
#define GL_VIEWPORT                  0x0BA2
#define GL_SCISSOR_TEST              0x0C11
#define GL_BLEND                     0x0BE2
#define GL_COLOR_ATTACHMENT0         0x8CE0
#define GL_TEXTURE_2D                0x0DE1
#define GL_COLOR_BUFFER_BIT          0x00004000
#define GL_TEXTURE0                  0x84C0
#define GL_NEAREST                   0x2600
#define GL_LINEAR                    0x2601
#define GL_TEXTURE_MAG_FILTER        0x2800
#define GL_TEXTURE_MIN_FILTER        0x2801
#define GL_TEXTURE_WRAP_S            0x2802
#define GL_TEXTURE_WRAP_T            0x2803
#define GL_CLAMP_TO_EDGE             0x812F

#define GL3_FLAG_HW_RENDER_BOTTOM_LEFT (1 << 0)

struct gl3_buffer_locations
{
   int mvp;
};

typedef struct
{
   float data[16];
} math_matrix_4x4;

typedef struct
{
   unsigned width;
   unsigned height;
} openxr_projection_t;

struct gl3_filter_chain_texture
{
   GLuint image;
   unsigned dims;
   unsigned padded_dims;
   unsigned origin;
   GLenum format;
};

struct gl3_viewport
{
   unsigned pos;
   unsigned dims;
};

typedef struct gl3_filter_chain gl3_filter_chain_t;
typedef struct gl_openxr gl_openxr_t;

typedef struct
{
   video_views_map_t views;
   int headset_eye[2];
   bool frame_repeat;
} video_frame_info_t;

typedef struct gl3
{
   struct
   {
      GLuint tex[RETRO_VIDEO_VIEWS_MAX];
      unsigned tex_dims[RETRO_VIDEO_VIEWS_MAX];
      GLenum tex_format[RETRO_VIDEO_VIEWS_MAX];
      unsigned copied;
   } views;
   struct
   {
      GLuint alpha_blend;
      struct gl3_buffer_locations alpha_blend_loc;
   } pipelines;
   struct
   {
      gl_openxr_t *host;
      video_xr_quad_set_t set;
      struct
      {
         video_xr_slot_t info;
         video_xr_quad_t quads[4];
         video_xr_inset_t insets[RETRO_VIDEO_VIEWS_MAX];
         unsigned num_quads;
         unsigned num_insets;
         bool published;
      } shown[VIDEO_XR_MAX_SLOTS];
      float mvp[16];
      GLuint fbo;
      GLuint samplers[2];
      bool proj_shown;
   } xr;
   math_matrix_4x4 mvp_no_rot;
   math_matrix_4x4 mvp_no_rot_yflip;
   unsigned rotation_raw;
   unsigned flags;
} gl3_t;

static unsigned failures;

#define OPENXR_PROJECTION_SLOT VIDEO_XR_MAX_SLOTS

/* The runtime's textures are 0x100 + slot * 2 + layer; any other name
 * attached is an image of the driver's own. */
#define RUNTIME_TEX(s, l) (0x100u + (s) * 2u + (l))

static unsigned offscreen_passes, viewport_passes, replays, agains;
static unsigned frames_ended, last_pass_dims, last_pass_pos;
static unsigned scissors, last_scissor_pos, last_scissor_dims;
static bool chain_drawn = true;
static unsigned runtime_attaches, own_attaches, clears;
static unsigned tex_binds, quads_drawn, eye_dims;
static unsigned published[OPENXR_PROJECTION_SLOT + 1];
static video_xr_quad_set_t planned;

static void glGetIntegerv(GLenum pname, GLint *v)
{
   (void)pname;
   v[0] = 0;
   if (pname == GL_VIEWPORT)
      v[1] = v[2] = v[3] = 0;
}
static GLboolean glIsEnabled(GLenum cap) { (void)cap; return 0; }
static void glEnable(GLenum cap) { (void)cap; }
static void glDisable(GLenum cap) { (void)cap; }
static void glGenFramebuffers(GLsizei n, GLuint *fbos)
{
   (void)n;
   fbos[0] = 1;
}
static void glBindFramebuffer(GLenum target, GLuint fbo)
{
   (void)target;
   (void)fbo;
}
static void glFramebufferTexture2D(GLenum target, GLenum att,
      GLenum textarget, GLuint tex, GLint level)
{
   (void)target;
   (void)att;
   (void)textarget;
   (void)level;
   if (!tex)
      return;
   if (tex >= RUNTIME_TEX(0, 0)
         && tex <= RUNTIME_TEX(OPENXR_PROJECTION_SLOT, 1))
      runtime_attaches++;
   else
      own_attaches++;
}
static void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
   (void)x;
   (void)y;
   (void)w;
   (void)h;
}
static void glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
   scissors++;
   last_scissor_pos  = VIDEO_POS_PACK(x, y);
   last_scissor_dims = VIDEO_SCALE_PACK(w, h);
}
static void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
   (void)r;
   (void)g;
   (void)b;
   (void)a;
}
static void glClear(GLbitfield mask) { (void)mask; clears++; }
static void glGenSamplers(GLsizei n, GLuint *samplers)
{
   (void)n;
   samplers[0] = 1;
   samplers[1] = 2;
}
static void glSamplerParameteri(GLuint sampler, GLenum pname, GLint param)
{
   (void)sampler;
   (void)pname;
   (void)param;
}
static void glBindSampler(GLuint unit, GLuint sampler)
{
   (void)unit;
   (void)sampler;
}
static void glActiveTexture(GLenum unit) { (void)unit; }
static void glBindTexture(GLenum target, GLuint tex)
{
   (void)target;
   if (tex)
      tex_binds++;
}
static void gl3_draw_textured_quad(gl3_t *gl, GLuint prog,
      const struct gl3_buffer_locations *loc, const float *mvp,
      const float *vbo_data)
{
   (void)gl;
   (void)prog;
   (void)loc;
   (void)mvp;
   (void)vbo_data;
   quads_drawn++;
}

static void gl3_filter_chain_set_input_texture(gl3_filter_chain_t *chain,
      const struct gl3_filter_chain_texture *in)
{
   (void)chain;
   (void)in;
}
static void gl3_filter_chain_build_offscreen_passes(
      gl3_filter_chain_t *chain, const struct gl3_viewport *vp)
{
   (void)chain;
   (void)vp;
   offscreen_passes++;
}
static bool gl3_filter_chain_replay_ready(const gl3_filter_chain_t *chain)
{
   (void)chain;
   return chain_drawn;
}
static void gl3_filter_chain_build_viewport_pass_replay(
      gl3_filter_chain_t *chain, const struct gl3_viewport *vp,
      const float *mvp)
{
   (void)chain;
   (void)mvp;
   replays++;
   last_pass_dims = vp->dims;
   last_pass_pos  = vp->pos;
}
static void gl3_filter_chain_build_viewport_pass(gl3_filter_chain_t *chain,
      const struct gl3_viewport *vp, const float *mvp)
{
   (void)chain;
   (void)mvp;
   viewport_passes++;
   last_pass_dims = vp->dims;
   last_pass_pos  = vp->pos;
}
static void gl3_filter_chain_build_viewport_pass_again(
      gl3_filter_chain_t *chain, const struct gl3_viewport *vp,
      const float *mvp)
{
   (void)chain;
   (void)mvp;
   agains++;
   last_pass_dims = vp->dims;
   last_pass_pos  = vp->pos;
}
static void gl3_filter_chain_end_frame(gl3_filter_chain_t *chain)
{
   (void)chain;
   frames_ended++;
}

static void gl3_xr_plan(gl3_t *gl, const video_frame_info_t *video_info,
      bool views, bool eyes, unsigned frame_dims, unsigned src_dims,
      unsigned ui_dims)
{
   (void)video_info;
   (void)views;
   (void)eyes;
   (void)frame_dims;
   (void)src_dims;
   (void)ui_dims;
   gl->xr.set = planned;
}
static void gl3_xr_forget(gl3_t *gl, unsigned s, bool yield)
{
   (void)yield;
   gl->xr.shown[s].published = false;
}
static unsigned gl3_xr_eye_screens(const video_frame_info_t *video_info)
{
   (void)video_info;
   return 0;
}
unsigned gl_openxr_slot_refused(gl_openxr_t *xr, unsigned slot)
{
   (void)xr;
   (void)slot;
   return 0;
}
bool gl_openxr_slot_target(gl_openxr_t *xr, unsigned slot,
      unsigned dims, unsigned layers, uint32_t textures[2])
{
   (void)xr;
   (void)dims;
   (void)layers;
   textures[0] = RUNTIME_TEX(slot, 0);
   textures[1] = RUNTIME_TEX(slot, 1);
   return true;
}
void gl_openxr_slot_publish(gl_openxr_t *xr, unsigned slot,
      const video_xr_quad_set_t *quads, const void *projection)
{
   (void)xr;
   (void)quads;
   (void)projection;
   published[slot]++;
}
void gl_openxr_slot_forget(gl_openxr_t *xr, unsigned slot)
{
   (void)xr;
   (void)slot;
}
static void gl3_xr_copy_ui(gl3_t *gl, unsigned s, GLuint tex,
      unsigned ui_dims)
{
   (void)gl;
   (void)s;
   (void)tex;
   (void)ui_dims;
}
unsigned gl_openxr_eye_dims(const gl_openxr_t *xr)
{
   (void)xr;
   return eye_dims;
}
void gl_openxr_projection(gl_openxr_t *xr, unsigned width,
      unsigned height, openxr_projection_t *out)
{
   (void)xr;
   out->width  = width;
   out->height = height;
}

#include "driver_code.inc"

#define CHECK(cond) do { if (!(cond)) { \
   printf("FAIL line %u: %s\n", (unsigned)__LINE__, #cond); \
   failures++; } } while (0)

/* Any chain: the pass steps are stubbed. */
#define FRAME_CHAIN ((gl3_filter_chain_t*)(uintptr_t)1)

/* @screens screens drawn whole, no menu. */
static void plan(unsigned dims, unsigned screens, float x)
{
   unsigned s;
   memset(&planned, 0, sizeof(planned));
   for (s = 0; s < screens; s++)
   {
      planned.slots[s].dims         = dims;
      planned.slots[s].layers       = 1;
      planned.slots[s].view[0]      = -1;
      planned.quads[s].slot         = s;
      planned.quads[s].pose.position.x = x;
   }
   planned.num_quads = screens;
}

/* @chains: each view's, for a plan that draws views. */
static void draw_views(gl3_t *gl, bool new_frame, gl3_filter_chain_t **chains)
{
   video_frame_info_t info;
   struct gl3_filter_chain_texture frame;
   memset(&info, 0, sizeof(info));
   memset(&frame, 0, sizeof(frame));
   frame.image = 0x10;
   frame.dims  = VIDEO_SCALE_PACK(64, 32);
   info.frame_repeat = !new_frame;
   gl3_xr_draw(gl, &info, new_frame, true, false, chains, FRAME_CHAIN, NULL,
         &frame, frame.dims, 0);
}

static void draw(gl3_t *gl, bool new_frame)
{
   draw_views(gl, new_frame, NULL);
}

/* View @view over slot 0's bottom right quarter each way. */
static void plan_inset(int view)
{
   video_xr_inset_t *in = &planned.insets[planned.num_insets++];
   in->rect.x  = 0.75;
   in->rect.y  = 0.75;
   in->rect.w  = 0.25;
   in->rect.h  = 0.25;
   in->view[0] = view;
   in->view[1] = (planned.slots[0].layers > 1) ? view : -1;
   in->slot    = 0;
   in->screen  = 1;
}

static void reset(gl3_t *gl)
{
   memset(gl, 0, sizeof(*gl));
   offscreen_passes = viewport_passes = replays = agains = 0;
   frames_ended = last_pass_dims = last_pass_pos = 0;
   scissors = last_scissor_pos = last_scissor_dims = 0;
   chain_drawn = true;
   runtime_attaches = own_attaches = clears = 0;
   tex_binds = quads_drawn = 0;
   eye_dims = VIDEO_SCALE_PACK(64, 32);
   memset(published, 0, sizeof(published));
}

/* A new frame: each screen straight into the runtime's texture, one
 * final pass each, and the chain's frame ended. */
static void test_direct(void)
{
   gl3_t gl;
   unsigned dims = VIDEO_SCALE_PACK(64, 32);
   reset(&gl);
   plan(dims, 2, 0.0f);
   draw(&gl, true);
   CHECK(offscreen_passes == 2 && viewport_passes == 2);
   CHECK(frames_ended == 2 && replays == 0);
   CHECK(runtime_attaches == 2 && own_attaches == 0);
   CHECK(published[0] == 1 && published[1] == 1);
}

/* A repeat of what was shown draws nothing. */
static void test_repeat_same(void)
{
   gl3_t gl;
   unsigned dims = VIDEO_SCALE_PACK(64, 32);
   reset(&gl);
   plan(dims, 1, 0.0f);
   draw(&gl, true);
   viewport_passes = frames_ended = runtime_attaches = 0;
   draw(&gl, false);
   CHECK(viewport_passes == 0 && replays == 0 && frames_ended == 0);
   CHECK(published[0] == 1);
}

/* Resized or moved while paused: the last frame's final pass replayed
 * at the new size straight into the runtime's texture. No other pass
 * runs and no frame ends, so shader feedback and history stay where
 * that frame left them. */
static void test_repeat_changed(void)
{
   gl3_t gl;
   unsigned dims    = VIDEO_SCALE_PACK(64, 32);
   unsigned changed = VIDEO_SCALE_PACK(80, 40);
   reset(&gl);
   plan(dims, 1, 0.0f);
   draw(&gl, true);
   offscreen_passes = viewport_passes = frames_ended = 0;
   runtime_attaches = own_attaches = 0;
   plan(changed, 1, 0.0f);
   draw(&gl, false);
   CHECK(replays == 1 && offscreen_passes == 0 && viewport_passes == 0);
   CHECK(frames_ended == 0 && last_pass_dims == changed);
   CHECK(runtime_attaches == 1 && own_attaches == 0);
   replays = 0;
   plan(changed, 1, 0.5f);
   draw(&gl, false);
   CHECK(replays == 1 && offscreen_passes == 0 && frames_ended == 0);
   CHECK(published[0] == 3);
}

/* A chain with no frame drawn since it was built has nothing to replay:
 * its passes run, and no frame ends. */
static void test_repeat_unready(void)
{
   gl3_t gl;
   unsigned dims    = VIDEO_SCALE_PACK(64, 32);
   unsigned changed = VIDEO_SCALE_PACK(80, 40);
   reset(&gl);
   plan(dims, 1, 0.0f);
   draw(&gl, true);
   offscreen_passes = viewport_passes = frames_ended = 0;
   chain_drawn = false;
   plan(changed, 1, 0.0f);
   draw(&gl, false);
   CHECK(offscreen_passes == 1 && viewport_passes == 1);
   CHECK(replays == 0 && frames_ended == 0);
}

/* A blank screen's texture is cleared: no pass runs and no frame ends.
 * Going blank on a repeat clears it then, with no replay. */
static void test_blank(void)
{
   gl3_t gl;
   unsigned dims = VIDEO_SCALE_PACK(64, 32);
   reset(&gl);
   plan(dims, 2, 0.0f);
   planned.slots[1].blank = true;
   draw(&gl, true);
   CHECK(offscreen_passes == 1 && viewport_passes == 1);
   CHECK(frames_ended == 1 && replays == 0);
   CHECK(runtime_attaches == 2 && clears == 2);
   CHECK(published[0] == 1 && published[1] == 1);
   viewport_passes = clears = 0;
   planned.slots[0].blank = true;
   draw(&gl, false);
   CHECK(replays == 0 && viewport_passes == 0 && frames_ended == 1);
   CHECK(clears == 1 && published[0] == 2 && published[1] == 1);
}

/* The tracked eyes wait for their views' copies and are drawn from
 * them. A blank screen's are cleared with nothing read, and wait for no
 * copy: none is made. */
static void test_projection_blank(void)
{
   gl3_t gl;
   video_frame_info_t info;
   const unsigned s = OPENXR_PROJECTION_SLOT;
   reset(&gl);
   memset(&info, 0, sizeof(info));
   info.views.num_views       = 2;
   info.views.views[0].screen = 1;
   info.views.views[0].width  = 64;
   info.views.views[0].height = 32;
   info.views.views[1]        = info.views.views[0];
   info.headset_eye[0]        = 0;
   info.headset_eye[1]        = 1;
   gl.views.tex[0]            = 0x20;
   gl.views.tex[1]            = 0x21;
   gl.views.copied            = 0x1;
   CHECK(!gl3_xr_draw_projection(&gl, &info));
   CHECK(clears == 0 && published[s] == 0);
   gl.views.copied            = 0x3;
   CHECK(gl3_xr_draw_projection(&gl, &info));
   CHECK(clears == 2 && quads_drawn == 2 && tex_binds == 2);
   CHECK(runtime_attaches == 2 && published[s] == 1);

   reset(&gl);
   info.views.blank           = 1u << 1;
   CHECK(gl3_xr_eye_views(&info) == 0);
   CHECK(gl3_xr_draw_projection(&gl, &info));
   CHECK(clears == 2 && quads_drawn == 0 && tex_binds == 0);
   CHECK(runtime_attaches == 2 && published[s] == 1 && gl.xr.proj_shown);
}

/* An inset is drawn into its base's texture after it, by its view's
 * chain at its rect, which is given from the top and drawn from the
 * bottom; its frame ends too. One with a zero rect is not drawn. */
static void test_inset(void)
{
   gl3_t gl;
   gl3_filter_chain_t *chains[RETRO_VIDEO_VIEWS_MAX];
   unsigned dims = VIDEO_SCALE_PACK(64, 32);
   memset(chains, 0, sizeof(chains));
   chains[1] = FRAME_CHAIN;
   reset(&gl);
   plan(dims, 1, 0.0f);
   plan_inset(1);
   draw_views(&gl, true, chains);
   CHECK(offscreen_passes == 2 && viewport_passes == 2 && agains == 0);
   CHECK(frames_ended == 2 && replays == 0);
   CHECK(runtime_attaches == 1 && own_attaches == 0 && clears == 1);
   CHECK(last_pass_pos == VIDEO_POS_PACK(48, 0));
   CHECK(last_pass_dims == VIDEO_SCALE_PACK(16, 8));
   CHECK(published[0] == 1);

   reset(&gl);
   plan(dims, 1, 0.0f);
   plan_inset(1);
   planned.insets[0].rect.w = 0.0;
   draw_views(&gl, true, chains);
   CHECK(offscreen_passes == 1 && viewport_passes == 1);
   CHECK(frames_ended == 1 && last_pass_dims == dims && clears == 1);
}

/* A blank inset's rect is cleared, and no pass runs for it. */
static void test_inset_blank(void)
{
   gl3_t gl;
   gl3_filter_chain_t *chains[RETRO_VIDEO_VIEWS_MAX];
   unsigned dims = VIDEO_SCALE_PACK(64, 32);
   memset(chains, 0, sizeof(chains));
   chains[1] = FRAME_CHAIN;
   reset(&gl);
   plan(dims, 1, 0.0f);
   plan_inset(1);
   planned.insets[0].blank = true;
   draw_views(&gl, true, chains);
   CHECK(offscreen_passes == 1 && viewport_passes == 1 && frames_ended == 1);
   CHECK(clears == 2 && scissors == 1);
   CHECK(last_scissor_pos == VIDEO_POS_PACK(48, 0));
   CHECK(last_scissor_dims == VIDEO_SCALE_PACK(16, 8));
   CHECK(last_pass_dims == dims);
}

/* A repeat draws a slot again when its insets changed, and only then:
 * the base and the inset replay. */
static void test_inset_repeat(void)
{
   gl3_t gl;
   gl3_filter_chain_t *chains[RETRO_VIDEO_VIEWS_MAX];
   unsigned dims = VIDEO_SCALE_PACK(64, 32);
   memset(chains, 0, sizeof(chains));
   chains[1] = FRAME_CHAIN;
   reset(&gl);
   plan(dims, 2, 0.0f);
   plan_inset(1);
   draw_views(&gl, true, chains);
   offscreen_passes = viewport_passes = frames_ended = 0;
   draw_views(&gl, false, chains);
   CHECK(viewport_passes == 0 && replays == 0 && published[0] == 1);
   planned.insets[0].rect.x = 0.0;
   draw_views(&gl, false, chains);
   CHECK(replays == 2 && offscreen_passes == 0 && viewport_passes == 0);
   CHECK(frames_ended == 0 && last_pass_pos == VIDEO_POS_PACK(0, 0));
   CHECK(published[0] == 2 && published[1] == 1);
   /* Gone: the base alone again. */
   replays = 0;
   planned.num_insets = 0;
   draw_views(&gl, false, chains);
   CHECK(replays == 1 && published[0] == 3 && published[1] == 1);
}

/* A view over a stereo base is in both layers: its passes run and its
 * frame ends once, and the second layer draws its final pass again. */
static void test_inset_stereo(void)
{
   gl3_t gl;
   gl3_filter_chain_t *chains[RETRO_VIDEO_VIEWS_MAX];
   unsigned dims = VIDEO_SCALE_PACK(64, 32);
   memset(chains, 0, sizeof(chains));
   chains[0] = chains[1] = chains[2] = FRAME_CHAIN;
   reset(&gl);
   plan(dims, 1, 0.0f);
   planned.slots[0].layers  = 2;
   planned.slots[0].view[0] = 0;
   planned.slots[0].view[1] = 1;
   plan_inset(2);
   draw_views(&gl, true, chains);
   CHECK(offscreen_passes == 3 && viewport_passes == 3 && agains == 1);
   CHECK(frames_ended == 3 && replays == 0);
   CHECK(runtime_attaches == 2 && clears == 2);
   CHECK(last_pass_dims == VIDEO_SCALE_PACK(16, 8));
}

int main(void)
{
   test_direct();
   test_repeat_same();
   test_repeat_changed();
   test_repeat_unready();
   test_blank();
   test_projection_blank();
   test_inset();
   test_inset_blank();
   test_inset_repeat();
   test_inset_stereo();
   printf("%u failed (10 GL3 headset draw cases)\n", failures);
   return failures ? 1 : 0;
}
