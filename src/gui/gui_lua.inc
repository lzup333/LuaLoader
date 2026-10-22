        // ====================================================================
        // Lua GUI 函数
        // ====================================================================

        int l_gui_begin_scroll(lua_State *L){
            const char *id=luaL_checkstring(L,1);
            float x=luaL_checknumber(L,2),y=luaL_checknumber(L,3),w=luaL_checknumber(L,4),h=luaL_checknumber(L,5);
            float cw=luaL_checknumber(L,6),ch=luaL_checknumber(L,7);
            if(!g_ctx||g_scroll_stack.size()>=8||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(w)||!std::isfinite(h)||!std::isfinite(cw)||!std::isfinite(ch)||w<=0||h<=0||cw<0||ch<0)return luaL_error(L,"invalid scroll viewport");
            const std::string key=g_current_mod+"#"+id;
            if(!g_scrolls.count(key)&&g_scrolls.size()>=4096)return luaL_error(L,"scroll ID limit exceeded");
            auto &m=g_scrolls[key];
            Viewport bounds{true,g_ctx->ox+x,g_ctx->oy+y,w,h};
            float mx=std::max(0.0f,cw-w),my=std::max(0.0f,ch-h);m.clamp(mx,my);
            bool hit=mouse_in(bounds.x,bounds.y,w,h);
            if(hit)g_hover_gui=true;
            if(hit&&g_mouse_pressed&&!g_active_scroll.empty()&&!g_scroll_stack.empty()){
                auto old=g_scrolls.find(g_active_scroll);
                if(old!=g_scrolls.end()&&!old->second.bar_x&&!old->second.bar_y){old->second.active=false;g_active_scroll.clear();}
            }
            if(hit&&g_mouse_pressed&&g_active_scroll.empty()){
                g_active_scroll=key;m.begin(g_mouse_x,g_mouse_y,mx>0&&g_mouse_y>=bounds.y+h-12*g_scale,my>0&&g_mouse_x>=bounds.x+w-12*g_scale);
                if(m.bar_x||m.bar_y)g_click_state.armed=key+"#bar";
            }
            if(g_active_scroll==key){
                if(g_mouse_down||g_mouse_released)m.move(g_mouse_x,g_mouse_y,mx,my,
                    w-std::min(w,std::max(16*g_scale,cw>0?w*w/cw:w)),
                    h-std::min(h,std::max(16*g_scale,ch>0?h*h/ch:h)),6*g_scale);
                if(m.dragged){g_scroll_drag=true;g_click_state.armed=key+"#scroll";}
                if(!g_mouse_down){m.active=false;g_active_scroll.clear();}
            }
            if(hit&&!g_wheel_used&&g_wheel_delta){
                if(my>0)m.y-=g_wheel_delta*0.5f;else m.x-=g_wheel_delta*0.5f;
                m.clamp(mx,my);g_wheel_used=true;
            }
            ScrollFrame f;f.parent=g_ctx;f.old_clip=g_viewport;f.old_drag=g_scroll_drag;f.motion=&m;f.bounds=bounds;f.content_w=cw;f.content_h=ch;
            f.child.ox=bounds.x-m.x;f.child.oy=bounds.y-m.y;f.child.cw=cw;
            g_scroll_stack.push_back(f);g_ctx=&g_scroll_stack.back().child;
            g_viewport=bounds;if(f.old_clip.enabled){float px=bounds.x,py=bounds.y,pw=w,ph=h;f.old_clip.crop(px,py,pw,ph);g_viewport={true,px,py,std::max(0.0f,pw),std::max(0.0f,ph)};}
            lua_pushnumber(L,m.x);lua_pushnumber(L,m.y);lua_pushnumber(L,w);lua_pushnumber(L,h);return 4;
        }
        void end_scroll(){
            if(g_scroll_stack.empty())return;auto f=g_scroll_stack.back();g_scroll_stack.pop_back();g_ctx=f.parent;g_viewport=f.old_clip;
            float track=std::max(3.0f,5*g_scale);auto &m=*f.motion;
            if(f.content_h>f.bounds.h){float thumb=std::max(16*g_scale,f.bounds.h*f.bounds.h/f.content_h);thumb=std::min(thumb,f.bounds.h);
                float y=f.bounds.y+(f.bounds.h-thumb)*m.y/(f.content_h-f.bounds.h);
                draw_rect(f.bounds.x+f.bounds.w-track,y,track,thumb,kBorder);}
            if(f.content_w>f.bounds.w){float thumb=std::max(16*g_scale,f.bounds.w*f.bounds.w/f.content_w);thumb=std::min(thumb,f.bounds.w);
                float x=f.bounds.x+(f.bounds.w-thumb)*m.x/(f.content_w-f.bounds.w);
                draw_rect(x,f.bounds.y+f.bounds.h-track,thumb,track,kBorder);}
            // Keep click cancellation for the release frame, including siblings.
        }
        int l_gui_end_scroll(lua_State *){end_scroll();return 0;}
        int l_gui_scroll_to(lua_State *L){
            const char *id=luaL_checkstring(L,1);float x=luaL_checknumber(L,2),y=luaL_checknumber(L,3);
            if(!std::isfinite(x)||!std::isfinite(y))return luaL_error(L,"invalid scroll offset");
            const std::string key=g_current_mod+"#"+id;
            if(!g_scrolls.count(key)&&g_scrolls.size()>=4096)return luaL_error(L,"scroll ID limit exceeded");
            auto &m=g_scrolls[key];m.x=std::max(0.0f,x);m.y=std::max(0.0f,y);return 0;
        }
        int l_gui_image_cache_stats(lua_State *L){size_t used,limit,entries;uint64_t evictions;images::stats(used,limit,entries,evictions);lua_pushinteger(L,used);lua_pushinteger(L,limit);lua_pushinteger(L,entries);lua_pushinteger(L,evictions);return 4;}
        Color lua_color(lua_State *L,int index);
        int l_gui_text(lua_State *L) {
            const char *s = luaL_checkstring(L, 1);
            if (!g_ctx) return 0;
            float x, y;
            const float w = text_width(s);
            const float h = line_height();
            place(x, y, w, h);
            Color color=lua_isnoneornil(L,2)?themed_color(kText):lua_color(L,2);
            draw_string(x, y, s, color, 1.0f);
            return 0;
        }

        int l_gui_text_wrapped(lua_State *L) {
            const char *s = luaL_checkstring(L, 1);
            Color color=lua_isnoneornil(L,2)?themed_color(kText):lua_color(L,2);
            if (!g_ctx) return 0;
            // 按内容宽度粗略换行
            const float max_w = g_ctx->cw;
            float x, y;
            place(x, y, g_ctx->cw, line_height());
            float cur_x = x, cur_y = y;
            float line_w = 0.0f;
            std::string line;
            for (const char *p = s; *p;) {
                const size_t n = utf8_next(p);
                const std::string ch(p, n);
                const float cw = text_width(ch.c_str());
                if (line_w + cw > max_w && !line.empty()) {
                    draw_string(cur_x, cur_y, line.c_str(), color, 1.0f);
                    cur_y += line_height();
                    cur_x = x;
                    line_w = 0.0f;
                    line.clear();
                }
                line += ch;
                line_w += cw;
                p += n;
            }
            if (!line.empty()) draw_string(cur_x, cur_y, line.c_str(), color, 1.0f);
            g_ctx->prev_h = (cur_y - y) + line_height();
            return 0;
        }

        int l_gui_separator(lua_State *) {
            if (!g_ctx) return 0;
            advance_line();
            float x, y;
            place(x, y, g_ctx->cw, 2.0f);
            Color c;
            make_color(c, 120, 130, 170, 255);
            draw_rect(x, y, g_ctx->cw, 1.0f * g_scale, c);
            g_ctx->prev_h = 6.0f * g_scale;
            return 0;
        }

        int l_gui_spacing(lua_State *) {
            if (!g_ctx) return 0;
            advance_line();
            g_ctx->prev_h = line_height() * 0.5f;
            return 0;
        }

        int l_gui_same_line(lua_State *) {
            if (g_ctx) g_ctx->same_line = true;
            return 0;
        }

        int l_gui_button(lua_State *L) {
            const char *label = luaL_checkstring(L, 1);
            if (!g_ctx) {
                lua_pushboolean(L, 0);
                return 1;
            }
            float x, y;
            const float w = text_width(label) + 24.0f * g_scale;
            const float h = button_height();
            place(x, y, w, h);

            const bool hovered = mouse_in(x, y, w, h);
            if (hovered) g_hover_gui = true;
            bool clicked = false;
            clicked=widget_click(label,x,y,hovered);
            const float g1 = 1.0f * g_scale;
            draw_rect(x, y, w, h, kTrackEdge);                                   // 按钮边框
            draw_rect(x + g1, y + g1, w - 2 * g1, h - 2 * g1,
                      hovered ? (g_mouse_down ? kBtnActive : kBtnHover) : kBtn);
            draw_string(x + 12.0f * g_scale, y + (h - line_height()) * 0.5f, label, kText, 1.0f);
            lua_pushboolean(L, clicked);
            return 1;
        }

        int l_gui_checkbox(lua_State *L) {
            const char *label = luaL_checkstring(L, 1);
            bool value = lua_toboolean(L, 2) != 0;
            if (!g_ctx) {
                lua_pushboolean(L, value);
                return 1;
            }
            const float box = 18.0f * g_scale;
            const float h = line_height();
            const float w = box + item_gap() + text_width(label);
            float x, y;
            place(x, y, w, h);

            const bool hovered = mouse_in(x, y, w, h);
            if (hovered) g_hover_gui = true;
            if (widget_click(label,x,y,hovered)) value = !value;

            const float by = y + (h - box) * 0.5f;
            const float g1 = 1.0f * g_scale;
            draw_rect(x, by, box, box, kTrackEdge);                                  // 外边框
            draw_rect(x + g1, by + g1, box - 2 * g1, box - 2 * g1, value ? kFill : kTrack);
            draw_string(x + box + item_gap(), y, label, kText, 1.0f);
            lua_pushboolean(L, value);
            return 1;
        }

        int l_gui_slider(lua_State *L) {
            const char *label = luaL_checkstring(L, 1);
            float value = static_cast<float>(luaL_checknumber(L, 2));
            const float mn = static_cast<float>(luaL_optnumber(L, 3, 0.0));
            const float mx = static_cast<float>(luaL_optnumber(L, 4, 1.0));
            if (!g_ctx) {
                lua_pushnumber(L, value);
                return 1;
            }

            const float h = button_height();
            const float w = g_ctx->cw;
            float x, y;
            place(x, y, w, h);

            char text[128];
            std::snprintf(text, sizeof(text), "%s: %.2f", label, value);
            draw_string(x, y + (h - line_height()) * 0.5f, text, kText, 1.0f);

            // 轨道在右侧
            const float track_w = w * 0.45f;
            const float track_x = x + w - track_w;
            const float track_h = 10.0f * g_scale;
            const float track_y = y + (h - track_h) * 0.5f;

            const std::string id = g_current_mod + "#" + label;
            const bool hovered = mouse_in(track_x, y, track_w, h);
            if (hovered || g_slider_active == id) g_hover_gui = true;
            if (hovered && g_mouse_pressed) g_slider_active = id;
            if (!g_mouse_down && g_slider_active == id) g_slider_active.clear();

            float ratio = 0.0f;
            if (g_slider_active == id && g_mouse_down) {
                ratio = (g_mouse_x - track_x) / track_w;
                if (ratio < 0.0f) ratio = 0.0f;
                if (ratio > 1.0f) ratio = 1.0f;
                value = mn + (mx - mn) * ratio;
            } else {
                ratio = (mx != mn) ? (value - mn) / (mx - mn) : 0.0f;
                if (ratio < 0.0f) ratio = 0.0f;
                if (ratio > 1.0f) ratio = 1.0f;
            }

            const float g1 = 1.0f * g_scale;
            draw_rect(track_x, track_y, track_w, track_h, kTrackEdge);           // 轨道边框
            draw_rect(track_x + g1, track_y + g1, track_w - 2 * g1, track_h - 2 * g1, kTrack);
            draw_rect(track_x + g1, track_y + g1, (track_w - 2 * g1) * ratio, track_h - 2 * g1, kFill);
            // 旋钮
            const float knob_w = 8.0f * g_scale;
            const float kx = track_x + g1 + (track_w - 2 * g1) * ratio - knob_w * 0.5f;
            draw_rect(kx, track_y - 2.0f * g_scale, knob_w, track_h + 4.0f * g_scale, kWhite);
            lua_pushnumber(L, value);
            return 1;
        }

        int l_gui_progress_bar(lua_State *L) {
            const float value = static_cast<float>(luaL_checknumber(L, 1));
            if (!g_ctx) return 0;
            float x, y;
            const float w = g_ctx->cw;
            const float h = 18.0f * g_scale;
            place(x, y, w, h);
            float ratio = value;
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;
            const float g1 = 1.0f * g_scale;
            draw_rect(x, y, w, h, kTrackEdge);                                   // 外边框
            draw_rect(x + g1, y + g1, w - 2 * g1, h - 2 * g1, kTrack);
            draw_rect(x + g1, y + g1, (w - 2 * g1) * ratio, h - 2 * g1, kFill);
            char ptxt[16];
            std::snprintf(ptxt, sizeof(ptxt), "%d%%", static_cast<int>(ratio * 100.0f + 0.5f));
            draw_string(x + (w - text_width(ptxt)) * 0.5f, y + (h - line_height()) * 0.5f, ptxt, kText, 1.0f);
            return 0;
        }

        int l_gui_collapsing_header(lua_State *L) {
            const char *label = luaL_checkstring(L, 1);
            if (!g_ctx) {
                lua_pushboolean(L, 0);
                return 1;
            }
            const std::string id = g_current_mod + "#" + label;
            bool open = g_headers[id];
            float x, y;
            const float w = g_ctx->cw;
            const float h = button_height();
            place(x, y, w, h);
            const bool hovered = mouse_in(x, y, w, h);
            if (hovered) g_hover_gui = true;
            if (widget_click(label,x,y,hovered)) open = !open;
            g_headers[id] = open;
            const float g1 = 1.0f * g_scale;
            draw_rect(x, y, w, h, kTrackEdge);
            draw_rect(x + g1, y + g1, w - 2 * g1, h - 2 * g1, hovered ? kBtnHover : kBtn);
            const std::string text = (open ? "v " : "> ") + std::string(label);
            draw_string(x + 8.0f * g_scale, y + (h - line_height()) * 0.5f, text.c_str(), kText, 1.0f);
            lua_pushboolean(L, open);
            return 1;
        }

        int l_gui_input_text(lua_State *L) {
            const char *label = luaL_checkstring(L, 1);
            const char *value = luaL_optstring(L, 2, "");
            if (!g_ctx) {
                lua_pushstring(L, value);
                return 1;
            }
            const std::string id = g_current_mod + "#" + label;
            const bool editing = g_edit.active && g_edit.owner == id;

            float x, y;
            const float w = g_ctx->cw;
            const float h = button_height();
            place(x, y, w, h);
            const bool hovered = mouse_in(x, y, w, h);
            if (hovered) g_hover_gui = true;
            if (hovered && g_mouse_pressed) {
                if(g_click_state.armed.empty())g_click_state.armed=g_render_owner+"#input-focus";
                g_edit.active = true;
                g_edit.owner = id;
                g_edit.buf = value;
                if (ime::supported()) {
                    ime::open(value);            // Android：弹出系统键盘（支持中文输入法）
                } else if (kbd::supported()) {
                    kbd::reset();
                }
            }

            const std::string shown = editing ? (g_edit.buf + g_edit.pinyin + "|") : std::string(value[0] ? value : "(tap to edit)");
            draw_rect(x, y, w, h, editing ? kBtnActive : (hovered ? kBtnHover : kBtn));
            draw_string(x + 8.0f * g_scale, y + (h - line_height()) * 0.5f, shown.c_str(), kText, 1.0f);

            lua_pushstring(L, editing ? g_edit.buf.c_str() : value);
            return 1;
        }

        // --------------------------------------------------------------------
        // 窗口定位 / 显示控制：允许 Mod 指定绘制位置、控制出现与消失
        // --------------------------------------------------------------------
        WindowState *current_window() {
            if (g_current_mod.empty()) return nullptr;
            return &g_windows[g_current_mod];
        }

        /// mod.gui.set_pos(x, y)  —— 设置窗口左上角（屏幕像素）
        int l_gui_set_pos(lua_State *L) {
            const float x = static_cast<float>(luaL_checknumber(L, 1));
            const float y = static_cast<float>(luaL_checknumber(L, 2));
            if (WindowState *ws = current_window()) {
                if(!std::isfinite(x)||!std::isfinite(y))return luaL_error(L,"invalid window position");
                auto saved=g_saved_positions.find(g_current_mod);
                if(ws->initial_position&&saved!=g_saved_positions.end()){ws->x=saved->second.first;ws->y=saved->second.second;}
                else{ws->x=x;ws->y=y;}
            }
            return 0;
        }

        /// mod.gui.get_pos() -> x, y
        int l_gui_get_pos(lua_State *L) {
            WindowState *ws = current_window();
            lua_pushnumber(L, ws ? ws->x : 0.0);
            lua_pushnumber(L, ws ? ws->y : 0.0);
            return 2;
        }

        /// mod.gui.set_width(w) —— 设置窗口宽度（高度由内容自动撑开）
        int l_gui_set_width(lua_State *L) {
            const float w = static_cast<float>(luaL_checknumber(L, 1));
            if (WindowState *ws = current_window()) {
                if (w > 60.0f) ws->w = w;
            }
            return 0;
        }

        /// mod.gui.set_cursor(x, y) —— 移动当前布局光标（相对窗口内容区左上角），
        /// 之后绘制的控件从该位置开始（用于自由摆放）
        int l_gui_set_cursor(lua_State *L) {
            const float x = static_cast<float>(luaL_checknumber(L, 1));
            const float y = static_cast<float>(luaL_checknumber(L, 2));
            if (g_ctx) {
                g_ctx->cx = x;
                g_ctx->cy = y;
                g_ctx->prev_h = 0.0f;
                g_ctx->same_line = false;
            }
            return 0;
        }

        /// mod.gui.set_visible(v) -> v
        int l_gui_set_visible(lua_State *L) {
            const bool v = lua_toboolean(L, 1) != 0;
            if (WindowState *ws = current_window()) ws->visible = v;
            lua_pushboolean(L, v);
            return 1;
        }

        /// mod.gui.is_visible() -> bool
        int l_gui_is_visible(lua_State *L) {
            WindowState *ws = current_window();
            lua_pushboolean(L, ws && ws->visible);
            return 1;
        }

        int l_gui_show(lua_State *) {
            if (WindowState *ws = current_window()) ws->visible = true;
            return 0;
        }

        int l_gui_hide(lua_State *) {
            if (WindowState *ws = current_window()) ws->visible = false;
            return 0;
        }

        /// mod.gui.set_collapsed(v) -> v
        int l_gui_set_collapsed(lua_State *L) {
            const bool v = lua_toboolean(L, 1) != 0;
            if (WindowState *ws = current_window()) ws->collapsed = v;
            lua_pushboolean(L, v);
            return 1;
        }

        /// mod.gui.is_collapsed() -> bool
        int l_gui_is_collapsed(lua_State *L) {
            WindowState *ws = current_window();
            lua_pushboolean(L, ws && ws->collapsed);
            return 1;
        }

        /// mod.gui.toggle_collapsed() —— 收起/展开
        int l_gui_toggle_collapsed(lua_State *) {
            if (WindowState *ws = current_window()) ws->collapsed = !ws->collapsed;
            return 0;
        }

        Color lua_color(lua_State *L, int index) {
            luaL_checktype(L, index, LUA_TTABLE);
            index=lua_absindex(L,index);const char *names[]={"r","g","b","a"};
            int values[4] = {255, 255, 255, 255};
            for (int i = 0; i < 4; ++i) {
                lua_rawgeti(L, index, i + 1);
                if(lua_isnil(L,-1)){lua_pop(L,1);lua_getfield(L,index,names[i]);}
                if (!lua_isnil(L, -1)) {
                    const lua_Integer v = luaL_checkinteger(L, -1);
                    values[i] = v < 0 ? 0 : v > 255 ? 255 : static_cast<int>(v);
                }
                lua_pop(L, 1);
            }
            Color c; make_color(c, values[0], values[1], values[2], values[3]); return c;
        }
        int l_gui_set_style(lua_State *L) {
            luaL_checktype(L, 1, LUA_TTABLE);
            WindowState *ws = current_window();
            if (!ws) return 0;
            const char *keys[] = {"panel", "title", "button", "hover", "active", "border", "text"};
            for (const char *key : keys) {
                lua_getfield(L, 1, key);
                if (!lua_isnil(L, -1)) ws->colors[key] = lua_color(L, lua_gettop(L));
                lua_pop(L, 1);
            }
            return 0;
        }
        // Absolute positions relative to the window content area, in screen pixels.
        int l_gui_rect(lua_State *L) {
            float x=luaL_checknumber(L,1), y=luaL_checknumber(L,2);
            float w=luaL_checknumber(L,3), h=luaL_checknumber(L,4);
            Color c=lua_color(L,5);
            if (g_ctx && w>0 && h>0) draw_rect(g_ctx->ox+x,g_ctx->oy+y,w,h,c);
            return 0;
        }
        int l_gui_text_at(lua_State *L) {
            float x=luaL_checknumber(L,1), y=luaL_checknumber(L,2);
            const char *text=luaL_checkstring(L,3);
            Color c=lua_color(L,4);
            float scale=luaL_optnumber(L,5,1.0f);
            if (g_ctx && scale>0 && scale<=4) draw_string(g_ctx->ox+x,g_ctx->oy+y,text,c,scale);
            return 0;
        }
        int l_gui_button_at(lua_State *L) {
            float x=luaL_checknumber(L,1), y=luaL_checknumber(L,2);
            float w=luaL_checknumber(L,3), h=luaL_checknumber(L,4);
            const char *text=luaL_checkstring(L,5);
            if (!g_ctx || w<=0 || h<=0) { lua_pushboolean(L,false); return 1; }
            x+=g_ctx->ox; y+=g_ctx->oy;
            const bool hover=mouse_in(x,y,w,h);
            if (hover) g_hover_gui=true;
            draw_rect(x,y,w,h,hover ? kBtnHover : kBtn);
            draw_string(x+(w-text_width(text))*0.5f,y+(h-line_height())*0.5f,text,kText);
            lua_pushboolean(L,widget_click(text,x,y,hover)); return 1;
        }
        int l_gui_set_content_height(lua_State *L) {
            const float height=luaL_checknumber(L,1);
            if (g_ctx && height>=0) {g_ctx->cy=height;g_ctx->prev_h=0;}
            return 0;
        }

        bool image_owned(const char *id) {return std::string(id).rfind(g_current_mod+"#",0)==0;}
        int l_gui_load_image(lua_State *L) {
            const char *path=luaL_checkstring(L,1);
            std::string root;
            for(auto &kv:core::lua_mod_handles)if(kv.second && kv.second->mod_id==g_current_mod)root=kv.second->private_dir;
            const auto id=root.empty()?std::string():images::request(g_current_mod,root,path);
            if(id.empty())lua_pushnil(L);else lua_pushlstring(L,id.data(),id.size());return 1;
        }
        int l_gui_release_image(lua_State *L) {
            const char *id=luaL_checkstring(L,1);if(image_owned(id))g_image_release_queue.push_back(id);return 0;
        }
        int l_gui_image_status(lua_State *L) {
            const char *id=luaL_checkstring(L,1);int w=0,h=0;
            const auto state=image_owned(id)?images::status(id,w,h):"invalid";
            lua_pushlstring(L,state.data(),state.size());lua_pushinteger(L,w);lua_pushinteger(L,h);return 3;
        }
        int l_gui_image(lua_State *L) {
            const char *id=luaL_checkstring(L,1);
            float x=luaL_checknumber(L,2),y=luaL_checknumber(L,3),w=luaL_checknumber(L,4),h=luaL_checknumber(L,5);
            Color c=lua_isnoneornil(L,6)?kWhite:lua_color(L,6);
            int src[4]={0,0,0,0};
            if(!lua_isnoneornil(L,7)){luaL_checktype(L,7,LUA_TTABLE);for(int i=0;i<4;++i){lua_rawgeti(L,7,i+1);src[i]=luaL_checkinteger(L,-1);lua_pop(L,1);}}
            if(g_ctx && image_owned(id))draw_image(id,g_ctx->ox+x,g_ctx->oy+y,w,h,c,src[0],src[1],src[2],src[3]);return 0;
        }
        int l_gui_image_fit(lua_State *L) {
            const char *id=luaL_checkstring(L,1);
            float x=luaL_checknumber(L,2),y=luaL_checknumber(L,3),w=luaL_checknumber(L,4),h=luaL_checknumber(L,5);
            const char *mode=luaL_optstring(L,6,"contain");
            Color c=lua_isnoneornil(L,7)?kWhite:lua_color(L,7);
            const int fit=std::strcmp(mode,"cover")==0?-2:-1;
            if(g_ctx && image_owned(id))draw_image(id,g_ctx->ox+x,g_ctx->oy+y,w,h,c,0,0,0,0,fit);return 0;
        }
        int l_gui_nine_slice(lua_State *L) {
            const char *id=luaL_checkstring(L,1);
            float x=luaL_checknumber(L,2),y=luaL_checknumber(L,3),w=luaL_checknumber(L,4),h=luaL_checknumber(L,5);
            int border=luaL_checkinteger(L,6);Color c=lua_isnoneornil(L,7)?kWhite:lua_color(L,7);
            float edge=luaL_optnumber(L,8,0);
            if(g_ctx && image_owned(id) && border>0)draw_image(id,g_ctx->ox+x,g_ctx->oy+y,w,h,c,0,0,0,0,border,edge);return 0;
        }
        int l_gui_image_button(lua_State *L) {
            const char *id=luaL_checkstring(L,1);
            float x=luaL_checknumber(L,2),y=luaL_checknumber(L,3),w=luaL_checknumber(L,4),h=luaL_checknumber(L,5);
            const char *label=luaL_optstring(L,6,"");
            int border=luaL_optinteger(L,7,0);
            if(!g_ctx || !image_owned(id)||w<=0||h<=0){lua_pushboolean(L,false);return 1;}
            x+=g_ctx->ox;y+=g_ctx->oy;const bool hovered=mouse_in(x,y,w,h);
            if(hovered)g_hover_gui=true;
            Color tint;make_color(tint,hovered?255:235,hovered?255:235,hovered?255:235,255);
            draw_image(id,x,y,w,h,tint,0,0,0,0,border);
            if(*label){
                const float estimated=text_width(label);
                const float available=std::max(1.0f,w-16.0f*g_scale);
                const float font_scale=estimated>available?available/estimated:1.0f;
                draw_string(x+(w-estimated*font_scale)*0.5f,
                            y+(h-line_height()*font_scale)*0.5f,label,kText,font_scale);
            }
            lua_pushboolean(L,widget_click(label,x,y,hovered));return 1;
        }
        int l_gui_set_skin(lua_State *L) {
            const char *panel=luaL_checkstring(L,1),*icon=luaL_checkstring(L,3);
            const int border=luaL_checkinteger(L,2);float size=luaL_optnumber(L,4,48);float edge=luaL_optnumber(L,5,0);
            if(WindowState *ws=current_window()){
                if(image_owned(panel)&&image_owned(icon)&&border>0&&size>=24&&size<=128){
                    ws->skin_panel=panel;ws->skin_icon=icon;ws->skin_border=border;ws->skin_edge=edge;ws->icon_size=size;
                }
            }return 0;
        }

        // Optional title and compact launcher; unchanged defaults for other mods.
        int l_gui_set_title(lua_State *L) {
            const char *title = luaL_checkstring(L, 1);
            const char *icon = luaL_optstring(L, 2, "");
            if (WindowState *ws = current_window()) {
                ws->title = title;
                ws->compact_label = icon;
            }
            return 0;
        }

        const luaL_Reg gui_funcs[] = {
                {"begin_scroll", l_gui_begin_scroll},
                {"end_scroll", l_gui_end_scroll},
                {"scroll_to", l_gui_scroll_to},
                {"image_cache_stats", l_gui_image_cache_stats},
                {"load_image", l_gui_load_image},
                {"image_status", l_gui_image_status},
                {"release_image", l_gui_release_image},
                {"image", l_gui_image},
                {"nine_slice", l_gui_nine_slice},
                {"image_fit", l_gui_image_fit},
                {"image_button", l_gui_image_button},
                {"set_skin", l_gui_set_skin},
                {"set_style", l_gui_set_style},
                {"rect", l_gui_rect},
                {"text_at", l_gui_text_at},
                {"button_at", l_gui_button_at},
                {"set_content_height", l_gui_set_content_height},
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
                {"set_title", l_gui_set_title},
                {"set_pos", l_gui_set_pos},
                {"get_pos", l_gui_get_pos},
                {"set_width", l_gui_set_width},
                {"set_cursor", l_gui_set_cursor},
                {"set_visible", l_gui_set_visible},
                {"is_visible", l_gui_is_visible},
                {"show", l_gui_show},
                {"hide", l_gui_hide},
                {"set_collapsed", l_gui_set_collapsed},
                {"is_collapsed", l_gui_is_collapsed},
                {"toggle_collapsed", l_gui_toggle_collapsed},
                {nullptr, nullptr},
        };

