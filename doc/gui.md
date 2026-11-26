# 原生 GUI（`mod.gui` / `mod.on_gui`）

## 多窗口（推荐）

旧版 `mod.on_gui` 每个 Mod 只有一个窗口。需要多个面板时用 `mod.gui.window{...}`：

```lua
local main = mod.gui.window{
    id      = "main",          -- 同一 Mod 内唯一
    title   = "主面板",
    x = 200, y = 120, w = 420,
    visible = false,           -- 初始隐藏，等按键/物品召唤
    layer   = "hud",           -- "hud" = HUD 上方；"hud_below" = HUD 下方
    draw    = function(win)
        mod.gui.text("窗口 " .. win.id)
        if mod.gui.button("关闭") then win:hide() end
    end,
}

local bag = mod.gui.window{
    id = "bag", title = "背包", x = 520, y = 200, visible = false,
    layer = "hud_below",
    draw = function() mod.gui.text("会被原版 HUD 盖住") end,
}
```

窗口句柄方法：

| 方法 | 说明 |
|---|---|
| `win:show()` / `win:hide()` / `win:toggle()` | 显示 / 完全隐藏 / 切换 |
| `win:is_visible()` | 是否显示 |
| `win:collapse()` / `win:expand()` / `win:toggle_collapsed()` | 收起 / 展开 |
| `win:is_collapsed()` | 是否收起 |
| `win:set_title(s)` | 标题 |
| `win:set_pos(x,y)` / `win:get_pos()` | 位置 |
| `win:set_size(w,h?)` / `win:get_size()` | 尺寸（高度默认随内容） |
| `win:set_layer("hud"|"hud_below")` / `win:get_layer()` | 层级 |
| `win:destroy()` | 删除窗口 |

也可以按 id 操作：`mod.gui.show("main")`、`mod.gui.hide("main")`、`mod.gui.toggle("main")`、`mod.gui.is_open("main")`。

用物品或按键召唤：

```lua
on_use = function(player) mod.gui.toggle("main") end
-- 或
mod.controls.register{ id="open_panel", name="打开面板", key="K", on_press=function() mod.gui.toggle("main") end }
```

## 旧版单窗口（`mod.on_gui`）

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
| `set_visible(v)` / `is_visible()` | 当前（旧版）窗口显隐 |
| `show(id?)` / `hide(id?)` / `toggle(id?)` / `is_open(id?)` | 显示 / 完全隐藏 / 切换；无参数=当前窗口 |
| `set_collapsed(v)` / `is_collapsed()` / `toggle_collapsed()` | 收起为小图标 / 展开 |
| `set_title(title, icon?)` | 标题与收起入口文字（`icon` 建议一个字） |
| `set_cursor(x, y)` | 移动布局光标（相对内容区左上角） |
| `set_content_height(h)` | 自定义布局时声明内容高度 |

隐藏时仍会回调 `on_gui`（可在里面 `show()` 重新显示），但控件不绘制、不响应输入。
位置只需首次设置，否则每帧重设会阻止拖动。

> **完全隐藏**：`mod.gui.hide()` 会让窗口、收起图标和文件夹入口一起消失。
> 它在 `on_gui` 之外（`setup`、按键回调、定时器等）调用同样有效；需要恢复时调用
> `mod.gui.show()`。`set_collapsed(true)` 只是收起成小图标，和完全隐藏不同。

```lua
function toggle_ui()
    if mod.gui.is_visible() then
        mod.gui.hide()   -- 完全隐藏（任何上下文都可调用）
    else
        mod.gui.show()   -- 恢复窗口
    end
end
```

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
