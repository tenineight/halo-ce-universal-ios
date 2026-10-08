/*
HOST_PICKER.M

The iOS document picker for the host (host_picker.h).
*/

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "host.h"
#include "host_picker.h"

@interface HaloFilePicker : NSObject <UIDocumentPickerDelegate>
@property (nonatomic) SDL_DialogFileCallback callback;
@property (nonatomic) void *userdata;
/* the window made to show the picker over, when the game has none yet */
@property (nonatomic, strong) UIWindow *window;
@end

/* the picker showing (one at a time) */
static HaloFilePicker *showing;

@implementation HaloFilePicker

- (void)finish:(const char *)path
{
	const char *files[2] = { path, NULL };
	SDL_DialogFileCallback callback = self.callback;
	void *userdata = self.userdata;

	self.window.hidden = YES;
	self.window = nil;
	showing = nil;
	callback(userdata, path ? files : &files[1], -1);
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
	NSURL *url = urls.firstObject;

	(void)controller;
	if (!url)
	{
		[self finish:NULL];
		return;
	}
	/* (read where it is: the access lasts the run) */
	if (![url startAccessingSecurityScopedResource])
		host_logf("file picker: no security scope for %s", url.path.UTF8String);
	[self finish:url.fileSystemRepresentation];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller
{
	(void)controller;
	[self finish:NULL];
}

@end

/* the content types of the filters' extensions; any file for "*" */
static NSArray<UTType *> *content_types(const SDL_DialogFileFilter *filters, int count)
{
	NSMutableArray<UTType *> *types = [NSMutableArray array];
	int index;

	for (index = 0; index < count; index++)
	{
		NSString *pattern = [NSString stringWithUTF8String:filters[index].pattern ? filters[index].pattern : ""];

		for (NSString *extension in [pattern componentsSeparatedByString:@";"])
		{
			UTType *type;

			if ([extension isEqualToString:@"*"])
				return @[ UTTypeItem ];
			type = [UTType typeWithFilenameExtension:extension];
			if (type)
				[types addObject:type];
		}
	}
	return types.count ? types : @[ UTTypeItem ];
}

static UIViewController *presenter(SDL_Window *window, HaloFilePicker *picker)
{
	UIWindow *ui_window = nil;

	if (window)
	{
		ui_window = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
			SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
	}
	if (!ui_window)
	{
		for (UIScene *scene in UIApplication.sharedApplication.connectedScenes)
		{
			if (![scene isKindOfClass:UIWindowScene.class])
				continue;
			for (UIWindow *candidate in ((UIWindowScene *)scene).windows)
			{
				if (candidate.isKeyWindow && candidate.rootViewController)
					ui_window = candidate;
			}
			/* none yet (the game's first start, before its window): one
			of the picker's own */
			if (!ui_window)
			{
				picker.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
				picker.window.rootViewController = [[UIViewController alloc] init];
				picker.window.rootViewController.view.backgroundColor = UIColor.blackColor;
				[picker.window makeKeyAndVisible];
				ui_window = picker.window;
			}
			break;
		}
	}
	{
		UIViewController *controller = ui_window.rootViewController;

		/* (over whatever it shows already) */
		while (controller.presentedViewController)
			controller = controller.presentedViewController;
		return controller;
	}
}

void ios_pick_file(SDL_Window *window, const SDL_DialogFileFilter *filters, int count,
	SDL_DialogFileCallback callback, void *userdata)
{
	const char *none[1] = { NULL };
	HaloFilePicker *picker;
	UIDocumentPickerViewController *controller;
	UIViewController *over;

	if (showing)
	{
		callback(userdata, none, -1);
		return;
	}
	picker = [[HaloFilePicker alloc] init];
	picker.callback = callback;
	picker.userdata = userdata;
	over = presenter(window, picker);
	if (!over)
	{
		host_logf("file picker: no window to show it over");
		callback(userdata, none, -1);
		return;
	}
	controller = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:content_types(filters, count)
		asCopy:NO];
	controller.delegate = picker;
	controller.allowsMultipleSelection = NO;
	showing = picker;
	[over presentViewController:controller animated:YES completion:nil];
}
