#!/usr/bin/env python3
"""The format pick is the only writer of the swapchain's HDR flags.

Runs the shipping resize and default-chain code from vulkan.c against
vulkan_mode_test.c's mocks. CFLAGS adds flags; CC=x86_64-w64-mingw32-gcc
with RUNNER=wine runs it as a Windows program.
"""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
vulkan = (root / "gfx/drivers/vulkan.c").read_text(encoding="utf-8")


def source_block(source, marker):
    # The definition, not a declaration ahead of it
    start = source.index(marker)
    while source.index(";", start) < source.index("{", start):
        start = source.index(marker, start + 1)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


code = "".join(source_block(vulkan, marker) for marker in (
    "static void vulkan_set_hdr_inverse_tonemap(",
    "static void vulkan_set_hdr10(",
    "static bool vulkan_init_default_filter_chain(",
    "static void vulkan_apply_pending_resize("))

with tempfile.TemporaryDirectory(prefix="retroarch-vulkan-mode-") as directory:
    directory = Path(directory)
    (directory / "vulkan_mode.inc").write_text(code, encoding="utf-8")
    cc = shlex.split(os.environ.get("CC", "cc"))
    exe = directory / ("vulkan_mode.exe" if "mingw" in os.environ.get("CC", "")
                       else "vulkan_mode")
    subprocess.run(cc + ["-std=gnu89", "-O1", "-g1", "-Wall", "-Wextra",
                        "-Werror=declaration-after-statement",
                        "-Werror=implicit-function-declaration",
                        "-DVULKAN_HDR_SWAPCHAIN",
                        "-I" + str(root / "libretro-common/include"),
                        "-I" + str(root / "gfx/include"),
                        "-I" + str(directory)]
                   + shlex.split(os.environ.get("CFLAGS", ""))
                   + [str(Path(__file__).with_name("vulkan_mode_test.c")),
                      "-o", str(exe)], check=True)
    sys.exit(subprocess.run(shlex.split(os.environ.get("RUNNER", ""))
                            + [str(exe)]).returncode)
