/* Linux/macOS x86_64 不使用 FFI_EFI64/FFI_GNUW64（Windows ABI）。
 * 这个 fork 的 ffi64.c 无条件引用了 efi64 实现，而 win64.S 只在 Windows
 * 构建里加入；这里补上不会被执行到的桩，避免 Linux 链接失败。 */
#include <stdlib.h>
#include <ffi.h>

ffi_status ffi_prep_cif_machdep_efi64(ffi_cif *cif) {
    (void) cif;
    return FFI_BAD_ABI;
}

void ffi_call_efi64(ffi_cif *cif, void (*fn)(void), void *rvalue, void **avalue) {
    (void) cif; (void) fn; (void) rvalue; (void) avalue;
    abort();
}

void ffi_call_go_efi64(ffi_cif *cif, void (*fn)(void), void *rvalue,
                       void **avalue, void *closure) {
    (void) cif; (void) fn; (void) rvalue; (void) avalue; (void) closure;
    abort();
}

ffi_status ffi_prep_closure_loc_efi64(ffi_closure *closure, ffi_cif *cif,
                                      void (*fun)(ffi_cif *, void *, void **, void *),
                                      void *user_data, void *codeloc) {
    (void) closure; (void) cif; (void) fun; (void) user_data; (void) codeloc;
    return FFI_BAD_ABI;
}

ffi_status ffi_prep_go_closure_efi64(ffi_go_closure *closure, ffi_cif *cif,
                                     void (*fun)(ffi_cif *, void *, void **, void *)) {
    (void) closure; (void) cif; (void) fun;
    return FFI_BAD_ABI;
}
