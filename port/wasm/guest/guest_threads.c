/*
GUEST_THREADS.C

Threads whose procedure has another type than LPTHREAD_START_ROUTINE. A
WebAssembly call through a function pointer traps unless the pointer's type
is the function's own, so such a procedure is started through a function of
the right type (port/wasm/include/halo_wasm_prototypes.h).
*/

#include "platform.h"

/* ---------- private code */

static DWORD WINAPI void_thread_start(
	LPVOID procedure)
{
	((void (*)(void))procedure)();
	return 0;
}

/* ---------- public code */

/* CreateThread for a void (void) procedure cast to LPTHREAD_START_ROUTINE
(cache_files_windows.c); the procedure takes no parameter */
HANDLE WINAPI halo_wasm_create_void_thread(LPSECURITY_ATTRIBUTES attributes, DWORD stack_size,
	LPTHREAD_START_ROUTINE procedure, LPVOID parameter, DWORD flags, LPDWORD thread_id)
{
	(void)parameter;
	return CreateThread(attributes, stack_size, void_thread_start, (LPVOID)procedure, flags, thread_id);
}
