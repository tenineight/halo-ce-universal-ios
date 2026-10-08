/*
HOST_MAIN.C

Entry point of the macOS port's host (port/macos/README.md).

It reserves the guest's linear memory (host_memory.c), starts SDL's video
on the main thread (host_sdl.c), instantiates the guest (wasm2c's
translation of halo.wasm, build/macos/wasm2c) and runs its _start on the
main thread, where SDL and Cocoa want the game's window and events. Each
thread the guest creates (wasi-threads' "thread-spawn") is a native thread
running its own instance of the module over the same memory.

The log goes to the standard error stream and to halo-host.txt in the
game's folder (host_base_path, ~/Library/Application Support/Halo/).
*/

#include "host.h"
#include "game.h"

#include <SDL3/SDL.h>
#ifdef HALO_IOS
/* (main becomes SDL_main, which UIKit's application delegate calls on the
main thread once the application has started) */
#include <SDL3/SDL_main.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* wasm2c's runtime's trap target (wasm-rt-impl.c, wasm-rt-impl.h) */
extern WASM_RT_THREAD_LOCAL wasm_rt_jmp_buf g_wasm_rt_jmp_buf;

/* the file descriptor of the guest's one WASI preopen, the host's "/"
(host_wasi.c) */
#define PREOPEN_FD 3

static struct w2c_halo halo_instance;
static w2c_game main_instance;

/* ---------- logging and termination */

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file;

static void log_line(const char *text)
{
	pthread_mutex_lock(&log_lock);
	fprintf(stderr, "halo-host: %s\n", text);
	if (log_file)
	{
		fprintf(log_file, "%s\n", text);
		fflush(log_file);
	}
	pthread_mutex_unlock(&log_lock);
}

void host_logf(const char *format, ...)
{
	char text[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	log_line(text);
}

void host_fatal(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	host_logf("fatal: %s", message);
	/* (Cocoa shows it on the main thread, which may be the one waiting for
	this one; started from a terminal, the message is there) */
	if (pthread_main_np() && !isatty(STDERR_FILENO))
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo", message, NULL);
	fflush(NULL);
	_exit(1);
}

static void log_open(void)
{
	const char *base = host_base_path();
	char path[1024];

	if (!base)
		return;
	snprintf(path, sizeof(path), "%shalo-host.txt", base);
	log_file = fopen(path, "w");
}

/* ---------- handles for host objects the guest holds */

static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;
static void **handle_objects;
static uint32_t handle_capacity;

uint32_t host_handle_new(void *object)
{
	uint32_t handle;

	if (!object)
		return 0;
	pthread_mutex_lock(&handle_lock);
	/* handle n is slot n - 1: 0 is no object */
	for (handle = 1; handle <= handle_capacity && handle_objects[handle - 1]; handle++)
		;
	if (handle > handle_capacity)
	{
		uint32_t capacity = handle_capacity ? handle_capacity * 2 : 64;
		void **grown = realloc(handle_objects, capacity * sizeof(*grown));

		if (!grown)
			host_fatal("out of memory for handles");
		memset(grown + handle_capacity, 0, (capacity - handle_capacity) * sizeof(*grown));
		handle_objects = grown;
		handle_capacity = capacity;
	}
	handle_objects[handle - 1] = object;
	pthread_mutex_unlock(&handle_lock);
	return handle;
}

void *host_handle_get(uint32_t handle)
{
	void *object = NULL;

	pthread_mutex_lock(&handle_lock);
	if (handle && handle <= handle_capacity)
		object = handle_objects[handle - 1];
	pthread_mutex_unlock(&handle_lock);
	return object;
}

void host_handle_free(uint32_t handle)
{
	pthread_mutex_lock(&handle_lock);
	if (handle && handle <= handle_capacity)
		handle_objects[handle - 1] = NULL;
	pthread_mutex_unlock(&handle_lock);
}

/* ---------- running guest code */

void host_run_guest(void (*function)(void *), void *argument, const char *what)
{
	/* (calls may nest: the guest calling the host calling the guest) */
	wasm_rt_jmp_buf saved = g_wasm_rt_jmp_buf;
	wasm_rt_trap_t trap;

	trap = (wasm_rt_trap_t)WASM_RT_SETJMP(g_wasm_rt_jmp_buf);
	if (trap != WASM_RT_TRAP_NONE)
	{
		uintptr_t address = host_fault_address;

		if (address && address - HALO_LINEAR_MEMORY_BASE < HOST_MEMORY_RESERVATION)
			host_fatal("%s stopped: %s (guest address %08llx, pc %p)", what, wasm_rt_strerror(trap),
				(unsigned long long)(address - HALO_LINEAR_MEMORY_BASE), (void *)host_fault_pc);
		if (address)
			host_fatal("%s stopped: %s (address %p, pc %p)", what, wasm_rt_strerror(trap), (void *)address,
				(void *)host_fault_pc);
		host_fatal("%s stopped: %s", what, wasm_rt_strerror(trap));
	}
	function(argument);
	g_wasm_rt_jmp_buf = saved;
}

static void instantiate(w2c_game *instance)
{
	/* (the WASI and memory imports keep no state per instance) */
	wasm2c_game_instantiate(instance, NULL, &halo_instance, NULL, NULL);
}

/* ---------- the guest's threads (wasi-threads) */

#define GUEST_THREAD_STACK_SIZE (8 * 1024 * 1024)
#define GUEST_THREAD_ID_LIMIT 0x1fffffff

struct guest_thread
{
	uint32_t id;
	uint32_t argument;
};

static void guest_thread_run(void *data)
{
	struct guest_thread *thread = data;
	w2c_game *instance = calloc(1, sizeof(*instance));

	if (!instance)
		host_fatal("out of memory for a guest thread");
	instantiate(instance);
	w2c_game_wasi_thread_start(instance, thread->id, thread->argument);
	wasm2c_game_free(instance);
	free(instance);
}

static void *guest_thread_main(void *data)
{
	wasm_rt_init_thread();
	host_run_guest(guest_thread_run, data, "a game thread");
	wasm_rt_free_thread();
	free(data);
	return NULL;
}

/* the guest's import "wasi" "thread-spawn": the new thread's id, or a
negative WASI errno value */
u32 w2c_wasi_thread0x2Dspawn(struct w2c_wasi *instance, u32 argument)
{
	static uint32_t next_id;
	struct guest_thread *thread = malloc(sizeof(*thread));
	pthread_attr_t attributes;
	pthread_t handle;
	int error;

	(void)instance;
	if (!thread)
		return (u32)-(int32_t)host_wasi_errno(ENOMEM);
	thread->id = __sync_add_and_fetch(&next_id, 1) % GUEST_THREAD_ID_LIMIT + 1;
	thread->argument = argument;
	pthread_attr_init(&attributes);
	pthread_attr_setstacksize(&attributes, GUEST_THREAD_STACK_SIZE);
	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	error = pthread_create(&handle, &attributes, guest_thread_main, thread);
	pthread_attr_destroy(&attributes);
	if (error)
	{
		host_logf("cannot start a game thread (%s)", strerror(error));
		free(thread);
		return (u32)-(int32_t)host_wasi_errno(error);
	}
	return thread->id;
}

/* ---------- the guest's imports (module "halo") */

u32 w2c_halo_host_errno(struct w2c_halo *instance)
{
	(void)instance;
	return host_wasi_errno(errno);
}

/* ---------- the entry point */

static void run_main(void *argument)
{
	(void)argument;
	instantiate(&main_instance);
	w2c_game_0x5Fstart(&main_instance);
}

int main(int argc, char **argv)
{
	int fd;

	signal(SIGPIPE, SIG_IGN);
	/* the guest's file system is the host's: its preopen "/" */
	fd = open("/", O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		host_fatal("cannot open the file system's root (%s)", strerror(errno));
	if (fd != PREOPEN_FD)
	{
		if (dup2(fd, PREOPEN_FD) != PREOPEN_FD)
			host_fatal("cannot open the file system's root as descriptor %d (%s)", PREOPEN_FD, strerror(errno));
		close(fd);
	}
	/* (after the preopen, which must be descriptor 3) */
	log_open();
#ifdef HALO_IOS
	host_logf("Halo for iOS (WebAssembly guest, wasm2c)");
#else
	host_logf("Halo for macOS (WebAssembly guest, wasm2c)");
#endif
#ifdef HALO_IOS
	/* (the game's few words for iOS: sdl_platform.c) */
	setenv("HALO_IOS", "1", 1);
#endif
	host_wasi_initialize(argc, argv);
	wasm_rt_init();
	host_memory_initialize(wasm2c_game_min_env_memory, wasm2c_game_max_env_memory);
	host_memory_watch_install();
	host_sdl_initialize();
	host_run_guest(run_main, NULL, "the game");
	/* main returned 0 (_start calls proc_exit otherwise) */
	host_logf("the game exited");
	fflush(NULL);
	exit(0);
}
