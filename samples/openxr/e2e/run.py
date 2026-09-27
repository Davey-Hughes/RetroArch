#!/usr/bin/env python3
"""End-to-end check of RetroArch's headset output.

Runs the video_views test core with Headset Output on against Monado's
simulated headset in headless gamescope, through the test API layer in
samples/openxr/test_layer, which records every xrEndFrame's quads and
saves the images they show. Not run in CI: it needs a GPU, gamescope,
Monado, and a RetroArch built with HAVE_OPENXR.

Usage: run.py <retroarch> <out dir> <monado.env> [--validate] [case ...]

monado.env holds KEY=VALUE lines (values may be double-quoted): MODE
(inprocess or service), RUNTIME_JSON, CLIENT_ENV and SERVICE_ENV
(space-separated KEY=VALUE pairs), SERVICE_SOCKET, DISPLAY_PERIOD_NS and
VALIDATION_BASELINE: the messages raised without RetroArch's headset
code, one per line, a VUID and then text the message must contain (an
object's name), so a VUID alone does not hide RetroArch's own.
--validate runs RetroArch under the Vulkan validation layer and fails a
case on any other message, or when none of the baseline's appear: the
window's own come every run, so without them the layer did not load.
"""

import json
import math
import os
import re
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'samples', 'cores', 'video_views', 'e2e'))
from run import CORE, read_png, send, write_cfg  # noqa: E402

PRESET = os.path.join(HERE, 'output_size.slangp')
LAYER_DIR = os.path.join(ROOT, 'samples', 'openxr', 'test_layer')
LAYER = 'XR_APILAYER_RETROARCH_test_recorder'
SYSTEM_LAYERS = '/usr/share/openxr/1/api_layers/explicit.d'
W, H = 1600, 960
TOL = 8
POS = 0.01
BLEND = 2  # XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
VUID = re.compile(r'(VUID-[A-Za-z0-9_-]+|UNASSIGNED-[A-Za-z0-9_.-]+)')
MESSAGE = re.compile(r'Validation (Error|Warning|Performance Warning):')
DENSITY = re.compile(r'\[OpenXR\] (\d+) pixels across (\d+) degrees per eye')
# The Monado client's sockets live under a relative XDG_RUNTIME_DIR in
# the case's directory: an absolute scratch path overflows sun_path.
RUNTIME_DIR = 'run'

RED, BLUE, GREEN = (255, 0, 0), (0, 0, 255), (0, 255, 0)
YELLOW, WHITE, GREY = (255, 255, 0), (255, 255, 255), (32, 32, 32)
MAGENTA = (255, 0, 255)


def read_env(path):
    env = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#') and '=' in line:
                k, v = line.split('=', 1)
                env[k.strip()] = v.strip().strip('"')
    return env


def pairs(s):
    return dict(p.split('=', 1) for p in s.split() if '=' in p)


def read_baseline(path):
    entries = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#'):
                parts = line.split(None, 1)
                entries.append((parts[0], parts[1] if len(parts) > 1 else ''))
    return entries


def unexpected(log, baseline):
    """The VUIDs of validation messages the baseline does not cover, and
    how many it does. A message runs from its first line to a blank line
    or the next one."""
    blocks, cur = [], None
    for line in log.splitlines():
        if MESSAGE.search(line):
            if cur:
                blocks.append(cur)
            cur = [line]
        elif cur is not None:
            if line.strip():
                cur.append(line)
            else:
                blocks.append(cur)
                cur = None
    if cur:
        blocks.append(cur)
    extra = set()
    known = 0
    for b in blocks:
        text = '\n'.join(b)
        m = VUID.search(text)
        vuid = m.group(1) if m else b[0].strip()
        if any(vuid == v and t in text for v, t in baseline):
            known += 1
        else:
            extra.add(vuid)
    return sorted(extra), known


class Result(object):
    def __init__(self, d):
        self.dir = d
        self.log = ''
        self.frames = []
        self.releases = []
        self.chains = {}
        self.marks = {}
        self.shot = None


def load(res):
    path = os.path.join(res.dir, 'xr', 'frames.jsonl')
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                try:
                    e = json.loads(line)
                except ValueError:
                    continue  # a line cut short at exit
                if e['ev'] == 'frame':
                    res.frames.append(e)
                elif e['ev'] == 'release':
                    res.releases.append(e)
                elif e['ev'] == 'swapchain':
                    res.chains[e['sc']] = e
    # stdout (the validation layer's messages) and RetroArch's log file.
    for name in ('run.log', 'retroarch.log'):
        p = os.path.join(res.dir, name)
        if os.path.exists(p):
            with open(p, errors='replace') as f:
                res.log += f.read()


def stop(p):
    if p is None or p.poll() is not None:
        return
    os.killpg(p.pid, signal.SIGTERM)
    try:
        p.wait(5)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait(5)


def wait_shot(d):
    deadline = time.time() + 10
    while time.time() < deadline:
        time.sleep(0.5)
        pngs = [x for x in os.listdir(os.path.join(d, 'shots'))
                if x.endswith('.png')]
        if pngs:
            time.sleep(1)
            return os.path.join(d, 'shots', sorted(pngs)[0])
    return None


def write_overlay(d):
    """An overlay whose top-left sixteenth is opaque magenta, and the
    settings that show it over the menu."""
    rows = b''
    for y in range(16):
        rows += b'\0' + b''.join(bytes(MAGENTA + (255,)) if x < 4 and y < 4
                                  else bytes(4) for x in range(16))

    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data
                + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))
    with open(os.path.join(d, 'corner.png'), 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n'
                + chunk(b'IHDR', struct.pack('>IIBBBBB', 16, 16, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
    cfg = os.path.join(d, 'corner.cfg')
    with open(cfg, 'w') as f:
        f.write('overlays = 1\noverlay0_overlay = "corner.png"\n'
                'overlay0_full_screen = true\noverlay0_normalized = true\n'
                'overlay0_descs = 0\n')
    return {'input_overlay_enable': 'true', 'input_overlay': cfg,
            'input_overlay_hide_in_menu': 'false',
            'input_overlay_opacity': '1.0'}


def run_case(retroarch, root, monado, case, validate):
    d = os.path.realpath(os.path.join(root, case['name']))
    if os.path.commonpath([root, d]) != root or d == root:
        raise RuntimeError('refusing to touch ' + d)
    shutil.rmtree(d, ignore_errors=True)
    for sub in ('shots', 'xr', RUNTIME_DIR, 'config'):
        os.makedirs(os.path.join(d, sub))
    os.chmod(os.path.join(d, RUNTIME_DIR), 0o700)

    settings = {'video_openxr_enable': 'true',
                'video_openxr_distance': '1.8',
                'video_openxr_width': '1.6'}
    if case.get('overlay'):
        settings.update(write_overlay(d))
    settings.update(case.get('settings', {}))
    cfg = os.path.join(d, 'retroarch.cfg')
    write_cfg(cfg, d, 'vulkan', settings)
    options = {'video_views_test_map': case.get('map', '3ds'),
               'video_views_test_hw': 'off'}
    options.update(case.get('options', {}))
    with open(os.path.join(d, 'opts.cfg'), 'w') as f:
        for k in sorted(options):
            f.write('%s = "%s"\n' % (k, options[k]))
    script = os.path.join(d, 'script.txt')
    with open(script, 'w') as f:
        f.write(case.get('script', ''))

    runtime = monado['RUNTIME_JSON']
    if case.get('no_runtime'):
        # A manifest whose library is missing: the loader fails instead
        # of falling back to another runtime.
        runtime = os.path.join(d, 'missing_runtime.json')
        with open(runtime, 'w') as f:
            json.dump({'file_format_version': '1.0.0',
                       'runtime': {'name': 'missing',
                                   'library_path':
                                   '/nonexistent/libopenxr_missing.so'}}, f)

    child = {
        'XR_RUNTIME_JSON': runtime,
        'XR_API_LAYER_PATH': LAYER_DIR + ':' + SYSTEM_LAYERS,
        'XR_ENABLE_API_LAYERS': LAYER,
        'RA_XR_LAYER_OUT': os.path.join(d, 'xr'),
        'RA_XR_LAYER_SNAP_EVERY': '30',
        'RA_XR_LAYER_SCRIPT': script,
        'XDG_RUNTIME_DIR': RUNTIME_DIR,
        'WAYLAND_DISPLAY': 'openxr-e2e-no-socket',
    }
    child.update(pairs(monado.get('CLIENT_ENV', '')))
    if validate:
        child['VK_INSTANCE_LAYERS'] = 'VK_LAYER_KHRONOS_validation'
    exports = ' '.join('%s=%s' % (k, shlex.quote(v))
                       for k, v in sorted(child.items()))
    # Never reach the desktop: refuse the real display; what the runtime
    # and RetroArch open lives in this case's directory. The gamescope
    # WSI layer still needs gamescope's own socket, by an absolute path.
    guard = ('case "$DISPLAY" in ""|:0) echo "refusing DISPLAY=$DISPLAY" >&2;'
             ' exit 99;; esac; cd ' + shlex.quote(d) + ' || exit 98;'
             ' case "${GAMESCOPE_WAYLAND_DISPLAY:-}" in ""|/*) ;;'
             ' *) GAMESCOPE_WAYLAND_DISPLAY="$XDG_RUNTIME_DIR/'
             '$GAMESCOPE_WAYLAND_DISPLAY"; export GAMESCOPE_WAYLAND_DISPLAY;;'
             ' esac; export ' + exports + '; exec "$0" "$@"')

    svc = svc_log = None
    if monado.get('MODE') == 'service':
        env = dict(os.environ)
        env.pop('DISPLAY', None)
        env['WAYLAND_DISPLAY'] = 'openxr-e2e-no-socket'
        env['XDG_RUNTIME_DIR'] = RUNTIME_DIR
        # Monado's and libsurvive's config stay out of ~/.config.
        env['XDG_CONFIG_HOME'] = os.path.join(d, 'config')
        env.update(pairs(monado.get('SERVICE_ENV', '')))
        svc_log = open(os.path.join(d, 'service.log'), 'w')
        svc = subprocess.Popen(['monado-service'], cwd=d, env=env,
                               stdout=svc_log, stderr=subprocess.STDOUT,
                               stdin=subprocess.DEVNULL,
                               start_new_session=True)
        sock = os.path.join(d, RUNTIME_DIR,
                            monado.get('SERVICE_SOCKET', 'monado_comp_ipc'))
        deadline = time.time() + 10
        while time.time() < deadline and not os.path.exists(sock):
            time.sleep(0.1)

    w, h = case.get('screen', (W, H))
    cmd = ['gamescope', '--backend', 'headless',
           '-w', str(w), '-h', str(h), '-W', str(w), '-H', str(h),
           '-r', '60', '--', 'sh', '-c', guard,
           retroarch, '--config', cfg, '-L', case.get('core', CORE),
           '--verbose'] + case.get('args', [])
    if case.get('content'):
        cmd.append(case['content'])
    log = open(os.path.join(d, 'run.log'), 'w')
    p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                         start_new_session=True)
    res = Result(d)
    try:
        for kind, arg in case['steps']:
            if kind == 'wait':
                time.sleep(arg)
            elif kind == 'send':
                send(arg)
            elif kind == 'mark':
                res.marks[arg] = time.monotonic()
            elif kind == 'script':
                tmp = script + '.tmp'
                with open(tmp, 'w') as f:
                    f.write(arg + '\n')
                os.replace(tmp, script)
            elif kind == 'shot':
                send('SCREENSHOT')
                res.shot = wait_shot(d)
        send('QUIT')
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            res.marks['hung'] = time.monotonic()
    finally:
        stop(p)
        stop(svc)
        log.close()
        if svc_log:
            svc_log.close()
    load(res)
    return res


def quads(frame):
    return [q for q in frame['layers'] if q['type'] == 'quad']


def last_snap(res):
    for fr in reversed(res.frames):
        if fr['snap'] and quads(fr):
            return fr
    return None


def image(res, fr, q):
    return os.path.join(res.dir, 'xr', 'snap_%d_sc%d_l%d.png'
                        % (fr['n'], q['sc'], q['layer']))


def colour(path, fx, fy):
    w, h, bpp, rows = read_png(path)
    x = min(w - 1, int(fx * w))
    y = min(h - 1, int(fy * h))
    px = rows[y][x * bpp:x * bpp + bpp]
    return tuple(px[:3]), (px[3] if bpp == 4 else 255)


def near(got, want, tol=TOL):
    return all(abs(a - b) <= tol for a, b in zip(got, want))


def at(q, want):
    return all(abs(a - b) <= POS for a, b in zip(q['pose'][:3], want))


def sized(q, want):
    return all(abs(a - b) <= POS for a, b in zip(q['size'], want))


def facing(q, want):
    """The same orientation, whatever the quaternion's sign."""
    return abs(sum(a * b for a, b in zip(q['pose'][3:], want))) >= 0.999


def no_quads(res):
    return ['no frame with quads and images (see %s)'
            % os.path.join(res.dir, 'run.log')]


def image_size(res, q, native, distance=1.8):
    """Errors unless q's swapchain has the size video_xr_image_dims()
    gives at the density the runtime recommended: the quad's angular
    width in headset pixels, never below the source's width, in the
    quad's shape. The log rounds the angle, so allow a pixel or two."""
    m = DENSITY.search(res.log)
    sc = res.chains.get(q['sc'])
    if not m or not sc:
        return ['no headset density or swapchain for quad sc%d' % q['sc']]
    px_per_rad = int(m.group(1)) / math.radians(int(m.group(2)))
    angular = 2.0 * math.atan(q['size'][0] / (2.0 * distance)) * px_per_rad
    slack = 0 if native[0] > angular * 1.02 + 1 else 2 + int(angular * 0.01)
    w = max(native[0], math.ceil(angular))
    h = w * q['size'][1] / q['size'][0]
    errors = []
    if abs(sc['w'] - w) > slack or abs(sc['h'] - h) > slack + 1:
        errors.append('sc%d is %dx%d, want about %dx%d (%.1f px/rad)'
                      % (q['sc'], sc['w'], sc['h'], w, round(h), px_per_rad))
    if q['rect'] != [0, 0, sc['w'], sc['h']]:
        errors.append('sc%d shows %s, want all of it' % (q['sc'], q['rect']))
    return errors


def check_screens(swap=False, horizontal=False):
    def check(res):
        fr = last_snap(res)
        if not fr:
            return no_quads(res)
        q = [x for x in quads(fr) if not x['flags'] & BLEND]
        left = [x for x in q if x['eye'] == 'left']
        right = [x for x in q if x['eye'] == 'right']
        both = [x for x in q if x['eye'] == 'both']
        if (len(left), len(right), len(both)) != (1, 1, 1):
            return ['want a left, a right and a both-eye quad, got %s'
                    % [x['eye'] for x in q]]
        lq, rq, bq = left[0], right[0], both[0]
        errors = []
        if lq['sc'] != rq['sc'] or {lq['layer'], rq['layer']} != {0, 1}:
            errors.append('the eyes are not two layers of one image')
        for x in (lq, rq):
            if not at(x, (0.0, 0.0, -1.8)) or not sized(x, (1.6, 0.96)):
                errors.append('top screen at %s size %s'
                              % (x['pose'][:3], x['size']))
            if not facing(x, (0.0, 0.0, 0.0, 1.0)):
                errors.append('top screen turned %s' % x['pose'][3:])
        want = (1.472, 0.0, -1.8) if horizontal else (0.0, -0.992, -1.8)
        if not at(bq, want) or not sized(bq, (1.28, 0.96)):
            errors.append('bottom screen at %s size %s, want %s'
                          % (bq['pose'][:3], bq['size'], want))
        errors += image_size(res, lq, (400, 240))
        errors += image_size(res, bq, (320, 240))
        if res.chains.get(lq['sc'], {}).get('layers') != 2:
            errors.append('the top screen\'s image has no layer per eye')
        want_l, want_r = (BLUE, RED) if swap else (RED, BLUE)
        for x, want, what in ((lq, want_l, 'left eye'),
                              (rq, want_r, 'right eye'),
                              (bq, YELLOW, 'bottom screen')):
            c, _ = colour(image(res, fr, x), 0.5, 0.5)
            if not near(c, want):
                errors.append('%s shows %s, want %s' % (what, c, want))
        # Each view's white marker is at its top-left: not flipped.
        for x, what in ((lq, 'left eye'), (rq, 'right eye'),
                        (bq, 'bottom screen')):
            c, _ = colour(image(res, fr, x), 0.01, 0.015)
            if not near(c, WHITE):
                errors.append('%s top-left is %s, want white' % (what, c))
        return errors
    return check


def threaded(check):
    def run(res):
        errors = check(res)
        if 'Starting threaded video driver' not in res.log:
            errors.append('threaded video did not start')
        return errors
    return run


def size_colour(w, h):
    """What output_size.slangp draws into a w x h target: the final
    pass's size, and in blue its first pass's, at half the viewport."""
    return tuple(int(round(v / 2048.0 * 255.0)) for v in (w, h, w / 2.0))


def sized_like(res, fr, q, what, tol=2):
    """Errors unless q's image shows the colour for its own size."""
    sc = res.chains.get(q['sc'])
    if not sc:
        return ['no swapchain for %s' % what]
    want = size_colour(sc['w'], sc['h'])
    c, _ = colour(image(res, fr, q), 0.5, 0.5)
    if not near(c, want, tol):
        return ['%s (%dx%d) shows %s, want %s' % (what, sc['w'], sc['h'],
                                                  c, want)]
    return []


def window_like(res, points, tol=2):
    """Errors unless the window screenshot shows, at each (fx, fy), the
    colour for a (w, h) target."""
    if not res.shot:
        return ['no window screenshot']
    errors = []
    for fx, fy, w, h, what in points:
        c, _ = colour(res.shot, fx, fy)
        if not near(c, size_colour(w, h), tol):
            errors.append('window %s (%dx%d) shows %s, want %s'
                          % (what, w, h, c, size_colour(w, h)))
    return errors


def check_sized_screens(res):
    """Every headset image drawn as a first draw at its own size, and
    the window at its rectangles: 2D, the top screen over the bottom."""
    fr = last_snap(res)
    if not fr:
        return no_quads(res)
    q = [x for x in quads(fr) if not x['flags'] & BLEND]
    eyes = dict((x['eye'], x) for x in q)
    if sorted(eyes) != ['both', 'left', 'right']:
        return ['want a left, a right and a both-eye quad, got %s'
                % [x['eye'] for x in q]]
    errors = []
    for eye, what in (('left', 'left eye'), ('right', 'right eye'),
                      ('both', 'bottom screen')):
        errors += sized_like(res, fr, eyes[eye], what)
    k = min(W / 400.0, H / 480.0)
    return errors + window_like(res, (
        (0.5, 0.25, 400 * k, 240 * k, 'top screen'),
        (0.5, 0.75, 320 * k, 240 * k, 'bottom screen')))


def check_sized_frame(res):
    fr = last_snap(res)
    if not fr:
        return no_quads(res)
    q = [x for x in quads(fr) if not x['flags'] & BLEND]
    if len(q) != 1 or q[0]['eye'] != 'both':
        return ['want one quad for both eyes, got %s' % [x['eye'] for x in q]]
    # The core's frame has the window's shape, so it fills the window.
    return (sized_like(res, fr, q[0], 'frame')
            + window_like(res, ((0.5, 0.5, W, H, 'frame'),)))


def check_frame(res):
    fr = last_snap(res)
    if not fr:
        return no_quads(res)
    q = [x for x in quads(fr) if not x['flags'] & BLEND]
    if len(q) != 1 or q[0]['eye'] != 'both':
        return ['want one quad for both eyes, got %s' % [x['eye'] for x in q]]
    errors = []
    if not at(q[0], (0.0, 0.0, -1.8)) or not sized(q[0], (1.6, 0.96)):
        errors.append('frame at %s size %s' % (q[0]['pose'][:3], q[0]['size']))
    errors += image_size(res, q[0], (800, 480))
    for fx, fy, want in ((0.25, 0.25, RED), (0.75, 0.25, BLUE),
                         (0.5, 0.75, YELLOW), (0.1, 0.75, GREY)):
        c, _ = colour(image(res, fr, q[0]), fx, fy)
        if not near(c, want):
            errors.append('(%.2f,%.2f) shows %s, want %s' % (fx, fy, c, want))
    return errors


def menu_size(res, q, ui, distance=1.7):
    """Errors unless the menu quad's swapchain has the UI's size, at
    most twice the headset's pixels across the quad, in its shape."""
    m = DENSITY.search(res.log)
    sc = res.chains.get(q['sc'])
    if not m or not sc:
        return ['no headset density or swapchain for the menu']
    px_per_rad = int(m.group(1)) / math.radians(int(m.group(2)))
    most = 4.0 * math.atan(q['size'][0] / (2.0 * distance)) * px_per_rad
    if ui[0] <= most * 0.98 - 1:
        w, h, slack = ui[0], ui[1], 0
    else:
        w = min(ui[0], math.ceil(most))
        h = w * q['size'][1] / q['size'][0]
        slack = 2 + int(most * 0.01)
    errors = []
    if abs(sc['w'] - w) > slack or abs(sc['h'] - h) > slack + 1:
        errors.append('menu sc%d is %dx%d, want about %dx%d (%.1f px/rad)'
                      % (q['sc'], sc['w'], sc['h'], w, round(h), px_per_rad))
    if q['rect'] != [0, 0, sc['w'], sc['h']]:
        errors.append('menu sc%d shows %s, want all of it'
                      % (q['sc'], q['rect']))
    return errors


def check_menu(ui=(W, H)):
    def check(res):
        fr = last_snap(res)
        if not fr:
            return no_quads(res)
        q = quads(fr)
        menus = [x for x in q if x['flags'] & BLEND]
        if len(menus) != 1:
            return ['want one blended menu quad, got %d' % len(menus)]
        m = menus[0]
        errors = []
        if len(q) != 4:
            errors.append('want the three screen quads behind the menu, '
                          'got %d' % (len(q) - 1))
        if q[-1] is not m:
            errors.append('the menu is not drawn last')
        if not at(m, (0.0, 0.0, -1.7)) or not sized(m, (1.6, 0.96)):
            errors.append('menu at %s size %s' % (m['pose'][:3], m['size']))
        errors += menu_size(res, m, ui)
        # The UI over running content is translucent; a core's frame
        # there would be opaque.
        _, alpha = colour(image(res, fr, m), 0.5, 0.5)
        if not 0 < alpha < 255:
            errors.append('the menu\'s centre has alpha %d' % alpha)
        return errors
    return check


def check_menu_overlay(res):
    """The overlay shows in the window, never in the headset's menu."""
    errors = check_menu()(res)
    fr = last_snap(res)
    menus = [x for x in quads(fr) if x['flags'] & BLEND] if fr else []
    if menus:
        c, _ = colour(image(res, fr, menus[0]), 0.05, 0.05)
        if near(c, MAGENTA, 40):
            errors.append('the overlay is in the headset\'s menu quad')
    if not res.shot:
        errors.append('no window screenshot')
    else:
        c, _ = colour(res.shot, 0.05, 0.05)
        if not near(c, MAGENTA):
            errors.append('the window shows %s at the overlay, want %s'
                          % (c, MAGENTA))
    return errors


def check_menu_closed(res):
    last = [f for f in res.frames if quads(f)]
    if not last:
        return no_quads(res)
    if any(x['flags'] & BLEND for x in quads(last[-1])):
        return ['the menu quad stayed after the menu closed']
    if not any(x['flags'] & BLEND for f in last for x in quads(f)):
        return ['the menu quad never appeared']
    return []


SETTLE = [('wait', 8)]
SIZED = {'video_shader_enable': 'true'}
SHOT = [('wait', 8), ('shot', None)]

CASES = [
    {'name': '3ds-stereo', 'map': '3ds', 'steps': SETTLE,
     'check': check_screens()},
    {'name': '3ds-swap', 'map': '3ds',
     'settings': {'video_stereo_swap_eyes': 'true'}, 'steps': SETTLE,
     'check': check_screens(swap=True)},
    {'name': '3ds-horizontal', 'map': '3ds',
     'settings': {'video_screen_layout': '1'}, 'steps': SETTLE,
     'check': check_screens(horizontal=True)},
    {'name': 'no-map', 'map': 'none', 'steps': SETTLE,
     'check': check_frame},
    {'name': '3ds-stereo-threaded', 'map': '3ds',
     'settings': {'video_threaded': 'true'}, 'steps': SETTLE,
     'check': threaded(check_screens())},
    {'name': '3ds-output-size', 'map': '3ds', 'settings': SIZED,
     'args': ['--set-shader=' + PRESET], 'steps': SHOT,
     'check': check_sized_screens},
    {'name': 'no-map-output-size', 'map': 'none', 'settings': SIZED,
     'args': ['--set-shader=' + PRESET], 'steps': SHOT,
     'check': check_sized_frame},
    {'name': 'menu', 'map': '3ds', 'settings': {'menu_driver': 'ozone'},
     'steps': [('wait', 6), ('send', 'MENU_TOGGLE'), ('wait', 4)],
     'check': check_menu()},
    # The screens drawn in the same frames as the menu.
    {'name': 'menu-running', 'map': '3ds',
     'settings': {'menu_driver': 'ozone', 'menu_pause_libretro': 'false'},
     'steps': [('wait', 6), ('send', 'MENU_TOGGLE'), ('wait', 4)],
     'check': check_menu()},
    # A UI no bigger than the headset's density allows: copied, not drawn.
    {'name': 'menu-small', 'map': '3ds', 'settings': {'menu_driver': 'ozone'},
     'screen': (320, 192),
     'steps': [('wait', 6), ('send', 'MENU_TOGGLE'), ('wait', 4)],
     'check': check_menu((320, 192))},
    {'name': 'menu-overlay', 'map': '3ds', 'overlay': True,
     'settings': {'menu_driver': 'ozone'},
     'steps': [('wait', 6), ('send', 'MENU_TOGGLE'), ('wait', 3),
               ('shot', None), ('wait', 3)],
     'check': check_menu_overlay},
    {'name': 'menu-closes', 'map': '3ds', 'settings': {'menu_driver': 'ozone'},
     'steps': [('wait', 6), ('send', 'MENU_TOGGLE'), ('wait', 3),
               ('send', 'MENU_TOGGLE'), ('wait', 3)],
     'check': check_menu_closed},
]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    validate = '--validate' in sys.argv[1:]
    if len(args) < 3:
        print(__doc__)
        return 2
    retroarch = os.path.abspath(args[0])
    root = os.path.realpath(args[1])
    monado = read_env(args[2])
    names = args[3:]
    baseline = []
    if validate and monado.get('VALIDATION_BASELINE'):
        baseline = read_baseline(monado['VALIDATION_BASELINE'])
    os.makedirs(root, exist_ok=True)
    failed = 0
    for case in CASES:
        if names and case['name'] not in names:
            continue
        res = run_case(retroarch, root, monado, case, validate)
        errors = case['check'](res)
        if 'hung' in res.marks:
            errors.append('RetroArch did not quit')
        if validate:
            extra, known = unexpected(res.log, baseline)
            if extra:
                errors.append('validation: ' + ', '.join(extra))
            if not known:
                errors.append('validation: no message at all; did the '
                              'layer load?')
        print('%s %s' % ('FAIL' if errors else 'pass', case['name']))
        for e in errors:
            print('    ' + e)
        failed += bool(errors)
    print('%d failed' % failed)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
