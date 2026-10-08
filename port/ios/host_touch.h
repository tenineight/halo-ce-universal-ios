/*
HOST_TOUCH.H

The iOS port's on-screen controls (host_touch.c): a virtual gamepad the
game takes as any other, drawn over the game in landscape and below it in
portrait, and the touches elsewhere as the mouse (the menus' pointer, and
aiming).
*/

#ifndef __HALO_IOS_HOST_TOUCH_H
#define __HALO_IOS_HOST_TOUCH_H

#include <SDL3/SDL.h>

/* once the game's window has its OpenGL context; framebuffer is the
window's (UIKit has no framebuffer 0) */
void touch_controls_initialize(SDL_Window *window, unsigned int framebuffer);

/* takes the touches, and notices controllers coming and going; nonzero
when the game is not to see the event */
int touch_controls_event(const SDL_Event *event);

/* whether the game is being played rather than its menus shown (the game
takes the mouse as relative motion then: platform_mouse_capture); a tap
clicks only in the menus, since in the game a click fires */
void touch_controls_set_playing(int playing);

/* the game's blit of its picture to the window, in the window's pixels
(OpenGL's, from the bottom): in portrait it moves the picture to the top,
for the controls below */
void touch_controls_place_picture(int *x0, int *y0, int *x1, int *y1);

/* draws the controls over the frame, before it is shown */
void touch_controls_draw(void);

#endif
