/*
HOST_SDL.C

SDL3 on behalf of the guest (port/wasm/guest/guest_sdl.c), which imports
these from module "halo" (port/wasm/guest/guest_host.h). SDL's objects are
host pointers, which the guest cannot hold; it gets small handles into the
table here instead. Its pointers are guest addresses (host_guest_pointer).

OpenGL ES comes from ANGLE (on Metal) through EGL: host_sdl_initialize
points SDL at the libraries in the application bundle's Frameworks folder.
On iOS (HALO_IOS) it is the system's, through SDL's UIKit video.
*/

#include "host.h"
#include "host_gl_functions.h"

#include <SDL3/SDL.h>
#ifdef HALO_IOS
#include "host_picker.h"
#include "host_touch.h"
#endif
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------- handles */

#define HANDLE_COUNT 256

enum handle_type
{
	_handle_free,
	_handle_window,
	_handle_context,
	_handle_renderer,
	_handle_gamepad,
	_handle_audio,
};

struct handle
{
	int type;
	void *object;
};

static struct handle handles[HANDLE_COUNT];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t handle_new(int type, void *object)
{
	uint32_t index;

	if (!object)
		return 0;
	pthread_mutex_lock(&handle_lock);
	/* an object that already has a handle keeps it */
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == type && handles[index].object == object)
		{
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == _handle_free)
		{
			handles[index].type = type;
			handles[index].object = object;
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	pthread_mutex_unlock(&handle_lock);
	host_logf("out of SDL handles");
	return 0;
}

static void *handle_get(uint32_t handle, int type)
{
	void *object = NULL;

	if (handle == 0 || handle >= HANDLE_COUNT)
		return NULL;
	pthread_mutex_lock(&handle_lock);
	if (handles[handle].type == type)
		object = handles[handle].object;
	pthread_mutex_unlock(&handle_lock);
	return object;
}

static void handle_free(uint32_t handle, int type)
{
	if (handle == 0 || handle >= HANDLE_COUNT)
		return;
	pthread_mutex_lock(&handle_lock);
	if (handles[handle].type == type)
	{
		handles[handle].type = _handle_free;
		handles[handle].object = NULL;
	}
	pthread_mutex_unlock(&handle_lock);
}

/* copies text into the guest's buffer of size bytes */
static void copy_to_guest(u32 buffer, u32 size, const char *text)
{
	char *destination = host_guest_pointer(buffer);

	if (destination && size)
		SDL_strlcpy(destination, text ? text : "", size);
}

static void store_guest_int(u32 address, int value)
{
	int *destination = host_guest_pointer(address);

	if (destination)
		*destination = value;
}

/* ---------- starting up */

#ifdef HALO_IOS
/* iOS stops an application in the background from using the GPU: from
when it is about to go there until it is back, the game waits at its next
frame (w2c_halo_host_sdl_gl_swap_window). SDL hands the application's
comings and goings to a watch as they happen. */
static volatile int in_background;

static bool SDLCALL lifecycle_watch(void *data, SDL_Event *event)
{
	(void)data;
	if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND || event->type == SDL_EVENT_DID_ENTER_BACKGROUND)
		in_background = 1;
	else if (event->type == SDL_EVENT_WILL_ENTER_FOREGROUND || event->type == SDL_EVENT_DID_ENTER_FOREGROUND)
		in_background = 0;
	return true;
}

static void wait_in_background(void)
{
	if (!in_background)
		return;
	host_logf("in the background: the game waits");
	/* (what was drawn, before the GPU is out of reach) */
	host_glFinish();
	while (in_background)
	{
		/* (UIKit tells of the return through the main thread's run loop,
		which SDL runs here) */
		SDL_PumpEvents();
		SDL_Delay(50);
	}
	host_logf("back in the foreground");
}

/* iOS has OpenGL ES itself (OpenGLES.framework), which SDL's UIKit video
offers without EGL */
void host_sdl_initialize(void)
{
	SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
	/* (the touches are the on-screen controls': host_touch.c) */
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
	SDL_SetAppMetadata("Halo", NULL, NULL);
	if (!SDL_Init(SDL_INIT_VIDEO))
		host_fatal("SDL cannot start its video: %s", SDL_GetError());
	SDL_AddEventWatch(lifecycle_watch, NULL);
}
#else
/* folder/name if that file exists */
static int library_in(char *path, size_t size, const char *folder, const char *name)
{
	char candidate[PATH_MAX];

	snprintf(candidate, sizeof(candidate), "%s/%s", folder, name);
	if (access(candidate, R_OK) != 0)
		return 0;
	if (!realpath(candidate, path))
		SDL_strlcpy(path, candidate, size);
	return 1;
}

/* the folder holding ANGLE's libEGL.dylib and libGLESv2.dylib: the bundle's
Frameworks folder, or when the host runs outside the bundle, the build's
copy (build/macos/third_party/angle) in a folder above the executable or
the working directory */
static int angle_folder(char *folder, size_t size)
{
	char executable[PATH_MAX], resolved[PATH_MAX], directory[PATH_MAX], path[PATH_MAX];
	uint32_t length = sizeof(executable);
	int depth;

	if (_NSGetExecutablePath(executable, &length) != 0 || !realpath(executable, resolved))
		SDL_strlcpy(resolved, ".", sizeof(resolved));
	SDL_strlcpy(directory, dirname(resolved), sizeof(directory));
	snprintf(folder, size, "%s/../Frameworks", directory);
	if (library_in(path, sizeof(path), folder, "libEGL.dylib"))
		return 1;
	for (depth = 0; depth < 8; depth++)
	{
		static const char *const subfolders[] = { "third_party/angle", "build/macos/third_party/angle" };
		int index;

		for (index = 0; index < 2; index++)
		{
			snprintf(folder, size, "%s/%s", directory, subfolders[index]);
			if (library_in(path, sizeof(path), folder, "libEGL.dylib"))
				return 1;
		}
		if (!strcmp(directory, "/"))
			break;
		SDL_strlcpy(directory, dirname(directory), sizeof(directory));
	}
	if (getcwd(directory, sizeof(directory)))
	{
		snprintf(folder, size, "%s/build/macos/third_party/angle", directory);
		if (library_in(path, sizeof(path), folder, "libEGL.dylib"))
			return 1;
	}
	return 0;
}

void host_sdl_initialize(void)
{
	char folder[PATH_MAX], egl[PATH_MAX], gles[PATH_MAX];

	if (!angle_folder(folder, sizeof(folder)) ||
		!library_in(egl, sizeof(egl), folder, "libEGL.dylib") ||
		!library_in(gles, sizeof(gles), folder, "libGLESv2.dylib"))
		host_fatal("ANGLE's libEGL.dylib and libGLESv2.dylib are missing from the application's Frameworks folder");
	host_logf("OpenGL ES: ANGLE from %s", folder);
	/* Cocoa offers OpenGL ES only through EGL, and only when forced to;
	it decides when its video starts, so before SDL_Init */
	SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
	SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
	SDL_SetHint(SDL_HINT_EGL_LIBRARY, egl);
	SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, gles);
	/* ANGLE's default display on macOS is its OpenGL backend, on Apple's
	deprecated OpenGL; its Metal one unless the player says otherwise */
	setenv("ANGLE_DEFAULT_PLATFORM", "metal", 0);
	SDL_SetAppMetadata("Halo", NULL, NULL);
	if (!SDL_Init(SDL_INIT_VIDEO))
		host_fatal("SDL cannot start its video: %s", SDL_GetError());
	/* (the Command keys fire: host_menu.m) */
	host_menu_remove_command_shortcuts();
}

#endif

/* ---------- the process */

const char *host_base_path(void)
{
	static char path[PATH_MAX];

	if (!path[0])
	{
#ifdef HALO_IOS
		/* the application's Documents folder, which the Files app shows */
		const char *documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);

		if (!documents)
			host_fatal("no folder for the game's data: %s", SDL_GetError());
		snprintf(path, sizeof(path), "%s%s", documents, documents[strlen(documents) - 1] == '/' ? "" : "/");
#else
		/* ~/Library/Application Support/Halo/, created if need be */
		char *preferences = SDL_GetPrefPath("", "Halo");

		if (!preferences)
			host_fatal("no folder for the game's data: %s", SDL_GetError());
		SDL_strlcpy(path, preferences, sizeof(path));
		SDL_free(preferences);
#endif
	}
	return path;
}

void w2c_halo_host_base_path(struct w2c_halo *instance, u32 buffer, u32 size)
{
	(void)instance;
	copy_to_guest(buffer, size, host_base_path());
}

/* ---------- general */

u32 w2c_halo_host_sdl_init(struct w2c_halo *instance, u32 flags)
{
	(void)instance;
	return SDL_Init((SDL_InitFlags)flags) ? 1 : 0;
}

u32 w2c_halo_host_sdl_set_hint(struct w2c_halo *instance, u32 name, u32 value)
{
	(void)instance;
	return SDL_SetHint(host_guest_pointer(name), host_guest_pointer(value)) ? 1 : 0;
}

void w2c_halo_host_sdl_get_error(struct w2c_halo *instance, u32 buffer, u32 size)
{
	(void)instance;
	copy_to_guest(buffer, size, SDL_GetError());
}

u64 w2c_halo_host_sdl_ticks_ns(struct w2c_halo *instance)
{
	(void)instance;
	return SDL_GetTicksNS();
}

u64 w2c_halo_host_sdl_thread_id(struct w2c_halo *instance)
{
	(void)instance;
	return (u64)SDL_GetCurrentThreadID();
}

void w2c_halo_host_sdl_delay_ns(struct w2c_halo *instance, u64 nanoseconds)
{
	(void)instance;
	if ((s64)nanoseconds > 0)
		SDL_DelayPrecise(nanoseconds);
}

void w2c_halo_host_sdl_scancode_name(struct w2c_halo *instance, u32 scancode, u32 buffer, u32 size)
{
	(void)instance;
	copy_to_guest(buffer, size, SDL_GetScancodeName((SDL_Scancode)(s32)scancode));
}

u32 w2c_halo_host_sdl_scancode_from_name(struct w2c_halo *instance, u32 name)
{
	(void)instance;
	return (u32)SDL_GetScancodeFromName(host_guest_pointer(name));
}

/* ---------- windows */

u32 w2c_halo_host_sdl_create_window(struct w2c_halo *instance, u32 title, u32 width, u32 height, u64 flags)
{
	(void)instance;
	return handle_new(_handle_window,
		SDL_CreateWindow(host_guest_pointer(title), (int)width, (int)height, (SDL_WindowFlags)flags));
}

void w2c_halo_host_sdl_destroy_window(struct w2c_halo *instance, u32 window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	(void)instance;
	if (!object)
		return;
	handle_free(window, _handle_window);
	SDL_DestroyWindow(object);
}

u64 w2c_halo_host_sdl_window_flags(struct w2c_halo *instance, u32 window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	(void)instance;
	return object ? (u64)SDL_GetWindowFlags(object) : 0;
}

void w2c_halo_host_sdl_window_size(struct w2c_halo *instance, u32 window, u32 which, u32 width, u32 height)
{
	SDL_Window *object = handle_get(window, _handle_window);
	int w = 0, h = 0;

	(void)instance;
	if (object)
	{
		if (which)
			SDL_GetWindowSizeInPixels(object, &w, &h);
		else
			SDL_GetWindowSize(object, &w, &h);
	}
	store_guest_int(width, w);
	store_guest_int(height, h);
}

u32 w2c_halo_host_sdl_set_window_size(struct w2c_halo *instance, u32 window, u32 width, u32 height)
{
	SDL_Window *object = handle_get(window, _handle_window);

	(void)instance;
	return object && SDL_SetWindowSize(object, (int)width, (int)height) ? 1 : 0;
}

u32 w2c_halo_host_sdl_set_window_fullscreen(struct w2c_halo *instance, u32 window, u32 fullscreen)
{
	SDL_Window *object = handle_get(window, _handle_window);

	(void)instance;
	return object && SDL_SetWindowFullscreen(object, fullscreen != 0) ? 1 : 0;
}

u32 w2c_halo_host_sdl_set_window_fullscreen_mode(struct w2c_halo *instance, u32 window, u32 display, u32 width,
	u32 height, f32 refresh_rate)
{
	SDL_Window *object = handle_get(window, _handle_window);
	SDL_DisplayMode mode;

	(void)instance;
	if (!object)
		return 0;
	if (!display)
		return SDL_SetWindowFullscreenMode(object, NULL) ? 1 : 0;
	if (!SDL_GetClosestFullscreenDisplayMode((SDL_DisplayID)display, (int)width, (int)height, refresh_rate, true,
		&mode))
		return 0;
	return SDL_SetWindowFullscreenMode(object, &mode) ? 1 : 0;
}

u32 w2c_halo_host_sdl_set_relative_mouse(struct w2c_halo *instance, u32 window, u32 enabled)
{
	SDL_Window *object = handle_get(window, _handle_window);

	(void)instance;
#ifdef HALO_IOS
	touch_controls_set_playing(enabled != 0);
#endif
	return object && SDL_SetWindowRelativeMouseMode(object, enabled != 0) ? 1 : 0;
}

void w2c_halo_host_sdl_warp_mouse(struct w2c_halo *instance, u32 window, f32 x, f32 y)
{
	SDL_Window *object = handle_get(window, _handle_window);

	(void)instance;
	if (object)
		SDL_WarpMouseInWindow(object, x, y);
}

/* ---------- displays */

/* guest_sdl.c's struct guest_display_mode: the same layout in both ABIs */
struct guest_display_mode
{
	uint32_t format;
	int32_t w;
	int32_t h;
	float pixel_density;
	float refresh_rate;
	int32_t refresh_rate_numerator;
	int32_t refresh_rate_denominator;
};

u32 w2c_halo_host_sdl_primary_display(struct w2c_halo *instance)
{
	(void)instance;
	return (u32)SDL_GetPrimaryDisplay();
}

u32 w2c_halo_host_sdl_display_for_window(struct w2c_halo *instance, u32 window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	(void)instance;
	return object ? (u32)SDL_GetDisplayForWindow(object) : 0;
}

u32 w2c_halo_host_sdl_display_mode(struct w2c_halo *instance, u32 display, u32 which, u32 mode)
{
	const SDL_DisplayMode *source = which ? SDL_GetCurrentDisplayMode((SDL_DisplayID)display) :
		SDL_GetDesktopDisplayMode((SDL_DisplayID)display);
	struct guest_display_mode *destination = host_guest_pointer(mode);

	(void)instance;
	if (!source || !destination)
		return 0;
	destination->format = (uint32_t)source->format;
	destination->w = source->w;
	destination->h = source->h;
	destination->pixel_density = source->pixel_density;
	destination->refresh_rate = source->refresh_rate;
	destination->refresh_rate_numerator = source->refresh_rate_numerator;
	destination->refresh_rate_denominator = source->refresh_rate_denominator;
	return 1;
}

/* ---------- OpenGL ES contexts */

u32 w2c_halo_host_sdl_gl_set_attribute(struct w2c_halo *instance, u32 attribute, u32 value)
{
	(void)instance;
	return SDL_GL_SetAttribute((SDL_GLAttr)attribute, (int)value) ? 1 : 0;
}

#ifdef HALO_IOS
/* UIKit has no default framebuffer: SDL draws a window into a framebuffer
object of its own, which takes the place of framebuffer 0 the renderer
presents to (d3d8_gl.c) */
static GLuint window_framebuffer;
static int drawing_to_window;
static void (GL_APIENTRY *driver_glBindFramebuffer)(GLenum target, GLuint framebuffer);
static void (GL_APIENTRY *driver_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
	GLbitfield, GLenum);

static void GL_APIENTRY bind_framebuffer(GLenum target, GLuint framebuffer)
{
	if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)
		drawing_to_window = framebuffer == 0 || framebuffer == window_framebuffer;
	driver_glBindFramebuffer(target, framebuffer ? framebuffer : window_framebuffer);
}

/* the picture's blit to the window: in portrait the on-screen controls
move it up (host_touch.c) */
static void GL_APIENTRY blit_framebuffer(GLint source_x0, GLint source_y0, GLint source_x1, GLint source_y1,
	GLint x0, GLint y0, GLint x1, GLint y1, GLbitfield mask, GLenum filter)
{
	if (drawing_to_window)
		touch_controls_place_picture(&x0, &y0, &x1, &y1);
	driver_glBlitFramebuffer(source_x0, source_y0, source_x1, source_y1, x0, y0, x1, y1, mask, filter);
}

static void window_framebuffer_install(SDL_Window *window)
{
	window_framebuffer = (GLuint)SDL_GetNumberProperty(SDL_GetWindowProperties(window),
		SDL_PROP_WINDOW_UIKIT_OPENGL_FRAMEBUFFER_NUMBER, 0);
	driver_glBindFramebuffer = host_glBindFramebuffer;
	host_glBindFramebuffer = bind_framebuffer;
	driver_glBlitFramebuffer = host_glBlitFramebuffer;
	host_glBlitFramebuffer = blit_framebuffer;
	host_logf("OpenGL ES: the window is framebuffer %u", (unsigned)window_framebuffer);
	touch_controls_initialize(window, window_framebuffer);
}
#endif

u32 w2c_halo_host_sdl_gl_create_context(struct w2c_halo *instance, u32 window)
{
	SDL_Window *object = handle_get(window, _handle_window);
	SDL_GLContext context;

	(void)instance;
	if (!object)
		return 0;
	/* SDL makes a new context current */
	context = SDL_GL_CreateContext(object);
	if (!context)
		return 0;
	if (!host_gl_load())
		host_fatal("the OpenGL ES driver lacks functions the game needs");
#ifdef HALO_IOS
	window_framebuffer_install(object);
#endif
	return handle_new(_handle_context, context);
}

u32 w2c_halo_host_sdl_gl_make_current(struct w2c_halo *instance, u32 window, u32 context)
{
	(void)instance;
	return SDL_GL_MakeCurrent(handle_get(window, _handle_window), handle_get(context, _handle_context)) ? 1 : 0;
}

u32 w2c_halo_host_sdl_gl_set_swap_interval(struct w2c_halo *instance, u32 interval)
{
	(void)instance;
	return SDL_GL_SetSwapInterval((int)interval) ? 1 : 0;
}

u32 w2c_halo_host_sdl_gl_swap_window(struct w2c_halo *instance, u32 window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	(void)instance;
#ifdef HALO_IOS
	if (in_background)
	{
		/* (this frame was drawn as the GPU went out of reach: not shown) */
		wait_in_background();
		return 1;
	}
	touch_controls_draw();
#endif
	return object && SDL_GL_SwapWindow(object) ? 1 : 0;
}

/* ---------- the 2D renderer */

u32 w2c_halo_host_sdl_create_renderer(struct w2c_halo *instance, u32 window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	(void)instance;
	return object ? handle_new(_handle_renderer, SDL_CreateRenderer(object, NULL)) : 0;
}

void w2c_halo_host_sdl_destroy_renderer(struct w2c_halo *instance, u32 renderer)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	(void)instance;
	if (!object)
		return;
	handle_free(renderer, _handle_renderer);
	SDL_DestroyRenderer(object);
}

u32 w2c_halo_host_sdl_set_render_vsync(struct w2c_halo *instance, u32 renderer, u32 vsync)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	(void)instance;
	return object && SDL_SetRenderVSync(object, (int)vsync) ? 1 : 0;
}

u32 w2c_halo_host_sdl_set_render_draw_color(struct w2c_halo *instance, u32 renderer, u32 r, u32 g, u32 b, u32 a)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	(void)instance;
	return object && SDL_SetRenderDrawColor(object, (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a) ? 1 : 0;
}

u32 w2c_halo_host_sdl_set_render_scale(struct w2c_halo *instance, u32 renderer, f32 x, f32 y)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	(void)instance;
	return object && SDL_SetRenderScale(object, x, y) ? 1 : 0;
}

u32 w2c_halo_host_sdl_render_clear(struct w2c_halo *instance, u32 renderer)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	(void)instance;
	return object && SDL_RenderClear(object) ? 1 : 0;
}

u32 w2c_halo_host_sdl_render_fill_rect(struct w2c_halo *instance, u32 renderer, f32 x, f32 y, f32 w, f32 h)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);
	SDL_FRect rectangle = { x, y, w, h };

	(void)instance;
	return object && SDL_RenderFillRect(object, &rectangle) ? 1 : 0;
}

u32 w2c_halo_host_sdl_render_debug_text(struct w2c_halo *instance, u32 renderer, f32 x, f32 y, u32 text)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	(void)instance;
	return object && SDL_RenderDebugText(object, x, y, host_guest_pointer(text)) ? 1 : 0;
}

u32 w2c_halo_host_sdl_render_present(struct w2c_halo *instance, u32 renderer)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	(void)instance;
	return object && SDL_RenderPresent(object) ? 1 : 0;
}

/* ---------- events */

#ifdef HALO_IOS
/* the invite link the application was opened with, for the game's next
look at the clipboard (w2c_halo_host_sdl_get_clipboard_text) */
static char opened_url[256];

/* UIKit hands SDL the URL the application was opened with as a drop */
static int ios_opened_url(const SDL_Event *event)
{
	if (event->type != SDL_EVENT_DROP_FILE && event->type != SDL_EVENT_DROP_TEXT)
		return 0;
	if (event->drop.data && !SDL_strncasecmp(event->drop.data, "halo://", 7))
	{
		host_logf("opened with %s: to join", event->drop.data);
		SDL_strlcpy(opened_url, event->drop.data, sizeof(opened_url));
	}
	return 1;
}
#endif

_Static_assert(sizeof(SDL_Event) == 128, "the guest's SDL_Event is 128 bytes");

u32 w2c_halo_host_sdl_poll_event(struct w2c_halo *instance, u32 event)
{
	SDL_Event host_event;
	void *destination = host_guest_pointer(event);

	(void)instance;
	do
	{
		if (!SDL_PollEvent(&host_event))
			return 0;
	}
#ifdef HALO_IOS
	/* (the touches are the on-screen controls' and the mouse's; an invite
	link the application was opened with is what the game, coming to the
	front, finds on the clipboard: network.join_from_clipboard) */
	while (touch_controls_event(&host_event) || ios_opened_url(&host_event));
#else
	while (0);
#endif
	/* the layouts agree except for the pointers of text, drop and user
	events, which the guest does not read */
	if (destination)
		memcpy(destination, &host_event, sizeof(host_event));
	return 1;
}

void w2c_halo_host_sdl_pump_events(struct w2c_halo *instance)
{
	(void)instance;
	SDL_PumpEvents();
}

u32 w2c_halo_host_sdl_push_quit(struct w2c_halo *instance)
{
	SDL_Event event;

	(void)instance;
	SDL_zero(event);
	event.type = SDL_EVENT_QUIT;
	event.quit.timestamp = SDL_GetTicksNS();
	return SDL_PushEvent(&event) ? 1 : 0;
}

/* ---------- dialogs */

u32 w2c_halo_host_sdl_set_clipboard_text(struct w2c_halo *instance, u32 text)
{
	(void)instance;
	return SDL_SetClipboardText(host_guest_pointer(text)) ? 1 : 0;
}

void w2c_halo_host_sdl_get_clipboard_text(struct w2c_halo *instance, u32 buffer, u32 size)
{
#ifdef HALO_IOS
	/* iOS asks the player each time an application reads the clipboard,
	which the game does whenever it comes to the front (for an invite link:
	network.join_from_clipboard); it reads only the invite link the
	application was opened with, once (ios_opened_url) */
	(void)instance;
	copy_to_guest(buffer, size, opened_url);
	opened_url[0] = 0;
#else
	char *text = SDL_GetClipboardText();

	(void)instance;
	copy_to_guest(buffer, size, text);
	SDL_free(text);
#endif
}

u32 w2c_halo_host_sdl_show_simple_message_box(struct w2c_halo *instance, u32 flags, u32 title, u32 message)
{
	(void)instance;
	return SDL_ShowSimpleMessageBox((SDL_MessageBoxFlags)flags, host_guest_pointer(title),
		host_guest_pointer(message), NULL) ? 1 : 0;
}

/* guest_sdl.c's struct guest_message_box_button */
struct guest_message_box_button
{
	uint32_t flags;
	int32_t id;
	uint32_t text;
};

u32 w2c_halo_host_sdl_show_message_box(struct w2c_halo *instance, u32 flags, u32 title, u32 message,
	u32 buttons, u32 count, u32 answer)
{
	const struct guest_message_box_button *guest_buttons = host_guest_pointer(buttons);
	SDL_MessageBoxButtonData host_buttons[8];
	SDL_MessageBoxData data;
	int index, button_count = (int)count, chosen = -1;
	bool result;

	(void)instance;
	if (button_count < 0 || !guest_buttons)
		button_count = 0;
	if (button_count > 8)
		button_count = 8;
	for (index = 0; index < button_count; index++)
	{
		host_buttons[index].flags = (SDL_MessageBoxButtonFlags)guest_buttons[index].flags;
		host_buttons[index].buttonID = guest_buttons[index].id;
		host_buttons[index].text = host_guest_pointer(guest_buttons[index].text);
	}
	SDL_zero(data);
	data.flags = (SDL_MessageBoxFlags)flags;
	data.title = host_guest_pointer(title);
	data.message = host_guest_pointer(message);
	data.numbuttons = button_count;
	data.buttons = host_buttons;
	result = SDL_ShowMessageBox(&data, &chosen);
	store_guest_int(answer, chosen);
	return result ? 1 : 0;
}

struct choose_file_state
{
	int done;
	char path[PATH_MAX];
};

static void SDLCALL choose_file_done(void *userdata, const char *const *files, int filter)
{
	struct choose_file_state *state = userdata;

	(void)filter;
	if (!files)
		host_logf("the file dialog failed: %s", SDL_GetError());
	else if (files[0])
		SDL_strlcpy(state->path, files[0], sizeof(state->path));
	__atomic_store_n(&state->done, 1, __ATOMIC_RELEASE);
}

void w2c_halo_host_sdl_choose_file(struct w2c_halo *instance, u32 filters, u32 path, u32 size)
{
	const char *text = host_guest_pointer(filters);
	char copy[1024];
	char *names[16], *patterns[16];
	SDL_DialogFileFilter list[16];
	struct choose_file_state state;
	char *line;
	int count = 0, index;

	(void)instance;
	/* "name\tpattern\n" lines */
	SDL_strlcpy(copy, text ? text : "", sizeof(copy));
	for (line = copy; *line && count < 16;)
	{
		char *end = strchr(line, '\n');
		char *tab;

		if (end)
			*end = 0;
		tab = strchr(line, '\t');
		if (tab)
		{
			*tab = 0;
			names[count] = line;
			patterns[count] = tab + 1;
			count++;
		}
		if (!end)
			break;
		line = end + 1;
	}
	for (index = 0; index < count; index++)
	{
		list[index].name = names[index];
		list[index].pattern = patterns[index];
	}
	state.done = 0;
	state.path[0] = 0;
#ifdef HALO_IOS
	/* (SDL has no file dialogs on iOS) */
	ios_pick_file(NULL, count ? list : NULL, count, choose_file_done, &state);
#else
	SDL_ShowOpenFileDialog(choose_file_done, &state, NULL, count ? list : NULL, count, NULL, false);
#endif
	/* the guest's main thread is this one, the main thread: the dialog
	runs on its events */
	while (!__atomic_load_n(&state.done, __ATOMIC_ACQUIRE))
	{
		SDL_PumpEvents();
		SDL_Delay(10);
	}
	copy_to_guest(path, size, state.path);
}

/* ---------- gamepads */

u32 w2c_halo_host_sdl_get_gamepads(struct w2c_halo *instance, u32 ids, u32 capacity)
{
	uint32_t *destination = host_guest_pointer(ids);
	int count = 0, index;
	SDL_JoystickID *list = SDL_GetGamepads(&count);

	(void)instance;
	if (!list)
		return 0;
	if (count > (int)capacity)
		count = (int)capacity;
	if (!destination || count < 0)
		count = 0;
	for (index = 0; index < count; index++)
		destination[index] = list[index];
	SDL_free(list);
	return (u32)count;
}

u32 w2c_halo_host_sdl_open_gamepad(struct w2c_halo *instance, u32 id)
{
	SDL_Gamepad *gamepad = SDL_OpenGamepad((SDL_JoystickID)id);

	(void)instance;
	if (gamepad)
		host_logf("gamepad %u: %s (type %d, %04x:%04x)", (unsigned)id, SDL_GetGamepadName(gamepad),
			(int)SDL_GetGamepadType(gamepad), SDL_GetGamepadVendor(gamepad), SDL_GetGamepadProduct(gamepad));
	return handle_new(_handle_gamepad, gamepad);
}

u32 w2c_halo_host_sdl_gamepad_from_id(struct w2c_halo *instance, u32 id)
{
	(void)instance;
	return handle_new(_handle_gamepad, SDL_GetGamepadFromID((SDL_JoystickID)id));
}

u32 w2c_halo_host_sdl_gamepad_axis(struct w2c_halo *instance, u32 gamepad, u32 axis)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	(void)instance;
	return object ? (u32)(s32)SDL_GetGamepadAxis(object, (SDL_GamepadAxis)(s32)axis) : 0;
}

u32 w2c_halo_host_sdl_gamepad_button(struct w2c_halo *instance, u32 gamepad, u32 button)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	(void)instance;
	return object && SDL_GetGamepadButton(object, (SDL_GamepadButton)(s32)button) ? 1 : 0;
}

u32 w2c_halo_host_sdl_gamepad_type(struct w2c_halo *instance, u32 gamepad)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	(void)instance;
	return (u32)(object ? SDL_GetGamepadType(object) : SDL_GAMEPAD_TYPE_UNKNOWN);
}

u32 w2c_halo_host_sdl_rumble_gamepad(struct w2c_halo *instance, u32 gamepad, u32 low, u32 high,
	u32 milliseconds)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	(void)instance;
	return object && SDL_RumbleGamepad(object, (Uint16)low, (Uint16)high, milliseconds) ? 1 : 0;
}

/* ---------- audio */

/* SDL calls audio_callback on its own audio thread, which cannot run guest
code (it is no guest thread); it passes each request to the guest's thread
for the stream (guest_sdl.c's audio_thread, waiting in host_sdl_audio_wait)
and waits until that says host_sdl_audio_done. SDL holds the stream's lock
throughout, so the audio the guest puts into the stream meanwhile is kept
in the binding's buffer instead, and put in by audio_callback once the guest
is done: the guest's thread putting it in itself would wait for the lock
forever */
struct audio_binding
{
	SDL_AudioStream *stream;
	pthread_mutex_t lock;
	pthread_cond_t requested;
	pthread_cond_t done;
	int pending;
	int additional;
	int total;
	unsigned char *buffer;
	int buffer_length;
	int buffer_size;
};

/* the binding whose callback this (guest) thread is running, if any */
static __thread struct audio_binding *calling_back;

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
	struct audio_binding *binding = userdata;

	pthread_mutex_lock(&binding->lock);
	binding->additional = additional;
	binding->total = total;
	binding->pending = 1;
	pthread_cond_signal(&binding->requested);
	while (binding->pending)
		pthread_cond_wait(&binding->done, &binding->lock);
	pthread_mutex_unlock(&binding->lock);
	if (binding->buffer_length)
	{
		SDL_PutAudioStreamData(stream, binding->buffer, binding->buffer_length);
		binding->buffer_length = 0;
	}
}

/* audio the guest puts into its stream during the stream's callback, for
audio_callback to put in; 1 on success */
static int audio_keep(struct audio_binding *binding, const void *data, int length)
{
	if (length < 0 || (length && !data))
		return 0;
	if (binding->buffer_length + length > binding->buffer_size)
	{
		int size = (binding->buffer_length + length) * 2;
		unsigned char *buffer = SDL_realloc(binding->buffer, (size_t)size);

		if (!buffer)
			return 0;
		binding->buffer = buffer;
		binding->buffer_size = size;
	}
	memcpy(binding->buffer + binding->buffer_length, data, (size_t)length);
	binding->buffer_length += length;
	return 1;
}

u32 w2c_halo_host_sdl_open_audio_stream(struct w2c_halo *instance, u32 device, u32 spec, u32 callback)
{
	/* SDL_AudioSpec (format, channels, freq) has the same layout in both ABIs */
	const SDL_AudioSpec *host_spec = host_guest_pointer(spec);
	struct audio_binding *binding = SDL_calloc(1, sizeof(*binding));
	uint32_t handle;

	(void)instance;
	if (!binding)
		return 0;
	pthread_mutex_init(&binding->lock, NULL);
	pthread_cond_init(&binding->requested, NULL);
	pthread_cond_init(&binding->done, NULL);
	/* the device starts paused, so no callback comes before the guest's
	thread waits for it */
	binding->stream = SDL_OpenAudioDeviceStream((SDL_AudioDeviceID)device, host_spec,
		callback ? audio_callback : NULL, binding);
	if (!binding->stream)
	{
		pthread_cond_destroy(&binding->done);
		pthread_cond_destroy(&binding->requested);
		pthread_mutex_destroy(&binding->lock);
		SDL_free(binding);
		return 0;
	}
	handle = handle_new(_handle_audio, binding);
	if (!handle)
	{
		SDL_DestroyAudioStream(binding->stream);
		SDL_free(binding);
	}
	return handle;
}

u32 w2c_halo_host_sdl_audio_wait(struct w2c_halo *instance, u32 stream, u32 additional, u32 total)
{
	struct audio_binding *binding = handle_get(stream, _handle_audio);

	(void)instance;
	if (!binding)
		return 0;
	pthread_mutex_lock(&binding->lock);
	while (!binding->pending)
		pthread_cond_wait(&binding->requested, &binding->lock);
	store_guest_int(additional, binding->additional);
	store_guest_int(total, binding->total);
	pthread_mutex_unlock(&binding->lock);
	calling_back = binding;
	return 1;
}

void w2c_halo_host_sdl_audio_done(struct w2c_halo *instance, u32 stream)
{
	struct audio_binding *binding = handle_get(stream, _handle_audio);

	(void)instance;
	if (!binding)
		return;
	calling_back = NULL;
	pthread_mutex_lock(&binding->lock);
	binding->pending = 0;
	pthread_cond_signal(&binding->done);
	pthread_mutex_unlock(&binding->lock);
}

u32 w2c_halo_host_sdl_put_audio_stream_data(struct w2c_halo *instance, u32 stream, u32 data, u32 length)
{
	struct audio_binding *binding = handle_get(stream, _handle_audio);

	(void)instance;
	if (!binding)
		return 0;
	if (calling_back == binding)
		return (u32)audio_keep(binding, host_guest_pointer(data), (int)length);
	return SDL_PutAudioStreamData(binding->stream, host_guest_pointer(data), (int)length) ? 1 : 0;
}

u32 w2c_halo_host_sdl_resume_audio_stream_device(struct w2c_halo *instance, u32 stream)
{
	struct audio_binding *binding = handle_get(stream, _handle_audio);

	(void)instance;
	return binding && SDL_ResumeAudioStreamDevice(binding->stream) ? 1 : 0;
}
