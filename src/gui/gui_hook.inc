// ====================================================================
// 帧钩子
//
// 在游戏帧里驱动 GUI：
//   - Main.Draw 后置     → render_guarded()：绘制所有 Mod 窗口
//   - Main.Update 前置    → update_prefix()：指针在 GUI 上时屏蔽游戏输入
//   - Android 光标层钩子   → 接管被 GUI 吃掉的手指，避免点击穿透到世界
// ====================================================================

patch_hook_id_t g_draw_hook = PATCH_HOOK_INVALID_ID;
patch_hook_id_t g_below_hook = PATCH_HOOK_INVALID_ID;
patch_hook_id_t g_update_hook = PATCH_HOOK_INVALID_ID;
patch_hook_id_t g_pi_hook = PATCH_HOOK_INVALID_ID;     // PlayerInput.Update（IME 启用用）
patch_hook_id_t g_cursor_hook = PATCH_HOOK_INVALID_ID; // Main.DrawCursor（GUI 时接管光标）
bool g_installed = false;
bool g_in_render = false;
int g_render_layer = 1;          // 0 = HUD 下方, 1 = HUD 上方
unsigned g_frame_token = 0;      // Main.Update 每帧自增
unsigned g_input_token = ~0u;    // 最近一次采样输入所属帧
#if defined(__ANDROID__)
patch_hook_id_t g_layer_hook = PATCH_HOOK_INVALID_ID;
patch_hook_id_t g_cursor_list_hook = PATCH_HOOK_INVALID_ID;
std::unordered_map<int, bool> g_captured_touches;

/// 这个手指（cursor id）是否已被 GUI 捕获（从按下到抬起都算）。
bool cursor_captured(void *cursor) {
    const auto f = g_gfx.f_cursor_id;
    auto *id = f && cursor ? static_cast<int *>(patchlib_field_get_pointer(f, cursor)) : nullptr;
    return id && g_captured_touches.count(*id);
}

/// 手指抬起后释放捕获状态。
/// Cursor 对象会存活到抬起帧，所以这里按 down==false 清理。
void release_finished_touches() {
    if (!g_gfx.f_cm_instance || !g_gfx.m_cm_get_num_cursors || !g_gfx.m_cm_get_cursor)
        return;
    void *instance = read_static_object(g_gfx.f_cm_instance);
    if (!instance)
        return;
    auto count = reinterpret_cast<FnGetNumCursors>(patchlib_method_get_pointer(g_gfx.m_cm_get_num_cursors));
    auto at = reinterpret_cast<FnGetCursor>(patchlib_method_get_pointer(g_gfx.m_cm_get_cursor));
    if (!count || !at)
        return;
    const auto f = g_gfx.f_cursor_id;
    for (int i = 0, n = count(instance); i < n; ++i) {
        void *c = at(instance, i);
        if (!c)
            continue;
        auto *id = f ? static_cast<int *>(patchlib_field_get_pointer(f, c)) : nullptr;
        auto *down = static_cast<unsigned char *>(patchlib_field_get_pointer(g_gfx.f_cursor_down, c));
        if (id && down && !*down)
            g_captured_touches.erase(*id);
    }
}

/// 判断并捕获一个手指：
///   - 落点在 GUI 内 → 记录 id，并设置 Cursor.ignoreWorld/ignore；
///   - 一旦捕获，后续帧即使移出窗口也保持捕获，直到抬起。
/// 返回 true 表示这次点击不应传给游戏世界。
bool capture_cursor(void *cursor) {
    if (!cursor || !g_gfx.f_cursor_pos || !g_gfx.f_cursor_down || !g_gfx.m_adjust_real_screen)
        return false;
    auto *position = static_cast<Vec2 *>(patchlib_field_get_pointer(g_gfx.f_cursor_pos, cursor));
    auto *down = static_cast<unsigned char *>(patchlib_field_get_pointer(g_gfx.f_cursor_down, cursor));
    const auto id_field = g_gfx.f_cursor_id;
    auto *id = id_field ? static_cast<int *>(patchlib_field_get_pointer(id_field, cursor)) : nullptr;
    if (!position || !down || !id)
        return false;
    auto adjust = reinterpret_cast<FnAdjustRealScreenForUI>(patchlib_method_get_pointer(g_gfx.m_adjust_real_screen));
    if (!adjust)
        return false;
    const Vec2 pos = adjust(*position);
    // Once claimed, keep this finger captured through its release frame.
    if (*down && point_in_gui(pos.x, pos.y))
        g_captured_touches[*id] = true;
    if (!g_captured_touches.count(*id))
        return false;
    for (auto field : {g_gfx.f_cursor_ignore_world, g_gfx.f_cursor_ignore}) {
        if (field) {
            auto *v = static_cast<unsigned char *>(patchlib_field_get_pointer(field, cursor));
            if (v)
                *v = 1;
        }
    }
    return true;
}

/// 光标层前缀钩子：在游戏处理该 Cursor 前决定是否吞掉。
bool cursor_layer_prefix(patch_handle_t, void **args, const patch_method_signature_t *, void *) {
    return args && args[0] && capture_cursor(*static_cast<void **>(args[0]));
}

/// 光标列表后置钩子：遍历当前所有 Cursor，更新捕获集合，
/// 并清掉已经消失的 id（对象会存活到抬起帧，不能按 down 立即删）。
void cursor_list_postfix(patch_handle_t, void **, void *, const patch_method_signature_t *) {
    if (!g_gfx.f_cm_instance || !g_gfx.m_cm_get_num_cursors || !g_gfx.m_cm_get_cursor)
        return;
    void *instance = read_static_object(g_gfx.f_cm_instance);
    if (!instance)
        return;
    auto count = reinterpret_cast<FnGetNumCursors>(patchlib_method_get_pointer(g_gfx.m_cm_get_num_cursors));
    auto at = reinterpret_cast<FnGetCursor>(patchlib_method_get_pointer(g_gfx.m_cm_get_cursor));
    if (!count || !at)
        return;
    std::unordered_map<int, bool> live;
    const auto id_field = g_gfx.f_cursor_id;
    for (int i = 0, n = count(instance); i < n; ++i) {
        void *c = at(instance, i);
        if (!c)
            continue;
        auto *id = id_field ? static_cast<int *>(patchlib_field_get_pointer(id_field, c)) : nullptr;
        if (id)
            live[*id] = true;
        capture_cursor(c);
    }
    // Cursor objects persist for the release frame. Prune only disappeared IDs.
    for (auto it = g_captured_touches.begin(); it != g_captured_touches.end();) {
        if (!live.count(it->first))
            it = g_captured_touches.erase(it);
        else
            ++it;
    }
}
#endif

/// 当前指针是否落在任一已打开窗口内（用上一帧缓存的位置）
bool pointer_over_windows() {
    if (g_windows.empty() && !g_edit.active)
        return false;
#if defined(__ANDROID__)
    if (!g_mouse_down && !g_prev_mouse_down)
        return false;
    return point_in_gui(g_mouse_x, g_mouse_y);
#else
    if (!g_gfx.usable || !g_gfx.a_mouse_x.valid() || !g_gfx.a_mouse_left.valid())
        return false;
    const float mx = static_cast<float>(read_int(g_gfx.a_mouse_x, 0));
    const float my = static_cast<float>(read_int(g_gfx.a_mouse_y, 0));
    return point_in_gui(mx, my);
#endif
}

/// Main.Update 前缀：指针悬停在 GUI 上时屏蔽游戏鼠标，避免误触游戏操作
bool update_prefix(patch_handle_t, void **, const patch_method_signature_t *, void *) {
    ++g_frame_token;
#if defined(__ANDROID__)
    void *cm_instance = g_gfx.f_cm_instance ? read_static_object(g_gfx.f_cm_instance) : nullptr;
    if (cm_instance && g_gfx.m_cm_get_num_cursors && g_gfx.m_cm_get_cursor && g_gfx.m_adjust_real_screen &&
        g_gfx.f_cursor_pos && g_gfx.f_cursor_down) {
        auto fn_num_cursors =
            reinterpret_cast<FnGetNumCursors>(patchlib_method_get_pointer(g_gfx.m_cm_get_num_cursors));
        auto fn_get_cursor = reinterpret_cast<FnGetCursor>(patchlib_method_get_pointer(g_gfx.m_cm_get_cursor));
        auto fn_adjust =
            reinterpret_cast<FnAdjustRealScreenForUI>(patchlib_method_get_pointer(g_gfx.m_adjust_real_screen));

        if (fn_num_cursors && fn_get_cursor && fn_adjust) {
            const int num = fn_num_cursors(cm_instance);
            for (int i = 0; i < num; ++i) {
                void *c = fn_get_cursor(cm_instance, i);
                if (!c)
                    continue;
                auto *pos_ptr = static_cast<Vec2 *>(patchlib_field_get_pointer(g_gfx.f_cursor_pos, c));
                auto *down_ptr = static_cast<unsigned char *>(patchlib_field_get_pointer(g_gfx.f_cursor_down, c));
                if (!pos_ptr || !down_ptr || *down_ptr == 0)
                    continue;

                const Vec2 ui_pos = fn_adjust(*pos_ptr);
                if (point_in_gui(ui_pos.x, ui_pos.y) || cursor_captured(c)) {
                    if (g_gfx.f_cursor_ignore_world) {
                        auto *p =
                            static_cast<unsigned char *>(patchlib_field_get_pointer(g_gfx.f_cursor_ignore_world, c));
                        if (p)
                            *p = 1;
                    }
                    if (g_gfx.f_cursor_ignore) {
                        auto *p = static_cast<unsigned char *>(patchlib_field_get_pointer(g_gfx.f_cursor_ignore, c));
                        if (p)
                            *p = 1;
                    }
                    write_bool(g_gfx.a_mouse_left, false);
                    write_bool(g_gfx.a_mouse_right, false);
                }
            }
        }
    }
#endif
    if (pointer_over_windows()) {
        write_bool(g_gfx.a_mouse_left, false);
        write_bool(g_gfx.a_mouse_right, false);
    }
    return false; // 不跳过原函数
}

/// 每个 Main.Update 只采样一次输入/切换一次图片帧；HUD 上下两层共用。
void ensure_input_frame() {
    if (g_input_token == g_frame_token)
        return;
    g_input_token = g_frame_token;
    update_screen_size();
    update_input();
    images::begin_frame();
    g_wheel_delta = read_int(g_gfx.a_scroll_wheel, 0);
    g_wheel_used = false;
    g_hover_gui = false;
    g_scroll_drag = false;
    g_viewport = {};
}

bool any_named_window_layer(const int layer) {
    for (const auto &kv : g_named_windows) {
        const NamedWindow &nw = kv.second;
        if (nw.ws.visible && nw.layer == layer && mod_alive(nw.mod_id))
            return true;
    }
    return false;
}

bool any_named_window() {
    for (const auto &kv : g_named_windows)
        if (mod_alive(kv.second.mod_id))
            return true;
    return false;
}

void prune_named_windows() {
    for (auto it = g_named_windows.begin(); it != g_named_windows.end();) {
        if (!it->second.destroyed && mod_alive(it->second.mod_id)) {
            ++it;
            continue;
        }
        if (it->second.destroyed && it->second.L) {
            if (it->second.handle_ref != LUA_NOREF)
                luaL_unref(it->second.L, LUA_REGISTRYINDEX, it->second.handle_ref);
            if (it->second.draw_ref != LUA_NOREF)
                luaL_unref(it->second.L, LUA_REGISTRYINDEX, it->second.draw_ref);
        }
        it = g_named_windows.erase(it);
    }
}

/// 绘制某一层的具名窗口（0 = HUD 下方，1 = HUD 上方）。
void render_named_layer(const int layer) {
    if (layer == 1)
        prune_named_windows();
    if (!any_named_window_layer(layer))
        return;
    if (read_bool(g_gfx.a_show_splash, false))
        return;

#if defined(__ANDROID__)
    struct NamedUIRenderScope {
        ~NamedUIRenderScope() {
            if (g_gfx.m_setup_rendering_ui) {
                unsigned char v = 0;
                void *args[1] = {&v};
                patchlib_method_invoke_args(g_gfx.m_setup_rendering_ui, nullptr, nullptr, args);
            }
        }
    } named_ui_scope;
    if (g_gfx.m_setup_rendering_ui) {
        unsigned char v = 1;
        void *args[1] = {&v};
        patchlib_method_invoke_args(g_gfx.m_setup_rendering_ui, nullptr, nullptr, args);
    }
#endif

    ensure_input_frame();
    g_viewport = {};
    g_scroll_drag = false;

    patch_handle_t sb = read_static_object(g_gfx.f_sprite_batch);
    struct SbGuard {
        patch_handle_t h;
        ~SbGuard() { patchlib_free(h); }
    } sb_guard{sb};
    if (!sb || !patchlib_is_valid(sb))
        return;

    // 指针归属：只认本层窗口。
    g_input_owner.clear();
    if (!g_pointer_owner.empty()) {
        auto it = g_named_windows.find(g_pointer_owner);
        if (it != g_named_windows.end() && it->second.layer == layer && it->second.ws.visible)
            g_input_owner = g_pointer_owner;
    }
    if (g_input_owner.empty() && g_mouse_pressed) {
        NamedWindow *top = nullptr;
        for (auto &kv : g_named_windows) {
            NamedWindow &nw = kv.second;
            if (!nw.ws.visible || nw.layer != layer || !mod_alive(nw.mod_id))
                continue;
            const float h = nw.ws.collapsed ? title_height() : nw.ws.h;
            if (g_mouse_x >= nw.ws.x && g_mouse_x <= nw.ws.x + nw.ws.w && g_mouse_y >= nw.ws.y &&
                g_mouse_y <= nw.ws.y + h && (!top || nw.order >= top->order))
                top = &nw;
        }
        if (top) {
            g_input_owner = top->key;
            g_pointer_owner = top->key;
            top->order = ++g_next_window_order;
        }
    }

    std::vector<NamedWindow *> list;
    for (auto &kv : g_named_windows) {
        NamedWindow &nw = kv.second;
        if (!nw.ws.visible || nw.layer != layer || !mod_alive(nw.mod_id))
            continue;
        if (nw.order == 0)
            nw.order = ++g_next_window_order;
        list.push_back(&nw);
    }
    std::sort(list.begin(), list.end(), [](const NamedWindow *a, const NamedWindow *b) {
        return a->order < b->order;
    });

    g_sb = sb;
    sb_begin();
    for (NamedWindow *nw : list) {
        draw_named_window(*nw);
        g_active_named = nullptr;
        g_render_owner.clear();
    }
    sb_end();
    g_sb = nullptr;
    g_style_window = nullptr;

    if (g_hover_gui) {
        write_bool(g_gfx.a_mouse_left, false);
        write_bool(g_gfx.a_mouse_right, false);
    }
    if (!g_mouse_down)
        g_drag_id.clear();
}

/// 具名窗口绘制的异常保护版本。
void render_named_guarded(const int layer) {
    if (g_in_render)
        return;
    struct Guard {
        ~Guard() {
            g_in_render = false;
            g_recording = false;
            g_ctx = nullptr;
            g_active_named = nullptr;
            g_current_mod.clear();
            g_render_owner.clear();
            g_cmds.clear();
            g_scroll_stack.clear();
            g_viewport = {};
            g_sb = nullptr;
        }
    } guard;
    g_in_render = true;
    try {
        render_named_layer(layer);
    } catch (...) {
        LOG_ERROR("[gui] named window render exception (layer {})", layer);
        if (g_sb) {
            try {
                sb_end();
            } catch (...) {
            }
        }
        g_click_state.reset();
        g_drag_id.clear();
        g_slider_active.clear();
        g_pointer_owner.clear();
    }
}

/// HUD 下方那一层的绘制入口（Main.DrawInterface 前缀）。
bool frame_below_prefix(patch_handle_t, void **, const patch_method_signature_t *, void *) {
    if (!g_installed || g_in_render)
        return false;
    g_render_layer = 0;
    resolve_gfx();
    if (!g_gfx.usable)
        return false;
    render_named_guarded(0);
    return false;
}

/// 绘制一帧 GUI：解析图形后端 → 更新输入 → 画窗口/文件夹/图标 → 提交 SpriteBatch。
void render() {
    resolve_gfx();
    if (!g_gfx.usable)
        return;

    // 没有任何带 on_gui 的 Mod 时完全不介入渲染
    bool any_gui = false;
    for (auto &kv : core::lua_mod_handles) {
        if (kv.second && kv.second->gui_ref != LUA_NOREF) {
            any_gui = true;
            break;
        }
    }
    if (!any_gui) {
        if (!any_named_window()) {
            clear_text_cache();
            images::clear();
            g_windows.clear();
            g_headers.clear();
            g_edit = EditState{};
            ime::close();
            kbd::reset();
            g_scrolls.clear();
            g_active_scroll.clear();
            g_pointer_owner.clear();
            g_input_owner.clear();
            g_drag_id.clear();
            g_slider_active.clear();
            g_click_state.reset();
            g_render_owner.clear();
            g_mouse_down = g_prev_mouse_down = false;
#if defined(__ANDROID__)
            g_active_cursor_valid = false;
            g_captured_touches.clear();
#endif
            return;
        }
        // 只有具名窗口：继续走清理/图片修剪，但不画 legacy 窗口。
    }

    // 启动画面（showSplash）阶段贴图/字体尚未就绪，此时绘制会导致
    // DrawInvBG / DrawBorderString 解引用空资源而崩溃，必须跳过。
    if (read_bool(g_gfx.a_show_splash, false))
        return;

#if defined(__ANDROID__)
    struct UIRenderScope {
        ~UIRenderScope() {
            if (g_gfx.m_setup_rendering_ui) {
                unsigned char v = 0;
                void *args[1] = {&v};
                patchlib_method_invoke_args(g_gfx.m_setup_rendering_ui, nullptr, nullptr, args);
            }
        }
    } ui_scope;

    if (g_gfx.m_setup_rendering_ui) {
        unsigned char v = 1;
        void *args[1] = {&v};
        patchlib_method_invoke_args(g_gfx.m_setup_rendering_ui, nullptr, nullptr, args);
    }
#endif

    std::vector<std::string> owners;
    for (const auto &kv : core::lua_mod_handles)
        if (kv.second && kv.second->gui_ref != LUA_NOREF)
            owners.push_back(kv.second->mod_id);
    for (const auto &kv : g_named_windows) {
        if (!mod_alive(kv.second.mod_id))
            continue;
        const std::string &id = kv.second.mod_id;
        if (std::find(owners.begin(), owners.end(), id) == owners.end())
            owners.push_back(id);
    }
    std::vector<std::string> prefixes;
    prefixes.reserve(owners.size() + g_named_windows.size());
    for (const auto &id : owners)
        prefixes.push_back(id + "#");
    for (const auto &kv : g_named_windows)
        if (mod_alive(kv.second.mod_id))
            prefixes.push_back(kv.second.key + "#");
    images::prune(owners);
    auto owner_alive = [&prefixes](const std::string &key) {
        for (const auto &p : prefixes)
            if (key.rfind(p, 0) == 0)
                return true;
        return false;
    };
    if (g_edit.active && !owner_alive(g_edit.owner)) {
        g_edit = EditState{};
        ime::close();
        kbd::reset();
    }
    for (auto it = g_scrolls.begin(); it != g_scrolls.end();)
        if (!owner_alive(it->first))
            it = g_scrolls.erase(it);
        else
            ++it;
    if (!g_active_scroll.empty() && !owner_alive(g_active_scroll))
        g_active_scroll.clear();
    for (auto it = g_headers.begin(); it != g_headers.end();) {
        if (!owner_alive(it->first))
            it = g_headers.erase(it);
        else
            ++it;
    }
    for (auto it = g_member_gestures.begin(); it != g_member_gestures.end();)
        if (!window_live(it->first))
            it = g_member_gestures.erase(it);
        else
            ++it;
    for (auto it = g_windows.begin(); it != g_windows.end();) {
        bool live = false;
        for (const auto &id : owners)
            if (id == it->first) {
                live = true;
                break;
            }
        if (!live) {
            g_position_applied.erase(it->first);
            it = g_windows.erase(it);
        } else
            ++it;
    }
    for (const auto &id : g_image_release_queue)
        images::release(id);
    g_image_release_queue.clear();
    ensure_input_frame();

    patch_handle_t sb = read_static_object(g_gfx.f_sprite_batch);

    // 桌面端 read_static_object 每次都会新建 GCHandle，必须在本帧结束时释放，
    // 否则渲染钩子每帧都会泄漏一个句柄（越玩越卡的主因之一）。
    struct StaticObjectGuard {
        patch_handle_t h;

        ~StaticObjectGuard() {
            patchlib_free(h);
        }
    } sb_guard{sb};

    if (!sb || !patchlib_is_valid(sb))
        return;

    g_sb = sb;
    sb_begin();

    std::vector<lua_mod_handle_t *> windows;
    for (auto &kv : core::lua_mod_handles)
        if (kv.second && kv.second->gui_ref != LUA_NOREF) {
            auto &ws = g_windows[kv.second->mod_id];
            if (ws.order == 0)
                ws.order = ++g_next_window_order;
            const float access = ws.collapsed ? (ws.skin_icon.empty() ? title_height() : ws.icon_size) : title_height();
            ws.x = std::max(0.0f, std::min(ws.x, std::max(0.0f, g_screen_w - access)));
            ws.y = std::max(0.0f, std::min(ws.y, std::max(0.0f, g_screen_h - access)));
            windows.push_back(kv.second);
        }
    if (!g_pointer_owner.empty() && g_pointer_owner != "#keyboard" &&
        g_pointer_owner.rfind("#folder:", 0) != 0 && !g_named_windows.count(g_pointer_owner)) {
        auto it = g_windows.find(g_pointer_owner);
        if (it == g_windows.end() || !it->second.visible)
            g_pointer_owner.clear();
    }
    for (auto *h : windows) {
        if (!g_position_applied[h->mod_id]) {
            auto pos = g_saved_positions.find(h->mod_id);
            if (pos != g_saved_positions.end()) {
                g_windows[h->mod_id].x = pos->second.first;
                g_windows[h->mod_id].y = pos->second.second;
            }
            g_position_applied[h->mod_id] = true;
        }
    }
    g_input_owner = g_pointer_owner;
    if (g_named_windows.count(g_input_owner))
        g_input_owner.clear();
    if (g_input_owner.empty())
        g_input_owner = folder_hit(g_mouse_x, g_mouse_y);
    if (g_mouse_pressed && g_input_owner.rfind("#folder:", 0) == 0)
        g_pointer_owner = g_input_owner;
    if (g_input_owner == "#keyboard" && !g_edit.active)
        g_input_owner.clear();
    if (g_edit.active && !ime::supported() && !kbd::supported() &&
        keyboard_contains(g_mouse_x, g_mouse_y, g_scale, g_screen_w, g_screen_h))
        g_input_owner = "#keyboard";
    if (g_input_owner.empty()) {
        int top = -1;
        for (auto *h : windows) {
            const auto &ws = g_windows[h->mod_id];
            if (!ws.visible || grouped_icon(h->mod_id))
                continue;
            const bool compact = ws.collapsed;
            const float w = compact ? (ws.skin_icon.empty() ? title_height() : ws.icon_size) : ws.w;
            const float height = compact ? w : ws.collapsed ? title_height() : ws.h;
            if (g_mouse_x >= ws.x && g_mouse_x <= ws.x + w && g_mouse_y >= ws.y && g_mouse_y <= ws.y + height &&
                ws.order >= top) {
                top = ws.order;
                g_input_owner = h->mod_id;
            }
        }
        if (g_mouse_pressed && !g_input_owner.empty()) {
            g_pointer_owner = g_input_owner;
            g_windows[g_input_owner].order = ++g_next_window_order;
        }
    }
    std::sort(windows.begin(), windows.end(),
              [](auto *a, auto *b) { return g_windows[a->mod_id].order < g_windows[b->mod_id].order; });
    for (auto *h : windows) {
        if (!grouped_icon(h->mod_id))
            draw_mod_gui(h);
        g_render_owner.clear();
    }
    draw_folders();
    if (!g_mouse_down) {
        g_drag_id.clear();
        g_slider_active.clear();
        g_pointer_owner.clear();
    }

#if defined(__ANDROID__)
    if (!g_mouse_down)
        g_active_cursor_valid = false;
#endif

    g_style_window = nullptr;
    sb_end();
    g_sb = nullptr;
}

/// 每帧最后一步：文本输入提交 / 点击状态收尾 / 触摸释放 / 输入屏蔽。
/// 放在 legacy 与具名窗口都画完之后统一执行一次。
void finalize_gui_frame() {
    if (!g_installed)
        return;

    if (g_edit.active) {
        if (ime::supported()) {
            std::string t;
            const int st = ime::poll(t);
            if (st == 0)
                g_edit.buf = t;
            else if (st == 1 || st == 3) {
                g_edit.buf = t;
                g_edit.active = false;
            } else {
                g_edit.active = false;
            }
        } else if (kbd::supported()) {
            kbd::Event ev;
            kbd::poll(ev);
            if (!ev.ime.empty())
                g_edit.buf += ev.ime;
            for (char ch : ev.append)
                g_edit.buf.push_back(ch);
            if (ev.backspace)
                utf8_pop_back(g_edit.buf);
            if (ev.enter || ev.escape)
                g_edit.active = false;
        } else {
            patch_handle_t sb = read_static_object(g_gfx.f_sprite_batch);
            if (sb && patchlib_is_valid(sb)) {
                g_sb = sb;
                sb_begin();
                g_cmds.clear();
                g_recording = true;
                draw_virtual_keyboard();
                g_recording = false;
                flush_cmds();
                sb_end();
                g_sb = nullptr;
            }
            patchlib_free(sb);
        }
        if (!g_edit.active)
            kbd::reset();
    }
    if (!g_mouse_down)
        g_active_scroll.clear();
    g_click_state.finish(g_mouse_down, g_mouse_released);

#if defined(__ANDROID__)
    if (!g_mouse_down)
        g_active_cursor_valid = false;
    release_finished_touches();
#endif

    if (g_hover_gui) {
        write_bool(g_gfx.a_mouse_left, false);
        write_bool(g_gfx.a_mouse_right, false);
    }
}

/// render 的带异常保护版本：
///   - 防重入（g_in_render）
///   - 无论正常返回还是抛异常，都通过 RenderGuard 把本帧状态清干净
///   - 异常时额外尝试结束 SpriteBatch，避免把游戏的绘制状态带坏
void render_guarded() {
    if (g_in_render)
        return;

    struct RenderGuard {
        ~RenderGuard() {
            g_in_render = false;
            g_recording = false;
            g_ctx = nullptr;
            g_sb = nullptr;
            g_style_window = nullptr;
            g_current_mod.clear();
            g_render_owner.clear();
            g_cmds.clear();
            g_scroll_stack.clear();
            g_viewport = {};
            g_scroll_drag = false;
        }
    } guard;

    g_in_render = true;
    try {
        render();
    } catch (...) {
        LOG_ERROR("[gui] render exception; resetting frame state");
        if (g_sb) {
            try {
                sb_end();
            } catch (...) {
            }
        }
        g_click_state.reset();
        g_drag_id.clear();
        g_slider_active.clear();
        g_pointer_owner.clear();
    }
}

/// HUD 上方窗口的绘制入口：挂在 DrawInterface_36_Cursor **之前**。
/// 原版随后用 UIScaleMatrix / SamplerStateForCursor 自己画指针，
/// 因此窗口永远压在指针下面，且不会出现两套鼠标样式/缩放。
///
/// 此时前一个 HUD 层还开着 SpriteBatch，不能再 Begin/End 自己的批次：
/// 置 g_batch_borrowed 借用当前批次（UI 缩放、采样状态与原版一致）。
bool cursor_frame_prefix(patch_handle_t, void **, const patch_method_signature_t *, void *) {
    if (!g_installed || g_in_render)
        return false;
    if (g_update_hook == PATCH_HOOK_INVALID_ID)
        ++g_frame_token;
    g_render_layer = 1;
    resolve_gfx();
    if (!g_gfx.usable)
        return false;

    struct BorrowGuard {
        ~BorrowGuard() {
            g_batch_borrowed = false;
            g_sb = nullptr;
        }
    } guard;
    g_batch_borrowed = true;
    render_named_guarded(1);
    render_guarded();
    finalize_gui_frame();
    return false;
}

/// Main.Draw 后置：仅在 Cursor 层钩子不可用时作为兜底绘制入口。
void frame_postfix(patch_handle_t, void **, void *, const patch_method_signature_t *) {
    if (!g_installed || g_in_render || g_cursor_hook != PATCH_HOOK_INVALID_ID)
        return;
    if (g_update_hook == PATCH_HOOK_INVALID_ID)
        ++g_frame_token;
    g_render_layer = 1;
    resolve_gfx();
    if (!g_gfx.usable)
        return;
    render_named_guarded(1);
    render_guarded();
    finalize_gui_frame();
}
