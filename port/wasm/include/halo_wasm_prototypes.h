/*
HALO_WASM_PROTOTYPES.H

Force-included by the WebAssembly guest build (tools/macos_build.py) into
every game unit, after the prefix headers. Some game files call functions
with no prototype in scope, or declare them with another return type. x86
ignores the difference (the result is in EAX either way, the caller pops
the arguments), but a WebAssembly call carries the callee's exact type:
wasm-ld turns a direct call whose type differs from the definition's into a
trap, and an indirect one traps at run time. So every such call must see the
real declaration. The guest link (-Wl,--fatal-warnings) reports any left.
*/

#ifndef __HALO_WASM_PROTOTYPES_H
#define __HALO_WASM_PROTOTYPES_H

/* ---------- calls without a prototype in scope */

/* cseries.h; libtiff's tif_read.c calls it without including cseries.h,
which declares it as returning int */
void debug_free(void *pointer, const char *file, long line);

/* game_engine.h; object_types.c calls it without including game_engine.h */
void game_engine_vehicle_placement_begin(void);

/* ---------- libtiff's lseek */

/* Defined by tools/macos_build.py for source/bitmaps/libtiff/. libtiff
declares `extern long lseek();` (tiffcompat.h) rather than including
<unistd.h>, so its calls pass and return a 32-bit long, while wasi-libc's
lseek takes and returns a 64-bit off_t. A prototype ahead of it would
conflict with that declaration, so the name is renamed in those units to a
local function of libtiff's type that calls the real one. */
#ifdef HALO_WASM_LIBTIFF

long long lseek(int fd, long long offset, int whence);

static __inline__ long halo_wasm_lseek_long(int fd, long offset, int whence)
{
	return (long)lseek(fd, offset, whence);
}

#define lseek halo_wasm_lseek_long

#endif /* HALO_WASM_LIBTIFF */

/* ---------- MSVC intrinsics */

/* scenario.c declares _ReadWriteBarrier and `#pragma intrinsic`s it: a
compiler-only barrier, which clang knows as a builtin only when it targets
MSVC. Here it would be a call to an undefined function. An empty asm
statement that clobbers memory is the same barrier; the source's own
declaration then redeclares this internal function. */
static __inline__ void _ReadWriteBarrier(void)
{
	__asm__ __volatile__("" ::: "memory");
}

/* ---------- results declared as void */

/* Defined by tools/macos_build.py for the units that declare these
rasterizer_xbox.c functions as returning void while the definitions return a
value: rasterizer_xbox_plasma_energy.c (rasterizer_set_texture),
rasterizer_xbox_motion_sensor.c and rasterizer_xbox_text.c
(rasterizer_set_texture_bitmap_data). Their declaration cannot be fixed by a
prototype ahead of it (two different return types are an error), so the
name is renamed in those units to a local void function that calls the real
one and drops the result, exactly what the x86 code did. */
#ifdef HALO_WASM_VOID_RESULT_FIXUPS

union point2d;
struct bitmap_data;

union point2d *rasterizer_set_texture(short stage, short bitmap_type, short bitmap_usage,
	long bitmap_definition_index, short bitmap_sequence_index);
unsigned char rasterizer_set_texture_bitmap_data(short stage, struct bitmap_data const *bitmap);

static __inline__ void halo_wasm_rasterizer_set_texture_void(short stage, short bitmap_type,
	short bitmap_usage, long bitmap_definition_index, short bitmap_sequence_index)
{
	rasterizer_set_texture(stage, bitmap_type, bitmap_usage, bitmap_definition_index, bitmap_sequence_index);
}

static __inline__ void halo_wasm_rasterizer_set_texture_bitmap_data_void(short stage,
	struct bitmap_data const *bitmap)
{
	rasterizer_set_texture_bitmap_data(stage, bitmap);
}

#define rasterizer_set_texture halo_wasm_rasterizer_set_texture_void
#define rasterizer_set_texture_bitmap_data halo_wasm_rasterizer_set_texture_bitmap_data_void

#endif /* HALO_WASM_VOID_RESULT_FIXUPS */

/* ---------- a thread procedure declared as void (void) */

/* Defined by tools/macos_build.py for cache_files_windows.c, which starts
its thread with (LPTHREAD_START_ROUTINE)cache_file_windows_thread_proc, a
void (void) function: CreateThread (port/linux/src/xbox_kernel.c) would call
it as DWORD (LPVOID), which traps. CreateThread is renamed in that unit (its
XDK declaration with it) to port/wasm/guest/guest_threads.c's, which starts
the procedure through a function of the right type. */
#ifdef HALO_WASM_VOID_THREAD_PROCEDURE
#define CreateThread halo_wasm_create_void_thread
#endif

#endif /* __HALO_WASM_PROTOTYPES_H */
