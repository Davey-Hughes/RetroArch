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

/* STANDIN-TESTS: later tasks add test functions above this line. */

int main(void)
{
   test_runtime();
   /* STANDIN-CALLS: later tasks add calls above this line. */
   if (failures)
   {
      fprintf(stderr, "%d failure(s)\n", failures);
      return 1;
   }
   puts("PASS openxr_core_test");
   return 0;
}
