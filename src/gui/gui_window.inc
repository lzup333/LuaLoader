        // ====================================================================
        // 每个 Mod 的窗口状态
        // ====================================================================

        struct WindowState {
            float x{40}, y{40};
            float w{380}, h{160};
            bool visible{true};   ///< 是否显示（Mod 可控制出现/消失）
            std::unordered_map<std::string, Color> colors;
            std::string title;
            std::string skin_panel,skin_icon;
            int skin_border=16;
            float skin_edge=0;
            float icon_size=48;
            IconGesture icon_gesture;
            std::string compact_label; // opt-in: square collapsed launcher
            bool collapsed{false}; ///< 是否收起为标题栏
            int order{0};
            bool initial_position=true;
        };

        std::unordered_map<std::string, WindowState> g_windows;

        struct IconFolder {std::string id;float x=40,y=40;std::vector<std::string> members;IconGesture gesture;};
        std::vector<IconFolder> g_folders;
        std::string g_open_folder,g_merge_target,g_merge_source,g_layout_path;
        std::unordered_map<std::string,IconGesture> g_member_gestures;
        std::unordered_map<std::string,std::pair<float,float>> g_saved_positions;
        std::unordered_map<std::string,bool> g_position_applied;
        std::string g_member_drag;float g_member_press_time=0;
        auto g_merge_since=std::chrono::steady_clock::now();
        int g_folder_serial=0;bool g_folder_layout_dirty=false;
        bool window_live(const std::string &id){for(const auto &kv:core::lua_mod_handles)if(kv.second&&kv.second->mod_id==id&&kv.second->gui_ref!=LUA_NOREF)return true;return false;}
        std::vector<std::string> folder_members(const IconFolder &f){std::vector<std::string> v;for(auto &id:f.members){auto it=g_windows.find(id);if(window_live(id)&&it!=g_windows.end()&&it->second.visible)v.push_back(id);}return v;}
        bool grouped_icon(const std::string &id){
            auto it=g_windows.find(id);if(it==g_windows.end()||!it->second.collapsed)return false;
            for(const auto &f:g_folders)if(folder_members(f).size()>=2&&std::find(f.members.begin(),f.members.end(),id)!=f.members.end())return true;
            return false;
        }
        Viewport folder_icon(const IconFolder &f){float sz=48*g_scale;return {true,f.x,f.y,sz,sz};}
        Viewport folder_panel(const IconFolder &f){
            const float cell=64*g_scale,w=3*cell+16*g_scale,h=3*cell+40*g_scale;
            return {true,std::clamp(f.x,0.0f,std::max(0.0f,g_screen_w-w)),std::clamp(f.y+52*g_scale,0.0f,std::max(0.0f,g_screen_h-h)),w,h};
        }
        std::unordered_map<std::string,int> g_folder_pages;
        std::string folder_hit(float x,float y){for(auto it=g_folders.rbegin();it!=g_folders.rend();++it){if(folder_members(*it).size()<2)continue;
            if(folder_icon(*it).contains(x,y)||(it->id==g_open_folder&&folder_panel(*it).contains(x,y)))return "#folder:"+it->id;}return {};}
        auto g_layout_retry=std::chrono::steady_clock::time_point::min();
        void save_layout(){
            if(g_layout_path.empty()||!g_folder_layout_dirty||std::chrono::steady_clock::now()<g_layout_retry)return;
            try{nlohmann::json j;j["version"]=1;j["folders"]=nlohmann::json::array();
                for(const auto &f:g_folders)j["folders"].push_back({{"id",f.id},{"x",f.x},{"y",f.y},{"members",f.members}});
                for(const auto &kv:g_windows){g_saved_positions[kv.first]={kv.second.x,kv.second.y};}
                for(const auto &kv:g_saved_positions)j["positions"][kv.first]={kv.second.first,kv.second.second};
                const auto tmp=g_layout_path+".tmp";std::ofstream out(tmp,std::ios::trunc);out<<j.dump(2);out.close();
                if(!out)throw std::runtime_error("layout write failed");
                std::error_code ec;std::filesystem::rename(tmp,g_layout_path,ec);
                if(ec){
                    const auto backup=g_layout_path+".bak";ec.clear();
                    if(std::filesystem::exists(g_layout_path))std::filesystem::rename(g_layout_path,backup,ec);
                    if(ec)throw std::runtime_error("cannot preserve old layout");
                    std::filesystem::rename(tmp,g_layout_path,ec);
                    if(ec){std::error_code restore;std::filesystem::rename(backup,g_layout_path,restore);throw std::runtime_error("cannot replace layout");}
                    std::filesystem::remove(backup,ec);
                }
                g_folder_layout_dirty=false;
            }catch(const std::exception &e){LOG_WARN("[gui-folder] save failed: {}",e.what());g_layout_retry=std::chrono::steady_clock::now()+std::chrono::seconds(5);}
        }
        void load_layout(const std::string &root){
            g_layout_path=(std::filesystem::path(root)/"gui_layout.json").string();g_folders.clear();g_saved_positions.clear();g_position_applied.clear();g_folder_serial=0;
            try{std::ifstream in(g_layout_path);if(!in)return;auto j=nlohmann::json::parse(in);if(j.value("version",0)!=1)return;
                std::unordered_map<std::string,bool> seen;
                if(j.contains("folders"))for(const auto &v:j["folders"]){if(g_folders.size()>=64)break;IconFolder f;f.id=std::to_string(++g_folder_serial);f.x=v.value("x",40.0f);f.y=v.value("y",40.0f);if(!std::isfinite(f.x)||!std::isfinite(f.y))continue;
                    for(const auto &m:v.at("members")){auto id=m.get<std::string>();if(id.size()>256||seen[id]||f.members.size()>=64)continue;seen[id]=true;f.members.push_back(id);}
                    if(f.members.size()>=2)g_folders.push_back(std::move(f));}
                if(j.contains("positions"))for(auto it=j["positions"].begin();it!=j["positions"].end();++it){if(g_saved_positions.size()>=4096)break;
                    float x=it.value().at(0).get<float>(),y=it.value().at(1).get<float>();if(std::isfinite(x)&&std::isfinite(y))g_saved_positions[it.key()]={x,y};}
            }catch(const std::exception &e){LOG_WARN("[gui-folder] invalid layout: {}",e.what());g_folders.clear();}
        }
        void merge_icon(const std::string &source,const std::string &target){
            if(source.empty()||source==target)return;
            if(target.rfind("#folder:",0)==0){for(auto &f:g_folders)if("#folder:"+f.id==target&&f.members.size()<64){f.members.push_back(source);break;}}
            else {if(g_folders.size()>=64)return;IconFolder f;f.id=std::to_string(++g_folder_serial);f.x=g_windows[target].x;f.y=g_windows[target].y;f.members={target,source};g_folders.push_back(std::move(f));}
            g_merge_target.clear();g_merge_source.clear();g_folder_layout_dirty=true;
        }
        void track_merge(const std::string &source){
            auto &ws=g_windows[source];std::string target;
            const float size=ws.skin_icon.empty()?title_height():ws.icon_size;
            const float cx=ws.x+size/2,cy=ws.y+size/2;
            for(const auto &f:g_folders)if(folder_members(f).size()>=2&&folder_icon(f).contains(cx,cy)){target="#folder:"+f.id;break;}
            if(target.empty())for(const auto &kv:g_windows){if(kv.first==source||!kv.second.collapsed||!kv.second.visible||grouped_icon(kv.first))continue;
                const float size=kv.second.skin_icon.empty()?title_height():kv.second.icon_size;
                Viewport box{true,kv.second.x,kv.second.y,size,size};if(box.contains(cx,cy)){target=kv.first;break;}}
            if(source!=g_merge_source||target!=g_merge_target){g_merge_source=source;g_merge_target=target;g_merge_since=std::chrono::steady_clock::now();}
            if(g_mouse_released&&!target.empty()&&std::chrono::steady_clock::now()-g_merge_since>=std::chrono::milliseconds(350))merge_icon(source,target);
        }
        void draw_folders();
        WindowState *g_style_window = nullptr;
        Color themed_color(const Color &c) {
            if (!g_style_window) return c;
            const char *key = &c == &kPanel ? "panel" : &c == &kTitle ? "title" :
                &c == &kBtn ? "button" : &c == &kBtnHover ? "hover" :
                &c == &kBtnActive ? "active" : &c == &kBorder ? "border" :
                &c == &kTrackEdge ? "border" : &c == &kText ? "text" : nullptr;
            if (!key) return c;
            auto it = g_style_window->colors.find(key);
            return it == g_style_window->colors.end() ? c : it->second;
        }
        std::string g_drag_id;
        float g_drag_dx = 0.0f, g_drag_dy = 0.0f;

        std::string g_slider_active;
        std::unordered_map<std::string, bool> g_headers;

        // 文本输入状态
        struct EditState {
            bool active{false};
            std::string owner;
            std::string buf;
            bool shift{false};
            // 内置拼音输入法（PC：硬件键盘打拼音 -> 候选栏选词）
            std::string pinyin;              // 当前拼音串（小写字母）
            std::vector<std::string> cands;  // 当前候选（最多 90，按页展示）
            bool ime_on{true};               // 中/英（Shift 单击切换）
            size_t page{0};                  // 候选翻页
        };
        EditState g_edit;

        void draw_folders(){
            g_viewport={};g_scroll_drag=false;g_style_window=nullptr;g_recording=true;
            if(g_mouse_pressed&&folder_hit(g_mouse_x,g_mouse_y).empty()){
                g_open_folder.clear();g_member_gestures.clear();g_member_drag.clear();
            }
            for(auto &f:g_folders){auto members=folder_members(f);if(members.size()<2)continue;
                const float size=48*g_scale;f.x=std::clamp(f.x,0.0f,std::max(0.0f,g_screen_w-size));f.y=std::clamp(f.y,0.0f,std::max(0.0f,g_screen_h-size));
                const std::string owner="#folder:"+f.id;g_render_owner=owner;
                const bool hover=mouse_in(f.x,f.y,size,size);if(hover)g_hover_gui=true;
                auto move=f.gesture.update(g_mouse_x,g_mouse_y,g_mouse_pressed&&hover,g_mouse_down,g_mouse_released,hover,f.x,f.y,6*g_scale);
                if(move.move){f.x=std::clamp(move.x,0.0f,std::max(0.0f,g_screen_w-size));f.y=std::clamp(move.y,0.0f,std::max(0.0f,g_screen_h-size));if(g_mouse_released)g_folder_layout_dirty=true;}
                if(move.tap)g_open_folder=g_open_folder==f.id?"":f.id;
                draw_rect(f.x,f.y,size,size,kTitle);
                for(size_t i=0;i<std::min(size_t(4),members.size());++i){auto &ws=g_windows[members[i]];float x=f.x+5*g_scale+(i%2)*20*g_scale,y=f.y+5*g_scale+(i/2)*20*g_scale;
                    if(!ws.skin_icon.empty())draw_image(ws.skin_icon,x,y,18*g_scale,18*g_scale,kWhite);else draw_rect(x,y,18*g_scale,18*g_scale,kBtnHover);}
                if(g_open_folder!=f.id)continue;
                auto panel=folder_panel(f);draw_rect(panel.x,panel.y,panel.w,panel.h,kPanel);
                int &page=g_folder_pages[f.id];const int pages=(int(members.size())+8)/9;page=std::clamp(page,0,pages-1);
                const bool prev=mouse_in(panel.x,panel.y,40*g_scale,24*g_scale),next=mouse_in(panel.x+panel.w-40*g_scale,panel.y,40*g_scale,24*g_scale);
                if(widget_click("folder-prev",panel.x,panel.y,prev))page=std::max(0,page-1);
                if(widget_click("folder-next",panel.x+panel.w-40*g_scale,panel.y,next))page=std::min(pages-1,page+1);
                draw_string(panel.x+8*g_scale,panel.y+2*g_scale,"<",kText,0.7f);
                draw_string(panel.x+panel.w-25*g_scale,panel.y+2*g_scale,">",kText,0.7f);
                for(int i=page*9;i<std::min(int(members.size()),page*9+9);++i){auto id=members[i];auto &ws=g_windows[id];int slot=i-page*9;
                    float x=panel.x+8*g_scale+(slot%3)*64*g_scale,y=panel.y+28*g_scale+(slot/3)*64*g_scale;
                    bool hit=mouse_in(x,y,56*g_scale,56*g_scale);if(hit)g_hover_gui=true;
                    auto &gesture=g_member_gestures[id];
                    auto r=gesture.update(g_mouse_x,g_mouse_y,g_mouse_pressed&&hit,g_mouse_down,g_mouse_released,hit,x,y,8*g_scale);
                    if(r.move){g_member_drag=id;draw_rect(r.x,r.y,48*g_scale,48*g_scale,kBtnHover);
                        if(g_mouse_released&&!panel.contains(g_mouse_x,g_mouse_y)){
                            ws.x=std::clamp(r.x,0.0f,std::max(0.0f,g_screen_w-ws.icon_size));ws.y=std::clamp(r.y,0.0f,std::max(0.0f,g_screen_h-ws.icon_size));ws.collapsed=true;
                            f.members.erase(std::remove(f.members.begin(),f.members.end(),id),f.members.end());g_open_folder.clear();g_folder_layout_dirty=true;
                        }}
                    if(r.tap){ws.collapsed=false;ws.order=++g_next_window_order;g_open_folder.clear();}
                    if(!ws.skin_icon.empty())draw_image(ws.skin_icon,x+8*g_scale,y,40*g_scale,40*g_scale,kWhite);else draw_rect(x+8*g_scale,y,40*g_scale,40*g_scale,kBtn);
                    std::string label=ws.title.empty()?id:ws.title;
                    if(label.size()>24){size_t n=0;while(n<label.size()){size_t next=n+utf8_next(label.c_str()+n);if(next>24)break;n=next;}label.resize(n);}
                    draw_string(x,y+42*g_scale,label.c_str(),kText,0.42f);
                }
            }
            g_render_owner.clear();
            if(!g_mouse_down)g_member_drag.clear();
            for(auto it=g_folders.begin();it!=g_folders.end();)if(it->members.size()<2)it=g_folders.erase(it);else ++it;
            if(g_mouse_released){g_merge_target.clear();g_merge_source.clear();}
            if(!g_mouse_down)save_layout();
            g_recording=false;flush_cmds();
        }
        void draw_virtual_keyboard();

        bool point_in_gui(const float x, const float y) {
            if (x == -FLT_MAX || y == -FLT_MAX) return false;
            if(!folder_hit(x,y).empty())return true;
            if (g_edit.active && !ime::supported() && !kbd::supported()) {
                const float sw = g_screen_w > 0 ? static_cast<float>(g_screen_w) : 0.0f;
                const float sh = g_screen_h > 0 ? static_cast<float>(g_screen_h) : 0.0f;
                if (keyboard_contains(x, y, g_scale, sw, sh)) return true;
            }
            for (const auto &kv: g_windows) {
                const WindowState &ws = kv.second;
                if (!ws.visible||grouped_icon(kv.first)) continue;
                const bool compact = ws.collapsed;
                const float hit_w = compact ? (ws.skin_icon.empty()?title_height():ws.icon_size) : ws.w;
                const float body_h = compact ? hit_w : ws.collapsed ? title_height() : ws.h;
                if (x >= ws.x && x <= ws.x + hit_w && y >= ws.y && y <= ws.y + body_h) return true;
            }
            return false;
        }

        // ====================================================================
        // 绘制单个 Mod 的窗口
        // ====================================================================

        void end_scroll();
        void draw_mod_gui(lua_mod_handle_t *h) {
            if (!h || h->gui_ref == LUA_NOREF) return;

            WindowState &ws = g_windows[h->mod_id];
            g_style_window = &ws;
            g_render_owner=h->mod_id;

            // 隐藏时：仍然回调 on_gui（Mod 可在此把自己设回显示），但不绘制、不响应输入
            if (!ws.visible) {
                g_ctx = nullptr;
                g_current_mod = h->mod_id;
                g_recording = true;
                g_cmds.clear();
                lua_engine::call_on_gui(h);
                g_recording = false;
                g_current_mod.clear();
                g_cmds.clear();
                return;
            }

            if (ws.collapsed) {
                const float size = ws.skin_icon.empty()?title_height():ws.icon_size;
                const bool hit=mouse_in(ws.x,ws.y,size,size);
                const bool can_press=g_drag_id.empty() || g_drag_id==h->mod_id;
                if(hit && g_mouse_pressed && can_press)g_drag_id=h->mod_id;
                const auto gesture=ws.icon_gesture.update(g_mouse_x,g_mouse_y,
                    g_mouse_pressed && can_press,g_mouse_down,g_mouse_released,hit,
                    ws.x,ws.y,6.0f*g_scale);
                if(gesture.move){
                    ws.x=std::max(0.0f,std::min(gesture.x,std::max(0.0f,g_screen_w-size)));
                    ws.y=std::max(0.0f,std::min(gesture.y,std::max(0.0f,g_screen_h-size)));
                }
                const bool hovered=mouse_in(ws.x,ws.y,size,size);
                if(hovered || ws.icon_gesture.active)g_hover_gui=true;
                bool custom_icon=false;
                if(!ws.skin_icon.empty()){draw_image(ws.skin_icon,ws.x,ws.y,size,size,kWhite);int iw=0,ih=0;custom_icon=images::status(ws.skin_icon,iw,ih)=="ready";}
                if(!custom_icon)draw_rect(ws.x, ws.y, size, size, hovered ? kBtnHover : kTitle);
                if(!custom_icon)draw_string(ws.x + (size - text_width(ws.compact_label.c_str())) * 0.5f,
                            ws.y + (size - line_height()) * 0.5f,
                            ws.compact_label.empty()?"M":ws.compact_label.c_str(), kWhite, 1.0f);
                if(gesture.move){track_merge(h->mod_id);if(g_mouse_released)g_folder_layout_dirty=true;}
                if(!g_merge_target.empty()&&g_merge_source==h->mod_id)draw_string(ws.x,ws.y-20*g_scale,"+",kWhite,0.7f);
                if (gesture.tap) ws.collapsed = false;
                return;
            }
            const float p = pad();
            const float th = title_height();
            const float body_h = ws.collapsed ? th : ws.h;

            bool custom_panel=false;
            if(!ws.skin_panel.empty()) {
                draw_image(ws.skin_panel,ws.x,ws.y,ws.w,body_h,kWhite,0,0,0,0,ws.skin_border,ws.skin_edge);
                int iw=0,ih=0;custom_panel=images::status(ws.skin_panel,iw,ih)=="ready";
            }
            if(!custom_panel) {
                draw_rect(ws.x - 2.0f, ws.y - 2.0f, ws.w + 4.0f, body_h + 4.0f, kBorder);
                draw_rect(ws.x, ws.y, ws.w, body_h, kPanel);
                draw_rect(ws.x, ws.y, ws.w, th, kTitle);
                // DrawInvBG has fixed corners: a 1px rectangle produces a thick black bar.
            }

            // 收缩按钮（标题栏左端）：展开显示 v，收起显示 >
            const float a = th - 8.0f * g_scale;
            const float ax = ws.x + 4.0f * g_scale;
            const float ay = ws.y + (th - a) * 0.5f;
            const bool arrow_hovered = mouse_in(ax, ay, a, a);
            if (arrow_hovered) g_hover_gui = true;
            if (widget_click("window-collapse",ax,ay,arrow_hovered)) ws.collapsed = !ws.collapsed;
            if(ws.skin_panel.empty())draw_rect(ax, ay, a, a, arrow_hovered ? kBtnHover : kBtn);
            const char *arrow = ws.collapsed ? ">" : "v";
            draw_string(ax + (a - text_width(arrow)) * 0.5f, ay + (a - line_height()) * 0.5f, arrow,
                        kWhite, 1.0f);

            // 标题文本
            draw_string(ax + a + item_gap(), ws.y + (th - line_height()) * 0.5f, (ws.title.empty() ? h->mod_id.c_str() : ws.title.c_str()),
                        custom_panel ? themed_color(kText) : kWhite, 1.0f);

            // 标题栏拖动（避开收缩按钮）
            const bool title_hovered = mouse_in(ws.x, ws.y, ws.w, th) && !arrow_hovered;
            if (title_hovered) g_hover_gui = true;
            if (title_hovered && g_mouse_pressed && g_drag_id.empty()) {
                g_drag_id = h->mod_id;
                g_drag_dx = g_mouse_x - ws.x;
                g_drag_dy = g_mouse_y - ws.y;
            }
            if (!g_drag_id.empty() && g_drag_id == h->mod_id && g_mouse_down) {
                ws.x = g_mouse_x - g_drag_dx;
                ws.y = g_mouse_y - g_drag_dy;
            }

            if(g_mouse_released&&g_drag_id==h->mod_id)g_folder_layout_dirty=true;

            // 收起：只画标题栏，不回调/不绘制内容
            if (ws.collapsed) {
                ws.h = th;
                return;
            }

            // 内容：先记录指令（在 Lua 回调内不能直接调托管绘制），
            // 回调返回后再离开 Lua VM 统一执行。
            Ctx ctx;
            ctx.ox = ws.x + p;
            ctx.oy = ws.y + th + p * 0.5f;
            ctx.cw = ws.w - p * 2.0f;
            g_ctx = &ctx;
            g_current_mod = h->mod_id;
            g_cmds.clear();
            g_recording = true;
            lua_engine::call_on_gui(h);
            while(!g_scroll_stack.empty())end_scroll();
            ws.initial_position=false;
            g_recording = false;
            g_current_mod.clear();
            g_ctx = nullptr;
            g_viewport={};
            if(ws.visible && !ws.collapsed)flush_cmds();else g_cmds.clear();

            const float content_h = ctx.cy + ctx.prev_h;
            ws.h = th + p + content_h + p;
        }

        // ====================================================================
        // 虚拟键盘（文本输入）
        // ====================================================================

        const char *const kRow1 = "1234567890";
        const char *const kRow2 = "qwertyuiop";
        const char *const kRow3 = "asdfghjkl";
        const char *const kRow4 = "zxcvbnm";

        void vkb_append(const char c) {
            g_edit.buf.push_back(c);
        }

        /// 画一个方块按键（带边框 + 居中文字）
        void draw_key_box(const float x, const float y, const float w, const float h, const char *label,
                          const bool hovered) {
            const float g1 = 1.0f * g_scale;
            draw_rect(x, y, w, h, kTrackEdge);
            draw_rect(x + g1, y + g1, w - 2 * g1, h - 2 * g1, hovered ? kBtnHover : kBtn);
            draw_string(x + (w - text_width(label)) * 0.5f, y + (h - line_height()) * 0.5f, label,
                        kWhite, 1.0f);
        }

        void draw_key(const float x, const float y, const float w, const float h, const char *label,
                      const char out, bool &consumed) {
            const bool hovered = mouse_in(x, y, w, h);
            if (hovered) g_hover_gui = true;
            if (widget_click(label,x,y,hovered)) {
                vkb_append(out);
                consumed = true;
            }
            draw_key_box(x, y, w, h, label, hovered);
        }

        /// 拼音输入法候选栏（PC：底部居中，数字键/点击选词，-/= 翻页）
        void draw_ime_bar() {
            if (g_edit.pinyin.empty()) return;
            constexpr size_t kPageSize = 9;
            const float h = 30.0f * g_scale;
            const float gap = 6.0f * g_scale;
            const float pad = 8.0f * g_scale;
            const size_t first = g_edit.page * kPageSize;
            const size_t last = std::min<size_t>(g_edit.cands.size(), first + kPageSize);
            const bool has_prev = g_edit.page > 0;
            const bool has_more = last < g_edit.cands.size();

            // 依次排布：模式 / ◀ / 候选… / 无候选 / ▶
            float total = 0.0f;
            auto chip_w = [&](const std::string &s) { return text_width(s.c_str()) + 16.0f * g_scale; };
            std::vector<std::string> labels;
            labels.push_back(g_edit.ime_on ? "中" : "英");
            if (has_prev) labels.push_back("◀");
            for (size_t i = first; i < last; ++i)
                labels.push_back(std::to_string(i - first + 1) + "." + g_edit.cands[i]);
            if (g_edit.cands.empty()) labels.push_back("无候选");
            if (has_more) labels.push_back("▶");
            for (auto &s: labels) total += chip_w(s) + gap;
            total -= gap;

            float x = (g_screen_w - total) * 0.5f;
            const float y = g_screen_h - h - 12.0f * g_scale;
            draw_rect(x - pad, y - pad, total + 2 * pad, h + 2 * pad, kPanel);

            auto chip = [&](const std::string &label, const char *id) -> bool {
                const float w = chip_w(label);
                const bool hover = mouse_in(x, y, w, h);
                if (hover) g_hover_gui = true;
                const bool clicked = widget_click(id, x, y, hover);
                draw_rect(x, y, w, h, hover ? kBtnHover : kBtn);
                draw_string(x + 8.0f * g_scale, y + (h - line_height()) * 0.5f, label.c_str(), kText, 1.0f);
                x += w + gap;
                return clicked;
            };

            chip(g_edit.ime_on ? "中" : "英", "ime-mode");
            if (has_prev && chip("◀", "ime-prev")) {
                if (g_edit.page > 0) --g_edit.page;
                return;
            }
            for (size_t i = first; i < last; ++i) {
                const std::string label = std::to_string(i - first + 1) + "." + g_edit.cands[i];
                const std::string id = "ime-cand" + std::to_string(i - first);
                if (chip(label, id.c_str())) {
                    g_edit.buf += g_edit.cands[i];
                    g_edit.pinyin.clear();
                    g_edit.cands.clear();
                    g_edit.page = 0;
                    return;
                }
            }
            if (g_edit.cands.empty()) chip("无候选", "ime-none");
            if (has_more && chip("▶", "ime-next")) {
                ++g_edit.page;
                return;
            }
        }

        void draw_virtual_keyboard() {
            // 几何与 keyboard_rect() 共用，保证热区与键盘一致
            const KeyboardRect k = keyboard_rect(g_scale, static_cast<float>(g_screen_w),
                                                 static_cast<float>(g_screen_h));
            const float key = k.key;
            const float wide = k.wide;
            const float gap = k.gap;
            const float pd = k.pad;
            const float kb_w = k.w;
            const float kb_h = k.h;
            const float ox = k.x;
            const float oy = k.y;
            const float inner = kb_w - 2.0f * pd;

            // 各行内容宽度（用于行内居中）
            const float w_qwe = 10.0f * key + 9.0f * gap;
            const float w_asd = 9.0f * key + 8.0f * gap;
            const float w_fun = wide + gap + 7.0f * key + 6.0f * gap + gap + wide;
            const float w_spc = 6.0f * key + gap + 3.0f * key;

            // 面板 + 边框
            draw_rect(ox - 2.0f * g_scale, oy - 2.0f * g_scale, kb_w + 4.0f * g_scale,
                      kb_h + 4.0f * g_scale, kBorder);
            draw_rect(ox, oy, kb_w, kb_h, kPanel);

            const float lx = ox + pd;
            bool consumed = false;
            float y = oy + pd;
            const float step = key + gap;

            // 数字行
            for (int i = 0; i < 10; ++i) {
                char label[2] = {kRow1[i], '\0'};
                draw_key(lx + i * step, y, key, key, label, kRow1[i], consumed);
            }
            y += step;
            // qwerty 行
            for (int i = 0; i < 10; ++i) {
                const char out = g_edit.shift ? static_cast<char>(std::toupper(kRow2[i])) : kRow2[i];
                char label[2] = {out, '\0'};
                draw_key(lx + i * step, y, key, key, label, out, consumed);
            }
            y += step;
            // asdf 行（水平居中）
            for (int i = 0; i < 9; ++i) {
                const char out = g_edit.shift ? static_cast<char>(std::toupper(kRow3[i])) : kRow3[i];
                char label[2] = {out, '\0'};
                draw_key(lx + (w_qwe - w_asd) * 0.5f + i * step, y, key, key, label, out, consumed);
            }
            y += step;
            // 功能行：shift + zxcvbnm + del（整行居中）
            {
                float x = lx + (inner - w_fun) * 0.5f;
                {
                    const bool hv = mouse_in(x, y, wide, key);
                    if (hv) g_hover_gui = true;
                    if (widget_click("kb-shift",x,y,hv)) g_edit.shift = !g_edit.shift;
                    draw_key_box(x, y, wide, key, g_edit.shift ? "ABC" : "abc", hv);
                    x += wide + gap;
                }
                for (int i = 0; i < 7; ++i) {
                    const char out = g_edit.shift ? static_cast<char>(std::toupper(kRow4[i])) : kRow4[i];
                    char label[2] = {out, '\0'};
                    draw_key(x, y, key, key, label, out, consumed);
                    x += step;
                }
                {
                    const bool hv = mouse_in(x, y, wide, key);
                    if (hv) g_hover_gui = true;
                    if (widget_click("kb-delete",x,y,hv) && !g_edit.buf.empty()) g_edit.buf.pop_back();
                    draw_key_box(x, y, wide, key, "del", hv);
                }
            }
            y += step;
            // 空格 / 完成
            {
                float x = lx + (inner - w_spc) * 0.5f;
                const float space_w = key * 6.0f;
                {
                    const bool hv = mouse_in(x, y, space_w, key);
                    if (hv) g_hover_gui = true;
                    if (widget_click("kb-space",x,y,hv)) vkb_append(' ');
                    draw_key_box(x, y, space_w, key, "space", hv);
                    x += space_w + gap;
                }
                {
                    const float done_w = key * 3.0f;
                    const bool hv = mouse_in(x, y, done_w, key);
                    if (hv) g_hover_gui = true;
                    if (widget_click("kb-done",x,y,hv)) g_edit.active = false;
                    draw_key_box(x, y, done_w, key, "done", hv);
                }
            }
            (void) consumed;
        }

