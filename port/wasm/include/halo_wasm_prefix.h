/*
HALO_WASM_PREFIX.H

Force-included ahead of halo_linux_prefix.h in every unit of the
WebAssembly guest (tools/macos_build.py, port/wasm/README.md). WebAssembly
is ILP32 like the Xbox, and aligns doubles and 64-bit integers to 8 bytes as
MSVC does, so the game's structures keep their layout; what differs from
the other ports is fixed up here.
*/

#ifndef __HALO_WASM_PREFIX_H
#define __HALO_WASM_PREFIX_H

/* wasi-libc defines struct timeval with any header that needs its types,
where musl and glibc wait for <sys/time.h>. Define it under its own name
first, before halo_linux_winsock_names.h renames the XDK's (game units). */
#include <time.h>

/* MSVC's va_list is char *, and the game declares its argument pointers
that way (terminal.c: char *arglist); WebAssembly's is void *, which
clang's builtins take only by reference to the exact type. Same size, same
meaning: cast. Defined ahead of <stdarg.h>, which then keeps these. */
#include <stdarg.h>
#undef va_start
#undef va_end
#undef va_arg
#undef va_copy
#define va_start(ap, param) __builtin_va_start(*(__builtin_va_list *)&(ap), param)
#define va_end(ap) __builtin_va_end(*(__builtin_va_list *)&(ap))
#define va_arg(ap, type) __builtin_va_arg(*(__builtin_va_list *)&(ap), type)
#define va_copy(destination, source) \
	__builtin_va_copy(*(__builtin_va_list *)&(destination), *(__builtin_va_list *)&(source))

/* WebAssembly code cannot see its call stack: get_return_eip
(hud_draw.c) and data.c's diagnostics read 0 */
#define __builtin_return_address(level) ((void *)0)
#define __builtin_frame_address(level) ((void *)0)

#endif /* __HALO_WASM_PREFIX_H */
