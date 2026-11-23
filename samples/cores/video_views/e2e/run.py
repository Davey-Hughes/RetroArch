#!/usr/bin/env python3
"""End-to-end check of RetroArch's view presentation.

Runs the video_views test core in headless gamescope for each case,
takes a GPU screenshot over the network command port (gamescope's own
with the menu open), checks the colour where each view should land,
and checks the views status the core logged; a case is shot once that
status is final, not after a fixed wait. One case also bounds the GPU
memory RetroArch holds, and the pointer cases move the pointer of
gamescope's own X server and look for the menu's cursor under it. The
exact cases check every pixel of
views filled with noise, drawn straight from the frame by the stock
chain and for a preset copied out of it, on glcore once for each way it
copies them, or a software frame's written straight in; and, on glcore,
a frame drawn whole out of a core's larger framebuffer.
The screens cases check what the frontend makes of the screens the
core describes: which maps it accepts and the log of the screens it
keeps, a light gun kept off a screen that takes no input, which
screens it lays out, leaves out or draws black by the core's show and
state, the user's setting and the core's own file of choices, and the
pictures of image screens on the drivers that draw them.
In full it needs a GPU, gamescope and a RetroArch build.

With RA_E2E_XVFB=1 each case runs on an Xvfb server of its own instead
of gamescope, with Mesa's software renderers where there is no GPU:
the screen is read with ImageMagick's import and the pointer moved on
that server, several cases at once (RA_E2E_JOBS, 4 by default). The
GPU memory cases are left out there. Xvfb has no XVideo adaptor: for
the xvideo driver RA_E2E_XV_STANDIN names samples/gfx/xv_standin's
library, preloaded to be one. RA_E2E_ONLY, a
regular expression, runs only the cases whose names it finds; CI runs a
set of them that way (Linux-vulkan-validation.yml).

With RA_E2E_WINE=1 the binary is a retroarch.exe run under Wine, on
Xvfb as above, each worker in a copy of WINEPREFIX of its own, with
the core built by make DLL=1 and paths given as
Wine's Z: drive has them; a D3DCompiler_47.dll next to the binary is
taken over Wine's own (Windows-wine-views.yml). The cases that need a
preset run only on drivers whose views take one.

With RA_E2E_HDR=1 the vulkan cases have HDR10 output, HDR_CASES and
HDR_MENU_CASES with them: in gamescope with its HDR on, or on an Xvfb
of depth 30 through samples/gfx/vulkan_hdr_ui_pass's fake HDR10 layer.
The 16-bit PQ screenshots are decoded back to the SDR colours the
points name; on Xvfb a screen window's pixels are read as the 10-bit
codes its swapchain holds. A case fails on any validation error or an
HDR pass it should not take.

Usage: run.py <retroarch binary> <output dir> <driver> [driver ...]
"""

import collections
import ctypes
import importlib.util
import itertools
import os
import pathlib
import random
import re
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
WINE = os.environ.get('RA_E2E_WINE') == '1'
CORE = os.path.normpath(os.path.join(
    HERE, '..', 'video_views_libretro.' + ('dll' if WINE else 'so')))
W, H = 1600, 960
TOLERANCE = 8
# A 16-bit PQ screenshot decoded back to SDR: the 10-bit swapchain's
# rounding lifts a saturated colour's zero channel by up to 10.
HDR_TOLERANCE = 16
# A screen window's 10-bit codes against its colour's PQ codes: an SDR
# copy of the colour is hundreds of codes away.
WINDOW_TOLERANCE = 8
# How long a case runs on once its views status is final: the frames
# that take up the layout, with threaded video's one behind. Wine's
# software D3D draws a frame in far longer.
SETTLE = 2.0 if WINE else 0.5

RED, BLUE, GREEN = (255, 0, 0), (0, 0, 255), (0, 255, 0)
YELLOW, WHITE = (255, 255, 0), (255, 255, 255)
BLACK, GREY = (0, 0, 0), (32, 32, 32)

# Aspect Ratio: Custom.
CUSTOM_VP = {'aspect_ratio_index': '23'}

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
    # A custom viewport larger than the window is cropped, not squeezed:
    # the 8-pixel marker is 66 wide, and an origin left of the window or
    # of an eye's half shows the picture's right part.
    ('vb-custom-crop', 'vb',
     dict(CUSTOM_VP, video_stereo_mode='0', custom_viewport_x='0',
          custom_viewport_y='0', custom_viewport_width='3200',
          custom_viewport_height='1920'),
     [(50, 50, WHITE), (100, 100, GREEN), (1500, 900, GREEN)]),
    ('vb-custom-offset', 'vb',
     dict(CUSTOM_VP, video_stereo_mode='0', custom_viewport_x='-1600',
          custom_viewport_y='-960', custom_viewport_width='3200',
          custom_viewport_height='1920'),
     [(50, 50, GREEN), (800, 480, GREEN)]),
    ('vb-custom-sbs-offset', 'vb',
     dict(CUSTOM_VP, video_stereo_mode='1', custom_viewport_x='-1600',
          custom_viewport_y='0', custom_viewport_width='3200',
          custom_viewport_height='960'),
     [(400, 480, RED), (1200, 480, BLUE), (50, 50, RED)]),
    ('no-map', 'none', {'video_stereo_mode': '2'},
     [(400, 240, RED), (1200, 240, BLUE), (800, 720, YELLOW),
      (100, 720, GREY)]),
    ('invalid-map', 'invalid', {'video_stereo_mode': '2'},
     [(400, 240, RED), (1200, 240, GREY)]),
    # Threaded video crops this frame above its second screen, so the map
    # no longer fits and the cropped frame shows whole, not laid out.
    ('crop-threaded', 'crop',
     {'video_stereo_mode': '0', 'video_screen_layout': '1',
      'video_threaded': 'true'},
     [(800, 100, GREEN), (800, 900, YELLOW), (1500, 480, GREEN)]),
]

# The same cases drawn in hardware: through a GL compatibility context on
# gl, a core context with each origin on glcore, and into the core's own
# images on vulkan.
# {driver: [(case, video_views_test_hw)]}
HW_CASES = {
    'gl': [(name, 'gl_compat')
           for name in ('3ds-2d', '3ds-sbs-full', '3ds-anaglyph',
                        '3ds-interlaced')],
    'glcore': [(name, hw) for hw in ('gl', 'gl_topleft')
               for name in ('3ds-2d', '3ds-sbs-full', '3ds-anaglyph',
                            '3ds-interlaced')],
    'vulkan': [(name, 'vulkan')
               for name in ('3ds-2d', '3ds-sbs-full', '3ds-anaglyph',
                            '3ds-interlaced', '3ds-sbs-full-threaded')],
    'd3d11': [(name, 'd3d11')
              for name in ('3ds-2d', '3ds-sbs-full', '3ds-anaglyph',
                           '3ds-interlaced', '3ds-sbs-full-threaded')],
    'd3d12': [(name, 'd3d12')
              for name in ('3ds-2d', '3ds-sbs-full', '3ds-anaglyph',
                           '3ds-interlaced', '3ds-sbs-full-threaded')],
    # The core's own Direct3D 9 textures, its views drawn straight from
    # them
    'd3d9_hlsl': [(name, 'd3d9')
                  for name in ('3ds-2d', '3ds-sbs-full', '3ds-anaglyph',
                               '3ds-interlaced', '3ds-sbs-full-threaded')],
}

# A frame past the declared maximum in the core's own Vulkan image.
# Nothing is copied out of it, so threaded video does not crop it, and
# it is laid out as any other.
VULKAN_CROP_CASES = [
    ('crop', 'crop', {'video_stereo_mode': '0', 'video_screen_layout': '1'},
     [(400, 480, GREEN), (1200, 480, YELLOW), (400, 100, BLACK)]),
    ('crop-threaded', 'crop',
     {'video_stereo_mode': '0', 'video_screen_layout': '1',
      'video_threaded': 'true'},
     [(400, 480, GREEN), (1200, 480, YELLOW), (400, 100, BLACK)]),
]

# A frame a driver can't lay out is drawn whole, and the menu then lays
# out as for a frame without views. Threaded video crops every crop-map
# frame, so the menu in each stereo mode must match the first mode's.
# gamescope captures it: a GPU screenshot leaves the menu out.
# [(name, map option, settings, stereo modes)]
MENU_CASES = [
    ('crop-threaded-menu', 'crop',
     {'video_screen_layout': '1', 'video_threaded': 'true',
      'menu_driver': 'ozone', 'menu_timedate_enable': 'false'},
     ('0', '2')),
]
# Pixels allowed to differ from the first mode's menu: the cursor's
# border pulses. A menu laid out for one eye differs in over 200000.
MENU_DIFF_MAX = 20000
# How far the menu's brightness, the 99th percentile of max(r, g, b)
# over the window in raw codes, may move from the first mode's.
MENU_BRIGHTNESS_MAX = 2

# RGUI is drawn in the picture's rectangle, not the whole window: a 4:3
# picture on the 5:3 screen leaves 160-pixel black strips either side.
# (name, map option, settings, points)
MENU_FIT_CASE = ('no-map-rgui-fit', 'none',
                 {'aspect_ratio_index': '0', 'menu_driver': 'rgui',
                  'rgui_menu_color_theme': '4', 'rgui_particle_effect': '0',
                  'menu_timedate_enable': 'false'}, [])
MENU_FIT_DRIVERS = ('d3d10', 'd3d11', 'd3d12')
MENU_FIT_STRIP = 150

# The menu draws its own cursor where it has the pointer: with a per-eye
# UI, at the pointer in the copy it is over and at the matching place in
# the other. Each case holds the pointer at two window points on one menu
# row, the second in the second eye's area, so only the cursor differs
# between the two screens. The ds map is not the window's shape, so
# there RGUI, fitted to the core's aspect, does not fill the window.
# [(name, map option, settings, eye areas (x, y, w, h), points)]
POINTER_CASES = [
    ('pointer-2d', '3ds', {'video_stereo_mode': '0'},
     [(0, 0, 1600, 960)], [(900, 420), (1050, 420)]),
    ('pointer-sbs-half', '3ds', {'video_stereo_mode': '1'},
     [(0, 0, 800, 960), (800, 0, 800, 960)], [(450, 420), (1325, 420)]),
    ('pointer-sbs-full', '3ds', {'video_stereo_mode': '2'},
     [(0, 0, 800, 960), (800, 0, 800, 960)], [(450, 420), (1400, 420)]),
    ('pointer-top-bottom', '3ds', {'video_stereo_mode': '3'},
     [(0, 0, 1600, 480), (0, 480, 1600, 480)], [(900, 210), (1050, 690)]),
    ('pointer-anaglyph', '3ds', {'video_stereo_mode': '4'},
     [(0, 0, 1600, 960)], [(900, 420), (1050, 420)]),
    ('pointer-ds-2d', 'ds', {'video_stereo_mode': '0'},
     [(0, 0, 1600, 960)], [(960, 420), (1090, 420)]),
    ('pointer-ds-anaglyph', 'ds', {'video_stereo_mode': '4'},
     [(0, 0, 1600, 960)], [(960, 420), (1090, 420)]),
]
# Themes with a white cursor, which is how the check tells it apart.
POINTER_MENUS = {
    'ozone': {'menu_driver': 'ozone',
              'ozone_menu_color_theme': 'basic_black'},
    'rgui': {'menu_driver': 'rgui', 'rgui_menu_color_theme': '4',
             'rgui_particle_effect': '0'},
}
POINTER_SETTINGS = {'menu_mouse_enable': 'true',
                    'menu_pointer_enable': 'false',
                    'menu_timedate_enable': 'false'}
POINTER_PARK = (40, 900)
# How far the drawn cursor may sit from the pointer, and how much of the
# screen around the pointer counts as that cursor.
POINTER_TOLERANCE = 6
POINTER_REACH = 48

# The core declares Azahar's 8000x4800 maximum, draws its usual frame in
# hardware as Azahar does, and runs a preset keeping eight frames of
# history. Sized for that maximum, the history is 1.75 GiB a chain: the
# limit leaves room for the main chain's, not for a view's.
# [(name, map option, settings, points, GPU memory limit in MiB)]
HISTORY_PRESET = os.path.join(HERE, 'history.slangp')
MEMORY_CASES = [
    ('3ds-sbs-full-history', '3ds',
     {'video_stereo_mode': '2', 'video_shader_enable': 'true'},
     [(400, 240, RED), (1200, 240, BLUE), (400, 720, YELLOW),
      (1200, 720, YELLOW), (4, 4, WHITE), (804, 4, WHITE)],
     3072),
]
MEMORY_HW = {'glcore': 'gl', 'vulkan': 'vulkan'}

# Views arrive pixel for pixel: the core fills each view with noise,
# drawn unfiltered at an integer scale, and every pixel of each view on
# screen must be its source pixel. The stock chain's views are drawn
# straight from the frame; each case runs again with a passthrough
# preset, whose views are copied out of it, and on glcore once more with
# Mesa hiding glCopyImageSubData, so the views are blitted instead. The
# screenshots must match.
# [(name, map option, core options, hw)]
EXACT_SETTINGS = {'video_stereo_mode': '0', 'video_scale_integer': 'true',
                  'video_smooth': 'false'}
NOISE = {'video_views_test_pattern': 'noise'}
NOISE_565 = dict(NOISE, video_views_test_format='rgb565')
NOISE_LARGE = dict(NOISE, video_views_test_max='large')
EXACT_CASES = [
    ('exact-ds', 'ds', NOISE, 'off'),
    ('exact-ds-rgb565', 'ds', NOISE_565, 'off'),
    # The second screen starts 240 columns in.
    ('exact-3ds-rgb565', '3ds', NOISE_565, 'off'),
    # Rows counted from the frame's bottom, and from its top.
    ('exact-3ds', '3ds', NOISE, 'gl'),
    ('exact-3ds', '3ds', NOISE, 'gl_topleft'),
    ('exact-3ds', '3ds', NOISE, 'gl_compat'),
    ('exact-3ds', '3ds', NOISE, 'vulkan'),
    ('exact-3ds', '3ds', NOISE, 'd3d11'),
    ('exact-3ds', '3ds', NOISE, 'd3d12'),
    ('exact-3ds', '3ds', NOISE, 'd3d9'),
    # No map: the frame drawn whole. A core's GL framebuffer as large as
    # its maximum holds the frame in a corner, so the stock chain would
    # copy it out; glcore draws it straight from there instead.
    ('whole-3ds', 'none', NOISE_LARGE, 'gl'),
    ('whole-3ds', 'none', NOISE_LARGE, 'gl_topleft'),
]
EXACT_HW = {'gl': ('off', 'gl_compat'),
            'glcore': ('off', 'gl', 'gl_topleft'), 'vulkan': ('off', 'vulkan'),
            'gl1': ('off',), 'd3d10': ('off',), 'd3d11': ('off', 'd3d11'),
            'd3d12': ('off', 'd3d12'), 'd3d9_hlsl': ('off', 'd3d9'),
            'sdl2': ('off',), 'sdl3': ('off',), 'd3d8': ('off',),
            'gdi': ('off',), 'sdl': ('off',), 'x11': ('off',)}
# Drivers whose views take a preset: on the others a preset draws the
# frame whole, so the cases that need one are left out there.
PRESET_DRIVERS = ('glcore', 'vulkan', 'd3d10', 'd3d11', 'd3d12')
# gl takes GLSL presets: views through two passes, the first into a
# framebuffer at the view's size or its placement's, and through one,
# each with earlier frames and feedback of its own.
GL_PRESETS = [('-preset', os.path.join(HERE, 'pass.glslp')),
              ('-preset-viewport', os.path.join(HERE, 'pass_viewport.glslp')),
              ('-preset-one', os.path.join(HERE, 'pass_one.glslp')),
              # Each view's own earlier frames and feedback
              ('-preset-prev', os.path.join(HERE, 'prev.glslp')),
              ('-preset-prev-one', os.path.join(HERE, 'prev_one.glslp')),
              ('-preset-feedback', os.path.join(HERE, 'feedback.glslp'))]
# d3d9_hlsl takes Cg presets the same, but for feedback, which it has not.
D3D9_PRESETS = [('-preset', os.path.join(HERE, 'pass.cgp')),
                ('-preset-viewport', os.path.join(HERE, 'pass_viewport.cgp')),
                ('-preset-one', os.path.join(HERE, 'pass_one.cgp')),
                ('-preset-prev', os.path.join(HERE, 'prev.cgp')),
                ('-preset-prev-one', os.path.join(HERE, 'prev_one.cgp'))]
# {driver: its presets of its own shader language}
DRIVER_PRESETS = {'gl': GL_PRESETS, 'd3d9_hlsl': D3D9_PRESETS}
GL_PRESET_CASES = ('3ds-sbs-full', '3ds-top-bottom', '3ds-anaglyph',
                   '3ds-interlaced', 'ds-horizontal')
# Drivers whose views' presets have each view of a software frame written
# straight out of it into a texture of its own, not read in place. Not
# vulkan's, which does from a staging buffer but copies from a linear
# image, as for lavapipe's XRGB8888 frames.
WRITE_SW_VIEWS = ('gl', 'glcore', 'd3d9_hlsl', 'd3d10', 'd3d11', 'd3d12')
# Drivers whose views' presets read a software frame's views where they
# lie when they can.
SW_IN_PLACE_DRIVERS = ('glcore', 'vulkan')
WRITE_SW_RE = re.compile(
    r'\] Views\' presets have each view of a software frame written '
    r'straight in\.')
# d3d9_hlsl's presets on a core's Direct3D 9 texture: each view copied
# out of it on the GPU
D3D9_HW_COPY_RE = re.compile(
    r'\] Views\' presets have each view of a hardware frame copied out '
    r'of the core\'s texture\.')
# Drivers on shader model 4, where the rect presets' gathers of more than
# the red component do not compile.
SM4_DRIVERS = ('d3d10',)
# Drivers that draw anaglyph's frames whole: no blend of theirs makes its
# colours, which mix the channels.
WHOLE_ANAGLYPH_DRIVERS = ('d3d8', 'sdl2', 'sdl3')
# Drivers left out of the pointer cases: on Xvfb SDL 1.2's fullscreen
# window (SDL2's sdl12-compat) reports no pointer motion, even with a
# window manager.
NO_POINTER_DRIVERS = ('sdl',)
# Filtered, a view drawn straight from the frame must stop at its edge
# as its copy does: the ds screens touch in the frame, so a bottom row
# blended with the other screen's top shows. Compared, not checked
# pixel for pixel. [(name, map option, core options, hw)]
SMOOTH_CASES = [
    ('smooth-ds', 'ds', NOISE, 'off'),
    ('smooth-3ds', '3ds', NOISE, 'gl'),
    ('smooth-3ds', '3ds', NOISE, 'vulkan'),
    ('smooth-3ds', '3ds', NOISE, 'd3d11'),
    ('smooth-3ds', '3ds', NOISE, 'd3d12'),
    ('smooth-whole-3ds', 'none', NOISE_LARGE, 'gl'),
]
SMOOTH_SETTINGS = dict(EXACT_SETTINGS, video_smooth='true')
LINEAR_PRESET = os.path.join(HERE, 'history_linear.slangp')
# Drivers that draw the stock chain's views straight from the frame,
# and that draw a frame whole straight from a larger texture.
DIRECT_DRIVERS = ('gl1', 'gl', 'glcore', 'vulkan', 'd3d9_hlsl', 'd3d10',
                  'd3d11', 'd3d12', 'sdl2', 'sdl3', 'd3d8', 'gdi', 'sdl',
                  'x11', 'xvideo')
DIRECT_RE = re.compile(r'\] Views drawn straight from the frame\.')
PRESET_RE = re.compile(r'\] Views drawn through the preset')
WHOLE_DRIVERS = ('glcore',)
WHOLE_RE = re.compile(r'\] Frames drawn straight from the core\'s texture\.')
# The core's 2D maps, as (x, y, width, height) in the packed frame.
EXACT_VIEWS = {'ds': [(0, 0, 256, 192), (0, 192, 256, 192)],
               '3ds': [(0, 0, 400, 240), (240, 240, 320, 240)]}
# Without a map the core still draws its 2D 3ds screens.
EXACT_VIEWS['none'] = EXACT_VIEWS['3ds']
NOISE_COLOURS = [BLACK, RED, GREEN, BLUE, (0, 255, 255), (255, 0, 255),
                 YELLOW]
# A preset reads a frame inside a larger texture where it lies. Each
# preset runs on the core's frame drawn in hardware into a framebuffer
# as large as its maximum, and again on its software frame, which comes
# on its own: the screenshots must match, but for linear filtering with
# border wrap, whose blend of the edge texels with the border is the
# shader's and may round a step from the sampler's.
# [(preset, read in place, tolerance)]
RECT_PRESETS = [('rect_nearest.slangp', True, 0),
                ('rect_nearest_edge.slangp', True, 0),
                ('rect_nearest_repeat.slangp', True, 0),
                ('rect_nearest_mirror.slangp', True, 0),
                ('rect_linear.slangp', True, 0),
                ('rect_linear_border.slangp', True, 1),
                ('rect_hist_linear_repeat.slangp', False, 0),
                ('rect_full.slangp', True, 0),
                ('rect_two.slangp', True, 0)]
RECT_HW = {'glcore': ('gl', 'gl_topleft')}
IN_PLACE_RE = re.compile(
    r'\[GLCore\] Preset reads frames where the core leaves them\.')
# A view's preset reads the view where it lies in the frame. Each runs
# a rect preset, read in place, and its rect_hist twin, which reads a
# frame of history too and so has the views copied out: the screenshots
# must match, linear with border to within a step as above.
# [(name, map option, core options, hw)]
VIEW_RECT_CASES = [('view-rect-ds', 'ds', NOISE, 'off'),
                   ('view-rect-ds-rgb565', 'ds', NOISE_565, 'off'),
                   ('view-rect-3ds', '3ds', NOISE, 'gl'),
                   ('view-rect-3ds', '3ds', NOISE, 'gl_topleft'),
                   ('view-rect-3ds', '3ds', NOISE, 'vulkan'),
                   ('view-rect-3ds', '3ds', NOISE, 'd3d11'),
                   ('view-rect-3ds', '3ds', NOISE, 'd3d12')]
VIEW_RECT_PRESETS = (('nearest', 0), ('nearest_repeat', 0), ('linear', 0),
                     ('linear_border', 1))
VIEW_IN_PLACE_RE = re.compile(
    r'\] Views\' presets read the frame where it lies\.')

# Mesa without GL 4.3 and ARB_copy_image: glcore must blit.
NO_COPY_IMAGE = {'MESA_GL_VERSION_OVERRIDE': '4.2',
                 'MESA_EXTENSION_OVERRIDE': '-GL_ARB_copy_image'}
COPY_RE = re.compile(r'\[GLCore\] Views copied by ([\w ]+)\.')

STATUS_RE = re.compile(
    r'\[video_views\] presents=(\d) stereo=(\d) accepted=(\d) views=\d+ '
    r'screens=(\d) images=(\d)')

# RA_E2E_HDR=1: the vulkan cases with HDR10 output, and HDR_CASES too.
HDR = os.environ.get('RA_E2E_HDR') == '1'
# Gamut 0 is "Accurate": plain Rec.709 to Rec.2020.
HDR10_SETTINGS = {'video_hdr_mode': '1', 'video_hdr_paper_white_nits': '203',
                  'video_hdr_max_nits': '1000', 'video_hdr_menu_nits': '203',
                  'video_hdr_use_display_peak': 'false',
                  'video_hdr_expand_gamut': '0'}
# Builds the fake HDR10 layer for Xvfb.
HDR_UI_PASS = os.path.normpath(os.path.join(
    HERE, '../../../gfx/vulkan_hdr_ui_pass/run.py'))
# What the driver logs as it takes each HDR pass.
GAME_PASS = '[Vulkan] HDR game pass:'
COMPOSITE_PASS = '[Vulkan] HDR composite pass:'
# What it logs as stock views go straight into the swapchain, encoded,
# with no game pass.
STOCK_DIRECT = '[Vulkan] Stock views encoded straight into the HDR swapchain'
# What it logs as a screen window's output is made in SDR or HDR10, and
# for a window it can't draw.
SECOND_OUTPUT_SDR = '[Vulkan] Second output: SDR, format'
SECOND_OUTPUT_HDR10 = '[Vulkan] Second output: HDR10'
WINDOW_NOT_DRAWN = "A screen window can't be drawn"
# The X window x11_video_window_create() makes for screen 2.
SCREEN2_WINDOW = b'RetroArch - Screen 2'
# The HDR10 pairs on the main window's surface only, so a screen
# window's output is SDR.
SDR_WINDOWS = {'FAKE_HDR_FIRST_SURFACE_ONLY': '1'}
# Run only with RA_E2E_HDR=1.
# [(name, map option, settings, points, extra)]. extra may hold preset
# (a preset's path), menu (opened, as in MENU_CASES), env (added to
# RetroArch's), raw (points are the PQ codes' top bytes, not decoded),
# game and composite (whether the HDR game pass and composite are taken:
# True and menu by default), log (lines the log holds, in order), absent
# (lines it does not hold), after (a line the screenshot waits for) and
# window (points on screen 2's window, read on Xvfb only: see
# window_errors()). The fake layer's own lines, and the log and after of
# a case whose env sets one of its FAKE_HDR_ knobs, are checked on Xvfb
# only, where the layer is.
HDR_CASES = [
    ('hdr-preset-menu-sbs', '3ds',
     {'video_stereo_mode': '2', 'video_shader_enable': 'true',
      'menu_driver': 'rgui'}, [],
     {'preset': os.path.join(HERE, 'pass.slangp'), 'menu': True}),
    # An SDR preset's views are drawn in FP16 and encoded by the game
    # pass, as the preset's whole frame would be. The anaglyph's points
    # are the 3ds map's only colours with a channel neither 0 nor 255,
    # which a wrong encode moves.
    ('hdr-preset-3ds', '3ds',
     {'video_stereo_mode': '4', 'video_shader_enable': 'true'},
     [c for c in CASES if c[0] == '3ds-anaglyph'][0][3],
     {'preset': os.path.join(HERE, 'pass.slangp'),
      'log': ["Views' presets read the frame where it lies."]}),
    # A preset that writes HDR10 itself draws its views straight into the
    # swapchain: its codes are the frame's colours, with no game pass.
    ('hdr-emit-preset', '3ds',
     {'video_stereo_mode': '0', 'video_shader_enable': 'true'},
     [(800, 240, GREEN), (800, 720, YELLOW)],
     {'preset': os.path.join(HERE, 'hdr10_pass.slangp'), 'raw': True,
      'game': False}),
    # Screen 2 in a window of its own in SDR, its stock screen, as the
    # main window's, drawn straight into its swapchain. A depth-30 Xvfb
    # surface lists no 8-bit format, so the window's SDR output is
    # 10-bit, its codes sRGB.
    ('hdr-window-ds', 'ds',
     {'video_windows_enable': 'true', 'video_stereo_mode': '0'},
     [(800, 480, GREEN)],
     {'env': SDR_WINDOWS, 'log': [STOCK_DIRECT, SECOND_OUTPUT_SDR],
      'absent': [WINDOW_NOT_DRAWN], 'game': False,
      'window': [(800, 480, YELLOW, 'sdr')]}),
    # An SDR preset's screen in an SDR window: its chain's final pass
    # straight into the window's swapchain.
    ('hdr-preset-window-ds', 'ds',
     {'video_windows_enable': 'true', 'video_stereo_mode': '0',
      'video_shader_enable': 'true'},
     [(800, 480, GREEN)],
     {'preset': os.path.join(HERE, 'pass.slangp'), 'env': SDR_WINDOWS,
      'log': [SECOND_OUTPUT_SDR], 'absent': [WINDOW_NOT_DRAWN],
      'window': [(800, 480, YELLOW, 'sdr')]}),
    # The window draws its screen with the stock chain instead.
    ('hdr-emit-preset-window', 'ds',
     {'video_windows_enable': 'true', 'video_stereo_mode': '0',
      'video_shader_enable': 'true'}, [],
     {'preset': os.path.join(HERE, 'hdr10_pass.slangp'), 'game': False,
      'env': SDR_WINDOWS,
      'log': [SECOND_OUTPUT_SDR, "Views' preset writes HDR: screen windows"],
      'absent': [WINDOW_NOT_DRAWN]}),
    # Screen 2's window takes the main window's HDR10 and encodes as it
    # does, its stock screen straight into its swapchain: two HDR10
    # swapchains, PQ codes in the window's, no game pass in either.
    ('hdr-window-hdr', 'ds',
     {'video_windows_enable': 'true', 'video_stereo_mode': '0'},
     [(800, 480, GREEN)],
     {'log': ['fake-hdr: HDR10 swapchain', 'fake-hdr: HDR10 swapchain',
              SECOND_OUTPUT_HDR10],
      'absent': [WINDOW_NOT_DRAWN], 'window': [(800, 480, YELLOW)],
      'game': False}),
    # The display loses HDR10 with the window up: the window, made in
    # HDR10, is made again in SDR after the main window falls back, and
    # its FP16 image is freed. No points: the main window is then 10-bit
    # SDR, whose screenshots are not taken.
    ('hdr-window-drop', 'ds',
     {'video_windows_enable': 'true', 'video_stereo_mode': '0'}, [],
     # run.log, then retroarch.log
     {'env': {'FAKE_HDR_DROP_AFTER': '120'},
      'log': ['fake-hdr: HDR10 dropped', SECOND_OUTPUT_HDR10,
              'HDR10 not available on this surface, falling back.',
              SECOND_OUTPUT_SDR],
      'absent': [WINDOW_NOT_DRAWN], 'game': False,
      'after': 'HDR10 not available on this surface, falling back.'}),
]
# (game, composite) for the other cases that check the HDR passes they
# take; the runs made from a case (crop-threaded-menu-0,
# view-rect-ds-rect-nearest) take its entry. Stock views go straight
# into the swapchain; the anaglyph's canvas through the game pass.
HDR_PASSES = {'3ds-top-bottom': (False, False), '3ds-anaglyph': (True, False),
              'view-rect-ds': (True, False),
              # drawn whole, so straight to the swapchain
              'crop-threaded-menu': (False, True),
              'hdr-menu-brightness': (None, True)}
# Run only with RA_E2E_HDR=1, as MENU_CASES but compared by brightness:
# the menu in a views UI layer is encoded at menu nits, as without views.
# Menu nits apart from paper white show a layer encoded at paper white.
# [(name, map option, settings, stereo modes)]
HDR_MENU_CASES = [
    ('hdr-menu-brightness', '3ds',
     {'menu_driver': 'rgui', 'video_hdr_menu_nits': '400'}, ('0', '2')),
    ('hdr-menu-brightness-threaded', '3ds',
     {'menu_driver': 'rgui', 'video_hdr_menu_nits': '400',
      'video_threaded': 'true'}, ('0', '2')),
]
# PQ (SMPTE ST 2084), and Rec.2020 primaries to Rec.709 and back as
# hdr_common.glsl has them.
M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32
BT709_TO_2020 = ((0.6274040, 0.3292820, 0.0433136),
                 (0.0690970, 0.9195400, 0.0113612),
                 (0.0163916, 0.0880132, 0.8955950))
BT2020_TO_709 = ((1.6604910, -0.5876411, -0.0728499),
                 (-0.1245505, 1.1328999, -0.0083494),
                 (-0.0181508, -0.1005789, 1.1187297))
# The fake HDR10 layer's environment once built, and the directory it
# is built in, which main() removes.
HDR_LAYER_ENV = {}
HDR_LAYER_DIRS = []
HDR_LAYER_LOCK = threading.Lock()
# The core's rules cases (video_views_test_screens rules), each sent on
# the ds map for a frame, with whether the frontend must accept it (None:
# on IMAGES_DRIVERS); then the full map.
SCREENS_RULES = [
    ('no-flag', 1), ('zero-screens', 1), ('absent-viewless', 1),
    ('swap-one-side', 1), ('swap-both', 1), ('unknown-flags', 1),
    ('too-many', 0), ('null-screens', 0), ('view-past-count', 0),
    ('image', None), ('active-viewless', 0), ('blank-viewless', 0),
    ('eye-unpaired', 0), ('base-past-count', 0), ('over-self', 0),
    ('over-over', 0), ('size-negative', 0), ('anchor', 0),
    ('over-size', 0), ('opacity', 0), ('private-owners', 0),
    ('swap-self', 0), ('swap-past-count', 0), ('swap-disagree', 0),
    ('show-unknown', 0), ('state-unknown', 0), ('state-unknown-viewed', 0),
    ('order-mixed', 0), ('order-mixed-first', 0), ('full', 1)]
SCREENS_CASE_RE = re.compile(
    r'\[video_views\] screens case=([\w-]+) accepted=(\d)')
SCREENS_FULL_RE = re.compile(r'\[video_views\] screens case=full ')
SCREEN_BUFFER_RE = re.compile(r'\[video_views\] screen_buffer=(\d)')
# The full map's screens as the frontend logs them.
SCREENS_FULL_LOG = [
    '[Video] Screen 0: "Top", flags 0x22, state 0, show 0, base 0, '
    'size 0.000, anchor 0, opacity 0.000, order 2, owners 0x0, '
    'swap with 2, group 1.',
    '[Video] Screen 1: "Bottom", flags 0x19, state 1, show 0, base 0, '
    'size 0.500, anchor 0, opacity 0.000, order 1, owners 0x0, '
    'swap with 0, group 1.',
    '[Video] Screen 2: "VMU A1", flags 0xd, state 2, show 2, base 0, '
    'size 0.250, anchor 8, opacity 0.750, order 3, owners 0x1, '
    'swap with 0, group 0.']
SCREEN_LOG_RE = re.compile(r'\[Video\] Screen \d+: .*')
# GET_VIDEO_SCREEN_BUFFER, which the frontend knows
SCREEN_BUFFER_UNSUPPORTED_RE = re.compile(
    r'\[Environ\] UNSUPPORTED \(#(?:108|65644)\)')
SCREENS_SWITCHED_RE = re.compile(r'\[video_views\] screens switched')
# The light gun aimed, with the menu closed, at the top screen's centre
# and then the bottom one's, as ds-horizontal lays them out. Aimed at
# the top first, so the second aim reads differently whatever the
# pointer's start. A blank screen still takes the gun.
# [(name, video_views_test_screens, the screen the second aim lands
# in: 'bottom', 'top' or None, points, what run_case() is given
# besides, with 'aim' the two points instead)]
SCREENS_AIM_SETTINGS = {'video_stereo_mode': '0', 'video_screen_layout': '1'}
SCREENS_AIM_POINTS = [(400, 480, GREEN), (1200, 480, YELLOW)]
# The bottom screen blank: black, in its place.
SCREENS_BOTTOM_BLANK = [(400, 480, GREEN), (1200, 480, BLACK)]
SCREENS_AIM_CASES = [
    ('screens-no-input', 'no_input', None, SCREENS_AIM_POINTS, {}),
    ('screens-no-input-off', 'no_input_off', 'bottom', SCREENS_AIM_POINTS,
     {}),
    ('screens-blank-aim', 'blank', 'bottom', SCREENS_BOTTOM_BLANK, {}),
    # NO_INPUT sent with a NULL refresh, aimed after it
    ('screens-no-input-null', 'no_input_null', None, SCREENS_AIM_POINTS,
     {'until': SCREENS_SWITCHED_RE})]
SCREENS_AIM = ((400, 480), (1200, 480))
GUN_RE = re.compile(r'\[video_views\] lightgun x=(-?\d+) y=(-?\d+) '
                    r'offscreen=(\d) trigger=(\d) '
                    r'packed=\((-?\d+),(-?\d+)\)')
# What the screens cases check is the frontend's, the same on every
# driver: they run on those of Linux-vulkan-validation.yml's views step.
SCREENS_DRIVERS = ('gl', 'glcore', 'vulkan', 'sdl2')
# Those and the drivers whose SCREENS bit the draw and status cases
# check on every status line: the bit comes from each driver's FALLBACK.
SCREENS_STATUS_DRIVERS = SCREENS_DRIVERS + (
    'sdl3', 'gdi', 'd3d8', 'd3d9_hlsl', 'd3d10', 'd3d11', 'd3d12', 'xvideo')
# Those of them that draw image screens: IMAGES set, lends given.
IMAGES_DRIVERS = ('gl', 'glcore', 'vulkan', 'd3d11')
# What the window shows of the ds map's two screens, laid out as
# SCREENS_AIM_SETTINGS has them. Screen 0 alone is 4:3, 1280 wide from
# x 160.
SCREENS_BOTH = SCREENS_AIM_POINTS
SCREENS_TOP_ALONE = [(800, 480, GREEN), (1200, 480, GREEN),
                     (100, 480, BLACK)]
SCREENS_NONE = [(400, 480, BLACK), (800, 480, BLACK), (1200, 480, BLACK)]
# Screen 1 over screen 0's bottom right corner at a quarter of its
# height (x 1120-1440, y 720-960): points 20 pixels either side of its
# edges, and its centre. Those in it take the colour it is drawn in.
SCREENS_INSET_AROUND = [(800, 480, GREEN), (1000, 840, GREEN),
                        (1100, 840, GREEN), (1280, 700, GREEN),
                        (1460, 840, BLACK)]
SCREENS_INSET_IN = [(1140, 840), (1280, 740), (1280, 840)]
SCREENS_INSET = SCREENS_INSET_AROUND + [
    (x, y, YELLOW) for x, y in SCREENS_INSET_IN]
SCREENS_INSET_BLANK = SCREENS_INSET_AROUND + [
    (x, y, BLACK) for x, y in SCREENS_INSET_IN]
# At half opacity, over GREEN.
SCREENS_INSET_HALF = SCREENS_INSET_AROUND + [
    (x, y, (128, 255, 0)) for x, y in SCREENS_INSET_IN]
SCREENS_INSET_BLANK_HALF = SCREENS_INSET_AROUND + [
    (x, y, (0, 128, 0)) for x, y in SCREENS_INSET_IN]
# Aimed at the inset's centre: it takes the gun, or with NO_INPUT the
# top screen under it does.
SCREENS_INSET_AIM = ((400, 480), (1280, 840))
SCREENS_AIM_CASES += [
    ('screens-over-aim', 'over', 'bottom', SCREENS_INSET,
     {'aim': SCREENS_INSET_AIM}),
    ('screens-over-no-input-aim', 'over_no_input', 'top', SCREENS_INSET,
     {'aim': SCREENS_INSET_AIM})]


def screens_over_points(stereo, colour):
    """The over cases' points, the inset's in @colour: in one picture
    ('0'), side by side ('1') or interlaced ('5', each again a row down,
    in the other eye's rows)."""
    if stereo == '1':
        inside = [(640, 840), (1440, 840), (640, 760), (1440, 760)]
        return [(300, 480, GREEN), (1100, 480, GREEN), (540, 840, GREEN),
                (1340, 840, GREEN)] + [(x, y, colour) for x, y in inside]
    points = SCREENS_INSET_AROUND + [
        (x, y, colour) for x, y in SCREENS_INSET_IN]
    if stereo == '5':
        points += [(x, y + 1, c) for x, y, c in points]
    return points


# The over modes: the inset's colour, and solid where the driver draws
# it so.
SCREENS_OVER_MODES = [('over', YELLOW, YELLOW), ('over_blank', BLACK, BLACK),
                      ('over_half', (128, 255, 0), YELLOW),
                      ('over_blank_half', (0, 128, 0), BLACK)]
# Drivers that draw an inset at its opacity; the rest draw it solid.
OPACITY_DRIVERS = ('gl', 'glcore', 'gl1', 'vulkan', 'd3d8', 'd3d9_hlsl',
                   'd3d10', 'd3d11', 'sdl', 'sdl2', 'sdl3')
# The core's own screen choices, under the case directory. A file the
# frontend wrote again would have lost the comment.
CORE_FILE = os.path.join('video_views test', 'video_views test.screens.cfg')
CORE_FILE_OFF = {CORE_FILE: '# kept\nvideo_screen2_display = "off"\n'}
# A screen left out, blanked or drawn over another, drawn by the driver:
# on every driver. [(name, video_views_test_screens, points, what
# run_case() is given besides, with 'settings' added to
# SCREENS_AIM_SETTINGS, 'solid' the points instead on a driver not in
# OPACITY_DRIVERS and 'map' the map option instead of ds)]
SCREENS_DRAW_CASES = [
    ('screens-hidden', 'hidden', SCREENS_TOP_ALONE, {}),
    ('screens-blank-late', 'blank_late', SCREENS_BOTTOM_BLANK,
     {'until': SCREENS_SWITCHED_RE}),
    # Sent with a NULL refresh, every refresh after it NULL
    ('screens-hide-null', 'hide_null', SCREENS_TOP_ALONE,
     {'until': SCREENS_SWITCHED_RE}),
    ('screens-blank-null', 'blank_null', SCREENS_BOTTOM_BLANK,
     {'until': SCREENS_SWITCHED_RE}),
    ('screens-none-shown', 'none_shown', SCREENS_NONE, {}),
    ('screens-over', 'over', SCREENS_INSET, {}),
    ('screens-over-blank', 'over_blank', SCREENS_INSET_BLANK, {}),
    ('screens-over-half', 'over_half', SCREENS_INSET_HALF,
     {'solid': SCREENS_INSET}),
    ('screens-over-blank-half', 'over_blank_half', SCREENS_INSET_BLANK_HALF,
     {'solid': SCREENS_INSET_BLANK}),
] + [
    # The over cases side by side and interlaced: an inset in each eye's
    # area, after its base there.
    ('screens-%s-%s' % (mode.replace('_', '-'), kind), mode,
     screens_over_points(stereo, colour),
     {'solid': screens_over_points(stereo, solid),
      'settings': {'video_stereo_mode': stereo}})
    for kind, stereo in (('sbs', '1'), ('interlaced', '5'))
    for mode, colour, solid in SCREENS_OVER_MODES]
# Not on the drivers that draw views straight from the frame and keep
# nothing to draw them again from: a NULL refresh leaves their last
# picture, or on XShm black.
SCREENS_NULL_CASES = ('screens-hide-null', 'screens-blank-null')
NO_REDRAW_DRIVERS = ('sdl', 'x11', 'xvideo', 'gdi')
# xvideo's interlaced path places and clears by itself: the late blank
# again there, in each eye's rows. An entry as above.
SCREENS_XV_INTERLACED_CASE = (
    'screens-blank-late-interlaced', 'blank_late',
    SCREENS_BOTTOM_BLANK + [(400, 481, GREEN), (1200, 481, BLACK)],
    {'until': SCREENS_SWITCHED_RE, 'settings': {'video_stereo_mode': '5'}})
# Through the pass preset of each driver whose views take one, which
# draws one view's passes after another: Top (view 0), made Bottom's
# inset by the user, must still go over it; on the 3ds map side by
# side, Bottom made Top's inset goes over each eye's Top, blank too;
# and the over cases. Entries as above.
SCREENS_PRESETS = {'gl': 'pass.glslp', 'glcore': 'pass.slangp',
                   'vulkan': 'pass.slangp', 'd3d9_hlsl': 'pass.cgp',
                   'd3d10': 'pass.slangp', 'd3d11': 'pass.slangp',
                   'd3d12': 'pass.slangp'}
SCREENS_STEREO_OVER = {
    'map': '3ds',
    'files': {CORE_FILE: 'video_screen2_display = "over:1"\n'},
    'settings': {'video_shader_enable': 'true', 'video_stereo_mode': '1'}}
SCREENS_PRESET_CASES = [
    ('screens-user-over-preset', 'shown',
     [(800, 480, YELLOW), (1280, 840, GREEN)],
     {'files': {CORE_FILE: 'video_screen1_display = "over:2"\n'},
      'settings': {'video_shader_enable': 'true'}}),
    ('screens-user-over-stereo-preset', 'shown',
     [(400, 480, RED), (1200, 480, BLUE), (720, 840, YELLOW),
      (1520, 840, YELLOW)], SCREENS_STEREO_OVER),
    ('screens-user-over-blank-stereo-preset', 'blank',
     [(400, 480, RED), (1200, 480, BLUE), (720, 840, BLACK),
      (1520, 840, BLACK)], SCREENS_STEREO_OVER),
] + [
    (name + '-preset', mode, points,
     dict(extra, settings=dict(extra.get('settings', {}),
                               video_shader_enable='true')))
    for name, mode, points, extra in SCREENS_DRAW_CASES
    if mode in [m for m, _, _ in SCREENS_OVER_MODES]]
# Not on glcore and vulkan: their preset's last pass draws a translucent
# inset solid until the follow-up that keeps it blended.
SCREENS_PRESET_HALF = ('screens-over-half-preset',
                       'screens-over-half-sbs-preset',
                       'screens-over-half-interlaced-preset')
SCREENS_PRESET_SOLID = {'glcore': SCREENS_PRESET_HALF,
                        'vulkan': SCREENS_PRESET_HALF}
# What the frontend makes of show, state, the user's setting and the
# core's file: on SCREENS_DRIVERS. Each file a case starts with must be
# the same after it. Entries as SCREENS_DRAW_CASES'.
SCREENS_STATE_CASES = [
    ('screens-shown', 'shown', SCREENS_BOTH, {}),
    ('screens-absent', 'absent', SCREENS_TOP_ALONE, {}),
    ('screens-blank', 'blank', SCREENS_BOTTOM_BLANK, {}),
    ('screens-absent-back', 'absent_back', SCREENS_BOTH,
     {'until': SCREENS_SWITCHED_RE}),
    ('screens-user-off', 'shown', SCREENS_TOP_ALONE,
     {'settings': {'video_screen2_display': 'off'}}),
    # The user's Auto in the core's file beats the core's HIDDEN.
    ('screens-core-beaten', 'hidden', SCREENS_BOTH,
     {'files': {CORE_FILE: '# kept\nvideo_screen2_display = ""\n'}}),
    ('screens-per-core-own', 'shown', SCREENS_TOP_ALONE,
     {'files': CORE_FILE_OFF}),
    # A core of another name does not read it.
    ('screens-per-core-other', 'shown', SCREENS_BOTH,
     {'files': CORE_FILE_OFF,
      'env_extra': {'VIDEO_VIEWS_TEST_LIBRARY_NAME': 'video_views test B'}}),
    # Bottom made Top's inset by the user, at the default corner and
    # size, then at the top left at half Top's height.
    ('screens-user-over', 'shown', SCREENS_INSET,
     {'files': {CORE_FILE: 'video_screen2_display = "over:1"\n'}}),
    ('screens-user-over-corner', 'shown',
     [(480, 240, YELLOW), (1000, 700, GREEN)],
     {'files': {CORE_FILE: 'video_screen2_display = "over:1"\n'
                           'video_screen2_corner = "1"\n'
                           'video_screen2_size = "50"\n'}}),
    # Bottom SHOWN at a quarter of Top's height, beside it.
    ('screens-shown-sized', 'shown_sized',
     [(1440, 480, YELLOW), (640, 480, GREEN), (1440, 200, BLACK)], {}),
]
# With no screen placed the menu still lays out over the whole window,
# as it does over a frame without views: the second case, which the
# first one's menu must match. Opaque, so the frame behind is not seen.
SCREENS_MENU_SETTINGS = dict(SCREENS_AIM_SETTINGS, menu_driver='ozone',
                             menu_timedate_enable='false',
                             menu_framebuffer_opacity='1.000000')
SCREENS_MENU_CASE = ('screens-none-shown-menu', 'ds',
                     SCREENS_MENU_SETTINGS, [])
SCREENS_MENU_REF = ('screens-none-shown-menu-ref', 'none',
                    SCREENS_MENU_SETTINGS, [])
# With no map set, SCREENS says whether one sent now would have its
# views drawn: on SCREENS_STATUS_DRIVERS, unset on the drivers that
# would draw it whole. [(name, settings, those drivers, RetroArch's arguments)]
SCREENS_STATUS_CASES = [
    ('screens-status-no-map', {'video_stereo_mode': '0'}, (), ()),
    ('screens-status-no-map-anaglyph', {'video_stereo_mode': '4'},
     WHOLE_ANAGLYPH_DRIVERS, ()),
    ('screens-status-no-map-rolling',
     {'video_shader_subframes': '2', 'video_scan_subframes': 'true'},
     ('d3d10', 'd3d11', 'd3d12'), ()),
]
# gl draws a map whole under a preset that mipmaps the frame.
SCREENS_GL_STATUS_CASES = [
    ('screens-status-no-map-mipmap', {'video_shader_enable': 'true'},
     ('gl',), ('--set-shader=' + os.path.join(HERE, 'mipmap.glslp'),)),
]

# The image modes' screen 2, a 48x32 picture in quadrants: top left, top
# right, bottom left, bottom right. Mirrored by image_null's switch; at
# half opacity over GREEN.
MAGENTA = (255, 0, 255)
IMAGE_QUADRANTS = (RED, BLUE, WHITE, MAGENTA)
IMAGE_MIRRORED = (BLUE, RED, MAGENTA, WHITE)
IMAGE_HALF = ((128, 128, 0), (0, 128, 128), (128, 255, 128),
              (128, 128, 128))
# Where it lands, (x, y, width, height): beside Top, five pixels to a
# core pixel as Top, which moves to x 40 for it; and over Top's bottom
# right corner at a quarter of its height, 3:2.
SCREENS_IMAGE_BESIDE = (1320, 400, 240, 160)
SCREENS_IMAGE_INSET = (1080, 720, 360, 240)
# Top around each, and what lies past each edge of the picture: left,
# top, right, bottom (None: off the window).
SCREENS_IMAGE_BESIDE_TOP = [(680, 480, GREEN), (100, 480, GREEN),
                            (20, 480, BLACK)]
SCREENS_IMAGE_BESIDE_OUT = (GREEN, BLACK, BLACK, BLACK)
SCREENS_IMAGE_INSET_TOP = [(800, 480, GREEN)]
SCREENS_IMAGE_INSET_OUT = (GREEN, GREEN, BLACK, None)
# Under video_rotation 1 Top turns to 192x256 and the picture stays
# 48x32, side by side at 3.75 pixels to a core pixel, Top at x 350.
SCREENS_IMAGE_ROTATED = (1070, 420, 180, 120)
SCREENS_IMAGE_ROTATED_TOP = [(710, 480, GREEN), (360, 480, GREEN),
                             (340, 480, BLACK)]
IMAGE_SENT_RE = re.compile(r'\[video_views\] image serial=(\d+) '
                           r'accepted=(\d)')
IMAGE_NOT_LENT_RE = re.compile(r'\[video_views\] image buffer refused')
# video_driver_init_internal's line, once per init: after the switch,
# image_restart's restart.
SCREENS_RESTARTED_RE = re.compile(r'\[video_views\] screens switched'
                                  r'[\s\S]*\[Video\] Set video size to:')


def image_quadrants(rect):
    """The quadrants of the picture at rect, in IMAGE_QUADRANTS' order."""
    x, y, w, h = rect
    return [(x + w // 2 * i, y + h // 2 * j, w // 2, h // 2)
            for j in (0, 1) for i in (0, 1)]


def image_centres(rect, colours):
    """Each quadrant's centre in its colour."""
    return [(x + w // 2, y + h // 2, c)
            for (x, y, w, h), c in zip(image_quadrants(rect), colours)]


def image_points(rect, colours, out):
    """Each quadrant's centre and the second pixel in from each of its
    edges in its colour, and the second pixel past each edge of the
    picture in out's colour for that edge."""
    x, y, w, h = rect
    points = image_centres(rect, colours)
    for (qx, qy, qw, qh), c in zip(image_quadrants(rect), colours):
        cx, cy = qx + qw // 2, qy + qh // 2
        points += [(qx + 1, cy, c), (qx + qw - 2, cy, c),
                   (cx, qy + 1, c), (cx, qy + qh - 2, c)]
    cx, cy = x + w // 2, y + h // 2
    past = ((x - 2, cy), (cx, y - 2), (x + w + 1, cy), (cx, y + h + 1))
    return points + [(px, py, c) for (px, py), c in zip(past, out)
                     if c is not None]


def image_blends(rect, colours):
    """Across each edge between two quadrants, level with their centres:
    the second pixel each side, and the two quadrants' colours."""
    tl, tr, bl, br = colours
    x, y, w, h = rect
    mx, my = x + w // 2, y + h // 2
    top, bottom = y + h // 4, y + 3 * h // 4
    left, right = x + w // 4, x + 3 * w // 4
    return [((mx - 2, top), (mx + 1, top), tl, tr),
            ((mx - 2, bottom), (mx + 1, bottom), bl, br),
            ((left, my - 2), (left, my + 1), tl, bl),
            ((right, my - 2), (right, my + 1), tr, br)]


def screens_image_beside(colours):
    return SCREENS_IMAGE_BESIDE_TOP + image_points(
        SCREENS_IMAGE_BESIDE, colours, SCREENS_IMAGE_BESIDE_OUT)


def screens_image_rotated(colours):
    return SCREENS_IMAGE_ROTATED_TOP + image_points(
        SCREENS_IMAGE_ROTATED, colours, SCREENS_IMAGE_BESIDE_OUT)


def screens_image_inset(colours):
    return SCREENS_IMAGE_INSET_TOP + image_points(
        SCREENS_IMAGE_INSET, colours, SCREENS_IMAGE_INSET_OUT)


def screens_image_sbs(colours):
    """SCREENS_IMAGE_BESIDE's layout halved into each eye's half."""
    x, y, w, h = SCREENS_IMAGE_BESIDE
    points = []
    for eye in (0, W // 2):
        points += [(px // 2 + eye, py, c)
                   for px, py, c in SCREENS_IMAGE_BESIDE_TOP]
        points += image_points((x // 2 + eye, y, w // 2, h), colours,
                               SCREENS_IMAGE_BESIDE_OUT)
    return points


# The image modes, on IMAGES_DRIVERS. Entries as SCREENS_DRAW_CASES',
# with 'opts' added to the core's options, 'blends' pixels that must be
# neither side's colour, 'handins' how many pictures the core sends,
# 'preset' True to draw through the driver's SCREENS_PRESETS preset,
# 'hw' True to run once for each hardware kind EXACT_HW gives the driver,
# 'drivers' those of IMAGES_DRIVERS the case runs on and 'window' the
# colours of the picture's quadrants in screen 2's own window, which
# only the Xvfb runs can find.
SCREENS_IMAGE_CASES = [
    ('screens-image', 'image', screens_image_beside(IMAGE_QUADRANTS), {}),
    ('screens-image-over', 'image_over',
     screens_image_inset(IMAGE_QUADRANTS), {}),
    ('screens-image-over-half', 'image_over_half',
     screens_image_inset(IMAGE_HALF), {}),
    ('screens-image-over-blank', 'image_over_blank',
     screens_image_inset((BLACK,) * 4), {}),
    ('screens-image-smooth', 'image',
     SCREENS_IMAGE_BESIDE_TOP
     + image_centres(SCREENS_IMAGE_BESIDE, IMAGE_QUADRANTS),
     {'opts': {'video_views_test_image_sharp': 'off'},
      'settings': {'video_smooth': 'true'},
      'blends': image_blends(SCREENS_IMAGE_BESIDE, IMAGE_QUADRANTS)}),
    ('screens-image-sharp-smooth', 'image',
     screens_image_beside(IMAGE_QUADRANTS),
     {'settings': {'video_smooth': 'true'}}),
    ('screens-image-copy', 'image', screens_image_beside(IMAGE_QUADRANTS),
     {'opts': {'video_views_test_image': 'copy'}}),
    ('screens-image-null', 'image_null',
     screens_image_beside(IMAGE_MIRRORED), {'until': SCREENS_SWITCHED_RE}),
    ('screens-image-restart', 'image_restart',
     screens_image_beside(IMAGE_QUADRANTS),
     {'until': SCREENS_RESTARTED_RE, 'handins': 1}),
    ('screens-image-over-threaded', 'image_over',
     screens_image_inset(IMAGE_QUADRANTS),
     {'settings': {'video_threaded': 'true'}, 'drivers': ('gl', 'vulkan')}),
    # The inset over Top's passes.
    ('screens-image-over-preset', 'image_over',
     screens_image_inset(IMAGE_QUADRANTS), {'preset': True}),
    # Placed in each eye, whose rows interlace: a picture missing from
    # either loses every other row.
    ('screens-image-interlaced', 'image',
     screens_image_beside(IMAGE_QUADRANTS),
     {'settings': {'video_stereo_mode': '5'}}),
    # Side by side under the preset: each eye's picture follows a pass
    # of Top's, which may leave the driver's own state behind.
    ('screens-image-stereo-preset', 'image',
     screens_image_sbs(IMAGE_QUADRANTS),
     {'preset': True, 'settings': {'video_stereo_mode': '1'}}),
    # Side by side straight from the frame: the right eye's Top follows
    # the left eye's picture, whose texture and filter it must not keep.
    ('screens-image-sbs', 'image', screens_image_sbs(IMAGE_QUADRANTS),
     {'settings': {'video_stereo_mode': '1'}}),
    # Over the core's own frame, bottom up or top down: the picture's
    # first row stays at the top.
    ('screens-image-hw', 'image', screens_image_beside(IMAGE_QUADRANTS),
     {'hw': True}),
    # The user's rotation turns Top, never the picture.
    ('screens-image-rotated', 'image',
     screens_image_rotated(IMAGE_QUADRANTS),
     {'settings': {'video_rotation': '1'}}),
    # In a Game Screens window of its own, Top alone in the main one.
    ('screens-image-window', 'image', SCREENS_TOP_ALONE,
     {'window': IMAGE_QUADRANTS, 'drivers': ('gl', 'vulkan'),
      'settings': {'video_windows_enable': 'true'}}),
    # The window drawn again for a picture changed on a NULL refresh.
    ('screens-image-window-null', 'image_null', SCREENS_TOP_ALONE,
     {'window': IMAGE_MIRRORED, 'drivers': ('gl', 'vulkan'),
      'settings': {'video_windows_enable': 'true'},
      'until': SCREENS_SWITCHED_RE}),
]
# A screen's own window, as video_window_title() names it.
SCREEN_WINDOW_RE = re.compile(r'^\s*(0x[0-9a-f]+) "RetroArch - [^"]*":',
                              re.M)
# The contexts the window cases need where the driver's default makes no
# screen windows: GLX's.
WINDOW_CONTEXTS = {'gl': 'egl_x'}


# The last few files read_png() decoded on this thread, by path, with
# the stat that says they are the same file: a pointer case reads each
# screenshot to wait on it and again to check it.
PNG_CACHE = threading.local()


def read_png(path, raw=False):
    """8-bit RGB or RGBA, or 16-bit PQ RGB as read_png_file() reads it;
    non-interlaced: what rpng writes."""
    st    = os.stat(path)
    key   = (st.st_mtime_ns, st.st_size, st.st_ino, raw)
    cache = PNG_CACHE.__dict__.setdefault('files', {})
    hit   = cache.get(path)
    if hit and hit[0] == key:
        return hit[1]
    png = read_png_file(path, raw)
    if len(cache) >= 8:
        cache.pop(next(iter(cache)))
    cache[path] = (key, png)
    return png


def read_png_file(path, raw=False):
    """read_png() without the cache. A 16-bit PQ screenshot (HDR10 or
    scRGB) comes back as 8-bit RGB: the SDR colours it was encoded from,
    or with raw the codes' top bytes."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat = 8, b''
    w = h = ct = bd = cicp = None
    while pos < len(data):
        ln, = struct.unpack('>I', data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b'IHDR':
            w, h, bd, ct = struct.unpack('>IIBB', body[:10])
        elif typ == b'IDAT':
            idat += body
        elif typ == b'cICP':
            cicp = body[:2]
        elif typ == b'IEND':
            break
    if (bd, ct) not in ((8, 2), (8, 6), (16, 2)):
        raise ValueError('unsupported PNG: depth %s type %s' % (bd, ct))
    # cICP primaries 9 (Rec.2020) or 1 (scRGB's Rec.709), transfer PQ
    if bd == 16 and (not cicp or cicp[0] not in (1, 9) or cicp[1] != 16):
        raise ValueError('16-bit PNG without PQ in its cICP')
    bpp = (3 if ct == 2 else 4) * bd // 8
    data = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, i = [], bytearray(stride), 0
    # Up rows add the row above a byte at a time, with no carry between
    # bytes: done on the whole row as one integer, low 7 bits and top bit
    # apart, instead of byte by byte.
    low = int.from_bytes(b'\x7f' * stride, 'little')
    top = int.from_bytes(b'\x80' * stride, 'little')
    for _ in range(h):
        f = data[i]
        line = bytearray(data[i + 1:i + 1 + stride])
        i += 1 + stride
        if f == 2:
            p = int.from_bytes(line, 'little')
            q = int.from_bytes(prev, 'little')
            line = bytearray((((p & low) + (q & low)) ^ ((p ^ q) & top))
                             .to_bytes(stride, 'little'))
        for x in range(stride if f not in (0, 2) else 0):
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
    if bd == 16:
        memo = {}
        rows = [pq_row(line, cicp[0] == 9, raw, memo) for line in rows]
        bpp = 3
    return w, h, bpp, rows


def png_depth(path):
    """A PNG's bit depth, from its IHDR."""
    with open(path, 'rb') as f:
        return f.read(25)[24]


def pq16_to_srgb8(code16, bt2020):
    """The SDR colour hdr.frag encoded as a 16-bit PQ pixel (r, g, b):
    PQ to nits, paper white to 1.0, Rec.2020 primaries to Rec.709 when
    bt2020, gamma 2.4."""
    white = float(HDR10_SETTINGS['video_hdr_paper_white_nits'])
    lin = []
    for c in code16:
        e = (c / 65535) ** (1 / M2)
        nits = 10000 * (max(e - C1, 0) / (C2 - C3 * e)) ** (1 / M1)
        lin.append(nits / white)
    if bt2020:
        lin = [sum(m * v for m, v in zip(row, lin)) for row in BT2020_TO_709]
    return tuple(round(255 * min(max(v, 0.0), 1.0) ** (1 / 2.4))
                 for v in lin)


def srgb8_to_pq10(rgb):
    """The 10-bit codes HDR10 encodes an SDR colour (r, g, b) as:
    pq16_to_srgb8() backwards."""
    white = float(HDR10_SETTINGS['video_hdr_paper_white_nits'])
    lin = [(c / 255) ** 2.4 for c in rgb]
    lin = [sum(m * v for m, v in zip(row, lin)) for row in BT709_TO_2020]
    codes = []
    for v in lin:
        y = (max(v, 0.0) * white / 10000) ** M1
        codes.append(round(1023 * ((C1 + C2 * y) / (1 + C3 * y)) ** M2))
    return tuple(codes)


def pq_row(line, bt2020, raw, memo):
    """A row of 16-bit PQ RGB as read_png_file() returns it; memo holds
    the pixels decoded so far."""
    if raw:
        return line[0::2]
    out = bytearray(len(line) // 2)
    for i in range(0, len(line), 6):
        px = bytes(line[i:i + 6])
        rgb = memo.get(px)
        if rgb is None:
            rgb = memo[px] = bytes(pq16_to_srgb8(struct.unpack('>3H', px),
                                                 bt2020))
        out[i // 2:i // 2 + 3] = rgb
    return out


def send(cmd, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(cmd.encode(), ('127.0.0.1', port))
    s.close()


# Ports and displays handed to cases running side by side.
TAKEN_LOCK = threading.Lock()
TAKEN_PORTS = set()
TAKEN_DISPLAYS = set()


def free_port():
    with TAKEN_LOCK:
        while True:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind(('127.0.0.1', 0))
            port = s.getsockname()[1]
            s.close()
            if port not in TAKEN_PORTS:
                TAKEN_PORTS.add(port)
                return port


def write_cfg(path, d, driver, settings, port):
    cfg = {
        'video_driver': driver,
        # vk_wayland segfaults on teardown in headless gamescope; stay on X.
        'video_context_driver': 'vk_x' if driver == 'vulkan' else 'x',
        'input_driver': 'x',
        # Keep real controllers and achievements out of the run: the test
        # joypad driver has no pads without a test file ('null' falls
        # back to udev).
        'input_joypad_driver': 'test',
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
        # A core asking for the microphone made ALSA's default capture
        # device block the main thread once.
        'microphone_enable': 'false',
        'microphone_driver': 'null',
        'pause_nonactive': 'false',
        'gamemode_enable': 'false',
        'suspend_screensaver_enable': 'false',
        'confirm_quit': 'false',
        'quit_press_twice': 'false',
        'config_save_on_exit': 'false',
        # A first run offers the missing downloads in a dialog over the
        # menu, which then holds the menu's input.
        'rgui_show_start_screen': 'false',
        'network_cmd_enable': 'true',
        'network_cmd_port': str(port),
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
        # Everything else RetroArch can write by default lives under
        # ~/.config/retroarch; keep it all in the case directory instead.
        # config_set_defaults() computes content_favorites_path etc. from
        # playlist_directory before this file is even parsed, so the
        # playlist paths need setting directly, not just their directory.
        'playlist_directory': d,
        'content_favorites_path': os.path.join(d, 'content_favorites.lpl'),
        'content_history_path': os.path.join(d, 'content_history.lpl'),
        'content_image_history_path':
            os.path.join(d, 'content_image_history.lpl'),
        'content_music_history_path':
            os.path.join(d, 'content_music_history.lpl'),
        'content_video_history_path':
            os.path.join(d, 'content_video_history.lpl'),
        'input_remapping_directory': d,
        'rgui_config_directory': d,
        'video_shader_dir': d,
        'cheat_database_path': d,
        'recording_output_directory': d,
        'recording_config_directory': d,
        'runtime_log_directory': d,
        'thumbnails_directory': d,
        'core_assets_directory': d,
        'dynamic_wallpapers_directory': d,
    }
    if WINE:
        cfg = {k: winpath(v) for k, v in cfg.items()}
        cfg.update(video_context_driver='', input_driver='dinput')
    if HDR and driver == 'vulkan':
        cfg.update(HDR10_SETTINGS)
    cfg.update(settings)
    with open(path, 'w') as f:
        for k in sorted(cfg):
            f.write('%s = "%s"\n' % (k, cfg[k]))


def winpath(v):
    """An absolute path as Wine's Z: drive has it; anything else as is."""
    return 'Z:' + v if WINE and v.startswith('/') else v


def expected_status(driver, settings):
    """(presents, stereo) the frontend should report for these settings."""
    return (1, 0 if settings.get('video_stereo_mode', '0') == '0' else 1)


def wait_for_views(p, d, timeout=60, regex=STATUS_RE):
    """Wait for initialization to reach the core before timing the case,
    or for the logs to have regex."""
    deadline = time.monotonic() + timeout
    while p.poll() is None:
        for name in ('run.log', 'retroarch.log'):
            path = os.path.join(d, name)
            if os.path.exists(path):
                with open(path, errors='replace') as f:
                    if regex.search(f.read()):
                        return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.1)



def startup_stack(p, d):
    """Keep the native initialization fault site before terminating a case."""
    if WINE or not XVFB or os.name != 'posix' or p.poll() is not None:
        return
    debugger = shutil.which('gdb')
    if not debugger:
        return
    command = [debugger, '-batch', '-ex', 'set pagination off',
               '-ex', 'thread apply all bt', '-p', str(p.pid)]
    if os.environ.get('GITHUB_ACTIONS') == 'true' and os.geteuid() != 0:
        command = ['sudo', '-n'] + command
    with open(os.path.join(d, 'startup-stack.log'), 'w') as log:
        try:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                           timeout=15)
        except (OSError, subprocess.TimeoutExpired) as error:
            log.write(str(error) + '\n')


def statuses(d):
    """Every views status the core logged, in order."""
    found = []
    for name in ('run.log', 'retroarch.log'):
        path = os.path.join(d, name)
        if os.path.exists(path):
            with open(path, errors='replace') as f:
                found += [tuple(int(g) for g in m.groups())
                          for m in STATUS_RE.finditer(f.read())]
    return found


def wait_logged(p, d, line, timeout=20):
    """Until the logs hold line, and the frames after it are drawn; at
    most timeout seconds."""
    deadline = time.monotonic() + timeout
    while p.poll() is None and time.monotonic() < deadline:
        if line in log_text(d):
            time.sleep(SETTLE)
            return
        time.sleep(0.05)


def last_status(d):
    """The last views status the core logged, or None."""
    found = statuses(d)
    return found[-1] if found else None


def wait_settled(p, d, want, timeout=20):
    """Until the core reports the views status want, and the frames
    that take up its layout are drawn; at most timeout seconds, after
    which the case's checks report what went wrong."""
    deadline = time.monotonic() + timeout
    while p.poll() is None and time.monotonic() < deadline:
        found = last_status(d)
        if found and found[:2] == want:
            time.sleep(SETTLE)
            return
        time.sleep(0.05)


def png_done(path):
    """Whether the PNG at path is written to its end."""
    try:
        with open(path, 'rb') as f:
            f.seek(-12, os.SEEK_END)
            return f.read(12)[4:8] == b'IEND'
    except OSError:
        return False


# What RetroArch logs when the driver asked for is not in the build: it
# goes on with another, whose results would pass for the one asked for.
MISSING_DRIVER_RE = re.compile(r"Couldn't find any video driver named")


def status_errors(d, driver, settings, screens=None):
    """Check the last views status the core logged, and when screens is
    given the SCREENS bit of every status it logged: one that went on
    and off at load is wrong too."""
    found = last_status(d)
    if direct_drawn(d, MISSING_DRIVER_RE):
        return ['video driver "%s" is not in this build' % driver]
    if found is None:
        return ['no "[video_views] presents=" line in the logs']
    want = expected_status(driver, settings)
    errors = []
    if found[:2] != want:
        errors.append('status presents=%d stereo=%d, want presents=%d '
                      'stereo=%d' % (found[0], found[1], want[0], want[1]))
    seen = [s[3] for s in statuses(d)]
    if screens is not None and any(s != screens for s in seen):
        errors.append('status screens=%s, want screens=%d'
                      % (','.join(str(s) for s in seen), screens))
    return errors


def log_text(d):
    """A case's run.log and retroarch.log, one after the other."""
    text = ''
    for name in ('run.log', 'retroarch.log'):
        path = os.path.join(d, name)
        if os.path.exists(path):
            with open(path, errors='replace') as f:
                text += f.read()
    return text


def hdr_env():
    """The environment that loads the fake HDR10 layer, built the first
    time it is asked for."""
    with HDR_LAYER_LOCK:
        if not HDR_LAYER_ENV:
            spec = importlib.util.spec_from_file_location(
                'vulkan_hdr_ui_pass', HDR_UI_PASS)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            d = tempfile.mkdtemp(prefix='ra-e2e-hdr-')
            HDR_LAYER_DIRS.append(d)
            manifest = mod.build_layer(pathlib.Path(d))
            HDR_LAYER_ENV.update(VK_ADD_LAYER_PATH=str(manifest.parent),
                                 VK_INSTANCE_LAYERS='VK_LAYER_RA_fake_hdr')
        return dict(HDR_LAYER_ENV)


def hdr_passes(name):
    """(game, composite) from HDR_PASSES for a run named name; None for
    a pass it does not check."""
    found = [k for k in HDR_PASSES
             if name == k or name.startswith(k + '-')]
    return HDR_PASSES[max(found, key=len)] if found else (None, None)


def hdr_errors(d, game, composite):
    """A run with HDR10 output must have it (on Xvfb the fake layer logs
    its HDR10 swapchain), log no validation error and log each pass's
    mode once, not per frame (no case changes it mid-run). game and
    composite say whether it takes the HDR game pass and composite;
    None checks neither way."""
    text = log_text(d)
    errors = []
    if XVFB and 'fake-hdr: HDR10 swapchain' not in text:
        errors.append('no HDR10 swapchain was created (fell back to SDR?)')
    vuids = {}
    for line in text.splitlines():
        if 'Validation Error' in line:
            m = re.search(r'\[ (\S+) \]', line)
            vuid = m.group(1) if m else line.strip()[:200]
            vuids[vuid] = vuids.get(vuid, 0) + 1
    if vuids:
        errors.append('%d validation error(s)' % sum(vuids.values()))
        errors += ['  %s x%d' % (v, vuids[v]) for v in sorted(vuids)]
    for line, want, what in ((GAME_PASS, game, 'game pass'),
                             (COMPOSITE_PASS, composite, 'composite')):
        if want is not None and (line in text) != want:
            errors.append('%s HDR %s' % ('no' if want else 'an', what))
        modes = re.findall(re.escape(line) + r' mode (\d+)', text)
        for mode in sorted(set(modes)):
            if modes.count(mode) > 1:
                errors.append('"%s mode %s" logged %d times'
                              % (line, mode, modes.count(mode)))
    return errors


# Headless gamescope still opens the display, session bus and runtime
# dir it inherits; give it none. Short and private: a socket path holds
# 108 bytes, so not under the case directory.
DESKTOP_ENV = ('DISPLAY', 'WAYLAND_DISPLAY', 'WAYLAND_SOCKET',
               'DBUS_SESSION_BUS_ADDRESS', 'XAUTHORITY')


def isolated_env():
    env = {k: v for k, v in os.environ.items() if k not in DESKTOP_ENV}
    env['XDG_RUNTIME_DIR'] = tempfile.mkdtemp(
        prefix='ra-e2e-', dir=os.environ.get('RA_E2E_RUNTIME_BASE', '/tmp'))
    # Unset means autolaunch, not no session bus.
    env['DBUS_SESSION_BUS_ADDRESS'] = 'unix:path=' + os.path.join(
        env['XDG_RUNTIME_DIR'], 'no-session-bus')
    return env


XVFB = WINE or os.environ.get('RA_E2E_XVFB') == '1'
# samples/gfx/xv_standin's library, for the xvideo driver on Xvfb
XV_STANDIN = os.environ.get('RA_E2E_XV_STANDIN')
# Cases run side by side, each on an Xvfb of its own, four at a time:
# most of a case is waiting, so more than the cores still gains. One at
# a time in gamescope, whose screenshot names one display. Under Wine
# each worker has a prefix of its own: cases sharing a wineserver move
# each other's pointer. RA_E2E_JOBS sets how many.
JOBS = int(os.environ.get('RA_E2E_JOBS') or 0) or (4 if XVFB else 1)
WINE_PREFIXES = []
WINE_COPIES = []
WINE_WORKER = threading.local()
ONLY = os.environ.get('RA_E2E_ONLY')


def wanted(name):
    """Whether RA_E2E_ONLY lets case @name run."""
    return not ONLY or re.search(ONLY, name) is not None


def wine_prefixes():
    """Gives each worker a copy of the prepared prefix, or the prefix
    itself when there is one worker. The copies go in WINE_COPIES."""
    base = os.environ.get('WINEPREFIX') or os.path.expanduser('~/.wine')
    if JOBS == 1:
        WINE_PREFIXES.append(base)
        return
    prepared = os.path.isdir(base)
    if prepared:
        subprocess.run(['wineserver', '-w'],
                       env=dict(os.environ, WINEPREFIX=base))
    for _ in range(JOBS):
        p = os.path.join(tempfile.mkdtemp(prefix='ra-e2e-wine-'), 'prefix')
        WINE_COPIES.append(p)
        # With no prefix prepared, Wine makes each worker its own.
        if prepared:
            shutil.copytree(base, p, symlinks=True)
        WINE_PREFIXES.append(p)


def wine_prefix():
    """This worker's prefix."""
    if not hasattr(WINE_WORKER, 'prefix'):
        with TAKEN_LOCK:
            WINE_WORKER.prefix = WINE_PREFIXES.pop()
    return WINE_WORKER.prefix


def free_display():
    """An X display number nothing serves, kept for the caller until
    release_display()."""
    with TAKEN_LOCK:
        for n in range(90, 190):
            if n not in TAKEN_DISPLAYS and \
                    not os.path.exists('/tmp/.X11-unix/X%d' % n) and \
                    not os.path.exists('/tmp/.X%d-lock' % n):
                TAKEN_DISPLAYS.add(n)
                return n
    raise RuntimeError('no free X display')


# Drivers whose fullscreen window a window manager sizes: SDL 1.2's
# (SDL2's sdl12-compat) stays 1x1 without one.
WM_DRIVERS = ('sdl',)


def wm_ready_probe(timeout=10):
    """Require the WM to manage a new client, not just publish its identity."""
    x11 = ctypes.CDLL('libX11.so.6')
    pointer, window_type = ctypes.c_void_p, ctypes.c_ulong
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = pointer
    x11.XDefaultRootWindow.argtypes = [pointer]
    x11.XDefaultRootWindow.restype = window_type
    x11.XCreateSimpleWindow.argtypes = [
        pointer, window_type, ctypes.c_int, ctypes.c_int,
        ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, window_type, window_type]
    x11.XCreateSimpleWindow.restype = window_type
    for name in ('XMapWindow', 'XDestroyWindow'):
        getattr(x11, name).argtypes = [pointer, window_type]
    for name in ('XFlush', 'XCloseDisplay'):
        getattr(x11, name).argtypes = [pointer]
    display = x11.XOpenDisplay(os.environ['DISPLAY'].encode())
    if not display:
        return 1
    window = x11.XCreateSimpleWindow(
        display, x11.XDefaultRootWindow(display), 0, 0, 32, 32, 0, 0, 0)
    try:
        x11.XMapWindow(display, window)
        x11.XFlush(display)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = subprocess.run(
                ['xprop', '-id', hex(window), 'WM_STATE'],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                timeout=3, text=True)
            if state.returncode == 0 and 'window state: Normal' in state.stdout:
                return 0
            time.sleep(0.02)
        return 1
    finally:
        x11.XDestroyWindow(display, window)
        x11.XCloseDisplay(display)


def wm_start(env, starts=3):
    """openbox on env's display. One that exits before it is ready is
    started again, until the deadline."""
    deadline = time.monotonic() + 30
    why = 'not ready in time'
    for _ in range(starts):
        log = tempfile.TemporaryFile()
        p = subprocess.Popen(['openbox'], stdout=subprocess.DEVNULL,
                             stderr=log, env=env, start_new_session=True)
        try:
            while p.poll() is None and time.monotonic() < deadline:
                # A probe that is slow to answer on a loaded runner, with
                # the other cases' servers and players beside it, is not
                # an answer: ask again until the deadline.
                try:
                    probe = subprocess.run(
                        ['xprop', '-root', '_NET_SUPPORTING_WM_CHECK'],
                        env=env, stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL, timeout=5, text=True)
                except subprocess.TimeoutExpired:
                    continue
                if probe.returncode == 0 and 'window id #' in probe.stdout:
                    ready = subprocess.run(
                        [sys.executable, os.path.abspath(__file__),
                         '--wm-ready'], env=env,
                        timeout=max(0.1, deadline - time.monotonic()))
                    if ready.returncode == 0 and p.poll() is None:
                        log.close()
                        return p
                time.sleep(0.05)
            code = p.poll()
            if code is None:
                raise RuntimeError('openbox did not become ready on ' +
                                   env['DISPLAY'])
            try:
                log.seek(0)
                said = log.read()[-500:].decode(errors='replace').strip()
            except (OSError, AttributeError, TypeError):
                said = ''
            why = 'exited with %s%s' % (code, ': ' + said if said else '')
        except BaseException:
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
            raise
        finally:
            log.close()
        if time.monotonic() >= deadline:
            break
    raise RuntimeError('openbox did not become ready on %s: %s' %
                       (env['DISPLAY'], why))


def release_display(display):
    with TAKEN_LOCK:
        TAKEN_DISPLAYS.discard(int(display.lstrip(':')))


def xvfb_start(env, depth=24):
    """An Xvfb of the window's size: its process and DISPLAY."""
    n = free_display()
    # A client disconnect must not reset the server during startup.
    p = subprocess.Popen(['Xvfb', ':%d' % n, '-screen', '0',
                          '%dx%dx%d' % (W, H, depth), '-nolisten', 'tcp',
                          '-noreset'],
                         stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, env=env,
                         start_new_session=True)
    deadline = time.time() + 10
    while (p.poll() is None and time.time() < deadline
           and not os.path.exists('/tmp/.X11-unix/X%d' % n)):
        time.sleep(0.1)
    time.sleep(0.5)
    if p.poll() is not None or not os.path.exists('/tmp/.X11-unix/X%d' % n):
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        release_display(':%d' % n)
        raise RuntimeError('Xvfb did not become ready on :%d' % n)
    return p, ':%d' % n


def gamescope_shot(d, env, name='menu.png'):
    """What gamescope shows, menu included, in d/name."""
    out = os.path.join(d, name)
    if XVFB:
        try:
            # PNG24: read_png takes 8-bit RGB, not a palette.
            subprocess.run(['import', '-display', env['DISPLAY'], '-window',
                            'root', 'PNG24:' + out], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=20)
        except (OSError, subprocess.TimeoutExpired):
            return None
        return out if os.path.exists(out) else None
    try:
        subprocess.run(['gamescopectl', 'screenshot', out],
                       env=dict(env, GAMESCOPE_WAYLAND_DISPLAY='gamescope-0'),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return None
    deadline = time.time() + 5
    while time.time() < deadline and not os.path.exists(out):
        time.sleep(0.5)
    if not os.path.exists(out):
        return None
    time.sleep(1)
    return out


# Xlib's handler for the errors window_errors() can meet (a window gone,
# a point off it), which would otherwise end the runner. Global: Xlib
# keeps the pointer.
X_IGNORE_ERRORS = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p,
                                   ctypes.c_void_p)(lambda dpy, err: 0)


class XImage(ctypes.Structure):
    """Xlib's XImage, up to its channel masks."""
    _fields_ = [('width', ctypes.c_int), ('height', ctypes.c_int),
                ('xoffset', ctypes.c_int), ('format', ctypes.c_int),
                ('data', ctypes.c_void_p), ('byte_order', ctypes.c_int),
                ('bitmap_unit', ctypes.c_int),
                ('bitmap_bit_order', ctypes.c_int),
                ('bitmap_pad', ctypes.c_int), ('depth', ctypes.c_int),
                ('bytes_per_line', ctypes.c_int),
                ('bits_per_pixel', ctypes.c_int),
                ('red_mask', ctypes.c_ulong), ('green_mask', ctypes.c_ulong),
                ('blue_mask', ctypes.c_ulong)]


def window_errors(display, points):
    """Points (x, y, colour) on screen 2's window on the depth-30 Xvfb
    display, read as the 10-bit codes its swapchain wrote there: each
    colour is wanted PQ-encoded, as the main window's HDR10 has it, or
    as sRGB codes for a point (x, y, colour, 'sdr')."""
    x11 = ctypes.CDLL('libX11.so.6')
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XSetErrorHandler.argtypes = [ctypes.c_void_p]
    x11.XDefaultRootWindow.restype = ctypes.c_ulong
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x11.XQueryTree.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
        ctypes.POINTER(ctypes.c_uint)]
    x11.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                               ctypes.POINTER(ctypes.c_char_p)]
    x11.XGetImage.restype = ctypes.POINTER(XImage)
    x11.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int,
                              ctypes.c_int, ctypes.c_uint, ctypes.c_uint,
                              ctypes.c_ulong, ctypes.c_int]
    x11.XGetPixel.restype = ctypes.c_ulong
    x11.XGetPixel.argtypes = [ctypes.POINTER(XImage), ctypes.c_int,
                              ctypes.c_int]
    x11.XDestroyImage.argtypes = [ctypes.POINTER(XImage)]
    x11.XFree.argtypes = [ctypes.c_void_p]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    x11.XSetErrorHandler(ctypes.cast(X_IGNORE_ERRORS, ctypes.c_void_p))
    dpy = x11.XOpenDisplay(display.encode())
    if not dpy:
        return ["can't open %s for screen 2's window" % display]
    errors = []
    try:
        unused, kids = ctypes.c_ulong(), ctypes.POINTER(ctypes.c_ulong)()
        count, win = ctypes.c_uint(), None
        if x11.XQueryTree(dpy, x11.XDefaultRootWindow(dpy),
                          ctypes.byref(unused), ctypes.byref(unused),
                          ctypes.byref(kids), ctypes.byref(count)):
            for i in range(count.value):
                name = ctypes.c_char_p()
                if x11.XFetchName(dpy, kids[i], ctypes.byref(name)):
                    if name.value == SCREEN2_WINDOW:
                        win = kids[i]
                    x11.XFree(name)
            if kids:
                x11.XFree(kids)
        if win is None:
            return ['no window named "%s"' % SCREEN2_WINDOW.decode()]
        for point in points:
            x, y, colour = point[:3]
            # AllPlanes, ZPixmap
            img = x11.XGetImage(dpy, win, x, y, 1, 1,
                                ctypes.c_ulong(-1).value, 2)
            if not img:
                errors.append("screen 2's window has no pixel at (%d,%d)"
                              % (x, y))
                continue
            px = x11.XGetPixel(img, 0, 0)
            masks = (img.contents.red_mask, img.contents.green_mask,
                     img.contents.blue_mask)
            x11.XDestroyImage(img)
            if not all(masks) or any(m // (m & -m) != 1023 for m in masks):
                return errors + ["screen 2's window is not 10-bit"]
            got = tuple((px & m) // (m & -m) for m in masks)
            if point[3:] == ('sdr',):
                want = tuple(int(c * 1023 / 255.0 + 0.5) for c in colour)
            else:
                want = srgb8_to_pq10(colour)
            if any(abs(g - e) > WINDOW_TOLERANCE for g, e in zip(got, want)):
                errors.append("screen 2's window (%d,%d) got codes %s want %s"
                              % (x, y, got, want))
    finally:
        x11.XCloseDisplay(dpy)
    return errors


def screen_window_shots(d, env):
    """Each screen's own window on the case's Xvfb, as import reads it,
    in d/window-N.png, and the window tree in d/xwininfo.txt."""
    try:
        tree = subprocess.run(['xwininfo', '-root', '-tree'], env=env,
                              stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL,
                              universal_newlines=True, timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        tree = ''
    with open(os.path.join(d, 'xwininfo.txt'), 'w') as f:
        f.write(tree)
    for i, wid in enumerate(SCREEN_WINDOW_RE.findall(tree)):
        try:
            subprocess.run(['import', '-display', env['DISPLAY'], '-window',
                            wid, 'PNG24:' + os.path.join(
                                d, 'window-%d.png' % i)],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=20)
        except (OSError, subprocess.TimeoutExpired):
            pass


# Rounds of screenshots a pointer case takes before it gives up on the
# cursor showing where its check wants it.
POINTER_ROUNDS = 5


def pointer_shot(d, env, name, seq, point, before):
    """A screenshot in d/name, and its rows, once the menu has drawn the
    pointer put at a window point: white pixels within POINTER_REACH of
    it changed since @before, the rows of the screenshot before it (none:
    the first, taken as it comes). A loaded runner can show a frame from
    before the move for seconds; this waits on the screen, not a clock."""
    out = os.path.join(d, name)
    if not move_pointer(d, next(seq), point):
        return None, None
    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            os.remove(out)
        except OSError:
            pass
        if not gamescope_shot(d, env, name):
            return None, None
        try:
            w, h, bpp, rows = read_png(out)
        except (OSError, ValueError):
            return None, None
        if before is None:
            return out, rows
        if w == W and h == H and cursor_near(rows, before, bpp, point):
            return out, rows
    return None, None


def move_pointer(d, n, point):
    """Has the case's pointer helper put the pointer at a window point."""
    tmp = os.path.join(d, 'pointer.tmp')
    with open(tmp, 'w') as f:
        f.write('%d %d %d\n' % ((n,) + tuple(point)))
    os.replace(tmp, os.path.join(d, 'pointer.at'))
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            with open(os.path.join(d, 'pointer.done')) as f:
                if f.read().split() == [str(n)]:
                    return True
        except OSError:
            pass
        time.sleep(0.1)
    return False


def retroarch_pid(gamescope_pid, cfg):
    """The RetroArch gamescope runs with config cfg, or None: the one
    whose arguments start with it, not the shell or gamescope's reaper
    that pass it on. A wrapper script that execs RetroArch counts."""
    parents = {}
    for p in os.listdir('/proc'):
        try:
            with open('/proc/%s/stat' % p) as f:
                parents[int(p)] = int(f.read().rsplit(')', 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            continue
    ours, found = {gamescope_pid}, True
    while found:
        found = False
        for pid, ppid in parents.items():
            if ppid in ours and pid not in ours:
                ours.add(pid)
                found = True
    for pid in ours:
        try:
            with open('/proc/%d/cmdline' % pid, 'rb') as f:
                argv = f.read().split(b'\0')
        except OSError:
            continue
        if argv[1:3] == [b'--config', cfg.encode()]:
            return pid
    return None


def gpu_mib(pid):
    """GPU memory pid holds, in MiB: drm fdinfo's GTT and VRAM over its
    unique clients. None without a client."""
    d = '/proc/%d/fdinfo' % pid
    seen, kib = set(), 0
    try:
        fds = os.listdir(d)
    except OSError:
        return None
    for fd in fds:
        try:
            with open(os.path.join(d, fd)) as f:
                kv = dict(line.split(':', 1) for line in f if ':' in line)
        except OSError:
            continue
        kv = {k.strip(): v.split() for k, v in kv.items()}
        client = (tuple(kv.get('drm-pdev', ())),
                  tuple(kv.get('drm-client-id', ())))
        if not client[1] or client in seen:
            continue
        seen.add(client)
        for key in ('drm-memory-gtt', 'drm-memory-vram'):
            v = kv.get(key)
            if v:
                unit = v[1] if len(v) > 1 else ''
                kib += int(v[0]) * {'': 1.0 / 1024, 'KiB': 1, 'MiB': 1024,
                                    'GiB': 1 << 20}.get(unit, 1)
    return kib / 1024.0 if seen else None


def case_dir(root, driver, name, hw='off'):
    """The directory a case runs in."""
    if hw != 'off':
        name += '-hw-' + hw
    return os.path.realpath(os.path.join(root, driver + '-' + name))


def run_case(retroarch, root, driver, case, hw='off', menu=False,
             opts=None, args=(), gpu=None, pointer=(), env_extra=None,
             pointer_check=None, raw=False, passes=None, after=None,
             window=(), until=None, aim=(), aimed=None, files=None,
             screens=None, screenshot=True, window_shots=False):
    """gpu, a list, gets RetroArch's GPU memory in MiB sampled while it
    settles. pointer, window points, has the menu open with the pointer
    at each in turn, in d/pointer-N.png; pointer_check(d) gives the
    errors of a round of those, which is taken again, at most
    POINTER_ROUNDS times, until it has none: a frame the menu had not
    drawn the cursor in yet is not the code's failure. env_extra is
    added to RetroArch's environment. With HDR10 output raw reads the
    points as read_png() does, and passes, (game, composite), are what
    hdr_errors() checks; hdr_passes() gives them by default. The
    screenshot waits for the log line after, when given. window, points
    on screen 2's window, are checked by window_errors() on Xvfb, with
    RetroArch still up. until, a regex, keeps the case running until the
    logs have it. aim, window points, has the pointer at each in turn
    with the menu closed, until the core logs a light gun line after the
    move; aimed, a list, gets for each move made how many light gun lines
    the core had logged before it. files, {path under the case directory:
    text}, are there when it starts. screens, given, is the SCREENS bit
    every status must have. screenshot False takes none: the statuses
    alone are checked. window_shots True takes the screens' own windows
    on Xvfb as screen_window_shots() does, before the screenshot."""
    name, mapopt, settings, points = case
    hdr = HDR and driver == 'vulkan'
    d = case_dir(root, driver, name, hw)
    if os.path.commonpath([root, d]) != root or d == root:
        raise RuntimeError('refusing to touch ' + d)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, 'shots'))
    port = free_port()
    cfg = os.path.join(d, 'retroarch.cfg')
    write_cfg(cfg, d, driver, settings, port)
    with open(os.path.join(d, 'opts.cfg'), 'w') as f:
        f.write('video_views_test_map = "%s"\n' % mapopt)
        f.write('video_views_test_hw = "%s"\n' % hw)
        for k in sorted(opts or {}):
            f.write('%s = "%s"\n' % (k, opts[k]))
    for path in sorted(files or {}):
        os.makedirs(os.path.dirname(os.path.join(d, path)), exist_ok=True)
        with open(os.path.join(d, path), 'w', newline='') as f:
            f.write(files[path])

    # gamescope runs under isolated_env() below, so it can't probe the
    # desktop's display, bus or PipeWire. RetroArch, run as its child,
    # still gets whatever DISPLAY gamescope sets for it, so this guard
    # refuses if that ever turns out to be the desktop's, and points
    # RetroArch's Wayland at a socket that does not exist (unset,
    # libwayland falls back to wayland-0).
    helper = ''
    if pointer or aim:
        helper = ' %s %s --pointer %s &' % (
            shlex.quote(sys.executable),
            shlex.quote(os.path.abspath(__file__)), shlex.quote(d))
    guard = ('case "$DISPLAY" in ""|:0) echo "refusing DISPLAY=$DISPLAY" >&2;'
             ' exit 99;; esac;' + helper +
             ' WAYLAND_DISPLAY=video-views-e2e-no-socket; export WAYLAND_DISPLAY;'
             ' exec "$0" "$@"')
    cmd = ['gamescope', '--backend', 'headless',
           '-w', str(W), '-h', str(H), '-W', str(W), '-H', str(H),
           '-r', '60'] + \
        (['--hdr-enabled', '--hdr-debug-force-support'] if hdr else []) + \
        ['--', 'sh', '-c', guard,
         retroarch, '--config', cfg, '-L', CORE, '--verbose'] + list(args)
    env = isolated_env()
    if hdr:
        env.update(hdr_env() if XVFB else {'ENABLE_GAMESCOPE_WSI': '1'})
    env.update(env_extra or {})
    # Xvfb has no XVideo adaptor: the stand-in's, preloaded
    if driver == 'xvideo' and XV_STANDIN:
        env['LD_PRELOAD'] = XV_STANDIN
    xvfb = wm = None
    if XVFB:
        xvfb, env['DISPLAY'] = xvfb_start(env, 30 if hdr else 24)
        if driver in WM_DRIVERS:
            # sdl12-compat otherwise creates a GL renderer on Xvfb.
            env['SDL_RENDER_DRIVER'] = 'software'
            env['SDL_FRAMEBUFFER_ACCELERATION'] = '0'
            try:
                wm = wm_start(env)
            except Exception:
                xvfb.terminate()
                xvfb.wait(5)
                release_display(env['DISPLAY'])
                shutil.rmtree(env['XDG_RUNTIME_DIR'], ignore_errors=True)
                raise
        cmd = cmd[cmd.index('--') + 1:]
    if WINE:
        env['WINEPREFIX'] = wine_prefix()
        env['WINEDEBUG'] = '-all'
        # A D3DCompiler_47.dll next to the binary (vkd3d-utils, say)
        # over Wine's own.
        env['WINEDLLOVERRIDES'] = 'd3dcompiler_47=n,b'
        cmd = cmd[:cmd.index(retroarch)] + [
            'wine', retroarch, '--config', winpath(cfg), '-L',
            winpath(CORE), '--verbose'] + [
                a.replace('=/', '=Z:/') for a in args]
    log = open(os.path.join(d, 'run.log'), 'w')
    p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                         env=env, start_new_session=True)
    shot, window_got = None, []
    try:
        if not wait_for_views(p, d):
            startup_stack(p, d)
        if gpu is None:
            wait_settled(p, d, expected_status(driver, settings))
        if after:
            wait_logged(p, d, after)
        if until and wait_for_views(p, d, 20, until):
            # The frames after the line take up what it logged.
            time.sleep(SETTLE)
        for i, point in enumerate(aim):
            before = len(GUN_RE.findall(log_text(d)))
            if not move_pointer(d, i, point):
                break
            aimed.append(before)
            # A slow frame logs late: the next move counts after it.
            deadline = time.time() + 10
            while (len(GUN_RE.findall(log_text(d))) <= before
                   and time.time() < deadline and p.poll() is None):
                time.sleep(0.2)
        settle = time.time() + 5
        while gpu is not None and time.time() < settle:
            time.sleep(0.5)
            pid = retroarch_pid(p.pid, cfg)
            mib = gpu_mib(pid) if pid else None
            if mib is not None:
                gpu.append(mib)
        if window_shots and XVFB:
            screen_window_shots(d, env)
        if pointer:
            send('MENU_TOGGLE', port)
            time.sleep(3)
            # The menu takes the pointer once it has seen it move.
            seq = itertools.count()
            shot, rows = pointer_shot(d, env, 'pointer-park.png', seq,
                                      POINTER_PARK, None)
            for _ in range(POINTER_ROUNDS if pointer_check else 1):
                for i, point in enumerate(pointer):
                    if shot:
                        shot, rows = pointer_shot(
                            d, env, 'pointer-%d.png' % i, seq, point, rows)
                if not shot or not pointer_check or not pointer_check(d):
                    break
            if not shot:
                shot = None
        elif menu:
            send('MENU_TOGGLE', port)
            time.sleep(3)
            shot = gamescope_shot(d, env)
        elif screenshot:
            send('SCREENSHOT', port)
        deadline = time.time() + 10
        while screenshot and time.time() < deadline and not shot:
            time.sleep(0.05)
            pngs = [x for x in os.listdir(os.path.join(d, 'shots'))
                    if x.endswith('.png')]
            if pngs and png_done(os.path.join(d, 'shots', pngs[0])):
                shot = os.path.join(d, 'shots', pngs[0])
        if window and XVFB:
            window_got = window_errors(env['DISPLAY'], window)
        elif window:
            print('[e2e] %s: window points not checked' % name, flush=True)
        send('QUIT', port)
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            pass
    finally:
        if pointer or aim:
            open(os.path.join(d, 'pointer.quit'), 'w').close()
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGTERM)
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
        log.close()
        if wm:
            wm.terminate()
            try:
                wm.wait(5)
            except subprocess.TimeoutExpired:
                wm.kill()
        if xvfb:
            xvfb.terminate()
            try:
                xvfb.wait(5)
            except subprocess.TimeoutExpired:
                xvfb.kill()
            release_display(env['DISPLAY'])
        runtime_dir = env['XDG_RUNTIME_DIR']
        if os.path.basename(runtime_dir).startswith('ra-e2e-'):
            shutil.rmtree(runtime_dir, ignore_errors=True)

    errors = status_errors(d, driver, settings, screens) + window_got
    if hdr:
        errors += hdr_errors(d, *(passes or hdr_passes(name)))
    if not screenshot:
        return errors
    if not shot:
        return errors + ['no screenshot (see %s)'
                         % os.path.join(d, 'run.log')]
    w, h, bpp, rows = read_png(shot, raw)
    if (w, h) != (W, H):
        return errors + ['screenshot is %dx%d, want %dx%d' % (w, h, W, H)]
    tolerance = HDR_TOLERANCE if png_depth(shot) == 16 and not raw \
        else TOLERANCE
    for x, y, want in points:
        got = tuple(rows[y][x * bpp:x * bpp + 3])
        if any(abs(g - e) > tolerance for g, e in zip(got, want)):
            errors.append('(%d,%d) got %s want %s' % (x, y, got, want))
    return errors


def noise(x, y):
    """The core's noise_colour()."""
    h = (x * 0x9E3779B1 + y * 0x85EBCA77) & 0xffffffff
    h ^= h >> 15
    h = (h * 0x2C1B3C6D) & 0xffffffff
    h ^= h >> 12
    return NOISE_COLOURS[h % 7]


def rgb_row(rows, bpp, y, x, w):
    """w pixels of row y from column x, as RGB bytes."""
    seg = bytearray(rows[y][x * bpp:(x + w) * bpp])
    if bpp == 4:
        del seg[3::4]
    return seg


def white_boxes(rows, bpp):
    """(x, y, width, height) of each white rectangle, from its top-left
    pixel: the views' markers, which no noise colour matches."""
    white = bytes(WHITE)

    def is_white(x, y):
        return x >= 0 and y >= 0 and rows[y][x * bpp:x * bpp + 3] == white

    boxes = []
    for y, row in enumerate(rows):
        i = row.find(white)
        while i >= 0:
            x = i // bpp
            if (i % bpp == 0 and not is_white(x - 1, y)
                    and not is_white(x, y - 1)):
                w = h = 1
                while x + w < W and is_white(x + w, y):
                    w += 1
                while y + h < H and is_white(x, y + h):
                    h += 1
                boxes.append((x, y, w, h))
            i = row.find(white, i + 1)
    return boxes


def view_mismatches(rows, bpp, box, view):
    """Pixels of view, at the scale of the marker box from its corner,
    that are not their source pixel; None when box can't be its
    marker."""
    bx, by, bw, bh = box
    vx, vy, vw, vh = view
    if bw != bh or bw % 8:
        return None
    k = bw // 8
    if bx + vw * k > W or by + vh * k > H:
        return None
    bad = 0
    for sy in range(vh):
        want = b''.join(
            bytes(WHITE if sx < 8 and sy < 8 else noise(vx + sx, vy + sy)) * k
            for sx in range(vw))
        for r in range(k):
            got = rgb_row(rows, bpp, by + sy * k + r, bx, vw * k)
            if got != want:
                bad += sum(got[i:i + 3] != want[i:i + 3]
                           for i in range(0, len(want), 3))
    return bad


def exact_errors(rows, bpp, views):
    """Each view must show every one of its pixels at some integer scale,
    found by its marker, and the screens, one view each, stack in order
    from the top."""
    boxes = white_boxes(rows, bpp)
    errors = []
    tops = []
    for view in views:
        found = [(view_mismatches(rows, bpp, b, view), b) for b in boxes]
        found = [(m, b) for m, b in found if m is not None]
        exact = [b for m, b in found if m == 0]
        if exact:
            tops.append(exact[0][1])
        else:
            errors.append('view %s: %s' % (
                view, 'no marker found' if not found else
                'best match has %d wrong pixels' % min(found)[0]))
    if not errors and tops != sorted(tops):
        errors.append('screens out of order: tops at %s' % tops)
    return errors


def log_text(d):
    """A case's logs, its run's and RetroArch's."""
    text = ''
    for name in ('run.log', 'retroarch.log'):
        path = os.path.join(d, name)
        if os.path.exists(path):
            with open(path, errors='replace') as f:
                text += f.read()
    return text


def copy_path(d):
    """How glcore's log says it copies views out of the frame."""
    found = None
    for name in ('run.log', 'retroarch.log'):
        path = os.path.join(d, name)
        if os.path.exists(path):
            with open(path, errors='replace') as f:
                for m in COPY_RE.finditer(f.read()):
                    found = m.group(1)
    return found


def direct_drawn(d, regex):
    """Whether the log says the frame or its views were drawn straight
    from the frame's texture, by regex."""
    for name in ('run.log', 'retroarch.log'):
        path = os.path.join(d, name)
        if os.path.exists(path):
            with open(path, errors='replace') as f:
                if regex.search(f.read()):
                    return True
    return False


def run_exact_case(retroarch, root, driver, case, suffix='',
                   env_extra=None, path=None, preset=None, exact=True,
                   in_place=False):
    """The errors, and the screenshot's rows for comparing runs. path is
    how glcore must say it copies views. preset, a preset's path, has the
    views copied; without one a driver in DIRECT_DRIVERS must draw them
    straight from the frame. exact checks each view pixel for pixel.
    in_place: the preset can read views where they lie."""
    name, mapopt, opts, hw = case
    name += suffix
    settings = EXACT_SETTINGS if exact else SMOOTH_SETTINGS
    args = ()
    if preset:
        settings = dict(settings, video_shader_enable='true')
        args = ('--set-shader=' + preset,)
    errors = run_case(retroarch, root, driver,
                      (name, mapopt, settings, []), hw, opts=opts,
                      args=args, env_extra=env_extra)
    d = case_dir(root, driver, name, hw)
    if path and copy_path(d) != path:
        errors.append('views copied by %s, want %s' % (copy_path(d), path))
    if mapopt == 'none':
        regex, drivers, what = WHOLE_RE, WHOLE_DRIVERS, 'frame'
    else:
        regex, drivers, what = DIRECT_RE, DIRECT_DRIVERS, 'views'
    direct = direct_drawn(d, regex)
    if direct != (driver in drivers and not preset):
        errors.append('%s %s straight from the frame' %
                      (what, 'drawn' if direct else 'not drawn'))
    written = bool(preset) and mapopt != 'none' and hw == 'off' \
        and driver in WRITE_SW_VIEWS \
        and not (in_place and driver in SW_IN_PLACE_DRIVERS)
    if driver in WRITE_SW_VIEWS and direct_drawn(d, WRITE_SW_RE) != written:
        errors.append('views %s straight in'
                      % ('not written' if written else 'written'))
    if driver == 'd3d9_hlsl' and hw == 'd3d9' and mapopt != 'none' \
            and direct_drawn(d, D3D9_HW_COPY_RE) != bool(preset):
        errors.append('views %s out of the core\'s texture'
                      % ('copied' if not preset else 'not copied'))
    shots = sorted(x for x in os.listdir(os.path.join(d, 'shots'))
                   if x.endswith('.png'))
    if not shots:
        return errors, None
    w, h, bpp, rows = read_png(os.path.join(d, 'shots', shots[0]))
    if (w, h) != (W, H):
        return errors, None
    rows = [bytes(rgb_row(rows, bpp, y, 0, W)) for y in range(H)]
    if exact:
        errors += exact_errors(rows, 3, EXACT_VIEWS[mapopt])
    return errors, rows


def rows_differ(a, b, tolerance):
    """How many pixels of two screenshots' RGB rows differ by more than
    tolerance in a channel."""
    bad = 0
    for ra, rb in zip(a, b):
        if ra == rb:
            continue
        for i in range(0, len(ra), 3):
            if any(abs(ra[i + c] - rb[i + c]) > tolerance for c in range(3)):
                bad += 1
    return bad


def max_rgb_p99(rows, bpp):
    """The 99th percentile of max(r, g, b) over a screenshot's pixels."""
    counts = collections.Counter()
    for row in rows:
        counts.update(map(max, row[0::bpp], row[1::bpp], row[2::bpp]))
    total, seen = sum(counts.values()), 0
    for v in sorted(counts):
        seen += counts[v]
        if seen * 100 >= total * 99:
            return v
    return 0


def menu_diff(ref, shot):
    """How many pixels of two menu screenshots, as read_png() gives
    them, differ."""
    w, h, bpp, rows_a = ref
    sw, sh, sbpp, rows_b = shot
    if (sw, sh) != (w, h):
        return w * h
    diff = 0
    for y in range(h):
        if rows_b[y] == rows_a[y] and sbpp == bpp:
            continue
        for x in range(w):
            a = rows_a[y][x * bpp:x * bpp + 3]
            b = rows_b[y][x * sbpp:x * sbpp + 3]
            if any(abs(p - q) > TOLERANCE for p, q in zip(a, b)):
                diff += 1
    return diff


def run_menu_case(retroarch, root, driver, case, brightness=False):
    """The case with the menu open in each of its stereo modes; each
    mode's screen must match the first's, or with brightness be as
    bright, read in raw codes."""
    name, mapopt, settings, modes = case
    errors, images = [], []
    for mode in modes:
        c = ('%s-%s' % (name, mode), mapopt,
             dict(settings, video_stereo_mode=mode), [])
        errors += run_case(retroarch, root, driver, c, menu=True)
        path = os.path.join(root, '%s-%s-%s' % (driver, name, mode),
                            'menu.png')
        try:
            images.append(read_png(path, brightness)
                          if os.path.exists(path) else None)
        except ValueError:
            images.append(None)
    if errors or None in images:
        return errors or ['no menu screenshot']
    if brightness:
        ref = max_rgb_p99(images[0][3], images[0][2])
        for mode, (_, _, bpp, rows) in zip(modes[1:], images[1:]):
            p99 = max_rgb_p99(rows, bpp)
            if abs(p99 - ref) > MENU_BRIGHTNESS_MAX:
                errors.append('stereo mode %s: menu brightness %d, mode '
                              '%s has %d' % (mode, p99, modes[0], ref))
        return errors
    for mode, image in zip(modes[1:], images[1:]):
        diff = menu_diff(images[0], image)
        if diff > MENU_DIFF_MAX:
            errors.append('stereo mode %s: %d pixels differ from mode %s'
                          % (mode, diff, modes[0]))
    return errors


def run_hdr_case(retroarch, root, driver, case):
    """An HDR_CASES case."""
    name, mapopt, settings, points, extra = case
    menu = extra.get('menu', False)
    args = ('--set-shader=' + extra['preset'],) if 'preset' in extra else ()
    knobs = any(k.startswith('FAKE_HDR_') for k in extra.get('env', {}))
    lines = [line for line in extra.get('log', ())
             if XVFB or not (knobs or line.startswith('fake-hdr:'))]
    if len(lines) < len(extra.get('log', ())):
        print('[e2e] %s: log lines that need the fake layer not checked'
              % name, flush=True)
    errors = run_case(retroarch, root, driver,
                      (name, mapopt, settings, points), menu=menu,
                      args=args, env_extra=extra.get('env'),
                      raw=extra.get('raw', False),
                      passes=(extra.get('game', True),
                              extra.get('composite', menu)),
                      after=extra.get('after') if XVFB or not knobs
                      else None,
                      window=extra.get('window', ()))
    text, at = log_text(case_dir(root, driver, name)), 0
    for line in lines:
        at = text.find(line, at)
        if at < 0:
            errors.append('no "%s" in the log after the lines before it'
                          % line)
            break
        at += len(line)
    errors += ['"%s" in the log' % line for line in extra.get('absent', ())
               if line in text]
    return errors


def run_menu_fit_case(retroarch, root, driver, case):
    """RGUI over a picture narrower than the window stays inside it."""
    errors = run_case(retroarch, root, driver, case, menu=True)
    path = os.path.join(root, '%s-%s' % (driver, case[0]), 'menu.png')
    if errors or not os.path.exists(path):
        return errors or ['no menu screenshot']
    w, h, bpp, rows = read_png(path)
    lit = 0
    for x in list(range(MENU_FIT_STRIP)) + list(range(w - MENU_FIT_STRIP, w)):
        for y in range(h):
            if max(rows[y][x * bpp:x * bpp + 3]) > TOLERANCE:
                lit += 1
    if lit:
        errors.append('%d lit pixels in the strips beside the picture' % lit)
    if max(rows[h // 2][w // 2 * bpp:(w // 2 + 40) * bpp]) <= TOLERANCE:
        errors.append('nothing drawn in the middle of the screen')
    return errors


def cursor_pixels(rows, others, bpp):
    """Where one screen has the cursor and the other doesn't: the white
    pixels that differ. The box around the row under the pointer pulses
    between the two as well, in colour."""
    found = []
    for y in range(H):
        if rows[y] == others[y]:
            continue
        for x in range(W):
            a = rows[y][x * bpp:x * bpp + 3]
            if min(a) < 200:
                continue
            b = others[y][x * bpp:x * bpp + 3]
            if any(abs(p - q) > TOLERANCE for p, q in zip(a, b)):
                found.append((x, y))
    return found


def cursor_near(rows, others, bpp, point):
    """Whether cursor_pixels() would find one within POINTER_REACH of
    @point, looking only there."""
    x0 = max(0, point[0] - POINTER_REACH)
    x1 = min(W, point[0] + POINTER_REACH + 1)
    for y in range(max(0, point[1] - POINTER_REACH),
                   min(H, point[1] + POINTER_REACH + 1)):
        a_row, b_row = rows[y], others[y]
        if a_row[x0 * bpp:x1 * bpp] == b_row[x0 * bpp:x1 * bpp]:
            continue
        for x in range(x0, x1):
            a = a_row[x * bpp:x * bpp + 3]
            if min(a) < 200:
                continue
            b = b_row[x * bpp:x * bpp + 3]
            if any(abs(p - q) > TOLERANCE for p, q in zip(a, b)):
                return True
    return False


def box_of(pixels):
    xs, ys = [p[0] for p in pixels], [p[1] for p in pixels]
    return (min(xs), min(ys), max(xs), max(ys))


def pointer_errors(d, areas, points):
    """What is wrong with a pointer case's screenshots in d: each must
    show the cursor at the pointer and at the same place in every other
    eye's area, and nowhere else."""
    images = []
    for i in range(len(points)):
        try:
            images.append(read_png(os.path.join(d, 'pointer-%d.png' % i)))
        except (OSError, ValueError):
            return ['no screenshot with the pointer']
    if any(im[:3] != (W, H, images[0][2]) for im in images):
        return ['a screenshot is not %dx%d' % (W, H)]
    errors = []
    shapes = []
    for i, pt in enumerate(points):
        bpp, rows = images[i][2:]
        home = [a for a in areas
                if a[0] <= pt[0] < a[0] + a[2]
                and a[1] <= pt[1] < a[1] + a[3]][0]
        pixels = cursor_pixels(rows, images[1 - i][3], bpp)
        for area in areas:
            want = (pt[0] - home[0] + area[0], pt[1] - home[1] + area[1])
            near = set(p for p in pixels
                       if max(abs(p[0] - want[0]),
                              abs(p[1] - want[1])) <= POINTER_REACH)
            pixels = [p for p in pixels if p not in near]
            if not near:
                errors.append('pointer (%d,%d): no cursor at (%d,%d)'
                              % (pt + want))
                continue
            box = box_of(near)
            # The pointer is the arrow's tip, or without the assets the
            # middle of a square.
            if not (box[0] - POINTER_TOLERANCE <= want[0]
                    <= box[2] + POINTER_TOLERANCE
                    and box[1] - POINTER_TOLERANCE <= want[1]
                    <= box[3] + POINTER_TOLERANCE):
                errors.append('pointer (%d,%d): cursor in (%d,%d)-(%d,%d), '
                              'want it at (%d,%d)' % (pt + box + want))
            shapes.append((box[0] - want[0], box[1] - want[1],
                           box[2] - want[0], box[3] - want[1]))
        if pixels:
            errors.append('pointer (%d,%d): a cursor in (%d,%d)-(%d,%d)'
                          % (pt + box_of(pixels)))
    # The same cursor about every pointer, in every copy.
    if any(abs(p - q) > 2 for s in shapes for p, q in zip(s, shapes[0])):
        errors.append('cursors differ about the pointer: %s' % shapes)
    return errors


def run_pointer_case(retroarch, root, driver, menu, case):
    """The case's menu with the pointer at each of its points, checked
    by pointer_errors()."""
    name, mapopt, settings, areas, points = case
    c = ('%s-%s' % (name, menu), mapopt,
         dict(POINTER_SETTINGS, **dict(POINTER_MENUS[menu], **settings)), [])
    errors = run_case(retroarch, root, driver, c, pointer=points,
                      pointer_check=lambda d: pointer_errors(d, areas, points))
    if errors:
        return errors
    return pointer_errors(os.path.join(root, '%s-%s' % (driver, c[0])),
                          areas, points)


def run_memory_case(retroarch, root, driver, case):
    """The case with the large maximum and the history preset, and
    RetroArch's peak GPU memory in MiB."""
    name, mapopt, settings, points, limit = case
    gpu = []
    errors = run_case(retroarch, root, driver,
                      (name, mapopt, settings, points), MEMORY_HW[driver],
                      opts={'video_views_test_max': 'large'},
                      args=('--set-shader=' + HISTORY_PRESET,), gpu=gpu)
    peak = max(gpu) if gpu else None
    if peak is None:
        errors.append('no GPU memory reading')
    elif peak > limit:
        errors.append('GPU memory peaked at %d MiB, limit %d'
                      % (peak, limit))
    return errors, peak


def run_screens_rules_case(retroarch, root, driver):
    """The rules cases: which maps the frontend accepts, the screens it
    logs for the full map, and GET_VIDEO_SCREEN_BUFFER known, lending on
    IMAGES_DRIVERS only."""
    images = 1 if driver in IMAGES_DRIVERS else 0
    case = ('screens-rules', 'ds', {'video_stereo_mode': '0'}, [])
    errors = run_case(retroarch, root, driver, case,
                      opts={'video_views_test_screens': 'rules'},
                      until=SCREENS_FULL_RE)
    text = log_text(case_dir(root, driver, case[0]))
    logged = {}
    for name, accepted in SCREENS_CASE_RE.findall(text):
        logged.setdefault(name, []).append(int(accepted))
    for name, want in SCREENS_RULES:
        if want is None:
            want = images
        if name not in logged:
            errors.append('case %s not logged' % name)
        elif any(a != want for a in logged[name]):
            errors.append('case %s accepted=%s, want %d'
                          % (name, logged[name], want))
    known = set(name for name, _ in SCREENS_RULES)
    for name in sorted(set(logged) - known):
        errors.append('case %s not in SCREENS_RULES' % name)
    answers = set(SCREEN_BUFFER_RE.findall(text))
    if answers != {str(images)}:
        errors.append('screen_buffer=%s, want %d' % (sorted(answers), images))
    lines = SCREEN_LOG_RE.findall(text)[-3:]
    if lines != SCREENS_FULL_LOG:
        errors.append('the last screen lines are %s, want %s'
                      % (lines, SCREENS_FULL_LOG))
    if SCREEN_BUFFER_UNSUPPORTED_RE.search(text):
        errors.append('GET_VIDEO_SCREEN_BUFFER logged as unsupported')
    return errors


def run_screens_aim_case(retroarch, root, driver, case):
    """The light gun aimed at the bottom screen's centre with the menu
    closed: on that screen only when it takes input."""
    name, mode, lands, points, extra = case
    extra = dict(extra)
    aim = extra.pop('aim', SCREENS_AIM)
    aimed = []
    errors = run_case(retroarch, root, driver,
                      (name, 'ds', SCREENS_AIM_SETTINGS, points),
                      opts={'video_views_test_screens': mode},
                      aim=aim, aimed=aimed, **extra)
    guns = GUN_RE.findall(log_text(case_dir(root, driver, name)))
    if len(aimed) < len(aim):
        return errors + ['the pointer did not move']
    if len(guns) <= aimed[-1]:
        return errors + ['no light gun line after the aim at the bottom '
                         'screen']
    gun = guns[aimed[-1]]
    offscreen, px, py = int(gun[2]), int(gun[4]), int(gun[5])
    if lands == 'bottom' and not 192 <= py < 384:
        errors.append('the gun at the bottom screen has packed y %d, '
                      'want 192 to 383' % py)
    if lands == 'top' and (offscreen or not 0 <= py < 192):
        errors.append('the gun at the bottom screen is at packed '
                      '(%d, %d) offscreen=%d, want y below 192 on-screen'
                      % (px, py, offscreen))
    # Off-screen, or at the top screen's right edge; not at its centre,
    # where the first aim's line is.
    if lands is None and not (offscreen or px >= 250):
        errors.append('the gun at the bottom screen is at packed '
                      '(%d, %d), want x 250 or more, or off-screen'
                      % (px, py))
    return errors


def run_screens_show_case(retroarch, root, driver, case, screens=None):
    """A screens draw or state case: the window's points, each file the
    case started with left as it was, and screens as run_case() has
    it."""
    name, mode, points, extra = case
    extra = dict(extra)
    solid = extra.pop('solid', points)
    if driver not in OPACITY_DRIVERS:
        points = solid
    mapopt = extra.pop('map', 'ds')
    settings = dict(SCREENS_AIM_SETTINGS, **extra.pop('settings', {}))
    errors = run_case(retroarch, root, driver,
                      (name, mapopt, settings, points),
                      opts={'video_views_test_screens': mode},
                      screens=screens, **extra)
    d = case_dir(root, driver, name)
    for path in sorted(extra.get('files', {})):
        try:
            with open(os.path.join(d, path), newline='') as f:
                found = f.read()
        except OSError:
            found = None
        if found != extra['files'][path]:
            errors.append('%s is %r after the run, want %r'
                          % (path, found, extra['files'][path]))
    return errors


def run_screens_image_case(retroarch, root, driver, case, hw='off'):
    """An image case: the window's points, IMAGES in every status, each
    picture the core sent taken, through a lend unless it copies, and
    its blends, number of pictures, preset, until line and screen window
    where it has them."""
    name, mode, points, extra = case
    extra = dict(extra)
    until = extra.get('until')
    preset = extra.pop('preset', False)
    blends = extra.pop('blends', ())
    handins = extra.pop('handins', None)
    window = extra.pop('window', None)
    extra.pop('drivers', None)
    extra.pop('hw', None)
    opts = dict(extra.pop('opts', {}), video_views_test_screens=mode)
    settings = dict(SCREENS_AIM_SETTINGS, **extra.pop('settings', {}))
    if window:
        extra['window_shots'] = True
        if driver in WINDOW_CONTEXTS:
            settings['video_context_driver'] = WINDOW_CONTEXTS[driver]
    if preset:
        settings['video_shader_enable'] = 'true'
        extra['args'] = ('--set-shader='
                         + os.path.join(HERE, SCREENS_PRESETS[driver]),)
    errors = run_case(retroarch, root, driver,
                      (name, 'ds', settings, points), hw=hw, opts=opts,
                      screens=1, **extra)
    d = case_dir(root, driver, name, hw)
    text = log_text(d)
    if until and not until.search(text):
        errors.append('never logged /%s/' % until.pattern)
    if (preset and driver in DRIVER_PRESETS
            and not direct_drawn(d, PRESET_RE)):
        errors.append('views not drawn through the preset')
    if preset and direct_drawn(d, DIRECT_RE):
        errors.append('views drawn straight from the frame')
    images = [s[4] for s in statuses(d)]
    if any(i != 1 for i in images):
        errors.append('status images=%s, want images=1'
                      % ','.join(str(i) for i in images))
    sent = IMAGE_SENT_RE.findall(text)
    if not sent:
        errors.append('no picture sent')
    refused = [int(s) for s, a in sent if a != '1']
    if refused:
        errors.append('%d maps with the picture refused, serials %s'
                      % (len(refused), sorted(set(refused))))
    if handins is not None and len(sent) != handins:
        errors.append('%d pictures sent, want %d' % (len(sent), handins))
    if opts.get('video_views_test_image') != 'copy' \
            and IMAGE_NOT_LENT_RE.search(text):
        errors.append('the picture was not lent')
    shots = sorted(x for x in os.listdir(os.path.join(d, 'shots'))
                   if x.endswith('.png'))
    if blends and shots:
        w, h, bpp, rows = read_png(os.path.join(d, 'shots', shots[0]))
        if (w, h) != (W, H):
            blends = ()
        for a, b, ca, cb in blends:
            for x, y in (a, b):
                got = tuple(rows[y][x * bpp:x * bpp + 3])
                if any(all(abs(g - e) <= TOLERANCE for g, e in zip(got, c))
                       for c in (ca, cb)):
                    errors.append('(%d,%d) got %s, want a blend of %s and '
                                  '%s' % (x, y, got, ca, cb))
    if window:
        errors += screen_window_errors(d, window)
    return errors


def screen_window_errors(d, colours):
    """What is wrong with the one screen window's shot in d: the centres
    of the quadrants of its picture, the box its lit pixels fill, must
    be in colours."""
    shots = sorted(x for x in os.listdir(d)
                   if x.startswith('window-') and x.endswith('.png'))
    if len(shots) != 1:
        return ['%d screen windows shot, want 1 (see %s)'
                % (len(shots), os.path.join(d, 'xwininfo.txt'))]
    w, h, bpp, rows = read_png(os.path.join(d, shots[0]))
    lit = [(x, y) for y in range(h) for x in range(w)
           if any(v > TOLERANCE for v in rows[y][x * bpp:x * bpp + 3])]
    if not lit:
        return ['the screen window is black']
    x0, y0, x1, y1 = box_of(lit)
    errors = []
    for x, y, want in image_centres((x0, y0, x1 - x0 + 1, y1 - y0 + 1),
                                    colours):
        got = tuple(rows[y][x * bpp:x * bpp + 3])
        if any(abs(g - e) > TOLERANCE for g, e in zip(got, want)):
            errors.append('window (%d,%d) got %s want %s'
                          % (x, y, got, want))
    return errors


def run_screens_menu_case(retroarch, root, driver):
    """The menu with no screen placed: drawn in the window's left third
    and in its right one, and the same as over a frame without views."""
    errors = run_case(retroarch, root, driver, SCREENS_MENU_CASE, menu=True,
                      opts={'video_views_test_screens': 'none_shown'})
    errors += run_case(retroarch, root, driver, SCREENS_MENU_REF, menu=True)
    paths = [os.path.join(case_dir(root, driver, c[0]), 'menu.png')
             for c in (SCREENS_MENU_CASE, SCREENS_MENU_REF)]
    if errors or not all(os.path.exists(p) for p in paths):
        return errors or ['no menu screenshot']
    try:
        shot, ref = [read_png(p) for p in paths]
    except ValueError:
        return ['no menu screenshot']
    w, h, bpp, rows = shot
    diff = menu_diff(ref, shot)
    if diff > MENU_DIFF_MAX:
        errors.append('%d pixels differ from the menu without views' % diff)
    for side, x0 in (('left', 0), ('right', w - w // 3)):
        if not any(max(rows[y][x * bpp:x * bpp + 3]) > TOLERANCE
                   for y in range(0, h, 4)
                   for x in range(x0, x0 + w // 3, 4)):
            errors.append('nothing drawn in the %s third of the menu' % side)
    return errors


def gamescope_root(x11, dpy):
    """dpy's root window if this process runs inside the gamescope that
    owns it, else None: the server names its gamescope on its root."""
    x11.XInternAtom.restype = ctypes.c_ulong
    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                ctypes.c_int]
    x11.XDefaultRootWindow.restype = ctypes.c_ulong
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x11.XGetWindowProperty.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long,
        ctypes.c_long, ctypes.c_int, ctypes.c_ulong,
        ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong))]
    root = x11.XDefaultRootWindow(dpy)
    atom = x11.XInternAtom(dpy, b'GAMESCOPE_PID', 1)
    kind, fmt = ctypes.c_ulong(0), ctypes.c_int(0)
    count, left = ctypes.c_ulong(0), ctypes.c_ulong(0)
    value = ctypes.POINTER(ctypes.c_ulong)()
    if not atom or x11.XGetWindowProperty(
            dpy, root, atom, 0, 1, 0, 6, ctypes.byref(kind),
            ctypes.byref(fmt), ctypes.byref(count), ctypes.byref(left),
            ctypes.byref(value)) or fmt.value != 32 or count.value != 1:
        return None
    owner, pid = value[0], os.getpid()
    while pid > 1:
        try:
            with open('/proc/%d/stat' % pid) as f:
                pid = int(f.read().rsplit(')', 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            return None
        if pid == owner:
            return root
    return None


def pointer_helper(d):
    """Inside a case's gamescope: puts the pointer of gamescope's own X
    server at each point the runner writes to d/pointer.at, and keeps it
    moving there, between the point and the pixel right of it at uneven
    intervals: the menu hides a cursor that has not moved for 4 s
    (MENU_INPUT_HIDE_CURSOR_DELAY), and a screenshot of a loaded runner
    can come later than that. Never on a display that isn't this
    gamescope's."""
    if os.environ.get('DISPLAY', '') in ('', ':0'):
        return 99
    x11 = ctypes.CDLL('libX11.so.6')
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XWarpPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                 ctypes.c_ulong] + [ctypes.c_int] * 6
    x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    deadline = time.monotonic() + 5
    dpy = x11.XOpenDisplay(None)
    while not dpy and time.monotonic() < deadline:
        time.sleep(0.05)
        dpy = x11.XOpenDisplay(None)
    if not dpy:
        print('[e2e] pointer helper cannot open DISPLAY=%s'
              % os.environ['DISPLAY'], flush=True)
        return 1
    x11.XDefaultRootWindow.restype = ctypes.c_ulong
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    root = (x11.XDefaultRootWindow(dpy) if XVFB
            else gamescope_root(x11, dpy))
    if root is None:
        print('[e2e] refusing %s: not this gamescope\'s X server.'
              % os.environ['DISPLAY'], flush=True)
        return 99
    done, deadline = None, time.time() + 120
    at, off, wiggle = None, 0, 0.0
    while (time.time() < deadline
           and not os.path.exists(os.path.join(d, 'pointer.quit'))):
        try:
            with open(os.path.join(d, 'pointer.at')) as f:
                n, x, y = (int(v) for v in f.read().split())
        except (OSError, ValueError):
            n = done
        if n != done:
            x11.XWarpPointer(dpy, 0, root, 0, 0, 0, 0, x, y)
            x11.XSync(dpy, 0)
            done, at, off = n, (x, y), 0
            # Alive while the runner keeps moving it, however slowly
            deadline = time.time() + 120
            wiggle = time.time() + random.uniform(0.05, 0.15)
            tmp = os.path.join(d, 'pointer.done.tmp')
            with open(tmp, 'w') as f:
                f.write('%d\n' % n)
            os.replace(tmp, os.path.join(d, 'pointer.done'))
            print('[e2e] pointer at (%d,%d).' % (x, y), flush=True)
        elif at and time.time() >= wiggle:
            off = 1 - off
            x11.XWarpPointer(dpy, 0, root, 0, 0, 0, 0, at[0] + off, at[1])
            x11.XSync(dpy, 0)
            wiggle = time.time() + random.uniform(0.05, 0.15)
        time.sleep(0.02)
    return 0


def report(name, errors):
    """A case's result lines."""
    return ['%s %s' % ('FAIL' if errors else 'pass', name)] + \
        ['    ' + e for e in errors]


def driver_jobs(retroarch, root, driver):
    """The driver's cases as jobs that can run side by side, each
    returning its result lines and how many of them failed."""
    jobs = []

    def plain(case, hw):
        errors = run_case(retroarch, root, driver, case, hw)
        return report('%s/%s%s' % (driver, case[0], '' if hw == 'off'
                                   else ' (hw ' + hw + ')'), errors), \
            bool(errors)

    def own_preset(case, suffix, preset):
        name, mapopt, settings, points = case
        c = (name + suffix, mapopt,
             dict(settings, video_shader_enable='true'), points)
        errors = run_case(retroarch, root, driver, c,
                          args=('--set-shader=' + preset,))
        if not direct_drawn(case_dir(root, driver, c[0]), PRESET_RE):
            errors.append('views not drawn through the preset')
        if not direct_drawn(case_dir(root, driver, c[0]), WRITE_SW_RE):
            errors.append('views not written straight in')
        return report('%s/%s' % (driver, c[0]), errors), bool(errors)

    def exact(case):
        hw = '' if case[3] == 'off' else ' (hw ' + case[3] + ')'
        copy = 'glCopyImageSubData' if driver == 'glcore' else None
        runs = [('', None, copy, None)]
        if driver in PRESET_DRIVERS:
            runs.append(('-preset', None, copy, HISTORY_PRESET))
        if driver == 'glcore':
            runs.append(('-preset-blit', NO_COPY_IMAGE,
                         'framebuffer blits', HISTORY_PRESET))
        runs += [(suffix, None, None, preset)
                 for suffix, preset in DRIVER_PRESETS.get(driver, [])]
        first, lines, failed = None, [], 0
        for suffix, env_extra, path, preset in runs:
            errors, rows = run_exact_case(
                retroarch, root, driver, case, suffix, env_extra, path,
                preset)
            if first is None:
                first = rows
            elif rows and first and rows != first:
                errors.append('screenshot differs from %s/%s\'s'
                              % (driver, case[0]))
            lines += report('%s/%s%s%s' % (driver, case[0], suffix, hw),
                            errors)
            failed += bool(errors)
        return lines, failed

    def rect(preset, in_place, tolerance):
        name = 'rect-' + preset[:-len('.slangp')]
        path = os.path.join(HERE, preset)
        errors, first = run_exact_case(
            retroarch, root, driver, (name, 'none', NOISE_LARGE, 'off'),
            preset=path, exact=False)
        lines = report('%s/%s' % (driver, name), errors)
        failed = bool(errors)
        for hw in RECT_HW[driver]:
            case = (name, 'none', NOISE_LARGE, hw)
            errors, rows = run_exact_case(retroarch, root, driver, case,
                                          preset=path, exact=False)
            d = case_dir(root, driver, name, hw)
            if direct_drawn(d, IN_PLACE_RE) != in_place:
                errors.append('frame %s where it lies' %
                              ('not read' if in_place else 'read'))
            # A copied frame upside down filters to within a step of the
            # software frame's: weights round the other way.
            bad = rows_differ(rows, first, tolerance if in_place else 1) \
                if rows and first else None
            if bad is None:
                errors.append('no screenshot to compare')
            elif bad:
                errors.append('%d pixels differ from the software '
                              'frame\'s' % bad)
            lines += report('%s/%s (hw %s)' % (driver, name, hw), errors)
            failed += bool(errors)
        return lines, failed

    def view_rect(case, mode, tolerance):
        name, mapopt, opts, hw = case
        rows = {}
        errors = []
        for kind, in_place in (('rect', True), ('rect_hist', False)):
            preset = os.path.join(HERE, '%s_%s.slangp' % (kind, mode))
            c = ('%s-%s-%s' % (name, kind, mode), mapopt, opts, hw)
            e, rows[kind] = run_exact_case(retroarch, root, driver, c,
                                           preset=preset, exact=False,
                                           in_place=in_place)
            errors += e
            d = case_dir(root, driver, c[0], hw)
            sw_out = hw == 'off' and driver not in SW_IN_PLACE_DRIVERS
            if direct_drawn(d, VIEW_IN_PLACE_RE) != (in_place and not sw_out):
                errors.append('%s: views %s where they lie' % (
                    kind, 'not read' if in_place else 'read'))
        if not rows['rect'] or not rows['rect_hist']:
            errors.append('no screenshot to compare')
        elif rows_differ(rows['rect'], rows['rect_hist'], tolerance):
            errors.append('%d pixels differ from the copied views\''
                          % rows_differ(rows['rect'],
                                        rows['rect_hist'], tolerance))
        return report('%s/%s-%s%s' % (driver, name, mode, '' if hw == 'off'
                                      else ' (hw ' + hw + ')'), errors), \
            bool(errors)

    def smooth(case):
        hw = '' if case[3] == 'off' else ' (hw ' + case[3] + ')'
        errors, rows = run_exact_case(retroarch, root, driver, case,
                                      exact=False)
        copy_errors, copy_rows = run_exact_case(
            retroarch, root, driver, case, '-preset',
            preset=LINEAR_PRESET, exact=False)
        errors += copy_errors
        if not rows or not copy_rows:
            errors.append('no screenshot to compare')
        else:
            bad = rows_differ(rows, copy_rows, TOLERANCE)
            if bad:
                errors.append('%d pixels differ from the preset\'s copies'
                              % bad)
        return report('%s/%s%s' % (driver, case[0], hw), errors), \
            bool(errors)

    def memory(case):
        errors, peak = run_memory_case(retroarch, root, driver, case)
        return report('%s/%s (hw %s, GPU %s MiB)'
                      % (driver, case[0], MEMORY_HW[driver],
                         '?' if peak is None else '%d' % peak), errors), \
            bool(errors)

    def menu_case(case, brightness=False):
        errors = run_menu_case(retroarch, root, driver, case, brightness)
        return report('%s/%s' % (driver, case[0]), errors), bool(errors)

    def hdr_case(case):
        errors = run_hdr_case(retroarch, root, driver, case)
        return report('%s/%s' % (driver, case[0]), errors), bool(errors)

    def menu_fit():
        errors = run_menu_fit_case(retroarch, root, driver, MENU_FIT_CASE)
        return report('%s/%s' % (driver, MENU_FIT_CASE[0]), errors), \
            bool(errors)

    def pointer(menu, case):
        errors = run_pointer_case(retroarch, root, driver, menu, case)
        return report('%s/%s-%s' % (driver, case[0], menu), errors), \
            bool(errors)

    def screens_rules():
        errors = run_screens_rules_case(retroarch, root, driver)
        return report('%s/screens-rules' % driver, errors), bool(errors)

    def screens_aim(case):
        errors = run_screens_aim_case(retroarch, root, driver, case)
        return report('%s/%s' % (driver, case[0]), errors), bool(errors)

    def screens_show(case, screens=None):
        errors = run_screens_show_case(retroarch, root, driver, case,
                                       screens)
        return report('%s/%s' % (driver, case[0]), errors), bool(errors)

    def screens_status(case):
        name, settings, whole, args = case
        errors = run_case(retroarch, root, driver,
                          (name, 'none', settings, []), args=args,
                          screens=0 if driver in whole else 1,
                          screenshot=False)
        return report('%s/%s' % (driver, name), errors), bool(errors)

    def screens_preset(case):
        name, mode, points, extra = case
        preset = os.path.join(HERE, SCREENS_PRESETS[driver])
        extra = dict(extra, args=('--set-shader=' + preset,))
        errors = run_screens_show_case(retroarch, root, driver,
                                       (name, mode, points, extra))
        d = case_dir(root, driver, name)
        if driver in DRIVER_PRESETS and not direct_drawn(d, PRESET_RE):
            errors.append('views not drawn through the preset')
        if direct_drawn(d, DIRECT_RE):
            errors.append('views drawn straight from the frame')
        return report('%s/%s' % (driver, name), errors), bool(errors)

    def screens_image(case, hw):
        errors = run_screens_image_case(retroarch, root, driver, case, hw)
        name = case[0] if hw == 'off' else '%s (hw %s)' % (case[0], hw)
        return report('%s/%s' % (driver, name), errors), bool(errors)

    def screens_menu():
        errors = run_screens_menu_case(retroarch, root, driver)
        return report('%s/%s' % (driver, SCREENS_MENU_CASE[0]), errors), \
            bool(errors)

    runs = [(case, 'off') for case in CASES
            if not (driver in WHOLE_ANAGLYPH_DRIVERS and 'anaglyph' in case[0])]
    runs += [(case, hw) for name, hw in HW_CASES.get(driver, [])
             for case in CASES if case[0] == name]
    if driver == 'vulkan':
        runs += [(case, 'vulkan') for case in VULKAN_CROP_CASES]
    jobs += [lambda c=c, hw=hw: plain(c, hw)
             for c, hw in runs if wanted(c[0])]
    jobs += [lambda c=c, x=x, p=p: own_preset(c, x, p)
             for c in (CASES if driver in DRIVER_PRESETS else [])
             if c[0] in GL_PRESET_CASES and wanted(c[0])
             for x, p in DRIVER_PRESETS[driver]]
    jobs += [lambda c=c: exact(c) for c in EXACT_CASES
             if c[3] in EXACT_HW.get(driver, ()) and wanted(c[0])]
    jobs += [lambda p=p, i=i, t=t: rect(p, i, t)
             for p, i, t in (RECT_PRESETS if driver in RECT_HW else [])
             if wanted('rect-' + p[:-len('.slangp')])]
    jobs += [lambda c=c, m=m, t=t: view_rect(c, m, t)
             for c in VIEW_RECT_CASES
             if c[3] in EXACT_HW.get(driver, ())
             and driver in PRESET_DRIVERS and driver not in SM4_DRIVERS
             and wanted(c[0])
             for m, t in VIEW_RECT_PRESETS]
    jobs += [lambda c=c: smooth(c) for c in SMOOTH_CASES
             if c[3] in EXACT_HW.get(driver, ())
             and driver in PRESET_DRIVERS and wanted(c[0])]
    jobs += [lambda c=c: memory(c)
             for c in (MEMORY_CASES if driver in MEMORY_HW and not XVFB
                       else []) if wanted(c[0])]
    jobs += [lambda c=c: menu_case(c) for c in MENU_CASES if wanted(c[0])]
    jobs += [lambda c=c: menu_case(c, True)
             for c in (HDR_MENU_CASES if HDR and driver == 'vulkan' else [])
             if wanted(c[0])]
    jobs += [lambda c=c: hdr_case(c)
             for c in (HDR_CASES if HDR and driver == 'vulkan' else [])
             if wanted(c[0])]
    if driver in MENU_FIT_DRIVERS and wanted(MENU_FIT_CASE[0]):
        jobs.append(menu_fit)
    jobs += [lambda m=m, c=c: pointer(m, c) for m in sorted(POINTER_MENUS)
             for c in POINTER_CASES
             if wanted(c[0]) and driver not in NO_POINTER_DRIVERS]
    if driver in SCREENS_STATUS_DRIVERS and wanted('screens-rules'):
        jobs.append(screens_rules)
    jobs += [lambda c=c: screens_aim(c)
             for c in (SCREENS_AIM_CASES if driver in SCREENS_DRIVERS
                       else []) if wanted(c[0])]
    jobs += [lambda c=c: screens_show(
                 c, 1 if driver in SCREENS_STATUS_DRIVERS else None)
             for c in SCREENS_DRAW_CASES
             if wanted(c[0]) and not (driver in NO_REDRAW_DRIVERS
                                      and c[0] in SCREENS_NULL_CASES)]
    if driver == 'xvideo' and wanted(SCREENS_XV_INTERLACED_CASE[0]):
        jobs.append(lambda: screens_show(SCREENS_XV_INTERLACED_CASE))
    jobs += [lambda c=c: screens_preset(c)
             for c in (SCREENS_PRESET_CASES if driver in SCREENS_PRESETS
                       else [])
             if wanted(c[0])
             and c[0] not in SCREENS_PRESET_SOLID.get(driver, ())]
    jobs += [lambda c=c: screens_show(c)
             for c in (SCREENS_STATE_CASES if driver in SCREENS_DRIVERS
                       else []) if wanted(c[0])]
    if driver in SCREENS_DRIVERS and wanted(SCREENS_MENU_CASE[0]):
        jobs.append(screens_menu)
    jobs += [lambda c=c, hw=hw: screens_image(c, hw)
             for c in SCREENS_IMAGE_CASES
             if driver in c[3].get('drivers', IMAGES_DRIVERS)
             and wanted(c[0])
             and (XVFB and not WINE or 'window' not in c[3])
             for hw in ([h for h in EXACT_HW[driver] if h != 'off']
                        if c[3].get('hw') else ['off'])]
    jobs += [lambda c=c: screens_status(c)
             for c in (SCREENS_STATUS_CASES
                       if driver in SCREENS_STATUS_DRIVERS else [])
             + (SCREENS_GL_STATUS_CASES if driver == 'gl' else [])
             if wanted(c[0])]
    return jobs


def main():
    if len(sys.argv) == 2 and sys.argv[1] == '--wm-ready':
        return wm_ready_probe()
    if sys.argv[1:2] == ['--pointer']:
        return pointer_helper(sys.argv[2])
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    if XVFB and not shutil.which('import'):
        # The menu and pointer cases read the screen through
        # ImageMagick's import.
        print('import (ImageMagick) is needed under Xvfb')
        return 1
    retroarch = os.path.abspath(sys.argv[1])
    root = os.path.realpath(sys.argv[2])
    os.makedirs(root, exist_ok=True)
    jobs = [j for driver in sys.argv[3:]
            for j in driver_jobs(retroarch, root, driver)]
    failed = 0
    try:
        if WINE:
            wine_prefixes()
        with ThreadPoolExecutor(max_workers=JOBS) as pool:
            for future in [pool.submit(j) for j in jobs]:
                lines, n = future.result()
                for line in lines:
                    print(line, flush=True)
                failed += n
    finally:
        for p in WINE_COPIES:
            subprocess.run(['wineserver', '-k'],
                           env=dict(os.environ, WINEPREFIX=p),
                           stderr=subprocess.DEVNULL)
            shutil.rmtree(os.path.dirname(p), ignore_errors=True)
        for p in HDR_LAYER_DIRS:
            shutil.rmtree(p, ignore_errors=True)
    print('%d failed' % failed)
    return 1 if failed else 0

if __name__ == '__main__':
    sys.exit(main())
