# Lua 5.4.8 - vendored static library for TEFKernel-LuaLoader
# Only the interpreter core is built as a static library (no lua/luac binaries).
cmake_minimum_required(VERSION 3.28.3)

project(lua C)

file(GLOB LUA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

add_library(lua STATIC ${LUA_SOURCES})

target_include_directories(lua PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# 开启动态库加载（loadlib / require 原生模块）
if (WIN32)
    target_compile_definitions(lua PRIVATE LUA_USE_WINDOWS)
else()
    target_compile_definitions(lua PRIVATE LUA_USE_DLOPEN)
endif()

# Lua uses POSIX/ANSI C only; keep it warning-quiet and self-contained.
target_compile_options(lua PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-w>
)

if (UNIX AND NOT ANDROID)
    target_link_libraries(lua PUBLIC dl m)
elseif (ANDROID)
    target_link_libraries(lua PUBLIC dl m)
elseif (WIN32)
    target_link_libraries(lua PUBLIC)
endif()

set_target_properties(lua PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_STANDARD 99
)
