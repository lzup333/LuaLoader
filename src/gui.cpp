/*******************************************************************************
 * LuaLoader - gui (Dear ImGui 内置 GUI)
 * Copyright (C) 2026 lzup333
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

#include "gui.hpp"

#include <cctype>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#endif

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include "logger.hpp"
#include "core.hpp"

#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/array.h"
#include "tefkernel/patchlib/thread.h"
#include "tefkernel/patchlib/type.h"

namespace lualoader::gui {

    namespace {
        bool g_ctx_ready = false;
        bool g_installed = false;
        float g_ui_scale = 1.0f;       // UI 缩放（高 DPI 手机上放大，避免字太小）
        float g_font_bake_scale = 1.0f; // 字体图集的烘焙比例（建上下文时确定）
        ImGuiStyle g_base_style;        // 未缩放的基础样式

        patch_hook_id_t g_draw_hook = PATCH_HOOK_INVALID_ID;
        patch_handle_t g_f_screen_w = nullptr; // Main.screenWidth  (static int32)
        patch_handle_t g_f_screen_h = nullptr; // Main.screenHeight (static int32)

        // GUI 鼠标模式：true 时输入交给我们（桌面关闭相对模式，Android 无需此概念）
        bool g_gui_mouse = false;
        bool g_toggle_prev = false;

#if defined(__ANDROID__)
        // ---- Android：复用内核导出的 Dobby，钩 eglSwapBuffers 做 GLES 叠加渲染 ----
        using swap_fn = EGLBoolean (*)(EGLDisplay, EGLSurface);
        swap_fn p_orig_swap = nullptr;
        int g_surface_w = 0;
        int g_surface_h = 0;
        EGLDisplay g_egl_display = EGL_NO_DISPLAY;
        EGLSurface g_egl_surface = EGL_NO_SURFACE;
        bool g_in_render = false; // 防重入
        bool g_render_attached = false; // 渲染线程是否已附加到 il2cpp

        // 触摸输入：主线程经 patchlib 读 UnityEngine.Input.touches
        patch_handle_t g_input_touches = nullptr; // Input.get_touches -> Touch[]
        float g_touch_x = 0.0f;                   // Unity 坐标（原点左下）
        float g_touch_y = 0.0f;
        bool g_touch_down = false;
#else
        // ---- 桌面：SDL3 函数（运行时 dlsym）----
        using get_focus_fn = void *(*)();
        using set_rel_fn = bool (*)(void *, bool);
        using cursor_fn = bool (*)();
        using get_keys_fn = const bool *(*)(int *);
        using get_mouse_fn = uint32_t (*)(float *, float *);
        using warp_fn = bool (*)(void *, float, float);

        void *g_window = nullptr;
        get_focus_fn p_get_focus = nullptr;
        set_rel_fn p_set_rel = nullptr;
        cursor_fn p_show_cursor = nullptr;
        cursor_fn p_hide_cursor = nullptr;
        get_keys_fn p_get_keys = nullptr;
        get_mouse_fn p_get_mouse = nullptr;
        warp_fn p_warp = nullptr;

        constexpr int GUI_TOGGLE_SCANCODE = 73; // SDL_SCANCODE_INSERT
#endif

        void *dsym(const char *n) {
#if defined(_WIN32)
            // 先在主模块里找（SDL 若静态链接并导出即可命中），再尝试 SDL3/SDL2.dll
            if (HMODULE m = GetModuleHandleA(nullptr)) {
                if (void *p = reinterpret_cast<void *>(GetProcAddress(m, n))) return p;
            }
            static HMODULE sdl = nullptr;
            static bool tried = false;
            if (!tried) {
                tried = true;
                sdl = LoadLibraryA("SDL3.dll");
                if (!sdl) sdl = LoadLibraryA("SDL2.dll");
            }
            if (sdl) {
                if (void *p = reinterpret_cast<void *>(GetProcAddress(sdl, n))) return p;
            }
            return nullptr;
#else
            return dlsym(RTLD_DEFAULT, n);
#endif
        }

#if defined(__ANDROID__)
        /// 在一个已加载库里按名字找符号（名字可以是 soname；文件被删也能命中）
        void *dlopen_noload(const char *name) {
            return name ? dlopen(name, RTLD_NOW | RTLD_NOLOAD) : nullptr;
        }

        /// 解析已加载库中的符号（兼容内存加载 / 非全局可见 / 文件被删的情况）
        void *resolve_loaded_symbol(const char *libname, const char *sym) {
            if (void *p = dlsym(RTLD_DEFAULT, sym)) return p;
            // 先按 libname / soname 直接 NOLOAD
            for (const char *n: {libname, "libtefkernel.android.arm64-v8a.so",
                                 "libtefkernel.android.armeabi-v7a.so"}) {
                if (void *h = dlopen_noload(n)) {
                    if (void *s = dlsym(h, sym)) return s;
                }
            }
            if (void *r = dlsym(RTLD_DEFAULT, "DobbySymbolResolver")) {
                using resolver_fn = void *(*)(const char *, const char *);
                if (void *p = reinterpret_cast<resolver_fn>(r)(libname, sym)) return p;
            }
            // 再退一步：从 /proc/self/maps 找到库路径后 dlopen(RTLD_NOLOAD)
            if (FILE *f = fopen("/proc/self/maps", "r")) {
                char line[4096];
                while (fgets(line, sizeof(line), f)) {
                    if (!strstr(line, libname)) continue;
                    char *path = strrchr(line, '/');
                    if (!path) continue;
                    if (char *nl = strchr(path, '\n')) *nl = '\0';
                    if (char *del = strstr(path, " (deleted)")) *del = '\0';
                    if (void *h = dlopen(path, RTLD_NOW | RTLD_NOLOAD)) {
                        if (void *s = dlsym(h, sym)) {
                            fclose(f);
                            return s;
                        }
                    }
                }
                fclose(f);
            }
            return nullptr;
        }
#endif

#if defined(__ANDROID__)
        /// 采样 Unity 触摸（在主线程 Main.Draw 后置里调用）
        /// Touch 结构布局（UnityEngine.Touch，取自 dump）：
        ///   +0x4 Vector2.position, +0x24 TouchPhase
        void sample_input() {
            auto clear = [] {
                g_touch_down = false;
                g_touch_x = -FLT_MAX;
                g_touch_y = -FLT_MAX;
            };
            if (!g_input_touches) {
                clear();
                return;
            }
            patch_handle_t arr = nullptr;
            if (!patchlib_method_invoke_args(g_input_touches, nullptr, &arr, nullptr) || !arr) {
                clear();
                return;
            }
            const size_t n = patchlib_array_length(arr);
            if (n == 0) {
                clear();
                return;
            }
            unsigned char elem[0x48] = {};
            if (!patchlib_array_at(arr, 0, elem)) {
                clear();
                return;
            }
            g_touch_x = *reinterpret_cast<const float *>(elem + 0x4);
            g_touch_y = *reinterpret_cast<const float *>(elem + 0x8);
            const int phase = *reinterpret_cast<const int *>(elem + 0x24);
            // Began=0, Moved=1, Stationary=2, Ended=3, Canceled=4
            g_touch_down = (phase == 0 || phase == 1 || phase == 2);
        }
#endif

        void screen_size(int &w, int &h) {
#if defined(__ANDROID__)
            w = g_surface_w > 0 ? g_surface_w : 1280;
            h = g_surface_h > 0 ? g_surface_h : 720;
#else
            w = 1280;
            h = 720;
            if (g_f_screen_w) {
                int v = 0;
                patchlib_field_get_value(g_f_screen_w, nullptr, &v);
                if (v > 0) w = v;
            }
            if (g_f_screen_h) {
                int v = 0;
                patchlib_field_get_value(g_f_screen_h, nullptr, &v);
                if (v > 0) h = v;
            }
#endif
        }

#if !defined(__ANDROID__)
        /// 把指针移到屏幕中心（相对模式/绝对模式切换时避免卡在 (0,0)）
        void warp_to_center() {
            if (!p_warp || !g_window) return;
            int w = 0, h = 0;
            screen_size(w, h);
            p_warp(g_window, static_cast<float>(w) / 2.0f, static_cast<float>(h) / 2.0f);
        }

        void resolve_sdl() {
            if (!p_get_focus) p_get_focus = reinterpret_cast<get_focus_fn>(dsym("SDL_GetKeyboardFocus"));
            if (!p_set_rel) p_set_rel = reinterpret_cast<set_rel_fn>(dsym("SDL_SetWindowRelativeMouseMode"));
            if (!p_show_cursor) p_show_cursor = reinterpret_cast<cursor_fn>(dsym("SDL_ShowCursor"));
            if (!p_hide_cursor) p_hide_cursor = reinterpret_cast<cursor_fn>(dsym("SDL_HideCursor"));
            if (!p_get_keys) p_get_keys = reinterpret_cast<get_keys_fn>(dsym("SDL_GetKeyboardState"));
            if (!p_get_mouse) p_get_mouse = reinterpret_cast<get_mouse_fn>(dsym("SDL_GetMouseState"));
            if (!p_warp) p_warp = reinterpret_cast<warp_fn>(dsym("SDL_WarpMouseInWindow"));
            if (!g_window && p_get_focus) g_window = p_get_focus();
        }

        /// 热键切换 GUI 鼠标模式
        void poll_toggle() {
            if (!p_get_keys || !g_window) return;
            const bool *keys = p_get_keys(nullptr);
            if (!keys) return;
            const bool down = keys[GUI_TOGGLE_SCANCODE];
            if (down && !g_toggle_prev) {
                g_gui_mouse = !g_gui_mouse;
                if (g_gui_mouse) {
                    p_set_rel(g_window, false); // 关闭相对模式，释放鼠标
                    warp_to_center();
                    if (p_show_cursor) p_show_cursor();
                } else {
                    warp_to_center();           // 先居中，再回相对模式，避免卡左上角
                    p_set_rel(g_window, true);  // 还给游戏
                    if (p_hide_cursor) p_hide_cursor();
                }
                LOG_INFO("[gui] mouse {} (press Insert to toggle)", g_gui_mouse ? "released" : "recaptured");
            }
            g_toggle_prev = down;
        }
#endif

        // ---- 键盘：轮询 SDL 按键状态并转成 ImGui 事件（无需 hook SDL）----
        constexpr int NUM_SCANCODES = 512; // SDL_NUM_SCANCODES
        bool g_key_prev[NUM_SCANCODES] = {};
        bool g_mod_prev[4] = {};

        // ---- 自绘文本输入 + 虚拟键盘 ----
        // 不使用 ImGui::InputText：否则点击虚拟键盘按钮会切换 ImGui 的 active id，
        // 导致输入框失焦。这里自己维护“正在编辑哪个框”和缓冲区。
        constexpr int VKB_BUF_SIZE = 256;
        const void *g_edit_id = nullptr;      // 正在编辑的输入框（ImGuiID）
        char g_edit_buf[VKB_BUF_SIZE] = {};   // 编辑缓冲
        bool g_vkb_shift = false;             // 大小写锁定

        void vkb_append_char(char c) {
            const size_t len = std::strlen(g_edit_buf);
            if (len + 1 < sizeof(g_edit_buf)) {
                g_edit_buf[len] = c;
                g_edit_buf[len + 1] = '\0';
            }
        }

        void vkb_backspace() {
            const size_t len = std::strlen(g_edit_buf);
            if (len > 0) g_edit_buf[len - 1] = '\0';
        }

        void draw_virtual_keyboard();

#if !defined(__ANDROID__)
        const ImGuiKey k_mod_keys[4] = {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt, ImGuiMod_Super};

        /// SDL scancode -> ImGuiKey（覆盖常用键；数值取自 SDL3 固定枚举）
        ImGuiKey map_scancode(int sc) {
            if (sc >= 4 && sc <= 29) return static_cast<ImGuiKey>(ImGuiKey_A + (sc - 4)); // A-Z
            if (sc >= 30 && sc <= 39) { // 1-9,0
                if (sc == 39) return ImGuiKey_0;
                return static_cast<ImGuiKey>(ImGuiKey_1 + (sc - 30));
            }
            if (sc >= 58 && sc <= 69) return static_cast<ImGuiKey>(ImGuiKey_F1 + (sc - 58)); // F1-F12
            switch (sc) {
                case 40: return ImGuiKey_Enter;
                case 41: return ImGuiKey_Escape;
                case 42: return ImGuiKey_Backspace;
                case 43: return ImGuiKey_Tab;
                case 44: return ImGuiKey_Space;
                case 45: return ImGuiKey_Minus;
                case 46: return ImGuiKey_Equal;
                case 47: return ImGuiKey_LeftBracket;
                case 48: return ImGuiKey_RightBracket;
                case 49: return ImGuiKey_Backslash;
                case 51: return ImGuiKey_Semicolon;
                case 52: return ImGuiKey_Apostrophe;
                case 53: return ImGuiKey_GraveAccent;
                case 54: return ImGuiKey_Comma;
                case 55: return ImGuiKey_Period;
                case 56: return ImGuiKey_Slash;
                case 57: return ImGuiKey_CapsLock;
                case 73: return ImGuiKey_Insert;
                case 74: return ImGuiKey_Home;
                case 75: return ImGuiKey_PageUp;
                case 76: return ImGuiKey_Delete;
                case 77: return ImGuiKey_End;
                case 78: return ImGuiKey_PageDown;
                case 79: return ImGuiKey_RightArrow;
                case 80: return ImGuiKey_LeftArrow;
                case 81: return ImGuiKey_DownArrow;
                case 82: return ImGuiKey_UpArrow;
                case 224: return ImGuiKey_LeftCtrl;
                case 225: return ImGuiKey_LeftShift;
                case 226: return ImGuiKey_LeftAlt;
                case 227: return ImGuiKey_LeftSuper;
                case 228: return ImGuiKey_RightCtrl;
                case 229: return ImGuiKey_RightShift;
                case 230: return ImGuiKey_RightAlt;
                case 231: return ImGuiKey_RightSuper;
                default: return ImGuiKey_None;
            }
        }

        /// scancode -> 可输入字符（ASCII；需要 shift/caps 判断大小写）
        int scancode_to_char(int sc, bool shift, bool caps) {
            if (sc >= 4 && sc <= 29) { // 字母
                char c = static_cast<char>('a' + (sc - 4));
                return (shift != caps) ? (c - 32) : c;
            }
            if (sc >= 30 && sc <= 39) { // 数字及上档符号
                static const char *normal = "1234567890";
                static const char *shifted = "!@#$%^&*()";
                return shift ? shifted[sc - 30] : normal[sc - 30];
            }
            switch (sc) {
                case 44: return ' ';
                case 45: return shift ? '_' : '-';
                case 46: return shift ? '+' : '=';
                case 47: return shift ? '{' : '[';
                case 48: return shift ? '}' : ']';
                case 49: return shift ? '|' : '\\';
                case 51: return shift ? ':' : ';';
                case 52: return shift ? '"' : '\'';
                case 53: return shift ? '~' : '`';
                case 54: return shift ? '<' : ',';
                case 55: return shift ? '>' : '.';
                case 56: return shift ? '?' : '/';
                default: return 0;
            }
        }

        /// 每帧在 ImGui::NewFrame 之前调用；仅在 GUI 鼠标模式下把键盘交给 ImGui
        void poll_keyboard() {
            if (!p_get_keys) return;
            ImGuiIO &io = ImGui::GetIO();
            const bool *keys = p_get_keys(nullptr);
            if (!keys) return;

            if (!g_gui_mouse) {
                // 离开 GUI 输入模式：把仍按下的键释放掉，避免 ImGui 卡住
                for (int sc = 0; sc < NUM_SCANCODES; ++sc) {
                    if (!g_key_prev[sc]) continue;
                    const ImGuiKey k = map_scancode(sc);
                    if (k != ImGuiKey_None) io.AddKeyEvent(k, false);
                    g_key_prev[sc] = false;
                }
                for (int i = 0; i < 4; ++i) {
                    if (!g_mod_prev[i]) continue;
                    io.AddKeyEvent(k_mod_keys[i], false);
                    g_mod_prev[i] = false;
                }
                return;
            }

            const bool shift = keys[225] || keys[229];
            const bool caps = keys[57];
            for (int sc = 0; sc < NUM_SCANCODES; ++sc) {
                const bool down = keys[sc];
                if (down == g_key_prev[sc]) continue;
                g_key_prev[sc] = down;
                const ImGuiKey k = map_scancode(sc);
                if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
                if (down) {
                    const int c = scancode_to_char(sc, shift, caps);
                    if (c) io.AddInputCharacter(static_cast<unsigned int>(c));
                }
            }
            const bool mods[4] = {
                    keys[224] || keys[228], // Ctrl
                    keys[225] || keys[229], // Shift
                    keys[226] || keys[230], // Alt
                    keys[227] || keys[231], // Super
            };
            for (int i = 0; i < 4; ++i) {
                if (mods[i] == g_mod_prev[i]) continue;
                io.AddKeyEvent(k_mod_keys[i], mods[i]);
                g_mod_prev[i] = mods[i];
            }
        }
#endif // !__ANDROID__

        /// 屏幕虚拟键盘（底部居中）：点键直接改 g_edit_buf，手机端友好
        void draw_virtual_keyboard() {
            ImGuiIO &io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 10.0f),
                                    ImGuiCond_Always, ImVec2(0.5f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.88f);
            const ImGuiWindowFlags flags =
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                    ImGuiWindowFlags_NoNav;
            ImGui::Begin("##lualoader_vkb", nullptr, flags);

            const ImVec2 key(34 * g_ui_scale, 34 * g_ui_scale);
            const ImVec2 wide(58 * g_ui_scale, 34 * g_ui_scale);

            auto ch_key = [&](char c) {
                char lbl[2] = {c, '\0'};
                if (ImGui::Button(lbl, key)) vkb_append_char(c);
                ImGui::SameLine();
            };
            auto sym_key = [&](const char *lbl) {
                if (ImGui::Button(lbl, key)) vkb_append_char(lbl[0]);
                ImGui::SameLine();
            };
            auto letter_key = [&](char c) {
                const char out = g_vkb_shift ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
                char lbl[2] = {out, '\0'};
                if (ImGui::Button(lbl, key)) vkb_append_char(out);
                ImGui::SameLine();
            };

            for (const char *p = "1234567890"; *p; ++p) ch_key(*p);
            ImGui::NewLine();
            for (const char *p = "qwertyuiop"; *p; ++p) letter_key(*p);
            ImGui::NewLine();
            for (const char *p = "asdfghjkl"; *p; ++p) letter_key(*p);
            ImGui::NewLine();
            if (ImGui::Button(g_vkb_shift ? "大写" : "小写", wide)) g_vkb_shift = !g_vkb_shift;
            ImGui::SameLine();
            for (const char *p = "zxcvbnm"; *p; ++p) letter_key(*p);
            if (ImGui::Button("退格", wide)) vkb_backspace();
            ImGui::SameLine();
            ImGui::NewLine();
            if (ImGui::Button("空格", ImVec2(120 * g_ui_scale, 34 * g_ui_scale))) vkb_append_char(' ');
            ImGui::SameLine();
            sym_key(",");
            sym_key(".");
            sym_key("?");
            sym_key("!");
            if (ImGui::Button("完成", ImVec2(64 * g_ui_scale, 34 * g_ui_scale))) g_edit_id = nullptr;
            ImGui::SameLine();
            if (ImGui::Button("关闭", ImVec2(64 * g_ui_scale, 34 * g_ui_scale))) g_edit_id = nullptr;

            ImGui::End();
        }

        // ------------------------------------------------------------------
        // 字体与许可（重要）
        //
        // 本函数只在“运行时”读取操作系统里已安装的字体文件，把字形烘焙进 ImGui
        // 的内存字形图集，不复制、不打包、不再分发任何字体文件，因此 LuaLoader
        // 自身不产生字体再分发的许可义务。
        //
        // 若将来要把中文字体“随 Loader 一起打包发布”，只能选用允许再分发的
        // 开源字体，例如：
        //   - Noto Sans CJK / Source Han Sans —— SIL OFL 1.1（推荐）
        //   - 文泉驿微米黑 (WenQuanYi Micro Hei) —— Apache-2.0（另有 GPLv3 字体例外）
        //   - Droid Sans Fallback —— Apache-2.0
        // 严禁打包专有字体（微软雅黑 msyh、黑体 simhei、宋体 simsun、苹方 PingFang），
        // 它们在下表仅作“本机运行时兜底”，不得进入发布包。
        // ------------------------------------------------------------------
        void load_cjk_font(float size) {
            // 可自由再分发（OFL / Apache-2.0）——日后内置字体时从这组选
            static const char *redistributable[] = {
                    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                    "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
                    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.otf",
                    "/usr/share/fonts/truetype/noto/NotoSansSC-Regular.otf",
                    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
                    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
                    "/system/fonts/NotoSansCJK-Regular.ttc", // Android
                    "/system/fonts/DroidSansFallback.ttf",   // Android
                    nullptr,
            };
            // 专有字体：仅读取本机已安装的，绝不随包分发
            static const char *local_only[] = {
                    "C:/Windows/Fonts/msyh.ttc",          // 微软雅黑
                    "C:/Windows/Fonts/simhei.ttf",        // 黑体
                    "C:/Windows/Fonts/simsun.ttc",        // 宋体
                    "/System/Library/Fonts/PingFang.ttc", // 苹方
                    nullptr,
            };
            ImGuiIO &io = ImGui::GetIO();
            const ImWchar *ranges = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
            // 先试可再分发字体，实在没有再退回本机专有字体
            for (const char **p = redistributable; *p; ++p) {
                std::error_code ec;
                if (!std::filesystem::exists(*p, ec)) continue;
                if (io.Fonts->AddFontFromFileTTF(*p, size, nullptr, ranges)) {
                    LOG_INFO("[gui] CJK font loaded (redistributable): {}", *p);
                    return;
                }
            }
            for (const char **p = local_only; *p; ++p) {
                std::error_code ec;
                if (!std::filesystem::exists(*p, ec)) continue;
                if (io.Fonts->AddFontFromFileTTF(*p, size, nullptr, ranges)) {
                    LOG_INFO("[gui] CJK font loaded (local system font, not redistributed): {}", *p);
                    return;
                }
            }
            LOG_WARN("[gui] no CJK font found, Chinese may not render");
        }

        /// 根据屏幕尺寸计算 UI 缩放（手机放大；窗口越大字越大）
        float compute_ui_scale(int w, int h) {
            if (w <= 0 || h <= 0) return 1.0f;
            const int mn = w < h ? w : h;
#if defined(__ANDROID__)
            const float ref = 540.0f, hi = 3.0f;
#else
            const float ref = 720.0f, hi = 2.5f;
#endif
            float s = static_cast<float>(mn) / ref;
            if (s < 1.0f) s = 1.0f;
            if (s > hi) s = hi;
            return s;
        }

        /// 应用 UI 缩放：基础样式按比例缩放 + 字体全局缩放
        void apply_ui_scale() {
            ImGuiStyle &st = ImGui::GetStyle();
            st = g_base_style;
            if (g_ui_scale != 1.0f) st.ScaleAllSizes(g_ui_scale);
            ImGui::GetIO().FontGlobalScale = g_ui_scale / g_font_bake_scale;
        }

        void ensure_context() {
            if (g_ctx_ready) return;
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO &io = ImGui::GetIO();
            io.IniFilename = nullptr;

            // 先按当前屏幕算缩放，字体按缩放后的尺寸烘焙（保持清晰）
            int w = 0, h = 0;
            screen_size(w, h);
            g_ui_scale = compute_ui_scale(w, h);
            g_font_bake_scale = g_ui_scale;
            load_cjk_font(18.0f * g_font_bake_scale);

            ImGui::StyleColorsDark();
            g_base_style = ImGui::GetStyle();
            apply_ui_scale();
#if defined(__ANDROID__)
            ImGui_ImplOpenGL3_Init("#version 300 es"); // OpenGL ES 3.0
#else
            ImGui_ImplOpenGL3_Init(nullptr);
#endif
            g_ctx_ready = true;
            LOG_INFO("[gui] ImGui context created (ui_scale={:.2f})", g_ui_scale);
        }

        /// 把平台输入喂给 ImGui
        void feed_pointer() {
            ImGuiIO &io = ImGui::GetIO();
#if defined(__ANDROID__)
            // 输入已在主线程（Main.Draw 后置）采样，这里只读全局值
            int w = 0, h = 0;
            screen_size(w, h);
            // Unity 的鼠标/触摸坐标原点在左下，ImGui 在左上
            io.MousePos = ImVec2(g_touch_x, static_cast<float>(h) - g_touch_y);
            io.MouseDown[0] = g_touch_down;
            io.MouseDown[1] = false;
#else
            if (g_gui_mouse && p_get_mouse) {
                float mx = -FLT_MAX, my = -FLT_MAX;
                const uint32_t btn = p_get_mouse(&mx, &my);
                io.MousePos = ImVec2(mx, my);
                io.MouseDown[0] = (btn & 1u) != 0; // LMASK
                io.MouseDown[1] = (btn & 4u) != 0; // RMASK
            } else {
                // 鼠标归游戏：让 ImGui 忽略鼠标
                io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
                io.MouseDown[0] = false;
                io.MouseDown[1] = false;
            }
#endif
        }

        void render_once() {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui::NewFrame();

            ImGuiIO &io = ImGui::GetIO();
            int w = 0, h = 0;
            screen_size(w, h);
            io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));

            // 屏幕尺寸变化时同步更新缩放（字号/控件一起变）
            const float s = compute_ui_scale(w, h);
            if (s != g_ui_scale) {
                g_ui_scale = s;
                apply_ui_scale();
            }

            feed_pointer();

#if defined(__ANDROID__)
            // 叠加到默认帧缓冲；Unity 可能留下 FBO/视口，重建
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, w, h);
#endif

            // 每个定义了 mod.on_gui 的 Mod 一个窗口
            for (auto &kv: lualoader::core::lua_mod_handles) {
                auto *h = kv.second;
                if (!h || h->gui_ref == LUA_NOREF) continue; // 没有 on_gui 的 Mod 不开窗
                ImGui::SetNextWindowSize(ImVec2(360 * g_ui_scale, 260 * g_ui_scale), ImGuiCond_FirstUseEver);
                const bool open = ImGui::Begin(h->mod_id.c_str());
                if (open) lualoader::lua_engine::call_on_gui(h);
                ImGui::End();
            }

            // 有输入框处于编辑态时，弹出虚拟键盘
            if (g_edit_id) draw_virtual_keyboard();

            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

#if defined(__ANDROID__)
            glBindFramebuffer(GL_FRAMEBUFFER, 0); // 还原，交回游戏
#endif
        }

#if defined(__ANDROID__)
        /// Main.Draw 后置（主线程）：采样 Unity 输入到全局，供渲染线程读取
        void frame_postfix(patch_handle_t, void **, void *, const patch_method_signature_t *) {
            sample_input();
        }

        /// eglSwapBuffers 钩子：在真正交换前把 ImGui 叠加到默认帧缓冲
        EGLBoolean swap_hook(EGLDisplay dpy, EGLSurface surf) {
            if (!g_in_render && surf != EGL_NO_SURFACE) {
                EGLint w = 0, h = 0;
                eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
                eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
                if (w > 0) g_surface_w = w;
                if (h > 0) g_surface_h = h;
                g_egl_display = dpy;
                g_egl_surface = surf;
                g_gui_mouse = true; // Android 上触摸即 GUI 输入

                // 把渲染线程附加到 il2cpp：mod.on_gui 里可能调用 mod.patch（il2cpp），
                // 未附加的线程直接调用 il2cpp 会崩溃。
                if (!g_render_attached) {
                    patchlib_thread_attach();
                    g_render_attached = true;
                    LOG_INFO("[gui] 渲染线程已附加到 il2cpp");
                }

                g_in_render = true;
                ensure_context();
                render_once();
                g_in_render = false;
            }
            return p_orig_swap ? p_orig_swap(dpy, surf) : EGL_TRUE;
        }
#else
        void draw_postfix(patch_handle_t, void **, void *, const patch_method_signature_t *) {
            resolve_sdl();
            poll_toggle();
            if (g_gui_mouse && g_window && p_set_rel) {
                // 游戏每帧可能重新开启相对模式；GUI 模式下持续压制
                p_set_rel(g_window, false);
            }
            ensure_context();
            poll_keyboard(); // 必须在 ImGui::NewFrame 之前喂入键盘事件
            render_once();
        }
#endif
    } // namespace

    bool installed() { return g_installed; }

#if defined(__ANDROID__)
    void install() {
        if (g_installed) return;

        // 复用内核已加载的 Dobby：libtefkernel.so 导出了 DobbyHook；
        // LuaLoader 自身在内存中加载，故先用 /proc/self/maps 找到已加载路径再 dlopen(NOLOAD)。
        void *dobby = resolve_loaded_symbol("libtefkernel.so", "DobbyHook");
        if (!dobby) {
            LOG_ERROR("[gui] DobbyHook not found; Android GUI disabled");
            return;
        }
        using dobby_hook_fn = int (*)(void *, void *, void **);
        const auto hook = reinterpret_cast<dobby_hook_fn>(dobby);

        void *swap = resolve_loaded_symbol("libEGL.so", "eglSwapBuffers");
        if (!swap) {
            LOG_ERROR("[gui] eglSwapBuffers not found; Android GUI disabled");
            return;
        }
        if (hook(swap, reinterpret_cast<void *>(&swap_hook), reinterpret_cast<void **>(&p_orig_swap)) != 0) {
            LOG_ERROR("[gui] failed to hook eglSwapBuffers");
            return;
        }

        // 触摸输入：定位 UnityEngine.Input.touches（采样在主线程 Main.Draw 后置里做）
        if (const patch_handle_t input = patchlib_type_get_type("UnityEngine", "Input")) {
            g_input_touches = patchlib_type_get_method(input, "get_touches");
            LOG_INFO("[gui] input: Input.touches={}", reinterpret_cast<void *>(g_input_touches));
        } else {
            LOG_WARN("[gui] UnityEngine.Input not found; touch disabled");
        }

        // 在主线程每帧采样输入：hook Terraria.Main.Draw（后置）
        if (const patch_handle_t main_type = patchlib_type_get_type("Terraria", "Main")) {
            patch_handle_t draw = patchlib_type_get_method_by_param_count(main_type, "Draw", 1);
            if (!draw) draw = patchlib_type_get_method(main_type, "Draw");
            if (draw) {
                g_draw_hook = patchlib_install_prepost_hook(draw, nullptr, frame_postfix);
                LOG_INFO("[gui] hooked Main.Draw for input sampling (id={})", static_cast<int>(g_draw_hook));
            } else {
                LOG_WARN("[gui] Main.Draw not found; input sampling disabled");
            }
        }

        g_installed = true;
        LOG_INFO("[gui] hooked eglSwapBuffers (Android/GLES), ImGui GUI enabled");
    }

    void shutdown() {
        // Dobby 无卸载接口，置标志即可
        g_installed = false;
    }
#else
    void install() {
        if (g_installed) return;

        const patch_handle_t main_type = patchlib_type_get_type("Terraria", "Main");
        if (!main_type) {
            LOG_WARN("[gui] Terraria.Main not found, GUI disabled");
            return;
        }
        g_f_screen_w = patchlib_type_get_field(main_type, "screenWidth");
        g_f_screen_h = patchlib_type_get_field(main_type, "screenHeight");

        patch_handle_t draw = patchlib_type_get_method_by_param_count(main_type, "Draw", 1);
        if (!draw) draw = patchlib_type_get_method(main_type, "Draw");
        if (!draw) {
            LOG_WARN("[gui] Main.Draw not found, GUI disabled");
            return;
        }
        g_draw_hook = patchlib_install_prepost_hook(draw, nullptr, draw_postfix);
        if (g_draw_hook == PATCH_HOOK_INVALID_ID) {
            LOG_ERROR("[gui] failed to hook Main.Draw");
            return;
        }
        g_installed = true;
        LOG_INFO("[gui] hooked Main.Draw (id={}), ImGui GUI enabled", static_cast<int>(g_draw_hook));
    }

    void shutdown() {
        if (g_draw_hook != PATCH_HOOK_INVALID_ID) {
            patchlib_uninstall_hook(g_draw_hook);
            g_draw_hook = PATCH_HOOK_INVALID_ID;
        }
        g_installed = false;
    }
#endif

    namespace {
        int l_gui_text(lua_State *L) {
            ImGui::TextUnformatted(luaL_checkstring(L, 1));
            return 0;
        }

        int l_gui_text_wrapped(lua_State *L) {
            ImGui::TextWrapped("%s", luaL_checkstring(L, 1));
            return 0;
        }

        int l_gui_button(lua_State *L) {
            lua_pushboolean(L, ImGui::Button(luaL_checkstring(L, 1)));
            return 1;
        }

        int l_gui_checkbox(lua_State *L) {
            const char *label = luaL_checkstring(L, 1);
            bool v = lua_toboolean(L, 2) != 0;
            ImGui::Checkbox(label, &v);
            lua_pushboolean(L, v);
            return 1;
        }

        int l_gui_slider(lua_State *L) {
            const char *label = luaL_checkstring(L, 1);
            float v = static_cast<float>(luaL_checknumber(L, 2));
            const float mn = static_cast<float>(luaL_optnumber(L, 3, 0.0));
            const float mx = static_cast<float>(luaL_optnumber(L, 4, 1.0));
            ImGui::SliderFloat(label, &v, mn, mx);
            lua_pushnumber(L, v);
            return 1;
        }

        int l_gui_same_line(lua_State *) {
            ImGui::SameLine();
            return 0;
        }

        int l_gui_separator(lua_State *) {
            ImGui::Separator();
            return 0;
        }

        int l_gui_spacing(lua_State *) {
            ImGui::Spacing();
            return 0;
        }

        int l_gui_collapsing_header(lua_State *L) {
            lua_pushboolean(L, ImGui::CollapsingHeader(luaL_checkstring(L, 1)));
            return 1;
        }

        int l_gui_progress_bar(lua_State *L) {
            ImGui::ProgressBar(static_cast<float>(luaL_checknumber(L, 1)));
            return 0;
        }

        // 自绘输入框 + 虚拟键盘（不使用 ImGui::InputText，避免点虚拟键时失焦）
        int l_gui_input_text(lua_State *L) {
            const char *label = luaL_checkstring(L, 1);
            const char *value = luaL_optstring(L, 2, "");
            const ImGuiID id = ImGui::GetID(label);
            const void *key = reinterpret_cast<const void *>(static_cast<uintptr_t>(id));
            const bool editing = (g_edit_id == key);
            const float width = ImGui::GetContentRegionAvail().x;
            const ImVec2 box(width > 0 ? width : 240.0f, 0);

            if (!editing) {
                ImGui::PushID(label);
                if (ImGui::Button(value[0] ? value : "(点击输入)", box)) {
                    g_edit_id = key;
                    std::snprintf(g_edit_buf, sizeof(g_edit_buf), "%s", value);
                }
                ImGui::PopID();
            } else {
                // 可选：同时接受物理键盘（编辑时消费字符队列与功能键）
                ImGuiIO &io = ImGui::GetIO();
                for (const ImWchar c: io.InputQueueCharacters) {
                    if (c >= 32 && c < 127) vkb_append_char(static_cast<char>(c));
                }
                io.InputQueueCharacters.resize(0);
                if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) vkb_backspace();
                if (ImGui::IsKeyPressed(ImGuiKey_Enter)) g_edit_id = nullptr;
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g_edit_id = nullptr;

                char display[VKB_BUF_SIZE + 2];
                std::snprintf(display, sizeof(display), "%s|", g_edit_buf);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.35f, 0.55f, 1.0f));
                ImGui::Button(display, box);
                ImGui::PopStyleColor();
            }

            lua_pushstring(L, editing ? g_edit_buf : value);
            return 1;
        }

        const luaL_Reg gui_funcs[] = {
                {"text", l_gui_text},
                {"text_wrapped", l_gui_text_wrapped},
                {"button", l_gui_button},
                {"checkbox", l_gui_checkbox},
                {"slider", l_gui_slider},
                {"same_line", l_gui_same_line},
                {"separator", l_gui_separator},
                {"spacing", l_gui_spacing},
                {"collapsing_header", l_gui_collapsing_header},
                {"progress_bar", l_gui_progress_bar},
                {"input_text", l_gui_input_text},
                {nullptr, nullptr},
        };
    } // namespace

    void register_api(lua_State *L, lua_mod_handle_t *) {
        // 在 mod 表（栈顶）上挂 mod.gui
        lua_newtable(L);
        luaL_setfuncs(L, gui_funcs, 0);
        lua_setfield(L, -2, "gui");
    }

} // namespace lualoader::gui
