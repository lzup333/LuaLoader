-- 原生模块示例：require 加载 C 模块并调用
local ok, native = pcall(require, "nativehello")
if not ok then
    mod.warn("原生模块加载失败: " .. tostring(native))
    return
end

function mod.on_gui()
    mod.gui.text("原生模块示例")
    mod.gui.text(native.greet("LuaLoader"))
end
