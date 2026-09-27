/* Copyright  (C) 2010-2026 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (drm_scanout_test.c).
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
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* gfx/common/drm_scanout.c: the scanout timing of a connector named
 * the way Wayland names outputs, read through a non-master
 * descriptor that stays open for re-reads. The sysfs tree and the
 * device nodes are fakes under ./fixture, removed again at the end;
 * libdrm is stubbed here and tells the cards apart by the one byte
 * each fake node holds. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "../../../gfx/common/drm_scanout.h"

typedef struct
{
   uint32_t conn_id;
   uint32_t type;
   uint32_t type_id;
   uint32_t enc_id;   /* 0: idle */
   uint32_t crtc_id;
   uint16_t hdisplay;
   uint16_t htotal;
   uint16_t vdisplay;
   uint16_t vtotal;
   uint32_t clock;    /* kHz */
   uint32_t flags;
   int      vrr;
   int      off;      /* the CRTC has no mode */
} fake_conn_t;

/* Writable, so a test can change a CRTC between reads */
static fake_conn_t card0[] = {
   { 10, DRM_MODE_CONNECTOR_DisplayPort, 1, 20, 30, 3440, 3600, 1440, 1481, 319750, 0, 0 },
   { 11, DRM_MODE_CONNECTOR_DisplayPort, 3, 21, 31, 1920, 2200, 1080, 1125, 148500, 0, 0 },
   { 14, DRM_MODE_CONNECTOR_DisplayPort, 4, 24, 34, 1920, 2200, 1080, 1125,  74250, DRM_MODE_FLAG_INTERLACE, 0 },
   { 16, DRM_MODE_CONNECTOR_eDP,         1, 26, 36, 1280, 1440,  800,  823,  71100, 0, 0 },
};

static fake_conn_t card1[] = {
   { 12, DRM_MODE_CONNECTOR_HDMIA,       1, 22, 32, 3840, 4400, 2160, 2250, 594000, 0, 1 },
   { 13, DRM_MODE_CONNECTOR_DisplayPort, 3, 23, 33, 1920, 2200, 1080, 1125, 148500, 0, 0 },
   { 15, DRM_MODE_CONNECTOR_DisplayPort, 2,  0,  0,    0,    0,    0,    0,      0, 0, 0 },
};

static int fails;
static int allocs;
static int s_master; /* opens find no compositor holding the card */

static void check(const char *what, int ok)
{
   printf("[%s] %s\n", ok ? "pass" : "FAIL", what);
   if (!ok)
      fails++;
}

static const fake_conn_t *card_conns(int fd, int *n)
{
   char c = 0;
   if (pread(fd, &c, 1, 0) != 1)
      return NULL;
   if (c == '0')
   {
      *n = (int)(sizeof(card0) / sizeof(card0[0]));
      return card0;
   }
   if (c == '1')
   {
      *n = (int)(sizeof(card1) / sizeof(card1[0]));
      return card1;
   }
   return NULL;
}

/* which: 0 connector, 1 encoder, 2 CRTC */
static const fake_conn_t *find(int fd, uint32_t id, int which)
{
   int n = 0;
   int i;
   const fake_conn_t *c = card_conns(fd, &n);
   for (i = 0; c && i < n; i++)
   {
      uint32_t key = which == 0 ? c[i].conn_id
                   : which == 1 ? c[i].enc_id : c[i].crtc_id;
      if (id && key == id)
         return &c[i];
   }
   return NULL;
}

drmModeResPtr drmModeGetResources(int fd)
{
   int n = 0;
   int i;
   const fake_conn_t *c = card_conns(fd, &n);
   drmModeResPtr r;
   if (!c || !(r = (drmModeResPtr)calloc(1, sizeof(*r))))
      return NULL;
   r->count_connectors = n;
   r->connectors       = (uint32_t*)calloc(n, sizeof(uint32_t));
   for (i = 0; i < n; i++)
      r->connectors[i] = c[i].conn_id;
   allocs++;
   return r;
}

void drmModeFreeResources(drmModeResPtr p)
{
   if (!p)
      return;
   free(p->connectors);
   free(p);
   allocs--;
}

drmModeConnectorPtr drmModeGetConnectorCurrent(int fd, uint32_t id)
{
   const fake_conn_t *f = find(fd, id, 0);
   drmModeConnectorPtr c;
   if (!f || !(c = (drmModeConnectorPtr)calloc(1, sizeof(*c))))
      return NULL;
   c->connector_id      = f->conn_id;
   c->connector_type    = f->type;
   c->connector_type_id = f->type_id;
   c->encoder_id        = f->enc_id;
   c->connection        = DRM_MODE_CONNECTED;
   allocs++;
   return c;
}

void drmModeFreeConnector(drmModeConnectorPtr p)
{
   if (!p)
      return;
   free(p);
   allocs--;
}

drmModeEncoderPtr drmModeGetEncoder(int fd, uint32_t id)
{
   const fake_conn_t *f = find(fd, id, 1);
   drmModeEncoderPtr e;
   if (!f || !(e = (drmModeEncoderPtr)calloc(1, sizeof(*e))))
      return NULL;
   e->encoder_id = id;
   e->crtc_id    = f->crtc_id;
   allocs++;
   return e;
}

void drmModeFreeEncoder(drmModeEncoderPtr p)
{
   if (!p)
      return;
   free(p);
   allocs--;
}

drmModeCrtcPtr drmModeGetCrtc(int fd, uint32_t id)
{
   const fake_conn_t *f = find(fd, id, 2);
   drmModeCrtcPtr c;
   if (!f || !(c = (drmModeCrtcPtr)calloc(1, sizeof(*c))))
      return NULL;
   c->crtc_id       = id;
   c->mode_valid    = !f->off;
   c->mode.hdisplay = f->hdisplay;
   c->mode.htotal   = f->htotal;
   c->mode.vdisplay = f->vdisplay;
   c->mode.vtotal   = f->vtotal;
   c->mode.clock    = f->clock;
   c->mode.flags    = f->flags;
   allocs++;
   return c;
}

void drmModeFreeCrtc(drmModeCrtcPtr p)
{
   if (!p)
      return;
   free(p);
   allocs--;
}

/* Two CRTC properties: an unrelated one first, then VRR_ENABLED */
drmModeObjectPropertiesPtr drmModeObjectGetProperties(int fd,
      uint32_t object_id, uint32_t object_type)
{
   const fake_conn_t *f = find(fd, object_id, 2);
   drmModeObjectPropertiesPtr p;
   if (      !f || object_type != DRM_MODE_OBJECT_CRTC
         || !(p = (drmModeObjectPropertiesPtr)calloc(1, sizeof(*p))))
      return NULL;
   p->count_props    = 2;
   p->props          = (uint32_t*)calloc(2, sizeof(uint32_t));
   p->prop_values    = (uint64_t*)calloc(2, sizeof(uint64_t));
   p->props[0]       = 100;
   p->prop_values[0] = 7;
   p->props[1]       = 101;
   p->prop_values[1] = (uint64_t)f->vrr;
   allocs++;
   return p;
}

void drmModeFreeObjectProperties(drmModeObjectPropertiesPtr p)
{
   if (!p)
      return;
   free(p->props);
   free(p->prop_values);
   free(p);
   allocs--;
}

drmModePropertyPtr drmModeGetProperty(int fd, uint32_t id)
{
   drmModePropertyPtr p = (drmModePropertyPtr)calloc(1, sizeof(*p));
   (void)fd;
   if (!p)
      return NULL;
   p->prop_id = id;
   strcpy(p->name, id == 101 ? "VRR_ENABLED" : "ACTIVE");
   allocs++;
   return p;
}

void drmModeFreeProperty(drmModePropertyPtr p)
{
   if (!p)
      return;
   free(p);
   allocs--;
}

/* AUTH_MAGIC needs master: EACCES unless this descriptor is it */
int drmAuthMagic(int fd, drm_magic_t magic)
{
   (void)fd;
   (void)magic;
   return s_master ? -EINVAL : -EACCES;
}

/* ./fixture: the sysfs names the lookup scans and two device nodes */
static const char *const fixture_dirs[] = {
   "fixture",
   "fixture/char",
   "fixture/sys",
   "fixture/dev",
   "fixture/sys/card0",
   "fixture/sys/card0-DP-1",
   "fixture/sys/card0-DP-3",
   "fixture/sys/card0-DP-4",
   "fixture/sys/card0-eDP-1",
   "fixture/sys/card1",
   "fixture/sys/card1-HDMI-A-1",
   "fixture/sys/card1-DP-3",
   "fixture/sys/card1-DP-2",
   "fixture/sys/card2-DP-5",
   "fixture/sys/renderD128",
};
#define FIXTURE_DIRS (sizeof(fixture_dirs) / sizeof(fixture_dirs[0]))

/* ./fixture/char: the /sys/dev/char links a device number is found by */
static const struct
{
   const char *link;
   const char *target;
} fixture_links[] = {
   { "fixture/char/226:0",   "../sys/card0" },
   { "fixture/char/226:1",   "../../devices/pci0000:00/0000:03:00.0/drm/card1" },
   { "fixture/char/226:5",   "../../devices/pci0000:00/0000:05:00.0/drm/card5" },
   { "fixture/char/226:128", "../../devices/pci0000:00/0000:03:00.0/drm/renderD128" },
};
#define FIXTURE_LINKS (sizeof(fixture_links) / sizeof(fixture_links[0]))

static int fixture_node(const char *path, char card)
{
   FILE *f = fopen(path, "w");
   if (!f)
      return -1;
   fputc(card, f);
   return fclose(f);
}

static int make_fixture(void)
{
   size_t i;
   for (i = 0; i < FIXTURE_DIRS; i++)
      if (mkdir(fixture_dirs[i], 0755) != 0)
         return -1;
   if (     fixture_node("fixture/dev/card0", '0') != 0
         || fixture_node("fixture/dev/card1", '1') != 0)
      return -1;
   for (i = 0; i < FIXTURE_LINKS; i++)
      if (symlink(fixture_links[i].target, fixture_links[i].link) != 0)
         return -1;
   return 0;
}

static void remove_fixture(void)
{
   size_t i;
   unlink("fixture/dev/card0");
   unlink("fixture/dev/card1");
   for (i = 0; i < FIXTURE_LINKS; i++)
      unlink(fixture_links[i].link);
   for (i = FIXTURE_DIRS; i > 0; i--)
      rmdir(fixture_dirs[i - 1]);
}

static int open_fds(void)
{
   int n = 0;
   struct dirent *e;
   DIR *d = opendir("/proc/self/fd");
   if (!d)
      return -1;
   while ((e = readdir(d)))
      if (e->d_name[0] != '.')
         n++;
   closedir(d);
   return n;
}

static int open_scanout(const char *name, drm_scanout_t *s)
{
   memset(s, 0, sizeof(*s));
   return drm_scanout_open("fixture/sys", "fixture/dev", name, s);
}

/* A lookup that closes what it opened */
static int get(const char *name, drm_scanout_t *s)
{
   int fd = open_scanout(name, s);
   if (fd < 0)
      return 0;
   close(fd);
   return 1;
}

static int same(const drm_scanout_t *a, const drm_scanout_t *b)
{
   return a->frame_ns     == b->frame_ns
       && a->connector_id == b->connector_id
       && a->crtc_id      == b->crtc_id
       && a->hdisplay     == b->hdisplay
       && a->vtotal       == b->vtotal
       && a->vdisplay     == b->vdisplay
       && a->card         == b->card
       && a->vrr          == b->vrr;
}

/* Re-reads through the descriptor the lookup left open */
static void test_update(void)
{
   drm_scanout_t s;
   drm_scanout_t u;
   int fd = open_scanout("DP-1", &s);

   check("DP-1 opens for re-reads", fd >= 0);
   if (fd < 0)
      return;
   u = s;
   check("an update reads the same", drm_scanout_update(fd, &u) && same(&u, &s));

   card0[0].vrr = 1;
   check("an update sees VRR switched on", drm_scanout_update(fd, &u) && u.vrr);
   card0[0].vrr = 0;
   check("  and off again", drm_scanout_update(fd, &u) && !u.vrr);

   card0[0].vtotal = 1500;
   check("an update sees a new mode",
         drm_scanout_update(fd, &u) && u.vtotal == 1500 && u.frame_ns == 16888193);
   card0[0].vtotal = 1481;

   card0[0].off = 1;
   check("an update of a CRTC switched off fails", !drm_scanout_update(fd, &u));
   card0[0].off = 0;

   card0[0].flags = DRM_MODE_FLAG_INTERLACE;
   check("an update to an interlaced mode fails", !drm_scanout_update(fd, &u));
   card0[0].flags = 0;

   /* The compositor swaps two outputs' CRTCs */
   card0[0].crtc_id = 31;
   card0[1].crtc_id = 30;
   check("an update after the connector moved to another CRTC fails",
         !drm_scanout_update(fd, &u));
   card0[0].crtc_id = 30;
   card0[1].crtc_id = 31;

   card0[0].enc_id = 0;
   check("an update of a connector gone idle fails", !drm_scanout_update(fd, &u));
   card0[0].enc_id = 20;

   check("  and the same once it is back",
         drm_scanout_update(fd, &u) && same(&u, &s));

   close(fd);
}

static int open_card(unsigned major, unsigned minor)
{
   return drm_scanout_open_card("fixture/char", "fixture/dev", major, minor);
}

/* The card Vulkan names by device number, and a connector on it by id */
static void test_by_number(void)
{
   drm_scanout_t s;
   int fd = open_card(226, 1);

   check("card1 is opened by its device number", fd >= 0);
   if (fd >= 0)
   {
      memset(&s, 0, sizeof(s));
      check("  a connector on it is read by id",
            drm_scanout_read_connector(fd, 12, &s)
            && s.connector_id == 12 && s.crtc_id == 32
            && s.hdisplay == 3840 && s.vdisplay == 2160
            && s.vtotal == 2250 && s.vrr);
      memset(&s, 0, sizeof(s));
      check("  an idle connector is refused, leaving out as it was",
            !drm_scanout_read_connector(fd, 15, &s) && s.vtotal == 0);
      check("  another card's connector is refused",
            !drm_scanout_read_connector(fd, 10, &s));
      check("  an unknown connector is refused",
            !drm_scanout_read_connector(fd, 99, &s));
      close(fd);
   }

   fd = open_card(226, 0);
   check("card0 is opened through a relative link", fd >= 0);
   if (fd >= 0)
   {
      check("  its interlaced connector is refused",
            !drm_scanout_read_connector(fd, 14, &s));
      close(fd);
   }

   check("a render node is refused", open_card(226, 128) < 0);
   check("a card without a device node is refused", open_card(226, 5) < 0);
   check("a number with no link is refused", open_card(226, 9) < 0);

   s_master = 1;
   fd = open_card(226, 1);
   check("a card no one holds is refused, not kept as master", fd < 0);
   if (fd >= 0)
      close(fd);
   s_master = 0;
}

int main(void)
{
   drm_scanout_t s;
   int fds;

   remove_fixture();
   if (make_fixture() != 0)
   {
      puts("FAIL: could not build ./fixture");
      remove_fixture();
      return 1;
   }
   fds = open_fds();

   check("DP-1 is found", get("DP-1", &s));
   check("  its lines", s.vtotal == 1481 && s.vdisplay == 1440);
   check("  its frame period in ns", s.frame_ns == 16674276);
   check("  its connector, CRTC and card",
         s.connector_id == 10 && s.crtc_id == 30 && s.card == 0);
   check("  VRR off", !s.vrr);
   check("  its width", s.hdisplay == 3440);
   check("HDMI-A-1 is found with VRR on",
         get("HDMI-A-1", &s) && s.vrr && s.vtotal == 2250 && s.card == 1);
   check("eDP-1 is found by the kernel's mixed-case name",
         get("eDP-1", &s) && s.vtotal == 823 && s.card == 0);
   check("a name on two cards is refused", !get("DP-3", &s));
   check("an idle connector is refused", !get("DP-2", &s));
   check("an interlaced mode is refused", !get("DP-4", &s));
   check("a name on no card is refused", !get("DP-9", &s));
   check("a card without a device node is refused", !get("DP-5", &s));
   check("an empty name is refused", !get("", &s));
   s_master = 1;
   check("a card no compositor holds is refused, not kept as master",
         !get("DP-1", &s));
   s_master = 0;
   {
      char name[32];
      drm_scanout_connector_name(DRM_MODE_CONNECTOR_HDMIA, 1,
            name, sizeof(name));
      check("connector names are the kernel's", !strcmp(name, "HDMI-A-1"));
   }
   test_by_number();
   test_update();
   check("everything taken was freed", allocs == 0);
   check("every descriptor opened was closed", fds > 0 && open_fds() == fds);

   remove_fixture();
   puts(fails ? "FAILED" : "ALL OK");
   return fails != 0;
}
