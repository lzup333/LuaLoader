#if defined(__ANDROID__)
#include "controls.hpp"
#else
#include "keybind.hpp"
#endif
/*******************************************************************************
 * LuaLoader - lua engine
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

#include "lua_engine.hpp"
#include "lua_api.hpp"

#include <atomic>
#include <cctype>
#include <map>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "logger.hpp"
#include "platform.hpp"

namespace lualoader::lua_engine {

static int traceback(lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    if (msg) {
        luaL_traceback(L, L, msg, 1);
    } else {
        lua_pushliteral(L, "<error object>");
    }
    return 1;
}

/// 打开全部 Lua 标准库（base/table/string/math/utf8/coroutine/io/os/debug/package）。
/// 说明：LuaLoader 不再限制 Lua 能力；Mod 是否可信由玩家自行审查 Mod 包决定。
static void open_all_libs(lua_State *L) {
    luaL_openlibs(L);
}

/// 找到 Mod 的原生库目录列表（按平台_架构，其次平台，再次 mod 根）
static std::vector<std::string> native_dirs(const std::string &mod_dir, const std::string &private_dir,
                                            const std::string &platform, const std::string &arch);

#if defined(__ANDROID__)
/// 当前进程（游戏）的应用私有目录：/data/data/<pkg>/files
/// Android 的 linker 命名空间只允许 /data、/mnt/expand，/storage 下的 .so 无法 dlopen，
/// 因此需要把原生库复制到这里再加载。
static std::string android_app_private_dir() {
    std::ifstream cmd("/proc/self/cmdline", std::ios::binary);
    std::string pkg;
    if (cmd)
        std::getline(cmd, pkg, '\0');
    if (pkg.empty())
        return {};
    return "/data/data/" + pkg + "/files";
}
#endif

/// 把候选目录里的原生库复制到 Android 应用私有目录。
///
/// 为什么需要 staging：
///   Android 的 linker namespace 只允许从 /data、/mnt/expand 等目录 dlopen；
///   Mod 私有目录在 /storage/emulated/0/Android/data/... 下，无法直接加载，
///   所以先搬到 /data/data/<包名>/files/lualoader_native/<mod_id>/。
///
/// 为什么“临时目录 → 原子 rename → 清理旧文件”：
///   - 直接覆盖正在被映射的 .so 会 ETXTBSY / 读到被截断的库；
///   - 先完整复制到临时文件名，保证发布出去的每个库都是完整的；
///   - rename 只替换目录项，已经 dlopen 映射的旧 inode 仍然有效（热更新安全）；
///   - 最后删掉本次没有提供的旧 .so，避免下次启动又加载到已移除的模块。
///
/// @param src_dirs 候选源目录，前面的优先；同名文件 first match wins
/// @param dst_dir  应用私有目录下的暂存目录
/// @param mod_id   仅用于日志
/// @return false 表示 staging 失败；调用方不应再回退到旧缓存目录
static bool stage_native_libs(const std::vector<std::string> &src_dirs, const std::string &dst_dir,
                              const std::string &mod_id) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto failed = [&](const char *action) {
        LOG_WARN("[mod {}] native staging {} failed: {}", mod_id, action, ec.message());
        return false;
    };
    // 1) 收集所有候选目录中的 .so；同名文件以先扫描到的目录为准（目录优先级）。
    std::map<std::string, fs::path> sources;
    for (const auto &dir : src_dirs) {
        if (!fs::is_directory(dir, ec)) {
            if (ec && ec != std::errc::no_such_file_or_directory && ec != std::errc::not_a_directory)
                return failed("scan");
            continue;
        }
        fs::directory_iterator it(dir, ec), end;
        if (ec)
            return failed("scan");
        while (it != end) {
            const bool regular = it->is_regular_file(ec);
            if (ec)
                return failed("stat");
            if (regular && it->path().extension() == ".so")
                sources.emplace(it->path().filename().string(), it->path()); // first match wins
            it.increment(ec);
            if (ec)
                return failed("scan");
        }
    }
    // 2) 确保目标目录存在。
    fs::create_directories(dst_dir, ec);
    if (ec)
        return failed("mkdir");
    // 3) 建一个点号前缀的临时目录（不会被当成可加载的 .so 目录），
    //    名字带自增序号避免并发/残留冲突。
    static std::atomic<unsigned long long> sequence{0};
    fs::path temporary;
    bool created = false;
    for (int i = 0; i < 128 && !created; ++i) {
        temporary = fs::path(dst_dir) / (".stage-" + std::to_string(++sequence));
        created = fs::create_directory(temporary, ec);
        if (ec)
            return failed("temporary directory");
    }
    if (!created) {
        LOG_WARN("[mod {}] could not allocate native staging directory", mod_id);
        return false;
    }

    // 无论成功失败，退出时都清掉临时目录（发布后它应为空）。
    struct Cleanup {
        fs::path path;

        ~Cleanup() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } cleanup{temporary};

    // 4) 先把所有库完整复制到临时目录；任意一个失败就整体放弃，不发布半套文件。
    for (const auto &source : sources) {
        fs::copy_file(source.second, temporary / source.first, ec);
        if (ec)
            return failed("copy");
    }
    // 5) 逐个发布：rename 原子替换目录项，已映射的旧库不会被截断。
    for (const auto &source : sources) {
        fs::rename(temporary / source.first, fs::path(dst_dir) / source.first, ec);
        if (ec)
            return failed("publish");
    }
    // 6) 清理本次未提供的旧 .so，避免下次启动加载到已删除/改名的模块。
    fs::directory_iterator it(dst_dir, ec), end;
    if (ec)
        return failed("prune scan");
    while (it != end) {
        const auto path = it->path();
        const bool regular = it->is_regular_file(ec);
        if (ec)
            return failed("prune stat");
        if (regular && path.extension() == ".so" && !sources.count(path.filename().string())) {
            fs::remove(path, ec);
            if (ec)
                return failed("prune");
        }
        it.increment(ec);
        if (ec)
            return failed("prune scan");
    }
    return true;
}

/// 返回最终要写入 package.cpath 的原生库目录。
/// - Android：先 staging 到应用私有目录；成功返回 {dst_dir}，失败返回空。
/// - 其它平台：直接返回候选目录列表（Linux 桌面可直接 dlopen，Windows 用 LoadLibrary）。
static std::vector<std::string> prepare_native_libs(const std::string &mod_dir, const std::string &private_dir,
                                                    const std::string &platform, const std::string &arch,
                                                    const std::string &mod_id) {
    std::vector<std::string> src_dirs = native_dirs(mod_dir, private_dir, platform, arch);
#if defined(__ANDROID__)
    // 拿不到 /data/data/<pkg>/files 就退回原目录——大概率会被 linker namespace 拒绝，
    // 但至少保留原始错误，方便排查。
    const std::string base = android_app_private_dir();
    if (base.empty())
        return src_dirs;
    // mod_id 可能包含路径分隔符等字符，替换成安全的目录名。
    std::string safe_id = mod_id;
    for (char &c : safe_id)
        if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-')
            c = '_';
    const std::string dst_dir = base + "/lualoader_native/" + safe_id;
    if (stage_native_libs(src_dirs, dst_dir, mod_id))
        return {dst_dir};
    // staging 失败时返回空，绝不回退到旧缓存目录（否则会加载到过期/残留的模块）。
    return {};
#else
    (void)mod_id;
    return src_dirs;
#endif
}

/// 配置 package：把 Mod 目录加入 package.path（保留系统默认路径）。
/// 同时把 Mod 的原生库目录加入 package.cpath，并保留 package.loadlib。
///
/// 原生模块是标准 Lua C 模块：由 require 触发 loadlib 加载，loader 不预加载、不注入 API 表。
/// 模块对 lua_* 的依赖在链接时记录：
///   - Linux/Android：DT_NEEDED libloader.<平台>.<架构>.so
///   - Windows：loader 导入库
/// 因此运行时即使 loader 是 RTLD_LOCAL，模块也能在依赖链里解析到符号。
static void configure_package(lua_State *L, const std::string &mod_dir,
                              const std::vector<std::string> &native_search_dirs) {
    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    // 在默认搜索路径前追加 Mod 目录，保证 require 能加载 Mod 自带的 Lua 模块
    lua_getfield(L, -1, "path");
    const char *default_path = lua_tostring(L, -1);
    const std::string mod_pattern = mod_dir + "/?.lua;" + mod_dir + "/?/init.lua;";
    const std::string pattern = mod_pattern + (default_path ? default_path : "");
    lua_pop(L, 1);
    lua_pushstring(L, pattern.c_str());
    lua_setfield(L, -2, "path");

    // 开放原生模块：Mod 平台原生目录优先
    std::string native_pattern;
    for (const auto &dir : native_search_dirs) {
#ifdef _WIN32
        native_pattern += dir + "/?.dll;";
#else
        native_pattern += dir + "/?.so;";
#endif
    }
    lua_getfield(L, -1, "cpath");
    const char *default_cpath = lua_tostring(L, -1);
    lua_pop(L, 1);
    const std::string cpath = native_pattern + (default_cpath ? default_cpath : "");
    lua_pushstring(L, cpath.c_str());
    lua_setfield(L, -2, "cpath");
    // 保留 package.loadlib（open_all_libs 已提供，这里不再置空）
    lua_pop(L, 1);
}

/// 找到 Mod 的原生库目录列表（按优先级从高到低）。
///
/// 每个根目录（private_dir、mod_dir）下依次尝试：
///   native/<平台>_<架构>      —— 内核部署 Resources/ 后的标准位置
///   native/<平台>             —— 不区分架构的兜底
///   lib/native/<平台>_<架构>  —— 入口脚本同级目录（本地开发常见）
///   lib/native/<平台>
///
/// 之所以要这么多候选：Mod 可能被内核部署到 private_dir，也可能在本地开发时
/// 直接从 Resources/lib 跑；两种布局都要能找到原生库。
static std::vector<std::string> native_dirs(const std::string &mod_dir, const std::string &private_dir,
                                            const std::string &platform, const std::string &arch) {
    std::vector<std::string> dirs;
    const std::string pa = platform + "_" + arch;
    for (const std::string &base : {private_dir, mod_dir}) {
        if (base.empty())
            continue;
        dirs.push_back(base + "/native/" + pa);
        dirs.push_back(base + "/native/" + platform);
        dirs.push_back(base + "/lib/native/" + pa);
        dirs.push_back(base + "/lib/native/" + platform);
    }
    return dirs;
}

static bool run_file(lua_State *L, const std::string &path, std::string &err) {
    if (luaL_loadfile(L, path.c_str()) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        err = msg ? msg : "failed to load lua file";
        lua_pop(L, 1);
        return false;
    }

    lua_pushcfunction(L, traceback);
    lua_insert(L, 1);
    if (lua_pcall(L, 0, 0, 1) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        err = msg ? msg : "failed to run lua file";
        lua_pop(L, 2); // error + traceback function
        return false;
    }
    lua_remove(L, 1); // traceback function
    return true;
}

static void protected_call(lua_State *L, const int ref, const char *what) {
    if (ref == LUA_NOREF)
        return;
    const int base = lua_gettop(L);
    lua_pushcfunction(L, traceback);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (lua_pcall(L, 0, 0, base + 1) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        LOG_ERROR("[lua] {} failed: {}", what, msg ? msg : "unknown error");
    }
    lua_settop(L, base);
}

/// 调用栈顶的函数（函数与参数已入栈），带 traceback。调用后函数/参数出栈。
static void pcall_top(lua_State *L, const int nargs, const char *what) {
    const int funcidx = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, funcidx);
    const int status = lua_pcall(L, nargs, 0, funcidx);
    lua_remove(L, funcidx); // 移除 traceback
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        LOG_ERROR("[lua] {} failed: {}", what, msg ? msg : "unknown error");
        lua_pop(L, 1);
    }
}

/// 执行全局表 name（若存在）中的每个条目：
///   - 函数值：直接调用
///   - 字符串：作为全局函数名查找后调用
/// 其余类型与未知的函数名会被跳过（并给出警告）
static void run_todo_list(lua_State *L, const char *name) {
    lua_getglobal(L, name);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    const int len = static_cast<int>(luaL_len(L, -1));
    for (int i = 1; i <= len; ++i) {
        lua_rawgeti(L, -1, i);
        if (lua_isfunction(L, -1)) {
            pcall_top(L, 0, name);
            continue;
        }
        if (lua_type(L, -1) == LUA_TSTRING) {
            const std::string func_name = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_getglobal(L, func_name.c_str());
            if (lua_isfunction(L, -1)) {
                const std::string label = std::string(name) + " -> " + func_name;
                pcall_top(L, 0, label.c_str());
            } else {
                LOG_WARN("[lua] {} refers to unknown function '{}'", name, func_name);
                lua_pop(L, 1);
            }
            continue;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static int ref_global_field(lua_State *L, const char *key) {
    lua_getglobal(L, "mod");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return LUA_NOREF;
    }
    lua_getfield(L, -1, key);
    int ref = LUA_NOREF;
    if (lua_isfunction(L, -1)) {
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // mod
    return ref;
}

static bool read_string_field(lua_State *L, int table_idx, const char *key, std::string &out) {
    if (table_idx < 0)
        table_idx = lua_gettop(L) + table_idx + 1;
    lua_getfield(L, table_idx, key);
    bool ok = false;
    if (lua_isstring(L, -1)) {
        out = lua_tostring(L, -1);
        ok = true;
    }
    lua_pop(L, 1);
    return ok;
}

static bool read_int_field(lua_State *L, int table_idx, const char *key, int &out) {
    if (table_idx < 0)
        table_idx = lua_gettop(L) + table_idx + 1;
    lua_getfield(L, table_idx, key);
    bool ok = false;
    if (lua_isnumber(L, -1)) {
        out = static_cast<int>(lua_tointeger(L, -1));
        ok = true;
    }
    lua_pop(L, 1);
    return ok;
}

/// 读取 Lua 脚本中的 mod.meta 覆盖 mod.json 中的信息
static void read_info(lua_State *L, lua_mod_handle_t *handle) {
    lua_getglobal(L, "mod");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "meta");
    if (lua_istable(L, -1)) {
        std::string text;
        if (read_string_field(L, -1, "pkg_id", text)) {
            handle->info.pkg_id = text;
        } else if (read_string_field(L, -1, "name", text)) {
            handle->info.pkg_id = text;
        }
        if (read_string_field(L, -1, "version", text)) {
            handle->info.version = text;
        }
        read_int_field(L, -1, "version_code", handle->info.version_code);
        read_int_field(L, -1, "api_version", handle->info.api_version);
    }
    lua_pop(L, 1); // meta
    lua_pop(L, 1); // mod
}

/// 解析入口脚本路径。优先使用内核部署 Mod 资源的位置 <private_dir>/lib/，
/// 与 KernelLoader 查找动态库的约定保持一致，并提供若干回退路径便于本地开发。
static std::vector<std::filesystem::path> entry_candidates(const lua_mod_handle_t *handle,
                                                           const std::string &config_dir) {
    const std::vector<std::filesystem::path> dirs = {
        std::filesystem::path(handle->private_dir) / "lib",
        std::filesystem::path(handle->private_dir),
        std::filesystem::path(config_dir),
        std::filesystem::path(config_dir) / "Resources" / "lib",
    };
    std::vector<std::filesystem::path> candidates;
    for (const auto &dir : dirs) {
        candidates.push_back(dir / handle->entry);
    }
    return candidates;
}

/// 为单个 Mod 建立独立的 Lua 状态机并执行入口脚本。
///
/// 顺序很重要：
///   1. 定位入口脚本（同时确定 mod_dir，后面的原生库候选目录依赖它）
///   2. 创建 Lua 状态机
///   3. 打开标准库 → 准备原生库目录 → 配置 package.path / package.cpath
///   4. 注入全局 mod 表（日志/文件/patch/gui...）
///   5. 执行入口脚本；失败时走 close()，先注销回调/钩子再关状态机
///   6. 读取 mod.meta，并缓存 init/cleanup/on_gui 的注册表引用
bool load(lua_mod_handle_t *handle, const std::string &config_path, const std::string &private_dir, std::string &err) {
    handle->private_dir = private_dir;
    handle->platform = LUALOADER_PLATFORM_NAME;
    handle->arch = LUALOADER_ARCH_NAME;
    if (handle->entry.empty())
        handle->entry = "main.lua";

    // 1) 找入口脚本：优先内核部署位置 <private_dir>/lib，再逐级回退。
    const std::string config_dir = std::filesystem::path(config_path).parent_path().string();
    std::string entry_path;
    for (const auto &candidate : entry_candidates(handle, config_dir)) {
        if (std::filesystem::exists(candidate)) {
            entry_path = candidate.string();
            break;
        }
    }
    if (entry_path.empty()) {
        err = "entry script not found: " + (std::filesystem::path(private_dir) / "lib" / handle->entry).string();
        return false;
    }
    handle->mod_dir = std::filesystem::path(entry_path).parent_path().string();

    // 2) 每个 Mod 一份独立的 lua_State，Mod 之间互不影响。
    lua_State *L = luaL_newstate();
    if (!L) {
        err = "failed to create Lua state";
        return false;
    }
    handle->L = L;

    // 3) 标准库 + 原生库搜索目录 + package.path/cpath。
    open_all_libs(L);
    const std::vector<std::string> native_search =
        prepare_native_libs(handle->mod_dir, private_dir, handle->platform, handle->arch, handle->mod_id);
    configure_package(L, handle->mod_dir, native_search);

    // 4) 注入 mod 表；必须在执行入口脚本之前完成。
    lua_api::register_api(L, handle);

    // 5) 执行入口脚本。
    LOG_INFO("Running Lua entry: {}", entry_path);
    if (!run_file(L, entry_path, err)) {
        LOG_ERROR("Failed to run Lua mod {}: {}", handle->mod_id, err);
        // 入口可能已经装了钩子/注册了回调；必须在 Lua 状态机还活着时统一注销。
        close(handle);
        return false;
    }

    // 6) 读取脚本里的 mod.meta，并缓存生命周期回调（注册表引用）。
    read_info(L, handle);
    handle->init_ref = ref_global_field(L, "init");
    handle->cleanup_ref = ref_global_field(L, "cleanup");
    handle->gui_ref = ref_global_field(L, "on_gui");
    return true;
}

void call_init(lua_mod_handle_t *handle) {
    if (!handle || !handle->L)
        return;
    lua_State *L = handle->L;

    // 平台化任务清单，按“通用 -> 平台 -> 平台+架构”的顺序执行
    run_todo_list(L, "todo_list");
    const std::string platform_list = "todo_list_" + handle->platform;
    run_todo_list(L, platform_list.c_str());
    const std::string platform_arch_list = "todo_list_" + handle->platform + "_" + handle->arch;
    run_todo_list(L, platform_arch_list.c_str());

    protected_call(L, handle->init_ref, "mod.init");
}

void call_cleanup(lua_mod_handle_t *handle) {
    if (!handle || !handle->L)
        return;
    protected_call(handle->L, handle->cleanup_ref, "mod.cleanup");
}

void call_on_gui(lua_mod_handle_t *handle) {
    if (!handle || !handle->L)
        return;
    protected_call(handle->L, handle->gui_ref, "mod.on_gui");
}

void uninstall_hooks(lua_mod_handle_t *handle) {
    lua_api::uninstall_all_hooks(handle);
}

/// 关闭一个 Mod 的全部运行时资源。
///
/// 顺序不能反：
///   1. 先卸载钩子；
///   2. 再注销物品/弹幕/Buff/NPC/配方等 C++ 侧回调（此时 Lua 状态机必须还活着，
///      因为注销过程可能要读 Lua 注册表/引用）；
///   3. 最后 lua_close：触发 Lua 的 __gc，并卸载通过 require/loadlib 打开的原生库
///      （Lua 的 CLIBS 表会在状态机关闭时 dlclose/FreeLibrary）。
///
/// 如果先 lua_close 再注销，C++ 侧持有的引用/句柄就会指向已释放的状态机。
void close(lua_mod_handle_t *handle) {
    if (!handle)
        return;
    // 1) 卸载钩子（同时释放全局 hook slot）。
    lua_api::uninstall_all_hooks(handle);
    // 2) 注销各类注册回调，避免 C++ 侧继续调用已销毁的 Lua 函数。
    lua_api::unregister_items(handle);
    lua_api::unregister_buffs(handle);
    lua_api::unregister_projectiles(handle);
    lua_api::unregister_npcs(handle);
    lua_api::unregister_recipes(handle);
    // 3) 关状态机；原生库句柄由 Lua 自己管理并在这里一并释放。
    if (handle->L) {
#if defined(__ANDROID__)
        controls::unregister_owner(handle);
#else
        keybind::unregister_owner(handle);
#endif
        lua_close(handle->L);
        handle->L = nullptr;
    }
    // 4) 清掉注册表引用/网络回调，避免悬垂。
    handle->init_ref = LUA_NOREF;
    handle->cleanup_ref = LUA_NOREF;
    handle->gui_ref = LUA_NOREF;
    handle->net_recv_refs.clear();
}

} // namespace lualoader::lua_engine
