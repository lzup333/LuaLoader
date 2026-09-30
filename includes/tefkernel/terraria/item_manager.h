/*******************************************************************************
 * File: item_manager (plugin-side API for LuaLoader)
 * Project: tefkernel (MIT)
 * Copyright (c) 2026 eternalfuture-e38299
 *
 * 说明：内核原版 include/terraria/item_manager.h 用 `#if IS_TEFKERNEL_BUILD`
 * 包住了整个头，插件侧拿不到声明。这里提供一份不含该守卫的插件侧版本，
 * 配合 tef_api_imp.c 导出函数指针，供内核按名字填充。
 *******************************************************************************/

#ifndef TEFKERNEL_ITEM_MANAGER_H
#define TEFKERNEL_ITEM_MANAGER_H

#include <stdbool.h>

#include "../tef_api.h"
#include "../patchlib/type.h"
#include "../tefstd/vector.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct terraria_item_handle_t terraria_item_handle_t;

typedef struct terraria_item_ops_t {
    /** 最早初始化，用于注册回调等前置操作（init_static 之前，仅执行一次） */
    void (*early_init)(terraria_item_handle_t *current);

    /** 静态初始化 */
    void (*init_static)(terraria_item_handle_t *current);

    /** 属性设置（instance 由 hook 管理器释放） */
    void (*set_defaults)(terraria_item_handle_t *current, patch_handle_t instance);

    /** 使用条件（instance 由 hook 管理器释放） */
    bool (*can_use)(terraria_item_handle_t *current, patch_handle_t player_instance,
                    patch_handle_t item_instance, bool ignore_cursed);

    /** 获取纹理实例(Asset<Texture2d>)（内核会在加载后自动卸载，勿自行管理） */
    patch_handle_t (*get_texture)(terraria_item_handle_t *current);
} terraria_item_ops_t;

typedef struct terraria_item_handle_t {
    const char *parent_modloader_id; //<< 所属 modloader id
    const char *parent_id;           //<< 所属 mod id
    const char *internal_name;       //<< 内部名称
    bool has_tooltip;                //<< 是否存在物品介绍
    int runtime_id;                  //<< 由内核分配

    terraria_item_ops_t item_ops; //<< 内部逻辑
} terraria_item_handle_t;

DEFINE_FUNCTION(bool, terraria_item_manager_register_item, terraria_item_handle_t *item_handle)

DEFINE_FUNCTION(bool, terraria_item_manager_unregister_item_by_id, int runtime_id)

DEFINE_FUNCTION(terraria_item_handle_t *, terraria_item_manager_get_item, int runtime_id)

DEFINE_FUNCTION(tefstd_vector_t *, terraria_item_manager_get_items)

#ifdef __cplusplus
}
#endif
#endif //TEFKERNEL_ITEM_MANAGER_H
