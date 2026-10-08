/*
PLATFORM.H (port/ios/xiso)

What port/linux/src/xiso.c takes from the platform layer's platform.h,
for the iOS host's own copy of it (host_import.c): the log, which is the
host's there.
*/

#ifndef __HALO_IOS_XISO_PLATFORM_H
#define __HALO_IOS_XISO_PLATFORM_H

#include <fcntl.h>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
