/* The shared OpenXR units (gfx/common/openxr_*.c), as they ship,
 * against a runtime the test stands in for. Each unit's xr calls go
 * through the proc lookup it is given, so the stand-in answers them. */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include <boolean.h>
#include <compat/strl.h>
#include <string/stdstring.h>
#include <openxr/openxr.h>
#include <queues/message_queue.h>

#include "../../../msg_hash.h"

/* ---- logging and messages ---- */
static char t_log[4096];
static void t_append(const char *fmt, va_list ap)
{
   size_t len = strlen(t_log);
   if (len < sizeof(t_log) - 1)
      vsnprintf(t_log + len, sizeof(t_log) - len, fmt, ap);
}
void RARCH_LOG(const char *fmt, ...) { va_list ap; va_start(ap, fmt); t_append(fmt, ap); va_end(ap); }
void RARCH_WARN(const char *fmt, ...) { va_list ap; va_start(ap, fmt); t_append(fmt, ap); va_end(ap); }
void RARCH_ERR(const char *fmt, ...) { va_list ap; va_start(ap, fmt); t_append(fmt, ap); va_end(ap); }
static int t_notified = -1;
const char *msg_hash_to_str(enum msg_hash_enums msg) { t_notified = (int)msg; return "msg"; }
void runloop_msg_queue_push(const char *msg, size_t len, unsigned prio,
      unsigned duration, bool flush, char *title,
      enum message_queue_icon icon, enum message_queue_category category) { }

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
   fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
   fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); failures++; } } while (0)
#define LOGGED(s) (strstr(t_log, (s)) != NULL)
static void t_reset_log(void) { t_log[0] = '\0'; t_notified = -1; }

/* ---- the stand-in runtime ---- */
#define T_INSTANCE ((XrInstance)(uintptr_t)0x10)
#define T_SYSTEM   ((XrSystemId)0x20)
static const char *t_exts[8];
static unsigned    t_num_exts;
static XrResult    t_create_instance = XR_SUCCESS;
static XrResult    t_get_system      = XR_SUCCESS;
static uint32_t    t_views           = 2;
static uint32_t    t_rec_width       = 1728;
static uint32_t    t_max_w           = 8192;
static uint32_t    t_max_h           = 2048;
static const char *t_missing;          /* a function the stand-in lacks */
static const char *t_enabled[8];
static unsigned    t_num_enabled;
static unsigned    t_destroyed_instances;

static XRAPI_ATTR XrResult XRAPI_CALL t_enum_exts(const char *layer,
      uint32_t cap, uint32_t *count, XrExtensionProperties *p)
{
   uint32_t i;
   *count = t_num_exts;
   if (!cap)
      return XR_SUCCESS;
   for (i = 0; i < t_num_exts && i < cap; i++)
      strlcpy(p[i].extensionName, t_exts[i], sizeof(p[i].extensionName));
   return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL t_create_instance_fn(
      const XrInstanceCreateInfo *ci, XrInstance *out)
{
   uint32_t i;
   t_num_enabled = 0;
   for (i = 0; i < ci->enabledExtensionCount && i < 8; i++)
      t_enabled[t_num_enabled++] = ci->enabledExtensionNames[i];
   if (t_create_instance != XR_SUCCESS)
      return t_create_instance;
   *out = T_INSTANCE;
   return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL t_destroy_instance(XrInstance i)
{ t_destroyed_instances++; return XR_SUCCESS; }
static XRAPI_ATTR XrResult XRAPI_CALL t_get_system_fn(XrInstance i,
      const XrSystemGetInfo *gi, XrSystemId *out)
{
   if (t_get_system != XR_SUCCESS)
      return t_get_system;
   *out = T_SYSTEM;
   return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL t_get_system_properties(XrInstance i,
      XrSystemId s, XrSystemProperties *p)
{
   strlcpy(p->systemName, "Stand-in HMD", sizeof(p->systemName));
   p->graphicsProperties.maxSwapchainImageWidth  = t_max_w;
   p->graphicsProperties.maxSwapchainImageHeight = t_max_h;
   return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL t_enum_views(XrInstance i, XrSystemId s,
      XrViewConfigurationType t, uint32_t cap, uint32_t *count,
      XrViewConfigurationView *v)
{
   *count = t_views;
   if (cap >= 1 && t_views >= 1)
      v[0].recommendedImageRectWidth = t_rec_width;
   return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL t_enum_modes(XrInstance i, XrSystemId s,
      XrViewConfigurationType t, uint32_t cap, uint32_t *count,
      XrEnvironmentBlendMode *m)
{
   *count = 1;
   if (cap)
      m[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
   return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL t_refresh_rates(XrSession s,
      uint32_t cap, uint32_t *count, float *r) { *count = 0; return XR_SUCCESS; }
static XRAPI_ATTR XrResult XRAPI_CALL t_request_rate(XrSession s, float hz)
{ return XR_SUCCESS; }

#define T_SESSION ((XrSession)(uintptr_t)0x30)
#define T_OTHER   ((XrSession)(uintptr_t)0x31)
static XrEventDataBuffer t_events[8];
static unsigned t_num_events, t_next_event;
static XrResult t_poll_result = XR_SUCCESS;  /* after the queue runs dry */
static unsigned t_begun, t_ended_sessions, t_locks, t_unlocks;
static unsigned t_spaces_made, t_spaces_destroyed, t_space_fail_at;
static unsigned t_seq, t_state_seq, t_begin_seq, t_ended_seq, t_exiting_seq;
static bool t_begin_in_lock, t_end_in_lock;

static bool t_event_room(void)
{
   CHECK(t_num_events < sizeof(t_events) / sizeof(t_events[0]),
         "event queue full");
   return t_num_events < sizeof(t_events) / sizeof(t_events[0]);
}
static void t_push_state(XrSession s, XrSessionState state)
{
   XrEventDataSessionStateChanged *e;
   if (!t_event_room())
      return;
   e = (XrEventDataSessionStateChanged*)&t_events[t_num_events++];
   memset(e, 0, sizeof(t_events[0]));
   e->type    = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
   e->session = s;
   e->state   = state;
}
static void t_push_space(XrSession s, XrReferenceSpaceType type)
{
   XrEventDataReferenceSpaceChangePending *e;
   if (!t_event_room())
      return;
   e = (XrEventDataReferenceSpaceChangePending*)&t_events[t_num_events++];
   memset(e, 0, sizeof(t_events[0]));
   e->type               = XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING;
   e->session            = s;
   e->referenceSpaceType = type;
}
static void t_push_instance_loss(void)
{
   if (!t_event_room())
      return;
   memset(&t_events[t_num_events], 0, sizeof(t_events[0]));
   t_events[t_num_events++].type = XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING;
}
static XRAPI_ATTR XrResult XRAPI_CALL t_poll_event(XrInstance i, XrEventDataBuffer *ev)
{
   if (t_next_event < t_num_events)
   {
      memcpy(ev, &t_events[t_next_event++], sizeof(*ev));
      return XR_SUCCESS;
   }
   return t_poll_result == XR_SUCCESS ? XR_EVENT_UNAVAILABLE : t_poll_result;
}
static XRAPI_ATTR XrResult XRAPI_CALL t_create_session(XrInstance i,
      const XrSessionCreateInfo *ci, XrSession *s) { *s = T_SESSION; return XR_SUCCESS; }
static XRAPI_ATTR XrResult XRAPI_CALL t_destroy_session(XrSession s) { return XR_SUCCESS; }
static XRAPI_ATTR XrResult XRAPI_CALL t_begin_session(XrSession s,
      const XrSessionBeginInfo *b)
{
   t_begun++;
   t_begin_seq     = ++t_seq;
   t_begin_in_lock = (t_locks - t_unlocks == 1);
   return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL t_end_session(XrSession s)
{
   t_ended_sessions++;
   t_end_in_lock = (t_locks - t_unlocks == 1);
   return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL t_create_space(XrSession s,
      const XrReferenceSpaceCreateInfo *ci, XrSpace *out)
{
   if (t_space_fail_at && t_spaces_made + 1 == t_space_fail_at)
      return XR_ERROR_RUNTIME_FAILURE;
   *out = (XrSpace)(uintptr_t)(0x40 + (++t_spaces_made));
   return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL t_destroy_space(XrSpace s) { t_spaces_destroyed++; return XR_SUCCESS; }
static XRAPI_ATTR XrResult XRAPI_CALL t_locate_views(XrSession s,
      const XrViewLocateInfo *li, XrViewState *vs, uint32_t cap, uint32_t *n, XrView *v)
{ *n = 0; return XR_ERROR_RUNTIME_FAILURE; }
static XRAPI_ATTR XrResult XRAPI_CALL t_locate_space(XrSpace a, XrSpace b,
      XrTime t, XrSpaceLocation *loc) { return XR_ERROR_RUNTIME_FAILURE; }

/* the session's hooks, counted */
static unsigned t_h_ended, t_h_exiting, t_h_local;
static XrSessionState t_h_last_state;
static void t_hook_state(void *u, XrSessionState s)
{ t_h_last_state = s; t_state_seq = ++t_seq; }
static void t_hook_ended(void *u) { t_h_ended++; t_ended_seq = ++t_seq; }
static void t_hook_exiting(void *u) { t_h_exiting++; t_exiting_seq = ++t_seq; }
static void t_hook_local(void *u) { t_h_local++; }
static void t_hook_lock(void *u) { t_locks++; }
static void t_hook_unlock(void *u) { t_unlocks++; }

/* STANDIN-FUNCTIONS: later tasks add stand-in functions above this line. */

static const struct { const char *name; PFN_xrVoidFunction fn; } t_procs[] = {
   { "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction)t_enum_exts },
   { "xrCreateInstance",                  (PFN_xrVoidFunction)t_create_instance_fn },
   { "xrDestroyInstance",                 (PFN_xrVoidFunction)t_destroy_instance },
   { "xrGetSystem",                       (PFN_xrVoidFunction)t_get_system_fn },
   { "xrGetSystemProperties",             (PFN_xrVoidFunction)t_get_system_properties },
   { "xrEnumerateViewConfigurationViews", (PFN_xrVoidFunction)t_enum_views },
   { "xrEnumerateEnvironmentBlendModes",  (PFN_xrVoidFunction)t_enum_modes },
   { "xrEnumerateDisplayRefreshRatesFB",  (PFN_xrVoidFunction)t_refresh_rates },
   { "xrRequestDisplayRefreshRateFB",     (PFN_xrVoidFunction)t_request_rate },
   { "xrPollEvent",            (PFN_xrVoidFunction)t_poll_event },
   { "xrCreateSession",        (PFN_xrVoidFunction)t_create_session },
   { "xrDestroySession",       (PFN_xrVoidFunction)t_destroy_session },
   { "xrBeginSession",         (PFN_xrVoidFunction)t_begin_session },
   { "xrEndSession",           (PFN_xrVoidFunction)t_end_session },
   { "xrCreateReferenceSpace", (PFN_xrVoidFunction)t_create_space },
   { "xrDestroySpace",         (PFN_xrVoidFunction)t_destroy_space },
   { "xrLocateViews",          (PFN_xrVoidFunction)t_locate_views },
   { "xrLocateSpace",          (PFN_xrVoidFunction)t_locate_space },
   /* STANDIN-PROCS: later tasks add rows above this line. */
   { NULL, NULL }
};

static XRAPI_ATTR XrResult XRAPI_CALL t_get_proc(XrInstance i,
      const char *name, PFN_xrVoidFunction *fn)
{
   unsigned k;
   *fn = NULL;
   if (t_missing && string_is_equal(name, t_missing))
      return XR_ERROR_FUNCTION_UNSUPPORTED;
   for (k = 0; t_procs[k].name; k++)
      if (string_is_equal(name, t_procs[k].name))
      {
         *fn = t_procs[k].fn;
         return XR_SUCCESS;
      }
   return XR_ERROR_FUNCTION_UNSUPPORTED;
}

/* ---- the units, as they ship ---- */
#include "../../../gfx/common/openxr_runtime.c"
#include "../../../gfx/common/openxr_session.c"
/* STANDIN-UNITS: later tasks add #includes above this line. */

static void t_runtime_defaults(void)
{
   t_num_exts        = 3;
   t_exts[0]         = "XR_KHR_vulkan_enable2";
   t_exts[1]         = "XR_FB_display_refresh_rate";
   t_exts[2]         = "XR_VALVE_frame_controller_interaction";
   t_create_instance = XR_SUCCESS;
   t_get_system      = XR_SUCCESS;
   t_views           = 2;
   t_rec_width       = 1728;
   t_max_w           = 8192;
   t_max_h           = 2048;
   t_missing         = NULL;
   t_reset_log();
}

static void test_runtime(void)
{
   openxr_runtime_t rt;

   /* The graphics extension and the optional ones it offers. */
   t_runtime_defaults();
   memset(&rt, 0, sizeof(rt));
   CHECK(openxr_runtime_create_instance(&rt, t_get_proc, "XR_KHR_vulkan_enable2")
         == OPENXR_RUNTIME_OK, "instance");
   CHECK(t_num_enabled == 3, "enabled %u extensions", t_num_enabled);
   CHECK(rt.refresh_ext && rt.frame_controller, "optional extensions not seen");
   CHECK(rt.DestroyInstance && rt.RequestDisplayRefreshRateFB, "functions not loaded");
   CHECK(openxr_runtime_find_system(&rt) == OPENXR_RUNTIME_OK, "system");
   CHECK(rt.max_dim == 2048, "max_dim %u: the smaller side, at most 4096", rt.max_dim);
   CHECK(rt.rec_width == 1728, "rec_width %u", (unsigned)rt.rec_width);
   CHECK(LOGGED("[OpenXR] Headset: Stand-in HMD.\n"), "log: %s", t_log);
   openxr_runtime_deinit(&rt);
   CHECK(t_destroyed_instances == 1, "instance not destroyed");

   /* No graphics extension: unavailable, with master's log line. */
   t_runtime_defaults();
   t_exts[0] = "XR_KHR_opengl_enable";
   memset(&rt, 0, sizeof(rt));
   CHECK(openxr_runtime_create_instance(&rt, t_get_proc, "XR_KHR_vulkan_enable2")
         == OPENXR_RUNTIME_UNAVAILABLE, "no extension: not unavailable");
   CHECK(LOGGED("[OpenXR] The runtime lacks XR_KHR_vulkan_enable2.\n"), "log: %s", t_log);
   openxr_runtime_deinit(&rt);

   /* An optional extension the runtime lacks is not asked for. */
   t_runtime_defaults();
   t_num_exts = 1;
   memset(&rt, 0, sizeof(rt));
   CHECK(openxr_runtime_create_instance(&rt, t_get_proc, "XR_KHR_vulkan_enable2")
         == OPENXR_RUNTIME_OK, "instance without optionals");
   CHECK(t_num_enabled == 1 && !rt.refresh_ext && !rt.frame_controller,
         "optional extensions asked for");
   openxr_runtime_deinit(&rt);

   /* xrCreateInstance failing: unavailable. */
   t_runtime_defaults();
   t_create_instance = XR_ERROR_RUNTIME_UNAVAILABLE;
   memset(&rt, 0, sizeof(rt));
   CHECK(openxr_runtime_create_instance(&rt, t_get_proc, "XR_KHR_vulkan_enable2")
         == OPENXR_RUNTIME_UNAVAILABLE, "create failing");
   CHECK(LOGGED("[OpenXR] No runtime (xrCreateInstance: "), "log: %s", t_log);
   openxr_runtime_deinit(&rt);

   /* No headset. */
   t_runtime_defaults();
   t_get_system = XR_ERROR_FORM_FACTOR_UNAVAILABLE;
   memset(&rt, 0, sizeof(rt));
   openxr_runtime_create_instance(&rt, t_get_proc, "XR_KHR_vulkan_enable2");
   CHECK(openxr_runtime_find_system(&rt) == OPENXR_RUNTIME_UNAVAILABLE, "no headset");
   CHECK(LOGGED("[OpenXR] No headset (xrGetSystem: "), "log: %s", t_log);
   openxr_runtime_deinit(&rt);

   /* A required function missing: failed. */
   t_runtime_defaults();
   t_missing = "xrGetSystemProperties";
   memset(&rt, 0, sizeof(rt));
   openxr_runtime_create_instance(&rt, t_get_proc, "XR_KHR_vulkan_enable2");
   CHECK(openxr_runtime_find_system(&rt) == OPENXR_RUNTIME_FAILED, "missing function");
   CHECK(LOGGED("[OpenXR] The runtime lacks functions headset output needs.\n"),
         "log: %s", t_log);
   openxr_runtime_deinit(&rt);

   /* A refresh function missing: the rate is measured, never asked. */
   t_runtime_defaults();
   t_missing = "xrRequestDisplayRefreshRateFB";
   memset(&rt, 0, sizeof(rt));
   openxr_runtime_create_instance(&rt, t_get_proc, "XR_KHR_vulkan_enable2");
   CHECK(!rt.refresh_ext, "refresh ext kept without its functions");
   openxr_runtime_deinit(&rt);
}

static void t_session_fresh(openxr_runtime_t *rt, openxr_session_t *s)
{
   t_runtime_defaults();
   memset(rt, 0, sizeof(*rt));
   openxr_runtime_create_instance(rt, t_get_proc, "XR_KHR_vulkan_enable2");
   openxr_runtime_find_system(rt);
   memset(s, 0, sizeof(*s));
   s->hooks.state_changed = t_hook_state;
   s->hooks.ended         = t_hook_ended;
   s->hooks.exiting       = t_hook_exiting;
   s->hooks.local_changed = t_hook_local;
   s->hooks.lock          = t_hook_lock;
   s->hooks.unlock        = t_hook_unlock;
   t_num_events = t_next_event = 0;
   t_poll_result = XR_SUCCESS;
   t_begun = t_ended_sessions = t_locks = t_unlocks = 0;
   t_spaces_made = t_spaces_destroyed = t_space_fail_at = 0;
   t_h_ended = t_h_exiting = t_h_local = 0;
   t_h_last_state = XR_SESSION_STATE_UNKNOWN;
   t_seq = t_state_seq = t_begin_seq = t_ended_seq = t_exiting_seq = 0;
   t_begin_in_lock = t_end_in_lock = false;
}

static void test_session(void)
{
   openxr_runtime_t rt;
   openxr_session_t s;
   int dummy_binding = 0;

   /* READY begins under the lock; FOCUSED is focused; STOPPING ends. */
   t_session_fresh(&rt, &s);
   CHECK(openxr_session_load(&s, &rt), "load");
   CHECK(openxr_session_create(&s, &dummy_binding), "create");
   CHECK(t_spaces_made == 2, "LOCAL and VIEW: %u spaces", t_spaces_made);
   openxr_session_set_alive(&s, true);
   t_push_state(T_SESSION, XR_SESSION_STATE_READY);
   openxr_session_poll(&s);
   CHECK(t_begun == 1 && s.running, "not begun");
   CHECK(t_locks == 1 && t_unlocks == 1, "begin not under the lock");
   CHECK(t_begin_in_lock, "xrBeginSession ran outside the lock");
   CHECK(t_state_seq && t_state_seq < t_begin_seq,
         "state_changed (%u) not before begin (%u)", t_state_seq, t_begin_seq);
   t_push_state(T_SESSION, XR_SESSION_STATE_SYNCHRONIZED);
   t_push_state(T_SESSION, XR_SESSION_STATE_VISIBLE);
   t_push_state(T_SESSION, XR_SESSION_STATE_FOCUSED);
   openxr_session_poll(&s);
   CHECK(openxr_session_focused(&s) && openxr_session_visible(&s), "not focused");
   CHECK(t_h_last_state == XR_SESSION_STATE_FOCUSED, "hook missed a state");
   CHECK(LOGGED("[OpenXR] Session focused.\n"), "log: %s", t_log);
   t_push_state(T_SESSION, XR_SESSION_STATE_VISIBLE);
   openxr_session_poll(&s);
   CHECK(!openxr_session_focused(&s) && openxr_session_visible(&s), "visible");
   t_push_state(T_SESSION, XR_SESSION_STATE_STOPPING);
   openxr_session_poll(&s);
   CHECK(t_ended_sessions == 1 && !s.running, "STOPPING did not end it");
   CHECK(t_end_in_lock, "xrEndSession ran outside the lock");
   CHECK(!openxr_session_visible(&s), "visible while stopping");

   /* Another session's events are not this one's. */
   t_session_fresh(&rt, &s);
   openxr_session_load(&s, &rt);
   openxr_session_create(&s, &dummy_binding);
   t_push_state(T_OTHER, XR_SESSION_STATE_READY);
   t_push_space(T_OTHER, XR_REFERENCE_SPACE_TYPE_LOCAL);
   openxr_session_poll(&s);
   CHECK(t_begun == 0 && t_h_local == 0, "acted on another session's events");

   /* LOCAL recentered for this session; STAGE is not LOCAL. */
   t_push_space(T_SESSION, XR_REFERENCE_SPACE_TYPE_STAGE);
   t_push_space(T_SESSION, XR_REFERENCE_SPACE_TYPE_LOCAL);
   openxr_session_poll(&s);
   CHECK(t_h_local == 1, "local_changed %u times", t_h_local);

   /* EXITING: ended once, exiting every time. */
   openxr_session_set_alive(&s, true);
   t_push_state(T_SESSION, XR_SESSION_STATE_EXITING);
   openxr_session_poll(&s);
   CHECK(t_h_ended == 1 && t_h_exiting == 1, "exiting: ended %u exiting %u",
         t_h_ended, t_h_exiting);
   CHECK(s.ended && !openxr_session_alive(&s) && !s.lost, "exit state");
   CHECK(t_ended_seq && t_ended_seq < t_exiting_seq,
         "ended (%u) not before exiting (%u)", t_ended_seq, t_exiting_seq);
   t_push_state(T_SESSION, XR_SESSION_STATE_EXITING);
   openxr_session_poll(&s);
   CHECK(t_h_ended == 1 && t_h_exiting == 2, "second EXITING");

   /* LOSS_PENDING ends it, not lost; instance loss is lost. */
   t_session_fresh(&rt, &s);
   openxr_session_load(&s, &rt);
   openxr_session_create(&s, &dummy_binding);
   t_push_state(T_SESSION, XR_SESSION_STATE_LOSS_PENDING);
   openxr_session_poll(&s);
   CHECK(s.ended && !s.lost && t_h_ended == 1, "loss pending");
   t_push_instance_loss();
   openxr_session_poll(&s);
   CHECK(s.lost && t_h_ended == 1, "instance loss: lost %d ended %u", s.lost, t_h_ended);

   /* xrPollEvent reporting the instance lost, from FOCUSED. */
   t_session_fresh(&rt, &s);
   openxr_session_load(&s, &rt);
   openxr_session_create(&s, &dummy_binding);
   openxr_session_set_alive(&s, true);
   t_push_state(T_SESSION, XR_SESSION_STATE_READY);
   t_push_state(T_SESSION, XR_SESSION_STATE_SYNCHRONIZED);
   t_push_state(T_SESSION, XR_SESSION_STATE_VISIBLE);
   t_push_state(T_SESSION, XR_SESSION_STATE_FOCUSED);
   openxr_session_poll(&s);
   CHECK(openxr_session_focused(&s), "not focused before the loss");
   t_poll_result = XR_ERROR_INSTANCE_LOST;
   openxr_session_poll(&s);
   CHECK(s.lost && s.ended, "XR_ERROR_INSTANCE_LOST not lost");

   /* Reset forgets an ended session but not a lost instance (the host
    * drops the runtime on it); destroy frees the spaces. */
   openxr_session_reset(&s);
   CHECK(!s.session && !s.ended && s.lost, "reset keeps lost");
   CHECK(t_spaces_destroyed == 2, "spaces destroyed: %u", t_spaces_destroyed);
   CHECK(retro_atomic_load_acquire_int(&s.state) == XR_SESSION_STATE_UNKNOWN,
         "state not cleared by reset");
   openxr_session_set_alive(&s, true);
   CHECK(!openxr_session_focused(&s), "focused after reset");

   /* A failed space leaves nothing behind. */
   t_session_fresh(&rt, &s);
   openxr_session_load(&s, &rt);
   t_space_fail_at = 2;
   t_reset_log();
   CHECK(!openxr_session_create(&s, &dummy_binding), "created without VIEW");
   CHECK(t_spaces_destroyed == 1 && !s.session && !s.local_space,
         "VIEW failure left spaces: %u destroyed", t_spaces_destroyed);
   CHECK(LOGGED("[OpenXR] No VIEW space ("), "log: %s", t_log);
   t_session_fresh(&rt, &s);
   openxr_session_load(&s, &rt);
   t_space_fail_at = 1;
   t_reset_log();
   CHECK(!openxr_session_create(&s, &dummy_binding), "created without LOCAL");
   CHECK(t_spaces_destroyed == 0 && !s.session && !s.local_space,
         "LOCAL failure left spaces");
   CHECK(LOGGED("[OpenXR] No LOCAL space ("), "log: %s", t_log);
   t_space_fail_at = 0;

   /* A runtime that lacks a session function. */
   t_session_fresh(&rt, &s);
   t_missing = "xrLocateSpace";
   CHECK(!openxr_session_load(&s, &rt), "loaded without xrLocateSpace");
   openxr_runtime_deinit(&rt);
}

/* STANDIN-TESTS: later tasks add test functions above this line. */

int main(void)
{
   test_runtime();
   test_session();
   /* STANDIN-CALLS: later tasks add calls above this line. */
   if (failures)
   {
      fprintf(stderr, "%d failure(s)\n", failures);
      return 1;
   }
   puts("PASS openxr_core_test");
   return 0;
}
