#!/usr/bin/env python3
"""End-to-end check of RetroArch's view presentation.

Runs the video_views test core in headless gamescope for each case,
takes a GPU screenshot over the network command port, checks the
colour where each view should land, and checks the views status the
core logged. The gl driver doesn't present views, so it runs one case
that expects the packed frame and no PRESENTS. Not run in CI: it needs
a GPU, gamescope and a RetroArch build.

Usage: run.py <retroarch binary> <output dir> <driver> [driver ...]
"""

import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.normpath(os.path.join(HERE, '..', 'video_views_libretro.so'))
W, H = 1600, 960
PORT = 55355
TOLERANCE = 8

RED, BLUE, GREEN = (255, 0, 0), (0, 0, 255), (0, 255, 0)
YELLOW, WHITE = (255, 255, 0), (255, 255, 255)
BLACK, GREY = (0, 0, 0), (32, 32, 32)

# name, map option, settings, [(x, y, rgb)] in a 1600x960 window
CASES = [
    ('3ds-2d', '3ds', {'video_stereo_mode': '0'},
     [(800, 240, GREEN), (800, 720, YELLOW), (200, 240, BLACK),
      (404, 4, WHITE)]),
    ('3ds-force-2d', '3ds_force', {'video_stereo_mode': '0'},
     [(800, 240, RED), (800, 720, YELLOW)]),
    ('3ds-sbs-full', '3ds', {'video_stereo_mode': '2'},
     [(400, 240, RED), (1200, 240, BLUE), (400, 720, YELLOW),
      (1200, 720, YELLOW), (20, 720, BLACK), (4, 4, WHITE),
      (804, 4, WHITE)]),
    ('3ds-sbs-full-threaded', '3ds',
     {'video_stereo_mode': '2', 'video_threaded': 'true'},
     [(400, 240, RED), (1200, 240, BLUE), (400, 720, YELLOW),
      (1200, 720, YELLOW)]),
    ('3ds-sbs-full-swap', '3ds',
     {'video_stereo_mode': '2', 'video_stereo_swap_eyes': 'true'},
     [(400, 240, BLUE), (1200, 240, RED)]),
    ('3ds-sbs-half', '3ds', {'video_stereo_mode': '1'},
     [(400, 240, RED), (1200, 240, BLUE), (400, 720, YELLOW),
      (1200, 720, YELLOW), (100, 240, BLACK)]),
    ('3ds-top-bottom', '3ds', {'video_stereo_mode': '3'},
     [(800, 120, RED), (800, 360, YELLOW), (800, 600, BLUE),
      (800, 840, YELLOW)]),
    ('3ds-anaglyph', '3ds', {'video_stereo_mode': '4'},
     [(800, 240, (116, 0, 255)), (800, 720, (210, 255, 0))]),
    ('3ds-interlaced', '3ds', {'video_stereo_mode': '5'},
     [(800, 240, RED), (800, 241, BLUE), (800, 720, YELLOW)]),
    ('3ds-interlaced-swap', '3ds',
     {'video_stereo_mode': '5', 'video_stereo_swap_eyes': 'true'},
     [(800, 240, BLUE), (800, 241, RED)]),
    ('ds-horizontal', 'ds',
     {'video_stereo_mode': '0', 'video_screen_layout': '1'},
     [(400, 480, GREEN), (1200, 480, YELLOW), (400, 100, BLACK)]),
    ('vb-sbs-full', 'vb', {'video_stereo_mode': '2'},
     [(400, 480, RED), (1200, 480, BLUE)]),
    ('no-map', 'none', {'video_stereo_mode': '2'},
     [(400, 240, RED), (1200, 240, BLUE), (800, 720, YELLOW),
      (100, 720, GREY)]),
    ('invalid-map', 'invalid', {'video_stereo_mode': '2'},
     [(400, 240, RED), (1200, 240, GREY)]),
]

# The same cases drawn through a GL core context, which only glcore
# presents, with each origin: (case, video_views_test_hw).
HW_CASES = [(name, hw) for hw in ('gl', 'gl_topleft')
            for name in ('3ds-2d', '3ds-sbs-full', '3ds-anaglyph',
                         '3ds-interlaced')]

# The gl driver doesn't present views: the map is accepted, STEREO is off,
# and the core's 2D packing shows whole.
GL_CASES = [
    ('3ds-sbs-full-gl', '3ds', {'video_stereo_mode': '2'},
     [(400, 240, GREEN), (1200, 240, GREY), (800, 720, YELLOW),
      (100, 720, GREY)]),
]

STATUS_RE = re.compile(
    r'\[video_views\] presents=(\d) stereo=(\d) accepted=(\d)')


def read_png(path):
    """8-bit RGB or RGBA, non-interlaced: what rpng writes."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat = 8, b''
    w = h = ct = bd = None
    while pos < len(data):
        ln, = struct.unpack('>I', data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b'IHDR':
            w, h, bd, ct = struct.unpack('>IIBB', body[:10])
        elif typ == b'IDAT':
            idat += body
        elif typ == b'IEND':
            break
    if bd != 8 or ct not in (2, 6):
        raise ValueError('unsupported PNG: depth %s type %s' % (bd, ct))
    bpp = 3 if ct == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows


def send(cmd):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(cmd.encode(), ('127.0.0.1', PORT))
    s.close()


def write_cfg(path, d, driver, settings):
    cfg = {
        'video_driver': driver,
        # vk_wayland segfaults on teardown in headless gamescope; stay on X.
        'video_context_driver': 'vk_x' if driver == 'vulkan' else 'x',
        'input_driver': 'x',
        # Keep real controllers (udev) and achievements out of the run.
        'input_joypad_driver': 'null',
        'input_autodetect_enable': 'false',
        'cheevos_enable': 'false',
        'video_fullscreen': 'true',
        'video_windowed_fullscreen': 'true',
        'video_gpu_screenshot': 'true',
        'video_font_enable': 'false',
        'menu_enable_widgets': 'false',
        'video_stereo_mode': '0',
        'video_stereo_swap_eyes': 'false',
        'video_screen_layout': '0',
        'video_scale_integer': 'false',
        'aspect_ratio_index': '22',
        'audio_enable': 'false',
        'audio_driver': 'null',
        'pause_nonactive': 'false',
        'confirm_quit': 'false',
        'quit_press_twice': 'false',
        'config_save_on_exit': 'false',
        'network_cmd_enable': 'true',
        'network_cmd_port': str(PORT),
        'history_list_enable': 'false',
        'savestate_thumbnail_enable': 'false',
        'global_core_options': 'true',
        'core_options_path': os.path.join(d, 'opts.cfg'),
        'screenshot_directory': os.path.join(d, 'shots'),
        'savefile_directory': d,
        'savestate_directory': d,
        'system_directory': d,
        'cache_directory': d,
        'log_dir': d,
        'log_to_file': 'true',
        'libretro_log_level': '0',
    }
    cfg.update(settings)
    with open(path, 'w') as f:
        for k in sorted(cfg):
            f.write('%s = "%s"\n' % (k, cfg[k]))


def expected_status(driver, settings):
    """(presents, stereo) the frontend should report for these settings."""
    if driver == 'gl':
        return (0, 0)
    return (1, 0 if settings.get('video_stereo_mode', '0') == '0' else 1)


def status_errors(d, driver, settings):
    """Check the last views status the core logged."""
    found = None
    for name in ('run.log', 'retroarch.log'):
        path = os.path.join(d, name)
        if os.path.exists(path):
            with open(path, errors='replace') as f:
                for m in STATUS_RE.finditer(f.read()):
                    found = tuple(int(g) for g in m.groups())
    if found is None:
        return ['no "[video_views] presents=" line in the logs']
    want = expected_status(driver, settings)
    errors = []
    if found[:2] != want:
        errors.append('status presents=%d stereo=%d, want presents=%d '
                      'stereo=%d' % (found[0], found[1], want[0], want[1]))
    if driver == 'gl' and found[2] != 1:
        errors.append('map not accepted on gl')
    return errors


def run_case(retroarch, root, driver, case, hw='off'):
    name, mapopt, settings, points = case
    if hw != 'off':
        name += '-hw-' + hw
    d = os.path.realpath(os.path.join(root, driver + '-' + name))
    if os.path.commonpath([root, d]) != root or d == root:
        raise RuntimeError('refusing to touch ' + d)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, 'shots'))
    cfg = os.path.join(d, 'retroarch.cfg')
    write_cfg(cfg, d, driver, settings)
    with open(os.path.join(d, 'opts.cfg'), 'w') as f:
        f.write('video_views_test_map = "%s"\n' % mapopt)
        f.write('video_views_test_hw = "%s"\n' % hw)

    # Never reach the desktop: refuse the real display, and point
    # RetroArch's Wayland at a socket that does not exist (unset, libwayland
    # falls back to wayland-0). gamescope itself keeps the caller's env.
    guard = ('case "$DISPLAY" in ""|:0) echo "refusing DISPLAY=$DISPLAY" >&2;'
             ' exit 99;; esac;'
             ' WAYLAND_DISPLAY=video-views-e2e-no-socket; export WAYLAND_DISPLAY;'
             ' exec "$0" "$@"')
    cmd = ['gamescope', '--backend', 'headless',
           '-w', str(W), '-h', str(H), '-W', str(W), '-H', str(H),
           '-r', '60', '--',
           'sh', '-c', guard,
           retroarch, '--config', cfg, '-L', CORE, '--verbose']
    log = open(os.path.join(d, 'run.log'), 'w')
    p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                         start_new_session=True)
    shot = None
    try:
        time.sleep(5)
        send('SCREENSHOT')
        deadline = time.time() + 10
        while time.time() < deadline and not shot:
            time.sleep(0.5)
            pngs = [x for x in os.listdir(os.path.join(d, 'shots'))
                    if x.endswith('.png')]
            if pngs:
                time.sleep(1)
                shot = os.path.join(d, 'shots', pngs[0])
        send('QUIT')
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            pass
    finally:
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGTERM)
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
        log.close()

    errors = status_errors(d, driver, settings)
    if not shot:
        return errors + ['no screenshot (see %s)'
                         % os.path.join(d, 'run.log')]
    w, h, bpp, rows = read_png(shot)
    if (w, h) != (W, H):
        return errors + ['screenshot is %dx%d, want %dx%d' % (w, h, W, H)]
    for x, y, want in points:
        got = tuple(rows[y][x * bpp:x * bpp + 3])
        if any(abs(g - e) > TOLERANCE for g, e in zip(got, want)):
            errors.append('(%d,%d) got %s want %s' % (x, y, got, want))
    return errors


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    retroarch = os.path.abspath(sys.argv[1])
    root = os.path.realpath(sys.argv[2])
    os.makedirs(root, exist_ok=True)
    failed = 0
    for driver in sys.argv[3:]:
        if driver == 'gl':
            runs = [(case, 'off') for case in GL_CASES]
        else:
            runs = [(case, 'off') for case in CASES]
        if driver == 'glcore':
            runs += [(case, hw) for name, hw in HW_CASES
                     for case in CASES if case[0] == name]
        for case, hw in runs:
            errors = run_case(retroarch, root, driver, case, hw)
            print('%s %s/%s%s' % ('FAIL' if errors else 'pass', driver,
                                  case[0], '' if hw == 'off' else ' (hw ' + hw + ')'))
            for e in errors:
                print('    ' + e)
            failed += bool(errors)
    print('%d failed' % failed)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
