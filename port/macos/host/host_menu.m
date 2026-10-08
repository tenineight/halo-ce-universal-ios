/*
HOST_MENU.M

The menu bar SDL gives the application (SDL_cocoaevents.m) answers
Command-W (close), Command-Q (quit), Command-H (hide) and Command-M
(minimize). The game binds the left Command key to fire, so that a player on a
trackpad can fire and aim at once (port_config.c): while firing, walking
forward (W) would close the game and the flashlight (Q) would quit it. The
menus keep their items, without the shortcuts.
*/

#import <AppKit/AppKit.h>

#include "host.h"

static void remove_command_shortcuts(NSMenu *menu)
{
	for (NSMenuItem *item in [menu itemArray])
	{
		if ([item hasSubmenu])
			remove_command_shortcuts([item submenu]);
		else if ([[item keyEquivalent] length] &&
			([item keyEquivalentModifierMask] & NSEventModifierFlagCommand))
			[item setKeyEquivalent:@""];
	}
}

void host_menu_remove_command_shortcuts(void)
{
	@autoreleasepool
	{
		remove_command_shortcuts([NSApp mainMenu]);
	}
}
