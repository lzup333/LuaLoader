/*******************************************************************************
 * LuaLoader - platform detection
 * Copyright (C) 2026 lzup333
 * Based on KernelLoader (https://github.com/eternalfuture-e38299/TEFKernel-KernelLoader)
 *   Copyright (C) 2026 eternalfuture-e38299
 * Author: lzup333
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *******************************************************************************/

#pragma once

// 与 KernelLoader 保持一致的平台 / 架构命名，可用于：
//   - 入口脚本的平台化选择（main.<平台>.<架构>.lua）
//   - 暴露给 Lua 的 mod.platform / mod.arch

#if defined(_WIN32) || defined(_WIN64)
#define LUALOADER_PLATFORM_NAME "windows"
#elif defined(__APPLE__) && defined(__MACH__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
#define LUALOADER_PLATFORM_NAME "ios"
#else
#define LUALOADER_PLATFORM_NAME "macos"
#endif
#elif defined(__ANDROID__)
#define LUALOADER_PLATFORM_NAME "android"
#elif defined(__linux__)
#define LUALOADER_PLATFORM_NAME "linux"
#else
#define LUALOADER_PLATFORM_NAME "unknown"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define LUALOADER_ARCH_NAME "x64"
#elif defined(__i386__) || defined(_M_IX86)
#define LUALOADER_ARCH_NAME "x86"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define LUALOADER_ARCH_NAME "arm64"
#elif defined(__arm__) || defined(_M_ARM)
#define LUALOADER_ARCH_NAME "arm"
#else
#define LUALOADER_ARCH_NAME "unknown"
#endif
