/*
GUEST_HOST.H

The services the WebAssembly guest imports from its host (module "halo"),
as the guest sees them. The macOS host defines them in port/macos/host;
see port/wasm/README.md for the design.

Every value crossing the boundary is a 32-bit or 64-bit integer or a float.
Pointers are guest addresses (offsets into the linear memory), which the
host translates; host objects (SDL windows, gamepads, audio streams) are
small integer handles.
*/

#ifndef __GUEST_HOST_H
#define __GUEST_HOST_H

#define HOST_IMPORT(name) __attribute__((import_module("halo"), import_name(#name))) name

/* ---------- the process */

/* the host's errno after the last host call on this thread, as a WASI
errno value (the guest's <errno.h>) */
int HOST_IMPORT(host_errno)(void);
/* the folder the game keeps its settings and data in, ending in a
separator (SDL_GetBasePath) */
void HOST_IMPORT(host_base_path)(char *buffer, unsigned int size);

/* ---------- memory (port/linux/src/xbox_memory.c)

Page protections of the linear memory, which the host maps: */
#define HOST_PROT_NONE 0
#define HOST_PROT_READ 1
#define HOST_PROT_WRITE 2

/* fresh zeroed pages over [address, address + size) with that protection;
0 on success */
int HOST_IMPORT(host_memory_map)(unsigned int address, unsigned int size, int protection);
int HOST_IMPORT(host_memory_protect)(unsigned int address, unsigned int size, int protection);

/* write tracking (port/linux/src/memory_watch.c's interface): the host
owns the page faults */
void HOST_IMPORT(host_memory_watch_initialize)(void);
void HOST_IMPORT(host_memory_watch_protect)(unsigned int address, unsigned int size);
unsigned int HOST_IMPORT(host_memory_watch_generation)(unsigned int address, unsigned int size);
unsigned int HOST_IMPORT(host_memory_watch_serial)(void);
void HOST_IMPORT(host_memory_watch_prepare_write)(unsigned int address, unsigned int size);
void HOST_IMPORT(host_memory_watch_forget)(unsigned int address, unsigned int size);

/* ---------- SDL (guest_sdl.c) */

int HOST_IMPORT(host_sdl_init)(unsigned int flags);
int HOST_IMPORT(host_sdl_set_hint)(const char *name, const char *value);
void HOST_IMPORT(host_sdl_get_error)(char *buffer, unsigned int size);
long long HOST_IMPORT(host_sdl_ticks_ns)(void);
long long HOST_IMPORT(host_sdl_thread_id)(void);
void HOST_IMPORT(host_sdl_delay_ns)(long long nanoseconds);
void HOST_IMPORT(host_sdl_scancode_name)(int scancode, char *buffer, unsigned int size);
int HOST_IMPORT(host_sdl_scancode_from_name)(const char *name);

/* windows */
unsigned int HOST_IMPORT(host_sdl_create_window)(const char *title, int width, int height, long long flags);
void HOST_IMPORT(host_sdl_destroy_window)(unsigned int window);
long long HOST_IMPORT(host_sdl_window_flags)(unsigned int window);
/* which: 0 the size, 1 the size in pixels */
void HOST_IMPORT(host_sdl_window_size)(unsigned int window, int which, int *width, int *height);
int HOST_IMPORT(host_sdl_set_window_size)(unsigned int window, int width, int height);
int HOST_IMPORT(host_sdl_set_window_fullscreen)(unsigned int window, int fullscreen);
/* the display's mode closest to width x height at refresh_rate for the
window's exclusive fullscreen, or with display 0, none (borderless) */
int HOST_IMPORT(host_sdl_set_window_fullscreen_mode)(unsigned int window, unsigned int display, int width,
	int height, float refresh_rate);
int HOST_IMPORT(host_sdl_set_relative_mouse)(unsigned int window, int enabled);
void HOST_IMPORT(host_sdl_warp_mouse)(unsigned int window, float x, float y);

/* displays */
unsigned int HOST_IMPORT(host_sdl_primary_display)(void);
unsigned int HOST_IMPORT(host_sdl_display_for_window)(unsigned int window);
/* which: 0 the desktop mode, 1 the current mode; fills a struct
guest_display_mode (guest_sdl.c); 0 if there is none */
int HOST_IMPORT(host_sdl_display_mode)(unsigned int display, int which, void *mode);

/* OpenGL ES contexts */
int HOST_IMPORT(host_sdl_gl_set_attribute)(int attribute, int value);
unsigned int HOST_IMPORT(host_sdl_gl_create_context)(unsigned int window);
int HOST_IMPORT(host_sdl_gl_make_current)(unsigned int window, unsigned int context);
int HOST_IMPORT(host_sdl_gl_set_swap_interval)(int interval);
int HOST_IMPORT(host_sdl_gl_swap_window)(unsigned int window);

/* the 2D renderer (the first start's progress window) */
unsigned int HOST_IMPORT(host_sdl_create_renderer)(unsigned int window);
void HOST_IMPORT(host_sdl_destroy_renderer)(unsigned int renderer);
int HOST_IMPORT(host_sdl_set_render_vsync)(unsigned int renderer, int vsync);
int HOST_IMPORT(host_sdl_set_render_draw_color)(unsigned int renderer, int r, int g, int b, int a);
int HOST_IMPORT(host_sdl_set_render_scale)(unsigned int renderer, float x, float y);
int HOST_IMPORT(host_sdl_render_clear)(unsigned int renderer);
int HOST_IMPORT(host_sdl_render_fill_rect)(unsigned int renderer, float x, float y, float w, float h);
int HOST_IMPORT(host_sdl_render_debug_text)(unsigned int renderer, float x, float y, const char *text);
int HOST_IMPORT(host_sdl_render_present)(unsigned int renderer);

/* events: SDL_Event has the same 128-byte layout in both ABIs for every
event the platform layer reads (none of them carries a pointer) */
int HOST_IMPORT(host_sdl_poll_event)(void *event);
void HOST_IMPORT(host_sdl_pump_events)(void);
int HOST_IMPORT(host_sdl_push_quit)(void);

/* dialogs */
int HOST_IMPORT(host_sdl_set_clipboard_text)(const char *text);
void HOST_IMPORT(host_sdl_get_clipboard_text)(char *buffer, unsigned int size);
int HOST_IMPORT(host_sdl_show_simple_message_box)(unsigned int flags, const char *title, const char *message);
/* buttons: count struct guest_message_box_button (guest_sdl.c); the
chosen button's id in *answer */
int HOST_IMPORT(host_sdl_show_message_box)(unsigned int flags, const char *title, const char *message,
	const void *buttons, int count, int *answer);
/* filters: "name\tpattern\n" lines; waits for the player and copies the
chosen file's path (empty if none) */
void HOST_IMPORT(host_sdl_choose_file)(const char *filters, char *path, unsigned int size);

/* gamepads */
int HOST_IMPORT(host_sdl_get_gamepads)(unsigned int *ids, int capacity);
unsigned int HOST_IMPORT(host_sdl_open_gamepad)(unsigned int id);
unsigned int HOST_IMPORT(host_sdl_gamepad_from_id)(unsigned int id);
int HOST_IMPORT(host_sdl_gamepad_axis)(unsigned int gamepad, int axis);
int HOST_IMPORT(host_sdl_gamepad_button)(unsigned int gamepad, int button);
int HOST_IMPORT(host_sdl_gamepad_type)(unsigned int gamepad);
int HOST_IMPORT(host_sdl_rumble_gamepad)(unsigned int gamepad, unsigned int low, unsigned int high,
	unsigned int milliseconds);

/* audio. A stream with a callback calls it on a guest thread (guest_sdl.c),
which waits in host_sdl_audio_wait for SDL to ask for audio (0 once the
stream is gone), runs the callback and says host_sdl_audio_done; what the
callback puts into the stream meanwhile goes in once it is done. */
unsigned int HOST_IMPORT(host_sdl_open_audio_stream)(unsigned int device, const void *spec, int callback);
int HOST_IMPORT(host_sdl_audio_wait)(unsigned int stream, int *additional, int *total);
void HOST_IMPORT(host_sdl_audio_done)(unsigned int stream);
int HOST_IMPORT(host_sdl_put_audio_stream_data)(unsigned int stream, const void *data, int length);
int HOST_IMPORT(host_sdl_resume_audio_stream_device)(unsigned int stream);

/* ---------- OpenGL ES (the generated guest_gl.c has the rest) */

/* copies glGetString(name) (or glGetStringi when index >= 0) */
void HOST_IMPORT(host_gl_get_string)(unsigned int name, int index, char *buffer, unsigned int size);
int HOST_IMPORT(host_gl_has_extension)(const char *name);
unsigned int HOST_IMPORT(host_gl_read_buffer_word)(unsigned int buffer, unsigned int offset);
void HOST_IMPORT(host_gl_buffer_write)(unsigned int target, unsigned int offset, unsigned int size,
	const void *data);
void HOST_IMPORT(host_gl_fence_frame)(unsigned int slot);
void HOST_IMPORT(host_gl_wait_frame)(unsigned int slot);

#endif
