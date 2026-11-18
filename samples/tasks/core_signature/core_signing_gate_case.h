/* Copyright (C) 2026 The RetroArch team
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* No include guard: core_signing_gate_test.c includes this once per case,
 * with GATE_NAME, GATE_KEYS and the platform's macros defined. */
#ifndef GATE_RESET_ONLY
#undef CORE_SIGNING_KEYS_H__
#undef CORE_SIGNING_HAS_KEYS
#undef CORE_SIGNING_KEYS_COUNT
#undef CORE_SIGNING_KEYS_LIST
#undef CORE_SIGNING_TEST_KEY
#include <core_signing_keys.h>
gate_check(GATE_NAME, GATE_KEYS, CORE_SIGNING_KEYS_COUNT);
#undef GATE_NAME
#undef GATE_KEYS
#endif

#undef __linux__
#undef __x86_64__
#undef __aarch64__
#undef __arm__
#undef __i386__
#undef __i686__
#undef __mips__
#undef __riscv
#undef __APPLE__
#undef __FreeBSD__
#undef _WIN32
#undef _M_X64
#undef _M_IX86
#undef _M_ARM
#undef _M_ARM64
#undef _MSC_VER
#undef _XBOX
#undef __WINRT__
#undef ANDROID
#undef ANDROID_PLAY_DELIVERY
#undef WEBOS
#undef HAKCHI
#undef DINGUX
#undef HAVE_LAKKA
#undef HAVE_STEAM
#undef GATE_CONFIG_STEAM
