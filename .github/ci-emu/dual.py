#!/usr/bin/env python3
"""Second-display checks for RetroArch on Android, driven through adb.

  dual.py selftest             the parsers, offline
  dual.py probe                the device's displays and how to capture them
  dual.py install APK [CORE ...]
  dual.py core CORE ...        a core into the app's cores directory
  dual.py run --driver gl|vulkan [--threaded 0|1] [--cases a,b,...]
  dual.py geometry LABEL       the main display's layout with no second display
  dual.py compare LABEL LABEL
  dual.py launch [options]     start a core and leave it running
  dual.py state LABEL          evidence: displays, windows, focus, log, captures
  dual.py rate                 frontend frames per second over 10 s
  dual.py menu LABEL [PRESS ...] menu buttons, then the main display, OCR'd
  dual.py frametimes LABEL     main-window present intervals over 5 s

RA_SERIAL picks the device (default emulator-5580; an adb serial or
ip:port), RA_PKG the package (default com.retroarch.aarch64, the
emulator's; the Thor runs com.retroarch beside Davey's own
com.retroarch.aarch64, which no command here installs, stops, starts or
writes). Host output goes under RA_DUAL_OUT/<serial>, device files under
the app's own files/ra-dual.
ANDROID_HOME is the SDK (adb, aapt2), RA_TREE the RetroArch tree the
shader case reads, RA_EMU_TOKEN the emulator console's token file
(default ~/.emulator_console_auth_token).
Stdlib and ImageMagick only; nothing here opens a window or touches the
desktop, and input is only sent while RetroArch has the focus.
"""

import argparse
import contextlib
import io
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get('ANDROID_HOME', '/home/davey/.local/share/android-sdk')
ADB = os.path.join(SDK, 'platform-tools', 'adb')
EMU_TOKEN = os.environ.get('RA_EMU_TOKEN') or os.path.expanduser(
    '~/.emulator_console_auth_token')
SERIAL = os.environ.get('RA_SERIAL', 'emulator-5580')
PKG = os.environ.get('RA_PKG') or 'com.retroarch.aarch64'
# Davey's own RetroArch on his devices: never installed over, stopped,
# driven or written; only an emulator may run this package
KEEP = 'com.retroarch.aarch64'
TREE = os.environ.get('RA_TREE', '/home/davey/projects/RetroArch/.claude/'
                      'worktrees/screen-layouts-assign')
OUT = os.path.join(os.environ.get('RA_DUAL_OUT', os.path.join(HERE, 'out')),
                   re.sub(r'[^A-Za-z0-9_.-]', '_', SERIAL))
ACTIVITY = PKG + '/com.retroarch.browser.retroactivity.RetroActivityFuture'
LAUNCHER = PKG + '/com.retroarch.browser.mainmenu.MainMenuActivity'
DEV = '/sdcard/Android/data/%s/files/ra-dual' % PKG
CORES = '/data/user/0/%s/cores' % PKG
TEST_CORE = 'video_views_libretro_android.so'
PORT = 55355
ASK_WAIT = 120                  # s to answer a permission dialog on a device
EMU_INDEX = 1                   # the emulator's own number for display 2
EMU_SECOND = (1240, 1080, 320)  # width, height, dpi: the Thor's bottom panel

GREEN = (0, 255, 0)
YELLOW = (255, 255, 0)
WHITE = (255, 255, 255)
NAMES = {GREEN: 'green (screen 0)', YELLOW: 'yellow (screen 1)'}
# Linear sRGB -> linear Display P3 (both D65)
SRGB_TO_P3 = ((0.8224621, 0.1775380, 0.0),
              (0.0331941, 0.9668058, 0.0),
              (0.0170827, 0.0723974, 0.9105199))


def display_p3(c):
    """sRGB colour c as a Display P3 screencap gives it (the Thor's).
    P3 uses the sRGB transfer curve."""
    lin = [v / 255.0 for v in c]
    lin = [v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
           for v in lin]
    out = []
    for row in SRGB_TO_P3:
        v = min(max(sum(m * l for m, l in zip(row, lin)), 0.0), 1.0)
        v = v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055
        out.append(int(round(v * 255)))
    return tuple(out)


_FORMS = {}


def forms(c):
    """The pixel values colour c may capture as: sRGB, Display P3."""
    if c not in _FORMS:
        p = display_p3(c)
        _FORMS[c] = (c,) if p == c else (c, p)
    return _FORMS[c]
# The test core's maps: screen -> (x, y, width, height) in the packed frame
VIEWS = {
    'ds': {0: (0, 0, 256, 192), 1: (0, 192, 256, 192)},
    '3ds': {0: (0, 0, 400, 240), 1: (240, 240, 320, 240)},
}
POINTER = re.compile(r'\[video_views\] pointer(1?) x=(-?\d+) y=(-?\d+) '
                     r'pressed=1 packed=\((\d+),(\d+)\)')
PAD = re.compile(r'\[video_views\] pad port=(\d) buttons=0x([0-9a-f]+)')
# What the second output logs when it fails; only a real failure may
WARNINGS = re.compile(r'\[(GL|Vulkan)\] A screen window'
                      r'|\[Vulkan\] (A second output|Could not make a second'
                      r'|The device.s queue cannot present to a second)'
                      r'|\[Android\] Second display|\[SecondDisplay\]')
# the display goes away under a present: the one warning a removal may log
GONE = (r"\[Vulkan\] A screen window's surface is gone\. "
        r'Its screens go back to the main window\.$')
# gl: a feedback pass, then a pass sized to the output, so a second
# display larger than the main one must grow that pass's FBO
GL_PRESET = ('shaders = 3\n'
             'shader0 = feedback.glsl\n'
             'filter_linear0 = false\n'
             'scale_type0 = source\n'
             'scale0 = 1.0\n'
             'shader1 = pass.glsl\n'
             'filter_linear1 = false\n'
             'scale_type1 = viewport\n'
             'scale1 = 1.0\n'
             'shader2 = pass.glsl\n'
             'filter_linear2 = false\n'
             'feedback_pass = 0\n')
# driver -> (preset, files pushed from the tree, the preset's text when
# the harness writes it)
SHADERS = {'gl': ('feedback_viewport.glslp', ('feedback.glsl', 'pass.glsl'),
                  GL_PRESET),
           'vulkan': ('history.slangp', ('history.slangp', 'history.slang'),
                      None)}
DIRS = ('config', 'remaps', 'saves', 'states', 'system', 'playlists',
        'screenshots', 'cache', 'logs', 'runtime', 'thumbnails', 'records',
        'records_config', 'core_assets', 'roms')


class Fail(Exception):
    pass


class Skip(Exception):
    pass


# ---- adb, the emulator console, network commands ----

def adb(*args, check=True, timeout=60, binary=False):
    r = subprocess.run([ADB, '-s', SERIAL] + list(args),
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=timeout)
    if check and r.returncode != 0:
        raise Fail('adb %s: exit %d: %s' % (
            ' '.join(args), r.returncode,
            r.stderr.decode(errors='replace').strip()))
    return r.stdout if binary else r.stdout.decode(errors='replace')


def sh(cmd, check=True, timeout=60):
    return adb('shell', cmd, check=check, timeout=timeout)


def is_emulator():
    return SERIAL.startswith('emulator-')


def console(cmd, timeout=30):
    """One emulator console command; its answer, or Fail on KO."""
    if not is_emulator():
        raise Skip('%s has no emulator console' % SERIAL)
    with open(EMU_TOKEN) as f:
        token = f.read().strip()
    s = socket.create_connection(('127.0.0.1', int(SERIAL.split('-')[1])),
                                 timeout=timeout)

    def answer():
        buf = b''
        while True:
            chunk = s.recv(65536)
            if not chunk:
                return buf.decode(errors='replace')
            buf += chunk
            text = buf.decode(errors='replace')
            if re.search(r'(^|\n)(OK|KO[^\n]*)\r?\n$', text):
                return text

    try:
        answer()
        s.sendall(('auth %s\n' % token).encode())
        answer()
        s.sendall((cmd + '\n').encode())
        text = answer()
    except socket.timeout:
        raise Fail('console %r: no answer in %d s' % (cmd, timeout))
    finally:
        s.close()
    if re.search(r'(^|\n)KO', text):
        raise Fail('console %r: %s' % (cmd, text.strip()))
    return text


def ra_cmd(cmd, wait=2):
    """A network command, sent from inside the device; the answer, if any
    came back (toybox nc waits for one, so it is capped)."""
    out = sh("echo -n '%s' | timeout %d nc -u 127.0.0.1 %d"
             % (cmd, wait, PORT), check=False, timeout=wait + 30)
    return out.strip()


def wait_until(fn, timeout, msg, every=0.5):
    end = time.time() + timeout
    while True:
        v = fn()
        if v:
            return v
        if time.time() > end:
            raise Fail(msg)
        time.sleep(every)


# ---- the log ----

def log_lines():
    return adb('logcat', '-d', '-v', 'raw', '-s', 'RetroArch:V').splitlines()


_marks = [0]


def mark():
    """A line in RetroArch's log to read on from."""
    _marks[0] += 1
    tag = 'ra-dual-mark-%d-%d' % (os.getpid(), _marks[0])
    sh('log -t RetroArch ' + tag)
    return tag


def lines_after(tag):
    lines = log_lines()
    for i in range(len(lines) - 1, -1, -1):
        if tag in lines[i]:
            return lines[i + 1:]
    raise Fail('log marker %s is gone' % tag)


def wait_log(rx, timeout, tag=None, what=None):
    pat = re.compile(rx)

    def find():
        for line in (lines_after(tag) if tag else log_lines()):
            m = pat.search(line)
            if m:
                return m
        return None
    return wait_until(find, timeout,
                      what or 'no log line matching %r in %d s' % (rx, timeout))


# ---- displays, windows, focus ----

def _first(rx, text, default=None, conv=str):
    m = re.search(rx, text)
    return conv(m.group(1)) if m else default


def _braced(text, key):
    """(what precedes it on its line, the inside) of every key{...} in
    text, nested braces included."""
    out = []
    i = 0
    while True:
        i = text.find(key + '{', i)
        if i < 0:
            return out
        j = i + len(key)
        depth = 0
        while j < len(text):
            if text[j] == '{':
                depth += 1
            elif text[j] == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out.append((text[text.rfind('\n', 0, i) + 1:i],
                    text[i + len(key) + 1:j]))
        i = j + 1


def parse_displays(text):
    """Logical displays by id, from dumpsys display. Window manager's
    override, which carries the rotation, wins over the base info, but
    not for the state: the Thor's override keeps OFF with the panels on."""
    res = {}
    base_state = {}
    blocks = _braced(text, 'DisplayInfo')
    blocks.sort(key=lambda pb: 'mOverrideDisplayInfo' not in pb[0])
    for pre, b in blocks:
        # API 29 has the id inside the quotes: "Name, displayId N"
        m = (re.match(r'"([^"]*)", displayId (\d+)', b)
             or re.match(r'"([^"]*), displayId (\d+)"', b))
        if not m:
            continue
        did = int(m.group(2))
        if 'mBaseDisplayInfo' in pre:
            base_state.setdefault(did, _first(r', state (\w+)', b, ''))
        if did in res:
            continue
        real = re.search(r'\breal (\d+) x (\d+)', b)
        fps = _first(r'renderFrameRate ([\d.]+)', b, None, float)
        mode = _first(r', mode (\d+)', b)
        if fps is None and mode:
            fps = _first(r'\{id=%s, width=\d+, height=\d+, fps=([\d.]+)'
                         % mode, b, None, float)
        res[did] = {
            'id': did,
            'name': m.group(1),
            'unique_id': _first(r'uniqueId "([^"]*)"', b, ''),
            'type': _first(r', type (\w+)', b, ''),
            'state': _first(r', state (\w+)', b, ''),
            'rotation': _first(r', rotation (\d)', b, 0, int),
            'size': [int(real.group(1)), int(real.group(2))] if real else None,
            'fps': fps,
            'private': 'FLAG_PRIVATE' in b,
            'presentation': 'FLAG_PRESENTATION' in b,
            'trusted': 'FLAG_TRUSTED' in b,
            'density': _first(r', density (\d+)', b, None, int),
            'physical': 'deviceProductInfo DeviceProductInfo{' in b,
            'connection': _first(r'connectionToSinkType=(\d+)', b, -1, int),
        }
    for did, state in base_state.items():
        if state:
            res[did]['state'] = state
    return res


def displays():
    return parse_displays(sh('dumpsys display'))


def ordinal(ds, did):
    """Where did sits in DisplayManager.getDisplays(): ascending ids,
    private displays left out."""
    ids = sorted(i for i, d in ds.items() if not d['private'])
    return ids.index(did) + 1


def find_second(ds, probe):
    want = probe['second']['name']
    for i in sorted(ds):
        d = ds[i]
        if i != probe['main']['id'] and not d['private'] and d['name'] == want:
            return i
    return None


def parse_windows(text):
    """Display id -> titles of the app's windows on it."""
    res = {}
    for b in re.split(r'\n(?=\s*Window #\d+ )', text):
        if not re.search(r'\bpackage=%s(?=\s)' % re.escape(PKG), b):
            continue
        d = _first(r'mDisplayId=(\d+)', b, None, int)
        if d is not None:
            res.setdefault(d, []).append(
                _first(r'Window #\d+ Window\{\S+ \S+ ([^}]*)\}', b, '?'))
    return res


def app_windows():
    return parse_windows(sh('dumpsys window windows'))


def parse_focus(text):
    return (_first(r'mTopFocusedDisplayId=(-?\d+)', text, None, int),
            re.findall(r'mCurrentFocus=(Window\{[^}]*\})', text))


def focus():
    return parse_focus(sh('dumpsys window'))


def ours(title):
    """A window or layer title of PKG's RetroActivityFuture, full or
    short component form."""
    return bool(re.search(r'(^|[\s\[])%s/\S*RetroActivityFuture'
                          % re.escape(PKG), title))


def mentions(text):
    """PKG named in text, not as the start of a longer package name."""
    return bool(re.search(r'(?<![\w.])%s(?![\w.])' % re.escape(PKG), text))


def ours_second(title):
    """A window title of PKG's Presentation: the package alone, or on
    API 29 Sys2037 (TYPE_PRESENTATION) and the Activity's name."""
    return bool(re.search(r'(^|[\s{])(%s|Sys2037:%s/\S*)\}?$'
                          % (re.escape(PKG), re.escape(PKG)), title))


def in_front_of(main, top, cur):
    """RetroArch has the keys: its Activity is focused on the main
    display, or its Presentation holds the top focused display."""
    if top == main:
        return any(ours(c) for c in cur)
    return any(ours_second(c) for c in cur)


def require_front(main=0):
    top, cur = focus()
    if not in_front_of(main, top, cur):
        raise Fail('RetroArch does not have the focus (top display %s, %s); '
                   'no input sent' % (top, cur))


def in_front(main=0):
    return in_front_of(main, *focus())


def refocus_after_add(main, settle=10.0):
    """Android may give a display it adds the focus and start its
    launcher there, and RetroArch pauses on the focus loss: once the
    launcher has the focus (or settle s pass), RetroArch is brought back
    to the front. True if it had to be."""
    def taken():
        top, cur = focus()
        return top != main and any(not ours(c) for c in cur)
    try:
        wait_until(taken, settle, 'the focus stayed')
    except Fail:
        pass
    if in_front(main):
        return False
    sh('am start -n %s -a android.intent.action.MAIN '
       '-c android.intent.category.LAUNCHER' % LAUNCHER)
    wait_until(lambda: in_front(main), 15,
               'RetroArch did not get the focus back after the display was '
               'added')
    return True


def alive():
    return bool(sh('pidof %s' % PKG, check=False).strip())


def keep_alone():
    """Davey's RetroArch beside the one under test shares its log tag
    and maybe the command port, so nothing runs while it does."""
    # || true: pidof finding nothing is not an error, adb failing is
    if PKG != KEEP and sh('pidof %s || true' % KEEP).strip():
        raise Fail('%s is running: close it on the device first (the '
                   'harness never stops it)' % KEEP)


OFFLINE = ('selftest', 'compare')


def refused(cmd, pkg, serial):
    """Davey's own RetroArch on a device: offline commands only."""
    return (cmd not in OFFLINE and pkg == KEEP
            and not serial.startswith('emulator-'))


# ---- captures ----

class Image(object):
    def __init__(self, w, h, rgba):
        self.w, self.h, self.rgba = w, h, rgba

    @staticmethod
    def from_png(path):
        wh = subprocess.run(['magick', 'identify', '-format', '%w %h', path],
                            stdout=subprocess.PIPE, check=True).stdout.split()
        rgba = subprocess.run(['magick', path, '-depth', '8', 'RGBA:-'],
                              stdout=subprocess.PIPE, check=True).stdout
        return Image(int(wh[0]), int(wh[1]), rgba)

    def near(self, x, y, c, tol=48):
        i = (y * self.w + x) * 4
        return any(all(abs(self.rgba[i + k] - f[k]) <= tol for k in range(3))
                   for f in forms(c))

    def fraction(self, c, box, tol=48, step=4):
        """How much of box is colour c."""
        x0, y0, x1, y1 = box
        n = hit = 0
        for y in range(y0, y1, step):
            for x in range(x0, x1, step):
                n += 1
                hit += self.near(x, y, c, tol)
        return float(hit) / max(n, 1)

    def differs(self, other, step=4):
        """How much of the image changed against other, same size."""
        if (self.w, self.h) != (other.w, other.h):
            return 1.0
        n = diff = 0
        for y in range(0, self.h, step):
            for x in range(0, self.w, step):
                i = (y * self.w + x) * 4
                n += 1
                diff += self.rgba[i:i + 3] != other.rgba[i:i + 3]
        return float(diff) / max(n, 1)

    def box(self, c, tol=48, step=2):
        """(x0, y0, x1, y1) of a filled rectangle of colour c, x1 and y1
        past its edge, or None."""
        fs = forms(c)
        data, w, h = self.rgba, self.w, self.h
        x0 = y0 = None
        x1 = y1 = -1
        n = 0
        for y in range(0, h, step):
            row = y * w * 4
            for x in range(0, w, step):
                i = row + x * 4
                for r, g, b in fs:
                    if (abs(data[i] - r) <= tol and abs(data[i + 1] - g) <= tol
                            and abs(data[i + 2] - b) <= tol):
                        break
                else:
                    continue
                n += 1
                if x0 is None or x < x0:
                    x0 = x
                if x > x1:
                    x1 = x
                if y0 is None:
                    y0 = y
                y1 = y
        if n < 100:
            return None
        x1 = min(x1 + step, w)
        y1 = min(y1 + step, h)
        if n < 0.8 * ((x1 - x0) // step) * ((y1 - y0) // step):
            return None
        return (x0, y0, x1, y1)


def _screencap(sfid, path):
    args = ['exec-out', 'screencap', '-p'] + (['-d', sfid] if sfid else [])
    png = adb(*args, binary=True, timeout=120)
    if not png.startswith(b'\x89PNG'):
        raise Fail('screencap %s gave no PNG' % (sfid or ''))
    with open(path, 'wb') as f:
        f.write(png)
    return Image.from_png(path)


def _console_shot(path, number=EMU_INDEX):
    """The emulator's own screenshot of its display number."""
    d = tempfile.mkdtemp(prefix='shot.', dir=os.path.dirname(path))
    console('screenrecord screenshot --display %d %s' % (number, d))

    def done():
        pngs = [f for f in os.listdir(d) if f.endswith('.png')]
        if not pngs:
            return None
        p = os.path.join(d, pngs[0])
        size = os.path.getsize(p)
        time.sleep(0.3)
        return p if size and size == os.path.getsize(p) else None
    p = wait_until(done, 15, 'the emulator wrote no screenshot of display %d'
                   % number, every=0.3)
    os.replace(p, path)
    os.rmdir(d)
    return Image.from_png(path)


def capture(did, ds, probe, where, label):
    """Display did as an Image, saved as where/label.png."""
    if did not in ds:
        raise Fail('display %d is gone' % did)
    os.makedirs(where, exist_ok=True)
    path = os.path.join(where, label + '.png')
    role = 'main' if did == probe['main']['id'] else 'second'
    method = probe['capture'][role]
    uid = ds[did]['unique_id']
    if method == 'screencap' and uid.startswith('local:'):
        return _screencap(uid[6:], path)
    if method == 'screencap-default':
        return _screencap(None, path)
    if method == 'console':
        return _console_shot(path, probe.get('console_display', EMU_INDEX))
    raise Fail('no way to capture display %d (%s, %s)' % (did, method, uid))


# ---- RetroArch ----

def cfg_text(d):
    return ''.join('%s = "%s"\n' % (k, v) for k, v in sorted(d.items()))


def push_text(remote, text):
    os.makedirs(OUT, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=OUT, suffix='.cfg')
    with os.fdopen(fd, 'w') as f:
        f.write(text)
    try:
        adb('push', tmp, remote)
    finally:
        os.unlink(tmp)


def base_cfg():
    c = {
        'config_save_on_exit': 'false',
        'audio_enable': 'false',
        'audio_driver': 'null',
        'pause_nonactive': 'false',
        'network_cmd_enable': 'true',
        'network_cmd_port': str(PORT),
        'network_cmd_bind_address': '127.0.0.1',
        'log_verbosity': 'true',
        'frontend_log_level': '1',
        'libretro_log_level': '1',
        'log_to_file': 'false',
        'menu_driver': 'rgui',
        'video_font_enable': 'false',
        'menu_enable_widgets': 'false',
        'input_overlay_enable': 'false',
        'video_smooth': 'false',
        'video_scale_integer': 'false',
        'video_stereo_mode': '0',
        'video_screen_layout': '0',
        'global_core_options': 'true',
        'core_options_path': DEV + '/core-options.cfg',
        'game_specific_options': 'false',
        'auto_overrides_enable': 'false',
        'auto_remaps_enable': 'false',
        'auto_shaders_enable': 'false',
        'savestate_auto_save': 'false',
        'savestate_auto_load': 'false',
        'video_windows_enable': 'true',
        'video_screen1_display': '',
        'video_screen2_display': '',
        'video_menu_follows': '0',
        'video_menu_display': '',
        'video_notifications_window': '0',
        'rgui_config_directory': DEV + '/config',
        'input_remapping_directory': DEV + '/remaps',
        'savefile_directory': DEV + '/saves',
        'savestate_directory': DEV + '/states',
        'system_directory': DEV + '/system',
        'playlist_directory': DEV + '/playlists',
        'content_history_path': DEV + '/playlists/history.lpl',
        'content_favorites_path': DEV + '/playlists/favorites.lpl',
        'content_music_history_path': DEV + '/playlists/music.lpl',
        'content_video_history_path': DEV + '/playlists/video.lpl',
        'content_image_history_path': DEV + '/playlists/image.lpl',
        'screenshot_directory': DEV + '/screenshots',
        'cache_directory': DEV + '/cache',
        'log_dir': DEV + '/logs',
        'runtime_log_directory': DEV + '/runtime',
        'thumbnails_directory': DEV + '/thumbnails',
        'recording_output_directory': DEV + '/records',
        'recording_config_directory': DEV + '/records_config',
        'core_assets_directory': DEV + '/core_assets',
    }
    return c


def load_probe():
    path = os.path.join(OUT, 'probe.json')
    if not os.path.exists(path):
        sys.exit('dual.py: no %s; run "dual.py probe" first' % path)
    with open(path) as f:
        p = json.load(f)
    p['displays'] = dict((int(k), v) for k, v in p['displays'].items())
    return p


class Run(object):
    def __init__(self, driver, threaded, probe):
        self.driver = driver
        self.threaded = threaded
        self.probe = probe
        self.main = probe['main']['id']
        self.ordinal = probe['ordinal']
        self.dir = OUT

    def capture(self, did, ds, label):
        return capture(did, ds, self.probe, self.dir, label)


READY = (r'\[Video\] View map: \d+ views, 2 screens'
         r'|\[video_views\] presents=')


def start(run, cfg=None, opts=None, core=TEST_CORE, content=None,
          ready=READY, on=None, extras=None, menu_only=False):
    """RetroArch from cold with the test config, running core; on
    display on (an id) instead of the default one if given. Screen 2
    names the second display; a None in cfg drops that key. extras:
    the launch intent's string extras; with ROM in them the launch
    itself starts the content. menu_only: no content, the menu alone."""
    keep_alone()
    sh('am force-stop %s' % PKG)
    ds = displays()
    sid = find_second(ds, run.probe)
    if sid is not None:
        run.ordinal = ordinal(ds, sid)
    c = base_cfg()
    c['video_driver'] = run.driver
    c['video_threaded'] = 'true' if run.threaded else 'false'
    c['video_screen2_display'] = display_key(ds, sid) if sid is not None else ''
    merged_cfg(c, cfg)
    o = {'video_views_test_map': 'ds'}
    o.update(opts or {})
    push_text(DEV + '/retroarch.cfg', cfg_text(c))
    push_text(DEV + '/core-options.cfg', cfg_text(o))
    adb('logcat', '-c', check=False)
    # remade(): the events since this launch
    adb('logcat', '-b', 'events', '-c', check=False)
    launched = bool(extras and 'ROM' in extras)
    sh('am start -W%s -n %s -e CONFIGFILE %s/retroarch.cfg%s'
       % (' --display %d' % on if on is not None else '', ACTIVITY, DEV,
          ''.join(' -e %s %s' % kv for kv in sorted((extras or {}).items()))))
    wait_log(r'\[NetCMD\] .*\b%d\b' % PORT, 30,
             what='no network commands: is %s/retroarch.cfg read?' % DEV)
    if menu_only:
        check_driver(run)
        return
    if launched:
        pass
    elif content:
        ra_cmd('LOAD_CONTENT %s/%s|%s' % (CORES, core, content))
    else:
        ra_cmd('LOAD_CORE %s/%s' % (CORES, core))
        time.sleep(1.0)
        ra_cmd('START_CORE')
    try:
        wait_log(r'\[Core\] Loading dynamic libretro core from: ".*%s"'
                 % re.escape(core), 20)
    except Fail:
        if content or launched:
            raise
        ra_cmd('START_CORE')
        wait_log(r'\[Core\] Loading dynamic libretro core from: ".*%s"'
                 % re.escape(core), 20, what='the core did not start')
    if ready:
        wait_log(ready, 20, what='%s started without its views' % core)
    check_driver(run)


def check_driver(run, timeout=10):
    """The run's video driver came up; Skip when Vulkan fell back to
    gl."""
    def seen():
        lines = log_lines()
        if any(('[Vulkan] Found GPU #' if run.driver == 'vulkan'
                else '[GL] Vendor:') in l for l in lines):
            return 'ok'
        if run.driver == 'vulkan' and any('[GL] Vendor:' in l
                                          for l in lines):
            return 'gl'
        return None
    try:
        got = wait_until(seen, timeout, 'no driver line')
    except Fail:
        raise Fail('no "[Vulkan] Found GPU" line' if run.driver == 'vulkan'
                   else 'no "[GL] Vendor:" line')
    if got == 'gl':
        raise Skip('no Vulkan here: RetroArch fell back to gl')


def merged_cfg(c, cfg):
    """c with cfg's keys set; a None value drops the key."""
    for k, v in (cfg or {}).items():
        if v is None:
            c.pop(k, None)
        else:
            c[k] = v
    return c


def start_core(core=TEST_CORE, ready=READY):
    """The core started from the running menu, as start() starts it."""
    tag = mark()
    ra_cmd('LOAD_CORE %s/%s' % (CORES, core))
    time.sleep(1.0)
    ra_cmd('START_CORE')
    loaded = (r'\[Core\] Loading dynamic libretro core from: ".*%s"'
              % re.escape(core))
    try:
        wait_log(loaded, 20, tag=tag)
    except Fail:
        ra_cmd('START_CORE')
        wait_log(loaded, 20, tag=tag, what='the core did not start')
    if ready:
        wait_log(ready, 20, tag=tag,
                 what='%s started without its views' % core)


def frac_point(box, fx, fy):
    x0, y0, x1, y1 = box
    return (int(x0 + fx * (x1 - x0)), int(y0 + fy * (y1 - y0)))


def ra_display(run):
    """The display RetroArch's Activity is on: run.main unless placement
    moved it."""
    on = [d for d, t in app_windows().items() if any(ours(x) for x in t)]
    return on[0] if len(on) == 1 else run.main


def held_tap(run, did, x, y, ms=300):
    """A touch held for ms: a plain tap's down and up can land in one
    input poll, and the core reads the pointer once a frame."""
    require_front(ra_display(run))
    sh('input -d %d swipe %d %d %d %d %d' % (did, x, y, x, y, ms))


def pointers(lines, which=''):
    return [(int(m.group(4)), int(m.group(5)))
            for m in (POINTER.search(l) for l in lines)
            if m and m.group(1) == which]


def tap_check(run, did, box, view, fx, fy, tol=3):
    x, y = frac_point(box, fx, fy)
    ex, ey = view[0] + fx * view[2], view[1] + fy * view[3]
    tag = mark()
    held_tap(run, did, x, y)
    got = wait_until(lambda: pointers(lines_after(tag)), 5,
                     'touch (%d,%d) on display %d: the core saw no pointer'
                     % (x, y, did), every=0.3)[0]
    if abs(got[0] - ex) > tol or abs(got[1] - ey) > tol:
        raise Fail('touch (%d,%d) on display %d: packed %s, expected '
                   '(%.0f,%.0f)' % (x, y, did, got, ex, ey))
    return got


def touch_points(run, did, box, view):
    for fx, fy in ((0.25, 0.25), (0.75, 0.5), (0.5, 0.85)):
        tap_check(run, did, box, view, fx, fy)


def tap_outside(run, did, img, box):
    """A touch in the letterbox, where there is one: the core sees none."""
    x0, y0, x1, y1 = box
    spots = []
    if y0 >= 40:
        spots.append(((x0 + x1) // 2, y0 // 2))
    if img.h - y1 >= 40:
        spots.append(((x0 + x1) // 2, (y1 + img.h) // 2))
    if x0 >= 40:
        spots.append((x0 // 2, (y0 + y1) // 2))
    if img.w - x1 >= 40:
        spots.append(((x1 + img.w) // 2, (y0 + y1) // 2))
    if not spots:
        return False
    x, y = spots[0]
    tag = mark()
    held_tap(run, did, x, y)
    time.sleep(1.5)
    hits = pointers(lines_after(tag))
    if hits:
        raise Fail('touch (%d,%d) outside the screen on display %d reached '
                   'the core at %s' % (x, y, did, hits[0]))
    return True


def check_routed(run, label, second=YELLOW, main=GREEN, view=None,
                 timeout=15):
    """The second display shows the second colour's screen, fitted with
    its marker top-left, and the main display the other screen only
    (main None: anything but the second screen)."""
    view = view or VIEWS['ds'][1]

    def routed():
        ds = displays()
        sid = find_second(ds, run.probe)
        if sid is not None and sid in app_windows():
            return (sid, ds)
        return None
    sid, ds = wait_until(routed, timeout,
                         '%s: no RetroArch window on the second display'
                         % label)

    def shown():
        img = run.capture(sid, ds, label + '-second')
        b = img.box(second)
        return (img, b) if b else None
    img2, box2 = wait_until(shown, 10, '%s: display %d shows no %s screen'
                            % (label, sid, NAMES[second]))
    if main and img2.box(main):
        raise Fail('%s: display %d shows the %s screen too'
                   % (label, sid, NAMES[main]))
    got = float(box2[2] - box2[0]) / (box2[3] - box2[1])
    want = float(view[2]) / view[3]
    if abs(got / want - 1.0) > 0.05:
        raise Fail('%s: the screen on display %d is %dx%d, not %d:%d'
                   % (label, sid, box2[2] - box2[0], box2[3] - box2[1],
                      view[2], view[3]))
    mx = box2[0] + max(2, (box2[2] - box2[0]) // 100)
    my = box2[1] + max(2, (box2[3] - box2[1]) // 100)
    if not img2.near(mx, my, WHITE):
        raise Fail('%s: no white marker top-left of the screen on display '
                   '%d: rotated or mirrored?' % (label, sid))
    img0 = run.capture(run.main, ds, label + '-main')
    box0 = img0.box(main) if main else None
    if main and not box0:
        raise Fail('%s: the main display shows no %s screen'
                   % (label, NAMES[main]))
    if img0.box(second):
        raise Fail('%s: the main display still shows the %s screen'
                   % (label, NAMES[second]))
    return sid, img2, box2, img0, box0


def expect_attached(run, label, settle=3.0):
    """Both screens on the main display, stacked; the second display not
    taken."""
    time.sleep(settle)
    ds = displays()
    sid = find_second(ds, run.probe)
    if sid is not None and sid in app_windows():
        raise Fail('%s: RetroArch took display %d' % (label, sid))
    img0 = run.capture(run.main, ds, label + '-main')
    g, y = img0.box(GREEN), img0.box(YELLOW)
    if not g or not y:
        raise Fail('%s: the main display does not show both screens '
                   '(green %s, yellow %s)' % (label, g, y))
    if g[3] > y[1] + 4:
        raise Fail('%s: the screens are not stacked top to bottom: %s %s'
                   % (label, g, y))
    if sid is not None:
        try:
            img2 = run.capture(sid, ds, label + '-second')
        except Fail as e:
            # the console takes no shot of an untrusted display
            if 'no answer' not in str(e):
                raise
            print('note %s: display %d not captured (%s); no RetroArch '
                  'window there' % (label, sid, e), flush=True)
            return img0, g, y
        if img2.box(YELLOW) or img2.box(GREEN):
            raise Fail('%s: display %d shows a screen' % (label, sid))
    return img0, g, y


def add_second():
    console('multidisplay add %d %d %d %d 0' % ((EMU_INDEX,) + EMU_SECOND))


def at_emu_size(d):
    """d is EMU_SECOND, as turned by its rotation."""
    w, h = EMU_SECOND[:2]
    return d['size'] == ([h, w] if d['rotation'] % 2 else [w, h])


def second_at_emu(run):
    ds = displays()
    sid = find_second(ds, run.probe)
    return sid is not None and at_emu_size(ds[sid])


def restore(run):
    """The emulator's second display back at its size, density and flags
    after any case."""
    if not is_emulator():
        return
    ds = displays()
    sid = find_second(ds, run.probe)
    flagged = (sid is not None
               and (ds[sid]['presentation']
                    != run.probe['second']['presentation']
                    or ds[sid]['density'] not in (None, EMU_SECOND[2])))
    if flagged:
        console('multidisplay del %d' % EMU_INDEX)
        wait_until(lambda: find_second(displays(), run.probe) is None, 15,
                   'the second display is still listed after multidisplay '
                   'del')
    if flagged or sid is None or not at_emu_size(ds[sid]):
        add_second()
        wait_until(lambda: second_at_emu(run), 20,
                   'the second display did not come back at %dx%d'
                   % EMU_SECOND[:2])


# ---- cases ----

CASES = []


def case(name, emulator_only=False, driver=None, info=False, allow=None):
    """allow: a pattern for the warnings the case means to cause."""
    def deco(fn):
        CASES.append({'name': name, 'fn': fn, 'emulator_only': emulator_only,
                      'driver': driver, 'info': info, 'allow': allow})
        return fn
    return deco


@case('picker')
def c_picker(run):
    start(run)
    sid, img2, box2, img0, box0 = check_routed(run, 'picker')
    return 'display %d (ordinal %d): screen %dx%d at (%d,%d)' % (
        sid, run.ordinal, box2[2] - box2[0], box2[3] - box2[1],
        box2[0], box2[1])


@case('touch')
def c_touch(run):
    start(run)
    sid, img2, box2, img0, box0 = check_routed(run, 'touch')
    touch_points(run, sid, box2, VIEWS['ds'][1])
    top, cur = focus()
    if top != run.main or not any(ours(c) for c in cur):
        raise Fail('touching display %d moved the focus from RetroArch\'s '
                   'window (top display %s, %s)' % (sid, top, cur))
    outside = tap_outside(run, sid, img2, box2)
    touch_points(run, run.main, box0, VIEWS['ds'][0])
    return 'letterbox touch checked' if outside else 'no letterbox to touch'


def pad_masks(tag):
    """Port 0's pad masks the core logged since tag, in order."""
    out = []
    for line in lines_after(tag):
        m = PAD.search(line)
        if m and m.group(1) == '0':
            out.append(int(m.group(2), 16))
    return out


def hold_method(sdk, emulator, usage):
    """How a target holds X across polls, from a device native input
    takes as a keyboard (one of unknown source becomes a pad). usage is
    input's keyevent line: API 33 has no --duration and would send
    "--duration" and "400" as keys, but its --longpress holds."""
    if sdk >= 31:
        return 'keyboard' if '--duration' in usage else 'longpress'
    if emulator:
        return 'sendevent'
    raise Skip('API %d: no input --duration, and sendevent needs an '
               'emulator\'s adb root; X cannot be held' % sdk)


def qwerty2_node(text):
    """The emulator keyboard's node in getevent -p's listing."""
    for block in re.split(r'\n(?=add device)', text):
        m = re.match(r'add device \d+: (\S+)\s+name:\s+"qwerty2"', block)
        if m:
            return m.group(1)
    return None


def adb_root():
    """adbd as root: SELinux denies the shell user sendevent."""
    if not is_emulator():
        raise Fail('%s is not an emulator: no adb root' % SERIAL)
    if sh('id -u').strip() == '0':
        return
    out = adb('root', check=False).strip()

    def rooted():
        try:
            return sh('id -u', check=False, timeout=10).strip() == '0'
        except subprocess.TimeoutExpired:
            return False
    wait_until(rooted, 30, 'adb root did not take: %s' % out)


def hold_setup():
    """The shell command that holds X about 0.4 s, and its name."""
    usage = [l for l in sh('input 2>&1', check=False).splitlines()
             if ' keyevent ' in l]
    method = hold_method(int(sh('getprop ro.build.version.sdk').strip()),
                         is_emulator(), usage[0] if usage else '')
    if method == 'keyboard':
        return ('input keyboard keyevent --duration 400 KEYCODE_X',
                'input keyboard keyevent --duration 400')
    if method == 'longpress':
        return ('input keyboard keyevent --longpress KEYCODE_X',
                'input keyboard keyevent --longpress')
    adb_root()
    node = qwerty2_node(sh('getevent -p'))
    if node is None:
        raise Fail('no qwerty2 in getevent -p')
    return ('sendevent {0} 1 45 1; sendevent {0} 0 0 0; sleep 0.4; '
            'sendevent {0} 1 45 0; sendevent {0} 0 0 0'.format(node),
            'sendevent on qwerty2 %s, 0.4 s' % node)


def press_x(what, hold):
    """X held (the keyboard's RetroPad A): the core logs A down, then
    up."""
    tag = mark()
    sh(hold[0])
    wait_until(lambda: 0x100 in pad_masks(tag) and pad_masks(tag)[-1] == 0,
               6, '%s: the core saw no A press and release (%s)'
               % (what, hold[1]), every=0.3)


@case('keys_second')
def c_keys_second(run):
    hold = hold_setup()
    start(run)
    sid, img2, box2, img0, box0 = check_routed(run, 'keys-second')
    # Native input drops a keyboard's first key: it only registers it
    require_front(run.main)
    sh(hold[0])
    time.sleep(1)
    x2, y2 = frac_point(box2, 0.5, 0.5)
    held_tap(run, sid, x2, y2)
    top, cur = focus()
    if top != run.main or not any(ours(c) for c in cur):
        raise Fail('a tap on display %d moved the keys to display %s, %s'
                   % (sid, top, cur))
    press_x('X after a tap on the Presentation', hold)
    x0, y0 = frac_point(box0, 0.5, 0.5)
    held_tap(run, run.main, x0, y0)
    top, cur = focus()
    if top != run.main:
        raise Fail('a tap on display %d left the keys at display %s, %s'
                   % (run.main, top, cur))
    press_x('X at the Activity', hold)
    return 'keys stay at display %d after a tap on display %d (%s)' % (
        run.main, sid, hold[1])


@case('two_fingers')
def c_two_fingers(run):
    start(run)
    sid, img2, box2, img0, box0 = check_routed(run, 'two-fingers')
    x2, y2 = frac_point(box2, 0.5, 0.5)
    x0, y0 = frac_point(box0, 0.5, 0.5)
    require_front(run.main)
    tag = mark()
    sh('input -d %d motionevent DOWN %d %d' % (sid, x2, y2))
    try:
        time.sleep(0.5)
        sh('input -d %d motionevent DOWN %d %d' % (run.main, x0, y0))
        time.sleep(1.0)
        with open(os.path.join(run.dir, 'two-fingers-input.txt'), 'w') as f:
            f.write(sh('dumpsys input', check=False))
        lines = lines_after(tag)
    finally:
        sh('input -d %d motionevent UP %d %d' % (run.main, x0, y0),
           check=False)
        sh('input -d %d motionevent UP %d %d' % (sid, x2, y2), check=False)
    first, second = pointers(lines), pointers(lines, '1')
    if not second:
        raise Fail('with a finger on each display the core saw no second '
                   'pointer (two-fingers-input.txt shows whether Android '
                   'kept both down)')
    if (abs(first[-1][0] - 128) > 4 or abs(first[-1][1] - 96) > 4
            or abs(second[-1][0] - 128) > 4 or abs(second[-1][1] - 288) > 4):
        raise Fail('pointer 0 at %s and pointer 1 at %s; expected the main '
                   'display first (128,96), then (128,288)'
                   % (first[-1], second[-1]))


@case('menu')
def c_menu(run):
    start(run)
    sid, img2, box2, img0, box0 = check_routed(run, 'menu-before')
    area = (box0[2] - box0[0]) * (box0[3] - box0[1])
    ra_cmd('MENU open', wait=3)
    try:
        def opened():
            b = run.capture(run.main, displays(), 'menu-main').box(GREEN)
            return b is None or (b[2] - b[0]) * (b[3] - b[1]) < area // 2
        wait_until(opened, 10, 'the menu did not open on the main display')
        time.sleep(1.0)
        if sid not in app_windows():
            raise Fail('display %d let RetroArch go under the menu' % sid)
        b = run.capture(sid, displays(), 'menu-second').box(YELLOW)
        if not b or max(abs(b[i] - box2[i]) for i in range(4)) > 4:
            raise Fail('display %d did not keep its last image under the '
                       'menu: %s, was %s' % (sid, b, box2))
    finally:
        ra_cmd('MENU close', wait=3)
    sid, img2, box2, img0, box0 = check_routed(run, 'menu-after')
    touch_points(run, sid, box2, VIEWS['ds'][1])


@case('close_content')
def c_close_content(run):
    start(run)
    sid = check_routed(run, 'close-before')[0]
    ra_cmd('CLOSE_CONTENT')
    wait_until(lambda: sid not in app_windows(), 10,
               'display %d kept the RetroArch window after CLOSE_CONTENT'
               % sid)
    tag = mark()
    ra_cmd('LOAD_CORE %s/%s' % (CORES, TEST_CORE))
    time.sleep(1.0)
    ra_cmd('START_CORE')
    wait_log(r'\[Core\] Loading dynamic libretro core from', 20, tag=tag)
    check_routed(run, 'close-after')


# ---- part 01: the focus after the Presentation goes ----

# VIRTUAL_DISPLAY_FLAG_*: PUBLIC | PRESENTATION | SUPPORTS_TOUCH |
# SHOULD_SHOW_SYSTEM_DECORATIONS | TRUSTED: a launcher of its own under
# the Presentation, as the Thor's bottom panel has Cocoon's
HOME_FLAGS = 1 | 2 | 64 | 512 | 1024
SELECTED = re.compile(r'^SELECTED (\d+)\s*$', re.M)
FOCUS_DUMP = re.compile(r'mTopFocusedDisplayId|mCurrentFocus|mFocusedApp'
                        r'|Display: mDisplayId')
INPUT_FOCUS = re.compile(r'FocusedDisplayId|FocusedApplications'
                         r'|FocusedWindows')
FOCUS_EVENTS = re.compile(r'\b(wm_focused_\w+|wm_task_to_front'
                          r'|wm_task_moved|input_focus)\b')


def selected_row(text):
    """A MENU reply's selected row; None while the menu is closed."""
    m = SELECTED.search(text)
    return int(m.group(1)) if m else None


def menu_selected():
    return selected_row(ra_cmd('MENU show', wait=3))


def held_key(hold, keycode, scancode):
    """hold_setup()'s hold of X, as another key: keycode for input,
    scancode for sendevent."""
    return (hold[0].replace('KEYCODE_X', keycode)
            .replace(' 1 45 ', ' 1 %d ' % scancode))


def display_windows(did):
    """Titles of every window on display did, any package."""
    out = []
    for b in re.split(r'\n(?=\s*Window #\d+ )',
                      sh('dumpsys window windows')):
        if _first(r'mDisplayId=(\d+)', b, None, int) == did:
            out.append(_first(r'Window #\d+ Window\{\S+ \S+ ([^}]*)\}', b,
                              '?'))
    return out


def focus_evidence(run, name, notes):
    """Where Android has the focus, and how it got there."""
    out = list(notes) + ['']
    out += [l for l in sh('dumpsys window', check=False).splitlines()
            if FOCUS_DUMP.search(l)]
    out.append('')
    lines = sh('dumpsys input', check=False).splitlines()
    for i, l in enumerate(lines):
        if INPUT_FOCUS.search(l):
            out += lines[i:i + 4]
    out.append('')
    out += [l for l in adb('logcat', '-d', '-b', 'events', '-v',
                           'threadtime', check=False).splitlines()
            if FOCUS_EVENTS.search(l)]
    out.append('')
    out += [l for l in log_lines() if '[SecondDisplay]' in l]
    with open(os.path.join(run.dir, name), 'w') as f:
        f.write('\n'.join(out) + '\n')


def focus_after_dismiss(run, hold):
    start(run)
    sid, img2, box2, img0, box0 = check_routed(run, 'focus-dismiss')
    under = [t for t in display_windows(sid) if not mentions(t)]
    # Native input drops a keyboard's first key: it only registers it
    require_front(run.main)
    sh(hold[0])
    time.sleep(1)
    x2, y2 = frac_point(box2, 0.5, 0.5)
    require_front(run.main)
    sh('input -d %d tap %d %d' % (sid, x2, y2))
    time.sleep(1.0)
    tapped = focus()
    if os.environ.get('RA_F1_VIA_MENU') == '1':
        rows = menu('open')
        menu('a', menu_pick(rows, r'^Close Content$', 'quick menu')[0])
    else:
        ra_cmd('CLOSE_CONTENT')
    wait_until(lambda: sid not in app_windows(), 10,
               'display %d kept the RetroArch window after closing content'
               % sid)
    wait_until(lambda: menu_selected() is not None, 10,
               'no menu after closing content')
    time.sleep(1.0)
    top, cur = focus()
    focus_evidence(run, 'focus-after-dismiss.txt', [
        'windows under the Presentation on display %d: %s' % (sid, under),
        'after the tap on display %d: top display %s, %s'
        % (sid, tapped[0], tapped[1]),
        'after closing content: top display %s, %s' % (top, cur)])
    # No key is sent unless RetroArch has the focus
    if top != run.main or not any(ours(c) for c in cur):
        raise Fail('after a tap on display %d and closing content the keys '
                   'are at display %s, %s (after the tap: display %s, %s)'
                   % (sid, top, cur, tapped[0], tapped[1]))
    before = menu_selected()

    def moved():
        s = menu_selected()
        return (s,) if s is not None and s != before else None
    sh(held_key(hold, 'KEYCODE_DPAD_DOWN', 108))
    after = wait_until(moved, 6, 'a Down after closing content did not '
                       'move RetroArch\'s menu from row %s (%s)'
                       % (before, hold[1]), every=0.5)[0]
    return ('after the tap the keys were at display %s; after closing '
            'content at display %d, and a Down moved the menu from row %s '
            'to %d' % (tapped[0], top, before, after))


@case('focus_after_dismiss')
def c_focus_after_dismiss(run):
    """Finding F1: a tap on the Presentation, then the content closed;
    the keys must stay with RetroArch."""
    hold = hold_setup()
    home = is_emulator() and os.environ.get('RA_F1_HOME') == '1'
    if home:
        replace_second(run, HOME_FLAGS)
    try:
        return focus_after_dismiss(run, hold)
    finally:
        if home:
            replace_second(run, 0)


@case('focus_mode', info=True)
def c_focus_mode(run):
    """AYN's Focus Mode, set on the Thor by hand first (RA_FOCUS_MODE
    names it): with a two-screen game and no tap the keys reach the
    core; where they go after the close is recorded. RA_FOCUS_PAD=1
    also waits 30 s for a button from the real pad."""
    mode = os.environ.get('RA_FOCUS_MODE', '')
    if not mode:
        raise Skip('RA_FOCUS_MODE not set')
    hold = hold_setup()
    start(run)
    sid, img2, box2, img0, box0 = check_routed(run, 'focus-mode')
    time.sleep(1.0)
    top, cur = focus()
    notes = ['focus mode %s, game running: top display %s, %s; on '
             'display %d: %s' % (mode, top, cur, sid, display_windows(sid))]
    mine = ((top == run.main and any(ours(c) for c in cur))
            or (top == sid and any(ours_second(c) for c in cur)))
    # No key is sent unless RetroArch has the focus
    if not mine:
        focus_evidence(run, 'focus-mode.txt', notes)
        raise Fail('focus mode %s: with the game running the keys are at '
                   'display %s, %s, not RetroArch' % (mode, top, cur))
    # Native input drops a keyboard's first key: it only registers it
    sh(hold[0])
    time.sleep(1)
    press_x('X in focus mode %s' % mode, hold)
    if os.environ.get('RA_FOCUS_PAD') == '1':
        tag = mark()
        wait_until(lambda: any(pad_masks(tag)), 30,
                   'focus mode %s: no button from the pad in 30 s' % mode,
                   every=0.5)
    ra_cmd('CLOSE_CONTENT')
    wait_until(lambda: sid not in app_windows(), 10,
               'display %d kept the RetroArch window after closing content'
               % sid)
    time.sleep(1.0)
    after = focus()
    notes.append('after closing content: top display %s, %s' % after)
    focus_evidence(run, 'focus-mode.txt', notes)
    return ('focus mode %s: keys at display %s reach the core; after the '
            'close they are at display %s' % (mode, top, after[0]))


@case('background')
def c_background(run):
    start(run)
    sid = check_routed(run, 'background-before')[0]
    require_front(run.main)
    sh('input keyevent KEYCODE_HOME')
    wait_until(lambda: sid not in app_windows(), 10,
               'display %d kept the RetroArch window after HOME' % sid)
    time.sleep(2.0)
    if sid in app_windows():
        raise Fail('the RetroArch window came back on display %d in the '
                   'background' % sid)
    if run.capture(sid, displays(), 'background-away').box(YELLOW):
        raise Fail('display %d shows the bottom screen in the background'
                   % sid)
    if not alive():
        raise Fail('RetroArch died in the background')
    sh('am start -n %s -a android.intent.action.MAIN '
       '-c android.intent.category.LAUNCHER' % LAUNCHER)
    sid, img2, box2, img0, box0 = check_routed(run, 'background-after')
    touch_points(run, sid, box2, VIEWS['ds'][1])


@case('remove_add', emulator_only=True, allow=GONE)
def c_remove_add(run):
    start(run)
    sid = check_routed(run, 'remove-before')[0]
    console('multidisplay del %d' % EMU_INDEX)
    wait_until(lambda: find_second(displays(), run.probe) is None, 15,
               'the second display is still listed after multidisplay del')

    def both():
        try:
            return expect_attached(run, 'removed', settle=0.5)
        except Fail:
            return None
    wait_until(both, 15, 'the screens did not come back to the main display')
    if not alive():
        raise Fail('RetroArch died when the display went away')
    add_second()
    wait_until(lambda: find_second(displays(), run.probe) is not None, 20,
               'the second display is not listed after multidisplay add')
    refocused = refocus_after_add(run.main)
    sid2, img2, box2, img0, box0 = check_routed(run, 'remove-after',
                                                timeout=30)
    touch_points(run, sid2, box2, VIEWS['ds'][1])
    return 'display %d went away and came back as %d%s' % (
        sid, sid2, '; it took the focus, so RetroArch was brought to the '
        'front again' if refocused else '')


@case('resize', emulator_only=True)
def c_resize(run):
    w, h, dpi = EMU_SECOND
    # From any other size the resize below can be a no-op
    restore(run)
    ds = displays()
    sid = find_second(ds, run.probe)
    if sid is None or ds[sid]['size'] != [w, h]:
        raise Fail('the second display is not %dx%d before the resize: %s'
                   % (w, h, ds[sid]['size'] if sid is not None else 'gone'))
    start(run)
    check_routed(run, 'resize-before')

    def resized():
        ds = displays()
        sid = find_second(ds, run.probe)
        return sid is not None and ds[sid]['size'] == [h, w]
    try:
        console('multidisplay add %d %d %d %d 0' % (EMU_INDEX, h, w, dpi))
        wait_until(resized, 20, 'the second display did not become %dx%d'
                   % (h, w))
        time.sleep(1.0)
        sid, img2, box2, img0, box0 = check_routed(run, 'resize-after',
                                                   timeout=20)
        if img2.w != h or box2[2] - box2[0] > h:
            raise Fail('the screen was not fitted again: %dx%d capture, '
                       'box %s' % (img2.w, img2.h, box2))
        touch_points(run, sid, box2, VIEWS['ds'][1])
    finally:
        add_second()
    wait_until(lambda: second_at_emu(run), 20,
               'the second display did not go back to %dx%d' % (w, h))
    return 'resized to %dx%d and back to %dx%d' % (h, w, w, h)


def user_rotation_held(did, r, timeout=10):
    """The window manager reads back user rotation lock r on display did;
    Skip when it keeps its own (the Thor puts persist.panel.orientation
    back on display 0)."""
    got = ['']

    def held():
        got[0] = sh('cmd window user-rotation -d %d' % did,
                    check=False).strip()
        return got[0].split() == ['lock', str(r)]
    try:
        wait_until(held, timeout, '')
    except Fail:
        raise Skip('display %d keeps its user rotation: lock %d reads back '
                   'as "%s"' % (did, r, got[0]))


@case('rotate')
def c_rotate(run):
    start(run)
    check_routed(run, 'rotate-before')
    m = run.main
    cur = sh('cmd window user-rotation -d %d' % m, check=False).split()
    was = sh('settings get system user_rotation', check=False).strip()
    r0 = displays()[m]['rotation']
    r1 = (r0 + 1) % 4
    sh('cmd window user-rotation -d %d lock %d' % (m, r1))
    try:
        user_rotation_held(m, r1)
        wait_until(lambda: displays()[m]['rotation'] == r1, 15,
                   'the main display did not rotate to %d' % r1)
        time.sleep(1.5)
        sid, img2, box2, img0, box0 = check_routed(run, 'rotate-after')
        touch_points(run, sid, box2, VIEWS['ds'][1])
        touch_points(run, m, box0, VIEWS['ds'][0])
    finally:
        if cur == ['free']:
            # no lock first: SystemUI locks again as the display turns back
            sh('cmd window user-rotation -d %d free' % m, check=False)
            if m == 0 and was.isdigit():
                sh('settings put system user_rotation %s' % was, check=False)
        elif len(cur) >= 2 and cur[0] in ('lock', 'free'):
            sh('cmd window user-rotation -d %d %s %s' % (m, cur[0], cur[1]),
               check=False)
        else:
            sh('cmd window user-rotation -d %d lock %d' % (m, r0),
               check=False)


# Screen Orientation's values as RetroActivityCommon.setScreenOrientation
# asks for them, by natural orientation (ActivityInfo: LANDSCAPE 0,
# PORTRAIT 1, REVERSE_LANDSCAPE 8, REVERSE_PORTRAIT 9), and the rotation
# AOSP's window manager gives each
ASKS = {'portrait': (1, 0, 9, 8), 'landscape': (0, 9, 8, 1)}
TURNS = {'portrait': {1: 0, 0: 1, 9: 2, 8: 3},
         'landscape': {0: 0, 9: 1, 8: 2, 1: 3}}
ORIENTATIONS = ('Normal', '90 deg', '180 deg', '270 deg')


def natural(d):
    """Display d unturned: 'portrait' or 'landscape'."""
    w, h = d['size'] if d['rotation'] % 2 == 0 else d['size'][::-1]
    return 'portrait' if w < h else 'landscape'


def orientation_choice(nat, r0):
    """The Screen Orientation value that turns a display of natural
    orientation nat a quarter turn from rotation r0, what it asks for and
    the rotation that gives. Never 0: driver init asks for nothing then."""
    for r in ((r0 + 1) % 4, (r0 + 3) % 4):
        for v in (1, 2, 3):
            if TURNS[nat][ASKS[nat][v]] == r:
                return v, ASKS[nat][v], r
    raise Fail('no Screen Orientation turns a %s display from rotation %d'
               % (nat, r0))


def set_orientation(value):
    """Settings > Video > Output > Screen Orientation to value through
    MENU, read back; the menu closed after."""
    want = ORIENTATIONS[value]
    rows = main_menu()
    for rx in (r'^Settings$', r'^Video$', r'^Output$'):
        rows = menu('a', menu_pick(rows, rx, 'menu')[0])
    i, got = menu_pick(rows, r'^Screen Orientation$', 'Video > Output')
    for _ in ORIENTATIONS:
        if got == want:
            break
        i, got = menu_pick(menu('right', i), r'^Screen Orientation$',
                           'Video > Output')
    menu('close')
    if got != want:
        raise Fail('Screen Orientation shows %r, not %r' % (got, want))


def main_settled(run, label, timeout=30):
    """The main display presents screen 0 at its shape, marker top-left,
    in a capture its new size, the same twice running: the turn is over
    and RetroArch drew at the new size."""
    view = VIEWS['ds'][0]
    last = [None]
    why = ['no capture']

    def settled():
        ds = displays()
        size = ds[run.main]['size']
        img = run.capture(run.main, ds, label)
        b = img.box(GREEN)
        now = None
        if [img.w, img.h] != size:
            why[0] = 'a %dx%d capture of a %dx%d display' % (
                img.w, img.h, size[0], size[1])
        elif not b:
            why[0] = 'no %s screen' % NAMES[GREEN]
        elif abs(float(b[2] - b[0]) / (b[3] - b[1])
                 / (float(view[2]) / view[3]) - 1.0) > 0.05:
            why[0] = 'the screen at %dx%d, not %d:%d' % (
                b[2] - b[0], b[3] - b[1], view[2], view[3])
        elif not img.near(b[0] + max(2, (b[2] - b[0]) // 100),
                          b[1] + max(2, (b[3] - b[1]) // 100), WHITE):
            why[0] = 'no white marker top-left of the screen'
        else:
            now = (img.w, img.h, b)
            why[0] = 'the screen moved between captures'
        same = now is not None and now == last[0]
        last[0] = now
        return same
    try:
        wait_until(settled, timeout, '')
    except Fail:
        raise Fail('%s: the main display did not settle in %d s: %s'
                   % (label, timeout, why[0]))


def orientation_back(run, r0):
    """Screen Orientation back to Normal and RetroArch stopped: only the
    stop drops its request (deinit asks for Normal, a fixed orientation
    on Android). Whether the main display is back at r0."""
    try:
        if alive():
            set_orientation(0)
    except (Fail, subprocess.TimeoutExpired):
        pass
    sh('am force-stop %s' % PKG, check=False)
    try:
        wait_until(lambda: displays()[run.main]['rotation'] == r0, 20, '')
        return True
    except (Fail, subprocess.TimeoutExpired):
        return False


@case('rotate_reinit')
def c_rotate_reinit(run):
    start(run)
    check_routed(run, 'rotate-reinit-before')
    m = run.main
    d = displays()[m]
    r0 = d['rotation']
    value, asked, r1 = orientation_choice(natural(d), r0)
    try:
        set_orientation(value)
        tag = mark()
        ra_cmd('REINIT video', wait=3)
        wait_log(r'\[Video\] Setting screen orientation to %d\.' % value, 30,
                 tag=tag, what='REINIT video asked for no Screen '
                 'Orientation %d' % value)
        try:
            wait_until(lambda: displays()[m]['rotation'] != r0, 20, '')
        except Fail:
            raise Skip('display %d did not turn: still at rotation %d 20 s '
                       'after RetroArch asked for Screen Orientation %s '
                       '(orientation %d)' % (m, r0, ORIENTATIONS[value],
                                             asked))
        main_settled(run, 'rotate-reinit-settle')
        sid, img2, box2, img0, box0 = check_routed(run, 'rotate-reinit-after',
                                                   timeout=20)
        touch_points(run, sid, box2, VIEWS['ds'][1])
        touch_points(run, m, box0, VIEWS['ds'][0])
        d1 = displays()[m]
    finally:
        back = orientation_back(run, r0)
    if not back:
        raise Fail('display %d did not go back to rotation %d once '
                   'RetroArch stopped' % (m, r0))
    return ('Screen Orientation %s: display %d turned from rotation %d to '
            '%d (%dx%d; %d expected)' % (ORIENTATIONS[value], m, r0,
                                         d1['rotation'], d1['size'][0],
                                         d1['size'][1], r1))


@case('auto')
def c_auto(run):
    start(run, cfg={'video_screen2_display': ''})
    return auto_routed(run, 'auto')


@case('off')
def c_off(run):
    start(run, cfg={'video_windows_enable': 'false'})
    expect_attached(run, 'off')


@case('modes')
def c_modes(run):
    """Game Screen 2 Display on the main display, then Off."""
    ds = displays()
    start(run, cfg={'video_screen2_display': display_key(ds, run.main)})
    expect_attached(run, 'attached')
    start(run, cfg={'video_screen2_display': 'off'})
    time.sleep(3.0)
    ds = displays()
    sid = find_second(ds, run.probe)
    if sid is not None and sid in app_windows():
        raise Fail('off: RetroArch took display %d' % sid)
    img0 = run.capture(run.main, ds, 'off-main')
    if not img0.box(GREEN) or img0.box(YELLOW):
        raise Fail('off: the main display should show screen 1 alone')
    return ('Screen 2 on display %d, then Off, kept display %s free'
            % (run.main, sid))


@case('map_3ds')
def c_map_3ds(run):
    start(run, opts={'video_views_test_map': '3ds'})
    sid, img2, box2, img0, box0 = check_routed(run, '3ds',
                                               view=VIEWS['3ds'][1])
    touch_points(run, sid, box2, VIEWS['3ds'][1])
    touch_points(run, run.main, box0, VIEWS['3ds'][0])


@case('vk_hw', driver='vulkan')
def c_vk_hw(run):
    start(run, opts={'video_views_test_hw': 'vulkan'})
    sid, img2, box2, img0, box0 = check_routed(run, 'vk-hw')
    touch_points(run, sid, box2, VIEWS['ds'][1])


@case('none', emulator_only=True)
def c_none(run):
    console('multidisplay del %d' % EMU_INDEX)
    wait_until(lambda: find_second(displays(), run.probe) is None, 15,
               'the second display is still listed after multidisplay del')
    start(run, cfg={'video_screen2_display': ''})
    img0, g, y = expect_attached(run, 'none')
    tap_check(run, run.main, g, VIEWS['ds'][0], 0.5, 0.5)
    tap_check(run, run.main, y, VIEWS['ds'][1], 0.5, 0.5)
    if not alive():
        raise Fail('RetroArch is not running')


@case('osd')
def c_osd(run):
    start(run, cfg={'video_font_enable': 'true', 'menu_enable_widgets': 'true',
                    'fps_show': 'true', 'input_overlay_enable': 'true'})
    sid, img2, box2, img0, box0 = check_routed(run, 'osd-before', main=None)
    ra_cmd('SHOW_MSG ra-dual: the main display only')
    time.sleep(1.0)
    ds = displays()
    changed = run.capture(run.main, ds, 'osd-main').differs(img0)
    img = run.capture(sid, ds, 'osd-second')
    b = img.box(YELLOW)
    if not b or max(abs(b[i] - box2[i]) for i in range(4)) > 4:
        raise Fail('display %d changed under the message: %s, was %s'
                   % (sid, b, box2))
    if img.fraction(YELLOW, b) < 0.985:
        raise Fail('something is drawn over the screen on display %d' % sid)
    if changed < 0.001:
        raise Fail('the message did not show on the main display')


@case('shader')
def c_shader(run):
    preset, files, text = SHADERS[run.driver]
    src = os.path.join(TREE, 'samples/cores/video_views/e2e')
    sh('mkdir -p %s/shaders' % DEV)
    for f in files:
        adb('push', os.path.join(src, f), '%s/shaders/%s' % (DEV, f))
    if text:
        push_text('%s/shaders/%s' % (DEV, preset), text)
    start(run, cfg={'video_shader_enable': 'true'})
    tag = mark()
    ra_cmd('SET_SHADER %s/shaders/%s' % (DEV, preset))
    wait_log(r'\[Shaders\].*%s' % re.escape(preset), 10, tag=tag,
             what='%s was not applied' % preset)
    sid, img2, box2, img0, box0 = check_routed(run, 'shader')
    touch_points(run, sid, box2, VIEWS['ds'][1])
    if not is_emulator():
        return 'preset on both displays'
    w, h = 2560, 1600
    tag = mark()
    console('multidisplay add %d %d %d %d 0' % (EMU_INDEX, w, h,
                                                EMU_SECOND[2]))

    def grown():
        ds = displays()
        sid = find_second(ds, run.probe)
        return sid is not None and ds[sid]['size'] == [w, h]
    wait_until(grown, 20, 'the second display did not become %dx%d' % (w, h))
    n = 0
    if run.driver == 'gl':
        # Before the picture, so a failure tells "no FBO grown" from
        # "grown, then not drawn"
        n = wait_until(lambda: sum(1 for l in lines_after(tag)
                                   if 'Recreating FBO texture' in l), 10,
                       'a second display larger than the main one grew '
                       'no FBO')
    time.sleep(1.0)
    sid, img2, box2, img0, box0 = check_routed(run, 'shader-big',
                                               timeout=20)
    touch_points(run, sid, box2, VIEWS['ds'][1])
    if run.driver != 'gl':
        return 'preset on both displays, second larger than the main'
    return 'FBOs grown for the larger display (%d lines)' % n


@case('anaglyph')
def c_anaglyph(run):
    start(run, cfg={'video_stereo_mode': '4'},
          opts={'video_views_test_map': '3ds'})
    sid, img2, box2, img0, box0 = check_routed(run, 'anaglyph', main=None,
                                               view=VIEWS['3ds'][1])
    touch_points(run, sid, box2, VIEWS['3ds'][1])
    stereo = [l for l in log_lines() if '[video_views] presents=' in l]
    return stereo[-1].strip() if stereo else ''


@case('gl_hw', driver='gl')
def c_gl_hw(run):
    start(run, opts={'video_views_test_hw': 'gles3'})
    if any('GL entry points missing' in l for l in log_lines()):
        raise Fail('the core found no GLES 3 entry points')
    sid, img2, box2, img0, box0 = check_routed(run, 'gl-hw')
    touch_points(run, sid, box2, VIEWS['ds'][1])


# API 34+ lists layers as RequestedLayerState{NAME parentId=... z=...}
LAYER_STATE = re.compile(r'RequestedLayerState\{(.*?)(?: (?:parentId|'
                         r'relativeParentId|mirrorId|z|layerStack)=| !handle'
                         r'|\}$)')


def parse_layers(text):
    """PKG's main-window layer names in dumpsys SurfaceFlinger --list,
    BLAST first; leashes and activity records hold no frames."""
    names = []
    for l in text.splitlines():
        l = l.strip()
        m = LAYER_STATE.match(l)
        name = m.group(1) if m else l
        if ours(name) and not name.startswith(('Surface(name=',
                                               'ActivityRecord')):
            names.append(name)
    names.sort(key=lambda n: '(BLAST)' not in n)
    return names


def sf_frame_times(seconds=5.0):
    """Main-window present intervals from SurfaceFlinger's latency
    history: mean, median, p95, max in ms and how many ran over 1.5
    medians."""
    layers = parse_layers(sh('dumpsys SurfaceFlinger --list'))
    for layer in layers:
        sh('dumpsys SurfaceFlinger --latency-clear "%s"' % layer, check=False)
    time.sleep(seconds)
    for layer in layers:
        rows = sh('dumpsys SurfaceFlinger --latency "%s"' % layer,
                  check=False).split('\n')
        pres = []
        for r in rows[1:]:
            p = r.split()
            if len(p) == 3 and 0 < int(p[1]) < (1 << 62):
                pres.append(int(p[1]))
        d = sorted((b - a) / 1e6 for a, b in zip(pres, pres[1:]) if b > a)
        if len(d) >= 30:
            med = d[len(d) // 2]
            return {'layer': layer, 'frames': len(d),
                    'mean': sum(d) / len(d), 'median': med,
                    'p95': d[int(len(d) * 0.95) - 1], 'max': d[-1],
                    'slow': sum(1 for x in d if x > 1.5 * med)}
    raise Fail('no SurfaceFlinger latency history for %s' % layers)


def show_times(t):
    return ('%(frames)d frames: mean %(mean).2f median %(median).2f '
            'p95 %(p95).2f max %(max).2f ms, %(slow)d slow' % t)


@case('frametimes', info=True)
def c_frametimes(run):
    start(run)
    check_routed(run, 'frametimes-on')
    on = sf_frame_times()
    waits = sum(1 for l in adb('logcat', '-d').splitlines()
                if 'dequeueBuffer timed out' in l)
    start(run, cfg={'video_windows_enable': 'false'})
    expect_attached(run, 'frametimes-off', settle=1.0)
    off = sf_frame_times()
    with open(os.path.join(run.dir, 'frametimes.json'), 'w') as f:
        json.dump({'on': on, 'off': off, 'dequeue_timeouts': waits}, f)
    return 'on: %s | off: %s | dequeueBuffer timed out: %d' % (
        show_times(on), show_times(off), waits)


def frame_count():
    reply = ra_cmd('GET_OPTION frame_count')
    nums = re.findall(r'\d+', reply)
    return int(nums[-1]) if nums else None


@case('rate', info=True)
def c_rate(run):
    start(run)
    check_routed(run, 'rate')
    a = frame_count()
    if a is None:
        raise Skip('no answer to GET_OPTION through toybox nc')
    t = time.time()
    time.sleep(10)
    b = frame_count()
    return '%.1f frames/s with the second display' % ((b - a) / (time.time() - t))


# ---- B-assign: screens by display name, and RetroArch moving ----

NAME_SIZE = 64                  # VIDEO_WINDOW_DISPLAY_NAME_SIZE
WRONG = 'No Such Display'
REFUSALS = r'\[SecondDisplay\] Cannot (show on|move to) display'
# VIRTUAL_DISPLAY_FLAG_*: PUBLIC | OWN_CONTENT_ONLY | SUPPORTS_TOUCH,
# with no PRESENTATION; then also SHOULD_SHOW_SYSTEM_DECORATIONS | TRUSTED
UNTRUSTED = 1 | 8 | 64
TRUSTED = UNTRUSTED | 512 | 1024
MENU_ROW = re.compile(r'^(\d+)\t([^\t]*)\t([^\t]*)$')
EMU_THIRD = 2                   # the emulator's own number for display 3
ROM = DEV + '/roms/move.bin'    # content the test core takes and ignores
# logcat -v threadtime: date, time, pid, tid
THREADS = re.compile(r'^\S+\s+\S+\s+(\d+)\s+(\d+)\s')
# An Activity start as ActivityTaskManager logs it; API 33 on add the
# result, negative when the start threw
STARTED = re.compile(r'\bSTART u\d+ \{([^}]*)\}.*? from uid (\d+)'
                     r'(?:.*?result code=(-?\d+))?')
MOVE_FAILED = re.compile(r'\[SecondDisplay\] Cannot move to display (-?\d+)')
# The events log (am_ before API 29): an Activity made, sent a new
# intent, created, destroyed, relaunched
LIFE = re.compile(r'\b(?:wm|am)_(create_activity|new_intent|on_create_called'
                  r'|on_destroy_called|relaunch_resume_activity'
                  r'|relaunch_activity): \[([^\]]*)\]')
# The events whose third field is the task id
TASKED = ('create_activity', 'new_intent', 'relaunch_resume_activity',
          'relaunch_activity')


def display_key(ds, did):
    """Display did as a screen's display setting names it (00 P2): its
    name, " 2" or " 3" added for the second or third with that name in
    getDisplays() order."""
    ids = sorted(i for i, d in ds.items() if not d['private'])
    name = ds[did]['name']
    n = sum(1 for i in ids[:ids.index(did)] if ds[i]['name'] == name)
    return name if not n else '%s %d' % (name[:NAME_SIZE - 3], n + 1)


DISPLAY = 'Display: '           # a display entry's label starts with this


def shows(text, key):
    """A menu label or value naming display key, alone or with its model
    or size; or text is key itself (Auto, OFF or a whole label)."""
    return (text == key or text == DISPLAY + key
            or text.startswith(DISPLAY + key + ' ('))


def list_view(ds):
    """What a published list says of each display besides own and
    refused, as Android shows it: its key and whether it is on. Every
    display dumpsys lists is valid."""
    return [(display_key(ds, i), ds[i]['state'] == 'ON')
            for i in sorted(ds) if not ds[i]['private']]


def activity_on(did):
    """RetroArch's Activity window is on display did."""
    return any(ours(t) for t in app_windows().get(did, []))


def replace_second(run, flags, size=EMU_SECOND):
    """The emulator's second display made again with VIRTUAL_DISPLAY
    flags, and size (width, height, dpi): its id and record."""
    console('multidisplay del %d' % EMU_INDEX)
    wait_until(lambda: find_second(displays(), run.probe) is None, 15,
               'the second display is still listed after multidisplay del')
    console('multidisplay add %d %d %d %d %d'
            % ((EMU_INDEX,) + tuple(size) + (flags,)))
    sid = wait_until(lambda: find_second(displays(), run.probe), 20,
                     'no second display after multidisplay add with flags %d'
                     % flags)
    refocus_after_add(run.main)
    return sid, displays()[sid]


def menu(*words):
    """A MENU network command; the rows it answers with, as (index,
    label, value)."""
    text = ra_cmd('MENU ' + ' '.join(str(w) for w in words), wait=3)
    return [(int(m.group(1)), m.group(2), m.group(3))
            for m in (MENU_ROW.match(l) for l in text.splitlines()) if m]


def menu_top():
    """MENU open at the main menu: with content running it opens on the
    Quick Menu, one back from it."""
    rows = menu('open')
    if not any(r[1] == 'Settings' for r in rows):
        rows = menu('b')
    return rows


def menu_pick(rows, rx, what):
    for i, label, value in rows:
        if re.search(rx, label):
            return i, value
    raise Fail('%s: no row matches %r in %s' % (what, rx,
                                                 [r[1] for r in rows]))


def display_choices():
    """The displays the Game Screen 2 Display dropdown offers, from
    RetroArch's copy of the last published list, and the row's value."""
    rows = menu_top()
    try:
        for rx in (r'^Settings$', r'^Video$', r'^Stereo'):
            rows = menu('a', menu_pick(rows, rx, 'menu')[0])
        i, value = menu_pick(rows, r'^Game Screen 2 Display$', 'screens')
        keys = [r[1] for r in menu('a', i) if r[1] not in ('Auto', 'OFF')]
        menu('b')
    finally:
        menu('close')
    return keys, value


def started_on(run, did, other):
    """After start(on=did): the Presentation failed on other, so did was
    RetroArch's display; Skip when Android put RetroArch elsewhere."""
    try:
        wait_log(r'\[SecondDisplay\] Cannot show on display %d\b' % other,
                 15)
    except Fail:
        if activity_on(run.main) and not activity_on(did):
            raise Skip('am start --display %d left RetroArch on display %d'
                       % (did, run.main))
        raise


def system_lines():
    return adb('logcat', '-d', '-b', 'main', '-b', 'system', '-v',
               'threadtime').splitlines()


def app_uid():
    m = re.search(r'^package:%s uid:(\d+)' % re.escape(PKG),
                  sh('pm list packages -U %s' % PKG), re.M)
    if not m:
        raise Fail('pm lists no uid for %s' % PKG)
    return int(m.group(1))


def parse_move_asks(lines, uid):
    """RetroArch's moveToDisplay calls in a main and system log: the
    app's own starts of its Activity, and failures before any start (a
    start that throws logs both)."""
    own = re.compile(r'\bcmp=%s/\S*RetroActivityFuture\b' % re.escape(PKG))
    asks = []
    threw = False
    # API 35 logs a start it denies as result code=0, after this line
    denied = False
    for l in lines:
        m = STARTED.search(l)
        if 'Permission Denial: starting Intent' in l and own.search(l):
            denied = True
        elif m and int(m.group(2)) == uid and own.search(m.group(1)):
            asks.append(l)
            threw = (denied or m.group(3) is None
                     or int(m.group(3)) < 0)
            denied = False
        elif MOVE_FAILED.search(l):
            if not threw:
                asks.append(l)
            threw = False
    return asks


def asks_after(lines, uid, rx, until=None):
    """parse_move_asks() for the lines after the first one matching rx
    (all lines once it has left the buffer), up to the first after it
    containing until."""
    pat = re.compile(rx)
    first = next((i for i, l in enumerate(lines) if pat.search(l)), -1)
    last = next((i for i in range(first + 1, len(lines))
                 if until and until in lines[i]), len(lines))
    span = set(lines[first + 1:last])
    return [l for l in parse_move_asks(lines, uid) if l in span]


def few_moves(uid, rx, what, until=None):
    """At most one move asked for after rx: RetroArch's moves never
    loop."""
    n = len(asks_after(system_lines(), uid, rx, until))
    if n > 1:
        raise Fail('%d moves %s' % (n, what))


def posted(lines):
    """Every failed move was logged on the UI thread (tid == pid), not on
    the thread that loaded the core: the app thread only posts a move.
    The UI thread's tid, None with no failure."""
    core = [THREADS.match(l) for l in lines
            if 'Loading dynamic libretro core' in l]
    core = core[-1].group(2) if core and core[-1] else None
    ui = None
    for l in lines:
        m = THREADS.match(l) if MOVE_FAILED.search(l) else None
        if not m:
            continue
        if m.group(1) != m.group(2) or m.group(2) == core:
            raise Fail('a move failed on thread %s of process %s, not its UI '
                       'thread (the core loaded on %s)'
                       % (m.group(2), m.group(1), core))
        ui = m.group(2)
    return ui


def parse_life(lines):
    """(event, task id or None) for each RetroActivityFuture event in an
    events log."""
    out = []
    for l in lines:
        m = LIFE.search(l)
        if not m or 'RetroActivityFuture' not in m.group(2):
            continue
        f = m.group(2).split(',')
        task = (int(f[2]) if m.group(1) in TASKED and len(f) > 2
                and f[2].isdigit() else None)
        out.append((m.group(1), task))
    return out


def judge_life(ev, moves=None):
    """Why parse_life's events ev show RetroArch remade: a relaunch,
    onDestroy, another onCreate, another task. With moves, judged from
    the first new intent on, and there must be moves of them; without,
    the launch's own create must be there. Fails with no such evidence.
    With the events and the tasks seen."""
    names = [e for e, t in ev]
    if not names:
        raise Fail('cannot tell: no activity events')
    if moves:
        if 'new_intent' not in names:
            raise Fail('cannot tell: no new intent in the activity events %s'
                       % names)
        if names.count('new_intent') != moves:
            raise Fail('%d new intents for %d moves: %s'
                       % (names.count('new_intent'), moves, names))
        after, own = names[names.index('new_intent'):], 0
    elif 'create_activity' not in names:
        raise Fail('cannot tell: no create_activity in the activity events '
                   '%s' % names)
    else:
        after, own = names, 1
    why = sorted(set(e for e in after
                     if e.startswith('relaunch') or e == 'on_destroy_called'))
    for e in ('create_activity', 'on_create_called'):
        if after.count(e) > own:
            why.append('%d %s%s' % (after.count(e), e,
                                    ' after the move' if moves else ''))
    tasks = sorted(set(t for e, t in ev if t is not None))
    if len(tasks) > 1:
        why.append('tasks %s' % tasks)
    return why, names, tasks


def remade(pid, moves=None):
    """How RetroArch was remade since start() launched it in process pid:
    a new process, or judge_life's reasons."""
    now = sh('pidof %s' % PKG, check=False).strip()
    ev = parse_life(adb('logcat', '-d', '-b', 'events', '-v', 'threadtime',
                        check=False).splitlines())
    try:
        why, names, tasks = judge_life(ev, moves)
    except Fail:
        if now == pid:
            raise
        why, names, tasks = [], [e for e, t in ev], []
    if now != pid:
        why.insert(0, 'process %s is now %s' % (pid, now or 'gone'))
    return why, names, tasks


def in_place(pid, moves=None):
    """RetroArch's own moves kept its process, Activity and task: no
    System.exit, onDestroy or second onCreate; moves new intents if
    given. What the events log showed."""
    why, names, tasks = remade(pid, moves)
    if why:
        raise Fail('RetroArch was remade: %s' % ', '.join(why))
    return 'task %s, %d new intents' % (tasks[0] if tasks else '?',
                                        names.count('new_intent'))


@case('names')
def c_names(run):
    start(run)
    sid = check_routed(run, 'names')[0]
    start(run, cfg={'video_screen2_display': WRONG})
    d = displays()[sid]
    if not (d['physical'] and d['state'] == 'ON'):
        expect_attached(run, 'names-absent')
        return ('by name onto display %d; an absent name follows Auto, '
                'which skips it' % sid)
    if d['connection'] in (2, 3):
        check_routed(run, 'names-absent', second=GREEN, main=YELLOW,
                     view=VIEWS['ds'][0])
    else:
        check_routed(run, 'names-absent')
    return 'by name onto display %d; an absent name follows Auto there' % sid


@case('screen1_out', allow=REFUSALS)
def c_screen1_out(run):
    ds = displays()
    key = display_key(ds, find_second(ds, run.probe))
    start(run, cfg={'video_screen1_display': key,
                    'video_screen2_display': display_key(ds, run.main)})
    uid = app_uid()
    # RetroArch goes with Screen 1; a main display that refuses Screen 2's
    # Presentation takes it back, and Screen 1 gets the Presentation
    try:
        if not run.probe['main']['presentation']:
            settle_on(run.main, timeout=30)
        sid, img2, box2, img0, box0 = check_routed(
            run, 'screen1', second=GREEN, main=YELLOW, view=VIEWS['ds'][0],
            timeout=30)
        touch_points(run, sid, box2, VIEWS['ds'][0])
        touch_points(run, run.main, box0, VIEWS['ds'][1])
    finally:
        # A move loop fails by name, whatever it broke first
        few_moves(uid, READY, 'after content start')
    return ('screen 1 on display %d (%s), screen 2 on display %d'
            % (sid, key, run.main))


@case('main_none')
def c_main_none(run):
    """Both screens on the second display: RetroArch goes there with the
    menu, and the main display is left to Android."""
    ds = displays()
    sid = find_second(ds, run.probe)
    start(run, cfg={'video_screen1_display': display_key(ds, sid)})
    settle_on(sid)
    if run.main in app_windows():
        raise Fail('RetroArch kept a window on display %d' % run.main)
    img2 = run.capture(sid, displays(), 'none-second')
    g, y = img2.box(GREEN), img2.box(YELLOW)
    if not g or not y:
        raise Fail('display %d does not show both screens (green %s, '
                   'yellow %s)' % (sid, g, y))
    tap_check(run, sid, g, VIEWS['ds'][0], 0.5, 0.5)
    tap_check(run, sid, y, VIEWS['ds'][1], 0.5, 0.5)
    ra_cmd('MENU open', wait=3)
    try:
        wait_until(lambda: run.capture(sid, displays(), 'none-menu')
                   .differs(img2) > 0.05, 10,
                   'the menu did not show on display %d' % sid)
        if run.main in app_windows() or not activity_on(sid):
            raise Fail('opening the menu moved RetroArch')
    finally:
        ra_cmd('MENU close', wait=3)
    return ('both screens and the menu on display %d with RetroArch; '
            'display %d left to Android' % (sid, run.main))


@case('menu_rows')
def c_menu_rows(run):
    start(run, cfg={'video_screen2_display': ''})
    ds = displays()
    key = display_key(ds, find_second(ds, run.probe))
    rows = menu_top()
    for rx in (r'^Settings$', r'^Video$', r'^Stereo'):
        rows = menu('a', menu_pick(rows, rx, 'menu')[0])
    labels = [r[1] for r in rows]
    for want in ('Game Screen 1 Display', 'Game Screen 2 Display'):
        if want not in labels:
            raise Fail('no "%s" row in %s' % (want, labels))
    for gone in ('Screen 1', 'Screen 2'):
        if gone in labels:
            raise Fail('a "%s" mode row is listed: %s' % (gone, labels))
    if any('Monitor' in l for l in labels):
        raise Fail('a monitor row is listed on Android: %s' % labels)
    i, value = menu_pick(rows, r'^Game Screen 2 Display$', 'screens')
    if value != 'Auto':
        raise Fail('Game Screen 2 Display shows %r, not Auto' % value)
    choices = menu('a', i)
    if not any(r[1] == 'Auto' for r in choices):
        raise Fail('the dropdown has no "Auto" entry: %s'
                   % [r[1] for r in choices])
    if choices[-1][1] != 'OFF':
        raise Fail('the dropdown does not end with "OFF": %s'
                   % [r[1] for r in choices])
    j = menu_pick(choices, r'^%s( \(|$)' % re.escape(DISPLAY + key),
                  'dropdown')[0]
    rows = menu('a', j)
    value = menu_pick(rows, r'^Game Screen 2 Display$', 'screens')[1]
    if not value.startswith(DISPLAY + key):
        raise Fail('Game Screen 2 Display shows %r after picking %r'
                   % (value, key))
    # Left and right go round Auto, the displays in list order and OFF
    order = ['Auto'] + [k for k, on in list_view(ds)] + ['OFF']
    i = menu_pick(rows, r'^Game Screen 2 Display$', 'screens')[0]
    at = order.index(key)
    for word in ['right'] * len(order) + ['left', 'right']:
        at = (at + (1 if word == 'right' else -1)) % len(order)
        got = menu_pick(menu(word, i), r'^Game Screen 2 Display$',
                        'screens')[1]
        if not shows(got, order[at]):
            raise Fail('%s on Game Screen 2 Display gave %r, not %r'
                       % (word, got, order[at]))
    menu('close')
    check_routed(run, 'menu-rows')
    return ('Game Screen 2 Display: Auto, then %r from the dropdown; left and '
            'right went round %s' % (value, order))


@case('move_cocoon', emulator_only=True, allow=REFUSALS)
def c_move_cocoon(run):
    ds = displays()
    sid = find_second(ds, run.probe)
    before = list_view(ds)
    # Content from the launch intent, which the move's copy carries
    push_text(ROM, 'ra-dual move\n')
    start(run, cfg={'video_screen1_display': display_key(ds, run.main)},
          on=sid, extras={'LIBRETRO': '%s/%s' % (CORES, TEST_CORE),
                          'ROM': ROM})
    pid = sh('pidof %s' % PKG).strip()
    uid = app_uid()
    wait_until(lambda: activity_on(run.main), 15,
               'RetroArch did not move onto display %d' % run.main)
    if not parse_move_asks(system_lines(), uid):
        raise Skip('am start --display %d left RetroArch on display %d'
                   % (sid, run.main))
    sid2, img2, box2, img0, box0 = check_routed(run, 'cocoon', second=YELLOW,
                                                main=GREEN)
    if sh('pidof %s' % PKG).strip() != pid:
        raise Fail('the move restarted RetroArch')
    if any(re.search(r'Cannot move to display', l) for l in log_lines()):
        raise Fail('the move was refused')
    asks = parse_move_asks(system_lines(), uid)
    if len(asks) != 1:
        raise Fail('%d moveToDisplay calls for one launch' % len(asks))
    kept = in_place(pid, moves=1)
    touch_points(run, sid2, box2, VIEWS['ds'][1])
    after = list_view(displays())
    if after != before:
        raise Fail('the displays changed with the move: %s, was %s'
                   % (after, before))
    keys = display_choices()[0]
    if len(keys) != len(before) or not all(any(shows(c, k) for c in keys)
                                           for k, on in before):
        raise Fail('after the move RetroArch lists %s, not %s'
                   % (keys, [k for k, on in before]))
    return ('launched with content on display %d; RetroArch moved onto '
            "display %d, Screen 1's, in place (%s); screen 2 on display %d; "
            'the list kept %s' % (sid, run.main, kept, sid2, before))


@case('move_refused', emulator_only=True, allow=REFUSALS)
def c_move_refused(run):
    sid, d = replace_second(run, UNTRUSTED)
    if d['presentation'] or d['trusted']:
        raise Skip('flags %d made display %d presentation=%s trusted=%s'
                   % (UNTRUSTED, sid, d['presentation'], d['trusted']))
    # With no game the menu, on Screen 2's display, wants RetroArch there
    start(run, cfg={'video_menu_follows': '1'}, menu_only=True)
    pid = sh('pidof %s' % PKG).strip()
    uid = app_uid()
    f0 = frame_count()
    try:
        wait_log(r'\[SecondDisplay\] Cannot move to display %d\b' % sid, 10)
        how = 'refused at once'
    except Fail:
        how = 'not refused, so given up on after its timeout'
    f1 = frame_count()
    time.sleep(3.0)
    if not activity_on(run.main):
        raise Fail('RetroArch left display %d' % run.main)
    if sid in app_windows():
        raise Fail('RetroArch has a window on display %d, which refused it'
                   % sid)
    menu_drawn(run, run.main, 'refused-menu')
    lines = system_lines()
    asks = parse_move_asks(lines, uid)
    if len(asks) != 1:
        raise Fail('the move was asked for %d times for one refusal'
                   % len(asks))
    ui = posted(lines)
    if f0 is not None and f1 is not None and f1 <= f0:
        raise Fail('no frames ran while the move was asked: frame %d, then '
                   '%d' % (f0, f1))
    start_core()
    img0, g, y = attached(run, 'refused')
    if any(re.search(r'Cannot show on display %d\b' % sid, l)
           for l in log_lines()):
        raise Fail('a Presentation was tried on display %d after it could '
                   'not take RetroArch' % sid)
    tap_check(run, run.main, g, VIEWS['ds'][0], 0.5, 0.5)
    kept = in_place(pid)
    key = display_key(displays(), sid)
    keys, value = display_choices()
    if not any(shows(k, key) for k in keys) or not value.startswith(
            DISPLAY + key + ' ('):
        raise Fail('refused display %d is not listed: dropdown %s, Screen 2 '
                   'Display %r' % (sid, keys, value))
    frames = ('frames %d to %d' % (f0, f1) if f0 is not None and f1 is not
              None else 'frames not read')
    return ('display %d could not take RetroArch: the move was %s, %s, %s; '
            'the menu, then both screens, stayed with RetroArch (%s); %r '
            'still listed'
            % (sid, how, 'failed on UI thread %s' % ui if ui else
               'no failure logged', frames, kept, value))


@case('move_both_refuse', emulator_only=True, allow=REFUSALS)
def c_move_both_refuse(run):
    if run.probe['main']['presentation']:
        raise Skip('display %d takes a Presentation: nothing refuses'
                   % run.main)
    sid, d = replace_second(run, TRUSTED)
    if d['presentation'] or not d['trusted']:
        raise Skip('flags %d made display %d presentation=%s trusted=%s'
                   % (TRUSTED, sid, d['presentation'], d['trusted']))
    ds = displays()
    start(run, cfg={'video_screen1_display': display_key(ds, run.main)},
          on=sid)
    pid = sh('pidof %s' % PKG).strip()
    uid = app_uid()
    wait_until(lambda: activity_on(run.main), 15,
               'RetroArch did not move onto display %d' % run.main)
    if not parse_move_asks(system_lines(), uid):
        raise Skip('am start --display %d left RetroArch on display %d'
                   % (sid, run.main))
    wait_log(r'\[SecondDisplay\] Cannot show on display %d\b' % sid, 15,
             what='display %d took the Presentation after the move' % sid)
    attached(run, 'both-refuse')
    if not activity_on(run.main):
        raise Fail('RetroArch moved a second time')
    if any('Cannot move to display' in l for l in log_lines()):
        raise Fail('a move was refused')
    asks = parse_move_asks(system_lines(), uid)
    if len(asks) != 1:
        raise Fail('%d moveToDisplay calls for two refusals' % len(asks))
    kept = in_place(pid, moves=1)
    return ('moved once onto display %d (%s); display %d refused too, so '
            'both screens joined RetroArch' % (run.main, kept, sid))


@case('move_density', emulator_only=True, allow=REFUSALS)
def c_move_density(run):
    """Its PASS or FAIL line names "relaunch": whether moving onto a
    display of another density remade the Activity."""
    dpi0 = displays()[run.main]['density']
    if dpi0 is None:
        raise Skip('dumpsys display shows no density for display %d'
                   % run.main)
    dpi = 160 if dpi0 != 160 else 240
    sid, d = replace_second(run, 0, EMU_SECOND[:2] + (dpi,))
    if d['density'] != dpi:
        raise Skip('display %d came back at %s dpi, not %d'
                   % (sid, d['density'], dpi))
    ds = displays()
    start(run, cfg={'video_screen1_display': display_key(ds, run.main)},
          on=sid)
    pid = sh('pidof %s' % PKG).strip()
    uid = app_uid()
    wait_until(lambda: activity_on(run.main), 15,
               'RetroArch did not move onto display %d' % run.main)
    if not parse_move_asks(system_lines(), uid):
        raise Skip('am start --display %d left RetroArch on display %d'
                   % (sid, run.main))
    time.sleep(3.0)
    what = 'display %d (%d dpi) onto display %d (%d dpi)' % (
        sid, dpi, run.main, dpi0)
    try:
        why, names, tasks = remade(pid, moves=1)
    except Fail as e:
        raise Fail('relaunch unknown, moving from %s: %s' % (what, e))
    if why:
        raise Fail('relaunch: moving from %s remade the Activity: %s'
                   % (what, ', '.join(why)))
    try:
        check_routed(run, 'density', second=YELLOW, main=GREEN)
    except Fail as e:
        raise Fail('no relaunch, but %s' % e)
    return 'no relaunch: moved from %s in place, events %s' % (what, names)


@case('own_gone', emulator_only=True, allow=GONE)
def c_own_gone(run):
    ds = displays()
    sid = find_second(ds, run.probe)
    # Screen 1 and the menu with Screen 2, so RetroArch stays there
    start(run, cfg={'video_screen1_display': display_key(ds, sid)}, on=sid)
    if not activity_on(sid):
        raise Skip('am start --display %d left RetroArch on display %d'
                   % (sid, run.main))
    pid = sh('pidof %s' % PKG).strip()
    wait_until(lambda: run.capture(sid, displays(), 'gone-before')
               .box(YELLOW), 15, 'display %d shows no screen 2' % sid)
    console('multidisplay del %d' % EMU_INDEX)
    wait_until(lambda: find_second(displays(), run.probe) is None, 15,
               'the second display is still listed after multidisplay del')

    def placed():
        try:
            return expect_attached(run, 'gone', settle=0.5)
        except Fail:
            return None
    img0, g, y = wait_until(placed, 20, 'the screens were not placed on '
                            'display %d again' % run.main)
    if sh('pidof %s' % PKG, check=False).strip() != pid:
        raise Fail('RetroArch\'s process ended when its display went away')
    if not activity_on(run.main):
        raise Fail('RetroArch is not on display %d' % run.main)
    touched = in_front(run.main)
    if touched:
        tap_check(run, run.main, g, VIEWS['ds'][0], 0.5, 0.5)
    return ('display %d went away under RetroArch; both screens placed on '
            'display %d%s' % (sid, run.main, '' if touched else
                              ' (not in front, so no touch sent)'))


@case('move_list_change', emulator_only=True, allow=REFUSALS)
def c_move_list_change(run):
    ds = displays()
    sid = find_second(ds, run.probe)
    count = len(list_view(ds))
    start(run, cfg={'video_screen1_display': display_key(ds, run.main)},
          on=sid, ready=None)
    uid = app_uid()
    # Before the first frames, so while the move is asked or pending
    try:
        console('multidisplay add %d 800 600 %d 0'
                % (EMU_THIRD, EMU_SECOND[2]))
    except Fail as e:
        if 'not supported' not in str(e):
            raise
        raise Skip('this emulator makes no third display: %s' % e)
    try:
        wait_until(lambda: len(list_view(displays())) > count, 20,
                   'no third display after multidisplay add')

        def one():
            on = [i for i, t in app_windows().items()
                  if any(ours(x) for x in t)]
            return on if len(on) == 1 else None
        wait_until(lambda: one() == [run.main], 20,
                   'RetroArch did not end on display %d alone' % run.main)
        time.sleep(3.0)
        now = one()
        if now != [run.main]:
            raise Fail('RetroArch did not settle: display %d, then %s'
                       % (run.main, now))
        if not alive():
            raise Fail('RetroArch is not running')
        lines = adb('logcat', '-d', '-b', 'main', '-b', 'system', '-b',
                    'events', '-v', 'threadtime').splitlines()
        asks = parse_move_asks(lines, uid)
        if not asks:
            raise Skip('no moveToDisplay call seen, so no move was pending')
        ask = lines.index(asks[0])
        added = [i for i, l in enumerate(lines)
                 if 'Display device added' in l]
        landed = [i for i, l in enumerate(lines)
                  if [e for e, t in parse_life([l])] == ['new_intent']]
        if not added:
            when = 'at an unknown time'
        elif added[-1] < ask:
            when = 'before the move was asked'
        elif not landed:
            when = 'after the move was asked, with no new intent logged'
        elif added[-1] < landed[0]:
            when = ('inside the pending window, after the move was asked '
                    'and before Android delivered it')
        else:
            when = 'after the pending window, once Android delivered the move'
    finally:
        console('multidisplay del %d' % EMU_THIRD)
        wait_until(lambda: len(list_view(displays())) == count, 15,
                   'the third display is still listed')
    return ('the third display came %s; RetroArch settled on display %d '
            'after %d moveToDisplay calls' % (when, run.main, len(asks)))


# ---- commands ----

# ---- Screen windows and the menu on any display (2026-10-27) ----

MENU_FOLLOWS_SEPARATE = '8'     # VIDEO_MENU_FOLLOWS_SEPARATE
# The idle logo's image (00 D4): a local APK ships no assets
LOGO = '/usr/share/retroarch/assets/xmb/monochrome/png/retroarch.png'
LOGO_REMOTE = DEV + '/assets/xmb/monochrome/png/retroarch.png'
# RGUI's frame: 240 lines, entries on an 11-line stride from line 26
RGUI_LINES, RGUI_TOP, RGUI_STRIDE = 240, 26, 11
DARK = 16                       # a channel above this is drawn, not black
STEREO_ROWS = ('Game Screen 1 Display', 'Game Screen 2 Display',
               'Menu Follows', 'Menu Display', 'Notifications')


def presentation_on(did):
    """RetroArch's Presentation window is on display did."""
    return any(ours_second(t) for t in app_windows().get(did, []))


def settle_on(did, timeout=20, hold=2.0):
    """RetroArch's Activity on display did for hold s: placement's moves
    are over."""
    def there():
        if not activity_on(did):
            return False
        time.sleep(hold)
        return activity_on(did)
    wait_until(there, timeout, 'RetroArch did not settle on display %d'
               % did)


def attached(run, label, timeout=20):
    """expect_attached() once RetroArch has settled."""
    end = time.time() + timeout
    while True:
        try:
            return expect_attached(run, label, settle=0.5)
        except Fail:
            if time.time() > end:
                raise
            time.sleep(0.5)


def ink(img, tol=DARK, step=2):
    """(x0, y0, x1, y1) around everything drawn on black, x1 and y1 past
    its edge, or None for an all-black image."""
    data, w, h = img.rgba, img.w, img.h
    x0 = y0 = None
    x1 = y1 = -1
    for y in range(0, h, step):
        row = y * w * 4
        for x in range(0, w, step):
            i = row + x * 4
            if max(data[i], data[i + 1], data[i + 2]) <= tol:
                continue
            if x0 is None or x < x0:
                x0 = x
            if x > x1:
                x1 = x
            if y0 is None:
                y0 = y
            y1 = y
    if x0 is None:
        return None
    return (x0, y0, min(x1 + step, w), min(y1 + step, h))


def menu_frame(img, colour):
    """RGUI's frame drawn over a game screen of colour (ink() takes the
    game in): what is drawn along the middle row and column that is
    neither black nor the game's colour."""
    def drawn(x, y):
        i = (y * img.w + x) * 4
        return (max(img.rgba[i], img.rgba[i + 1], img.rgba[i + 2]) > DARK
                and not img.near(x, y, colour))
    xs = [x for x in range(img.w) if drawn(x, img.h // 2)]
    ys = [y for y in range(img.h) if drawn(img.w // 2, y)]
    if not xs or not ys:
        return None
    return (xs[0], ys[0], xs[-1] + 1, ys[-1] + 1)


def rgui_row(box, k):
    """The middle of RGUI entry k, RGUI's frame drawn in box."""
    x0, y0, x1, y1 = box
    scale = float(y1 - y0) / RGUI_LINES
    return ((x0 + x1) // 2,
            int(y0 + (RGUI_TOP + RGUI_STRIDE * k + RGUI_STRIDE / 2.0)
                * scale))


def settings_tab(rows):
    """rows are Material UI's Settings tab: Settings is a nav-bar tab
    there, not a row."""
    labels = set(r[1] for r in rows)
    return 'Video' in labels and 'Audio' in labels


def main_menu():
    """The menu open at its top, from wherever it was left (Material UI:
    its Settings tab)."""
    rows = menu('open')
    last = None
    for _ in range(8):
        if any(r[1] == 'Settings' for r in rows):
            return rows
        if rows and rows == last:
            break
        last = rows
        rows = menu('b')
    # Material UI's top level: left and right switch nav-bar tabs
    for _ in range(12):
        if settings_tab(rows):
            return rows
        if not any(r[1] in ('Load Content', 'Quick Menu') for r in rows):
            break
        rows = menu('left')
    raise Fail('the menu did not reach its top: %s' % [r[1] for r in rows])


def stereo_rows(settle=0.0):
    """The menu open on Settings > Video > Stereo 3D & Screens: its rows.
    settle: s between selecting each entry and its OK (Material UI
    scaled up: an OK on an entry below the screen can miss)."""
    rows = main_menu()
    steps = (r'^Settings$', r'^Video$', r'^Stereo')
    if not any(r[1] == 'Settings' for r in rows):
        steps = steps[1:]
    for rx in steps:
        i = menu_pick(rows, rx, 'menu')[0]
        if settle:
            menu('select', i)
            time.sleep(settle)
        rows = menu('a', i)
    return rows


def menu_state():
    """MENU show: the PATH line ('' at the top) and the rows."""
    text = ra_cmd('MENU show', wait=3)
    path = [l.strip() for l in text.splitlines() if l.startswith('PATH ')]
    rows = [(int(m.group(1)), m.group(2), m.group(3))
            for m in (MENU_ROW.match(l) for l in text.splitlines()) if m]
    return (path[0] if path else ''), rows


def rgui_box(img, b):
    """Ink box b is RGUI's frame: centred, with black bars on two
    opposite sides. A launcher or another app fills the display."""
    x0, y0, x1, y1 = b
    return (abs(x0 + x1 - img.w) <= img.w * 0.1
            and abs(y0 + y1 - img.h) <= img.h * 0.1
            and ((x0 >= img.w * 0.02 and x1 <= img.w * 0.98)
                 or (y0 >= img.h * 0.02 and y1 <= img.h * 0.98)))


def menu_drawn(run, did, label, timeout=10, ours=False):
    """Display did once something is drawn on it: its image and ink.
    ours: RetroArch's Activity is on did before and after the capture,
    and the ink is RGUI's frame, not an app's under RetroArch."""
    why = ['nothing is drawn']
    def drawn():
        if ours and not activity_on(did):
            why[0] = "RetroArch's Activity is not on it"
            return None
        img = run.capture(did, displays(), label)
        b = ink(img)
        if not b:
            why[0] = 'nothing is drawn'
        elif ours and not activity_on(did):
            why[0] = "RetroArch's Activity left it during the capture"
        elif ours and not rgui_box(img, b):
            why[0] = "ink %s on %dx%d is not RGUI's frame" % (b, img.w,
                                                              img.h)
        else:
            return img, b
        return None
    end = time.time() + timeout
    while True:
        v = drawn()
        if v:
            return v
        if time.time() > end:
            raise Fail('display %d shows no menu: %s' % (did, why[0]))
        time.sleep(0.5)


def tap_row(run, did, box, k):
    """A tap on RGUI entry k, drawn in box on display did: the menu's
    path after it, the core's pointers, and where it was."""
    x, y = rgui_row(box, k)
    tag = mark()
    held_tap(run, did, x, y)
    time.sleep(1.5)
    return menu_state()[0], pointers(lines_after(tag)), (x, y)


def message_on(run, dids, label):
    """The displays of dids that a SHOW_MSG changed."""
    ds = displays()
    before = dict((d, run.capture(d, ds, '%s-%d-0' % (label, d)))
                  for d in dids)
    ra_cmd('SHOW_MSG ra-dual %s' % label)
    time.sleep(1.0)
    return [d for d in dids
            if run.capture(d, ds, '%s-%d-1' % (label, d))
            .differs(before[d]) >= 0.001]


def push_logo():
    """The idle logo's image into the test config's assets directory."""
    if not os.path.exists(LOGO):
        raise Skip('no %s on this host to push' % LOGO)
    sh('mkdir -p %s' % os.path.dirname(LOGO_REMOTE))
    adb('push', LOGO, LOGO_REMOTE)


def judge_handover(states, shots, errors=()):
    """Make before break on one display, from watch_display()'s samples:
    why it broke (a sample with neither of RetroArch's windows there, an
    all-black capture, too few samples to tell), and whether both windows
    were seen at once."""
    why = []
    for n, what in ((len(states), 'window samples'), (len(shots), 'captures')):
        if n < 2:
            why.append('only %d %s (unsampled: %s)'
                       % (n, what, errors[0] if errors else 'nothing'))
    gaps = [t for t, a, p in states if not a and not p]
    if gaps:
        why.append('%d of %d window samples had no RetroArch window'
                   % (len(gaps), len(states)))
    black = [n for t, n, box in shots if box is None]
    if black:
        why.append('captures %s were black' % black)
    return why, any(a and p for t, a, p in states)


def watch_display(run, did, label, act, until, timeout=30):
    """act(), with display did sampled from 1 s before it until until()
    holds and 2 s more: (time, Activity there, Presentation there) and
    (time, capture number, ink box), and what could not be sampled."""
    ds = displays()
    stop = threading.Event()
    states, shots, errors = [], [], []

    def windows():
        while not stop.is_set():
            try:
                w = app_windows().get(did, [])
            except (Fail, subprocess.TimeoutExpired) as e:
                errors.append(str(e))
                stop.wait(0.5)
                continue
            states.append((time.time(), any(ours(t) for t in w),
                           any(ours_second(t) for t in w)))

    def captures():
        n = 0
        while not stop.is_set():
            n += 1
            try:
                img = run.capture(did, ds, '%s-%03d' % (label, n))
            except (Fail, subprocess.TimeoutExpired) as e:
                errors.append(str(e))
                stop.wait(0.5)
                continue
            shots.append((time.time(), n, ink(img)))

    threads = [threading.Thread(target=f) for f in (windows, captures)]
    for t in threads:
        t.start()
    try:
        time.sleep(1.0)
        act()
        wait_until(until, timeout, '%s: RetroArch did not settle' % label)
        time.sleep(2.0)
    finally:
        stop.set()
        for t in threads:
            t.join(120)
    return states, shots, errors


def auto_routed(run, label):
    """Screen 2 where Auto puts it on this device (00 P2): what was seen."""
    ds = displays()
    sid = find_second(ds, run.probe)
    if sid is None:
        raise Fail('the second display is gone')
    d = ds[sid]
    if d['physical'] and d['state'] == 'ON':
        if d['connection'] in (2, 3):
            check_routed(run, label, second=GREEN, main=YELLOW,
                         view=VIEWS['ds'][0], timeout=30)
            return 'external: screen 0 on display %d' % sid
        check_routed(run, label, timeout=30)
        return 'built-in: screen 1 on display %d' % sid
    attached(run, label)
    return 'display %d has no product info: Auto left it alone' % sid


@case('rows')
def c_rows(run):
    start(run)
    ds = displays()
    sid = find_second(ds, run.probe)
    key = display_key(ds, sid)
    keys = [k for k, on in list_view(ds)]
    try:
        rows = stereo_rows()
        labels = [r[1] for r in rows]
        for want in ('Game Screens on Other Displays', 'Game Screen 1 Display',
                     'Game Screen 2 Display', 'Menu Follows', 'Notifications'):
            if want not in labels:
                raise Fail('no "%s" row in %s' % (want, labels))
        for gone in ('Screen 1', 'Screen 2', 'Menu Display'):
            if gone in labels:
                raise Fail('a "%s" row is listed: %s' % (gone, labels))
        if any('Monitor' in l for l in labels):
            raise Fail('a monitor row is listed on Android: %s' % labels)
        # Game Screen 2 Display: OFF from the dropdown, then its display again
        drop = menu('a', menu_pick(rows, r'^Game Screen 2 Display$',
                                   'rows')[0])
        rows = menu('a', menu_pick(drop, r'^OFF$', 'dropdown')[0])
        value = menu_pick(rows, r'^Game Screen 2 Display$', 'rows')[1]
        if value != 'OFF':
            raise Fail('Game Screen 2 Display shows %r after picking OFF'
                       % value)
        drop = menu('a', menu_pick(rows, r'^Game Screen 2 Display$',
                                   'rows')[0])
        rows = menu('a', menu_pick(
            drop, r'^%s( \(|$)' % re.escape(DISPLAY + key), 'dropdown')[0])
        # Menu Follows: the ds map's Game Screen 1-2 and Separate; Separate
        # adds Menu Display
        i, value = menu_pick(rows, r'^Menu Follows$', 'rows')
        if value != 'Game Screen 1':
            raise Fail('Menu Follows shows %r, not Game Screen 1' % value)
        drop = menu('a', i)
        want = ['Game Screen 1', 'Game Screen 2', 'Separate']
        if [r[1] for r in drop] != want:
            raise Fail('Menu Follows offers %s, not %s'
                       % ([r[1] for r in drop], want))
        rows = menu('a', menu_pick(drop, r'^Separate$', 'dropdown')[0])
        i, value = menu_pick(rows, r'^Menu Display$', 'rows, Separate')
        if value != 'Auto':
            raise Fail('Menu Display shows %r, not Auto' % value)
        drop = menu('a', i)
        offered = [r[1] for r in drop]
        if (not offered or offered[0] != 'Auto' or 'OFF' in offered
                or not all(any(shows(o, k) for o in offered) for k in keys)):
            raise Fail('Menu Display offers %s, not Auto then %s'
                       % (offered, keys))
        rows = menu('a', menu_pick(
            drop, r'^%s( \(|$)' % re.escape(DISPLAY + key), 'dropdown')[0])
        i, value = menu_pick(rows, r'^Menu Display$', 'rows')
        if not shows(value, key):
            raise Fail('Menu Display shows %r after picking %r'
                       % (value, key))
        # Left and right go round Auto and the displays, with no OFF
        order = ['Auto'] + keys
        at = order.index(key)
        for word in ['right'] * len(order) + ['left', 'right']:
            at = (at + (1 if word == 'right' else -1)) % len(order)
            got = menu_pick(menu(word, i), r'^Menu Display$', 'rows')[1]
            if not shows(got, order[at]):
                raise Fail('%s on Menu Display gave %r, not %r'
                           % (word, got, order[at]))
        rows = menu('show')
        i = menu_pick(rows, r'^Menu Follows$', 'rows')[0]
        rows = menu('left', i)
        value = menu_pick(rows, r'^Menu Follows$', 'rows')[1]
        if value != 'Game Screen 2':
            raise Fail('left from Separate gave %r, not Game Screen 2' % value)
        if any(r[1] == 'Menu Display' for r in rows):
            raise Fail('Menu Display is listed with Menu Follows Game '
                       'Screen 2')
        drop = menu('a', i)
        rows = menu('a', menu_pick(drop, r'^Game Screen 1$', 'dropdown')[0])
        # Notifications
        i, value = menu_pick(rows, r'^Notifications$', 'rows')
        if value != 'With the Game':
            raise Fail('Notifications shows %r, not With the Game' % value)
        for word, want in (('right', 'With the Menu'),
                           ('left', 'With the Game')):
            got = menu_pick(menu(word, i), r'^Notifications$', 'rows')[1]
            if got != want:
                raise Fail('%s on Notifications gave %r, not %r'
                           % (word, got, want))
        offered = [r[1] for r in menu('a', i)]
        if offered != ['With the Game', 'With the Menu']:
            raise Fail('Notifications offers %s' % offered)
        rows = menu('b')
        # Game Screens on Other Displays OFF hides the rest
        i, value = menu_pick(rows, r'^Game Screens on Other Displays$',
                             'rows')
        if value.upper() != 'ON':
            raise Fail('Game Screens on Other Displays shows %r, not ON'
                       % value)
        rows = menu('left', i)
        value = menu_pick(rows, r'^Game Screens on Other Displays$',
                          'rows')[1]
        left = [r[1] for r in rows if r[1] in STEREO_ROWS]
        if value.upper() != 'OFF' or left:
            raise Fail('Game Screens on Other Displays %r still lists %s'
                       % (value, left))
        rows = menu('right', i)
        if 'Menu Follows' not in [r[1] for r in rows]:
            raise Fail('the rows did not come back with Game Screens on '
                       'Other Displays ON: %s' % [r[1] for r in rows])
    finally:
        menu('close')
    check_routed(run, 'rows-after', timeout=30)
    return ('five rows; Game Screen 2 Display OFF and back; Menu Follows '
            'offers Game Screen 1-2 and Separate, which alone lists Menu '
            'Display (Auto '
            'and %s, no OFF); Notifications both ways; OFF hides the rest'
            % keys)


@case('migration')
def c_migration(run):
    start(run, cfg={'video_screen2_display': None,
                    'video_screen2_window_mode': '2'})
    time.sleep(3.0)
    ds = displays()
    sid = find_second(ds, run.probe)
    if sid is not None and sid in app_windows():
        raise Fail('old Hidden: RetroArch took display %d' % sid)
    img0 = run.capture(run.main, ds, 'migration-hidden-main')
    if not img0.box(GREEN) or img0.box(YELLOW):
        raise Fail('old Hidden: the main display should show screen 1 '
                   'alone')
    try:
        value = menu_pick(stereo_rows(), r'^Game Screen 2 Display$', 'rows')[1]
    finally:
        menu('close')
    if value != 'OFF':
        raise Fail('old Hidden: Game Screen 2 Display shows %r, not OFF'
                   % value)
    start(run, cfg={'video_screen2_display': None,
                    'video_screen2_window_mode': '1'})
    try:
        value = menu_pick(stereo_rows(), r'^Game Screen 2 Display$', 'rows')[1]
    finally:
        menu('close')
    if value != 'Auto':
        raise Fail('old Separate: Game Screen 2 Display shows %r, not Auto'
                   % value)
    how = auto_routed(run, 'migration-separate')
    return ('old Hidden became OFF with display %s free; old Separate '
            'became Auto (%s)' % (sid, how))


@case('menu_in_presentation')
def c_menu_in_presentation(run):
    start(run, cfg={'video_menu_follows': '1'})
    settle_on(run.main)
    sid, img2, box2, img0, box0 = check_routed(run, 'mip-before')
    area = (box2[2] - box2[0]) * (box2[3] - box2[1])
    ra_cmd('MENU open', wait=3)
    try:
        def drawn():
            img = run.capture(sid, displays(), 'mip-second')
            b = img.box(YELLOW)
            return ((b is None or (b[2] - b[0]) * (b[3] - b[1]) < area // 2)
                    and ink(img))
        wait_until(drawn, 10, 'the menu was not drawn on display %d' % sid)
        img = run.capture(run.main, displays(), 'mip-main')
        b = img.box(GREEN)
        if not b or max(abs(b[i] - box0[i]) for i in range(4)) > 4:
            raise Fail('display %d changed under the menu: %s, was %s'
                       % (run.main, b, box0))
        if img.fraction(GREEN, b) < 0.985:
            raise Fail('something is drawn over screen 1 on display %d'
                       % run.main)
        if not activity_on(run.main):
            raise Fail('opening the menu moved RetroArch')
    finally:
        ra_cmd('MENU close', wait=3)
    check_routed(run, 'mip-after')
    return ('Menu Follows Screen 2: the menu drew in the Presentation on '
            'display %d; display %d kept screen 1' % (sid, run.main))


@case('menu_touch_presentation')
def c_menu_touch_presentation(run):
    ds = displays()
    sid = find_second(ds, run.probe)
    here = display_key(ds, run.main)
    start(run, cfg={'video_screen1_display': here,
                    'video_screen2_display': here,
                    'video_menu_follows': MENU_FOLLOWS_SEPARATE,
                    'video_menu_display': display_key(ds, sid),
                    'menu_rgui_transparency': 'false'})
    settle_on(run.main)
    img0, g, y = attached(run, 'mtp-before')
    try:
        rows = main_menu()
        k = menu_pick(rows, r'^Settings$', 'main menu')[0]
        img, box = menu_drawn(run, sid, 'mtp-menu')
        if not activity_on(run.main):
            raise Fail('opening the menu moved RetroArch')
        path, hits, at = tap_row(run, sid, box, k)
        if not path.endswith('Settings'):
            raise Fail('a tap at %s on display %d, on entry %d "Settings", '
                       'left the menu at %r' % (at, sid, k, path or 'its top'))
        if hits:
            raise Fail('the tap on the menu reached the core at %s' % hits[0])
        menu('b')
        before = menu_state()[0]
        tag = mark()
        x, yy = (at[0] * img0.w // img.w, at[1] * img0.h // img.h)
        held_tap(run, run.main, x, yy)
        time.sleep(1.5)
        after = menu_state()[0]
        if after != before:
            raise Fail('a tap at (%d,%d) on display %d acted on the menu in '
                       'display %d: %r, was %r'
                       % (x, yy, run.main, sid, after, before))
        if pointers(lines_after(tag)):
            raise Fail('a tap on display %d reached the core with the menu '
                       'open' % run.main)
    finally:
        menu('close')
    wait_until(lambda: sid not in app_windows(), 10,
               'display %d kept RetroArch after the menu closed' % sid)
    return ('a tap on "Settings" in the Presentation on display %d opened '
            'it; the same spot on display %d did nothing' % (sid, run.main))


@case('menu_no_move')
def c_menu_no_move(run):
    ds = displays()
    sid = find_second(ds, run.probe)
    seen = []
    for label, cfg, apart in (
            ('Menu Follows Screen 2', {'video_menu_follows': '1'}, False),
            ('a display of its own',
             {'video_screen2_display': display_key(ds, run.main),
              'video_menu_follows': MENU_FOLLOWS_SEPARATE,
              'video_menu_display': display_key(ds, sid)}, True)):
        start(run, cfg=cfg)
        settle_on(run.main)
        if apart:
            attached(run, 'nomove')
        else:
            check_routed(run, 'nomove')
        pid = sh('pidof %s' % PKG).strip()
        uid = app_uid()
        asks = len(parse_move_asks(system_lines(), uid))
        for n in range(1, 4):
            ra_cmd('MENU open', wait=3)
            if apart:
                wait_until(lambda: presentation_on(sid), 10,
                           '%s: open %d: no Presentation on display %d'
                           % (label, n, sid))
            time.sleep(1.0)
            ra_cmd('MENU close', wait=3)
            if apart:
                wait_until(lambda: sid not in app_windows(), 10,
                           '%s: close %d: display %d kept RetroArch'
                           % (label, n, sid))
            time.sleep(1.0)
            if not activity_on(run.main):
                raise Fail('%s: open and close %d moved RetroArch off '
                           'display %d' % (label, n, run.main))
        more = len(parse_move_asks(system_lines(), uid)) - asks
        if more:
            raise Fail('%s: %d moves asked while the menu opened and closed'
                       % (label, more))
        if sh('pidof %s' % PKG).strip() != pid:
            raise Fail('%s: RetroArch restarted' % label)
        seen.append(label)
    return ('the menu opened and closed 3 times (%s): RetroArch stayed on '
            'display %d, no move asked' % ('; '.join(seen), run.main))


@case('start_close_order')
def c_start_close_order(run):
    ds = displays()
    sid = find_second(ds, run.probe)
    start(run, cfg={'video_menu_follows': '1',
                    'menu_rgui_transparency': 'false'}, menu_only=True)
    settle_on(sid)
    menu_drawn(run, sid, 'order-menu')
    if run.main in app_windows():
        raise Fail('with no game RetroArch kept a window on display %d'
                   % run.main)
    states, shots, errors = watch_display(
        run, sid, 'order-start', start_core,
        lambda: activity_on(run.main) and presentation_on(sid))
    why, both = judge_handover(states, shots, errors)
    if why:
        raise Fail('game start broke display %d: %s' % (sid, '; '.join(why)))
    check_routed(run, 'order-started')
    states2, shots2, errors2 = watch_display(
        run, sid, 'order-close', lambda: ra_cmd('CLOSE_CONTENT'),
        lambda: activity_on(sid) and not presentation_on(sid))
    why2, both2 = judge_handover(states2, shots2, errors2)
    if why2:
        raise Fail('game close broke display %d: %s'
                   % (sid, '; '.join(why2)))
    wait_until(lambda: run.main not in app_windows(), 10,
               'after the close display %d kept a RetroArch window'
               % run.main)
    menu_drawn(run, sid, 'order-closed')
    return ('start: %d window samples, %d captures, both windows seen %s; '
            'close: %d, %d, %s; never empty or black on display %d%s'
            % (len(states), len(shots), both, len(states2), len(shots2),
               both2, sid, '; unsampled: %s' % (errors + errors2)[0]
               if errors or errors2 else ''))


@case('notifications_follow')
def c_notifications_follow(run):
    seen = []
    # (Notifications, a one-screen game, [(menu, where the message shows)])
    for value, one, checks in (('0', False, [('open', 'main')]),
                               ('1', False, [('open', 'second'),
                                             ('closed', 'second')]),
                               ('1', True, [('closed', 'main')])):
        name = ('Game', 'Menu')[int(value)]
        where = name + (', one screen' if one else '')
        tag = 'note-%s%s' % (name, '-one' if one else '')
        start(run, cfg={'video_menu_follows': '1',
                        'video_notifications_window': value,
                        'menu_enable_widgets': 'true',
                        'video_font_enable': 'true'},
              opts={'video_views_test_map': 'none'} if one else None,
              ready=r'\[video_views\] presents=' if one else READY)
        settle_on(run.main)
        if one:
            # No window on the menu's display, so only the main one
            sid = find_second(displays(), run.probe)
            wait_until(lambda: sid not in app_windows(), 10,
                       'a one-screen game: display %d was not freed' % sid)
            dids = (run.main,)
        else:
            sid = check_routed(run, tag)[0]
            dids = (run.main, sid)
        for state, on in checks:
            want = sid if on == 'second' else run.main
            if state == 'open':
                ra_cmd('MENU open', wait=3)
            try:
                time.sleep(2.0)
                got = message_on(run, dids, '%s-%s' % (tag, state))
            finally:
                if state == 'open':
                    ra_cmd('MENU close', wait=3)
            if got != [want]:
                raise Fail('With the %s, menu %s: the message changed %s, '
                           'not display %d alone'
                           % (where, state, got or 'no display', want))
            seen.append('With the %s, menu %s: display %d'
                        % (where, state, want))
    return '; '.join(seen)


@case('one_screen_menu_apart')
def c_one_screen_menu_apart(run):
    ds = displays()
    sid = find_second(ds, run.probe)
    start(run, cfg={'video_menu_follows': '1',
                    'menu_rgui_transparency': 'false'},
          opts={'video_views_test_map': 'none'},
          ready=r'\[video_views\] presents=')
    settle_on(run.main)
    wait_until(lambda: sid not in app_windows(), 10,
               'a one-screen game: display %d was not freed' % sid)
    img0 = run.capture(run.main, displays(), 'apart-main')
    if not img0.box(GREEN) or not img0.box(YELLOW):
        raise Fail('display %d does not show the one-screen game' % run.main)
    for n in (1, 2):
        ra_cmd('MENU open', wait=3)
        try:
            wait_until(lambda: presentation_on(sid), 10,
                       'open %d: no Presentation on display %d for the menu'
                       % (n, sid))
            menu_drawn(run, sid, 'apart-menu-%d' % n)
            img = run.capture(run.main, displays(), 'apart-main-%d' % n)
            if img.differs(img0) > 0.01:
                raise Fail('open %d: the menu drew over the game on display '
                           '%d too' % (n, run.main))
            if not activity_on(run.main):
                raise Fail('open %d: RetroArch moved' % n)
        finally:
            ra_cmd('MENU close', wait=3)
        wait_until(lambda: sid not in app_windows(), 10,
                   'close %d: display %d was not freed' % (n, sid))
    return ('a mapless game on display %d; the menu opened twice on display '
            '%d, which was free between' % (run.main, sid))


def wait_logo(run, did, timeout=15):
    """The idle window's logo, its image and ink box: a centred, at most
    half-size ink that two captures in a row agree on (a toast does not)."""
    end = time.time() + timeout
    last = seen = ok = err = None
    while True:
        try:
            img = run.capture(did, displays(), 'idle-logo')
        except (Fail, subprocess.TimeoutExpired) as e:
            err, last = str(e), None
        else:
            ok = True
            b = ink(img)
            seen = b or seen
            if (b and abs((b[0] + b[2]) / 2.0 - img.w / 2.0) <= img.w * 0.05
                    and abs((b[1] + b[3]) / 2.0 - img.h / 2.0) <= img.h * 0.05
                    and b[2] - b[0] <= img.w // 2
                    and b[3] - b[1] <= img.h // 2):
                if b == last:
                    return img, b
                last = b
            else:
                last = None
        if time.time() > end:
            break
        time.sleep(0.5)
    if not ok:
        raise Fail('display %d: no capture to look for the logo in: %s'
                   % (did, err))
    if not seen:
        raise Fail('display %d is all black: no logo (%s pushed?)'
                   % (did, LOGO_REMOTE))
    raise Fail('display %d shows no steady centred logo: last ink %s'
               % (did, seen))


@case('idle_logo', allow=REFUSALS)
def c_idle_logo(run):
    if run.probe['main']['presentation']:
        raise Skip('display %d takes a Presentation: the menu never needs '
                   'the main window there' % run.main)
    ds = displays()
    sid = find_second(ds, run.probe)
    key = display_key(ds, sid)
    push_logo()
    start(run, cfg={'video_screen1_display': key,
                    'video_screen2_display': key,
                    'video_menu_follows': MENU_FOLLOWS_SEPARATE,
                    'video_menu_display': display_key(ds, run.main),
                    'menu_enable_widgets': 'true',
                    'video_font_enable': 'true',
                    'assets_directory': DEV + '/assets',
                    'menu_rgui_transparency': 'false'})
    settle_on(sid)
    uid = app_uid()
    few_moves(uid, READY, 'after content start')
    opened = mark()
    # The menu's Presentation fails on its display, so RetroArch goes there
    ra_cmd('MENU open', wait=3)
    try:
        wait_log(r'\[SecondDisplay\] Cannot show on display %d\b'
                 % run.main, 15, what='display %d took the menu\'s '
                 'Presentation' % run.main)
        settle_on(run.main)
        menu_drawn(run, run.main, 'idle-menu', ours=True)
    finally:
        ra_cmd('MENU close', wait=3)
        # A move loop fails by name, whatever it broke first
        few_moves(uid, re.escape(opened), 'after the MENU open')

    def screens():
        if not presentation_on(sid):
            return None
        img = run.capture(sid, displays(), 'idle-second')
        return img if img.box(GREEN) and img.box(YELLOW) else None
    wait_until(screens, 15, 'display %d does not show both screens in the '
               'Presentation' % sid)
    # The logo loads through the task queue: wait for it
    img0, box = wait_logo(run, run.main)
    if img0.box(GREEN) or img0.box(YELLOW):
        raise Fail('the idle window on display %d shows a game screen'
                   % run.main)
    cx, cy = (box[0] + box[2]) / 2.0, (box[1] + box[3]) / 2.0
    if (abs(cx - img0.w / 2.0) > img0.w * 0.05
            or abs(cy - img0.h / 2.0) > img0.h * 0.05):
        raise Fail('the logo is not centred: %s on %dx%d'
                   % (box, img0.w, img0.h))
    if box[2] - box[0] > img0.w // 2 or box[3] - box[1] > img0.h // 2:
        raise Fail('more than a logo is drawn: %s on %dx%d'
                   % (box, img0.w, img0.h))
    tag = mark()
    held_tap(run, run.main, img0.w // 2, img0.h // 2)
    time.sleep(1.5)
    if pointers(lines_after(tag)):
        raise Fail('a tap on the idle window reached the core')
    try:
        rows = main_menu()
        k = menu_pick(rows, r'^Settings$', 'main menu')[0]
        img, mbox = menu_drawn(run, run.main, 'idle-menu-2', ours=True)
        if not activity_on(run.main) or not presentation_on(sid):
            raise Fail('opening the menu again moved a window')
        path, hits, at = tap_row(run, run.main, mbox, k)
    finally:
        menu('close')
        few_moves(uid, re.escape(opened), 'after the MENU open')
    if not path.endswith('Settings'):
        raise Fail('a tap at %s on display %d, on entry %d "Settings", left '
                   'the menu at %r' % (at, run.main, k, path or 'its top'))
    if hits:
        raise Fail('the menu tap reached the core at %s' % hits[0])
    return ('menu on display %d (refused), screens on display %d: logo %s '
            'on %dx%d; taps: none to the core on the logo, the menu\'s own '
            'entry when open' % (run.main, sid, box, img0.w, img0.h))


# ---- 06 emulator round: switch, gun_main_none, held_across ----

LIGHTGUN = re.compile(r'\[video_views\] lightgun x=(-?\d+) y=(-?\d+) '
                      r'offscreen=(\d) trigger=(\d)')


def gun_triggers(lines):
    """The test core's lightgun lines with TRIGGER down."""
    return [l for l in lines
            for m in [LIGHTGUN.search(l)] if m and m.group(4) == '1']


def first_after(lines, at, rx):
    pat = re.compile(rx)
    return next((i for i in range(at, len(lines)) if pat.search(lines[i])),
                None)


@case('switch', allow=REFUSALS)
def c_switch(run):
    """Content loaded over a two-screen game that put RetroArch on the
    second display, the new one wanting the main display: one move,
    asked once the new content runs, none while the switch runs."""
    ds = displays()
    sid = find_second(ds, run.probe)
    push_text(ROM, 'ra-dual switch\n')
    start(run, cfg={'video_screen1_display': 'off',
                    'video_screen2_display': display_key(ds, sid)})
    settle_on(sid)
    if run.main in app_windows():
        raise Fail('game 1: RetroArch kept a window on display %d'
                   % run.main)
    img = run.capture(sid, displays(), 'switch-before')
    if not img.box(YELLOW) or img.box(GREEN):
        raise Fail('game 1: display %d does not show screen 2 alone' % sid)
    uid = app_uid()
    # The next content is mapless: Screen 1, Off, shows at its Auto
    # choice, the main display
    ra_cmd('SET_CORE_OPTION video_views_test_map none', wait=3)
    push_text(DEV + '/core-options.cfg',
              cfg_text({'video_views_test_map': 'none'}))
    tag = mark()
    ra_cmd('LOAD_CONTENT %s/%s|%s' % (CORES, TEST_CORE, ROM), wait=3)
    wait_log(r'\[Core\] Loading dynamic libretro core from: ".*%s"'
             % re.escape(TEST_CORE), 20, tag=tag,
             what='the switch loaded no core')
    wait_log(r'\[video_views\] presents=', 20, tag=tag,
             what='the new content did not run')
    settle_on(run.main)
    wait_until(lambda: sid not in app_windows(), 10,
               'after the switch display %d kept a RetroArch window' % sid)
    img0 = run.capture(run.main, displays(), 'switch-after')
    if not img0.box(GREEN) or not img0.box(YELLOW):
        raise Fail('display %d does not show the mapless game' % run.main)
    time.sleep(2.0)
    lines = system_lines()
    at = next((i for i, l in enumerate(lines) if tag in l), None)
    if at is None:
        raise Fail('log marker %s is gone' % tag)
    loaded = first_after(lines, at, r'Loading dynamic libretro core')
    ran = first_after(lines, loaded or at, r'\[video_views\] presents=')
    asks = asks_after(lines, uid, re.escape(tag))
    if len(asks) != 1:
        raise Fail('%d moves asked after LOAD_CONTENT, not 1: %s'
                   % (len(asks), [a[:160] for a in asks[:3]]))
    i = lines.index(asks[0])
    if ran is None or i < ran:
        raise Fail('the move was asked while the switch ran (log line %d, '
                   'the new content ran at %s): %s' % (i, ran, asks[0][:160]))
    return ('game 1 (Screen 1 Off) on display %d; LOAD_CONTENT of a mapless '
            'game: 1 move, asked %d log lines after the new content ran, '
            'onto display %d with both screens; display %d free'
            % (sid, i - ran, run.main, sid))


@case('gun_main_none', allow=REFUSALS)
def c_gun_main_none(run):
    """Light gun taps: TRIGGER from a main window's screen and its
    letterbox, none from a main window that shows no screen."""
    if run.probe['main']['presentation']:
        raise Skip('display %d takes a Presentation: the main window there '
                   'always shows a screen' % run.main)
    start(run)
    settle_on(run.main)
    sid, img2, box2, img0, box0 = check_routed(run, 'gun-routed')
    x0, y0, x1, y1 = box0
    spots = [('screen 1', ((x0 + x1) // 2, (y0 + y1) // 2))]
    if x0 >= 40:
        spots.append(('the letterbox', (x0 // 2, (y0 + y1) // 2)))
    elif y0 >= 40:
        spots.append(('the letterbox', ((x0 + x1) // 2, y0 // 2)))
    else:
        raise Fail('harness: screen 1 at %s fills display %d, no letterbox'
                   % (box0, run.main))
    for what, (x, y) in spots:
        tag = mark()
        held_tap(run, run.main, x, y)
        time.sleep(1.5)
        if not gun_triggers(lines_after(tag)):
            raise Fail('a tap on %s at (%d,%d) on display %d gave the core '
                       'no lightgun TRIGGER' % (what, x, y, run.main))
    ds = displays()
    key = display_key(ds, sid)
    start(run, cfg={'video_screen1_display': key,
                    'video_screen2_display': key,
                    'video_menu_follows': MENU_FOLLOWS_SEPARATE,
                    'video_menu_display': display_key(ds, run.main),
                    'menu_rgui_transparency': 'false'})
    settle_on(sid)
    # The menu's Presentation fails on the main display, so RetroArch
    # goes there and stays once the menu closes
    ra_cmd('MENU open', wait=3)
    try:
        wait_log(r'\[SecondDisplay\] Cannot show on display %d\b'
                 % run.main, 15, what='display %d took the menu\'s '
                 'Presentation' % run.main)
        settle_on(run.main)
        menu_drawn(run, run.main, 'gun-menu', ours=True)
    finally:
        ra_cmd('MENU close', wait=3)

    def screens():
        if not presentation_on(sid):
            return None
        img = run.capture(sid, displays(), 'gun-second')
        return img if img.box(GREEN) and img.box(YELLOW) else None
    wait_until(screens, 15, 'display %d does not show both screens in the '
               'Presentation' % sid)
    time.sleep(1.0)
    img = run.capture(run.main, displays(), 'gun-main-none')
    if img.box(GREEN) or img.box(YELLOW):
        raise Fail('the main window on display %d shows a game screen'
                   % run.main)
    for fx, fy in ((0.5, 0.5), (0.2, 0.8)):
        x, y = int(img.w * fx), int(img.h * fy)
        tag = mark()
        held_tap(run, run.main, x, y)
        time.sleep(1.5)
        lines = lines_after(tag)
        hit = gun_triggers(lines)
        if hit:
            raise Fail('a tap at (%d,%d) on display %d, a main window with '
                       'no screen, gave the core a lightgun TRIGGER: %s'
                       % (x, y, run.main, hit[0].strip()[:160]))
        if pointers(lines):
            raise Fail('a tap at (%d,%d) on display %d, a main window with '
                       'no screen, reached the core at %s'
                       % (x, y, run.main, pointers(lines)[0]))
    return ('TRIGGER from taps on screen 1 and its letterbox on display %d; '
            'none from 2 taps on display %d showing no screen (RELOAD: the '
            'test core does not log it)' % (run.main, run.main))


def touch_down(run, did, x, y):
    require_front(ra_display(run))
    sh('input -d %d motionevent DOWN %d %d' % (did, x, y))


def touch_move(did, x, y):
    sh('input -d %d motionevent MOVE %d %d' % (did, x, y))


SPLIT = 'ra-dual-split'


def menu_then_up(cmd, did, x, y):
    """A MENU command and a MENU show, then the held finger lifted at
    once: well inside the long press time of the window the menu goes
    to. The two answers."""
    out = sh("echo -n 'MENU %s' | timeout 3 nc -u 127.0.0.1 %d; echo %s; "
             "echo -n 'MENU show' | timeout 3 nc -u 127.0.0.1 %d; "
             "input -d %d motionevent UP %d %d"
             % (cmd, PORT, SPLIT, PORT, did, x, y), check=False, timeout=40)
    a, _, b = out.partition(SPLIT)
    return a.strip(), b.strip()


def open_then_up(did, x, y, after):
    """MENU open with a finger down there, the finger lifted after s once
    the menu answers (two frames after it opened; toybox nc then waits out
    its timeout, which menu_then_up's lift waits for): the answer."""
    f = '/data/local/tmp/ra-dual-open.txt'
    return sh("rm -f %s; (echo -n 'MENU open' | timeout 3 nc -u 127.0.0.1 %d "
              "> %s) & n=0; while [ ! -s %s ] && [ $n -lt 60 ]; do "
              "sleep 0.05; n=$((n+1)); done; sleep %.2f; "
              "input -d %d motionevent UP %d %d; wait; cat %s"
              % (f, PORT, f, f, after, did, x, y, f), check=False,
              timeout=40)


def menu_text():
    """MENU show, its rows and selection, to compare."""
    return ra_cmd('MENU show', wait=3)


def quick_tap(run, did, x, y):
    """Shorter than the menu's short press: Material UI acts only on a
    tap."""
    require_front(ra_display(run))
    sh('input -d %d swipe %d %d %d %d 100' % (did, x, y, x, y))


def covered(run, did, colour, area, label, timeout=10):
    """Display did once the menu covers its game screen: colour's box gone
    or under half its area."""
    def drawn():
        img = run.capture(did, displays(), label)
        b = img.box(colour)
        return img if ((b is None or (b[2] - b[0]) * (b[3] - b[1])
                        < area // 2) and ink(img)) else None
    return wait_until(drawn, timeout, '%s: no menu drawn on display %d'
                      % (label, did))


def held_open_rgui(run, sid):
    """RGUI: a finger on the Presentation's first row as the menu opens
    there from the main window: the menu stays open on the lift; the
    next tap there resumes the game."""
    start(run, cfg={'video_menu_follows': '1',
                    'menu_rgui_transparency': 'false'}, menu_only=True)
    settle_on(sid)
    img, box = menu_drawn(run, sid, 'held-open-nogame', ours=True)
    start_core()
    settle_on(run.main)
    sid, img2, box2, img0, box0 = check_routed(run, 'held-open')
    area = (box2[2] - box2[0]) * (box2[3] - box2[1])
    # RGUI over the game is not the no-game frame: its first row from
    # the menu opened once with no finger
    menu('open')
    cal = covered(run, sid, YELLOW, area, 'held-open-calibrate')
    b = menu_frame(cal, YELLOW)
    if not b or not rgui_box(cal, b):
        raise Fail('harness: held-open: frame %s is not RGUI\'s (no-game '
                   'frame %s)' % (b, box))
    x, y = rgui_row(b, 0)
    menu('close')
    if not game_back(run, sid, YELLOW, area, 'held-open-closed'):
        raise Fail('harness: held-open: the game did not come back after '
                   'MENU close')
    touch_down(run, sid, x, y)
    rows = [(int(m.group(1)), m.group(2), m.group(3))
            for m in (MENU_ROW.match(l)
                      for l in menu_then_up('open', sid, x, y)[0]
                      .splitlines())
            if m]
    if not rows or not re.match(r'^Resume', rows[0][1]):
        raise Fail('harness: the Quick Menu\'s first row is %s, not Resume'
                   % (rows[:1],))
    time.sleep(1.5)
    try:
        shown = covered(run, sid, YELLOW, area, 'held-open-lifted')
    except Fail:
        raise Fail('rgui, opening onto the Presentation: display %d shows no '
                   'menu after the lift of a finger held on "%s" at (%d,%d)'
                   % (sid, rows[0][1], x, y))
    # the game shows around RGUI's frame: ink() would take it in
    b = menu_frame(shown, YELLOW)
    if not b or not rgui_box(shown, b):
        raise Fail('rgui, opening onto the Presentation: display %d shows no '
                   'RGUI frame after the lift: frame %s' % (sid, b))
    if abs(rgui_row(b, 0)[1] - y) > RGUI_STRIDE * (b[3] - b[1]) / RGUI_LINES:
        raise Fail('harness: RGUI\'s first row is at y %d in the '
                   'Presentation, the finger was at %d'
                   % (rgui_row(b, 0)[1], y))
    quick_tap(run, sid, x, y)
    wait_until(lambda: (lambda bb: bb and (bb[2] - bb[0]) * (bb[3] - bb[1])
                        >= area * 0.9)(run.capture(sid, displays(),
                                                   'held-open-resumed')
                                       .box(YELLOW)), 10,
               'rgui: the first tap after the lift, on "%s" at (%d,%d) on '
               'display %d, did not resume the game' % (rows[0][1], x, y,
                                                        sid))
    return ('menu opened onto display %d under a finger on "%s": still open '
            'after the lift; the next tap resumed' % (sid, rows[0][1]))


def held_across_menu(run, driver):
    """A finger held where the menu goes as Menu Follows moves it off the
    Presentation and back: nothing on the lift; the next tap acts."""
    start(run, cfg={'video_menu_follows': '1', 'menu_driver': driver,
                    'menu_rgui_transparency': 'false'})
    settle_on(run.main)
    sid, img2, box2, img0, box0 = check_routed(run, 'held-%s' % driver)
    game = {run.main: (GREEN, (box0[2] - box0[0]) * (box0[3] - box0[1])),
            sid: (YELLOW, (box2[2] - box2[0]) * (box2[3] - box2[1]))}
    notes = []
    try:
        rows = stereo_rows()
        i = menu_pick(rows, r'^Menu Follows$', 'rows')[0]
        k = menu_pick(rows, r'^Notifications$', 'rows')[0]
        # The Notifications row in each window, the menu moved there once
        # with no finger: RGUI's frame, or Material UI's highlight
        def where(did, img):
            if driver == 'rgui':
                return (displays()[did]['size'][0] // 2,
                        rgui_row(ink(img), k)[1])
            return mui_row(run, did, rows, k, 'held-%s-%d' % (driver, did))
        points = {sid: where(sid, covered(run, sid, YELLOW, game[sid][1],
                                          'held-%s-open' % driver))}
        menu('left', i)
        points[run.main] = where(run.main, covered(
            run, run.main, GREEN, game[run.main][1],
            'held-%s-calibrate' % driver))
        rows = menu('right', i)
        covered(run, sid, YELLOW, game[sid][1], 'held-%s-back' % driver)
        for word, want, did, how in (
                ('left', 'Game Screen 1', run.main, 'off the Presentation'),
                ('right', 'Game Screen 2', sid, 'onto the Presentation')):
            i = menu_pick(rows, r'^Menu Follows$', 'rows')[0]
            x, y = points[did]
            touch_down(run, did, x, y)
            answer, held = menu_then_up('%s %d' % (word, i), did, x, y)
            got = [m.group(3) for m in (MENU_ROW.match(l)
                                        for l in answer.splitlines())
                   if m and m.group(2) == 'Menu Follows']
            if got != [want]:
                raise Fail('%s on Menu Follows gave %s, not %r'
                           % (word, got, want))
            covered(run, did, game[did][0], game[did][1],
                    'held-%s-%d' % (driver, did))
            if not activity_on(run.main):
                raise Fail('Menu Follows %s moved RetroArch' % want)
            before = menu_text()
            time.sleep(1.0)
            if menu_text() != before:
                raise Fail('harness: the %s menu changes on its own' % driver)
            if before != held:
                raise Fail('%s, %s: a finger held at (%d,%d) on display %d '
                           'as the menu moved there reached it on the lift: '
                           'the menu was %r, then %r'
                           % (driver, how, x, y, did, held[:300],
                              before[:300]))
            quick_tap(run, did, x, y)
            time.sleep(1.5)
            tapped = menu_text()
            if tapped == before:
                raise Fail('%s, %s: the first tap after the lift, at (%d,%d) '
                           'on display %d, did nothing'
                           % (driver, how, x, y, did))
            path = menu_state()[0]
            notes.append('%s: held finger on Notifications dropped, the '
                         'next tap acted (%s)' % (how, path[-40:] or 'top'))
            rows = stereo_rows()
            if not activity_on(run.main):
                raise Fail('the tap on display %d moved RetroArch' % did)
    finally:
        menu('close')
    return notes


def diff_rows(a, b):
    """The pixel rows that differ between two same-size images."""
    n = a.w * 4
    return set(y for y in range(min(a.h, b.h))
               if a.rgba[y * n:(y + 1) * n] != b.rgba[y * n:(y + 1) * n])


def bands(ys, gap=3):
    """Runs of ys, as [first, last], a gap of at most gap inside one."""
    out = []
    for y in sorted(ys):
        if out and y - out[-1][1] <= gap:
            out[-1][1] = y
        else:
            out.append([y, y])
    return out


def mui_row(run, did, rows, k, label):
    """The middle of Material UI's entry k on display did, from its
    highlight in captures with k and the entries beside it selected."""
    n = len(rows)
    if n < 3:
        raise Fail('harness: %d rows, too few to find one' % n)
    if k == 0:
        sel = (0, 1, 2)
    elif k == n - 1:
        sel = (k - 2, k - 1, k)
    else:
        sel = (k - 1, k, k + 1)
    # this emulator's lone torn frame (half drawn along the quad's
    # diagonal, rework-06-t2-close-rca.md) changes most rows: retaken
    for attempt in range(3):
        shots = {}
        for i in sel:
            menu('select', rows[i][0])
            time.sleep(0.8)
            shots[i] = run.capture(did, displays(), '%s-sel%d%s'
                                   % (label, i, '-%d' % attempt
                                      if attempt else ''))
        a, b, c = (shots[i] for i in sel)
        if (a.w, a.h) != (b.w, b.h) or (b.w, b.h) != (c.w, c.h):
            raise Fail('harness: display %d changed size while %s was '
                       'measured' % (did, label))
        ab, bc = diff_rows(a, b), diff_rows(b, c)
        if len(ab | bc) <= b.h // 2:
            break
        print('note held_across: %s: %d of %d pixel rows changed, '
              'measured again' % (label, len(ab | bc), b.h), flush=True)
    if len(ab | bc) > b.h // 2:
        raise Fail('harness: %s: the list scrolled as its selection moved '
                   '(%d of %d pixel rows changed)'
                   % (label, len(ab | bc), b.h))
    if k == 0:
        ys, img, other = ab - bc, a, b
    elif k == n - 1:
        ys, img, other = bc - ab, c, b
    else:
        ys, img, other = ab & bc, b, a
    runs = bands(ys)
    if not runs:
        raise Fail('harness: %s: no highlight found for entry %d on '
                   'display %d' % (label, k, did))
    y0, y1 = max(runs, key=lambda r: r[1] - r[0])
    if not img.h * 0.02 <= y1 - y0 <= img.h * 0.3:
        raise Fail('harness: %s: entry %d\'s highlight on display %d is '
                   'rows %d-%d' % (label, k, did, y0, y1))
    y = (y0 + y1) // 2
    row = y * img.w * 4
    xs = [x for x in range(0, img.w, 2)
          if img.rgba[row + x * 4:row + x * 4 + 3]
          != other.rgba[row + x * 4:row + x * 4 + 3]]
    if not xs:
        raise Fail('harness: %s: entry %d\'s highlight has no width'
                   % (label, k))
    return ((xs[0] + xs[-1]) // 2, y)


def game_back(run, did, colour, area, label, timeout=10):
    """Display did shows its game screen again at 0.9 of its area or more:
    the menu closed."""
    def back():
        b = run.capture(did, displays(), label).box(colour)
        return b and (b[2] - b[0]) * (b[3] - b[1]) >= area * 0.9
    try:
        wait_until(back, timeout, 'no game')
        return True
    except Fail:
        return False


def held_open(run, sid, drv, where, spot='row0', hold=0.0):
    """A finger on the Quick Menu's first row (Resume), or on its header
    (spot 'header': a cancel), as the menu opens under it, held hold s
    after the open, then lifted: the menu stays open on the Quick Menu;
    the next tap on Resume resumes. where: presentation (Menu Follows
    Screen 2), main (Screen 1, Screen 2 still in the Presentation) or
    windows-off."""
    label = 'open-%s-%s%s%s' % (drv, where,
                                '' if spot == 'row0' else '-' + spot,
                                '-hold' if hold else '')
    cfg = {'menu_driver': drv, 'menu_rgui_transparency': 'false',
           'video_menu_follows': '1' if where == 'presentation' else '0'}
    if where == 'windows-off':
        cfg['video_windows_enable'] = 'false'
    start(run, cfg=cfg)
    settle_on(run.main)
    if where == 'windows-off':
        did, colour = run.main, GREEN
        box = run.capture(did, displays(), label + '-game').box(GREEN)
        if not box:
            raise Fail('%s: display %d shows no green screen' % (label, did))
    else:
        s, img2, box2, img0, box0 = check_routed(run, label)
        did, colour, box = ((s, YELLOW, box2) if where == 'presentation'
                            else (run.main, GREEN, box0))
    area = (box[2] - box[0]) * (box[3] - box[1])
    # the first row's place, the menu opened once with no finger
    rows = menu('open')
    if not rows or not re.match(r'^Resume', rows[0][1]):
        raise Fail('harness: the Quick Menu\'s first row is %s, not Resume'
                   % (rows[:1],))
    shown = covered(run, did, colour, area, label + '-menu')
    if drv == 'rgui':
        b = menu_frame(shown, colour)
        if not b or not rgui_box(shown, b):
            raise Fail('harness: %s: frame %s is not RGUI\'s'
                       % (label, b))
        x, y = rgui_row(b, 0)
        # the black bar above the frame, the probe's point; else the
        # title line
        hx, hy = ((b[0] + b[2]) // 2,
                  b[1] // 2 if b[1] >= 20
                  else b[1] + int(18 * (b[3] - b[1]) / RGUI_LINES))
    else:
        x, y = mui_row(run, did, rows, 0, label)
        hx, hy = x, max(4, y // 3)
        menu('select', rows[0][0])
    opened = menu_state()[0]
    menu('close')
    if not game_back(run, did, colour, area, label + '-closed'):
        raise Fail('harness: %s: the game did not come back after MENU '
                   'close' % label)
    px, py = (x, y) if spot == 'row0' else (hx, hy)
    touch_down(run, did, px, py)
    if spot == 'row0':
        # lifted 0.2 s after the open, or held hold s through it
        answer = open_then_up(did, px, py, hold or 0.2)
    else:
        answer = menu_then_up('open', did, px, py)[0]
    rows = [(int(m.group(1)), m.group(2), m.group(3))
            for m in (MENU_ROW.match(l) for l in answer.splitlines())
            if m]
    if not rows or not re.match(r'^Resume', rows[0][1]):
        raise Fail('harness: the Quick Menu\'s first row is %s, not Resume'
                   % (rows[:1],))
    if selected_row(answer) != 0:
        raise Fail('harness: %s: the menu opened with entry %s selected, '
                   'not Resume' % (label, selected_row(answer)))
    time.sleep(1.5)
    how = '%s, opening %s%s%s' % (drv, {
        'presentation': 'onto the Presentation',
        'main': 'in the main window',
        'windows-off': 'with windows off'}[where],
        '' if spot == 'row0' else ', finger on the header',
        ', held %.1f s' % hold if hold else '')
    where_at = ('the header' if spot == 'header' else '"%s"' % rows[0][1])
    if menu_text() == 'CLOSED':
        raise Fail('%s: the menu closed on the lift of a finger held on '
                   '%s at (%d,%d) on display %d'
                   % (how, where_at, px, py, did))
    path = menu_state()[0]
    if path != opened:
        raise Fail('%s: the lift of a finger held on %s at (%d,%d) on '
                   'display %d moved the menu from %r to %r'
                   % (how, where_at, px, py, did, opened, path))
    try:
        covered(run, did, colour, area, label + '-lifted')
    except Fail:
        raise Fail('%s: display %d shows no menu after the lift of a finger '
                   'held on %s at (%d,%d)' % (how, did, where_at, px, py))
    quick_tap(run, did, x, y)
    if not game_back(run, did, colour, area, label + '-resumed'):
        raise Fail('%s: the first tap after the lift, on "%s" at (%d,%d) on '
                   'display %d, did not resume the game'
                   % (how, rows[0][1], x, y, did))
    return ('opened %s under a finger on %s%s: still on %r after the lift; '
            'the next tap on Resume resumed'
            % (where, where_at, ' held %.1f s' % hold if hold else '',
               path))


def quick_menu_from(rows):
    """From a Settings list back to the Quick Menu by the menu's own
    entries, no close and open (Material UI: the Main tab, left of its
    Settings tab): its rows."""
    tabs = False
    for _ in range(8):
        if any(r[1] == 'Quick Menu' for r in rows):
            return menu('a', menu_pick(rows, r'^Quick Menu$', 'rows')[0])
        # RGUI's Settings list has the tab's rows, one level down
        tabs = tabs or (settings_tab(rows) and not menu_state()[0])
        rows = menu('left') if tabs else menu('b')
    raise Fail('harness: no way back to the Quick Menu: %s'
               % [r[1] for r in rows])


def held_across_resume(run, driver):
    """Round B's window change with Resume, which acts on release, under
    the finger: a finger held where the menu goes as Menu Follows moves
    it there, the menu taken back to the Quick Menu with the finger still
    down, then lifted on Resume: nothing on the lift; the next tap on
    Resume resumes."""
    start(run, cfg={'video_menu_follows': '1', 'menu_driver': driver,
                    'menu_rgui_transparency': 'false'})
    settle_on(run.main)
    sid, img2, box2, img0, box0 = check_routed(run, 'resume-%s' % driver)
    game = {run.main: (GREEN, (box0[2] - box0[0]) * (box0[3] - box0[1])),
            sid: (YELLOW, (box2[2] - box2[0]) * (box2[3] - box2[1]))}
    notes = []

    def resume_at(did, rows, label):
        if not rows or not re.match(r'^Resume', rows[0][1]):
            raise Fail('harness: the Quick Menu\'s first row is %s, not '
                       'Resume' % (rows[:1],))
        img = covered(run, did, game[did][0], game[did][1], label)
        if driver == 'rgui':
            b = menu_frame(img, game[did][0])
            if not b or not rgui_box(img, b):
                raise Fail('harness: %s: frame %s is not RGUI\'s'
                           % (label, b))
            return rgui_row(b, 0)
        at = mui_row(run, did, rows, 0, label)
        menu('select', rows[0][0])
        return at
    try:
        # Resume's place in each window, the menu moved there once with
        # no finger
        points = {sid: resume_at(sid, menu('open'),
                                 'resume-%s-open' % driver)}
        rows = stereo_rows()
        i = menu_pick(rows, r'^Menu Follows$', 'rows')[0]
        rows = menu('left', i)
        covered(run, run.main, GREEN, game[run.main][1],
                'resume-%s-moved' % driver)
        points[run.main] = resume_at(run.main, quick_menu_from(rows),
                                     'resume-%s-calibrate' % driver)
        rows = stereo_rows()
        i = menu_pick(rows, r'^Menu Follows$', 'rows')[0]
        rows = menu('right', i)
        covered(run, sid, YELLOW, game[sid][1], 'resume-%s-back' % driver)
        for word, want, did, how in (
                ('left', 'Game Screen 1', run.main, 'off the Presentation'),
                ('right', 'Game Screen 2', sid, 'onto the Presentation')):
            i = menu_pick(rows, r'^Menu Follows$', 'rows')[0]
            x, y = points[did]
            touch_down(run, did, x, y)
            rows = menu(word, i)
            got = [r[2] for r in rows if r[1] == 'Menu Follows']
            if got != [want]:
                raise Fail('%s on Menu Follows gave %s, not %r'
                           % (word, got, want))
            covered(run, did, game[did][0], game[did][1],
                    'resume-%s-%d' % (driver, did))
            if not activity_on(run.main):
                raise Fail('Menu Follows %s moved RetroArch' % want)
            rows = quick_menu_from(rows)
            if not rows or not re.match(r'^Resume', rows[0][1]):
                raise Fail('harness: the Quick Menu\'s first row is %s, '
                           'not Resume' % (rows[:1],))
            if menu_selected() != 0:
                menu('select', rows[0][0])
            covered(run, did, game[did][0], game[did][1],
                    'resume-%s-%d-quick' % (driver, did))
            before = menu_text()
            sh('input -d %d motionevent UP %d %d' % (did, x, y))
            time.sleep(1.5)
            after = menu_text()
            if after == 'CLOSED':
                raise Fail('%s, %s: a finger held at (%d,%d) on display %d '
                           'as the menu moved there, let go on "%s", closed '
                           'the menu' % (driver, how, x, y, did, rows[0][1]))
            if after != before:
                raise Fail('%s, %s: a finger held at (%d,%d) on display %d '
                           'as the menu moved there reached it on the lift '
                           'on "%s": the menu was %r, then %r'
                           % (driver, how, x, y, did, rows[0][1],
                              before[:300], after[:300]))
            try:
                covered(run, did, game[did][0], game[did][1],
                        'resume-%s-%d-lifted' % (driver, did))
            except Fail:
                raise Fail('%s, %s: display %d shows no menu after the lift '
                           'on "%s"' % (driver, how, did, rows[0][1]))
            quick_tap(run, did, x, y)
            if not game_back(run, did, game[did][0], game[did][1],
                             'resume-%s-%d-resumed' % (driver, did)):
                raise Fail('%s, %s: the first tap after the lift, on "%s" '
                           'at (%d,%d) on display %d, did not resume the '
                           'game' % (driver, how, rows[0][1], x, y, did))
            notes.append('%s: held finger let go on Resume dropped, the '
                         'next tap resumed' % how)
            if not activity_on(run.main):
                raise Fail('the tap on display %d moved RetroArch' % did)
            rows = stereo_rows()
    finally:
        menu('close')
    return notes


def held_back_open(run, sid, drv, where):
    """Android's Back held as the menu opens (a cancel through the open),
    let go after it: the menu stays on the Quick Menu; the next Back
    acts. where as held_open's."""
    label = 'back-%s-%s' % (drv, where)
    cfg = {'menu_driver': drv, 'menu_rgui_transparency': 'false',
           'video_menu_follows': '1' if where == 'presentation' else '0'}
    if where == 'windows-off':
        cfg['video_windows_enable'] = 'false'
    hold = hold_setup()
    if not hold[1].startswith('input keyboard keyevent --duration'):
        raise Skip('Back held through an open: needs input --duration, '
                   'this target holds keys by %s' % hold[1])
    start(run, cfg=cfg)
    settle_on(run.main)
    if where == 'windows-off':
        did, colour = run.main, GREEN
        box = run.capture(did, displays(), label + '-game').box(GREEN)
        if not box:
            raise Fail('%s: display %d shows no green screen' % (label, did))
    else:
        s, img2, box2, img0, box0 = check_routed(run, label)
        did, colour, box = ((s, YELLOW, box2) if where == 'presentation'
                            else (run.main, GREEN, box0))
    area = (box[2] - box[0]) * (box[3] - box[1])
    # Native input drops a keyboard's first key: it only registers it
    require_front(run.main)
    sh(hold[0])
    time.sleep(1.0)
    require_front(run.main)
    out = sh("input keyboard keyevent --duration 1600 KEYCODE_BACK & "
             "sleep 0.4; echo -n 'MENU show' | timeout 3 nc -u 127.0.0.1 %d; "
             "echo %s; echo -n 'MENU open' | timeout 3 nc -u 127.0.0.1 %d; "
             "wait" % (PORT, SPLIT, PORT), check=False, timeout=40)
    pre, _, answer = out.partition(SPLIT)
    pre = pre.strip()
    paths = [l.strip() for l in answer.splitlines() if l.startswith('PATH ')]
    rows = [(int(m.group(1)), m.group(2), m.group(3))
            for m in (MENU_ROW.match(l) for l in answer.splitlines()) if m]
    if not rows or not re.match(r'^Resume', rows[0][1]):
        raise Fail('harness: %s: the menu opened on %s, not the Quick Menu '
                   '(before the open: %r)' % (label, rows[:1], pre[:80]))
    opened = paths[0] if paths else ''
    time.sleep(1.5)
    how = '%s, Back held through the open %s' % (drv, {
        'presentation': 'onto the Presentation',
        'main': 'in the main window',
        'windows-off': 'with windows off'}[where])
    path = menu_state()[0]
    if menu_text() == 'CLOSED':
        raise Fail('%s: the menu closed on the release of Back' % how)
    if path != opened:
        raise Fail('%s: the release of Back took the menu from %r to %r'
                   % (how, opened, path))
    try:
        covered(run, did, colour, area, label + '-released')
    except Fail:
        raise Fail('%s: display %d shows no menu after Back was let go'
                   % (how, did))
    require_front(run.main)
    sh('input keyboard keyevent KEYCODE_BACK')
    time.sleep(1.5)
    after = menu_text()
    path2 = menu_state()[0] if after != 'CLOSED' else 'CLOSED'
    if path2 == opened:
        raise Fail('%s: the next Back, after the release, did nothing (the '
                   'menu still on %r)' % (how, opened))
    if path2 != 'CLOSED':
        menu('close')
    if not game_back(run, did, colour, area, label + '-closed'):
        raise Fail('harness: %s: the game did not come back after MENU '
                   'close' % label)
    return ('Back held through the open %s (menu %s before the open): '
            'still on %r after the release; the next Back went to %r'
            % (where, 'closed' if pre.startswith('CLOSED') else
               'open: %r' % pre[:40], opened, path2))


MUI_SCROLLBAR = (159, 159, 159)  # the thumb in Ozone Dark, Material UI's default


def mui_thumb(img, tol=12):
    """The middle of Material UI's scrollbar thumb in img, or None."""
    found = []
    for x in range(img.w - 1, int(img.w * 0.6), -1):
        best, y0 = (0, 0, 0), None
        for y in range(0, img.h, 2):
            if img.near(x, y, MUI_SCROLLBAR, tol):
                y0 = y if y0 is None else y0
                if y - y0 > best[0]:
                    best = (y - y0, y0, y)
            else:
                y0 = None
        if best[0] >= 40:
            found.append((x, best[1], best[2]))
        elif found:
            break
    if not found:
        return None
    xs = [f[0] for f in found]
    return ((min(xs) + max(xs)) // 2, (found[0][1] + found[0][2]) // 2)


def scrollbar_watch(run, sid):
    """Watch item, not judged: Material UI scaled up until the Stereo
    rows scroll in the Presentation; a finger dragging its scrollbar
    there as the menu moves to the main window, lifted, then a scroll
    and a tap there. Fails only when the menu stops answering."""
    start(run, cfg={'video_menu_follows': '1', 'menu_driver': 'glui',
                    'menu_scale_factor': '3.0'})
    settle_on(run.main)
    s, img2, box2, img0, box0 = check_routed(run, 'bar')
    area = (box0[2] - box0[0]) * (box0[3] - box0[1])
    try:
        for _ in range(3):
            rows = stereo_rows(settle=1.0)
            if any(r[1] == 'Menu Follows' for r in rows):
                break
        i = menu_pick(rows, r'^Menu Follows$', 'rows')[0]
        menu('select', rows[0][0])
        time.sleep(1.0)
        top = run.capture(sid, displays(), 'bar-top')
        at = mui_thumb(top)
        if not at:
            note = ('scrollbar watch: no scrollbar thumb on display %d at '
                    'menu scale 3.0' % sid)
            print('note held_across: %s' % note, flush=True)
            return note
        x, y = at
        touch_down(run, sid, x, y)
        # dragged to the end, where Menu Follows is on screen
        end_y = int(top.h * 0.9)
        for yy in (y + 40, y + 80, (y + end_y) // 2, end_y):
            touch_move(sid, x, yy)
        time.sleep(0.5)
        dragged = run.capture(sid, displays(), 'bar-dragged')
        answer, held = menu_then_up('left %d' % i, sid, x, end_y)
        got = [m.group(3) for m in (MENU_ROW.match(l)
                                    for l in answer.splitlines())
               if m and m.group(2) == 'Menu Follows']
        if got != ['Game Screen 1']:
            note = ('scrollbar watch: not run: with a finger dragging the '
                    'scrollbar on display %d (drag changed %.3f), MENU left '
                    'on Menu Follows left it %s'
                    % (sid, dragged.differs(top), got))
            print('note held_across: %s' % note, flush=True)
            return note
        a = covered(run, run.main, GREEN, area, 'bar-main-1')
        time.sleep(1.0)
        b = run.capture(run.main, displays(), 'bar-main-2')
        w, h = displays()[run.main]['size']
        require_front(ra_display(run))
        sh('input -d %d swipe %d %d %d %d 400'
           % (run.main, w // 2, int(h * 0.8), w // 2, int(h * 0.4)))
        time.sleep(1.5)
        c = run.capture(run.main, displays(), 'bar-main-3')
        scrolled = menu_text()
        quick_tap(run, run.main, w // 2, int(h * 0.6))
        time.sleep(1.5)
        tapped = menu_text()
        d = run.capture(run.main, displays(), 'bar-main-4')
        if not tapped or tapped.startswith('MENU ERROR'):
            time.sleep(2.0)
            tapped = menu_text()
            if not tapped or tapped.startswith('MENU ERROR'):
                raise Fail('scrollbar watch: the menu stopped answering '
                           'after the scroll and tap on display %d: %r'
                           % (run.main, tapped[:120]))
        note = ('scrollbar watch: thumb at (%d,%d) on display %d, the drag '
                'changed %.3f of it; after the move to display %d the list '
                'changed %.3f in 1 s untouched (thumb %s, then %s); the '
                'scroll changed %.3f of it (thumb now %s); the next tap %s '
                '(image change %.3f), the menu answers (%s)'
                % (x, y, sid, dragged.differs(top), run.main, b.differs(a),
                   mui_thumb(a), mui_thumb(b), c.differs(b), mui_thumb(c),
                   'acted' if tapped != scrolled else 'did nothing',
                   d.differs(c), menu_state()[0][-40:] or 'top'))
    finally:
        menu('close')
    print('note held_across: %s' % note, flush=True)
    return note


@case('held_across')
def c_held_across(run):
    sid = find_second(displays(), run.probe)
    notes = ['rgui: ' + held_open_rgui(run, sid)]
    for drv in ('rgui', 'glui'):
        for where in ('presentation', 'main', 'windows-off'):
            if (drv, where) != ('rgui', 'presentation'):
                notes.append('%s: %s' % (drv, held_open(run, sid, drv,
                                                        where)))
            notes.append('%s: %s' % (drv, held_open(run, sid, drv, where,
                                                    hold=1.6)))
            notes.append('%s: %s' % (drv, held_open(run, sid, drv, where,
                                                    spot='header')))
            notes.append('%s: %s' % (drv, held_back_open(run, sid, drv,
                                                         where)))
        notes.append('%s: %s' % (drv, '; '.join(held_across_menu(run, drv))))
        notes.append('%s: %s' % (drv, '; '.join(held_across_resume(run,
                                                                   drv))))
    notes.append('glui: ' + scrollbar_watch(run, sid))
    return ' | '.join(notes)


THREADTIME = re.compile(r'^\d\d-\d\d (\d\d):(\d\d):(\d\d(?:\.\d+)?)\s')
START_WINDOW = 2.0              # s, VIDEO_WINDOWS_MOVE_USEC


def line_time(l):
    """A logcat -v threadtime line's time of day, s."""
    m = THREADTIME.match(l)
    if not m:
        raise Fail('harness: no time on log line %r' % l[:80])
    return (int(m.group(1)) * 3600 + int(m.group(2)) * 60
            + float(m.group(3)))


def shows_on(run, did, label, alone, view=None, timeout=10):
    """Display did shows screen 2 alone at view's shape (alone), or the
    whole mapless frame, green and yellow."""
    why = ['nothing']
    end = time.time() + timeout
    while True:
        img = run.capture(did, displays(), label)
        g, y = img.box(GREEN), img.box(YELLOW)
        if alone and y and not g:
            got = float(y[2] - y[0]) / (y[3] - y[1])
            if abs(got / (float(view[2]) / view[3]) - 1.0) <= 0.05:
                return img
            why[0] = 'screen 2 at %dx%d, not %d:%d' % (
                y[2] - y[0], y[3] - y[1], view[2], view[3])
        elif not alone and g and y:
            return img
        else:
            why[0] = 'green %s, yellow %s' % (g, y)
        if time.time() > end:
            raise Fail('%s: display %d shows %s, not %s' % (
                label, did, why[0], 'screen 2 alone' if alone
                else 'the whole mapless frame'))
        time.sleep(0.5)


# first map, the one it changes to, the frame it changes at, whether that
# is after the start window
SETTLE_MAPS = (('none', 'ds', 10, False),
               ('ds', 'none', 5, False),
               ('ds', 'none', 300, True),
               ('none', 'ds', 300, True))
# a Vulkan hw core keeps its frame size: only none and 3ds swap
SETTLE_VK = (('none', '3ds', 10, False),
             ('3ds', 'none', 300, True))


def map_settle_one(run, ds, sid, first, then, frame, late, hw=None):
    """Screen 1 Off, Screen 2 on display sid; the test core's map goes
    from first to then at frame. In the start window: at most two moves,
    then the route of the final map. After it: routed again, no move."""
    label = 'settle-%s-%s-%d%s' % (first, then, frame,
                                   '-' + hw if hw else '')
    opts = {'video_views_test_map': first,
            'video_views_test_map_change': then,
            'video_views_test_map_change_frame': str(frame)}
    if hw:
        opts['video_views_test_hw'] = hw
    start(run, cfg={'video_screen1_display': 'off',
                    'video_screen2_display': display_key(ds, sid)},
          opts=opts)
    changed = r'\[video_views\] map changed at frame %d\b' % frame
    wait_log(changed, 60, what='%s: the core logged no map change at frame '
             '%d' % (label, frame))
    two = then != 'none'
    wait_log(r'\[Video\] View map: \d+ views, %s screens'
             % ('[12]' if two else '0'), 10,
             what='%s: RetroArch logged no %s map' % (label, then))
    # the final map's display: mapless shows Screen 1 at its Auto
    # choice, the main display; after the window RetroArch stays put
    if late:
        own = run.main if first == 'none' else sid
        time.sleep(3.0)
    else:
        own = sid if two else run.main
    settle_on(own, timeout=30)
    other = run.main if own == sid else sid
    if two and own != sid:
        wait_until(lambda: presentation_on(sid), 10,
                   '%s: no Presentation on display %d' % (label, sid))
        shows_on(run, sid, label + '-second', True, VIEWS[then][1])
        img = run.capture(own, displays(), label + '-main')
        if img.box(GREEN) or img.box(YELLOW):
            raise Fail('%s: display %d shows a game screen with Screen 1 '
                       'Off and a two-screen map' % (label, own))
    else:
        wait_until(lambda: other not in app_windows(), 10,
                   '%s: display %d kept a RetroArch window'
                   % (label, other))
        shows_on(run, own, label + '-own', two,
                 VIEWS[then][1] if two else None)
    lines = system_lines()
    uid = app_uid()
    load = first_after(lines, 0, r'Loading dynamic libretro core')
    at = first_after(lines, load or 0, changed)
    ran = first_after(lines, load or 0,
                      r'\[video_views\] presents=|\[Video\] View map:')
    if None in (load, at, ran):
        raise Fail('harness: %s: the load, first frame or change left the '
                   'log' % label)
    since_load = line_time(lines[at]) - line_time(lines[load])
    since_run = line_time(lines[at]) - line_time(lines[ran])
    if late and since_run <= START_WINDOW + 0.3:
        raise Fail('harness: %s: the change came %.2f s after the first '
                   'frame, not after the start window' % (label, since_run))
    if not late and since_load >= START_WINDOW - 0.1:
        raise Fail('harness: %s: the change came %.2f s after the core '
                   'loaded, not inside the start window'
                   % (label, since_load))
    at_start = asks_after(lines, uid, r'Loading dynamic libretro core')
    after = asks_after(lines, uid, changed)
    if late and after:
        raise Fail('%s: %d move(s) asked after the map change %.2f s in: %s'
                   % (label, len(after), since_run, after[0][:160]))
    if len(at_start) > 2:
        raise Fail('%s: %d moves after the start, not at most 2: %s'
                   % (label, len(at_start), [a[:120] for a in at_start[:3]]))
    return ('%s->%s at frame %d (%.2f s after the first frame): %d move(s) '
            'in all, %d after the change, settled on display %d'
            % (first, then, frame, since_run, len(at_start), len(after),
               own))


@case('map_settle', allow=REFUSALS)
def c_map_settle(run):
    ds = displays()
    sid = find_second(ds, run.probe)
    notes = [map_settle_one(run, ds, sid, *m) for m in SETTLE_MAPS]
    if run.driver == 'vulkan':
        notes += ['vulkan hw ' + map_settle_one(run, ds, sid, *m,
                                                hw='vulkan')
                  for m in SETTLE_VK]
    return '; '.join(notes)


def save_diagnostics(run):
    for name, cmd in (('logcat.txt', None),
                      ('display.txt', 'dumpsys display'),
                      ('windows.txt', 'dumpsys window windows'),
                      ('input.txt', 'dumpsys input')):
        try:
            text = (adb('logcat', '-d', '-v', 'threadtime') if cmd is None
                    else sh(cmd, check=False))
            with open(os.path.join(run.dir, name), 'w') as f:
                f.write(text)
        except Exception:
            pass


def cmd_run(args):
    probe = load_probe()
    run = Run(args.driver, args.threaded == 1, probe)
    if args.cases:
        names = args.cases.split(',')
    else:
        names = [c['name'] for c in CASES if not c['info']
                 and (not c['emulator_only'] or is_emulator())
                 and c['driver'] in (None, args.driver)]
    known = dict((c['name'], c) for c in CASES)
    top = os.path.join(OUT, 'run-%s-%s' % (
        args.driver, 'threaded' if run.threaded else 'unthreaded'))
    adb('logcat', '-G', '16M', check=False)
    results = []
    for name in names:
        c = known.get(name)
        run.dir = os.path.join(top, name)
        os.makedirs(run.dir, exist_ok=True)
        adb('logcat', '-b', 'crash', '-c', check=False)
        try:
            if c is None:
                raise Fail('no such case')
            if c['emulator_only'] and not is_emulator():
                raise Skip('emulator only')
            if c['driver'] and c['driver'] != args.driver:
                raise Skip('%s only' % c['driver'])
            note = c['fn'](run)
            warn = [l for l in log_lines() if WARNINGS.search(l)
                    and not (c['allow'] and re.search(c['allow'], l))]
            if warn:
                raise Fail('the second output warned: %s' % warn[0])
            crash = adb('logcat', '-d', '-b', 'crash', check=False)
            if mentions(crash):
                raise Fail('crash: %s' % crash.strip().splitlines()[0][:200])
            results.append(('PASS', name, note or ''))
        except Skip as e:
            results.append(('SKIP', name, str(e)))
        except (Fail, subprocess.TimeoutExpired) as e:
            results.append(('FAIL', name, str(e)))
            save_diagnostics(run)
        finally:
            try:
                restore(run)
            except Fail as e:
                results.append(('FAIL', name + '/restore', str(e)))
        r = results[-1]
        print('%s %s%s' % (r[0], r[1], (': ' + r[2]) if r[2] else ''),
              flush=True)
    sh('am force-stop %s' % PKG, check=False)
    counts = dict((k, sum(1 for r in results if r[0] == k))
                  for k in ('PASS', 'FAIL', 'SKIP'))
    line = 'cases: %d pass, %d fail, %d skip' % (
        counts['PASS'], counts['FAIL'], counts['SKIP'])
    print(line)
    with open(os.path.join(top, 'summary.txt'), 'w') as f:
        for r in results:
            f.write('%s %s%s\n' % (r[0], r[1], (': ' + r[2]) if r[2] else ''))
        f.write(line + '\n')
    return 1 if counts['FAIL'] else 0


def cmd_probe(args):
    os.makedirs(OUT, exist_ok=True)
    ds = displays()
    main = 0 if 0 in ds else min(ds)

    def others(d):
        return [i for i in sorted(d) if i != main and not d[i]['private']]
    if is_emulator() and not others(ds):
        add_second()
        try:
            wait_until(lambda: others(displays()), 20, 'no second display')
        except Fail:
            sh('am broadcast -a com.android.emulator.multidisplay.START -n '
               'com.android.emulator.multidisplay/.MultiDisplayServiceReceiver',
               check=False)
            add_second()
            wait_until(lambda: others(displays()), 20,
                       'the emulator made no second display: restart it '
                       'with RA_EMU_WINDOW=-qt-hide-window')
        ds = displays()
    cand = others(ds)
    if not cand:
        raise Fail('no second display on %s' % SERIAL)
    if is_emulator():
        cand.sort(key=lambda i: (ds[i]['size'] not in (
            list(EMU_SECOND[:2]), list(EMU_SECOND[1::-1])),
            'Hidden' in ds[i]['name'], i))
    else:
        cand.sort(key=lambda i: (not ds[i]['physical'],
                                 not ds[i]['presentation'], i))
    sec = cand[0]
    probe = {'serial': SERIAL, 'main': ds[main], 'second': ds[sec],
             'ordinal': ordinal(ds, sec), 'displays': ds, 'capture': {},
             'sdk': sh('getprop ro.build.version.sdk').strip(),
             'vulkan_hal': sh('getprop ro.hardware.vulkan',
                              check=False).strip()}
    for role, did in (('main', main), ('second', sec)):
        method = None
        uid = ds[did]['unique_id']
        path = os.path.join(OUT, 'probe-%s.png' % role)
        if uid.startswith('local:'):
            try:
                img = _screencap(uid[6:], path)
                if sorted([img.w, img.h]) == sorted(ds[did]['size']):
                    method = 'screencap'
            except Fail as e:
                print('screencap -d %s: %s' % (uid[6:], e))
        if method is None and role == 'main':
            _screencap(None, path)
            method = 'screencap-default'
        for number in ((EMU_INDEX, did) if is_emulator() and method is None
                       and role == 'second' else ()):
            try:
                img = _console_shot(path, number)
                if sorted([img.w, img.h]) == sorted(ds[did]['size']):
                    method = 'console'
                    probe['console_display'] = number
                    break
                print('console screenshot %d is %dx%d' % (number, img.w, img.h))
            except Fail as e:
                print('console screenshot %d: %s' % (number, e))
        probe['capture'][role] = method
    with open(os.path.join(OUT, 'probe.json'), 'w') as f:
        json.dump(probe, f, indent=1, sort_keys=True)
    for i in sorted(ds):
        d = ds[i]
        print('display %d "%s" %s rot %d %s %s fps %s physical=%s '
              'connection=%d private=%s presentation=%s %s' % (
                  i, d['name'], 'x'.join(map(str, d['size'] or [])),
                  d['rotation'], d['state'], d['type'], d['fps'],
                  d['physical'], d['connection'], d['private'],
                  d['presentation'], d['unique_id']))
    print('second: display %d, ordinal %d; capture main %s, second %s; '
          'sdk %s; vulkan hal "%s"' % (
              sec, probe['ordinal'], probe['capture']['main'],
              probe['capture']['second'], probe['sdk'], probe['vulkan_hal']))
    if not probe['capture']['second']:
        raise Fail('no way to capture the second display')
    return 0


def install_core(local):
    name = os.path.basename(local)
    tmp = '/data/local/tmp/' + name
    adb('push', local, tmp, timeout=600)
    sh("cat %s | run-as %s sh -c 'mkdir -p cores && cat > cores/%s && "
       "chmod 700 cores/%s'" % (tmp, PKG, name, name), timeout=300)
    sh('rm -f %s' % tmp)
    print('core: ' + sh('run-as %s ls -l cores/%s' % (PKG, name)).strip())


def apk_package(apk):
    tools = os.path.join(SDK, 'build-tools')
    for v in sorted(os.listdir(tools) if os.path.isdir(tools) else [],
                    reverse=True):
        aapt2 = os.path.join(tools, v, 'aapt2')
        if os.path.exists(aapt2):
            r = subprocess.run([aapt2, 'dump', 'packagename', apk],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if r.returncode != 0:
                raise Fail('aapt2 cannot read %s: %s' % (
                    apk, r.stderr.decode(errors='replace').strip()))
            return r.stdout.decode().strip()
    raise Fail('no aapt2 under %s' % tools)


def cmd_install(args):
    got = apk_package(args.apk)
    if got != PKG:
        raise Fail('%s is %s, not %s: set RA_PKG to match' % (
            os.path.basename(args.apk), got, PKG))
    adb('install', '-r', '-g', args.apk, timeout=900)
    # Not on a device: there the app stays out of the nightly's files
    # under /storage/emulated/0/RetroArch
    if is_emulator():
        sh('appops set --uid %s MANAGE_EXTERNAL_STORAGE allow' % PKG,
           check=False)
        sh('svc power stayon true', check=False)
        for k in ('window_animation_scale', 'transition_animation_scale',
                  'animator_duration_scale'):
            sh('settings put global %s 0' % k, check=False)
        # A fresh AVD's one-time "Viewing full screen" notice takes the
        # focus, and RetroArch pauses on the focus loss
        sh('settings put secure immersive_mode_confirmations confirmed',
           check=False)
    if PKG != KEEP or is_emulator():
        # A first start makes the app's own directories and assets;
        # never Davey's own RetroArch on a device
        sh('am force-stop %s' % PKG)
        adb('logcat', '-c', check=False)
        if not is_emulator():
            print('%s has no all-files access, so this first start may show '
                  'Android\'s All Files Access dialog: answer it on %s '
                  '(Cancel keeps the app out of /storage/emulated/0/'
                  'RetroArch); waiting up to %d s' % (PKG, SERIAL, ASK_WAIT),
                  flush=True)
        sh('am start -W -n %s' % ACTIVITY)
        if is_emulator():
            wait_log(r'.', 30,
                     what='RetroArch logged nothing on its first start')
        else:
            # The native start waits on the dialog's answer
            wait_log(r'\[ENV\] Storage permission resolved', ASK_WAIT,
                     what='the storage permission was not settled on the '
                     'first start')
        time.sleep(10.0)
        sh('am force-stop %s' % PKG)
    sh('mkdir -p %s' % ' '.join('%s/%s' % (DEV, d) for d in DIRS))
    for core in args.cores:
        install_core(core)
    print('installed %s on %s' % (os.path.basename(args.apk), SERIAL))
    return 0


def cmd_core(args):
    for core in args.cores:
        install_core(core)
    return 0


def cmd_geometry(args):
    probe = load_probe()
    run = Run('gl', True, probe)
    run.dir = os.path.join(OUT, 'geometry-' + args.label)
    os.makedirs(run.dir, exist_ok=True)
    if is_emulator():
        console('multidisplay del %d' % EMU_INDEX)
        wait_until(lambda: find_second(displays(), probe) is None, 15,
                   'the second display is still listed')
    try:
        start(run, cfg={'video_screen2_display': ''})
        img0, g, y = expect_attached(run, 'geometry')
        geo = {'size': [img0.w, img0.h], 'green': list(g), 'yellow': list(y)}
        with open(os.path.join(OUT, 'geometry-%s.json' % args.label),
                  'w') as f:
            json.dump(geo, f)
        print('%s: %s' % (args.label, geo))
    finally:
        sh('am force-stop %s' % PKG, check=False)
        restore(run)
    return 0


def cmd_compare(args):
    geo = []
    for label in (args.a, args.b):
        with open(os.path.join(OUT, 'geometry-%s.json' % label)) as f:
            geo.append(json.load(f))
    diff = [k for k in ('size', 'green', 'yellow')
            if any(abs(p - q) > 1 for p, q in zip(geo[0][k], geo[1][k]))]
    if diff:
        print('layout differs in %s: %s vs %s' % (', '.join(diff), geo[0],
                                                   geo[1]))
        return 1
    print('same layout: %s' % geo[0])
    return 0


def cmd_launch(args):
    probe = load_probe()
    run = Run(args.driver, args.threaded == 1, probe)
    cfg = dict(kv.split('=', 1) for kv in args.cfg)
    if args.display is not None:
        cfg['video_screen2_display'] = args.display
    opts = dict(kv.split('=', 1) for kv in args.opts)
    if args.core == TEST_CORE:
        opts.setdefault('video_views_test_map', args.map)
    start(run, cfg=cfg, opts=opts, core=args.core, content=args.content,
          ready=args.ready or None)
    print('running %s on %s with %s' % (args.core, SERIAL, args.driver))
    return 0


def cmd_state(args):
    d = os.path.join(OUT, 'state-' + args.label)
    os.makedirs(d, exist_ok=True)
    text = sh('dumpsys display')
    ds = parse_displays(text)
    wins = parse_windows(sh('dumpsys window windows'))
    wtext = sh('dumpsys window')
    top, cur = parse_focus(wtext)
    power = sh('dumpsys power', check=False)
    log = adb('logcat', '-d', '-v', 'threadtime')
    crash = adb('logcat', '-d', '-b', 'crash', check=False)
    for name, body in (('display.txt', text), ('window.txt', wtext),
                       ('power.txt', power), ('logcat.txt', log),
                       ('crash.txt', crash)):
        with open(os.path.join(d, name), 'w') as f:
            f.write(body)
    for i in sorted(ds):
        uid = ds[i]['unique_id']
        if uid.startswith('local:'):
            try:
                _screencap(uid[6:], os.path.join(d, 'display-%d.png' % i))
            except Fail as e:
                print('display %d: %s' % (i, e))
    raw = log_lines()
    last = [l for l in raw if POINTER.search(l) or PAD.search(l)
            or 'Video views:' in l or 'View map' in l][-6:]
    print('displays: ' + ' | '.join(
        '%d %s %s %s %sHz' % (i, ds[i]['name'], ds[i]['state'],
                              'x'.join(map(str, ds[i]['size'] or [])),
                              ds[i]['fps']) for i in sorted(ds)))
    print('RetroArch windows: %s' % (wins or 'none'))
    print('focus: top display %s, %s' % (top, cur[:1]))
    print('wakefulness: %s' % _first(r'mWakefulness=(\w+)', power, '?'))
    print('process: %s' % (sh('pidof %s' % PKG, check=False).strip()
                           or 'not running'))
    print('crash buffer: %s' % ('has %s' % PKG if mentions(crash)
                                else 'clean'))
    print('last core lines:\n  ' + '\n  '.join(last or ['none']))
    print('saved in %s' % d)
    return 0


def cmd_menu(args):
    probe = load_probe()
    d = os.path.join(OUT, 'menu')
    for p in args.presses:
        if p in ('open', 'close'):
            ra_cmd('SET_OPTION menu %s' % ('on' if p == 'open' else 'off'))
        else:
            ra_cmd('MENU ' + p)
        time.sleep(0.4)
    capture(probe['main']['id'], displays(), probe, d, args.label)
    path = os.path.join(d, args.label + '.png')
    big = os.path.join(d, args.label + '-x3.png')
    subprocess.run(['magick', path, '-scale', '300%', big], check=True)
    text = subprocess.run(['tesseract', big, '-', '--psm', '6'],
                          stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL).stdout.decode()
    print(text.strip())
    print('saved %s' % path)
    return 0


def cmd_frametimes(args):
    t = sf_frame_times(args.seconds)
    waits = sum(1 for l in adb('logcat', '-d').splitlines()
                if 'dequeueBuffer timed out' in l)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'frametimes-%s.json' % args.label), 'w') as f:
        json.dump(dict(t, dequeue_timeouts=waits), f)
    print('%s: %s; dequeueBuffer timed out: %d' % (args.label, show_times(t),
                                                   waits))
    return 0


def cmd_rate(args):
    a = frame_count()
    if a is None:
        print('no answer to GET_OPTION frame_count')
        return 1
    t = time.time()
    time.sleep(10)
    b = frame_count()
    print('%.1f frontend frames/s' % ((b - a) / (time.time() - t)))
    return 0


# ---- selftest ----

SAMPLE_DISPLAY = '''
  mViewports=[DisplayViewport{type=INTERNAL, valid=true, isActive=true}]
    mBaseDisplayInfo=DisplayInfo{"Built-in Screen", displayId 0, displayGroupId 0, FLAG_SECURE, FLAG_SUPPORTS_PROTECTED_BUFFERS, FLAG_TRUSTED, real 1080 x 1920, largest app 1080 x 1920, smallest app 1080 x 1920, appMode {id=1, width=1080, height=1920, fps=120.0}, mode 1, defaultMode 1, modes [{id=1, width=1080, height=1920, fps=120.0, alternativeRefreshRates=[60.0]}, {id=2, width=1080, height=1920, fps=60.0}], rotation 0, state ON, committedState ON, type INTERNAL, uniqueId "local:4619827259835644672", app 1080 x 1920, density 480, address {port=0}, deviceProductInfo DeviceProductInfo{name=, manufacturerPnpId=QCM, productId=1, modelYear=null, manufactureDate=ManufactureDate{week=27, year=2006}, connectionToSinkType=0}, removeMode 0}
    mOverrideDisplayInfo=DisplayInfo{"Built-in Screen", displayId 0, displayGroupId 0, FLAG_SECURE, FLAG_SUPPORTS_PROTECTED_BUFFERS, FLAG_TRUSTED, real 1920 x 1080, largest app 1920 x 1920, smallest app 1080 x 1080, appMode {id=1, width=1080, height=1920, fps=120.0}, mode 1, defaultMode 1, modes [{id=1, width=1080, height=1920, fps=120.0, alternativeRefreshRates=[60.0]}, {id=2, width=1080, height=1920, fps=60.0}], rotation 1, state ON, committedState ON, type INTERNAL, uniqueId "local:4619827259835644672", app 1920 x 1080, density 480, address {port=0}, deviceProductInfo DeviceProductInfo{name=, manufacturerPnpId=QCM, productId=1, modelYear=null, manufactureDate=ManufactureDate{week=27, year=2006}, connectionToSinkType=0}, removeMode 0}
    mBaseDisplayInfo=DisplayInfo{"Screen-2", displayId 4, displayGroupId 0, FLAG_SECURE, FLAG_PRESENTATION, FLAG_TRUSTED, real 1240 x 1080, mode 3, modes [{id=3, width=1080, height=1240, fps=60.0}], rotation 1, state ON, type INTERNAL, uniqueId "local:4619827259835644673", deviceProductInfo DeviceProductInfo{name=, manufacturerPnpId=QCM, connectionToSinkType=0}}
    mOverrideDisplayInfo=null
    mBaseDisplayInfo=DisplayInfo{"Emulator 2D Display", displayId 2, displayGroupId 0, FLAG_PRIVATE, real 1240 x 1080, mode 4, rotation 0, state ON, type VIRTUAL, uniqueId "virtual:com.android.emulator.multidisplay:1", deviceProductInfo null}
'''

SAMPLE_WINDOWS = '''WINDOW MANAGER WINDOWS (dumpsys window windows)
  Window #0 Window{a1b2c3 u0 %(pkg)s}:
    mDisplayId=4 rootTaskId=1 mSession=Session{1 2:3}
    mOwnerUid=10123 showForAllUsers=false package=%(pkg)s appop=NONE
  Window #1 Window{d4e5f6 u0 %(pkg)s/com.retroarch.browser.retroactivity.RetroActivityFuture}:
    mDisplayId=0 rootTaskId=12 mSession=Session{1 2:3}
    mOwnerUid=10123 showForAllUsers=false package=%(pkg)s appop=NONE
  Window #2 Window{778899 u0 com.android.systemui}:
    mDisplayId=4 rootTaskId=1
    mOwnerUid=10050 showForAllUsers=true package=com.android.systemui appop=NONE
  Window #3 Window{aabbcc u0 %(other)s/com.retroarch.browser.retroactivity.RetroActivityFuture}:
    mDisplayId=2 rootTaskId=15 mSession=Session{4 5:6}
    mOwnerUid=10124 showForAllUsers=false package=%(other)s appop=NONE
  mCurrentFocus=Window{d4e5f6 u0 %(pkg)s/com.retroarch.browser.retroactivity.RetroActivityFuture}
  mTopFocusedDisplayId=0
'''

# ra_dual29 (API 29), emulator-5582
SAMPLE_DISPLAY_29 = '''
    mBaseDisplayInfo=DisplayInfo{"Built-in Screen, displayId 0", uniqueId "local:0", app 1920 x 1080, real 1920 x 1080, largest app 1920 x 1080, smallest app 1920 x 1080, mode 1, defaultMode 1, modes [{id=1, width=1920, height=1080, fps=60.000004}], colorMode 0, supportedColorModes [0], hdrCapabilities android.view.Display$HdrCapabilities@40f16308, rotation 0, density 320 (320.0 x 320.0) dpi, layerStack 0, appVsyncOff 1000000, presDeadline 16666666, type BUILT_IN, address {port=0}, state ON, FLAG_SECURE, FLAG_SUPPORTS_PROTECTED_BUFFERS, removeMode 0}
    mOverrideDisplayInfo=DisplayInfo{"Built-in Screen, displayId 0", uniqueId "local:0", app 1920 x 984, real 1920 x 1080, largest app 1920 x 1776, smallest app 1080 x 936, mode 1, defaultMode 1, modes [{id=1, width=1920, height=1080, fps=60.000004}], colorMode 0, supportedColorModes [0], hdrCapabilities android.view.Display$HdrCapabilities@40f16308, rotation 0, density 320 (320.0 x 320.0) dpi, layerStack 0, appVsyncOff 1000000, presDeadline 16666666, type BUILT_IN, address {port=0}, state ON, FLAG_SECURE, FLAG_SUPPORTS_PROTECTED_BUFFERS, removeMode 0}
    mBaseDisplayInfo=DisplayInfo{"Emulator 2D Display, displayId 1", uniqueId "virtual:com.android.emulator.multidisplay:1234562", app 1240 x 1080, real 1240 x 1080, largest app 1240 x 1080, smallest app 1240 x 1080, mode 2, defaultMode 2, modes [{id=2, width=1240, height=1080, fps=60.0}], colorMode 0, supportedColorModes [0], hdrCapabilities null, rotation 0, density 320 (320.0 x 320.0) dpi, layerStack 1, appVsyncOff 0, presDeadline 16666666, type VIRTUAL, state ON, owner com.android.emulator.multidisplay (uid 1000), removeMode 0}
    mOverrideDisplayInfo=DisplayInfo{"Emulator 2D Display, displayId 1", uniqueId "virtual:com.android.emulator.multidisplay:1234562", app 1240 x 984, real 1240 x 1080, largest app 1240 x 1144, smallest app 1080 x 984, mode 2, defaultMode 2, modes [{id=2, width=1240, height=1080, fps=60.0}], colorMode 0, supportedColorModes [0], hdrCapabilities null, rotation 0, density 240 (320.0 x 320.0) dpi, layerStack 1, appVsyncOff 0, presDeadline 16666666, type VIRTUAL, state ON, owner com.android.emulator.multidisplay (uid 1000), removeMode 0}
'''

# input's keyevent usage on the Thor (API 33) and emulator-5580 (API 35)
KEYEVENT_33 = ('      keyevent [--longpress|--doubletap] <key code number or '
               'name> ... (Default: keyboard)')
KEYEVENT_35 = ('      keyevent [--longpress|--duration <duration to hold key '
               'down in ms>] [--doubletap] [--async] [--delay <duration '
               'between keycodes in ms>] <key code number or name> ... '
               '(Default: keyboard)')

# The Thor (API 33) awake, trimmed: window manager's copy says OFF
SAMPLE_DISPLAY_THOR = '''
    mBaseDisplayInfo=DisplayInfo{"Built-in Screen", displayId 0", displayGroupId 0, FLAG_SECURE, FLAG_SUPPORTS_PROTECTED_BUFFERS, FLAG_TRUSTED, real 1080 x 1920, mode 1, defaultMode 2, modes [{id=1, width=1080, height=1920, fps=60.000004, alternativeRefreshRates=[120.00001]}, {id=2, width=1080, height=1920, fps=120.00001, alternativeRefreshRates=[60.000004]}], rotation 0, state ON, committedState ON, type INTERNAL, uniqueId "local:4630946441858561667", app 1080 x 1920, deviceProductInfo DeviceProductInfo{name=, manufacturerPnpId=QCM, productId=1, modelYear=null, manufactureDate=ManufactureDate{week=27, year=2006}, connectionToSinkType=0}, removeMode 0}
    mOverrideDisplayInfo=DisplayInfo{"Built-in Screen", displayId 0", displayGroupId 0, FLAG_SECURE, FLAG_SUPPORTS_PROTECTED_BUFFERS, FLAG_TRUSTED, real 1920 x 1080, mode 1, defaultMode 2, modes [{id=1, width=1080, height=1920, fps=60.000004, alternativeRefreshRates=[120.00001]}, {id=2, width=1080, height=1920, fps=120.00001, alternativeRefreshRates=[60.000004]}], rotation 1, state OFF, committedState OFF, type INTERNAL, uniqueId "local:4630946441858561667", app 1920 x 1025, deviceProductInfo DeviceProductInfo{name=, manufacturerPnpId=QCM, productId=1, modelYear=null, manufactureDate=ManufactureDate{week=27, year=2006}, connectionToSinkType=0}, removeMode 0}
    mBaseDisplayInfo=DisplayInfo{"Screen-2", displayId 4", displayGroupId 0, FLAG_SECURE, FLAG_SUPPORTS_PROTECTED_BUFFERS, FLAG_PRESENTATION, FLAG_TRUSTED, real 1080 x 1240, mode 4, defaultMode 3, modes [{id=3, width=1080, height=1240, fps=120.00001, alternativeRefreshRates=[60.000004]}, {id=4, width=1080, height=1240, fps=60.000004, alternativeRefreshRates=[120.00001]}], rotation 0, state ON, committedState ON, type INTERNAL, uniqueId "local:4630946482288158084", app 1080 x 1240, deviceProductInfo DeviceProductInfo{name=, manufacturerPnpId=QCM, productId=1, modelYear=null, manufactureDate=ManufactureDate{week=27, year=2006}, connectionToSinkType=0}, removeMode 0}
    mOverrideDisplayInfo=DisplayInfo{"Screen-2", displayId 4", displayGroupId 0, FLAG_SECURE, FLAG_SUPPORTS_PROTECTED_BUFFERS, FLAG_PRESENTATION, FLAG_TRUSTED, real 1240 x 1080, mode 4, defaultMode 3, modes [{id=3, width=1080, height=1240, fps=120.00001, alternativeRefreshRates=[60.000004]}, {id=4, width=1080, height=1240, fps=60.000004, alternativeRefreshRates=[120.00001]}], rotation 1, state OFF, committedState OFF, type INTERNAL, uniqueId "local:4630946482288158084", app 1240 x 1025, deviceProductInfo DeviceProductInfo{name=, manufacturerPnpId=QCM, productId=1, modelYear=null, manufactureDate=ManufactureDate{week=27, year=2006}, connectionToSinkType=0}, removeMode 0}
'''

# API 35, emulator-5580, as the frametimes case listed them; the frame
# layer (#2674) follows the name ft35.py measured, and the StatusBar,
# !handle and other-package lines are made up
SAMPLE_LAYERS_35 ='''RequestedLayerState{Surface(name=79addf4 %(pkg)s/com.retroarch.browser.retroactivity.RetroActivityFuture)/@0x70ff78c - animation-leash of starting_reveal#2467}
RequestedLayerState{f6016a4 ActivityRecordInputSink %(pkg)s/com.retroarch.browser.retroactivity.RetroActivityFuture#2672 parentId=2666 z=-2147483648}
RequestedLayerState{ActivityRecord{9a7cc0d u0 %(pkg)s/com.retroarch.browser.retroactivity.RetroActivityFuture#2666 parentId=2665}
RequestedLayerState{c22f02b %(pkg)s/com.retroarch.browser.retroactivity.RetroActivityFuture#2673 parentId=2666}
RequestedLayerState{%(pkg)s/com.retroarch.browser.retroactivity.RetroActivityFuture#2674 parentId=2673}
RequestedLayerState{%(other)s/com.retroarch.browser.retroactivity.RetroActivityFuture#2702 parentId=2701}
RequestedLayerState{StatusBar#75 parentId=74 z=1}
RequestedLayerState{%(pkg)s/com.retroarch.browser.retroactivity.RetroActivityFuture#2500 !handle}
'''

# Before API 34, plain names (made up)
SAMPLE_LAYERS = '''com.android.systemui.ImageWallpaper#45
ActivityRecord{1a2b3c u0 %(pkg)s/com.retroarch.browser.retroactivity.RetroActivityFuture t12}#119
%(pkg)s/com.retroarch.browser.retroactivity.RetroActivityFuture#120
%(pkg)s/com.retroarch.browser.retroactivity.RetroActivityFuture(BLAST)#121
Surface(name=d4e5f6 %(pkg)s/com.retroarch.browser.retroactivity.RetroActivityFuture)/@0x1a2b - animation-leash of app_transition#130
%(other)s/com.retroarch.browser.retroactivity.RetroActivityFuture(BLAST)#140
'''

# The API 35 emulator's focus after multidisplay add: the new display's
# launcher has it
LAUNCHER_WINDOW = ('dc24ded u0 com.google.android.apps.nexuslauncher/'
                   'com.android.launcher3.secondarydisplay.'
                   'SecondaryDisplayLauncher')
RA_WINDOW = ('12c5066 u0 %s/com.retroarch.browser.retroactivity.'
             'RetroActivityFuture')


def focus_dump(top, window):
    return '  mCurrentFocus=%s\n  mTopFocusedDisplayId=%d\n' % (
        'Window{%s}' % window if window else 'null', top)


def refocus_with(before):
    """refocus_after_add(0) against a stand-in adb whose dumpsys window
    answers are before's (top, window) in turn, the last repeating, until
    an am start brings RetroArch to the front: the result and the
    am starts."""
    global adb
    starts = []
    left = list(before)

    def stand_in(*a, **kw):
        if a == ('shell', 'dumpsys window'):
            if starts:
                return focus_dump(0, RA_WINDOW % PKG)
            return focus_dump(*(left.pop(0) if len(left) > 1 else left[0]))
        if a[0] == 'shell' and a[1].startswith('am start '):
            starts.append(a[1])
            return ''
        raise Fail('no device in the selftest: adb %s' % (a,))

    saved = adb, time.sleep
    adb = stand_in
    time.sleep = lambda s: None
    try:
        return refocus_after_add(0, settle=0.2), starts
    finally:
        adb, time.sleep = saved


def held_with(answers, r):
    """user_rotation_held(0, r) against a stand-in adb whose user-rotation
    readbacks are answers in turn, the last repeating: None, or the
    Skip's reason."""
    global adb
    left = list(answers)

    def stand_in(*a, **kw):
        if a == ('shell', 'cmd window user-rotation -d 0'):
            return left.pop(0) if len(left) > 1 else left[0]
        raise Fail('no device in the selftest: adb %s' % (a,))

    saved = adb, time.sleep
    adb = stand_in
    time.sleep = lambda s: None
    try:
        user_rotation_held(0, r, timeout=0.05)
        return None
    except Skip as e:
        return str(e)
    finally:
        adb, time.sleep = saved


def run_main(argv, pkg, serial, pid, log=None):
    """main(argv) against a stand-in adb that answers pidof of Davey's
    RetroArch and, given a log, everything else (logcat -d with the log,
    the rest with nothing): the exit, the adb calls and the output."""
    global adb, PKG, SERIAL, apk_package
    calls = []

    def stand_in(*a, **kw):
        calls.append(a)
        if a == ('shell', 'pidof %s || true' % KEEP):
            return pid
        if log is None:
            raise Fail('no device in the selftest')
        return log if a[:2] == ('logcat', '-d') else ''

    saved = adb, PKG, SERIAL, apk_package, time.sleep
    adb, PKG, SERIAL = stand_in, pkg, serial
    apk_package = lambda apk: pkg
    time.sleep = lambda s: None
    out = io.StringIO()
    try:
        with contextlib.redirect_stdout(out):
            code = main(argv)
    finally:
        adb, PKG, SERIAL, apk_package, time.sleep = saved
    return code, calls, out.getvalue()


def cmd_selftest(args):
    global PKG, ASK_WAIT
    ds = parse_displays(SAMPLE_DISPLAY)
    assert sorted(ds) == [0, 2, 4], ds
    assert ds[0]['name'] == 'Built-in Screen' and ds[0]['size'] == [1920, 1080]
    assert ds[0]['fps'] == 120.0 and ds[0]['rotation'] == 1
    assert ds[0]['physical'] and ds[0]['connection'] == 0
    assert ds[0]['unique_id'] == 'local:4619827259835644672'
    assert ds[4]['presentation'] and not ds[4]['private'] and ds[4]['fps'] == 60.0
    assert ds[2]['private'] and not ds[2]['physical'] and ds[2]['type'] == 'VIRTUAL'
    assert ordinal(ds, 4) == 2
    d29 = parse_displays(SAMPLE_DISPLAY_29)
    assert sorted(d29) == [0, 1], d29
    assert d29[0]['name'] == 'Built-in Screen' and d29[0]['type'] == 'BUILT_IN'
    assert d29[0]['size'] == [1920, 1080] and d29[0]['fps'] == 60.000004
    assert d29[0]['unique_id'] == 'local:0' and d29[0]['rotation'] == 0
    d = d29[1]
    assert d['name'] == 'Emulator 2D Display' and d['type'] == 'VIRTUAL', d
    assert d['size'] == [1240, 1080] and d['fps'] == 60.0 and d['state'] == 'ON'
    assert not (d['private'] or d['presentation'] or d['physical']), d
    assert d['unique_id'] == 'virtual:com.android.emulator.multidisplay:1234562'
    assert ordinal(d29, 1) == 2
    assert find_second(d29, {'main': d29[0], 'second': d}) == 1
    thor = parse_displays(SAMPLE_DISPLAY_THOR)
    assert sorted(thor) == [0, 4], thor
    assert thor[0]['state'] == 'ON' and thor[4]['state'] == 'ON', thor
    assert thor[0]['size'] == [1920, 1080] and thor[0]['rotation'] == 1
    assert thor[4]['size'] == [1240, 1080] and thor[4]['rotation'] == 1
    assert thor[0]['fps'] == 60.000004 and thor[4]['presentation']
    # the Thor's display 0 keeps lock 1; the emulator's takes the lock,
    # the readback late under load
    why = held_with(['lock 1\n'], 2)
    assert why == 'display 0 keeps its user rotation: lock 2 reads back ' \
        'as "lock 1"', why
    assert held_with(['lock 0', 'lock 0', 'lock 1\n'], 1) is None
    assert 'as "free"' in held_with(['free'], 1)
    assert natural(thor[0]) == natural(ds[0]) == 'portrait'
    assert natural(d29[0]) == 'landscape'
    # the Thor: 180 deg, REVERSE_PORTRAIT; emulator-5580: 90 deg
    assert orientation_choice('portrait', 1) == (2, 9, 2)
    assert orientation_choice('landscape', 0) == (1, 9, 1)
    assert orientation_choice('landscape', 3) == (2, 8, 2)
    assert orientation_choice('portrait', 3) == (2, 9, 2)
    for nat in ASKS:
        for r0 in range(4):
            v, asked, r = orientation_choice(nat, r0)
            assert v and (r - r0) % 2 and TURNS[nat][asked] == r, (nat, r0)
    keep = PKG
    try:
        for pkg in (KEEP, 'com.retroarch'):
            PKG = pkg
            other = 'com.retroarch' if pkg == KEEP else KEEP
            sample = SAMPLE_WINDOWS % {'pkg': pkg, 'other': other}
            w = parse_windows(sample)
            assert sorted(w) == [0, 4] and len(w[4]) == 1, w
            top, cur = parse_focus(sample)
            assert top == 0 and ours(cur[0]), cur
            assert ours('ActivityRecord{1 u0 %s/.browser.retroactivity.'
                        'RetroActivityFuture t3}' % pkg)
            for t in ('Window{aabbcc u0 %s/com.retroarch.browser.'
                      'retroactivity.RetroActivityFuture}',
                      '%s/.browser.retroactivity.RetroActivityFuture#7'):
                assert not ours(t % other), t % other
            assert ours_second('Window{1595346 u0 %s}' % pkg)
            assert not ours_second('Window{1595346 u0 %s}' % other)
            assert not ours_second('Window{97ae8cb u0 %s/com.retroarch.'
                                   'browser.retroactivity.'
                                   'RetroActivityFuture}' % pkg)
            sys29 = ('Window{39bbf78 u0 Sys2037:%s/com.retroarch.browser.'
                     'retroactivity.RetroActivityFuture}')
            assert ours_second(sys29 % pkg)
            assert not ours_second(sys29 % other)
            assert not ours(sys29 % pkg)
            assert in_front_of(0, 1, [sys29 % pkg]) is True
            assert in_front_of(0, 4, ['Window{1 u0 %s}' % pkg]) is True
            assert in_front_of(0, 0, ['Window{1 u0 %s}' % pkg]) is False
            assert in_front_of(0, 4, [RA_WINDOW % pkg,
                                      'Window{2 u0 primaryScreenTopLayout}'
                                      ]) is False
            assert in_front_of(0, 0, ['Window{%s}' % (RA_WINDOW % pkg)])
            assert mentions('Process: %s, PID: 1' % pkg)
            assert mentions('pid: 1, tid: 2, name: RetroArch  >>> %s <<<'
                            % pkg)
            assert not mentions('Process: %s, PID: 1\n>>> %s <<<'
                                % (other, other)), other
            ra = '%s/com.retroarch.browser.retroactivity.RetroActivityFuture' % pkg
            names = parse_layers(SAMPLE_LAYERS_35 % {'pkg': pkg, 'other': other})
            assert names == ['f6016a4 ActivityRecordInputSink %s#2672' % ra,
                             'c22f02b %s#2673' % ra, '%s#2674' % ra,
                             '%s#2500' % ra], names
            names = parse_layers(SAMPLE_LAYERS % {'pkg': pkg, 'other': other})
            assert names == ['%s(BLAST)#121' % ra, '%s#120' % ra], names
            # the launcher takes the focus; it moves with no window; it stays
            for before, brought in (([(9, None), (9, LAUNCHER_WINDOW)], True),
                                    ([(9, None)], True),
                                    ([(0, RA_WINDOW % pkg)], False)):
                got, starts = refocus_with(before)
                assert got == brought and len(starts) == brought, (
                    before, got, starts)
                assert all(LAUNCHER in s for s in starts), starts
    finally:
        PKG = keep
    for pkg, serial, no in ((KEEP, '7e63c978', True),
                            (KEEP, '100.111.183.0:5555', True),
                            (KEEP, 'emulator-5580', False),
                            ('com.retroarch', '7e63c978', False),
                            ('com.retroarch', 'emulator-5580', False)):
        assert refused('probe', pkg, serial) == no, (pkg, serial)
        assert not any(refused(c, pkg, serial) for c in OFFLINE)
    code, calls, out = run_main(['probe'], KEEP, '7e63c978', '')
    assert code == 1 and not calls and 'own RetroArch' in out, (calls, out)
    for argv in (['probe'], ['install', 'x.apk'], ['core', 'x.so'], ['run'],
                 ['geometry', 'l'], ['launch'], ['state', 'l'], ['rate'],
                 ['menu', 'l', 'a'], ['frametimes', 'l']):
        code, calls, out = run_main(argv, 'com.retroarch', '7e63c978', '42\n')
        assert (code == 1 and 'is running' in out and
                calls == [('shell', 'pidof %s || true' % KEEP)]), (
            argv, calls, out)
    for serial, pkg, emu in (('emulator-5580', KEEP, True),
                             ('emulator-5580', 'com.retroarch', True),
                             ('7e63c978', 'com.retroarch', False),
                             ('100.111.183.0:5555', 'com.retroarch', False)):
        code, calls, out = run_main(
            ['install', 'x.apk'], pkg, serial, '',
            '[ENV] Storage permission resolved (granted: 0).\n')
        grant = ('shell', 'appops set --uid %s MANAGE_EXTERNAL_STORAGE '
                 'allow' % pkg)
        first = [c for c in calls if c[0] == 'shell'
                 and c[1].startswith('am start -W -n ')]
        assert code == 0 and (grant in calls) == emu and first, (
            serial, pkg, calls, out)
        assert (('shell', 'settings put secure immersive_mode_confirmations '
                 'confirmed') in calls) == emu, (serial, calls)
        assert out.count('All Files Access') == (0 if emu else 1), (
            serial, out)
    wait = ASK_WAIT
    ASK_WAIT = 0
    try:
        code, calls, out = run_main(['install', 'x.apk'], 'com.retroarch',
                                    '7e63c978', '', 'not the answer\n')
    finally:
        ASK_WAIT = wait
    assert code == 1 and 'not settled' in out, out
    px = bytearray(b'\x00\x00\x00\xff' * (200 * 100))
    for y in range(20, 80):
        for x in range(50, 150):
            px[(y * 200 + x) * 4:(y * 200 + x) * 4 + 3] = bytes(YELLOW)
    for y in range(20, 23):
        for x in range(50, 53):
            px[(y * 200 + x) * 4:(y * 200 + x) * 4 + 3] = bytes(WHITE)
    img = Image(200, 100, bytes(px))
    assert img.box(YELLOW) == (50, 20, 150, 80), img.box(YELLOW)
    assert img.box(GREEN) is None and img.near(51, 21, WHITE)
    assert img.fraction(YELLOW, (50, 20, 150, 80)) > 0.99
    assert img.differs(img) == 0.0
    # what the Thor's screencap gives
    assert display_p3(GREEN) == (117, 251, 76), display_p3(GREEN)
    assert display_p3(YELLOW) == (255, 255, 84), display_p3(YELLOW)
    assert forms(WHITE) == (WHITE,)
    refs = (GREEN, YELLOW, WHITE)
    for a in refs:
        for b in refs:
            for fa in forms(a):
                for fb in forms(b):
                    assert a == b or max(abs(fa[k] - fb[k])
                                         for k in range(3)) > 2 * 48, (fa, fb)
    for c, other in ((YELLOW, GREEN), (GREEN, YELLOW)):
        p3 = bytearray(px)
        for y in range(20, 80):
            for x in range(50, 150):
                if p3[(y * 200 + x) * 4:(y * 200 + x) * 4 + 3] == bytes(YELLOW):
                    p3[(y * 200 + x) * 4:(y * 200 + x) * 4 + 3] = bytes(
                        display_p3(c))
        img = Image(200, 100, bytes(p3))
        assert img.box(c) == (50, 20, 150, 80), (c, img.box(c))
        assert img.box(other) is None, (c, img.box(other))
        assert img.near(100, 50, c) and not img.near(100, 50, other)
        assert img.near(51, 21, WHITE) and not img.near(51, 21, c)
        assert img.fraction(c, (50, 20, 150, 80)) > 0.99
    assert frac_point((50, 20, 150, 80), 0.5, 0.5) == (100, 50)
    lines = ['[video_views] pointer x=0 y=16383 pressed=1 packed=(127,287)',
             '[video_views] pointer1 x=1 y=2 pressed=1 packed=(128,96)']
    assert pointers(lines) == [(127, 287)] and pointers(lines, '1') == [(128, 96)]
    assert (hold_method(35, False, KEYEVENT_35)
            == hold_method(31, True, KEYEVENT_35) == 'keyboard')
    assert hold_method(33, False, KEYEVENT_33) == 'longpress'
    assert hold_method(29, True, KEYEVENT_33) == 'sendevent'
    try:
        hold_method(29, False, KEYEVENT_33)
        raise AssertionError('API 29 device: no Skip')
    except Skip as e:
        assert 'API 29' in str(e), e
    assert qwerty2_node('add device 1: /dev/input/event14\n'
                        '  name:     "AT Translated Set 2 keyboard"\n'
                        'add device 2: /dev/input/event12\n'
                        '  name:     "qwerty2"\n') == '/dev/input/event12'
    assert qwerty2_node('add device 1: /dev/input/event1\n'
                        '  name:     "AT Translated Set 2 keyboard"\n') is None
    src = os.path.join(TREE, 'samples/cores/video_views/e2e')
    for driver, (preset, files, text) in SHADERS.items():
        for f in files:
            assert os.path.isfile(os.path.join(src, f)), (driver, src, f)
        if text is None:
            with open(os.path.join(src, preset)) as f:
                text = f.read()
        kv = dict(re.findall(r'^(\w+) = (\S+)$', text, re.M))
        named = set(kv['shader%d' % i] for i in range(int(kv['shaders'])))
        assert named <= set(files), (driver, named, files)
    # the FBO a larger display grows is a viewport-sized pass before the
    # last, which draws to the window; feedback_pass is a feedback.glsl
    kv = dict(re.findall(r'^(\w+) = (\S+)$', GL_PRESET, re.M))
    last = int(kv['shaders']) - 1
    assert any(kv.get('scale_type%d' % i) == 'viewport'
               for i in range(last)), kv
    fb = int(kv['feedback_pass'])
    assert fb < last and kv['shader%d' % fb] == 'feedback.glsl', kv
    assert ds[0]['density'] == 480 and ds[4]['density'] is None, ds
    assert ds[0]['trusted'] and not ds[2]['trusted'], ds
    assert list_view(ds) == [('Built-in Screen', True), ('Screen-2', True)]
    assert shows('Display: Screen-2 (1240x1080)', 'Screen-2')
    assert shows('Display: Screen-2', 'Screen-2')
    assert shows('Auto', 'Auto') and not shows('Display: Screen-22',
                                               'Screen-2')
    assert not shows('Screen-2 (1240x1080)', 'Screen-2')
    future = '%s/com.retroarch.browser.retroactivity.RetroActivityFuture' % PKG
    start_line = ('10-26 10:00:0%d.000   500   521 I ActivityTaskManager: '
                  'START u0 {flg=0x10000000 cmp=%s (has extras)} with '
                  'LAUNCH_SINGLE_INSTANCE from uid %d%s')
    failed = ('10-26 10:00:0%d.010  4000  4000 W RetroArch: [SecondDisplay] '
              'Cannot move to display %d')
    log = [start_line % (0, future, 2000, ' result code=0'),
           start_line % (1, future, 10123, ' result code=-96'),
           failed % (1, 4),
           failed % (2, -1),
           start_line % (3, future, 10123, ' result code=2'),
           start_line % (4, future, 10123, ''),
           failed % (4, 0)]
    assert parse_move_asks(log, 10123) == [log[1], log[3], log[4], log[5]]
    assert parse_move_asks(log[:3], 10124) == [log[2]]
    ready = ('10-26 10:00:01.500  4000  4071 I RetroArch: [Video] View map: '
             '2 views, 2 screens')
    tagged = [log[1], ready, log[4], 'ra-dual-mark-1-1', log[5]]
    assert asks_after(tagged, 10123, READY) == [log[4], log[5]]
    assert asks_after(tagged, 10123, READY, 'ra-dual-mark-1-1') == [log[4]]
    assert asks_after(tagged, 10123, 'ra-dual-mark-1-1') == [log[5]]
    assert asks_after(tagged[2:], 10123, READY) == [log[4], log[5]]
    assert THREADS.match(log[2]).groups() == ('4000', '4000')
    core = ('10-26 10:00:00.500  4000  4071 I RetroArch: [Core] Loading '
            'dynamic libretro core from: "x"')
    assert posted([core] + log) == '4000'
    try:
        posted([core, failed.replace('4000  4000', '4000  4071') % (1, 4)])
        raise AssertionError('a move failed on the core thread')
    except Fail:
        pass
    events = ('10-26 10:00:00.000   500   520 I wm_create_activity: '
              '[0,1234,12,%s,NULL,NULL,NULL,268435456]' % future,
              '10-26 10:00:00.100  4000  4000 I wm_on_create_called: '
              '[0,1234,com.retroarch.browser.retroactivity.'
              'RetroActivityFuture,performCreate,30]',
              '10-26 10:00:01.000   500   521 I wm_new_intent: '
              '[0,1234,12,%s,NULL,NULL,NULL,268435456]' % future,
              '10-26 10:00:02.000   500   521 I am_relaunch_resume_activity: '
              '[0,1234,12,%s]' % future,
              '10-26 10:00:02.000   500   521 I wm_create_activity: '
              '[0,99,3,com.android.launcher3/.Launcher,NULL,NULL,NULL,0]')
    assert parse_life(events) == [
        ('create_activity', 12), ('on_create_called', None),
        ('new_intent', 12), ('relaunch_resume_activity', 12)], \
        parse_life(events)
    life = parse_life(events)
    for ev, moves in (([], 1), ([], None), (life[:2], 1), (life[3:], None)):
        try:
            judge_life(ev, moves)
            raise AssertionError('judged %s with no evidence' % ev)
        except Fail as e:
            assert 'cannot tell' in str(e), e
    assert judge_life([('new_intent', 12)], 1) == ([], ['new_intent'], [12])
    assert judge_life(life[:2], None)[0] == []
    assert judge_life(life[:3], 1)[0] == []
    assert judge_life(life, 1)[0] == ['relaunch_resume_activity']
    assert judge_life(life[:2] + [life[3]] + [life[2]], 1)[0] == []
    assert judge_life(life[2:] + life[:2], 1)[0] == [
        'relaunch_resume_activity', '1 create_activity after the move',
        '1 on_create_called after the move']
    assert selected_row('MENU Main Menu\n0\tLoad Core\t\nSELECTED 3\n'
                        'COUNT 9\n') == 3
    assert selected_row('MENU Main Menu\nSELECTED 0\nCOUNT 9') == 0
    assert selected_row('CLOSED') is None
    assert held_key(('input keyboard keyevent --longpress KEYCODE_X', ''),
                    'KEYCODE_DPAD_DOWN', 108) == \
        'input keyboard keyevent --longpress KEYCODE_DPAD_DOWN'
    ev = 'sendevent /dev/input/event1'
    assert held_key(('{0} 1 45 1; {0} 0 0 0; sleep 0.4; {0} 1 45 0; '
                     '{0} 0 0 0'.format(ev), ''), 'KEYCODE_DPAD_DOWN',
                    108) == ('{0} 1 108 1; {0} 0 0 0; sleep 0.4; '
                             '{0} 1 108 0; {0} 0 0 0'.format(ev))
    assert 'video_screen2_window_mode' not in base_cfg()
    assert merged_cfg({'a': '1', 'b': '2'}, {'a': None, 'c': '3'}) == {
        'b': '2', 'c': '3'}
    px = bytearray(10 * 10 * 4)
    for yy in range(4, 8):
        for xx in range(4, 8):
            px[(yy * 10 + xx) * 4:(yy * 10 + xx) * 4 + 3] = b'\xff\xff\xff'
    assert ink(Image(10, 10, bytes(px))) == (4, 4, 8, 8), ink(
        Image(10, 10, bytes(px)))
    assert ink(Image(10, 10, bytes(10 * 10 * 4))) is None
    def framed(x0, x1):
        px = bytearray(40 * 30 * 4)
        for yy in range(30):
            for xx in range(x0, x1):
                px[(yy * 40 + xx) * 4:(yy * 40 + xx) * 4 + 3] = b'\x40' * 3
        img = Image(40, 30, bytes(px))
        return rgui_box(img, ink(img))
    assert framed(10, 30) and not framed(0, 40) and not framed(2, 20)
    assert rgui_row((0, 0, 320, 240), 0) == (160, 31)
    assert rgui_row((100, 60, 420, 540), 6) == (260, 255)
    made = [(0, True, False), (1, True, True), (2, False, True)]
    two = [(0, 1, (0, 0, 2, 2)), (1, 2, (0, 0, 2, 2))]
    assert judge_handover(made, two) == ([], True)
    why, both = judge_handover([(0, True, False), (1, False, False),
                                (2, False, True)], [(0, 1, None), (1, 2, (0, 0, 2, 2))])
    assert why == ['1 of 3 window samples had no RetroArch window',
                   'captures [1] were black'] and not both, why
    print('selftest: ok')
    return 0


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd')
    sub.add_parser('selftest')
    sub.add_parser('probe')
    p = sub.add_parser('install')
    p.add_argument('apk')
    p.add_argument('cores', nargs='*')
    p = sub.add_parser('core')
    p.add_argument('cores', nargs='+')
    p = sub.add_parser('run')
    p.add_argument('--driver', choices=('gl', 'vulkan'), default='gl')
    p.add_argument('--threaded', type=int, choices=(0, 1), default=1)
    p.add_argument('--cases', default='')
    p = sub.add_parser('geometry')
    p.add_argument('label')
    p = sub.add_parser('compare')
    p.add_argument('a')
    p.add_argument('b')
    p = sub.add_parser('launch')
    p.add_argument('--driver', choices=('gl', 'vulkan'), default='gl')
    p.add_argument('--threaded', type=int, choices=(0, 1), default=1)
    p.add_argument('--display', default=None)
    p.add_argument('--map', default='ds')
    p.add_argument('--core', default=TEST_CORE)
    p.add_argument('--content', default=None)
    p.add_argument('--ready', default=r'\[Video\] View map: \d+ views, 2 screens'
                   r'|\[video_views\] presents=')
    p.add_argument('--cfg', action='append', default=[])
    p.add_argument('--opts', action='append', default=[])
    p = sub.add_parser('state')
    p.add_argument('label')
    sub.add_parser('rate')
    p = sub.add_parser('menu')
    p.add_argument('label')
    p.add_argument('presses', nargs='*',
                   choices=('open', 'close', 'up', 'down', 'left', 'right',
                            'a', 'b'))
    p = sub.add_parser('frametimes')
    p.add_argument('label')
    p.add_argument('--seconds', type=float, default=5.0)
    args = ap.parse_args(argv)
    fn = {'selftest': cmd_selftest, 'probe': cmd_probe,
          'install': cmd_install, 'core': cmd_core, 'run': cmd_run, 'geometry': cmd_geometry,
          'compare': cmd_compare, 'launch': cmd_launch, 'state': cmd_state,
          'rate': cmd_rate, 'menu': cmd_menu,
          'frametimes': cmd_frametimes}.get(args.cmd)
    if not fn:
        ap.print_help()
        return 2
    if refused(args.cmd, PKG, SERIAL):
        print('dual.py %s: %s on %s is Davey\'s own RetroArch; set RA_PKG '
              '(com.retroarch on the Thor)' % (args.cmd, KEEP, SERIAL))
        return 1
    try:
        if args.cmd not in OFFLINE:
            keep_alone()
        return fn(args)
    except (Fail, Skip) as e:
        print('dual.py %s: %s' % (args.cmd, e))
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
