/* Copyright (C) 2026 The RetroArch team
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>

/* core_signing_keys.h as each platform's build sees it. The Makefile
 * compiles this against a copy beside a config.h, as configure leaves
 * them, so a case can set HAVE_STEAM the way a Steam build does. */

static unsigned checks;
static unsigned failures;

static void gate_check(const char *name, int want, int got)
{
   checks++;
   if (got == want)
      return;
   failures++;
   printf("FAIL %s: %d keys, expected %d\n", name, got, want);
}

static void gate_cases(void)
{
#define GATE_RESET_ONLY
#include "core_signing_gate_case.h"
#undef GATE_RESET_ONLY

   /* Desktop Linux */
#define GATE_NAME "Linux x86_64"
#define GATE_KEYS 2
#define __linux__ 1
#define __x86_64__ 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Linux aarch64"
#define GATE_KEYS 2
#define __linux__ 1
#define __aarch64__ 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Linux i686"
#define GATE_KEYS 0
#define __linux__ 1
#define __i386__ 1
#define __i686__ 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Linux armhf"
#define GATE_KEYS 0
#define __linux__ 1
#define __arm__ 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Linux MIPS (dingux, retrofw, rg350)"
#define GATE_KEYS 0
#define __linux__ 1
#define __mips__ 1
#define DINGUX 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Linux riscv64"
#define GATE_KEYS 0
#define __linux__ 1
#define __riscv 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Hakchi"
#define GATE_KEYS 0
#define __linux__ 1
#define __x86_64__ 1
#define HAKCHI 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Lakka x86_64"
#define GATE_KEYS 0
#define __linux__ 1
#define __x86_64__ 1
#define HAVE_LAKKA 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Lakka aarch64"
#define GATE_KEYS 0
#define __linux__ 1
#define __aarch64__ 1
#define HAVE_LAKKA 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Steam Linux x86_64"
#define GATE_KEYS 0
#define __linux__ 1
#define __x86_64__ 1
#define HAVE_STEAM 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Steam Linux x86_64, from config.h"
#define GATE_KEYS 0
#define __linux__ 1
#define __x86_64__ 1
#define GATE_CONFIG_STEAM 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Steam Linux aarch64, from config.h"
#define GATE_KEYS 0
#define __linux__ 1
#define __aarch64__ 1
#define GATE_CONFIG_STEAM 1
#include "core_signing_gate_case.h"

   /* Android */
#define GATE_NAME "Android arm64-v8a"
#define GATE_KEYS 2
#define ANDROID 1
#define __linux__ 1
#define __aarch64__ 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Android armeabi-v7a"
#define GATE_KEYS 2
#define ANDROID 1
#define __linux__ 1
#define __arm__ 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Android x86_64"
#define GATE_KEYS 2
#define ANDROID 1
#define __linux__ 1
#define __x86_64__ 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Android x86"
#define GATE_KEYS 2
#define ANDROID 1
#define __linux__ 1
#define __i686__ 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Play arm64-v8a"
#define GATE_KEYS 0
#define ANDROID 1
#define ANDROID_PLAY_DELIVERY 1
#define __linux__ 1
#define __aarch64__ 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Play x86_64"
#define GATE_KEYS 0
#define ANDROID 1
#define ANDROID_PLAY_DELIVERY 1
#define __linux__ 1
#define __x86_64__ 1
#include "core_signing_gate_case.h"

   /* webOS */
#define GATE_NAME "webOS armv7a"
#define GATE_KEYS 2
#define WEBOS 1
#define __linux__ 1
#define __arm__ 1
#include "core_signing_gate_case.h"

#define GATE_NAME "webOS aarch64"
#define GATE_KEYS 2
#define WEBOS 1
#define __linux__ 1
#define __aarch64__ 1
#include "core_signing_gate_case.h"

   /* Windows */
#define GATE_NAME "Windows x86_64 mingw"
#define GATE_KEYS 2
#define _WIN32 1
#define __x86_64__ 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Windows x86_64 MSVC 2019"
#define GATE_KEYS 2
#define _WIN32 1
#define _M_X64 1
#define _MSC_VER 1929
#include "core_signing_gate_case.h"

#define GATE_NAME "Windows x86_64 MSVC 2017"
#define GATE_KEYS 2
#define _WIN32 1
#define _M_X64 1
#define _MSC_VER 1910
#include "core_signing_gate_case.h"

#define GATE_NAME "Windows x86_64 MSVC 2013"
#define GATE_KEYS 2
#define _WIN32 1
#define _M_X64 1
#define _MSC_VER 1800
#include "core_signing_gate_case.h"

#define GATE_NAME "Windows x86"
#define GATE_KEYS 0
#define _WIN32 1
#define __i686__ 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Windows x86 MSVC 2019"
#define GATE_KEYS 0
#define _WIN32 1
#define _M_IX86 1
#define _MSC_VER 1929
#include "core_signing_gate_case.h"

#define GATE_NAME "Windows ARM"
#define GATE_KEYS 0
#define _WIN32 1
#define _M_ARM 1
#define _MSC_VER 1929
#include "core_signing_gate_case.h"

#define GATE_NAME "Windows ARM64"
#define GATE_KEYS 0
#define _WIN32 1
#define _M_ARM64 1
#define _MSC_VER 1929
#include "core_signing_gate_case.h"

#define GATE_NAME "Windows ARM64 mingw"
#define GATE_KEYS 0
#define _WIN32 1
#define __aarch64__ 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Windows x86_64 MSVC 2010"
#define GATE_KEYS 0
#define _WIN32 1
#define _M_X64 1
#define _MSC_VER 1600
#include "core_signing_gate_case.h"

#define GATE_NAME "Windows x86_64 MSVC 2005"
#define GATE_KEYS 0
#define _WIN32 1
#define _M_X64 1
#define _MSC_VER 1400
#include "core_signing_gate_case.h"

#define GATE_NAME "Windows x86_64 MSVC 2003"
#define GATE_KEYS 0
#define _WIN32 1
#define _M_X64 1
#define _MSC_VER 1310
#include "core_signing_gate_case.h"

#define GATE_NAME "Xbox"
#define GATE_KEYS 0
#define _WIN32 1
#define _XBOX 1
#define _M_X64 1
#define _MSC_VER 1929
#include "core_signing_gate_case.h"

#define GATE_NAME "UWP x86_64"
#define GATE_KEYS 0
#define _WIN32 1
#define __WINRT__ 1
#define _M_X64 1
#define _MSC_VER 1929
#include "core_signing_gate_case.h"

#define GATE_NAME "Steam Windows x86_64"
#define GATE_KEYS 0
#define _WIN32 1
#define __x86_64__ 1
#define HAVE_STEAM 1
#include "core_signing_gate_case.h"

#define GATE_NAME "Steam Windows x86_64, from config.h"
#define GATE_KEYS 0
#define _WIN32 1
#define __x86_64__ 1
#define GATE_CONFIG_STEAM 1
#include "core_signing_gate_case.h"

   /* Default server elsewhere, or none */
#define GATE_NAME "macOS arm64"
#define GATE_KEYS 0
#define __APPLE__ 1
#define __aarch64__ 1
#include "core_signing_gate_case.h"

#define GATE_NAME "FreeBSD x86_64"
#define GATE_KEYS 0
#define __FreeBSD__ 1
#define __x86_64__ 1
#include "core_signing_gate_case.h"

#define GATE_NAME "No platform"
#define GATE_KEYS 0
#include "core_signing_gate_case.h"
}

int main(void)
{
   /* FAIL lines survive a crash or a sanitizer exit */
   setvbuf(stdout, NULL, _IONBF, 0);
   gate_cases();
   printf("%u checks, %u failures\n", checks, failures);
   return failures ? 1 : 0;
}
