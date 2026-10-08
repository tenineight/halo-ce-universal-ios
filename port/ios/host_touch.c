/*
HOST_TOUCH.C

The iOS port's on-screen controls (host_touch.h).

The controls are an SDL virtual gamepad, which the game takes as the first
controller (port/linux/src/xinput_sdl.c): a stick to move, one to look, the face
buttons, the triggers and the rest of the Xbox controller the game uses
(source/input/input_abstraction.c's defaults: white is the flashlight,
black switches grenades). While a real controller is connected they are
put away, so that it is the first.

The right stick aims as a drag off the controls does (the mouse); touched
again straight after it was let go it also fires (the right trigger) until
let go, so that one thumb aims and fires.
A touch anywhere else is the mouse: a drag aims, as the mouse does
(halo_linux_mouse_look), and in the menus a tap clicks, which their pointer
takes (in the game a click would fire: that is the look stick's).

In landscape they are drawn over the game; in portrait the game's picture
moves to the top of the screen (touch_controls_place_picture, on the blit
that shows it) and they are drawn in the space below. They are drawn with
OpenGL ES straight before the frame is shown; the game takes up its own
state again after that (d3d8_gl.c invalidates its cache), and what this
changes is restored besides. A tap with four fingers hides them, or shows
them again; hidden, they still work.
*/

#define GLES_SILENCE_DEPRECATION
#include "host.h"
#include "host_import.h"
#include "host_picker.h"
#include "host_touch.h"

#include <OpenGLES/ES3/gl.h>
#include <TargetConditionals.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------- the controls */

enum control_kind
{
	_control_stick,
	_control_button,
	_control_trigger,
	/* opens and closes the settings (touch sensitivity) */
	_control_menu,
};

struct control
{
	int kind;
	int target; /* the SDL gamepad button or axis */
	const char *label;
	float color[3];
	/* where it is, in the window's pixels from the top left */
	float x, y, radius;
	SDL_FingerID finger;
	int held;
	/* a stick's deflection, -1 to 1 */
	float dx, dy;
	/* a stick's centre while held: the left stick's own, the look
	stick's where its finger landed (so that touching it does not turn) */
	float origin_x, origin_y;
	/* where the look stick's finger was last: it aims as the finger moves */
	float last_x, last_y;
};

static struct control controls[] = {
	/* (a stick's target: 0 the left stick, 1 the right, which looks) */
	{ _control_stick, 0, NULL, { 0.8f, 0.85f, 0.9f } },
	{ _control_stick, 1, NULL, { 0.8f, 0.85f, 0.9f } },
	{ _control_trigger, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, "GRENADE", { 0.95f, 0.65f, 0.25f } },
	{ _control_button, SDL_GAMEPAD_BUTTON_SOUTH, "JUMP", { 0.3f, 0.8f, 0.3f } },
	{ _control_button, SDL_GAMEPAD_BUTTON_EAST, "MELEE", { 0.9f, 0.25f, 0.25f } },
	{ _control_button, SDL_GAMEPAD_BUTTON_WEST, "ACTION", { 0.25f, 0.5f, 0.95f } },
	{ _control_button, SDL_GAMEPAD_BUTTON_NORTH, "WEAPON", { 0.95f, 0.85f, 0.2f } },
	{ _control_button, SDL_GAMEPAD_BUTTON_LEFT_STICK, "CROUCH", { 0.7f, 0.7f, 0.75f } },
	{ _control_button, SDL_GAMEPAD_BUTTON_RIGHT_STICK, "ZOOM", { 0.7f, 0.7f, 0.75f } },
	{ _control_button, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "LIGHT", { 0.9f, 0.9f, 0.9f } },
	{ _control_button, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "NADES", { 0.45f, 0.45f, 0.5f } },
	{ _control_button, SDL_GAMEPAD_BUTTON_BACK, "BACK", { 0.7f, 0.7f, 0.75f } },
	{ _control_button, SDL_GAMEPAD_BUTTON_START, "START", { 0.7f, 0.7f, 0.75f } },
	{ _control_menu, 0, "SETUP", { 0.55f, 0.55f, 0.6f } },
};

#define CONTROL_COUNT ((int)(sizeof(controls) / sizeof(controls[0])))

enum
{
	_stick,
	_look,
	_grenade,
	_jump,
	_melee,
	_action,
	_weapon,
	_crouch,
	_zoom,
	_light,
	_nades,
	_back,
	_start,
	_setup,
};

static SDL_Window *window;
static unsigned int window_framebuffer;
static SDL_Joystick *virtual_joystick;
static SDL_JoystickID virtual_id;
/* the real controllers connected: the controls are away while there are */
static int external_gamepads;

/* the game's picture, in the window's pixels from the top left, and how
far up it was moved (touch_controls_place_picture) */
static int picture_known;
static float picture_top, picture_bottom, picture_shift;

/* the look stick fires while held when touched again straight after it
was let go (LOOK_FIRE_MILLISECONDS) */
#define LOOK_FIRE_MILLISECONDS 300
static uint64_t look_released_ticks;
static int look_firing;

/* four fingers down together (within TOGGLE_MILLISECONDS of the first)
hide the controls, or show them again: hidden, they still work. The
fingers of that tap do nothing else until they are all lifted */
#define TOGGLE_FINGERS 4
#define TOGGLE_MILLISECONDS 300
static int controls_hidden;
static int fingers_down;
static uint64_t first_finger_ticks;
static int toggle_swallowing;

/* the sensitivities the settings set (touch_controls.txt in the game's
folder): the left stick's deflection for a finger's travel, and the aim
for a finger's motion (the look stick's and a drag's alike) */
#define MOVE_SENSITIVITY_MINIMUM 0.25f
#define MOVE_SENSITIVITY_MAXIMUM 4.0f
#define LOOK_SENSITIVITY_MINIMUM 0.1f
#define LOOK_SENSITIVITY_MAXIMUM 10.0f
static float move_sensitivity = 1.0f, look_sensitivity = 1.0f;

/* the settings, while open: every touch is theirs */
enum
{
	_slider_move,
	_slider_look,
	SLIDER_COUNT
};

static struct
{
	int open;
	/* the slider a finger drags, or NONE (-1) */
	int slider;
	SDL_FingerID finger;
} menu = { 0, -1, 0 };

/* the game is being played, not its menus (touch_controls_set_playing) */
static int playing;

/* the touch that is the mouse */
static struct
{
	int active;
	SDL_FingerID finger;
	float start_x, start_y, x, y;
	uint64_t start_ticks;
	float travelled;
} pointer;

static void window_pixels(int *width, int *height)
{
	*width = *height = 0;
	if (window)
		SDL_GetWindowSizeInPixels(window, width, height);
}

static void place(int index, float x, float y, float radius)
{
	controls[index].x = x;
	controls[index].y = y;
	controls[index].radius = radius;
}

/* a control on a circle around (x, y), at an angle clockwise from the
right */
static void place_around(int index, float x, float y, float distance, float degrees, float radius)
{
	float angle = degrees * 3.14159265f / 180.0f;

	place(index, x + distance * cosf(angle), y + distance * sinf(angle), radius);
}

/* where each control is for the window's shape now: the left stick with
CROUCH at its lower right, the look stick with WEAPON, ACTION and MELEE
around its left side, GRENADE above it, ZOOM at its upper right and JUMP
in the corner below */
static void layout(void)
{
	int width, height;
	float w, h, u;

	window_pixels(&width, &height);
	w = (float)width;
	h = (float)height;
	if (width >= height)
	{
		/* landscape: over the game, at the thumbs */
		float stick_x, stick_y, look_x, look_y;

		u = h / 9.0f;
		stick_x = 2.4f * u;
		stick_y = h - 2.4f * u;
		look_x = w - 3.0f * u;
		look_y = h - 2.6f * u;
		place(_stick, stick_x, stick_y, 1.5f * u);
		place_around(_crouch, stick_x, stick_y, 2.35f * u, 45.0f, 0.5f * u);
		place(_look, look_x, look_y, 1.5f * u);
		place_around(_weapon, look_x, look_y, 2.35f * u, 220.0f, 0.5f * u);
		place_around(_action, look_x, look_y, 2.35f * u, 180.0f, 0.5f * u);
		place_around(_melee, look_x, look_y, 2.35f * u, 140.0f, 0.55f * u);
		place(_grenade, look_x, h - 5.0f * u, 0.55f * u);
		place(_jump, w - 0.9f * u, h - 0.9f * u, 0.6f * u);
		place(_zoom, w - 1.0f * u, h - 4.6f * u, 0.5f * u);
		place(_back, 0.9f * u, 0.8f * u, 0.45f * u);
		place(_start, 2.1f * u, 0.8f * u, 0.45f * u);
		place(_light, w - 2.1f * u, 0.8f * u, 0.45f * u);
		place(_nades, w - 0.9f * u, 0.8f * u, 0.45f * u);
		place(_setup, w * 0.5f, 0.7f * u, 0.42f * u);
	}
	else
	{
		/* portrait: below the game's picture */
		float top = picture_known ? picture_bottom : h * 0.45f;
		float area = h - top;
		float middle, stick_x, look_x, look_y;

		u = w / 9.0f < area / 6.5f ? w / 9.0f : area / 6.5f;
		middle = top + area * 0.55f;
		/* (the sticks smaller than in landscape, for the look stick's
		arc to fit between them) */
		stick_x = 1.9f * u;
		look_x = w - 2.0f * u;
		look_y = middle + 0.2f * u;
		place(_stick, stick_x, middle, 1.15f * u);
		place_around(_crouch, stick_x, middle, 1.85f * u, 50.0f, 0.5f * u);
		place(_look, look_x, look_y, 1.15f * u);
		place_around(_weapon, look_x, look_y, 1.85f * u, 220.0f, 0.5f * u);
		place_around(_action, look_x, look_y, 1.85f * u, 180.0f, 0.5f * u);
		place_around(_melee, look_x, look_y, 1.85f * u, 140.0f, 0.55f * u);
		place(_grenade, look_x - 0.5f * u, middle - 2.1f * u, 0.55f * u);
		place(_jump, w - 0.85f * u, top + area - 0.85f * u, 0.6f * u);
		place(_zoom, w - 0.75f * u, middle - 1.6f * u, 0.5f * u);
		place(_back, 0.8f * u, top + 0.7f * u, 0.45f * u);
		place(_start, 2.0f * u, top + 0.7f * u, 0.45f * u);
		place(_light, 3.2f * u, top + 0.7f * u, 0.45f * u);
		place(_nades, 4.4f * u, top + 0.7f * u, 0.45f * u);
		/* (over the picture's corner) */
		place(_setup, w - 0.7f * u, 0.7f * u, 0.42f * u);
	}
}

/* ---------- the virtual gamepad */

static void gamepad_attach(void)
{
	SDL_VirtualJoystickDesc description;

	if (virtual_joystick)
		return;
	SDL_INIT_INTERFACE(&description);
	description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
	description.naxes = SDL_GAMEPAD_AXIS_COUNT;
	description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
	/* (every button and axis, so that each one's index is SDL's own) */
	description.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
	description.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
	description.name = "Touch controls";
	virtual_id = SDL_AttachVirtualJoystick(&description);
	virtual_joystick = virtual_id ? SDL_OpenJoystick(virtual_id) : NULL;
	if (!virtual_joystick)
		host_logf("touch controls: no virtual gamepad (%s)", SDL_GetError());
}

static void gamepad_detach(void)
{
	int index;

	if (!virtual_joystick)
		return;
	SDL_CloseJoystick(virtual_joystick);
	SDL_DetachVirtualJoystick(virtual_id);
	virtual_joystick = NULL;
	for (index = 0; index < CONTROL_COUNT; index++)
	{
		controls[index].held = 0;
		controls[index].dx = controls[index].dy = 0.0f;
	}
	look_firing = 0;
}

static void control_apply(const struct control *control)
{
	if (!virtual_joystick)
		return;
	switch (control->kind)
	{
	case _control_stick:
		/* (the look stick aims as the mouse does, finger_motion: its firing
		is the right trigger) */
		if (control->target)
		{
			SDL_SetJoystickVirtualAxis(virtual_joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
				look_firing ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
			break;
		}
		SDL_SetJoystickVirtualAxis(virtual_joystick, SDL_GAMEPAD_AXIS_LEFTX, (Sint16)(control->dx * 32767.0f));
		SDL_SetJoystickVirtualAxis(virtual_joystick, SDL_GAMEPAD_AXIS_LEFTY, (Sint16)(control->dy * 32767.0f));
		break;
	case _control_button:
		SDL_SetJoystickVirtualButton(virtual_joystick, control->target, control->held != 0);
		break;
	case _control_trigger:
		/* (SDL takes a virtual trigger's whole range, from released at the
		minimum) */
		SDL_SetJoystickVirtualAxis(virtual_joystick, control->target,
			control->held ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
		break;
	}
}

static void stick_move(struct control *control, float x, float y)
{
	/* (the left stick's sensitivity: its full deflection for less, or
	more, of the finger's travel) */
	float scale = control->target ? 1.0f : move_sensitivity;
	float dx = (x - control->origin_x) / control->radius * scale;
	float dy = (y - control->origin_y) / control->radius * scale;
	float length = sqrtf(dx * dx + dy * dy);

	if (length > 1.0f)
	{
		dx /= length;
		dy /= length;
	}
	control->dx = dx;
	control->dy = dy;
}

/* ---------- the mouse */

/* a point in the window's pixels as the game has it: in points, and where
it would be had its picture not moved */
static void game_point(float x, float y, float *game_x, float *game_y)
{
	int pixel_width, pixel_height, width = 0, height = 0;
	float density;

	window_pixels(&pixel_width, &pixel_height);
	SDL_GetWindowSize(window, &width, &height);
	density = width > 0 ? (float)pixel_width / (float)width : 1.0f;
	*game_x = x / density;
	*game_y = (y + picture_shift) / density;
}

static void mouse_motion(float x, float y, float dx, float dy)
{
	SDL_Event event;
	int pixel_width, pixel_height, width = 0, height = 0;
	float density;

	window_pixels(&pixel_width, &pixel_height);
	SDL_GetWindowSize(window, &width, &height);
	density = width > 0 ? (float)pixel_width / (float)width : 1.0f;
	SDL_zero(event);
	event.type = SDL_EVENT_MOUSE_MOTION;
	event.motion.timestamp = SDL_GetTicksNS();
	event.motion.windowID = SDL_GetWindowID(window);
	game_point(x, y, &event.motion.x, &event.motion.y);
	event.motion.xrel = dx / density;
	event.motion.yrel = dy / density;
	SDL_PushEvent(&event);
}

static void mouse_click(float x, float y)
{
	SDL_Event event;
	int down;

	for (down = 1; down >= 0; down--)
	{
		SDL_zero(event);
		event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		event.button.timestamp = SDL_GetTicksNS();
		event.button.windowID = SDL_GetWindowID(window);
		event.button.button = SDL_BUTTON_LEFT;
		event.button.down = down != 0;
		event.button.clicks = 1;
		game_point(x, y, &event.button.x, &event.button.y);
		SDL_PushEvent(&event);
	}
}

/* ---------- the settings */

static void settings_path(char *path, size_t size)
{
	snprintf(path, size, "%stouch_controls.txt", host_base_path());
}

static float clamp(float value, float minimum, float maximum)
{
	return value < minimum ? minimum : value > maximum ? maximum : value;
}

static void settings_load(void)
{
	char path[1024], line[128];
	FILE *file;

	settings_path(path, sizeof(path));
	file = fopen(path, "r");
	if (!file)
		return;
	while (fgets(line, sizeof(line), file))
	{
		float value;

		if (sscanf(line, " move = %f", &value) == 1)
			move_sensitivity = clamp(value, MOVE_SENSITIVITY_MINIMUM, MOVE_SENSITIVITY_MAXIMUM);
		else if (sscanf(line, " look = %f", &value) == 1)
			look_sensitivity = clamp(value, LOOK_SENSITIVITY_MINIMUM, LOOK_SENSITIVITY_MAXIMUM);
	}
	fclose(file);
}

static void settings_save(void)
{
	char path[1024];
	FILE *file;

	settings_path(path, sizeof(path));
	file = fopen(path, "w");
	if (!file)
	{
		host_logf("touch controls: cannot save %s", path);
		return;
	}
	fprintf(file, "# the on-screen controls' settings (their SETUP button)\n"
		"# the left stick's sensitivity, %.2f to %.2f\nmove = %.2f\n"
		"# aiming's sensitivity (the right stick, and dragging), %.2f to %.2f\nlook = %.2f\n",
		MOVE_SENSITIVITY_MINIMUM, MOVE_SENSITIVITY_MAXIMUM, move_sensitivity,
		LOOK_SENSITIVITY_MINIMUM, LOOK_SENSITIVITY_MAXIMUM, look_sensitivity);
	fclose(file);
}

/* a slider's position, 0 to 1, for its value: on a logarithmic scale (each
step a like change, from a quarter to four times for moving, a tenth to ten
times for aiming) */
static float slider_position(int slider)
{
	if (slider == _slider_move)
	{
		return logf(move_sensitivity / MOVE_SENSITIVITY_MINIMUM) /
			logf(MOVE_SENSITIVITY_MAXIMUM / MOVE_SENSITIVITY_MINIMUM);
	}
	return logf(look_sensitivity / LOOK_SENSITIVITY_MINIMUM) /
		logf(LOOK_SENSITIVITY_MAXIMUM / LOOK_SENSITIVITY_MINIMUM);
}

static void slider_set(int slider, float position)
{
	position = clamp(position, 0.0f, 1.0f);
	/* (to a hundredth, as shown) */
	if (slider == _slider_move)
	{
		move_sensitivity = MOVE_SENSITIVITY_MINIMUM *
			powf(MOVE_SENSITIVITY_MAXIMUM / MOVE_SENSITIVITY_MINIMUM, position);
		move_sensitivity = roundf(move_sensitivity * 100.0f) / 100.0f;
	}
	else
	{
		look_sensitivity = LOOK_SENSITIVITY_MINIMUM *
			powf(LOOK_SENSITIVITY_MAXIMUM / LOOK_SENSITIVITY_MINIMUM, position);
		look_sensitivity = roundf(look_sensitivity * 100.0f) / 100.0f;
	}
}

/* where the settings' panel and its parts are, in the window's pixels */
struct menu_geometry
{
	float unit;
	float left, top, right, bottom;
	float title_y;
	float row_y[SLIDER_COUNT];
	float label_x, track_left, track_right, value_x;
	float done_x, done_y, done_width, done_height;
	float import_x, import_y, import_width, import_height;
	float status_y;
};

static void menu_layout(struct menu_geometry *geometry)
{
	int width, height;
	float w, h, u, panel_width, panel_height;

	window_pixels(&width, &height);
	w = (float)width;
	h = (float)height;
	u = (w < h ? w : h) / 9.0f;
	panel_width = w * 0.9f < 11.0f * u ? w * 0.9f : 11.0f * u;
	panel_height = 6.4f * u;
	geometry->unit = u;
	geometry->left = (w - panel_width) / 2.0f;
	geometry->right = geometry->left + panel_width;
	geometry->top = (h - panel_height) / 2.0f;
	geometry->bottom = geometry->top + panel_height;
	geometry->title_y = geometry->top + 0.75f * u;
	geometry->row_y[_slider_move] = geometry->top + 2.0f * u;
	geometry->row_y[_slider_look] = geometry->top + 3.3f * u;
	geometry->label_x = geometry->left + 1.3f * u;
	geometry->track_left = geometry->left + 2.6f * u;
	geometry->track_right = geometry->right - 2.3f * u;
	geometry->value_x = geometry->right - 1.2f * u;
	/* IMPORT MAPS and DONE side by side, the import's progress below */
	geometry->import_width = 4.0f * u;
	geometry->import_height = 0.85f * u;
	geometry->done_width = 2.6f * u;
	geometry->done_height = 0.85f * u;
	{
		float gap = 0.6f * u;
		float start = (geometry->left + geometry->right) / 2.0f -
			(geometry->import_width + gap + geometry->done_width) / 2.0f;

		geometry->import_x = start + geometry->import_width / 2.0f;
		geometry->done_x = start + geometry->import_width + gap + geometry->done_width / 2.0f;
	}
	geometry->import_y = geometry->done_y = geometry->top + 4.55f * u;
	geometry->status_y = geometry->top + 5.65f * u;
}

/* lets go of whatever the controls hold, as the settings open */
static void controls_release(void)
{
	int index;

	for (index = 0; index < CONTROL_COUNT; index++)
	{
		if (controls[index].held)
		{
			controls[index].held = 0;
			controls[index].dx = controls[index].dy = 0.0f;
			control_apply(&controls[index]);
		}
	}
	look_firing = 0;
	control_apply(&controls[_look]);
	pointer.active = 0;
}

static void menu_open(void)
{
	controls_release();
	menu.open = 1;
	menu.slider = -1;
}

static void menu_close(void)
{
	menu.open = 0;
	menu.slider = -1;
	settings_save();
	host_logf("touch controls: move %.2f, look %.2f", move_sensitivity, look_sensitivity);
}

static void menu_slide(float x)
{
	struct menu_geometry geometry;

	menu_layout(&geometry);
	slider_set(menu.slider, (x - geometry.track_left) / (geometry.track_right - geometry.track_left));
}

/* the disc image picked for importing the maps (ios_pick_file) */
static void SDLCALL import_image_picked(void *userdata, const char *const *files, int filter)
{
	(void)userdata;
	(void)filter;
	if (files && files[0])
		maps_import_start(files[0]);
}

static void menu_down(SDL_FingerID finger, float x, float y)
{
	struct menu_geometry geometry;
	const struct control *setup = &controls[_setup];
	int slider;

	menu_layout(&geometry);
	layout();
	/* DONE, or SETUP again, closes it */
	if ((fabsf(x - geometry.done_x) <= geometry.done_width / 2.0f &&
		fabsf(y - geometry.done_y) <= geometry.done_height / 2.0f) ||
		hypotf(x - setup->x, y - setup->y) <= setup->radius * 1.2f)
	{
		menu_close();
		return;
	}
	/* IMPORT MAPS: a disc image to import them from */
	if (fabsf(x - geometry.import_x) <= geometry.import_width / 2.0f &&
		fabsf(y - geometry.import_y) <= geometry.import_height / 2.0f)
	{
		static const SDL_DialogFileFilter filters[] = {
			{ "Xbox disc images", "iso;xiso" },
			{ "All files", "*" },
		};
		char status[256];
		float fraction;

		if (maps_import_status(status, sizeof(status), &fraction) != _maps_import_running)
			ios_pick_file(window, filters, 2, import_image_picked, NULL);
		return;
	}
	if (menu.slider >= 0)
		return;
	for (slider = 0; slider < SLIDER_COUNT; slider++)
	{
		if (fabsf(y - geometry.row_y[slider]) <= 0.55f * geometry.unit &&
			x >= geometry.track_left - 0.4f * geometry.unit && x <= geometry.track_right + 0.4f * geometry.unit)
		{
			menu.slider = slider;
			menu.finger = finger;
			menu_slide(x);
			return;
		}
	}
}

/* ---------- touches */

static int controls_shown(void)
{
	return virtual_joystick != NULL && external_gamepads == 0;
}

static struct control *control_at(float x, float y)
{
	struct control *best = NULL;
	float best_distance = 0.0f;
	int index;

	if (!controls_shown())
		return NULL;
	layout();
	for (index = 0; index < CONTROL_COUNT; index++)
	{
		struct control *control = &controls[index];
		float reach = control->radius * (control->kind == _control_stick ? 1.4f : 1.2f);
		float dx = x - control->x, dy = y - control->y;
		float distance = sqrtf(dx * dx + dy * dy);

		if (!control->held && distance <= reach && (!best || distance < best_distance))
		{
			best = control;
			best_distance = distance;
		}
	}
	return best;
}

static struct control *control_of(SDL_FingerID finger)
{
	int index;

	for (index = 0; index < CONTROL_COUNT; index++)
	{
		if (controls[index].held && controls[index].finger == finger)
			return &controls[index];
	}
	return NULL;
}

static void finger_down(SDL_FingerID finger, float x, float y)
{
	struct control *control;

	if (menu.open)
	{
		menu_down(finger, x, y);
		return;
	}
	control = control_at(x, y);
	if (control && control->kind == _control_menu)
	{
		menu_open();
		return;
	}
	if (control)
	{
		control->held = 1;
		control->finger = finger;
		if (control->kind == _control_stick)
		{
			control->origin_x = control->target ? x : control->x;
			control->origin_y = control->target ? y : control->y;
			control->last_x = x;
			control->last_y = y;
			/* the look stick touched again straight after it was let go:
			it fires while held */
			if (control->target)
			{
				look_firing = look_released_ticks &&
					SDL_GetTicks() - look_released_ticks < LOOK_FIRE_MILLISECONDS;
			}
			stick_move(control, x, y);
		}
		control_apply(control);
		return;
	}
	if (pointer.active)
		return;
	pointer.active = 1;
	pointer.finger = finger;
	pointer.start_x = pointer.x = x;
	pointer.start_y = pointer.y = y;
	pointer.start_ticks = SDL_GetTicks();
	pointer.travelled = 0.0f;
	/* (the menus' pointer goes where the finger is) */
	mouse_motion(x, y, 0.0f, 0.0f);
}

static void finger_motion(SDL_FingerID finger, float x, float y)
{
	struct control *control;

	if (menu.open)
	{
		if (menu.slider >= 0 && menu.finger == finger)
			menu_slide(x);
		return;
	}
	control = control_of(finger);

	if (control)
	{
		if (control->kind == _control_stick)
		{
			stick_move(control, x, y);
			/* the look stick aims as a drag off the controls does */
			if (control->target)
			{
				mouse_motion(x, y, (x - control->last_x) * look_sensitivity,
					(y - control->last_y) * look_sensitivity);
			}
			else
				control_apply(control);
			control->last_x = x;
			control->last_y = y;
		}
		return;
	}
	if (pointer.active && pointer.finger == finger)
	{
		float dx = x - pointer.x, dy = y - pointer.y;

		pointer.travelled += sqrtf(dx * dx + dy * dy);
		pointer.x = x;
		pointer.y = y;
		mouse_motion(x, y, dx * look_sensitivity, dy * look_sensitivity);
	}
}

static void finger_up(SDL_FingerID finger, float x, float y, int canceled)
{
	struct control *control;

	if (menu.open)
	{
		if (menu.slider >= 0 && menu.finger == finger)
			menu.slider = -1;
		return;
	}
	control = control_of(finger);

	if (control)
	{
		control->held = 0;
		control->dx = control->dy = 0.0f;
		if (control->kind == _control_stick && control->target)
		{
			look_firing = 0;
			look_released_ticks = SDL_GetTicks();
		}
		control_apply(control);
		return;
	}
	if (pointer.active && pointer.finger == finger)
	{
		int pixel_width, pixel_height;

		pointer.active = 0;
		window_pixels(&pixel_width, &pixel_height);
		/* a tap in the menus: short, and staying put (within a fiftieth of
		the window) */
		if (!canceled && !playing && SDL_GetTicks() - pointer.start_ticks < 300 &&
			pointer.travelled < (float)(pixel_width < pixel_height ? pixel_width : pixel_height) / 50.0f)
		{
			mouse_click(x, y);
		}
	}
}

int touch_controls_event(const SDL_Event *event)
{
	int width, height;

	switch (event->type)
	{
	case SDL_EVENT_FINGER_DOWN:
	case SDL_EVENT_FINGER_MOTION:
	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
	{
		float x, y;

		if (!window)
			return 1;
		window_pixels(&width, &height);
		x = event->tfinger.x * (float)width;
		y = event->tfinger.y * (float)height;
		if (event->type == SDL_EVENT_FINGER_DOWN)
		{
			if (fingers_down++ == 0)
				first_finger_ticks = SDL_GetTicks();
			if (!toggle_swallowing && !menu.open && controls_shown() && fingers_down == TOGGLE_FINGERS &&
				SDL_GetTicks() - first_finger_ticks < TOGGLE_MILLISECONDS)
			{
				/* (what the tap's first fingers pressed is let go) */
				controls_release();
				controls_hidden = !controls_hidden;
				toggle_swallowing = 1;
				host_logf("touch controls: %s", controls_hidden ? "hidden" : "shown");
			}
		}
		else if (event->type != SDL_EVENT_FINGER_MOTION)
		{
			int still_down = 0;

			/* (SDL's own count, in case a lift was never seen) */
			SDL_free(SDL_GetTouchFingers(event->tfinger.touchID, &still_down));
			if (fingers_down > 0)
				fingers_down--;
			if (still_down == 0)
				fingers_down = 0;
			if (fingers_down == 0 && toggle_swallowing)
			{
				toggle_swallowing = 0;
				return 1;
			}
		}
		if (toggle_swallowing)
			return 1;
		if (event->type == SDL_EVENT_FINGER_DOWN)
			finger_down(event->tfinger.fingerID, x, y);
		else if (event->type == SDL_EVENT_FINGER_MOTION)
			finger_motion(event->tfinger.fingerID, x, y);
		else
			finger_up(event->tfinger.fingerID, x, y, event->type == SDL_EVENT_FINGER_CANCELED);
		return 1;
	}
	/* the mouse SDL would make of the touches: theirs is ours */
	case SDL_EVENT_MOUSE_MOTION:
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
	{
		static int logged;
		int touch = (event->type == SDL_EVENT_MOUSE_MOTION ? event->motion.which : event->button.which) ==
			SDL_TOUCH_MOUSEID;

		if (touch && !logged++)
			host_logf("touch controls: dropping the mouse SDL makes of the touches");
		return touch;
	}
	/* a real controller: the controls make way, so that it is the first */
	case SDL_EVENT_GAMEPAD_ADDED:
#if TARGET_OS_SIMULATOR
		/* (the simulator has a controller of its own, always there) */
		return 0;
#endif
		if (event->gdevice.which != virtual_id && SDL_IsGamepad(event->gdevice.which))
		{
			if (external_gamepads++ == 0)
			{
				host_logf("touch controls: a controller is connected, putting them away");
				gamepad_detach();
			}
		}
		return 0;
	case SDL_EVENT_GAMEPAD_REMOVED:
		if (event->gdevice.which != virtual_id && external_gamepads > 0 && --external_gamepads == 0)
		{
			host_logf("touch controls: back");
			gamepad_attach();
		}
		return 0;
	default:
		return 0;
	}
}

void touch_controls_set_playing(int game_playing)
{
	playing = game_playing;
}

/* ---------- the picture */

void touch_controls_place_picture(int *x0, int *y0, int *x1, int *y1)
{
	int width, height, top, bottom;

	(void)x0;
	(void)x1;
	window_pixels(&width, &height);
	if (!width || !height)
		return;
	top = *y0 > *y1 ? *y0 : *y1;
	bottom = *y0 > *y1 ? *y1 : *y0;
	picture_shift = 0.0f;
	if (height > width)
	{
		/* portrait: up to the top, the controls below */
		int shift = height - top;

		*y0 += shift;
		*y1 += shift;
		top += shift;
		bottom += shift;
		picture_shift = (float)shift;
	}
	picture_top = (float)(height - top);
	picture_bottom = (float)(height - bottom);
	picture_known = 1;
}

/* ---------- drawing */

struct vertex
{
	float x, y;
	/* where in its shape, -1 to 1 */
	float u, v;
	float color[4];
	/* 0 a square, 1 a disc, 2 a ring */
	float shape;
};

#define VERTEX_CAPACITY (6 * 4096)

static GLuint program, vertex_array, vertex_buffer;
static struct vertex vertices[VERTEX_CAPACITY];
static int vertex_count;
static float draw_width, draw_height;

static const char vertex_shader[] =
	"#version 300 es\n"
	"layout(location = 0) in vec2 position;\n"
	"layout(location = 1) in vec2 local;\n"
	"layout(location = 2) in vec4 color;\n"
	"layout(location = 3) in float shape;\n"
	"out vec2 v_local;\n"
	"out vec4 v_color;\n"
	"out float v_shape;\n"
	"void main()\n"
	"{\n"
	"\tv_local = local;\n"
	"\tv_color = color;\n"
	"\tv_shape = shape;\n"
	"\tgl_Position = vec4(position, 0.0, 1.0);\n"
	"}\n";

static const char fragment_shader[] =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec2 v_local;\n"
	"in vec4 v_color;\n"
	"in float v_shape;\n"
	"out vec4 fragment_color;\n"
	"void main()\n"
	"{\n"
	"\tfloat alpha = v_color.a;\n"
	"\tif (v_shape > 0.5)\n"
	"\t{\n"
	"\t\tfloat d = length(v_local);\n"
	"\t\tfloat edge = fwidth(d);\n"
	"\t\talpha *= 1.0 - smoothstep(1.0 - edge, 1.0, d);\n"
	"\t\tif (v_shape > 1.5)\n"
	"\t\t\talpha *= smoothstep(0.88 - edge, 0.88, d);\n"
	"\t}\n"
	"\tfragment_color = vec4(v_color.rgb * alpha, alpha);\n"
	"}\n";

/* 5x7 capitals, a row a byte, the leftmost column the highest bit */
static const unsigned char font[26][7] = {
	{ 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 }, { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e },
	{ 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e }, { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e },
	{ 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f }, { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 },
	{ 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f }, { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 },
	{ 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e }, { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c },
	{ 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f },
	{ 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 }, { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 },
	{ 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e }, { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 },
	{ 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d }, { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 },
	{ 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e }, { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
	{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e }, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 },
	{ 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a }, { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 },
	{ 0x11, 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04 }, { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f },
};

/* and the digits, and a full stop */
static const unsigned char digits[10][7] = {
	{ 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e }, { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e },
	{ 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f }, { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e },
	{ 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 }, { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e },
	{ 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e }, { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
	{ 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e }, { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c },
};
static const unsigned char full_stop[7] = { 0, 0, 0, 0, 0, 0x0c, 0x0c };

/* and the punctuation the settings' messages have */
static const struct
{
	char character;
	unsigned char rows[7];
} marks[] = {
	{ ',', { 0, 0, 0, 0, 0x0c, 0x04, 0x08 } },
	{ ':', { 0, 0x0c, 0x0c, 0, 0x0c, 0x0c, 0 } },
	{ '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } },
	{ '(', { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 } },
	{ ')', { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 } },
	{ '-', { 0, 0, 0, 0x1f, 0, 0, 0 } },
	{ '/', { 0, 0x01, 0x02, 0x04, 0x08, 0x10, 0 } },
	{ '\'', { 0x04, 0x04, 0x08, 0, 0, 0, 0 } },
	{ '_', { 0, 0, 0, 0, 0, 0, 0x1f } },
	{ '!', { 0x04, 0x04, 0x04, 0x04, 0x04, 0, 0x04 } },
	{ '?', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0, 0x04 } },
};

/* a character's rows, or NULL for a space (or one there is none of) */
static const unsigned char *glyph(char character)
{
	size_t index;

	/* (in capitals: there are no small letters) */
	character = (char)toupper((unsigned char)character);
	for (index = 0; index < sizeof(marks) / sizeof(marks[0]); index++)
	{
		if (marks[index].character == character)
			return marks[index].rows;
	}
	if (character >= 'A' && character <= 'Z')
		return font[character - 'A'];
	if (character >= '0' && character <= '9')
		return digits[character - '0'];
	if (character == '.')
		return full_stop;
	return NULL;
}

static GLuint shader_compile(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint status = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		char log[1024];

		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		host_logf("touch controls: shader: %s", log);
	}
	return shader;
}

static int draw_initialize(void)
{
	static int tried;
	GLuint vertex, fragment;
	GLint status = 0;

	if (program || tried)
		return program != 0;
	tried = 1;
	vertex = shader_compile(GL_VERTEX_SHADER, vertex_shader);
	fragment = shader_compile(GL_FRAGMENT_SHADER, fragment_shader);
	program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glLinkProgram(program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status)
	{
		host_logf("touch controls: the shaders do not link");
		glDeleteProgram(program);
		program = 0;
		return 0;
	}
	glGenVertexArrays(1, &vertex_array);
	glGenBuffers(1, &vertex_buffer);
	glBindVertexArray(vertex_array);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), NULL, GL_STREAM_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void *)offsetof(struct vertex, x));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void *)offsetof(struct vertex, u));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void *)offsetof(struct vertex, color));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void *)offsetof(struct vertex, shape));
	return 1;
}

/* a rectangle in the window's pixels from the top left */
static void quad(float x0, float y0, float x1, float y1, const float color[4], float shape)
{
	static const float corners[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
	int index;

	if (vertex_count + 6 > VERTEX_CAPACITY)
		return;
	for (index = 0; index < 6; index++)
	{
		struct vertex *vertex = &vertices[vertex_count++];
		float u = corners[index][0], v = corners[index][1];
		float x = u < 0 ? x0 : x1, y = v < 0 ? y0 : y1;

		vertex->x = x / draw_width * 2.0f - 1.0f;
		vertex->y = 1.0f - y / draw_height * 2.0f;
		vertex->u = u;
		vertex->v = v;
		memcpy(vertex->color, color, sizeof(vertex->color));
		vertex->shape = shape;
	}
}

static void disc(float x, float y, float radius, const float color[4], float shape)
{
	quad(x - radius, y - radius, x + radius, y + radius, color, shape);
}

static void text(const char *label, float x, float y, float fit_width, float maximum_pixel)
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 0.9f };
	int length = (int)strlen(label), index, row, column;
	float pixel = fit_width / (float)(length * 6 - 1);
	float left, top;

	if (pixel > maximum_pixel)
		pixel = maximum_pixel;
	pixel = floorf(pixel) > 1.0f ? floorf(pixel) : 1.0f;
	left = floorf(x - pixel * (float)(length * 6 - 1) / 2.0f);
	top = floorf(y - pixel * 3.5f);
	for (index = 0; index < length; index++)
	{
		const unsigned char *rows = glyph(label[index]);

		if (!rows)
			continue;
		for (row = 0; row < 7; row++)
		{
			for (column = 0; column < 5; column++)
			{
				if (rows[row] & (0x10 >> column))
				{
					float cell_x = left + (float)(index * 6 + column) * pixel;
					float cell_y = top + (float)row * pixel;

					quad(cell_x, cell_y, cell_x + pixel, cell_y + pixel, white, 0.0f);
				}
			}
		}
	}
}

static void controls_build(int portrait)
{
	int index;
	/* (over the game, fainter) */
	float fill = portrait ? 0.35f : 0.18f, held_fill = portrait ? 0.7f : 0.5f;
	float outline = portrait ? 0.8f : 0.5f;

	layout();
	for (index = 0; index < CONTROL_COUNT; index++)
	{
		const struct control *control = &controls[index];
		float color[4] = { control->color[0], control->color[1], control->color[2], 0.0f };

		if (control->kind == _control_stick)
		{
			/* (the look stick red while it fires) */
			if (control->target && look_firing)
			{
				color[0] = 0.95f;
				color[1] = 0.3f;
				color[2] = 0.25f;
			}
			color[3] = fill * 0.6f;
			disc(control->x, control->y, control->radius, color, 1.0f);
			color[3] = outline;
			disc(control->x, control->y, control->radius, color, 2.0f);
			color[3] = control->held ? held_fill : fill * 1.5f;
			disc(control->x + control->dx * control->radius, control->y + control->dy * control->radius,
				control->radius * 0.45f, color, 1.0f);
			continue;
		}
		color[3] = control->held ? held_fill : fill;
		disc(control->x, control->y, control->radius, color, 1.0f);
		color[3] = outline;
		disc(control->x, control->y, control->radius, color, 2.0f);
		text(control->label, control->x, control->y, control->radius * 1.5f, control->radius * 0.09f);
	}
}

/* the settings' panel, over the controls */
static void menu_build(void)
{
	static const float panel[4] = { 0.06f, 0.07f, 0.09f, 0.92f };
	static const float track[4] = { 0.35f, 0.37f, 0.42f, 1.0f };
	static const float filled[4] = { 0.35f, 0.6f, 0.95f, 1.0f };
	static const float knob[4] = { 0.9f, 0.92f, 0.95f, 1.0f };
	static const float button[4] = { 0.25f, 0.5f, 0.95f, 0.9f };
	static const char *const labels[SLIDER_COUNT] = { "MOVE", "LOOK" };
	struct menu_geometry geometry;
	float u;
	int slider;

	menu_layout(&geometry);
	u = geometry.unit;
	quad(geometry.left, geometry.top, geometry.right, geometry.bottom, panel, 0.0f);
	text("TOUCH CONTROLS", (geometry.left + geometry.right) / 2.0f, geometry.title_y,
		geometry.right - geometry.left - 2.0f * u, u * 0.07f);
	for (slider = 0; slider < SLIDER_COUNT; slider++)
	{
		float y = geometry.row_y[slider];
		float x = geometry.track_left + slider_position(slider) * (geometry.track_right - geometry.track_left);
		float value = slider == _slider_move ? move_sensitivity : look_sensitivity;
		char shown[16];

		text(labels[slider], geometry.label_x, y, 2.0f * u, u * 0.06f);
		quad(geometry.track_left, y - 0.06f * u, geometry.track_right, y + 0.06f * u, track, 0.0f);
		quad(geometry.track_left, y - 0.06f * u, x, y + 0.06f * u, filled, 0.0f);
		disc(x, y, (menu.slider == slider ? 0.38f : 0.32f) * u, knob, 1.0f);
		snprintf(shown, sizeof(shown), "%.2fX", value);
		text(shown, geometry.value_x, y, 1.8f * u, u * 0.06f);
	}
	quad(geometry.done_x - geometry.done_width / 2.0f, geometry.done_y - geometry.done_height / 2.0f,
		geometry.done_x + geometry.done_width / 2.0f, geometry.done_y + geometry.done_height / 2.0f, button, 0.0f);
	text("DONE", geometry.done_x, geometry.done_y, geometry.done_width * 0.6f, u * 0.06f);
	{
		static const float import_button[4] = { 0.3f, 0.32f, 0.38f, 0.9f };
		static const float importing[4] = { 0.35f, 0.6f, 0.95f, 1.0f };
		char status[256];
		float fraction, left = geometry.import_x - geometry.import_width / 2.0f;
		int state = maps_import_status(status, sizeof(status), &fraction);

		quad(left, geometry.import_y - geometry.import_height / 2.0f,
			geometry.import_x + geometry.import_width / 2.0f, geometry.import_y + geometry.import_height / 2.0f,
			import_button, 0.0f);
		/* (the button fills as the import goes) */
		if (state == _maps_import_running)
		{
			quad(left, geometry.import_y - geometry.import_height / 2.0f, left + fraction * geometry.import_width,
				geometry.import_y + geometry.import_height / 2.0f, importing, 0.0f);
		}
		text(state == _maps_import_running ? "IMPORTING" : "IMPORT MAPS", geometry.import_x, geometry.import_y,
			geometry.import_width * 0.75f, u * 0.06f);
		text(status, (geometry.left + geometry.right) / 2.0f, geometry.status_y,
			geometry.right - geometry.left - 0.8f * u, u * 0.045f);
	}
}

void touch_controls_draw(void)
{
	GLint previous_program, previous_array, previous_buffer, previous_framebuffer, viewport[4];
	GLint blend_source_rgb, blend_destination_rgb, blend_source_alpha, blend_destination_alpha;
	GLint blend_equation_rgb, blend_equation_alpha;
	GLboolean color_mask[4], blend, scissor, depth, cull, stencil;
	int width, height;

	if (!window || !controls_shown())
		return;
	window_pixels(&width, &height);
	if (!width || !height)
		return;
	draw_width = (float)width;
	draw_height = (float)height;
	vertex_count = 0;
	if (!controls_hidden)
		controls_build(height > width);
	if (menu.open)
		menu_build();
	if (!vertex_count)
		return;

	glGetIntegerv(GL_CURRENT_PROGRAM, &previous_program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_array);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous_buffer);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_framebuffer);
	glGetIntegerv(GL_VIEWPORT, viewport);
	glGetIntegerv(GL_BLEND_SRC_RGB, &blend_source_rgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &blend_destination_rgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_source_alpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_destination_alpha);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_equation_rgb);
	glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_equation_alpha);
	glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
	blend = glIsEnabled(GL_BLEND);
	scissor = glIsEnabled(GL_SCISSOR_TEST);
	depth = glIsEnabled(GL_DEPTH_TEST);
	cull = glIsEnabled(GL_CULL_FACE);
	stencil = glIsEnabled(GL_STENCIL_TEST);
	/* (after the state is saved: it binds its own objects) */
	if (!draw_initialize())
	{
		glBindVertexArray((GLuint)previous_array);
		glBindBuffer(GL_ARRAY_BUFFER, (GLuint)previous_buffer);
		return;
	}

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, window_framebuffer);
	glViewport(0, 0, width, height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glUseProgram(program);
	glBindVertexArray(vertex_array);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), NULL, GL_STREAM_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(vertex_count * sizeof(struct vertex)), vertices);
	glDrawArrays(GL_TRIANGLES, 0, vertex_count);

	glBindVertexArray((GLuint)previous_array);
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)previous_buffer);
	glUseProgram((GLuint)previous_program);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)previous_framebuffer);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	glBlendFuncSeparate((GLenum)blend_source_rgb, (GLenum)blend_destination_rgb, (GLenum)blend_source_alpha,
		(GLenum)blend_destination_alpha);
	glBlendEquationSeparate((GLenum)blend_equation_rgb, (GLenum)blend_equation_alpha);
	glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
	(blend ? glEnable : glDisable)(GL_BLEND);
	(scissor ? glEnable : glDisable)(GL_SCISSOR_TEST);
	(depth ? glEnable : glDisable)(GL_DEPTH_TEST);
	(cull ? glEnable : glDisable)(GL_CULL_FACE);
	(stencil ? glEnable : glDisable)(GL_STENCIL_TEST);
}

/* ---------- starting */

void touch_controls_initialize(SDL_Window *game_window, unsigned int framebuffer)
{
	window = game_window;
	window_framebuffer = framebuffer;
	if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
		host_logf("touch controls: no gamepads (%s)", SDL_GetError());
	gamepad_attach();
	settings_load();
	host_logf("touch controls: ready (move %.2f, look %.2f)", move_sensitivity, look_sensitivity);
}
