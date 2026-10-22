# 原生 GUI（`mod.gui` / `mod.on_gui`）

定义 `mod.on_gui` 后，加载器为 Mod 创建一个**可拖动窗口**，每帧回调一次。绘制走游戏自带的
`SpriteBatch`（PC: XNA/FNA，PE: Unity 兼容层），无需额外渲染后端。所有控件是**即时模式**：
返回值即当前状态，直接写回。

```lua
local speed, name = 3.0, ""

function mod.on_gui()                              -- 每帧回调一次
    mod.gui.text("我的 Mod")
    mod.gui.separator()
    speed = mod.gui.slider("速度", speed, 1.0, 10.0)  -- 返回新值，直接写回
    name  = mod.gui.input_text("名字", name)          -- 点击弹底部虚拟键盘
    if mod.gui.button("点我") then                    -- 本帧被点击返回 true
        mod.info("clicked")
    end
end
```

## 窗口控制

| 函数 | 说明 |
|---|---|
| `set_pos(x, y)` / `get_pos()` | 窗口位置（屏幕像素） |
| `set_width(w)` | 宽度（高度随内容自适应） |
| `set_visible(v)` / `is_visible()` / `show()` / `hide()` | 显隐 |
| `set_collapsed(v)` / `is_collapsed()` / `toggle_collapsed()` | 收起/展开 |
| `set_title(title, icon?)` | 标题与收起入口文字（`icon` 建议一个字） |
| `set_cursor(x, y)` | 移动布局光标（相对内容区左上角） |
| `set_content_height(h)` | 自定义布局时声明内容高度 |

隐藏时仍会回调 `on_gui`（可在里面 `show()` 重新显示），但控件不绘制、不响应输入。
位置只需首次设置，否则每帧重设会阻止拖动。

## 控件

| 函数 | 返回值 | 说明 |
|---|---|---|
| `text(s, color?)` / `text_wrapped(s, color?)` | – | 文本 / 自动换行 |
| `button(label)` | `bool` | 按钮 |
| `checkbox(label, v)` | `bool` | 复选框 |
| `slider(label, v, min, max)` | `number` | 滑条 |
| `input_text(label, v)` | `string` | 文本框 |
| `same_line()` | – | 下一个控件同行 |
| `separator()` / `spacing()` | – | 分隔线 / 空行 |
| `collapsing_header(label)` | `bool` | 可折叠标题 |
| `progress_bar(v)` | – | 进度条（0~1） |

## 自定义布局与配色

| 函数 | 说明 |
|---|---|
| `set_style(t)` | 覆盖配色，键：`panel/title/button/hover/active/border/text`，值 `{R,G,B,A}` |
| `rect(x,y,w,h,color)` | 原生背景块 |
| `text_at(x,y,text,color,scale=1)` | 自定义位置文本（scale 0~4） |
| `button_at(x,y,w,h,label)` | 自定义位置按钮 |

```lua
g.set_style({ panel = {18,29,40,245}, title = {29,57,69,255}, text = {228,237,232,255} })
g.rect(0, 0, 276, 72, {24,48,59,255})
if g.button_at(0, 90, 276, 42, "查看任务") then end
```

## 图片与皮肤

PNG 放 `Resources/lib/ui/`，路径写 `"lib/ui/x.png"`。

| 函数 | 说明 |
|---|---|
| `load_image(path)` | 注册图片，返回该 Mod 独享 ID |
| `image_status(id)` | `status, w, h`（`pending/ready/failed:原因/invalid`） |
| `release_image(id)` | 下一帧释放 |
| `image(id,x,y,w,h,color?,source?)` | 拉伸绘制；`source={sx,sy,sw,sh}` 像素裁切 |
| `image_fit(id,x,y,w,h,mode?,color?)` | `contain` / `cover` |
| `nine_slice(id,x,y,w,h,border,color?,edge?)` | 九宫格伸缩 |
| `image_button(id,x,y,w,h,label?,border?)` | 图片按钮 |
| `set_skin(panelId,border,iconId,iconSize=48,edge=0)` | 面板皮肤与收起入口图标 |

约束：仅 PNG，≤2048×2048、≤16 MiB；最多 4096 个描述符；纹理缓存预算约 32 MiB，闲置自动淘汰。

## 滚动容器

```lua
local _, sy, _, vh = g.begin_scroll("id", 0, 0, 350, 220, 350, count * row)
-- 只画可见项：sy..sy+vh
g.end_scroll()
```

`begin_scroll(id,x,y,w,h,content_w,content_h)` 返回 `sx,sy,vw,vh`；`scroll_to(id,x,y)` 设位置。
每次 `begin_scroll` 必须配对 `end_scroll()`。

## 其它

- **禁用 GUI**：环境变量 `LUALOADER_GUI_DISABLE=1`。
- **向后兼容**：`mod.gui.*` 函数名、参数、返回语义与旧版一致，旧 Mod 无需修改。
- 收起图标可拖拽合并成文件夹，布局自动保存到加载器私有目录 `gui_layout.json`。
