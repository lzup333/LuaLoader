/*******************************************************************************
 * LuaLoader - mod_api：原生模块 API 表（自解析 ELF 导出符号）
 * Copyright (C) 2026 lzup333
 *
 * Android/Linux：loader 被内核从 memfd 以 RTLD_LOCAL 加载，dlopen 的模块无法通过
 * ELF 全局作用域解析 lua_* / patchlib_*。这里把自己 ELF 的 .dynsym 解析成
 * 名字→地址 表，通过 ll_set_api 注入给模块（等价于内核给 C mod 传 TPF 符号表）。
 *
 * 安全：解析全程带边界校验；任何异常只记录警告并返回空表，绝不越界读。
 *******************************************************************************/

#include "mod_api.hpp"
#include "lualoader_mod.h"
#include "logger.hpp"

#include <cstring>
#include <string>
#include <unordered_map>

#if !defined(_WIN32)
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#endif

namespace lualoader::mod_api {

    namespace {
        std::unordered_map<std::string, void *> g_symbols;
        bool g_built = false;

        void build_symbols() {
            if (g_built) return;
            g_built = true;
#if !defined(_WIN32)
            Dl_info info{};
            if (!dladdr(reinterpret_cast<void *>(&build_symbols), &info) || !info.dli_fbase) {
                LOG_WARN("[native] dladdr 失败，无法建立符号表");
                return;
            }
            const auto *base = static_cast<const unsigned char *>(info.dli_fbase);

            // 通过 dl_iterate_phdr 找到自身装载范围，用于边界校验
            struct FindCtx {
                const void *base;
                ElfW(Addr) start;
                ElfW(Addr) end;
                bool found;
            } ctx{info.dli_fbase, 0, 0, false};

            dl_iterate_phdr([](struct dl_phdr_info *pi, size_t, void *data) -> int {
                auto *c = static_cast<FindCtx *>(data);
                if (reinterpret_cast<const void *>(pi->dlpi_addr) != c->base) return 0;
                ElfW(Addr) lo = ~static_cast<ElfW(Addr)>(0), hi = 0;
                for (int i = 0; i < pi->dlpi_phnum; ++i) {
                    const auto &ph = pi->dlpi_phdr[i];
                    if (ph.p_type != PT_LOAD) continue;
                    const ElfW(Addr) s = pi->dlpi_addr + ph.p_vaddr;
                    const ElfW(Addr) e = s + ph.p_memsz;
                    if (s < lo) lo = s;
                    if (e > hi) hi = e;
                }
                c->start = lo;
                c->end = hi;
                c->found = true;
                return 1; // 找到即停止
            }, &ctx);

            const auto in_range = [&](const void *p, size_t n) {
                if (!ctx.found || !p || n == 0) return false;
                const auto a = reinterpret_cast<ElfW(Addr)>(p);
                return a >= ctx.start && a + n <= ctx.end && a + n > a; // 溢出保护
            };

            if (!in_range(base, sizeof(ElfW(Ehdr)))) {
                LOG_WARN("[native] 自身 ELF 范围校验失败，跳过符号表");
                return;
            }
            const auto *eh = reinterpret_cast<const ElfW(Ehdr) *>(base);
            if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return;

            const auto *ph = reinterpret_cast<const ElfW(Phdr) *>(base + eh->e_phoff);
            const ElfW(Dyn) *dyn = nullptr;
            for (int i = 0; i < eh->e_phnum; ++i) {
                if (ph[i].p_type == PT_DYNAMIC) {
                    dyn = reinterpret_cast<const ElfW(Dyn) *>(base + ph[i].p_vaddr);
                    break;
                }
            }
            if (!dyn) return;

            const ElfW(Sym) *symtab = nullptr;
            const char *strtab = nullptr;
            size_t syment = sizeof(ElfW(Sym));
            const uint32_t *hashtab = nullptr;
            const uint32_t *gnu_hash = nullptr;

            for (const ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; ++d) {
                switch (d->d_tag) {
                    case DT_SYMTAB: symtab = reinterpret_cast<const ElfW(Sym) *>(d->d_un.d_ptr); break;
                    case DT_STRTAB: strtab = reinterpret_cast<const char *>(d->d_un.d_ptr); break;
                    case DT_SYMENT: syment = d->d_un.d_val; break;
                    case DT_HASH: hashtab = reinterpret_cast<const uint32_t *>(d->d_un.d_ptr); break;
                    case DT_GNU_HASH: gnu_hash = reinterpret_cast<const uint32_t *>(d->d_un.d_ptr); break;
                    default: break;
                }
            }
            if (!symtab || !strtab || syment != sizeof(ElfW(Sym)))
                return;

            size_t count = 0;
            if (hashtab && in_range(hashtab, 2 * sizeof(uint32_t))) {
                count = hashtab[1]; // DT_HASH: nchain == 符号数
            } else if (gnu_hash && in_range(gnu_hash, 4 * sizeof(uint32_t))) {
                // GNU hash 布局：nbuckets, symoffset, bloom_size, bloom_shift, bloom[], buckets[], chain[]
                const uint32_t nbuckets = gnu_hash[0];
                const uint32_t symoffset = gnu_hash[1];
                const uint32_t bloom_size = gnu_hash[2];
                const size_t word = sizeof(ElfW(Addr));
                const size_t bloom_bytes = (size_t) bloom_size * word;
                const uint32_t *buckets = reinterpret_cast<const uint32_t *>(
                        reinterpret_cast<const unsigned char *>(gnu_hash) + 16 + bloom_bytes);
                if (!in_range(buckets, (size_t) nbuckets * sizeof(uint32_t))) return;
                const uint32_t *chain = buckets + nbuckets;
                if (nbuckets == 0) return;
                if (!in_range(chain, sizeof(uint32_t))) return;

                uint32_t max_bucket = 0;
                for (uint32_t i = 0; i < nbuckets; ++i)
                    if (buckets[i] > max_bucket) max_bucket = buckets[i];
                if (max_bucket < symoffset) return; // 无重定位符号

                size_t idx = max_bucket;
                // 向前走到链尾（最低位=1），带边界保护
                while (in_range(chain + (idx - symoffset), sizeof(uint32_t))) {
                    if (chain[idx - symoffset] & 1u) break;
                    ++idx;
                }
                count = idx + 1;
            } else {
                LOG_WARN("[native] 无可用 hash 表，跳过符号表");
                return;
            }

            if (count == 0 || count > 200000) {
                LOG_WARN("[native] 符号数异常({})，跳过符号表", count);
                return;
            }
            if (!in_range(symtab, count * sizeof(ElfW(Sym)))) {
                LOG_WARN("[native] 符号表越界，跳过");
                return;
            }

            for (size_t i = 1; i < count; ++i) {
                const ElfW(Sym) &s = symtab[i];
                if (s.st_shndx == SHN_UNDEF || s.st_name == 0) continue;
                const unsigned char bind = ELF64_ST_BIND(s.st_info);
                const unsigned char type = ELF64_ST_TYPE(s.st_info);
                if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
                if (type != STT_FUNC && type != STT_OBJECT) continue;
                if (s.st_value == 0) continue;
                const char *name = strtab + s.st_name;
                // 名字要在范围内（strtab 至少覆盖到该名字）
                if (reinterpret_cast<const unsigned char *>(name) <
                            reinterpret_cast<const unsigned char *>(strtab) ||
                    !in_range(name, 1))
                    continue;
                if (*name == '\0') continue;
                g_symbols.emplace(name, const_cast<unsigned char *>(base) + s.st_value);
            }
            LOG_INFO("[native] 已建立自身符号表: {} 个符号", g_symbols.size());
#endif
        }

        void *lookup_symbol(const char *name) {
            if (!name) return nullptr;
            build_symbols();
            const auto it = g_symbols.find(name);
            return it == g_symbols.end() ? nullptr : it->second;
        }
    } // namespace

    const ll_api_t *get_api() {
        static const ll_api_t api = {
                /*version=*/1,
                /*size=*/sizeof(ll_api_t),
                /*lookup=*/&lookup_symbol,
        };
        return &api;
    }

    size_t symbol_count() {
        build_symbols();
        return g_symbols.size();
    }

} // namespace lualoader::mod_api
