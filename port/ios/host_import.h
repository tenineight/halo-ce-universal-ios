/*
HOST_IMPORT.H

Importing the game's maps folder from a disc image while the game runs
(host_import.c), for the on-screen controls' settings (host_touch.c): the
game's own first start does it before the game begins (sdl_platform.c's
platform_offer_game_data), this again whenever the player wants.
*/

#ifndef __HALO_IOS_HOST_IMPORT_H
#define __HALO_IOS_HOST_IMPORT_H

#include <stddef.h>

enum maps_import_state
{
	_maps_import_idle,
	_maps_import_running,
	_maps_import_succeeded,
	_maps_import_failed,
};

/* starts copying the image's maps folder over the game's (on a thread of
its own); 0 if one is under way already */
int maps_import_start(const char *image_path);

/* how it goes: the state, a line for the player, and the part done */
int maps_import_status(char *text, size_t size, float *fraction);

#endif
