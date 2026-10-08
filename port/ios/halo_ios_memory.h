/*
HALO_IOS_MEMORY.H

Where the guest's linear memory is, for the iOS port. The macOS port maps
it at a fixed address (HALO_LINEAR_MEMORY_BASE, 1 TB) that wasm2c's code
addresses as a constant; an iPhone's address space ends far below that, so
on iOS the host maps it wherever the system has room
(port/macos/host/host_memory.c) and HALO_LINEAR_MEMORY_BASE is this
variable, set once before any guest code runs.

wasm2c's code sees it as a constant, so that the compiler keeps it in a
register rather than reloading it after every store the guest makes; only
the host (HALO_IOS_MEMORY_WRITER) writes it.
*/

#ifndef __HALO_IOS_MEMORY_H
#define __HALO_IOS_MEMORY_H

#include <stdint.h>

#ifdef HALO_IOS_MEMORY_WRITER
extern uintptr_t halo_linear_memory_base;
#else
extern const uintptr_t halo_linear_memory_base;
#endif

#endif
