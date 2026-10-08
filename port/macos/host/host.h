/*
HOST.H

Internals of the macOS port's host (see port/macos/README.md and
port/wasm/README.md for the design). The host runs the guest, the game
compiled to WebAssembly and translated to C by wasm2c (build/macos/wasm2c),
and serves its imports: WASI (host_wasi.c), SDL (host_sdl.c), OpenGL ES
(host_gl.c and the generated host_gl_bridge.c), the platform layer's
posix_*.c (the generated host_posix_bridge.c) and its memory (host_memory.c).
*/

#ifndef __HALO_MACOS_HOST_H
#define __HALO_MACOS_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "wasm-rt.h"

/* wasm2c's types (wasm-rt.h has them only in the generated code) */
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;
typedef float f32;
typedef double f64;

/* the guest's imports of module "halo" take this as their first argument
(wasm2c's struct w2c_halo); the host has nothing per instance */
struct w2c_halo
{
	int unused;
};

/* ---------- logging (host_main.c) */

/* to the standard error stream and the game's debug.txt folder's
halo-host.txt */
void host_logf(const char *format, ...) __attribute__((format(printf, 1, 2)));
/* logs, shows the message to the player and terminates */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

/* ---------- the linear memory (host_memory.c)

The guest's memory is mapped at HALO_LINEAR_MEMORY_BASE (a constant the
build gives wasm2c's code too: tools/wasm2c_patch.py), with 4 GB of guard
pages after its 4 GB of addresses. */

#define HOST_MEMORY ((uint8_t *)HALO_LINEAR_MEMORY_BASE)
#define HOST_MEMORY_RESERVATION 0x200000000ull

/* reserves it, with initial_pages of 64 KB accessible; returns the memory
the guest imports */
wasm_rt_shared_memory_t *host_memory_initialize(uint64_t initial_pages, uint64_t maximum_pages);
extern wasm_rt_shared_memory_t host_memory;

/* the host address of guest address `address`, or NULL for 0 */
static inline void *host_guest_pointer(uint32_t address)
{
	return address ? HOST_MEMORY + address : NULL;
}

/* the guest address of a host pointer into the linear memory */
static inline uint32_t host_guest_address(const void *pointer)
{
	return (uint32_t)((const uint8_t *)pointer - HOST_MEMORY);
}

/* an array of `count` guest pointers as host pointers (in a buffer of this
thread's, valid until its next call) */
void *const *host_guest_pointer_array(uint32_t address, uint32_t count);

/* the write tracking's page faults (host_memory.c): installed after
wasm2c's runtime, whose handler they fall back to */
void host_memory_watch_install(void);
/* unprotects the watched pages of [address, address + size) ahead of a
write the host makes on the guest's behalf (the kernel's, in read(), fails
with EFAULT rather than fault) */
void host_memory_prepare_write(uint32_t address, uint32_t size);
/* the last page fault nothing expected (host address, and the pc), for the
report of the trap it becomes */
extern volatile uintptr_t host_fault_address, host_fault_pc;

/* ---------- handles for host objects the guest holds (host_main.c) */

uint32_t host_handle_new(void *object);
void *host_handle_get(uint32_t handle);
void host_handle_free(uint32_t handle);

/* ---------- errno (host_wasi.c) */

/* the WASI errno value of a host errno value */
uint16_t host_wasi_errno(int error);
/* the guest's command line (its argv[0] is "halo", then the host's
arguments) and environment */
void host_wasi_initialize(int argc, char **argv);

/* ---------- the guest's threads (host_main.c) */

/* runs function(argument) with a trap handler: a trap in the guest is
fatal, with what it was */
void host_run_guest(void (*function)(void *), void *argument, const char *what);

/* ---------- SDL and OpenGL ES (host_sdl.c, host_gl.c) */

/* the folder the game keeps its settings and data in, ending in '/' */
const char *host_base_path(void);
/* starts SDL's video (with ANGLE for OpenGL ES) on the main thread */
void host_sdl_initialize(void);
#ifndef HALO_IOS
/* takes the Command key shortcuts off SDL's menus (host_menu.m) */
void host_menu_remove_command_shortcuts(void);
#endif

#endif
