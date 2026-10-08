/*
HOST_MEMORY.C

The guest's linear memory, for the macOS port.

The host maps the memory itself, at the fixed address
HALO_LINEAR_MEMORY_BASE, so that wasm2c's code (patched by
tools/wasm2c_patch.py) addresses it as a constant: 8 GB of address space,
the 4 GB a 32-bit guest can address and 4 GB of guard pages after them (an
access is an unsigned 32-bit address plus an unsigned 32-bit offset), all
inaccessible beyond the memory's current size. wasm2c's runtime grows it in
place (wasm_rt_grow_memory_shared) from the descriptor filled here.

The guest also maps and protects pages of it itself (xbox_memory.c's Xbox
contiguous window, through host_memory_map and host_memory_protect), in the
Xbox's 4 KB pages; macOS on Apple Silicon has 16 KB pages. The protection
of each 4 KB page is kept here, and a 16 KB page gets the union of its four:
a page the guest made inaccessible may stay readable or writable while a
neighbour in the same 16 KB page is, which only loses a fault the game
never relies on.

This file also implements guest memory write tracking (the interface of
port/linux/src/memory_watch.c) over the window: the renderer write-protects
the pages behind the textures it caches, and the fault handler here records
the first write to each (in 16 KB pages). macOS reports a write to a
read-only page as SIGBUS, an access to an unmapped one as SIGSEGV; every
fault the tracking does not expect goes on to the handler installed before
(wasm2c's, which turns it into a trap).
*/

#include "host.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#define GUEST_PAGE 0x1000ull
#define HOST_PAGE 0x4000ull
#define GUEST_PAGES_PER_HOST_PAGE (HOST_PAGE / GUEST_PAGE)
#define GUEST_ADDRESS_SPACE 0x100000000ull
#define PROTECTION_TABLE_SIZE (GUEST_ADDRESS_SPACE / GUEST_PAGE)

/* a protection table entry: the guest's HOST_PROT_* bits, once set */
#define PROTECTION_SET 0x80
#define PROTECTION_BITS (HOST_PROT_READ | HOST_PROT_WRITE)

/* the guest's own definitions (port/wasm/guest/guest_host.h) */
#define HOST_PROT_NONE 0
#define HOST_PROT_READ 1
#define HOST_PROT_WRITE 2

/* the Xbox contiguous window (port/linux/src/platform.h) */
#define WINDOW_BASE 0x80000000ull
#define WINDOW_SIZE 0x08000000ull
#define WATCH_PAGE_COUNT (WINDOW_SIZE / HOST_PAGE)

wasm_rt_shared_memory_t host_memory;
#ifdef HALO_IOS
uintptr_t halo_linear_memory_base;
#endif
volatile uintptr_t host_fault_address, host_fault_pc;

static uint8_t protection_table[PROTECTION_TABLE_SIZE];
static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t watch_protected[WATCH_PAGE_COUNT];
static uint32_t watch_generation[WATCH_PAGE_COUNT];
static volatile uint32_t watch_serial = 1;
static volatile int watch_active;

static struct sigaction previous_segv, previous_bus;

static uint64_t round_down(uint64_t value, uint64_t alignment)
{
	return value & ~(alignment - 1);
}

static uint64_t round_up(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static int native_protection(int protection)
{
	if (protection & HOST_PROT_WRITE)
		return PROT_READ | PROT_WRITE;
	if (protection & HOST_PROT_READ)
		return PROT_READ;
	return PROT_NONE;
}

static int in_window(uint64_t address)
{
	return address >= WINDOW_BASE && address - WINDOW_BASE < WINDOW_SIZE;
}

static uint64_t watch_page(uint64_t address)
{
	return (address - WINDOW_BASE) / HOST_PAGE;
}

/* the native protection of the 16 KB page at guest address `page`: the
union of its 4 KB pages', without write access while the tracking watches
it (safe in the fault handler: no locks) */
static int page_protection(uint64_t page)
{
	int protection = 0;
	uint64_t index;

	for (index = 0; index < GUEST_PAGES_PER_HOST_PAGE; index++)
	{
		uint64_t address = page + index * GUEST_PAGE;
		uint8_t entry = protection_table[address / GUEST_PAGE];

		if (entry & PROTECTION_SET)
			protection |= entry & PROTECTION_BITS;
		else if (address < host_memory.size)
			protection |= HOST_PROT_READ | HOST_PROT_WRITE;
	}
	if (watch_active && in_window(page) && watch_protected[watch_page(page)])
		protection &= ~HOST_PROT_WRITE;
	return native_protection(protection);
}

/* gives the 16 KB pages over [from, to) (guest addresses) their protection,
in as few calls as their protections allow */
static int pages_apply(uint64_t from, uint64_t to)
{
	uint64_t run_start = round_down(from, HOST_PAGE);
	uint64_t page;
	int run_protection = -1;
	int result = 0;

	to = round_up(to, HOST_PAGE);
	for (page = run_start; page <= to; page += HOST_PAGE)
	{
		int protection = page < to ? page_protection(page) : -1;

		if (protection == run_protection)
			continue;
		if (run_protection >= 0 && mprotect(HOST_MEMORY + run_start, page - run_start, run_protection) != 0)
			result = -1;
		run_start = page;
		run_protection = protection;
	}
	return result;
}

/* ---------- the memory */

wasm_rt_shared_memory_t *host_memory_initialize(uint64_t initial_pages, uint64_t maximum_pages)
{
	uint64_t size = initial_pages * WASM_DEFAULT_PAGE_SIZE;
	uint64_t reservation = HOST_MEMORY_RESERVATION;
	void *result;

	if ((uint64_t)getpagesize() > HOST_PAGE || HOST_PAGE % (uint64_t)getpagesize())
		host_fatal("unexpected page size %d", getpagesize());
	if (maximum_pages > GUEST_ADDRESS_SPACE / WASM_DEFAULT_PAGE_SIZE || initial_pages > maximum_pages)
		host_fatal("the guest's memory (%llu to %llu pages) does not fit 4 GB",
			(unsigned long long)initial_pages, (unsigned long long)maximum_pages);
#ifdef HALO_IOS
	/* wherever there is room (port/ios/halo_ios_memory.h): 8 GB with the
	extended virtual addressing entitlement, without it 4 GB and a little,
	where an access far past the memory's end goes unnoticed */
	{
		static const uint64_t reservations[] = { HOST_MEMORY_RESERVATION, GUEST_ADDRESS_SPACE + 0x1000000ull };
		int index;

		result = MAP_FAILED;
		for (index = 0; index < 2 && result == MAP_FAILED; index++)
		{
			result = mmap(NULL, reservations[index], PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
			reservation = reservations[index];
		}
		if (result == MAP_FAILED)
			host_fatal("cannot reserve 4 GB of address space for the guest's memory (%s)", strerror(errno));
		if (reservation < HOST_MEMORY_RESERVATION)
			host_logf("guest memory: only %llu MB of address space (no extended virtual addressing)",
				(unsigned long long)(reservation >> 20));
		halo_linear_memory_base = (uintptr_t)result;
	}
#else
	/* a hint, which never replaces another mapping */
	result = mmap(HOST_MEMORY, HOST_MEMORY_RESERVATION, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
		-1, 0);
	if (result != (void *)HOST_MEMORY)
	{
		if (result != MAP_FAILED)
			munmap(result, HOST_MEMORY_RESERVATION);
		host_fatal("cannot reserve the guest's memory at %p (%s)", (void *)HOST_MEMORY,
			result == MAP_FAILED ? strerror(errno) : "the address is taken");
	}
#endif
	if (size && mprotect(HOST_MEMORY, size, PROT_READ | PROT_WRITE) != 0)
		host_fatal("cannot commit the guest's initial memory (%s)", strerror(errno));
	/* as wasm_rt_allocate_memory_shared fills it (wasm-rt-mem-impl.c), so
	that wasm_rt_grow_memory_shared grows it in place */
	host_memory.data = (_Atomic volatile uint8_t *)HOST_MEMORY;
	host_memory.data_end = (_Atomic volatile uint8_t *)(HOST_MEMORY + reservation);
	host_memory.page_size = WASM_DEFAULT_PAGE_SIZE;
	host_memory.pages = initial_pages;
	host_memory.max_pages = maximum_pages;
	host_memory.size = size;
	host_memory.is64 = false;
	if (pthread_mutex_init(&host_memory.mem_lock, NULL) != 0)
		host_fatal("cannot create the guest memory's lock");
	host_logf("guest memory at %p: %llu MB, at most %llu MB", (void *)HOST_MEMORY,
		(unsigned long long)(size >> 20), (unsigned long long)((maximum_pages * WASM_DEFAULT_PAGE_SIZE) >> 20));
	return &host_memory;
}

/* the guest's import "env" "memory" */
struct w2c_env;
wasm_rt_shared_memory_t *w2c_env_memory(struct w2c_env *instance)
{
	(void)instance;
	return &host_memory;
}

void *const *host_guest_pointer_array(uint32_t address, uint32_t count)
{
	static _Thread_local void **buffer;
	static _Thread_local uint32_t capacity;
	const uint32_t *guest = host_guest_pointer(address);
	uint32_t index;

	if (!count || !guest)
		return NULL;
	if (count > capacity)
	{
		void **grown = realloc(buffer, count * sizeof(*buffer));

		if (!grown)
			host_fatal("out of memory for %u pointers", count);
		buffer = grown;
		capacity = count;
	}
	for (index = 0; index < count; index++)
		buffer[index] = host_guest_pointer(guest[index]);
	return buffer;
}

/* ---------- write tracking (port/linux/src/memory_watch.c) */

static void mark_written(uint64_t page)
{
	uint64_t address = WINDOW_BASE + page * HOST_PAGE;

	watch_generation[page] = __sync_add_and_fetch(&watch_serial, 1);
	watch_protected[page] = 0;
	mprotect(HOST_MEMORY + address, HOST_PAGE, page_protection(address));
}

/* the window's 16 KB pages over [address, address + size); 0 if none */
static int watch_range(uint64_t address, uint64_t size, uint64_t *first, uint64_t *last)
{
	uint64_t end = address + size;

	if (!size || end <= WINDOW_BASE || address >= WINDOW_BASE + WINDOW_SIZE)
		return 0;
	if (address < WINDOW_BASE)
		address = WINDOW_BASE;
	if (end > WINDOW_BASE + WINDOW_SIZE)
		end = WINDOW_BASE + WINDOW_SIZE;
	*first = watch_page(address);
	*last = watch_page(end - 1);
	return 1;
}

/* new contents for the pages: newer than anything cached from them, and
no longer watched (memory_lock held) */
static void watch_forget(uint64_t address, uint64_t size)
{
	uint64_t first, last, page;

	if (!watch_range(address, size, &first, &last))
		return;
	for (page = first; page <= last; page++)
	{
		watch_protected[page] = 0;
		watch_generation[page] = __sync_add_and_fetch(&watch_serial, 1);
	}
}

void host_memory_prepare_write(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!watch_active || !watch_range(address, size, &first, &last))
		return;
	pthread_mutex_lock(&memory_lock);
	for (page = first; page <= last; page++)
	{
		if (watch_protected[page])
			mark_written(page);
	}
	pthread_mutex_unlock(&memory_lock);
}

static void fault_handler(int signal_number, siginfo_t *information, void *context)
{
	uint64_t address = (uint64_t)information->si_addr;
	ucontext_t *ucontext = context;
	struct sigaction *previous = signal_number == SIGBUS ? &previous_bus : &previous_segv;

	if (address - HALO_LINEAR_MEMORY_BASE < GUEST_ADDRESS_SPACE)
	{
		uint64_t guest = address - HALO_LINEAR_MEMORY_BASE;

		if (watch_active && in_window(guest))
		{
			uint64_t page = watch_page(guest);
			uint64_t page_address = WINDOW_BASE + page * HOST_PAGE;
			int protection;

			if (watch_protected[page])
			{
				mark_written(page);
				return;
			}
			/* another thread marked it written (or the guest changed its
			protection) between the access and here: make sure the page
			has its protection and try again */
			protection = page_protection(page_address);
			if (protection & PROT_WRITE)
			{
				mprotect(HOST_MEMORY + page_address, HOST_PAGE, protection);
				return;
			}
		}
	}
	/* a genuine fault, for the trap's report (host_run_guest) */
	host_fault_address = (uintptr_t)address;
	host_fault_pc = ucontext ? (uintptr_t)ucontext->uc_mcontext->__ss.__pc : 0;
	{
		char line[160];
		int length;

		if (address - HALO_LINEAR_MEMORY_BASE < HOST_MEMORY_RESERVATION)
			length = snprintf(line, sizeof(line), "halo-host: signal %d at guest address %08llx, pc %p\n",
				signal_number, (unsigned long long)(address - HALO_LINEAR_MEMORY_BASE), (void *)host_fault_pc);
		else
			length = snprintf(line, sizeof(line), "halo-host: signal %d at %p, pc %p\n", signal_number,
				(void *)address, (void *)host_fault_pc);
		if (length > 0)
			write(STDERR_FILENO, line, (size_t)length < sizeof(line) ? (size_t)length : sizeof(line) - 1);
	}
	if (previous->sa_flags & SA_SIGINFO)
	{
		if (previous->sa_sigaction)
		{
			/* wasm2c's: a trap, which does not return */
			previous->sa_sigaction(signal_number, information, context);
			return;
		}
	}
	else if (previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN)
	{
		previous->sa_handler(signal_number);
		return;
	}
	/* returning re-executes the faulting instruction under the default
	handler */
	signal(signal_number, SIG_DFL);
}

void host_memory_watch_install(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	/* (on the alternate stack wasm2c's runtime gives each thread, for a
	stack overflow) */
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	action.sa_sigaction = fault_handler;
	if (sigaction(SIGSEGV, &action, &previous_segv) != 0 || sigaction(SIGBUS, &action, &previous_bus) != 0)
		host_fatal("cannot install the page fault handler (%s)", strerror(errno));
}

/* ---------- the guest's imports (module "halo") */

static int check_range(uint32_t address, uint32_t size, uint64_t *end)
{
	if (address % GUEST_PAGE)
	{
		errno = EINVAL;
		return -1;
	}
	*end = round_up((uint64_t)address + size, GUEST_PAGE);
	if (*end > host_memory.size)
	{
		errno = ENOMEM;
		return -1;
	}
	return 0;
}

/* zeroes [from, to), within one 16 KB page */
static int clear_partial(uint64_t from, uint64_t to)
{
	uint64_t page = round_down(from, HOST_PAGE);

	if (mprotect(HOST_MEMORY + page, HOST_PAGE, PROT_READ | PROT_WRITE) != 0)
		return -1;
	memset(HOST_MEMORY + from, 0, to - from);
	return mprotect(HOST_MEMORY + page, HOST_PAGE, page_protection(page));
}

u32 w2c_halo_host_memory_map(struct w2c_halo *instance, u32 address, u32 size, u32 protection)
{
	uint64_t end, inner_start, inner_end, page;
	int result = 0;

	(void)instance;
	if (!size)
		return 0;
	if (check_range(address, size, &end) != 0)
		return (u32)-1;
	protection &= PROTECTION_BITS;
	pthread_mutex_lock(&memory_lock);
	for (page = address / GUEST_PAGE; page < end / GUEST_PAGE; page++)
		protection_table[page] = PROTECTION_SET | protection;
	watch_forget(address, end - address);
	inner_start = round_up(address, HOST_PAGE);
	inner_end = round_down(end, HOST_PAGE);
	if (inner_start < inner_end &&
		mmap(HOST_MEMORY + inner_start, inner_end - inner_start, native_protection(protection),
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void *)(HOST_MEMORY + inner_start))
	{
		result = -1;
	}
	if (inner_start > inner_end)
	{
		/* within one 16 KB page */
		if (clear_partial(address, end) != 0)
			result = -1;
	}
	else
	{
		if (address < inner_start && clear_partial(address, inner_start) != 0)
			result = -1;
		if (end > inner_end && clear_partial(inner_end, end) != 0)
			result = -1;
	}
	pthread_mutex_unlock(&memory_lock);
	return (u32)result;
}

u32 w2c_halo_host_memory_protect(struct w2c_halo *instance, u32 address, u32 size, u32 protection)
{
	uint64_t end, page;
	int result;

	(void)instance;
	if (!size)
		return 0;
	if (check_range(address, size, &end) != 0)
		return (u32)-1;
	protection &= PROTECTION_BITS;
	pthread_mutex_lock(&memory_lock);
	for (page = address / GUEST_PAGE; page < end / GUEST_PAGE; page++)
		protection_table[page] = PROTECTION_SET | protection;
	result = pages_apply(address, end);
	pthread_mutex_unlock(&memory_lock);
	return (u32)result;
}

void w2c_halo_host_memory_watch_initialize(struct w2c_halo *instance)
{
	(void)instance;
	watch_active = 1;
}

void w2c_halo_host_memory_watch_protect(struct w2c_halo *instance, u32 address, u32 size)
{
	uint64_t first, last, page;

	(void)instance;
	if (!watch_active || !watch_range(address, size, &first, &last))
		return;
	pthread_mutex_lock(&memory_lock);
	for (page = first; page <= last; page++)
	{
		if (!watch_protected[page])
		{
			uint64_t page_address = WINDOW_BASE + page * HOST_PAGE;

			watch_protected[page] = 1;
			mprotect(HOST_MEMORY + page_address, HOST_PAGE, page_protection(page_address));
		}
	}
	pthread_mutex_unlock(&memory_lock);
}

u32 w2c_halo_host_memory_watch_generation(struct w2c_halo *instance, u32 address, u32 size)
{
	uint64_t first, last, page;
	uint32_t newest = 0;

	(void)instance;
	if (!watch_range(address, size, &first, &last))
		return 0;
	for (page = first; page <= last; page++)
	{
		if (watch_generation[page] > newest)
			newest = watch_generation[page];
	}
	return newest;
}

u32 w2c_halo_host_memory_watch_serial(struct w2c_halo *instance)
{
	(void)instance;
	return watch_serial;
}

void w2c_halo_host_memory_watch_prepare_write(struct w2c_halo *instance, u32 address, u32 size)
{
	(void)instance;
	host_memory_prepare_write(address, size);
}

void w2c_halo_host_memory_watch_forget(struct w2c_halo *instance, u32 address, u32 size)
{
	uint64_t first, last, page;

	(void)instance;
	if (!watch_range(address, size, &first, &last))
		return;
	pthread_mutex_lock(&memory_lock);
	watch_forget(address, size);
	/* (pages it wrote-protected are writable again) */
	for (page = first; page <= last; page++)
	{
		uint64_t page_address = WINDOW_BASE + page * HOST_PAGE;

		mprotect(HOST_MEMORY + page_address, HOST_PAGE, page_protection(page_address));
	}
	pthread_mutex_unlock(&memory_lock);
}

/* ---------- memory.atomic.wait and memory.atomic.notify

wasm2c does not translate these, so a pass ahead of it (tools/macos_build.py)
replaces them with calls to these imports, with the instruction's offset
as the last argument. A waiter checks the value and queues itself under
its bucket's lock, which a notification takes too, so no wake-up is lost. */

#define WAIT_BUCKETS 256

struct waiter
{
	uint64_t address;
	pthread_cond_t condition;
	int woken;
	struct waiter *next;
};

static struct
{
	pthread_mutex_t lock;
	struct waiter *waiters;
} wait_buckets[WAIT_BUCKETS];
static pthread_once_t wait_once = PTHREAD_ONCE_INIT;

static void wait_initialize(void)
{
	int index;

	for (index = 0; index < WAIT_BUCKETS; index++)
		pthread_mutex_init(&wait_buckets[index].lock, NULL);
}

static unsigned wait_bucket(uint64_t address)
{
	return (unsigned)((address >> 2) * 0x9e3779b1u) % WAIT_BUCKETS;
}

/* (u32 sums of a 32-bit address and offset reach past 4 GB as wasm's do) */
static uint64_t atomic_address(u32 address, u32 offset, u32 size)
{
	uint64_t effective = (uint64_t)address + offset;

	if (effective % size)
		wasm_rt_trap(WASM_RT_TRAP_UNALIGNED);
	if (effective + size > host_memory.size)
		wasm_rt_trap(WASM_RT_TRAP_OOB);
	return effective;
}

static u32 atomic_wait(uint64_t address, uint64_t expected, int64_t timeout, int wide)
{
	unsigned bucket = wait_bucket(address);
	struct waiter self, **link;
	struct timespec deadline;
	int matches, timed_out = 0;

	pthread_once(&wait_once, wait_initialize);
	pthread_mutex_lock(&wait_buckets[bucket].lock);
	if (wide)
		matches = __atomic_load_n((uint64_t *)(HOST_MEMORY + address), __ATOMIC_SEQ_CST) == expected;
	else
		matches = __atomic_load_n((uint32_t *)(HOST_MEMORY + address), __ATOMIC_SEQ_CST) == (uint32_t)expected;
	if (!matches)
	{
		pthread_mutex_unlock(&wait_buckets[bucket].lock);
		return 1;
	}
	self.address = address;
	self.woken = 0;
	pthread_cond_init(&self.condition, NULL);
	self.next = wait_buckets[bucket].waiters;
	wait_buckets[bucket].waiters = &self;
	if (timeout >= 0)
	{
		struct timeval now;
		uint64_t nanoseconds;

		gettimeofday(&now, NULL);
		nanoseconds = (uint64_t)now.tv_usec * 1000 + (uint64_t)timeout;
		deadline.tv_sec = now.tv_sec + (time_t)(nanoseconds / 1000000000ull);
		deadline.tv_nsec = (long)(nanoseconds % 1000000000ull);
	}
	while (!self.woken && !timed_out)
	{
		if (timeout < 0)
			pthread_cond_wait(&self.condition, &wait_buckets[bucket].lock);
		else if (pthread_cond_timedwait(&self.condition, &wait_buckets[bucket].lock, &deadline) == ETIMEDOUT)
			timed_out = 1;
	}
	if (!self.woken)
	{
		for (link = &wait_buckets[bucket].waiters; *link; link = &(*link)->next)
		{
			if (*link == &self)
			{
				*link = self.next;
				break;
			}
		}
	}
	pthread_mutex_unlock(&wait_buckets[bucket].lock);
	pthread_cond_destroy(&self.condition);
	return self.woken ? 0 : 2;
}

u32 w2c_halo_host_atomic_wait32(struct w2c_halo *instance, u32 address, u32 expected, u64 timeout, u32 offset)
{
	(void)instance;
	return atomic_wait(atomic_address(address, offset, 4), expected, (int64_t)timeout, 0);
}

u32 w2c_halo_host_atomic_wait64(struct w2c_halo *instance, u32 address, u64 expected, u64 timeout, u32 offset)
{
	(void)instance;
	return atomic_wait(atomic_address(address, offset, 8), expected, (int64_t)timeout, 1);
}

u32 w2c_halo_host_atomic_notify(struct w2c_halo *instance, u32 address, u32 count, u32 offset)
{
	uint64_t effective = atomic_address(address, offset, 4);
	unsigned bucket = wait_bucket(effective);
	struct waiter **link;
	u32 woken = 0;

	(void)instance;
	pthread_once(&wait_once, wait_initialize);
	pthread_mutex_lock(&wait_buckets[bucket].lock);
	link = &wait_buckets[bucket].waiters;
	while (*link && woken < count)
	{
		struct waiter *waiter = *link;

		if (waiter->address != effective)
		{
			link = &waiter->next;
			continue;
		}
		*link = waiter->next;
		waiter->woken = 1;
		pthread_cond_signal(&waiter->condition);
		woken++;
	}
	pthread_mutex_unlock(&wait_buckets[bucket].lock);
	return woken;
}
