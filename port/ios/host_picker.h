/*
HOST_PICKER.H

Picking a file on iOS (host_picker.m), which SDL's file dialogs do not
offer there: the system's document picker, the file opened where it is
(the Files app's iCloud Drive, a USB drive, another app's folder) rather
than copied in first, as a disc image is 4 GB or more.
*/

#ifndef __HALO_IOS_HOST_PICKER_H
#define __HALO_IOS_HOST_PICKER_H

#include <SDL3/SDL.h>

/* shows the picker over window's (or, with none, over a window of its own)
and calls callback as SDL_ShowOpenFileDialog does: with the file's path,
which stays readable for the rest of the run, or an empty list if the
player cancelled. filters' patterns are extensions ("iso;xiso"), "*" for
any file. On the main thread; callback is called on it, from the run loop
SDL_PumpEvents runs */
void ios_pick_file(SDL_Window *window, const SDL_DialogFileFilter *filters, int count,
	SDL_DialogFileCallback callback, void *userdata);

#endif
