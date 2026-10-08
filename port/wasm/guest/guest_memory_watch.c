/*
GUEST_MEMORY_WATCH.C

Guest memory write tracking (port/linux/src/memory_watch.c's interface).
The linear memory is the host's mapping and the page faults are the host's,
so the tracking itself lives there (port/macos/host/host_memory.c).
*/

#include "platform.h"
#include "guest_host.h"

void memory_watch_initialize(void)
{
	host_memory_watch_initialize();
}

void memory_watch_protect(unsigned long address, unsigned long size)
{
	host_memory_watch_protect(address, size);
}

unsigned long memory_watch_generation(unsigned long address, unsigned long size)
{
	return host_memory_watch_generation(address, size);
}

unsigned long memory_watch_serial(void)
{
	return host_memory_watch_serial();
}

void memory_watch_prepare_write(void *address, unsigned long size)
{
	host_memory_watch_prepare_write((unsigned int)address, size);
}

void memory_watch_forget(void *address, unsigned long size)
{
	host_memory_watch_forget((unsigned int)address, size);
}
