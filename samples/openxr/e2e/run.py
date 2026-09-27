#!/usr/bin/env python3
"""End-to-end check of RetroArch's headset output.

Runs the video_views test core with Headset Output on against Monado's
simulated headset in headless gamescope, through the test API layer in
samples/openxr/test_layer, which records every xrEndFrame's quads and
saves the images they show. Not run in CI: it needs a GPU, gamescope,
Monado, and a RetroArch built with HAVE_OPENXR.

Usage: run.py <retroarch> <out dir> <monado.env> [--validate] [case ...]

Build the test core and layer first (make -C samples/cores/video_views,
make -C samples/openxr/test_layer). monado.env.example here runs
Monado's null compositor and simulated headset; RetroArch reaches Monado
through XR_RUNTIME_JSON, so the system's active runtime is not used.
gamescope and Monado get none of the desktop's display, session bus or
runtime dir.

monado.env holds KEY=VALUE lines (values may be double-quoted): MODE
(inprocess or service), RUNTIME_JSON (Monado's manifest), CLIENT_ENV and
SERVICE_ENV (space-separated KEY=VALUE pairs), SERVICE_SOCKET and
VALIDATION_BASELINE, relative to monado.env, by default
validation-baseline.txt here: the messages raised without RetroArch's
headset code, one per line, a VUID and then text the message must
contain (an object's name), so a VUID alone does not hide RetroArch's
own.
--validate runs RetroArch under the Vulkan validation layer, which must
be installed, and fails a case on any other message, or when none of the
baseline's appear: the window's own come every run, so without them the
layer did not load.
"""

import ctypes
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
from run import (CORE, free_port, isolated_env, read_png, send,  # noqa: E402
                 write_cfg)

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
DESTROY = re.compile(r'\[video_views\] context_destroy waited on the device '
                     r'from (\d+) to (\d+) us')
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
        if any(vuid == v and (t(text) if callable(t) else t in text)
               for v, t in baseline):
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
        self.events = []


def load(res):
    path = os.path.join(res.dir, 'xr', 'frames.jsonl')
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                try:
                    e = json.loads(line)
                except ValueError:
                    continue  # a line cut short at exit
                res.events.append(e)
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
    port = free_port()
    cfg = os.path.join(d, 'retroarch.cfg')
    write_cfg(cfg, d, 'vulkan', settings, port)
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
    # WSI layer still needs gamescope's own socket, by an absolute path in
    # gamescope's private runtime dir.
    helper = ''
    if case.get('unfocus'):
        helper = ' %s %s --unfocus-when %s &' % (
            shlex.quote(sys.executable), shlex.quote(os.path.abspath(__file__)),
            shlex.quote(os.path.join(d, 'unfocus')))
    guard = ('case "$DISPLAY" in ""|:0) echo "refusing DISPLAY=$DISPLAY" >&2;'
             ' exit 99;; esac; cd ' + shlex.quote(d) + ' || exit 98;' + helper +
             ' case "${GAMESCOPE_WAYLAND_DISPLAY:-}" in ""|/*) ;;'
             ' *) GAMESCOPE_WAYLAND_DISPLAY="$XDG_RUNTIME_DIR/'
             '$GAMESCOPE_WAYLAND_DISPLAY"; export GAMESCOPE_WAYLAND_DISPLAY;;'
             ' esac; export ' + exports + '; exec "$0" "$@"')

    w, h = case.get('screen', (W, H))
    cmd = ['gamescope', '--backend', 'headless',
           '-w', str(w), '-h', str(h), '-W', str(w), '-H', str(h),
           '-r', '60', '--', 'sh', '-c', guard,
           retroarch, '--config', cfg, '-L', case.get('core', CORE),
           '--verbose'] + case.get('args', [])
    if case.get('content'):
        cmd.append(case['content'])

    # gamescope and the service get none of the desktop's display, bus or
    # runtime dir.
    env = isolated_env()
    svc = svc_log = log = p = None
    res = Result(d)
    try:
        if monado.get('MODE') == 'service':
            svc_env = dict(env)
            svc_env['WAYLAND_DISPLAY'] = 'openxr-e2e-no-socket'
            svc_env['XDG_RUNTIME_DIR'] = RUNTIME_DIR
            # Monado's and libsurvive's config stay out of ~/.config.
            svc_env['XDG_CONFIG_HOME'] = os.path.join(d, 'config')
            svc_env.update(pairs(monado.get('SERVICE_ENV', '')))
            svc_log = open(os.path.join(d, 'service.log'), 'w')
            svc = subprocess.Popen(['monado-service'], cwd=d, env=svc_env,
                                   stdout=svc_log, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL,
                                   start_new_session=True)
            sock = os.path.join(d, RUNTIME_DIR, monado.get(
                'SERVICE_SOCKET', 'monado_comp_ipc'))
            deadline = time.time() + 10
            while time.time() < deadline and not os.path.exists(sock):
                time.sleep(0.1)

        log = open(os.path.join(d, 'run.log'), 'w')
        p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                             env=env, start_new_session=True)
        for kind, arg in case['steps']:
            if kind == 'wait':
                time.sleep(arg)
            elif kind == 'send':
                send(arg, port)
            elif kind == 'mark':
                res.marks[arg] = time.monotonic()
            elif kind == 'script':
                tmp = script + '.tmp'
                with open(tmp, 'w') as f:
                    f.write(arg + '\n')
                os.replace(tmp, script)
            elif kind == 'unfocus':
                open(os.path.join(d, 'unfocus'), 'w').close()
            elif kind == 'shot':
                send('SCREENSHOT', port)
                res.shot = wait_shot(d)
        send('QUIT', port)
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            res.marks['hung'] = time.monotonic()
    finally:
        stop(p)
        stop(svc)
        if log:
            log.close()
        if svc_log:
            svc_log.close()
        runtime_dir = env['XDG_RUNTIME_DIR']
        if os.path.basename(runtime_dir).startswith('ra-e2e-'):
            shutil.rmtree(runtime_dir, ignore_errors=True)
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


def check_recenter(res):
    shown = [f for f in res.frames if quads(f)]
    if not shown:
        return no_quads(res)
    errors = []
    first = [x for x in quads(shown[0]) if x['eye'] == 'left']
    last = [x for x in quads(shown[-1]) if x['eye'] == 'left']
    if not first or not last:
        return ['no left-eye quad']
    if not at(first[0], (0.0, 0.0, -1.8)):
        errors.append('screen 0 started at %s' % first[0]['pose'][:3])
    # The scripted head: 0.5, 0.2, 0.3, turned 90 degrees left.
    if not at(last[0], (-1.3, 0.2, 0.3)):
        errors.append('screen 0 at %s after recentering, want (-1.3, 0.2, 0.3)'
                      % last[0]['pose'][:3])
    if not facing(last[0], (0.0, 0.70711, 0.0, 0.70711)):
        errors.append('screen 0 turned %s, want a quarter turn left'
                      % last[0]['pose'][3:])
    if 'Headset recenter requested' not in res.log:
        errors.append('the network command did not reach the hotkey')
    return errors


def check_runtime_recenter(res):
    """The runtime's own recenter moves LOCAL: the screens go back
    straight ahead after a hotkey recenter. A STAGE change leaves them."""
    stage = res.marks.get('stage', 0) * 1e6
    local = res.marks.get('local', 0) * 1e6
    held = [x for f in res.frames if stage + 1e6 < f['t_us'] < local
            for x in quads(f) if x['eye'] == 'left']
    shown = [f for f in res.frames if quads(f)]
    if not held or not shown:
        return no_quads(res)
    errors = []
    moved = [x['pose'][:3] for x in held if not at(x, (-1.3, 0.2, 0.3))]
    if moved:
        errors.append('screen 0 left the hotkey\'s place before the LOCAL '
                      'change: %s' % moved[0])
    last = [x for x in quads(shown[-1]) if x['eye'] == 'left']
    if (     not last or not at(last[0], (0.0, 0.0, -1.8))
            or not facing(last[0], (0.0, 0.0, 0.0, 1.0))):
        errors.append('screen 0 ends at %s, want straight ahead'
                      % (last[0]['pose'] if last else None))
    n = res.log.count('[OpenXR] Recentered by the runtime.')
    if n != 1:
        errors.append('%d runtime recenters logged, want 1' % n)
    return errors


def check_pacing(res):
    """The headset's frames keep its own rate while the core is paused,
    and no new images are drawn for it."""
    if 'paused' not in res.marks or 'resumed' not in res.marks:
        return ['the run did not pause']
    t0 = (res.marks['paused'] + 0.5) * 1e6
    t1 = res.marks['resumed'] * 1e6
    periods = sorted(f['period_ns'] for f in res.frames if f['period_ns'] > 0)
    if not periods:
        return ['no frame period recorded']
    period = periods[len(periods) // 2]
    frames = [f for f in res.frames if t0 <= f['t_us'] <= t1]
    released = [r for r in res.releases if t0 <= r['t_us'] <= t1]
    before = [r for r in res.releases if r['t_us'] < t0 - 1e6]
    want = (t1 - t0) * 1e3 / period
    errors = []
    if abs(len(frames) - want) > 0.15 * want:
        errors.append('%d headset frames while paused, want about %.0f'
                      % (len(frames), want))
    if len(released) > 2:
        errors.append('%d images released while paused, want none'
                      % len(released))
    if not before:
        errors.append('no images were released before the pause')
    return errors


def check_focus(res):
    """Pause Content When Not Active with the window unfocused: the content
    runs while the headset session is focused, and pauses while the
    session is only visible."""
    if '[e2e] FocusOut sent to window' not in res.log:
        return ['no FocusOut was sent to the window']
    t = dict((k, v * 1e6) for k, v in res.marks.items())
    errors = []
    for a, b, runs, what in (('unfocused', 'visible', True, 'focused'),
                             ('visible', 'refocused', False, 'visible'),
                             ('refocused', 'end', True, 'focused again')):
        n = len([r for r in res.releases if t[a] + 1e6 <= r['t_us'] <= t[b]])
        if runs and n < 30:
            errors.append('%d images released with the session %s, want the '
                          'content running' % (n, what))
        elif not runs and n > 2:
            errors.append('%d images released with the session %s, want the '
                          'content paused' % (n, what))
    return errors


def window_is(res, want, what):
    if not res.shot:
        return ['no screenshot']
    w, h, bpp, rows = read_png(res.shot)
    got = tuple(rows[240][800 * bpp:800 * bpp + 3])
    if not near(got, want):
        return ['the window shows %s at the top screen, want %s (%s)'
                % (got, want, what)]
    return []


def check_no_runtime(res):
    errors = []
    # The loader may fail listing extensions or creating the instance.
    if (     '[OpenXR] No runtime' not in res.log
            and '[OpenXR] The runtime lacks' not in res.log):
        errors.append('no "[OpenXR] No runtime" in the log')
    if any(quads(f) for f in res.frames):
        errors.append('quads were submitted without a runtime')
    return errors + window_is(res, GREEN, 'the 2D map')


def check_session_fails(res):
    errors = []
    for line in ('[OpenXR] Rebuilding video without headset output.',
                 '[Video] Reinitialising the video driver at its request.',
                 '[OpenXR] Starting once without headset output after a failure.'):
        if line not in res.log:
            errors.append('missing "%s"' % line)
    if any(quads(f) for f in res.frames):
        errors.append('quads were submitted after the session failed')
    return errors + window_is(res, GREEN, 'the 2D map')


def check_session_lost(res):
    errors = []
    if '[OpenXR] The headset session ended' not in res.log:
        errors.append('no "[OpenXR] The headset session ended" in the log')
    lost = res.marks.get('lost', 0) * 1e6
    late = [f for f in res.frames if f['t_us'] > lost + 500000 and quads(f)]
    if late:
        errors.append('%d frames with quads after the session was lost'
                      % len(late))
    return errors + window_is(res, GREEN, 'the 2D map again')


# ---- Headset input ----

PAD_RE = re.compile(r'\[video_views\] pad port=(\d) buttons=0x([0-9a-f]+) '
                    r'lx=(-?\d+) ly=(-?\d+) rx=(-?\d+) ry=(-?\d+) '
                    r'l2=(-?\d+) r2=(-?\d+)')
POINTER_RE = re.compile(r'\[video_views\] pointer (?:x=(-?\d+) y=(-?\d+) '
                        r'pressed=1 packed=\((\d+),(\d+)\)|pressed=0)')
GUN_RE = re.compile(r'\[video_views\] lightgun x=(-?\d+) y=(-?\d+) '
                    r'offscreen=(\d) trigger=(\d) '
                    r'packed=\((-?\d+),(-?\d+)\)')
RUMBLE_RE = re.compile(r'\[video_views\] rumble port=(\d) strong=(\d+) '
                       r'weak=(\d+)')
MENU_RE = re.compile(r'\[Menu\] Headset pointer x=(-?\d+) y=(-?\d+) '
                     r'pressed=(\d) selection=(\d+)')
RP = dict((n, 1 << i) for i, n in enumerate(
    ('B', 'Y', 'SELECT', 'START', 'UP', 'DOWN', 'LEFT', 'RIGHT',
     'A', 'X', 'L', 'R', 'L2', 'R2', 'L3', 'R3')))
PROFILES = ('/interaction_profiles/valve/index_controller',
            '/interaction_profiles/oculus/touch_controller',
            '/interaction_profiles/htc/vive_controller',
            '/interaction_profiles/khr/simple_controller')


def script(*lines):
    return '\n'.join(lines)


def core_events(res):
    """The core's and the menu's input lines, in log order, as
    (kind, fields): pad, pointer, gun, rumble, menu."""
    out = []
    for line in res.log.splitlines():
        m = PAD_RE.search(line)
        if m:
            v = [int(g) for g in m.groups()[2:]]
            out.append(('pad', {'port': int(m.group(1)),
                                'buttons': int(m.group(2), 16),
                                'lx': v[0], 'ly': v[1], 'rx': v[2],
                                'ry': v[3], 'l2': v[4], 'r2': v[5]}))
            continue
        m = POINTER_RE.search(line)
        if m:
            if m.group(1) is None:
                out.append(('pointer', {'pressed': 0}))
            else:
                out.append(('pointer', {'pressed': 1,
                                        'cx': int(m.group(3)),
                                        'cy': int(m.group(4))}))
            continue
        m = GUN_RE.search(line)
        if m:
            v = [int(g) for g in m.groups()]
            out.append(('gun', {'x': v[0], 'y': v[1], 'offscreen': v[2],
                                'trigger': v[3], 'cx': v[4], 'cy': v[5]}))
            continue
        m = RUMBLE_RE.search(line)
        if m:
            v = [int(g) for g in m.groups()]
            out.append(('rumble', {'port': v[0], 'strong': v[1],
                                   'weak': v[2]}))
            continue
        m = MENU_RE.search(line)
        if m:
            v = [int(g) for g in m.groups()]
            out.append(('menu', {'x': v[0], 'y': v[1], 'pressed': v[2],
                                 'selection': v[3]}))
    return out


def kinds(evs, kind):
    return [f for k, f in evs if k == kind]


def pads(res, port):
    return [f for f in kinds(core_events(res), 'pad') if f['port'] == port]


def events(res, name):
    return [e for e in res.events if e['ev'] == name]


def close(got, want, tol=2):
    return all(abs(a - b) <= tol for a, b in zip(got, want))


def find(seq, start, pred):
    """The index of the first item from start on that matches, or -1."""
    for i in range(max(start, 0), len(seq)):
        if pred(seq[i]):
            return i
    return -1


def moved(f):
    return any(f[k] for k in ('buttons', 'lx', 'ly', 'rx', 'ry', 'l2', 'r2'))


def controllers(res):
    """RetroArch's controller lines in order, one thread's: 'live' when
    they are read again, 'released' when focus is lost."""
    return ['released' if 'released' in line else 'live'
            for line in res.log.splitlines()
            if '[OpenXR] Controllers: ' in line
            or '[OpenXR] Controllers released' in line]


def bindings_errors(res):
    got = dict((e['profile'], e['result']) for e in events(res, 'bindings'))
    return ['bindings for %s: %s (see Monado\'s warning in run.log)'
            % (p, got.get(p, 'not suggested'))
            for p in PROFILES if got.get(p) != 0]


def sync_errors(res, want):
    seen = set(tuple(sorted(e['sets'])) for e in events(res, 'sync'))
    seen.discard(())
    if not seen:
        return ['xrSyncActions never made a set active']
    return ['synced %s, want only %s' % (list(s), sorted(want))
            for s in sorted(seen) if s != tuple(sorted(want))]


def cursors(frame):
    """The laser's dots: small blended quads."""
    return [q for q in quads(frame)
            if q['flags'] & BLEND and q['size'][0] < 0.1]


def menus(frame):
    return [q for q in quads(frame)
            if q['flags'] & BLEND and q['size'][0] >= 0.1]


ALL_BUTTONS = script(*['action combined/%s 1' % n for n in (
    'b', 'y', 'select', 'start', 'dpad_up', 'dpad_down', 'dpad_left',
    'dpad_right', 'a', 'x', 'l', 'r', 'l2', 'r2', 'l3', 'r3')])
STICKS = script('action combined/left_stick 0.5 0.5',
                'action combined/right_stick -1 -0.25',
                'action combined/l2 0.25', 'action combined/r2 0.75')
SEPARATE = script('action separate/b@left 1',
                  'action separate/stick@left 0.5 0.5',
                  'action separate/a@right 1',
                  'action separate/r2@right 0.6',
                  'action combined/x 1')
HELD = script('action combined/b 1', 'action combined/left_stick 1 0')
RETURNED = script('action combined/a 1', 'action combined/left_stick -1 0')
DPAD = script('action combined/left_stick 0.8 0.8',
              'action combined/right_stick 0.5 0')


def check_combined(res):
    errors = bindings_errors(res) + sync_errors(res, ('combined', 'pointer'))
    p0 = pads(res, 0)
    i = find(p0, 0, lambda f: f['buttons'] == 0xffff
             and (f['l2'], f['r2']) == (32767, 32767))
    # Sticks up are negative Y; analog L2/R2 with only R2 past halfway.
    j = find(p0, i + 1, lambda f: f['buttons'] == RP['R2'] and close(
        (f['lx'], f['ly'], f['rx'], f['ry'], f['l2'], f['r2']),
        (16383, -16383, -32767, 8191, 8191, 24575)))
    k = find(p0, j + 1, lambda f: not moved(f))
    if i < 0:
        errors.append('player 1 never had every button and both triggers: %s'
                      % p0[-3:])
    elif j < 0:
        errors.append('player 1 never had the sticks and half triggers: %s'
                      % p0[i:i + 3])
    elif k < 0:
        errors.append('player 1 was not released')
    if any(moved(f) for f in pads(res, 1)):
        errors.append('player 2 moved in Combined')
    if '[OpenXR] Controllers: combined.' not in res.log:
        errors.append('no "[OpenXR] Controllers: combined." in the log')
    if 'Headset recenter requested' not in res.log:
        errors.append('Recenter did not reach the hotkey')
    if not any(menus(f) for f in res.frames):
        errors.append('RetroArch Menu did not open the menu')
    return errors


def check_separate(res):
    errors = bindings_errors(res) + sync_errors(res, ('separate', 'pointer'))
    p0, p1 = pads(res, 0), pads(res, 1)
    if find(p0, 0, lambda f: f['buttons'] == RP['B']
            and close((f['lx'], f['ly']), (16383, -16383))) < 0:
        errors.append('player 1 never had B and the left stick: %s' % p0[-3:])
    if find(p1, 0, lambda f: f['buttons'] == RP['A'] | RP['R2']
            and close((f['r2'],), (19660,))) < 0:
        errors.append('player 2 never had A and R2: %s' % p1[-3:])
    if any(f['buttons'] & RP['X'] for f in p0 + p1):
        errors.append('an action of the unsynced Combined set reached a pad')
    if not p0 or moved(p0[-1]) or not p1 or moved(p1[-1]):
        errors.append('the pads were not released at the end')
    if '[OpenXR] Controllers: separate.' not in res.log:
        errors.append('no "[OpenXR] Controllers: separate." in the log')
    return errors


def check_input_focus(res):
    p0 = pads(res, 0)
    i = find(p0, 0, lambda f: f['buttons'] == RP['B'] and f['lx'] == 32767)
    j = find(p0, i + 1, lambda f: not moved(f)) if i >= 0 else -1
    k = (find(p0, j + 1, lambda f: f['buttons'] == RP['B']
              and f['lx'] == 32767) if j >= 0 else -1)
    errors = []
    if i < 0:
        errors.append('B and the stick never reached player 1')
    elif j < 0:
        errors.append('still held after the headset lost focus')
    elif k < 0:
        errors.append('not held again once the headset had focus back')
    if 'Controllers released: the headset is not focused' not in res.log:
        errors.append('no "Controllers released" in the log')
    return errors


def check_input_unfocused(res):
    """The window unfocused, with Pause Content When Not Active and no
    background joypads: check_focus()'s pause rule, and the headset's
    controllers still reach player 1 while its session is focused. The
    content is paused while the session is only visible, so the core
    never reads the release: RetroArch's log shows it."""
    errors = check_focus(res)
    p0 = pads(res, 0)
    i = find(p0, 0, lambda f: f['buttons'] == RP['B'] and f['lx'] == 32767)
    # RETURNED is only scripted once the session is focused again.
    j = (find(p0, i + 1, lambda f: f['buttons'] == RP['A']
              and f['lx'] == -32767) if i >= 0 else -1)
    if i < 0:
        errors.append('B and the stick never reached player 1 with the '
                      'window unfocused')
    elif j < 0:
        errors.append('A and the stick never reached player 1 once the '
                      'headset had focus back: %s' % p0[i:i + 3])
    seq = controllers(res)
    if seq[:3] != ['live', 'released', 'live']:
        errors.append('controllers went %s, want live, released while '
                      'visible, live again' % seq)
    return errors


def check_input_dpad(res):
    """Forced Left Analog on player 1: the left stick is the D-pad and
    reads centred, the right stays analog."""
    p0 = pads(res, 0)
    i = find(p0, 0, lambda f: f['buttons'] == RP['UP'] | RP['RIGHT']
             and (f['lx'], f['ly']) == (0, 0)
             and close((f['rx'], f['ry']), (16383, 0)))
    errors = []
    if i < 0:
        errors.append('player 1 never had Up and Right from the left stick, '
                      'centred, with the right stick at 0.5: %s' % p0[-3:])
    elif find(p0, i + 1, lambda f: not moved(f)) < 0:
        errors.append('player 1 was not released')
    return errors


# Rays from a hand at (0.2, -0.4, -0.3) to points on the default 3DS
# quads: screen 0 at (0, 0, -1.8) 1.6x0.96, screen 1 at (0, -0.992, -1.8)
# 1.28x0.96, the menu at (0, 0, -1.7) 1.6x0.96.
AIM_BOTTOM = 'aim right 0.2 -0.4 -0.3 -0.32 -1.232 -1.8'   # screen 1, 0.25 0.75
AIM_TOP = 'aim right 0.2 -0.4 -0.3 0.4 0.24 -1.8'          # screen 0, 0.75 0.25
AIM_GUN = 'aim right 0.2 -0.4 -0.3 -0.4 0.24 -1.8'         # screen 0, 0.25 0.25
AIM_MISS = 'aim right 0.2 -0.4 -0.3 3.0 0.0 -1.8'          # beside every quad
AIM_LEFT_MISS = 'aim left -0.2 -0.4 -0.3 -3.0 0.0 -1.8'    # beside every quad
AIM_LEFT_BOTTOM = 'aim left -0.2 -0.4 -0.3 -0.32 -0.752 -1.8'   # screen 1, 0.25 0.25
AIM_RIGHT_BOTTOM = 'aim right 0.2 -0.4 -0.3 0.32 -1.232 -1.8'   # screen 1, 0.75 0.75
L2 = 'action combined/l2 1'
R2 = 'action combined/r2 1'


def trigger_errors(res):
    if any(f['buttons'] & (RP['L2'] | RP['R2']) or f['l2'] or f['r2']
           for f in pads(res, 0)):
        return ['a trigger that pointed also pressed L2 or R2']
    return []


def cursor_errors(res, want):
    shown = [f for f in res.frames if cursors(f)]
    if not shown:
        return ['no laser dot']
    errors = []
    c = cursors(shown[-1])[0]
    if not at(c, want):
        errors.append('the dot is at %s, want %s' % (c['pose'][:3], want))
    snaps = [f for f in shown if f['snap']]
    if not snaps:
        errors.append('no snapshot with the dot')
    else:
        fr = snaps[-1]
        dot = cursors(fr)[0]
        rgb, alpha = colour(image(res, fr, dot), 0.5, 0.5)
        if not near(rgb, WHITE) or alpha < 250:
            errors.append('the dot\'s centre is %s alpha %d' % (rgb, alpha))
        _, alpha = colour(image(res, fr, dot), 0.02, 0.02)
        if alpha:
            errors.append('the dot\'s corner is not transparent')
    if cursors(res.frames[-1]):
        errors.append('the dot stayed after the aim went away')
    return errors


def check_touch(res):
    evs = core_events(res)
    presses = [f for f in kinds(evs, 'pointer') if f['pressed']]
    errors = []
    # Screen 1 is the view (240, 240, 320, 240) of the 800x480 frame.
    if not presses:
        errors.append('no touch reached the core')
    elif not close((presses[0]['cx'], presses[0]['cy']), (320, 420), 3):
        errors.append('touched at %d,%d, want 320,420'
                      % (presses[0]['cx'], presses[0]['cy']))
    if not any(not f['pressed'] for f in kinds(evs, 'pointer')):
        errors.append('the touch was not released')
    return (errors + trigger_errors(res)
            + cursor_errors(res, (-0.32, -1.232, -1.8)))


def check_top_auto(res):
    errors = []
    if find(pads(res, 0), 0, lambda f: f['buttons'] & RP['R2']
            and f['r2'] == 32767) < 0:
        errors.append('the trigger on the top screen was not R2')
    if any(f['pressed'] for f in kinds(core_events(res), 'pointer')):
        errors.append('the top screen was touched in Auto')
    if any(cursors(f) for f in res.frames):
        errors.append('a dot showed on the top screen in Auto')
    return errors


def check_lightgun(res):
    """Always: the trigger fires the gun on screen 0, then off every
    screen (an off-screen shot) with no R2; the other hand, off the
    screens too, keeps L2."""
    evs = core_events(res)
    errors = []
    hit = find(evs, 0, lambda kf: kf[0] == 'gun'
               and not kf[1]['offscreen'] and kf[1]['trigger'])
    miss = (find(evs, hit + 1, lambda kf: kf[0] == 'gun'
                 and kf[1]['offscreen']) if hit >= 0 else -1)
    p0 = [i for i, (k, f) in enumerate(evs) if k == 'pad' and f['port'] == 0]
    if hit < 0:
        return ['the light gun never fired on screen 0: %s'
                % kinds(evs, 'gun')[-3:]]
    g = evs[hit][1]
    # Screen 0 maps into the left eye's view (0, 0, 400, 240).
    if not close((g['cx'], g['cy']), (100, 60), 3):
        errors.append('the gun aimed at %d,%d, want 100,60' % (g['cx'], g['cy']))
    if miss < 0:
        errors.append('the gun did not read offscreen on a miss')
    else:
        m = evs[miss][1]
        if (m['x'], m['y'], m['trigger']) != (-32768, -32768, 1):
            errors.append('the shot off the screens reads %s' % m)
        # The core logs a frame's pads just before its gun: the pad lines
        # directly before the miss are the miss's frame.
        k = miss
        while k > 0 and evs[k - 1][0] == 'pad':
            k -= 1
        pointing = ([i for i in p0 if i < hit][-1:]
                    + [i for i in p0 if hit < i < k])
        off = [i for i in p0 if i >= k]
        if any(evs[i][1]['buttons'] & RP['R2'] for i in pointing):
            errors.append('R2 while the gun pointed at the screen')
        if any(evs[i][1]['buttons'] & RP['R2'] for i in off):
            errors.append('R2 while the gun shot off the screens')
        if not any(evs[i][1]['buttons'] & RP['L2'] for i in off):
            errors.append('the other hand off the screens did not press L2')
    return errors + cursor_errors(res, (-0.4, 0.24, -1.8))


def packed_at(f, want):
    return 'cx' in f and close((f['cx'], f['cy']), want, 3)


def check_two_hands(res):
    evs = core_events(res)
    touches = kinds(evs, 'pointer')
    errors = []
    left, right = (320, 300), (480, 420)
    # Before any trigger, both on screen 1: the right hand points.
    aimed = [f for f in kinds(evs, 'gun')
             if packed_at(f, left) or packed_at(f, right)]
    if not aimed or not packed_at(aimed[0], right):
        errors.append('the right hand did not point first: %s' % aimed[:2])
    i = find(touches, 0, lambda f: f['pressed'] and packed_at(f, left))
    j = (find(touches, i + 1, lambda f: f['pressed'] and packed_at(f, right))
         if i >= 0 else -1)
    if i < 0:
        errors.append('the left hand did not touch first: %s' % touches[:3])
    elif j < 0:
        errors.append('the right hand did not take over when its trigger '
                      'went down: %s' % touches[i:i + 3])
    elif not any(not f['pressed'] for f in touches[i + 1:j]):
        errors.append('the touch dragged from hand to hand without a lift')
    if not any(len(cursors(f)) == 2 for f in res.frames):
        errors.append('no frame with a dot for each hand')
    return errors + trigger_errors(res)


def check_frame_always(res):
    """A core without views in Always: the whole frame is a light gun
    target, mapped over the frame the core last sent."""
    evs = core_events(res)
    shots = [f for f in kinds(evs, 'gun')
             if not f['offscreen'] and f['trigger']]
    errors = []
    # (0.25, 0.25) of the 800x480 frame.
    if not shots:
        errors.append('the light gun never fired on the frame: %s'
                      % kinds(evs, 'gun')[-3:])
    elif not close((shots[0]['cx'], shots[0]['cy']), (200, 120), 3):
        errors.append('the gun aimed at %d,%d, want 200,120'
                      % (shots[0]['cx'], shots[0]['cy']))
    return (errors + trigger_errors(res)
            + cursor_errors(res, (-0.4, 0.24, -1.8)))


def check_single_auto(res):
    """A core without views in Auto: no screen is live, so the laser
    leaves the core's pointer and light gun to the mouse, and the trigger
    stays R2; with the menu open, the laser points at the menu alone. The
    laser's miss reads -32768; the x driver's gun follows the mouse,
    which headless gamescope leaves at the window's corner (-32767)."""
    evs = core_events(res)
    errors = []
    if find(pads(res, 0), 0, lambda f: f['buttons'] & RP['R2']
            and f['r2'] == 32767) < 0:
        errors.append('the trigger on the only screen was not R2')
    laser = [f for f in kinds(evs, 'gun')
             if (f['x'], f['y']) == (-32768, -32768) or f['trigger']]
    if laser:
        errors.append('the laser answered the light gun: %s' % laser[:2])
    if any(f['pressed'] for f in kinds(evs, 'pointer')):
        errors.append('the only screen was touched in Auto')
    shown = [f for f in res.frames if cursors(f)]
    if not shown or not all(menus(f) for f in shown):
        errors.append('the dot was not on the open menu alone')
    return errors


def check_gun_tracking(res):
    """Always: the right hand takes the gun on screen 0, then loses
    tracking. It keeps the gun: its trigger still shoots off the screens,
    the left trigger stays L2, and tracked again the right still aims."""
    guns = kinds(core_events(res), 'gun')
    p0 = pads(res, 0)
    errors = []
    hit = find(guns, 0, lambda g: g['trigger'] and packed_at(g, (100, 60)))
    shot = (find(guns, hit + 1, lambda g: g['offscreen'] and g['trigger'])
            if hit >= 0 else -1)
    back = (find(guns, shot + 1, lambda g: not g['trigger']
                 and packed_at(g, (100, 60))) if shot >= 0 else -1)
    if hit < 0:
        errors.append('the right hand never shot screen 0: %s' % guns[-3:])
    elif shot < 0:
        errors.append('the untracked right hand\'s trigger did not shoot '
                      'off the screens: %s' % guns[hit:hit + 4])
    elif back < 0:
        errors.append('tracked again, the right hand did not aim the gun: %s'
                      % guns[shot:shot + 3])
    if not any(f['buttons'] & RP['L2'] and f['l2'] for f in p0):
        errors.append('the left trigger was not L2')
    if any(f['buttons'] & RP['R2'] or f['r2'] for f in p0):
        errors.append('the right trigger pressed R2')
    return errors


AIM_MENU_HIGH = 'aim right 0.2 -0.4 -0.3 0.16 0.192 -1.7'   # the menu, 0.6 0.3
AIM_MENU_LOW = 'aim right 0.2 -0.4 -0.3 0.16 -0.048 -1.7'   # the menu, 0.6 0.55


def check_menu_laser(res):
    """Ozone at 1600x960: pointing lower moves the selection down, and the
    trigger clicks."""
    lines = kinds(core_events(res), 'menu')
    high = [i for i, f in enumerate(lines)
            if close((f['x'], f['y']), (960, 288), 4)]
    low = [i for i, f in enumerate(lines)
           if close((f['x'], f['y']), (960, 528), 4)]
    if not high or not low:
        return ['the laser did not reach the menu at both points: %s'
                % lines[:6]]
    errors = []
    before = lines[high[-1]]['selection']
    hover = [lines[i]['selection'] for i in low if not lines[i]['pressed']]
    if not any(s > before for s in hover):
        errors.append('pointing lower did not move the selection down '
                      '(%d, then %s)' % (before, hover))
    press = [i for i in low if lines[i]['pressed']]
    if not press:
        errors.append('the trigger did not press on the menu')
    elif find(lines, press[0] + 1, lambda f: not f['pressed']) < 0:
        errors.append('the press on the menu was not released')
    shown = [f for f in res.frames if cursors(f)]
    if not shown or not any(at(c, (0.16, -0.048, -1.7))
                            for c in cursors(shown[-1])):
        errors.append('no dot on the menu where the laser points')
    return errors


def check_menu_rgui(res):
    """RGUI: the point is in its own small framebuffer, and a click
    selects the entry under it."""
    lines = kinds(core_events(res), 'menu')
    if not lines:
        return ['the laser never reached the menu']
    errors = []
    if max(f['y'] for f in lines) >= 480:
        errors.append('menu pointer y %d is window pixels, not RGUI\'s'
                      % max(f['y'] for f in lines))
    press = find(lines, 0, lambda f: f['pressed'])
    if press < 0:
        return errors + ['the trigger did not press on the menu']
    first = lines[0]['selection']
    if not any(f['selection'] > first for f in lines[press:]):
        errors.append('clicking lower did not move RGUI\'s selection (%d)'
                      % first)
    return errors


# A held hand is never still: a pixel, then two, right of AIM_MENU_HIGH
# (half a pixel in, so x truncates to 961 and 962 either side of float
# error).
AIM_MENU_SHAKE = 'aim right 0.2 -0.4 -0.3 0.1615 0.192 -1.7'
AIM_MENU_BACK = 'aim right 0.2 -0.4 -0.3 0.1625 0.192 -1.7'
# The menu's header: Ozone takes a tap or short press there as Back, a
# long press as nothing. Not at its top edge: a menu cursor drawn across
# it wraps the Vulkan viewport (master's gfx_display_vk_draw()).
AIM_MENU_TOP = 'aim right 0.2 -0.4 -0.3 0.16 0.4224 -1.7'         # 0.6 0.06
AIM_LEFT_MENU_TOP = 'aim left -0.2 -0.4 -0.3 -0.16 0.4224 -1.7'   # 0.4 0.06
STICK_DOWN = 'action combined/left_stick 0 -1'
# Stick taps shorter than the menu's repeat delay move one entry each,
# so Ozone's list never scrolls (scrolling wraps the viewport too). Each
# starts with the laser moving.
SHAKE = [('script', AIM_MENU_SHAKE), ('wait', 0.05)] + [
    step for _ in range(2) for step in (
        ('script', script(AIM_MENU_HIGH, STICK_DOWN)), ('wait', 0.05),
        ('script', script(AIM_MENU_SHAKE, STICK_DOWN)), ('wait', 0.05),
        ('script', AIM_MENU_HIGH), ('wait', 0.05),
        ('script', AIM_MENU_SHAKE), ('wait', 0.05))]


def changes(seq):
    return sum(1 for a, b in zip(seq, seq[1:]) if a != b)


def check_menu_stick(res):
    """Ozone: the headset's stick moves the selection under a still laser
    and under one that shakes by a pixel; the laser leaving the menu and
    coming back a pixel from where it left keeps the stick's selection."""
    lines = kinds(core_events(res), 'menu')
    shake = [i for i, f in enumerate(lines) if f['x'] == 961]
    back = find(lines, 0, lambda f: f['x'] == 962)
    if not shake or back < 1:
        return ['the laser did not shake on the menu and come back: %s'
                % lines[:6]]
    sel = [f['selection'] for f in lines]
    errors = []
    # A still laser selects once, where it first lands; the rest is the
    # stick's.
    if changes(sel[:shake[0]]) < 2:
        errors.append('the stick did not move the selection under a still '
                      'laser: %s' % sel[:shake[0]])
    if not changes(sel[shake[0]:shake[-1] + 1]):
        errors.append('the stick did not move the selection under a shaking '
                      'laser (%d)' % sel[shake[0]])
    if len(set(sel[back - 1:])) != 1:
        errors.append('the laser leaving the menu and coming back moved the '
                      'selection: %s' % sel[back - 1:])
    return errors


def check_menu_hands(res):
    """Both hands on the menu: the right points first, the left's trigger
    takes the press, and the right's, pulled while the left's is held,
    takes it back with a lift between."""
    lines = kinds(core_events(res), 'menu')
    left, right = (640, 57), (960, 57)
    errors = []
    if not lines or not close((lines[0]['x'], lines[0]['y']), right, 4):
        errors.append('the right hand did not point first: %s' % lines[:2])
    i = find(lines, 0, lambda f: f['pressed']
             and close((f['x'], f['y']), left, 4))
    j = (find(lines, i + 1, lambda f: f['pressed']
              and close((f['x'], f['y']), right, 4)) if i >= 0 else -1)
    if i < 0:
        errors.append('the left hand did not press on the menu: %s'
                      % lines[:4])
    elif j < 0:
        errors.append('the right hand did not take the press over: %s'
                      % lines[i:i + 3])
    elif not any(not f['pressed'] for f in lines[i + 1:j]):
        errors.append('the press dragged from hand to hand without a lift')
    if not any(len(cursors(f)) == 2 for f in res.frames):
        errors.append('no frame with a dot for each hand')
    return errors


# RGUI's 320x240 rows are 11 px from y 26; its Quick Menu has Reset at 1
# and Take Screenshot at 6.
AIM_RGUI_RESET = 'aim right 0.2 -0.4 -0.3 0.16 0.3101 -1.7'        # 0.6 0.177
AIM_RGUI_SHOT = 'aim right 0.2 -0.4 -0.3 0.16 0.09 -1.7'           # 0.6 0.406
# Shorter than a tap (200 ms).
CLICK = 0.1


def menu_cursor_errors(res, uv):
    """The laser's dot is its cursor: no menu snapshot may show the
    menu's own mouse cursor, a bright mark, around the laser's point."""
    for fr in res.frames:
        for q in (menus(fr) if fr['snap'] else []):
            path = image(res, fr, q)
            if not os.path.exists(path):
                continue
            w, h, bpp, rows = read_png(path)
            for y in range(max(0, int((uv[1] - 0.05) * h)),
                           min(h, int((uv[1] + 0.05) * h) + 1)):
                for x in range(max(0, int((uv[0] - 0.03) * w)),
                               min(w, int((uv[0] + 0.03) * w) + 1)):
                    if min(rows[y][x * bpp:x * bpp + 3]) > 200:
                        return ['snapshot %d shows the menu\'s own cursor '
                                'at %s' % (fr['n'], uv)]
    return []


def click_errors(res, before, uv):
    """One quick click with `before` selected: the pointed entry, Restart,
    acts once (the core resets once), and at the release, not the press."""
    evs = []
    for line in res.log.splitlines():
        m = MENU_RE.search(line)
        if m:
            evs.append(int(m.group(3)))
        elif '[Core] Reset.' in line:
            evs.append('reset')
    press = find(evs, 0, lambda e: e == 1)
    if press < 0:
        return ['the trigger did not press on the menu']
    lines = kinds(core_events(res), 'menu')
    at_press = [f for f in lines if f['pressed']][0]['selection']
    release = find(evs, press + 1, lambda e: e == 0)
    resets = [i for i, e in enumerate(evs) if e == 'reset']
    errors = []
    if at_press != before:
        errors.append('selection %d at the press, want %d'
                      % (at_press, before))
    if len(resets) != 1:
        errors.append('the click restarted %d times, want once'
                      % len(resets))
    elif release < 0 or resets[0] < release:
        errors.append('the click acted on the press, before the release')
    return errors + menu_cursor_errors(res, uv)


def check_menu_click(res):
    """Ozone: the laser's hover selects Restart, and a quick click on
    it restarts once."""
    return click_errors(res, 1, (0.6, 0.3))


def check_menu_click_rgui(res):
    """RGUI, with Core Options selected: a quick click on Reset restarts
    once; the press does nothing to Core Options."""
    return click_errors(res, 4, (0.6, 0.177))


def check_hw_teardown(res):
    """The core's context_destroy waits on the device without the queue
    lock, at a video reinit and at unload: the headset's frames stop
    before it and start again after."""
    waits = [(int(a), int(b)) for a, b in DESTROY.findall(res.log)]
    if len(waits) != 2:
        return ['%d context_destroy waits, want 2 (reinit, unload)'
                % len(waits)]
    errors = []
    for i, (t0, t1) in enumerate(waits):
        inside = [f for f in res.frames if t0 <= f['t_us'] <= t1]
        if inside:
            errors.append('%d headset frames while the core waited on the '
                          'device (%s)' % (len(inside), ('reinit', 'unload')[i]))
        end = waits[i + 1][0] if i + 1 < len(waits) else float('inf')
        if not [f for f in res.frames if t1 < f['t_us'] < end]:
            errors.append('no headset frames after the %s'
                          % ('reinit', 'unload')[i])
    return errors


def check_kept_retry(res):
    """A kept context's session that the runtime ended starts again at
    the next video reinit, on the same device."""
    errors = []
    for line in ('[OpenXR] The headset session ended',
                 '[Vulkan] Using cached Vulkan context.'):
        if line not in res.log:
            errors.append('missing "%s"' % line)
    made = res.log.count('[OpenXR] Session created.')
    if made != 2:
        errors.append('%d sessions created, want 2' % made)
    t = res.marks.get('reinit', 0) * 1e6
    after = [f for f in res.frames if f['t_us'] > t + 1e6]
    if len(after) < 40:
        errors.append('%d headset frames after the reinit' % len(after))
    stereo = re.findall(r'\[video_views\] presents=\d stereo=(\d)', res.log)
    if '0' not in stereo or stereo[-1:] != ['1']:
        errors.append('the core saw stereo %s, want it off and on again'
                      % ' '.join(stereo))
    ready = res.log.count('[OpenXR] Headset controllers ready.')
    if ready != 2:
        errors.append('headset controllers ready %d times, want 2 (one '
                      'per session)' % ready)
    if '[OpenXR] Headset controllers unavailable' in res.log:
        errors.append('headset controllers unavailable in a session')
    # The core logs no pads with a kept Vulkan context: RetroArch's log
    # shows the release when the session ends, and the new session's.
    seq = controllers(res)
    if seq != ['live', 'released', 'live']:
        errors.append('controllers went %s, want live, released when the '
                      'session ended, live in the new one' % seq)
    # The laser's dot is VULKAN_OPENXR_CURSOR_DIM square; each start makes
    # one, and each start here made a session.
    dots = [e for e in events(res, 'swapchain')
            if (e['w'], e['h']) == (32, 32)]
    if len(dots) != 2:
        errors.append('%d laser dot swapchains, want 2 (one per start)'
                      % len(dots))
    return errors


def check_kept_lost(res):
    """A kept context whose runtime lost the instance needs the content
    loaded again: the reinit says so and makes no session."""
    errors = []
    for line in ('[OpenXR] The headset session ended',
                 '[Vulkan] Using cached Vulkan context.',
                 'headset output starts when it is loaded again'):
        if line not in res.log:
            errors.append('missing "%s"' % line)
    made = res.log.count('[OpenXR] Session created.')
    if made != 1:
        errors.append('%d sessions created, want 1' % made)
    t = res.marks.get('reinit', 0) * 1e6
    after = [f for f in res.frames if f['t_us'] > t]
    if after:
        errors.append('%d headset frames after the reinit' % len(after))
    return errors


def kept_leak(text):
    """What a kept context's reinit leaks, headset or not, on master too:
    two unnamed buffers and two unnamed memory objects."""
    m = re.search(r'has 4 leaked objects that have not been destroyed\.\n'
                  r'(.*)', text)
    objects = m.group(1).rstrip('. ').split(', ') if m else []
    kinds = sorted(re.sub(r' 0x[0-9a-f]+$', '', o) for o in objects)
    return kinds == ['VkBuffer', 'VkBuffer', 'VkDeviceMemory', 'VkDeviceMemory']


SETTLE = [('wait', 8)]
VULKAN = {'video_views_test_hw': 'vulkan'}
TEARDOWN = [('wait', 6), ('send', 'FULLSCREEN_TOGGLE'), ('wait', 4),
            ('send', 'CLOSE_CONTENT'), ('wait', 4)]
KEPT_LEAK = [('VUID-vkDestroyDevice-device-05137', kept_leak)]
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
    {'name': 'recenter', 'map': '3ds',
     'steps': [('wait', 6), ('script', 'head 0.5 0.2 0.3 90'), ('wait', 1),
               ('send', 'HEADSET_RECENTER'), ('wait', 3)],
     'check': check_recenter},
    # The runtime's recenter after the hotkey's (space 2 is LOCAL).
    {'name': 'runtime-recenter', 'map': '3ds',
     'steps': [('wait', 6), ('script', 'head 0.5 0.2 0.3 90'), ('wait', 1),
               ('send', 'HEADSET_RECENTER'), ('wait', 2), ('mark', 'stage'),
               ('script', 'space 3'), ('wait', 2), ('mark', 'local'),
               ('script', 'space 2'), ('wait', 2)],
     'check': check_runtime_recenter},
    {'name': 'pacing', 'map': '3ds',
     'steps': [('wait', 6), ('send', 'PAUSE_TOGGLE'), ('mark', 'paused'),
               ('wait', 3), ('mark', 'resumed'), ('send', 'PAUSE_TOGGLE'),
               ('wait', 2)],
     'check': check_pacing},
    # The window loses its focus; the headset session keeps it, then is
    # only visible (4), then focused (5) again.
    {'name': 'focus', 'map': '3ds', 'unfocus': True,
     'settings': {'pause_nonactive': 'true'},
     'steps': [('wait', 6), ('unfocus', None), ('mark', 'unfocused'),
               ('wait', 3), ('mark', 'visible'), ('script', 'state 4'),
               ('wait', 3), ('mark', 'refocused'), ('script', 'state 5'),
               ('wait', 3), ('mark', 'end')],
     'check': check_focus},
    {'name': 'no-runtime', 'map': '3ds', 'no_runtime': True,
     'steps': [('wait', 6), ('shot', None)],
     'check': check_no_runtime},
    {'name': 'session-fails', 'map': '3ds', 'script': 'fail session\n',
     'steps': [('wait', 8), ('shot', None)],
     'check': check_session_fails},
    {'name': 'session-lost', 'map': '3ds',
     'steps': [('wait', 6), ('script', 'state 7'), ('mark', 'lost'),
               ('wait', 3), ('shot', None)],
     'check': check_session_lost},
    # A Vulkan core: reinit and unload while the headset runs.
    {'name': 'hw-teardown', 'map': 'none', 'options': VULKAN,
     'steps': TEARDOWN, 'check': check_hw_teardown},
    {'name': 'hw-teardown-threaded', 'map': 'none', 'options': VULKAN,
     'settings': {'video_threaded': 'true'}, 'steps': TEARDOWN,
     'check': threaded(check_hw_teardown)},
    {'name': 'kept-retry', 'map': 'none',
     'options': {'video_views_test_hw': 'vulkan_keep'},
     'steps': [('wait', 6), ('script', 'state 8'), ('wait', 2),
               ('mark', 'reinit'), ('send', 'FULLSCREEN_TOGGLE'),
               ('wait', 5)],
     'baseline': KEPT_LEAK, 'check': check_kept_retry},
    {'name': 'kept-lost', 'map': 'none',
     'options': {'video_views_test_hw': 'vulkan_keep'},
     'steps': [('wait', 6), ('script', 'fail instance'), ('wait', 2),
               ('mark', 'reinit'), ('send', 'FULLSCREEN_TOGGLE'),
               ('wait', 4)],
     'baseline': KEPT_LEAK, 'check': check_kept_lost},
    {'name': 'input-combined', 'map': '3ds',
     'steps': [('wait', 6), ('script', ALL_BUTTONS), ('wait', 2),
               ('script', STICKS), ('wait', 2), ('script', ''), ('wait', 2),
               ('script', 'action combined/recenter 1'), ('wait', 1),
               ('script', ''), ('wait', 1),
               ('script', 'action combined/menu 1'), ('wait', 1),
               ('script', ''), ('wait', 3)],
     'check': check_combined},
    {'name': 'input-separate', 'map': '3ds',
     'settings': {'video_openxr_controllers': '1'},
     'steps': [('wait', 6), ('script', SEPARATE), ('wait', 2),
               ('script', ''), ('wait', 2)],
     'check': check_separate},
    {'name': 'input-focus', 'map': '3ds',
     'steps': [('wait', 6), ('script', HELD), ('wait', 2),
               ('script', script(HELD, 'state 4')), ('wait', 2),
               ('script', script(HELD, 'state 5')), ('wait', 2),
               ('script', ''), ('wait', 1)],
     'check': check_input_focus},
    # Output spec section 3, Focus: the headset's focus keeps its
    # controllers and the content going while the window is unfocused;
    # only visible (4), they release and the content pauses.
    {'name': 'input-unfocused', 'map': '3ds', 'unfocus': True,
     'settings': {'pause_nonactive': 'true',
                  'input_joypad_background': 'false'},
     'steps': [('wait', 6), ('unfocus', None), ('mark', 'unfocused'),
               ('script', HELD), ('wait', 3), ('mark', 'visible'),
               ('script', script(HELD, 'state 4')), ('wait', 3),
               ('mark', 'refocused'), ('script', script(RETURNED, 'state 5')),
               ('wait', 3), ('mark', 'end'), ('script', ''), ('wait', 1)],
     'check': check_input_unfocused},
    # Forced, so the core's analog reads leave it on.
    {'name': 'input-dpad', 'map': '3ds',
     'settings': {'input_player1_analog_dpad_mode': '3'},
     'steps': [('wait', 6), ('script', DPAD), ('wait', 2), ('script', ''),
               ('wait', 2)],
     'check': check_input_dpad},
    {'name': 'input-touch', 'map': '3ds',
     'steps': [('wait', 6), ('script', AIM_BOTTOM), ('wait', 2),
               ('script', script(AIM_BOTTOM, R2)), ('wait', 2),
               ('script', AIM_BOTTOM), ('wait', 1),
               ('script', ''), ('wait', 2)],
     'check': check_touch},
    {'name': 'input-touch-threaded', 'map': '3ds',
     'settings': {'video_threaded': 'true'},
     'steps': [('wait', 6), ('script', AIM_BOTTOM), ('wait', 2),
               ('script', script(AIM_BOTTOM, R2)), ('wait', 2),
               ('script', AIM_BOTTOM), ('wait', 1),
               ('script', ''), ('wait', 2)],
     'check': check_touch},
    {'name': 'input-top-auto', 'map': '3ds',
     'steps': [('wait', 6), ('script', script(AIM_TOP, R2)), ('wait', 2),
               ('script', ''), ('wait', 1)],
     'check': check_top_auto},
    {'name': 'input-lightgun', 'map': '3ds',
     'settings': {'video_openxr_laser': '1'},
     'steps': [('wait', 6), ('script', script(AIM_GUN, R2)), ('wait', 2),
               ('script', script(AIM_MISS, R2)), ('wait', 2),
               ('script', script(AIM_MISS, R2, AIM_LEFT_MISS, L2)),
               ('wait', 2), ('script', ''), ('wait', 2)],
     'check': check_lightgun},
    {'name': 'input-two-hands', 'map': '3ds',
     'steps': [('wait', 6),
               ('script', script(AIM_LEFT_BOTTOM, AIM_RIGHT_BOTTOM)),
               ('wait', 1),
               ('script', script(AIM_LEFT_BOTTOM, AIM_RIGHT_BOTTOM, L2)),
               ('wait', 2),
               ('script', script(AIM_LEFT_BOTTOM, AIM_RIGHT_BOTTOM, L2, R2)),
               ('wait', 2),
               ('script', script(AIM_LEFT_BOTTOM, AIM_RIGHT_BOTTOM, R2)),
               ('wait', 1), ('script', ''), ('wait', 1)],
     'check': check_two_hands},
    # No views: one quad of the whole frame, where screen 0 would be.
    {'name': 'input-frame-always', 'map': 'none',
     'settings': {'video_openxr_laser': '1'},
     'steps': [('wait', 6), ('script', script(AIM_GUN, R2)), ('wait', 2),
               ('script', ''), ('wait', 2)],
     'check': check_frame_always},
    # The core runs behind the menu, though RetroArch blocks its input
    # there, and AIM_GUN meets the menu quad in front of the frame.
    {'name': 'input-single-auto', 'map': 'none',
     'settings': {'menu_driver': 'ozone', 'menu_pause_libretro': 'false'},
     'steps': [('wait', 6), ('script', script(AIM_GUN, R2)), ('wait', 2),
               ('script', ''), ('wait', 1), ('send', 'MENU_TOGGLE'),
               ('wait', 2), ('script', AIM_GUN), ('wait', 2),
               ('script', ''), ('send', 'MENU_TOGGLE'), ('wait', 2)],
     'check': check_single_auto},
    {'name': 'input-gun-tracking', 'map': '3ds',
     'settings': {'video_openxr_laser': '1'},
     'steps': [('wait', 6), ('script', script(AIM_GUN, R2)), ('wait', 2),
               ('script', script('aim right off', AIM_LEFT_MISS, L2)),
               ('wait', 2),
               ('script', script('aim right off', AIM_LEFT_MISS, R2)),
               ('wait', 2),
               ('script', script(AIM_GUN, AIM_LEFT_MISS)), ('wait', 2),
               ('script', ''), ('wait', 1)],
     'check': check_gun_tracking},
    {'name': 'input-menu', 'map': '3ds',
     'settings': {'menu_driver': 'ozone', 'frontend_log_level': '0'},
     'steps': [('wait', 6), ('send', 'MENU_TOGGLE'), ('wait', 3),
               ('script', AIM_MENU_HIGH), ('wait', 2),
               ('script', AIM_MENU_LOW), ('wait', 2),
               ('script', script(AIM_MENU_LOW, R2)), ('wait', 1),
               ('script', AIM_MENU_LOW), ('wait', 2)],
     'check': check_menu_laser},
    # Take Screenshot writes into the case's directory; a short press, well
    # inside a second, which would make it a long one.
    {'name': 'input-menu-rgui', 'map': '3ds',
     'settings': {'menu_driver': 'rgui', 'frontend_log_level': '0'},
     'steps': [('wait', 6), ('send', 'MENU_TOGGLE'), ('wait', 3),
               ('script', AIM_RGUI_SHOT), ('wait', 2),
               ('script', script(AIM_RGUI_SHOT, R2)), ('wait', 0.5),
               ('script', AIM_RGUI_SHOT), ('wait', 2)],
     'check': check_menu_rgui},
    {'name': 'input-menu-stick', 'map': '3ds',
     'settings': {'menu_driver': 'ozone', 'frontend_log_level': '0'},
     'steps': [('wait', 6), ('send', 'MENU_TOGGLE'), ('wait', 3),
               ('script', AIM_MENU_HIGH), ('wait', 2),
               ('script', script(AIM_MENU_HIGH, STICK_DOWN)), ('wait', 0.1),
               ('script', AIM_MENU_HIGH), ('wait', 1)] + SHAKE
     + [('wait', 1), ('script', AIM_MISS), ('wait', 1),
        ('script', AIM_MENU_BACK), ('wait', 1)],
     'check': check_menu_stick},
    {'name': 'input-menu-hands', 'map': '3ds',
     'settings': {'menu_driver': 'ozone', 'frontend_log_level': '0'},
     'steps': [('wait', 6), ('send', 'MENU_TOGGLE'), ('wait', 3),
               ('script', script(AIM_LEFT_MENU_TOP, AIM_MENU_TOP)),
               ('wait', 1),
               ('script', script(AIM_LEFT_MENU_TOP, AIM_MENU_TOP, L2)),
               ('wait', 1.5),
               ('script', script(AIM_LEFT_MENU_TOP, AIM_MENU_TOP, L2, R2)),
               ('wait', 1),
               ('script', script(AIM_LEFT_MENU_TOP, AIM_MENU_TOP, R2)),
               ('wait', 0.5),
               ('script', script(AIM_LEFT_MENU_TOP, AIM_MENU_TOP)),
               ('wait', 1)],
     'check': check_menu_hands},
    {'name': 'input-menu-click', 'map': '3ds',
     'settings': {'menu_driver': 'ozone', 'frontend_log_level': '0',
                  'confirm_reset': 'false'},
     'steps': [('wait', 6), ('send', 'MENU_TOGGLE'), ('wait', 3),
               ('script', AIM_MENU_HIGH), ('wait', 2),
               ('script', script(AIM_MENU_HIGH, R2)), ('wait', CLICK),
               ('script', AIM_MENU_HIGH), ('wait', 2)],
     'check': check_menu_click},
    {'name': 'input-menu-click-rgui', 'map': '3ds',
     'settings': {'menu_driver': 'rgui', 'frontend_log_level': '0',
                  'confirm_reset': 'false'},
     'steps': [('wait', 6), ('send', 'MENU_TOGGLE'), ('wait', 3)]
     + [('send', 'MENU_DOWN'), ('wait', 0.2)] * 4
     + [('script', AIM_RGUI_RESET), ('wait', 2),
        ('script', script(AIM_RGUI_RESET, R2)), ('wait', CLICK),
        ('script', AIM_RGUI_RESET), ('wait', 2)],
     'check': check_menu_click_rgui},
]


class XFocusChangeEvent(ctypes.Structure):
    _fields_ = [('type', ctypes.c_int), ('serial', ctypes.c_ulong),
                ('send_event', ctypes.c_int), ('display', ctypes.c_void_p),
                ('window', ctypes.c_ulong), ('mode', ctypes.c_int),
                ('detail', ctypes.c_int), ('pad', ctypes.c_long * 24)]


def unfocus_when(trigger):
    """Inside a case's gamescope: once trigger exists, tell the window with
    the X input focus that it lost it. gamescope gives the real focus
    straight back to its one window, so the event is sent, not caused."""
    if os.environ.get('DISPLAY', '') in ('', ':0'):
        return 99
    deadline = time.time() + 60
    while not os.path.exists(trigger):
        if time.time() > deadline:
            return 1
        time.sleep(0.1)
    x11 = ctypes.CDLL('libX11.so.6')
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XGetInputFocus.argtypes = [ctypes.c_void_p,
                                   ctypes.POINTER(ctypes.c_ulong),
                                   ctypes.POINTER(ctypes.c_int)]
    x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int,
                               ctypes.c_long, ctypes.c_void_p]
    x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    dpy = x11.XOpenDisplay(None)
    if not dpy:
        return 1
    win, revert = ctypes.c_ulong(0), ctypes.c_int(0)
    x11.XGetInputFocus(dpy, ctypes.byref(win), ctypes.byref(revert))
    ev = XFocusChangeEvent()
    ev.type, ev.window = 10, win.value  # FocusOut
    ev.mode, ev.detail = 0, 3           # NotifyNormal, NotifyNonlinear
    sent = x11.XSendEvent(dpy, win.value, 0, 1 << 21, ctypes.byref(ev))
    x11.XSync(dpy, 0)
    x11.XCloseDisplay(dpy)
    if win.value <= 1 or not sent:  # None or PointerRoot
        return 1
    print('[e2e] FocusOut sent to window 0x%x.' % win.value, flush=True)
    return 0


def main():
    if sys.argv[1:2] == ['--unfocus-when']:
        return unfocus_when(sys.argv[2])
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
    if validate:
        baseline = read_baseline(os.path.join(
            os.path.dirname(os.path.abspath(args[2])),
            monado.get('VALIDATION_BASELINE', os.path.join(
                HERE, 'validation-baseline.txt'))))
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
            extra, known = unexpected(res.log,
                                      baseline + case.get('baseline', []))
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
