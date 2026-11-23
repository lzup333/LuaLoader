#include "api_internal.hpp"

#include <atomic>
#include <unordered_map>
#include <new>

#include <ffi.h>

namespace lualoader::lua_api {

// ========================================================================
// Patchlib: 钩子（Prefix / Postfix）
// ========================================================================

/// 一个钩子槽位。动态分配，id 单调递增，因此没有固定上限。
/// generation 用来防止“旧 userdata 在槽位释放后误删新钩子”。
struct HookSlot {
    int id{0};
    uint64_t generation{0};
    lua_State *L{nullptr};
    lua_mod_handle_t *handle{nullptr}; ///< 所属 Mod，卸载时据此清理
    int prefix_ref{LUA_NOREF};         ///< prefix 函数在注册表里的引用
    int postfix_ref{LUA_NOREF};        ///< postfix 函数在注册表里的引用
    bool copy_handles{false};          ///< true 时把 instance/对象参数/返回值复制为 GC 托管句柄
    bool override_result{false};       ///< true 时允许 postfix 返回值覆盖原方法返回值（默认关闭，保证兼容）
    patch_hook_id_t hook_id{PATCH_HOOK_INVALID_ID}; ///< 内核返回的钩子 id

    /* 每个钩子用自己的 libffi closure 当跳板：closure 的 user_data 就是
     * HookSlot*，所以一个分发函数可以服务任意数量的钩子，没有 1024 上限。 */
    ffi_closure *prefix_closure{nullptr};
    ffi_closure *postfix_closure{nullptr};
};

/// id -> 槽位。槽位指针直接作为 userdata 交给内核回调，分发路径无需查表。
static std::unordered_map<int, HookSlot *> g_hook_slots;
static int g_next_slot_id = 1;
static uint64_t g_hook_generation = 0;

/// 诊断：当前正在派发的 hook 槽位/阶段，供 Lua panic 时定位
std::atomic<int> g_active_hook_slot{-1};
std::atomic<int> g_active_hook_phase{0}; // 1=prefix 2=postfix

struct HookUserdata {
    int slot;
    uint64_t generation;
};

/// 钩子回调：内核把安装时传入的 user（即 HookSlot*）原样回传。
static bool hook_prefix_dispatch(void *user, patch_handle_t instance, void **args,
                                 const patch_method_signature_t *sig, void *result);

static void hook_postfix_dispatch(void *user, patch_handle_t instance, void **args, void *result,
                                  const patch_method_signature_t *sig);

/// 压入一个参数值；copy_handles 时把托管对象句柄复制为 GC 托管句柄（原生指针只借用，不托管）
static void push_hook_arg_value(lua_State *L, const patch_type_t type, const void *value, const bool copy_handles) {
    if (copy_handles && type == PATCH_OBJECT && value) {
        auto *ptr = *static_cast<void *const *>(value);
        if (ptr) {
            push_owned_handle(L, copy_handle(ptr));
            return;
        }
    }
    push_patch_value(L, type, value);
}

/// 钩子回调参数：instance、参数表 args（数组）、返回值 result
static void push_hook_args(lua_State *L, patch_handle_t instance, void **args, const patch_method_signature_t *sig,
                           const bool copy_handles) {
    if (!instance) {
        lua_pushnil(L);
    } else if (copy_handles) {
        push_owned_handle(L, copy_handle(instance));
    } else {
        lua_pushlightuserdata(L, instance);
    }
    const int argc = sig ? static_cast<int>(tefstd_vector_size(&sig->arg_types)) : 0;
    if (argc < 0 || argc > 32) {
        LOG_ERROR("[hook] invalid argc={} (slot={}, sig={})", argc, g_active_hook_slot.load(),
                  static_cast<const void *>(sig));
        lua_createtable(L, 0, 0);
        return;
    }
    lua_createtable(L, argc, 0);
    for (int i = 0; i < argc; ++i) {
        const auto type = *static_cast<patch_type_t *>(tefstd_vector_at(&sig->arg_types, i));
        push_hook_arg_value(L, type, args[i], copy_handles);
        lua_rawseti(L, -2, i + 1);
    }
}

/// prefix 分发：原方法执行前调用 Lua 回调。
///
/// Lua 回调签名：function(instance, args, result) -> skip?, new_result?
///   - 返回 true 表示跳过原方法；此时若再返回一个值，会作为方法返回值写回 result。
///   - 返回 false/nil 表示继续执行原方法。
///
/// 返回 true 时调用方跳过原方法。任何 Lua 错误/非法返回值都退化为“不跳过”，
/// 避免因为脚本错误把游戏方法整个吞掉。
static bool hook_prefix_dispatch(void *user, patch_handle_t instance, void **args,
                                 const patch_method_signature_t *sig, void *result) {
    HookSlot *s = static_cast<HookSlot *>(user);
    if (!s || !s->L || s->prefix_ref == LUA_NOREF)
        return false;

    lua_State *L = s->L;
    // 诊断信息：panic 时能知道是哪个槽位、哪个阶段出错。
    g_active_hook_slot.store(s->id, std::memory_order_relaxed);
    g_active_hook_phase.store(1, std::memory_order_relaxed);
    const int base = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->prefix_ref);
    push_hook_args(L, instance, args, sig, s->copy_handles);
    // 第三个参数：当前返回值（void 方法传 nil）。
    if (result && sig && sig->return_type != PATCH_VOID) {
        push_hook_arg_value(L, sig->return_type, result, s->copy_handles);
    } else {
        lua_pushnil(L);
    }

    // 参数：instance, args, result -> 3 个；期望 2 个返回值 (skip, new_result)。
    if (pcall_log(L, 3, 2, "prefix hook") != LUA_OK) {
        lua_settop(L, base);
        g_active_hook_slot.store(-1, std::memory_order_relaxed);
        g_active_hook_phase.store(0, std::memory_order_relaxed);
        return false;
    }

    const bool skip = lua_toboolean(L, -2) != 0;
    if (skip && result && sig && sig->return_type != PATCH_VOID && !lua_isnil(L, -1)) {
        if (!try_read_patch_value(L, -1, sig->return_type, result)) {
            LOG_ERROR("[lua] prefix hook returned an invalid result; running original method");
            lua_settop(L, base);
            return false;
        }
    }
    lua_settop(L, base);
    g_active_hook_slot.store(-1, std::memory_order_relaxed);
    g_active_hook_phase.store(0, std::memory_order_relaxed);
    return skip;
}

/// postfix 分发：原方法执行后调用 Lua 回调。
///
/// Lua 回调签名：function(instance, args, result)
/// 默认忽略返回值；只有 { override_result = true } 时才读取第三个返回值覆盖 result。
static void hook_postfix_dispatch(void *user, patch_handle_t instance, void **args, void *result,
                                  const patch_method_signature_t *sig) {
    HookSlot *s = static_cast<HookSlot *>(user);
    if (!s || !s->L || s->postfix_ref == LUA_NOREF)
        return;

    lua_State *L = s->L;
    g_active_hook_slot.store(s->id, std::memory_order_relaxed);
    g_active_hook_phase.store(2, std::memory_order_relaxed);
    const int base = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->postfix_ref);
    push_hook_args(L, instance, args, sig, s->copy_handles);
    if (result && sig && sig->return_type != PATCH_VOID) {
        push_hook_arg_value(L, sig->return_type, result, s->copy_handles);
    } else {
        lua_pushnil(L);
    }

    // 默认忽略 postfix 返回值（兼容旧 Mod）；仅在 opt-in 时用返回值覆盖原方法返回值
    if (s->override_result) {
        if (pcall_log(L, 3, 1, "postfix hook") == LUA_OK) {
            if (result && sig && sig->return_type != PATCH_VOID && !lua_isnil(L, -1)) {
                if (!try_read_patch_value(L, -1, sig->return_type, result)) {
                    LOG_ERROR("[lua] postfix hook returned an invalid result; keeping original result");
                }
            }
        }
    } else {
        pcall_log(L, 3, 0, "postfix hook");
    }
    lua_settop(L, base);
    g_active_hook_slot.store(-1, std::memory_order_relaxed);
    g_active_hook_phase.store(0, std::memory_order_relaxed);
}

// ------------------------------------------------------------------------
// libffi 跳板：内核的普通 prepost hook 没有 user 参数，这里给每个钩子
// 生成一个 closure，把 HookSlot* 作为 closure 的 user_data 带进去。
// ------------------------------------------------------------------------
namespace {
    ffi_cif g_prefix_cif;
    ffi_cif g_postfix_cif;
    ffi_type *g_closure_arg_types[4] = {
        &ffi_type_pointer, &ffi_type_pointer, &ffi_type_pointer, &ffi_type_pointer
    };
    bool g_closure_cifs_ready = false;

    void ensure_closure_cifs() {
        if (g_closure_cifs_ready) return;
        g_closure_cifs_ready = true;
        if (ffi_prep_cif(&g_prefix_cif, FFI_DEFAULT_ABI, 4, &ffi_type_uint8,
                         g_closure_arg_types) != FFI_OK)
            LOG_ERROR("[hook] ffi_prep_cif(prefix) failed");
        if (ffi_prep_cif(&g_postfix_cif, FFI_DEFAULT_ABI, 4, &ffi_type_void,
                         g_closure_arg_types) != FFI_OK)
            LOG_ERROR("[hook] ffi_prep_cif(postfix) failed");
    }

    void prefix_closure_handler(ffi_cif *, void *ret, void **args, void *user) {
        const auto instance = *static_cast<patch_handle_t *>(args[0]);
        auto method_args = *static_cast<void ***>(args[1]);
        const auto sig = *static_cast<const patch_method_signature_t **>(args[2]);
        void *result = *static_cast<void **>(args[3]);
        const bool skip = hook_prefix_dispatch(user, instance, method_args, sig, result);
        *static_cast<uint8_t *>(ret) = skip ? 1 : 0;
    }

    void postfix_closure_handler(ffi_cif *, void *, void **args, void *user) {
        const auto instance = *static_cast<patch_handle_t *>(args[0]);
        auto method_args = *static_cast<void ***>(args[1]);
        void *result = *static_cast<void **>(args[2]);
        const auto sig = *static_cast<const patch_method_signature_t **>(args[3]);
        hook_postfix_dispatch(user, instance, method_args, result, sig);
    }

    bool make_closure(ffi_cif *cif, void (*fn)(ffi_cif *, void *, void **, void *),
                      void *user, ffi_closure **out_closure, void **out_code) {
        void *code = nullptr;
        ffi_closure *closure = static_cast<ffi_closure *>(
                ffi_closure_alloc(sizeof(ffi_closure), &code));
        if (!closure) return false;
        if (ffi_prep_closure_loc(closure, cif, fn, user, code) != FFI_OK) {
            ffi_closure_free(closure);
            return false;
        }
        *out_closure = closure;
        *out_code = code;
        return true;
    }
} // namespace

/// 释放槽位：卸载内核钩子、解除 Lua 引用、从 Mod 的 hook_slots 中移除，然后释放槽位。
/// 可重复调用（不存在的 id 直接返回）。
void release_slot(const int id) {
    auto it = g_hook_slots.find(id);
    if (it == g_hook_slots.end())
        return;

    HookSlot *s = it->second;
    if (s->hook_id != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(s->hook_id);
    }

    /* 不要 free ffi_closure，也不要 delete HookSlot。
     *
     * 旧实现用的是静态模板跳板，永远不会释放，所以“卸载时内核还残留一个
     * hook 节点”也不会崩。换成 libffi 后如果立刻 ffi_closure_free，而
     * 内核因为 uninstall 失败/重入仍然调用这个入口，就会直接跳进已释放
     * 的 mmap 区域（PC == fault address，且不在任何模块里）。
     *
     * 这里把 closure 和槽位都保留到进程退出；Lua 引用解除后把 ref 置为
     * NOREF、L 置空，残留回调最多安全地空转，不会 use-after-free。 */
    if (s->L) {
        if (s->prefix_ref != LUA_NOREF)
            luaL_unref(s->L, LUA_REGISTRYINDEX, s->prefix_ref);
        if (s->postfix_ref != LUA_NOREF)
            luaL_unref(s->L, LUA_REGISTRYINDEX, s->postfix_ref);
    }
    s->prefix_ref = LUA_NOREF;
    s->postfix_ref = LUA_NOREF;
    s->L = nullptr;
    if (s->handle) {
        auto &slots = s->handle->hook_slots;
        for (auto slot_it = slots.begin(); slot_it != slots.end(); ++slot_it) {
            if (*slot_it == id) {
                slots.erase(slot_it);
                break;
            }
        }
    }

    g_hook_slots.erase(it);
    /* 故意不 delete s：残留 hook 的 closure user_data 仍指向它。 */
}

/// 分配一个槽位；id 单调递增，实际只受 int 范围限制。
/// 每次分配都递增 generation，供 l_hook_remove 判断 userdata 是否过期。
static HookSlot *alloc_slot() {
    if (g_next_slot_id <= 0)
        g_next_slot_id = 1;

    auto *s = new (std::nothrow) HookSlot();
    if (!s)
        return nullptr;

    s->id = g_next_slot_id++;
    s->generation = ++g_hook_generation;
    g_hook_slots.emplace(s->id, s);
    return s;
}

/// 读取 table[key]；若是函数就转成注册表引用，否则返回 LUA_NOREF。
/// 调用后栈平衡（只把引用/NO_REF 返回给调用方）。
static int ref_function_field(lua_State *L, int table_idx, const char *key) {
    if (table_idx < 0)
        table_idx = lua_gettop(L) + table_idx + 1;
    lua_getfield(L, table_idx, key);
    int ref = LUA_NOREF;
    if (lua_isfunction(L, -1)) {
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, 1);
    }
    return ref;
}

/// mod.patch.install_hook(method, { prefix=fn, postfix=fn, copy=bool, override_result=bool })
///
/// 至少提供 prefix/postfix 之一。成功返回带 :remove() 的 userdata；
/// 失败会回滚槽位和已创建的 Lua 引用，不留下半装状态。
static int l_patch_install_hook(lua_State *L) {
    const patch_handle_t method = to_handle(L, 1);
    if (!method)
        return luaL_error(L, "invalid method handle");
    luaL_checktype(L, 2, LUA_TTABLE);

    HookSlot *s = alloc_slot();
    if (!s)
        return luaL_error(L, "failed to allocate hook slot");

    const int slot_id = s->id;
    s->L = L;
    s->handle = get_handle(L);
    s->prefix_ref = ref_function_field(L, 2, "prefix");
    s->postfix_ref = ref_function_field(L, 2, "postfix");
    lua_getfield(L, 2, "copy");
    s->copy_handles = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    // 显式 opt-in 才允许 postfix 返回值覆盖原方法返回值（默认关闭，保证旧 Mod 兼容）
    lua_getfield(L, 2, "override_result");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, 2, "result");
    }
    s->override_result = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    if (s->prefix_ref == LUA_NOREF && s->postfix_ref == LUA_NOREF) {
        release_slot(slot_id);
        return luaL_error(L, "hook must define a 'prefix' and/or 'postfix' function");
    }

    // 每个钩子一个 libffi closure，把 HookSlot* 带进普通 prepost hook。
    ensure_closure_cifs();
    void *prefix_code = nullptr;
    void *postfix_code = nullptr;
    if (s->prefix_ref != LUA_NOREF) {
        if (!make_closure(&g_prefix_cif, prefix_closure_handler, s,
                          &s->prefix_closure, &prefix_code)) {
            release_slot(slot_id);
            return luaL_error(L, "failed to create prefix hook trampoline");
        }
    }
    if (s->postfix_ref != LUA_NOREF) {
        if (!make_closure(&g_postfix_cif, postfix_closure_handler, s,
                          &s->postfix_closure, &postfix_code)) {
            release_slot(slot_id);
            return luaL_error(L, "failed to create postfix hook trampoline");
        }
    }

    s->hook_id = patchlib_install_prepost_hook(
        method,
        s->prefix_closure ? reinterpret_cast<prefix_callback_t>(prefix_code) : nullptr,
        s->postfix_closure ? reinterpret_cast<postfix_callback_t>(postfix_code) : nullptr);

    if (s->hook_id == PATCH_HOOK_INVALID_ID) {
        release_slot(slot_id);
        return luaL_error(L, "failed to install hook");
    }

    if (s->handle)
        s->handle->hook_slots.push_back(slot_id);

    auto *ud = static_cast<HookUserdata *>(lua_newuserdatauv(L, sizeof(HookUserdata), 0));
    ud->slot = slot_id;
    ud->generation = s->generation;
    luaL_getmetatable(L, HOOK_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

/// hook:remove()
/// 用 generation 校验：如果槽位已被释放，旧 userdata 不能误删新钩子。
int l_hook_remove(lua_State *L) {
    auto *ud = static_cast<HookUserdata *>(luaL_checkudata(L, 1, HOOK_METATABLE));
    if (ud->slot >= 0) {
        auto it = g_hook_slots.find(ud->slot);
        if (it != g_hook_slots.end() && it->second->generation == ud->generation) {
            release_slot(ud->slot);
        }
        ud->slot = -1;
    }
    return 0;
}

/// 把 mod.patch 下的钩子相关函数注册到当前 table。
void add_patch_hook(lua_State *L) {
    static const luaL_Reg fns[] = {
        {"install_hook", l_patch_install_hook},
        {nullptr, nullptr},
    };
    luaL_setfuncs(L, fns, 0);
}

} // namespace lualoader::lua_api
