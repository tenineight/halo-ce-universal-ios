/*
HOST_IMPORT.C

Importing the game's maps folder from a disc image while the game runs
(host_import.h), with the platform layer's own extraction
(port/linux/src/xiso.c, compiled into the host too: port/ios/xiso has the
little of platform.h it takes).

The maps are extracted to the game's folder's import/maps first, and only
once they all are do they replace its maps folder (the one there before is
deleted then), so the game keeps its maps while the copy goes, and a
failed one leaves them as they were. The game has the old ones open until
it is started again.
*/

#include "host.h"
#include "host_import.h"
#include "xiso.h"

#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static pthread_mutex_t import_lock = PTHREAD_MUTEX_INITIALIZER;
static int import_state = _maps_import_idle;
static char import_image[PATH_MAX];
static char import_file[256];
static char import_error[512];
static unsigned long long import_done, import_total;

/* xiso.c's log */
void platform_log(const char *format, ...)
{
	char text[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	host_logf("%s", text);
}

static int remove_entry(const char *path, const struct stat *information, int type, struct FTW *walk)
{
	(void)information;
	(void)type;
	(void)walk;
	remove(path);
	return 0;
}

/* deletes a folder and everything in it */
static void remove_tree(const char *path)
{
	nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static void import_progress(void *context, const char *file, unsigned long long done, unsigned long long total)
{
	(void)context;
	pthread_mutex_lock(&import_lock);
	snprintf(import_file, sizeof(import_file), "%s", file);
	import_done = done;
	import_total = total;
	pthread_mutex_unlock(&import_lock);
}

static void *import_thread(void *argument)
{
	char base[PATH_MAX], staging[PATH_MAX], staged[PATH_MAX], maps[PATH_MAX], previous[PATH_MAX];
	char error[512] = "";
	size_t length;
	int succeeded;

	(void)argument;
	snprintf(base, sizeof(base), "%s", host_base_path());
	length = strlen(base);
	if (length > 1 && base[length - 1] == '/')
		base[length - 1] = 0;
	snprintf(staging, sizeof(staging), "%s/import", base);
	snprintf(staged, sizeof(staged), "%s/import/maps", base);
	snprintf(maps, sizeof(maps), "%s/maps", base);
	snprintf(previous, sizeof(previous), "%s/maps.previous", base);
	host_logf("importing the maps folder from %s", import_image);
	remove_tree(staging);
	mkdir(staging, 0755);
	succeeded = xiso_extract_maps(import_image, staging, import_progress, NULL, error, sizeof(error)) != 0;
	if (succeeded)
	{
		/* the new maps in place of the old */
		remove_tree(previous);
		if (rename(maps, previous) != 0 && errno != ENOENT)
		{
			snprintf(error, sizeof(error), "Could not move the old maps folder aside (%s).", strerror(errno));
			succeeded = 0;
		}
		else if (rename(staged, maps) != 0)
		{
			snprintf(error, sizeof(error), "Could not put the new maps folder in place (%s).", strerror(errno));
			rename(previous, maps);
			succeeded = 0;
		}
		else
		{
			remove_tree(previous);
		}
	}
	remove_tree(staging);
	host_logf("importing the maps folder: %s", succeeded ? "done" : error);
	pthread_mutex_lock(&import_lock);
	snprintf(import_error, sizeof(import_error), "%s", error);
	import_state = succeeded ? _maps_import_succeeded : _maps_import_failed;
	pthread_mutex_unlock(&import_lock);
	return NULL;
}

int maps_import_start(const char *image_path)
{
	pthread_t thread;

	pthread_mutex_lock(&import_lock);
	if (import_state == _maps_import_running)
	{
		pthread_mutex_unlock(&import_lock);
		return 0;
	}
	snprintf(import_image, sizeof(import_image), "%s", image_path);
	import_file[0] = 0;
	import_error[0] = 0;
	import_done = import_total = 0;
	import_state = _maps_import_running;
	pthread_mutex_unlock(&import_lock);
	if (pthread_create(&thread, NULL, import_thread, NULL) != 0)
	{
		pthread_mutex_lock(&import_lock);
		snprintf(import_error, sizeof(import_error), "Could not start the import.");
		import_state = _maps_import_failed;
		pthread_mutex_unlock(&import_lock);
		return 0;
	}
	pthread_detach(thread);
	return 1;
}

int maps_import_status(char *text, size_t size, float *fraction)
{
	int state;

	pthread_mutex_lock(&import_lock);
	state = import_state;
	*fraction = import_total ? (float)((double)import_done / (double)import_total) : 0.0f;
	switch (state)
	{
	case _maps_import_running:
		snprintf(text, size, "Importing %s, %d%%", import_file[0] ? import_file : "the maps",
			(int)(*fraction * 100.0f));
		break;
	case _maps_import_succeeded:
		snprintf(text, size, "Maps imported. Restart Halo to use them.");
		break;
	case _maps_import_failed:
		snprintf(text, size, "Import failed: %s", import_error);
		break;
	default:
		snprintf(text, size, "Import the maps from a Halo disc image (.iso).");
		break;
	}
	pthread_mutex_unlock(&import_lock);
	return state;
}
