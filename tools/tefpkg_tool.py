#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tefpkg_tool.py —— Python 版 TEFPkg-Tool（打包 ModLoader / Loader 类型的 .tefpkg）

为什么不直接用它自带的 C++ 版：那份 TEFPkg-Tool 被改过，行为与 TEFKernel 内置的
tefpkg 实现不一定一致。这里用 ctypes 直接调用 **TEFKernel 自身的 libtefkernel** 里的
tefpkg_* API，产物与内核加载时用的编解码器完全同源。

固定条目 ID（对应 TEFKernel kernel_state.h，内核按 ID 而不是文件名取库）：
    Android  arm64=1   arm32=2
    Linux    x64=3     x86=4
    Windows  x64=5     x86=6
条目 0 为文件列表（file list），7..11 为预留给用户文件的空占位。

用法：
    tefpkg_tool.py build  <dir> <output.tefpkg> [fingerprint] [-c compress.json] [-e pat]... [-n]
    tefpkg_tool.py list   <pkg.tefpkg>
    tefpkg_tool.py extract <pkg.tefpkg> <outdir>
    tefpkg_tool.py verify <pkg.tefpkg> [fingerprint]

指纹默认 0x114514（与官方默认、现存 lualoader.tefpkg 一致）。
内核库路径必须由用户显式指定：--kernel-lib 或环境变量 TEFKERNEL_LIB（不自动扫描磁盘）。
"""
import argparse
import ctypes as C
import fnmatch
import json
import os
import struct
import sys

DEFAULT_FINGERPRINT = 0x114514
RESERVED_ENTRIES = 12
LOADER_TYPE = 1  # TEFPKG_TYPE_LOADER（仅用于文件列表/自检，不影响条目 ID）

# (system, arch) -> 固定条目 ID
LOADER_IDS = {
    ("android", "arm64"): 1,
    ("android", "arm"):   2,
    ("linux", "x64"):     3,
    ("linux", "x86"):     4,
    ("windows", "x64"):   5,
    ("windows", "x86"):   6,
}

# 压缩模式 -> (tefpkg_compress_t, level)
COMPRESS_TYPES = {
    "none":  (0x00, 0),
    "lz4":   (0x01, 1),
    "lz4hc": (0x02, 9),
}


def find_kernel(explicit=None):
    """内核库必须由用户显式指定：--kernel-lib 或环境变量 TEFKERNEL_LIB（不扫盘）。"""
    cand = explicit or os.environ.get("TEFKERNEL_LIB")
    if not cand:
        sys.exit("错误：请用 --kernel-lib <libtefkernel*.so> 或环境变量 TEFKERNEL_LIB "
                 "指定 TEFKernel 库（不再自动扫描磁盘）")
    if not os.path.exists(cand):
        sys.exit(f"错误：指定的 TEFKernel 库不存在：{cand}")
    try:
        C.CDLL(cand)
    except OSError as e:
        sys.exit(f"错误：无法加载 {cand}：{e}")
    return cand


class TefPkg:
    def __init__(self, lib_path):
        self.k = C.CDLL(lib_path)
        k = self.k
        k.tefpkg_create_reserved_from_memory.argtypes = [C.c_uint16, C.POINTER(C.c_void_p)]
        k.tefpkg_create_reserved_from_memory.restype = C.c_int
        k.tefpkg_add_entry_from_memory.argtypes = [C.c_void_p, C.c_uint8, C.c_uint8,
                                                   C.POINTER(C.c_uint8), C.c_uint32]
        k.tefpkg_add_entry_from_memory.restype = C.c_int
        k.tefpkg_add_entry_from_file.argtypes = [C.c_void_p, C.c_char_p, C.c_uint8, C.c_uint8]
        k.tefpkg_add_entry_from_file.restype = C.c_int
        k.tefpkg_sign_package.argtypes = [C.c_void_p, C.c_uint64]
        k.tefpkg_sign_package.restype = C.c_int
        k.tefpkg_save_memory_file.argtypes = [C.c_char_p, C.c_void_p, C.c_uint64]
        k.tefpkg_save_memory_file.restype = C.c_int
        k.tefpkg_open_readonly.argtypes = [C.c_char_p, C.POINTER(C.c_void_p)]
        k.tefpkg_open_readonly.restype = C.c_int
        k.tefpkg_verify_pkg.argtypes = [C.c_void_p]
        k.tefpkg_verify_pkg.restype = C.c_int
        k.tefpkg_verify_signature.argtypes = [C.c_void_p, C.c_uint64]
        k.tefpkg_verify_signature.restype = C.c_int
        k.tefpkg_close.argtypes = [C.c_void_p]
        k.tefpkg_get_entries_count.argtypes = [C.c_void_p]
        k.tefpkg_get_entries_count.restype = C.c_uint16
        k.tefpkg_get_reserved_entries.argtypes = [C.c_void_p]
        k.tefpkg_get_reserved_entries.restype = C.c_uint16
        k.tefpkg_get_entry_info.argtypes = [C.c_void_p, C.c_uint32, C.POINTER(C.c_void_p)]
        k.tefpkg_get_entry_info.restype = C.c_int
        k.tefpkg_extract_entry_to_memory.argtypes = [C.c_void_p, C.c_uint32,
                                                     C.POINTER(C.POINTER(C.c_uint8)),
                                                     C.POINTER(C.c_uint32)]
        k.tefpkg_extract_entry_to_memory.restype = C.c_int
        k.tefpkg_extract_entry_to_file.argtypes = [C.c_void_p, C.c_uint32, C.c_char_p]
        k.tefpkg_extract_entry_to_file.restype = C.c_int

    # ---- 只读操作 ----
    def open(self, path):
        pkg = C.c_void_p()
        rc = self.k.tefpkg_open_readonly(path.encode(), C.byref(pkg))
        if rc != 0:
            sys.exit(f"打开失败 {path}: rc={rc}")
        return pkg

    def entries(self, pkg):
        n = self.k.tefpkg_get_entries_count(pkg)
        out = []
        for i in range(n):
            e = C.c_void_p()
            if self.k.tefpkg_get_entry_info(pkg, i, C.byref(e)) != 0:
                continue
            raw = C.string_at(e, 34)
            idx, doff, csz, osz, chk, ts, ct, cl = struct.unpack("<IIIIQQBB", raw)
            out.append(dict(index=idx, offset=doff, csize=csz, osize=osz,
                            ctype=ct, clevel=cl))
        return out

    def header(self, pkg):
        # tefpkg_t 的第一个字段是 header（packed，52 字节）
        raw = C.string_at(pkg, 52)
        vals = struct.unpack("<IHHHHIIQQQQ", raw)
        keys = ["magic", "version", "file_count", "reserved_entries", "_reserved",
                "data_offset", "data_size", "timestamp", "checksum",
                "content_hash", "signature"]
        return dict(zip(keys, vals))


def parse_fp(s):
    return DEFAULT_FINGERPRINT if s is None else int(s, 0)


def loader_kind(name):
    """libloader.<system>.<arch>.<ext> -> (system, arch) 或 None"""
    if not name.startswith("libloader."):
        return None
    parts = name.split(".")
    if len(parts) < 4:
        return None
    return (parts[1], parts[2])


def load_compress_cfg(path):
    cfg = {}
    if not path:
        return cfg
    with open(path) as f:
        data = json.load(f)
    for name, spec in data.items():
        mode = spec.get("mode", "lz4hc") if isinstance(spec, dict) else spec
        level = spec.get("level") if isinstance(spec, dict) else None
        ctype, deflevel = COMPRESS_TYPES.get(mode, COMPRESS_TYPES["lz4hc"])
        cfg[name] = (ctype, level if level is not None else deflevel)
    return cfg


def cmd_build(a):
    lib = find_kernel(a.kernel_lib)
    tp = TefPkg(lib)
    fp = parse_fp(a.fingerprint or a.fingerprint_pos)
    ccfg = load_compress_cfg(a.compress)

    sid = os.path.join(a.dir, "")
    files = sorted(f for f in os.listdir(a.dir)
                   if os.path.isfile(os.path.join(a.dir, f)))
    files = [f for f in files if not any(fnmatch.fnmatch(f, p) for p in a.exclude)]

    loader = {}   # id -> filename
    extra = []    # (filename, id)
    for f in files:
        kind = loader_kind(f)
        eid = LOADER_IDS.get(kind) if kind else None
        if eid is not None:
            loader[eid] = f
        else:
            extra.append(f)

    if not loader:
        sys.exit(f"错误：{a.dir} 里没有 libloader.<system>.<arch>.{{so,dll}}")

    # 文件列表（条目 0）：列非 loader 文件，ID 从 12 起
    flist = struct.pack("<I", 0 if a.no_file_list else len(extra))
    if not a.no_file_list:
        fid = RESERVED_ENTRIES
        for f in extra:
            nb = f.encode()
            flist += struct.pack("<H", len(nb)) + nb + struct.pack("<I", fid)
            fid += 1

    pkg = C.c_void_p()
    rc = tp.k.tefpkg_create_reserved_from_memory(RESERVED_ENTRIES, C.byref(pkg))
    if rc != 0:
        sys.exit(f"create_reserved 失败 rc={rc}")
    arr = (C.c_uint8 * len(flist)).from_buffer_copy(flist)
    rc = tp.k.tefpkg_add_entry_from_memory(pkg, 0, 0, arr, len(flist))
    if rc != 0:
        sys.exit(f"写入文件列表失败 rc={rc}")

    ph = (C.c_uint8 * 1)(0xFF)
    for eid in range(1, RESERVED_ENTRIES):
        if eid in loader:
            fname = loader[eid]
            path = os.path.join(a.dir, fname)
            ctype, clevel = ccfg.get(fname, COMPRESS_TYPES["lz4hc"])
            rc = tp.k.tefpkg_add_entry_from_file(pkg, path.encode(), ctype, clevel)
            if rc != 0:
                sys.exit(f"加入条目 {eid} ({fname}) 失败 rc={rc}")
            print(f"  [{eid}] {fname}  {os.path.getsize(path)} B  lz4hc/{clevel}")
        else:
            rc = tp.k.tefpkg_add_entry_from_memory(pkg, 0, 0, ph, 1)
            if rc != 0:
                sys.exit(f"占位条目 {eid} 失败 rc={rc}")

    # 签名 -> 保存 -> 复验
    rc = tp.k.tefpkg_sign_package(pkg, fp)
    if rc != 0:
        sys.exit(f"签名失败 rc={rc}")
    rc = tp.k.tefpkg_save_memory_file(a.output.encode(), pkg, fp)
    if rc != 0:
        sys.exit(f"保存失败 rc={rc}")
    tp.k.tefpkg_close(pkg)

    q = tp.open(a.output)
    v = tp.k.tefpkg_verify_pkg(q)
    s = tp.k.tefpkg_verify_signature(q, fp)
    tp.k.tefpkg_close(q)
    if v != 0 or s != 0:
        sys.exit(f"自检失败：verify_pkg={v} verify_signature={s}")
    print(f"OK -> {a.output}  ({os.path.getsize(a.output)} B, 指纹 {fp:#x}, 完整性+签名通过)")


def cmd_list(a):
    tp = TefPkg(find_kernel(a.kernel_lib))
    pkg = tp.open(a.pkg)
    h = tp.header(pkg)
    print(f"包：{a.pkg}")
    print(f"  magic={h['magic']:#x} version={h['version']:#x} "
          f"files={h['file_count']} reserved={h['reserved_entries']} "
          f"签名={h['signature']:#x}")
    for e in tp.entries(pkg):
        print(f"  [{e['index']:>2}] csize={e['csize']:>9} osize={e['osize']:>9} "
              f"ctype={e['ctype']} level={e['clevel']}")
    tp.k.tefpkg_close(pkg)


def cmd_verify(a):
    tp = TefPkg(find_kernel(a.kernel_lib))
    pkg = tp.open(a.pkg)
    v = tp.k.tefpkg_verify_pkg(pkg)
    print(f"verify_pkg        = {v}  ({'OK' if v == 0 else 'FAIL'})")
    if a.fingerprint is not None:
        fp = parse_fp(a.fingerprint)
        s = tp.k.tefpkg_verify_signature(pkg, fp)
        print(f"verify_signature({fp:#x}) = {s}  ({'OK' if s == 0 else 'FAIL'})")
    tp.k.tefpkg_close(pkg)


def cmd_extract(a):
    tp = TefPkg(find_kernel(a.kernel_lib))
    pkg = tp.open(a.pkg)
    os.makedirs(a.outdir, exist_ok=True)
    for e in tp.entries(pkg):
        i = e["index"]
        # 有些占位条目是 1 字节 0xff，跳过
        if e["osize"] <= 1 and e["ctype"] == 0:
            continue
        out = os.path.join(a.outdir, f"entry_{i}.bin")
        rc = tp.k.tefpkg_extract_entry_to_file(pkg, i, out.encode())
        print(f"  [{i}] -> {out}  rc={rc}  ({e['osize']} B)")
    tp.k.tefpkg_close(pkg)


def main():
    ap = argparse.ArgumentParser(prog="tefpkg_tool.py", description="Python 版 TEFPkg-Tool（调 TEFKernel 库打包）")
    ap.add_argument("--kernel-lib", help="libtefkernel*.so 路径；也可用环境变量 TEFKERNEL_LIB（不自动扫描）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build", help="打包目录为 .tefpkg")
    b.add_argument("dir")
    b.add_argument("output")
    b.add_argument("fingerprint_pos", nargs="?", default=None)
    b.add_argument("-f", "--fingerprint", default=None)
    b.add_argument("-c", "--compress", default=None, help="压缩配置 JSON")
    b.add_argument("-e", "--exclude", action="append", default=[], help="排除匹配（文件列表用，可多次）")
    b.add_argument("-n", "--no-file-list", action="store_true")
    b.set_defaults(func=cmd_build)

    l = sub.add_parser("list", help="列出包内条目")
    l.add_argument("pkg")
    l.set_defaults(func=cmd_list)

    x = sub.add_parser("extract", help="解包到目录")
    x.add_argument("pkg")
    x.add_argument("outdir")
    x.set_defaults(func=cmd_extract)

    v = sub.add_parser("verify", help="校验完整性/签名")
    v.add_argument("pkg")
    v.add_argument("fingerprint", nargs="?", default=None)
    v.set_defaults(func=cmd_verify)

    args = ap.parse_args()
    # 子命令里也要能读到 --kernel-lib（放在主解析器上）
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
