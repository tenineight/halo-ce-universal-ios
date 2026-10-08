/*
HOST_WASI.C

The WASI (preview 1) functions wasi-libc imports, for the macOS port's
guest (module "wasi_snapshot_preview1").

The guest's file system is the host's: its one preopen, file descriptor 3,
is the host's "/" (host_main.c), and its file descriptors are the host's
own, so every call here is the POSIX call it describes, with the guest's
structures (laid out as the WASI specification has them, little-endian)
translated and errno values mapped to WASI's. Pointers are guest addresses.
*/

#include "host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* ---------- WASI's definitions */

enum
{
	WASI_ESUCCESS = 0, WASI_E2BIG = 1, WASI_EACCES = 2, WASI_EADDRINUSE = 3, WASI_EADDRNOTAVAIL = 4,
	WASI_EAFNOSUPPORT = 5, WASI_EAGAIN = 6, WASI_EALREADY = 7, WASI_EBADF = 8, WASI_EBADMSG = 9,
	WASI_EBUSY = 10, WASI_ECANCELED = 11, WASI_ECHILD = 12, WASI_ECONNABORTED = 13, WASI_ECONNREFUSED = 14,
	WASI_ECONNRESET = 15, WASI_EDEADLK = 16, WASI_EDESTADDRREQ = 17, WASI_EDOM = 18, WASI_EDQUOT = 19,
	WASI_EEXIST = 20, WASI_EFAULT = 21, WASI_EFBIG = 22, WASI_EHOSTUNREACH = 23, WASI_EIDRM = 24,
	WASI_EILSEQ = 25, WASI_EINPROGRESS = 26, WASI_EINTR = 27, WASI_EINVAL = 28, WASI_EIO = 29,
	WASI_EISCONN = 30, WASI_EISDIR = 31, WASI_ELOOP = 32, WASI_EMFILE = 33, WASI_EMLINK = 34,
	WASI_EMSGSIZE = 35, WASI_EMULTIHOP = 36, WASI_ENAMETOOLONG = 37, WASI_ENETDOWN = 38, WASI_ENETRESET = 39,
	WASI_ENETUNREACH = 40, WASI_ENFILE = 41, WASI_ENOBUFS = 42, WASI_ENODEV = 43, WASI_ENOENT = 44,
	WASI_ENOEXEC = 45, WASI_ENOLCK = 46, WASI_ENOLINK = 47, WASI_ENOMEM = 48, WASI_ENOMSG = 49,
	WASI_ENOPROTOOPT = 50, WASI_ENOSPC = 51, WASI_ENOSYS = 52, WASI_ENOTCONN = 53, WASI_ENOTDIR = 54,
	WASI_ENOTEMPTY = 55, WASI_ENOTRECOVERABLE = 56, WASI_ENOTSOCK = 57, WASI_ENOTSUP = 58, WASI_ENOTTY = 59,
	WASI_ENXIO = 60, WASI_EOVERFLOW = 61, WASI_EOWNERDEAD = 62, WASI_EPERM = 63, WASI_EPIPE = 64,
	WASI_EPROTO = 65, WASI_EPROTONOSUPPORT = 66, WASI_EPROTOTYPE = 67, WASI_ERANGE = 68, WASI_EROFS = 69,
	WASI_ESPIPE = 70, WASI_ESRCH = 71, WASI_ESTALE = 72, WASI_ETIMEDOUT = 73, WASI_ETXTBSY = 74,
	WASI_EXDEV = 75, WASI_ENOTCAPABLE = 76,
};

enum
{
	WASI_FILETYPE_UNKNOWN = 0, WASI_FILETYPE_BLOCK_DEVICE = 1, WASI_FILETYPE_CHARACTER_DEVICE = 2,
	WASI_FILETYPE_DIRECTORY = 3, WASI_FILETYPE_REGULAR_FILE = 4, WASI_FILETYPE_SOCKET_DGRAM = 5,
	WASI_FILETYPE_SOCKET_STREAM = 6, WASI_FILETYPE_SYMBOLIC_LINK = 7,
};

#define WASI_FDFLAG_APPEND 0x01
#define WASI_FDFLAG_DSYNC 0x02
#define WASI_FDFLAG_NONBLOCK 0x04
#define WASI_FDFLAG_RSYNC 0x08
#define WASI_FDFLAG_SYNC 0x10

#define WASI_OFLAG_CREAT 0x01
#define WASI_OFLAG_DIRECTORY 0x02
#define WASI_OFLAG_EXCL 0x04
#define WASI_OFLAG_TRUNC 0x08

#define WASI_LOOKUP_SYMLINK_FOLLOW 0x01

#define WASI_FSTFLAG_ATIM 0x01
#define WASI_FSTFLAG_ATIM_NOW 0x02
#define WASI_FSTFLAG_MTIM 0x04
#define WASI_FSTFLAG_MTIM_NOW 0x08

#define WASI_RIGHT_FD_READ (1ull << 1)
#define WASI_RIGHT_FD_SEEK (1ull << 2)
#define WASI_RIGHT_FD_TELL (1ull << 5)
#define WASI_RIGHT_FD_WRITE (1ull << 6)
#define WASI_RIGHTS_ALL ((1ull << 30) - 1)

enum { WASI_CLOCK_REALTIME = 0, WASI_CLOCK_MONOTONIC = 1, WASI_CLOCK_PROCESS_CPUTIME = 2, WASI_CLOCK_THREAD_CPUTIME = 3 };
enum { WASI_WHENCE_SET = 0, WASI_WHENCE_CUR = 1, WASI_WHENCE_END = 2 };
enum { WASI_EVENTTYPE_CLOCK = 0, WASI_EVENTTYPE_FD_READ = 1, WASI_EVENTTYPE_FD_WRITE = 2 };
#define WASI_SUBSCRIPTION_CLOCK_ABSTIME 0x01
#define WASI_EVENT_FD_READWRITE_HANGUP 0x01

/* sizes of the guest's structures */
#define WASI_IOVEC_SIZE 8
#define WASI_FDSTAT_SIZE 24
#define WASI_FILESTAT_SIZE 64
#define WASI_DIRENT_SIZE 24
#define WASI_SUBSCRIPTION_SIZE 48
#define WASI_EVENT_SIZE 32

/* the preopen (host_main.c) */
#define PREOPEN_FD 3

/* the instance type of the guest's WASI imports (it has nothing) */
struct w2c_wasi__snapshot__preview1;
#define WASI_INSTANCE struct w2c_wasi__snapshot__preview1 *instance

/* ---------- guest memory */

static uint8_t *guest(uint32_t address)
{
	return HOST_MEMORY + address;
}

static void store8(uint32_t address, uint8_t value)
{
	*guest(address) = value;
}

static void store16(uint32_t address, uint16_t value)
{
	memcpy(guest(address), &value, sizeof(value));
}

static void store32(uint32_t address, uint32_t value)
{
	memcpy(guest(address), &value, sizeof(value));
}

static void store64(uint32_t address, uint64_t value)
{
	memcpy(guest(address), &value, sizeof(value));
}

static uint16_t load16(uint32_t address)
{
	uint16_t value;

	memcpy(&value, guest(address), sizeof(value));
	return value;
}

static uint32_t load32(uint32_t address)
{
	uint32_t value;

	memcpy(&value, guest(address), sizeof(value));
	return value;
}

static uint64_t load64(uint32_t address)
{
	uint64_t value;

	memcpy(&value, guest(address), sizeof(value));
	return value;
}

/* a guest path (not terminated) as a host one; 0, or a WASI errno value */
static uint32_t guest_path(char *buffer, uint32_t address, uint32_t length)
{
	if (length >= PATH_MAX)
		return WASI_ENAMETOOLONG;
	memcpy(buffer, guest(address), length);
	buffer[length] = '\0';
	if (memchr(buffer, '\0', length))
		return WASI_EINVAL;
	/* (the preopen itself) */
	if (!length)
		strcpy(buffer, ".");
	return 0;
}

#define GUEST_PATH(name, address, length) \
	char name[PATH_MAX]; \
	do { uint32_t path_error = guest_path(name, address, length); if (path_error) return path_error; } while (0)

/* ---------- errno */

uint16_t host_wasi_errno(int error)
{
	switch (error)
	{
	case 0: return WASI_ESUCCESS;
	case E2BIG: return WASI_E2BIG;
	case EACCES: return WASI_EACCES;
	case EADDRINUSE: return WASI_EADDRINUSE;
	case EADDRNOTAVAIL: return WASI_EADDRNOTAVAIL;
	case EAFNOSUPPORT: return WASI_EAFNOSUPPORT;
	case EAGAIN: return WASI_EAGAIN;
	case EALREADY: return WASI_EALREADY;
	case EBADF: return WASI_EBADF;
	case EBADMSG: return WASI_EBADMSG;
	case EBUSY: return WASI_EBUSY;
	case ECANCELED: return WASI_ECANCELED;
	case ECHILD: return WASI_ECHILD;
	case ECONNABORTED: return WASI_ECONNABORTED;
	case ECONNREFUSED: return WASI_ECONNREFUSED;
	case ECONNRESET: return WASI_ECONNRESET;
	case EDEADLK: return WASI_EDEADLK;
	case EDESTADDRREQ: return WASI_EDESTADDRREQ;
	case EDOM: return WASI_EDOM;
	case EDQUOT: return WASI_EDQUOT;
	case EEXIST: return WASI_EEXIST;
	case EFAULT: return WASI_EFAULT;
	case EFBIG: return WASI_EFBIG;
	case EHOSTUNREACH: return WASI_EHOSTUNREACH;
	case EHOSTDOWN: return WASI_EHOSTUNREACH;
	case EIDRM: return WASI_EIDRM;
	case EILSEQ: return WASI_EILSEQ;
	case EINPROGRESS: return WASI_EINPROGRESS;
	case EINTR: return WASI_EINTR;
	case EINVAL: return WASI_EINVAL;
	case EIO: return WASI_EIO;
	case EISCONN: return WASI_EISCONN;
	case EISDIR: return WASI_EISDIR;
	case ELOOP: return WASI_ELOOP;
	case EMFILE: return WASI_EMFILE;
	case EMLINK: return WASI_EMLINK;
	case EMSGSIZE: return WASI_EMSGSIZE;
	case EMULTIHOP: return WASI_EMULTIHOP;
	case ENAMETOOLONG: return WASI_ENAMETOOLONG;
	case ENETDOWN: return WASI_ENETDOWN;
	case ENETRESET: return WASI_ENETRESET;
	case ENETUNREACH: return WASI_ENETUNREACH;
	case ENFILE: return WASI_ENFILE;
	case ENOBUFS: return WASI_ENOBUFS;
	case ENODEV: return WASI_ENODEV;
	case ENOENT: return WASI_ENOENT;
	case ENOEXEC: return WASI_ENOEXEC;
	case ENOLCK: return WASI_ENOLCK;
	case ENOLINK: return WASI_ENOLINK;
	case ENOMEM: return WASI_ENOMEM;
	case ENOMSG: return WASI_ENOMSG;
	case ENOPROTOOPT: return WASI_ENOPROTOOPT;
	case ENOSPC: return WASI_ENOSPC;
	case ENOSYS: return WASI_ENOSYS;
	case ENOTCONN: return WASI_ENOTCONN;
	case ENOTDIR: return WASI_ENOTDIR;
	case ENOTEMPTY: return WASI_ENOTEMPTY;
	case ENOTRECOVERABLE: return WASI_ENOTRECOVERABLE;
	case ENOTSOCK: return WASI_ENOTSOCK;
	case ENOTSUP: return WASI_ENOTSUP;
	case EOPNOTSUPP: return WASI_ENOTSUP;
	case ENOTTY: return WASI_ENOTTY;
	case ENXIO: return WASI_ENXIO;
	case EOVERFLOW: return WASI_EOVERFLOW;
	case EOWNERDEAD: return WASI_EOWNERDEAD;
	case EPERM: return WASI_EPERM;
	case EPIPE: return WASI_EPIPE;
	case EPROTO: return WASI_EPROTO;
	case EPROTONOSUPPORT: return WASI_EPROTONOSUPPORT;
	case EPROTOTYPE: return WASI_EPROTOTYPE;
	case ERANGE: return WASI_ERANGE;
	case EROFS: return WASI_EROFS;
	case ESPIPE: return WASI_ESPIPE;
	case ESRCH: return WASI_ESRCH;
	case ESTALE: return WASI_ESTALE;
	case ETIMEDOUT: return WASI_ETIMEDOUT;
	case ETXTBSY: return WASI_ETXTBSY;
	case EXDEV: return WASI_EXDEV;
	default: return WASI_EIO;
	}
}

static uint32_t wasi_error(void)
{
	return host_wasi_errno(errno);
}

/* 0 or the errno value of a call returning -1 on failure */
static uint32_t wasi_result(int result)
{
	return result < 0 ? wasi_error() : WASI_ESUCCESS;
}

/* ---------- the command line and the environment */

static char **guest_arguments;
static int guest_argument_count;
static char **guest_environment;
static int guest_environment_count;

static void list_add(char ***list, int *count, const char *text)
{
	char **grown = realloc(*list, (*count + 1) * sizeof(**list));

	if (!grown)
		host_fatal("out of memory for the guest's environment");
	grown[(*count)++] = strdup(text);
	*list = grown;
}

void host_wasi_initialize(int argc, char **argv)
{
	extern char **environ;
	static const char *const passed[] = { "HOME=", "USER=", "TMPDIR=", "LANG=", "HALO_" };
	char **variable;
	int index;

	list_add(&guest_arguments, &guest_argument_count, "halo");
	for (index = 1; index < argc; index++)
	{
		/* (what older macOS gives an application Finder starts) */
		if (!strncmp(argv[index], "-psn_", 5))
			continue;
		list_add(&guest_arguments, &guest_argument_count, argv[index]);
	}
	for (variable = environ; *variable; variable++)
	{
		for (index = 0; index < (int)(sizeof(passed) / sizeof(passed[0])); index++)
		{
			if (!strncmp(*variable, passed[index], strlen(passed[index])))
			{
				list_add(&guest_environment, &guest_environment_count, *variable);
				break;
			}
		}
	}
}

static uint32_t strings_sizes(char **list, int count, uint32_t count_address, uint32_t size_address)
{
	uint32_t size = 0;
	int index;

	for (index = 0; index < count; index++)
		size += (uint32_t)strlen(list[index]) + 1;
	store32(count_address, (uint32_t)count);
	store32(size_address, size);
	return WASI_ESUCCESS;
}

static uint32_t strings_get(char **list, int count, uint32_t pointers, uint32_t buffer)
{
	int index;

	for (index = 0; index < count; index++)
	{
		size_t length = strlen(list[index]) + 1;

		store32(pointers + index * 4, buffer);
		memcpy(guest(buffer), list[index], length);
		buffer += (uint32_t)length;
	}
	return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_args_sizes_get(WASI_INSTANCE, u32 count, u32 size)
{
	(void)instance;
	return strings_sizes(guest_arguments, guest_argument_count, count, size);
}

u32 w2c_wasi__snapshot__preview1_args_get(WASI_INSTANCE, u32 pointers, u32 buffer)
{
	(void)instance;
	return strings_get(guest_arguments, guest_argument_count, pointers, buffer);
}

u32 w2c_wasi__snapshot__preview1_environ_sizes_get(WASI_INSTANCE, u32 count, u32 size)
{
	(void)instance;
	return strings_sizes(guest_environment, guest_environment_count, count, size);
}

u32 w2c_wasi__snapshot__preview1_environ_get(WASI_INSTANCE, u32 pointers, u32 buffer)
{
	(void)instance;
	return strings_get(guest_environment, guest_environment_count, pointers, buffer);
}

/* ---------- clocks */

static int host_clock(uint32_t id, clockid_t *clock)
{
	switch (id)
	{
	case WASI_CLOCK_REALTIME: *clock = CLOCK_REALTIME; return 1;
	case WASI_CLOCK_MONOTONIC: *clock = CLOCK_MONOTONIC; return 1;
	case WASI_CLOCK_PROCESS_CPUTIME: *clock = CLOCK_PROCESS_CPUTIME_ID; return 1;
	case WASI_CLOCK_THREAD_CPUTIME: *clock = CLOCK_THREAD_CPUTIME_ID; return 1;
	default: return 0;
	}
}

u32 w2c_wasi__snapshot__preview1_clock_res_get(WASI_INSTANCE, u32 id, u32 resolution)
{
	clockid_t clock;
	struct timespec value;

	(void)instance;
	if (!host_clock(id, &clock))
		return WASI_EINVAL;
	if (clock_getres(clock, &value) != 0)
		return wasi_error();
	store64(resolution, (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec);
	return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_clock_time_get(WASI_INSTANCE, u32 id, u64 precision, u32 time)
{
	clockid_t clock;

	(void)instance;
	(void)precision;
	if (!host_clock(id, &clock))
		return WASI_EINVAL;
	store64(time, clock_gettime_nsec_np(clock));
	return WASI_ESUCCESS;
}

/* ---------- file descriptors */

static uint8_t filetype_of_mode(mode_t mode)
{
	switch (mode & S_IFMT)
	{
	case S_IFBLK: return WASI_FILETYPE_BLOCK_DEVICE;
	case S_IFCHR: return WASI_FILETYPE_CHARACTER_DEVICE;
	case S_IFDIR: return WASI_FILETYPE_DIRECTORY;
	case S_IFREG: return WASI_FILETYPE_REGULAR_FILE;
	case S_IFSOCK: return WASI_FILETYPE_SOCKET_STREAM;
	case S_IFLNK: return WASI_FILETYPE_SYMBOLIC_LINK;
	default: return WASI_FILETYPE_UNKNOWN;
	}
}

static uint64_t nanoseconds(struct timespec value)
{
	return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

static void store_filestat(uint32_t address, const struct stat *information)
{
	store64(address + 0, (uint64_t)information->st_dev);
	store64(address + 8, (uint64_t)information->st_ino);
	store8(address + 16, filetype_of_mode(information->st_mode));
	memset(guest(address + 17), 0, 7);
	store64(address + 24, (uint64_t)information->st_nlink);
	store64(address + 32, (uint64_t)information->st_size);
	store64(address + 40, nanoseconds(information->st_atimespec));
	store64(address + 48, nanoseconds(information->st_mtimespec));
	store64(address + 56, nanoseconds(information->st_ctimespec));
}

/* the guest's iovecs as host ones (in iovecs, of count, or a buffer the
caller frees); NULL if out of memory */
#define IOVEC_STACK 16

static struct iovec *host_iovecs(struct iovec *stack, uint32_t vectors, uint32_t count, int written_to)
{
	struct iovec *iovecs = count <= IOVEC_STACK ? stack : malloc(count * sizeof(*iovecs));
	uint32_t index;

	if (!iovecs)
		return NULL;
	for (index = 0; index < count; index++)
	{
		uint32_t base = load32(vectors + index * WASI_IOVEC_SIZE);
		uint32_t length = load32(vectors + index * WASI_IOVEC_SIZE + 4);

		/* (the kernel cannot write to pages the write tracking watches) */
		if (written_to)
			host_memory_prepare_write(base, length);
		iovecs[index].iov_base = guest(base);
		iovecs[index].iov_len = length;
	}
	return iovecs;
}

static uint32_t transfer(uint32_t fd, uint32_t vectors, uint32_t count, int64_t offset, uint32_t done_address,
	int reading)
{
	struct iovec stack[IOVEC_STACK];
	struct iovec *iovecs;
	ssize_t done;
	uint32_t result = WASI_ESUCCESS;

	if (count > IOV_MAX)
		return WASI_EINVAL;
	iovecs = host_iovecs(stack, vectors, count, reading);
	if (!iovecs)
		return WASI_ENOMEM;
	if (reading)
		done = offset < 0 ? readv((int)fd, iovecs, (int)count) : preadv((int)fd, iovecs, (int)count, offset);
	else
		done = offset < 0 ? writev((int)fd, iovecs, (int)count) : pwritev((int)fd, iovecs, (int)count, offset);
	if (done < 0)
		result = wasi_error();
	else
		store32(done_address, (uint32_t)done);
	if (iovecs != stack)
		free(iovecs);
	return result;
}

u32 w2c_wasi__snapshot__preview1_fd_read(WASI_INSTANCE, u32 fd, u32 iovs, u32 count, u32 read)
{
	(void)instance;
	return transfer(fd, iovs, count, -1, read, 1);
}

u32 w2c_wasi__snapshot__preview1_fd_pread(WASI_INSTANCE, u32 fd, u32 iovs, u32 count, u64 offset, u32 read)
{
	(void)instance;
	if ((int64_t)offset < 0)
		return WASI_EINVAL;
	return transfer(fd, iovs, count, (int64_t)offset, read, 1);
}

u32 w2c_wasi__snapshot__preview1_fd_write(WASI_INSTANCE, u32 fd, u32 iovs, u32 count, u32 written)
{
	(void)instance;
	return transfer(fd, iovs, count, -1, written, 0);
}

u32 w2c_wasi__snapshot__preview1_fd_pwrite(WASI_INSTANCE, u32 fd, u32 iovs, u32 count, u64 offset, u32 written)
{
	(void)instance;
	if ((int64_t)offset < 0)
		return WASI_EINVAL;
	return transfer(fd, iovs, count, (int64_t)offset, written, 0);
}

u32 w2c_wasi__snapshot__preview1_fd_seek(WASI_INSTANCE, u32 fd, u64 offset, u32 whence, u32 position)
{
	int host_whence;
	off_t result;

	(void)instance;
	switch (whence)
	{
	case WASI_WHENCE_SET: host_whence = SEEK_SET; break;
	case WASI_WHENCE_CUR: host_whence = SEEK_CUR; break;
	case WASI_WHENCE_END: host_whence = SEEK_END; break;
	default: return WASI_EINVAL;
	}
	result = lseek((int)fd, (off_t)(int64_t)offset, host_whence);
	if (result < 0)
		return wasi_error();
	store64(position, (uint64_t)result);
	return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_tell(WASI_INSTANCE, u32 fd, u32 position)
{
	off_t result;

	(void)instance;
	result = lseek((int)fd, 0, SEEK_CUR);
	if (result < 0)
		return wasi_error();
	store64(position, (uint64_t)result);
	return WASI_ESUCCESS;
}

static void readdir_forget(int fd);

u32 w2c_wasi__snapshot__preview1_fd_close(WASI_INSTANCE, u32 fd)
{
	(void)instance;
	readdir_forget((int)fd);
	return wasi_result(close((int)fd));
}

u32 w2c_wasi__snapshot__preview1_fd_renumber(WASI_INSTANCE, u32 fd, u32 to)
{
	(void)instance;
	if (fcntl((int)fd, F_GETFD) < 0)
		return wasi_error();
	if (fd == to)
		return WASI_ESUCCESS;
	readdir_forget((int)to);
	if (dup2((int)fd, (int)to) < 0)
		return wasi_error();
	readdir_forget((int)fd);
	close((int)fd);
	return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_sync(WASI_INSTANCE, u32 fd)
{
	(void)instance;
	return wasi_result(fsync((int)fd));
}

u32 w2c_wasi__snapshot__preview1_fd_datasync(WASI_INSTANCE, u32 fd)
{
	(void)instance;
	return wasi_result(fsync((int)fd));
}

u32 w2c_wasi__snapshot__preview1_fd_advise(WASI_INSTANCE, u32 fd, u64 offset, u64 length, u32 advice)
{
	(void)instance;
	(void)offset;
	(void)length;
	(void)advice;
	/* advice only (macOS has no posix_fadvise) */
	return wasi_result(fcntl((int)fd, F_GETFD));
}

u32 w2c_wasi__snapshot__preview1_fd_allocate(WASI_INSTANCE, u32 fd, u64 offset, u64 length)
{
	struct stat information;
	uint64_t end = offset + length;

	(void)instance;
	if (end < offset || (int64_t)end < 0)
		return WASI_EFBIG;
	if (fstat((int)fd, &information) != 0)
		return wasi_error();
	if ((uint64_t)information.st_size >= end)
		return WASI_ESUCCESS;
	return wasi_result(ftruncate((int)fd, (off_t)end));
}

u32 w2c_wasi__snapshot__preview1_fd_fdstat_get(WASI_INSTANCE, u32 fd, u32 address)
{
	struct stat information;
	int flags;
	uint16_t fdflags = 0;
	uint64_t rights = WASI_RIGHTS_ALL;
	uint8_t filetype;

	(void)instance;
	if (fstat((int)fd, &information) != 0)
		return wasi_error();
	flags = fcntl((int)fd, F_GETFL);
	if (flags < 0)
		return wasi_error();
	filetype = filetype_of_mode(information.st_mode);
	if (filetype == WASI_FILETYPE_SOCKET_STREAM)
	{
		int type = SOCK_STREAM;
		socklen_t length = sizeof(type);

		if (getsockopt((int)fd, SOL_SOCKET, SO_TYPE, &type, &length) == 0 && type == SOCK_DGRAM)
			filetype = WASI_FILETYPE_SOCKET_DGRAM;
	}
	if (flags & O_APPEND)
		fdflags |= WASI_FDFLAG_APPEND;
	if (flags & O_DSYNC)
		fdflags |= WASI_FDFLAG_DSYNC;
	if (flags & O_NONBLOCK)
		fdflags |= WASI_FDFLAG_NONBLOCK;
	if (flags & O_SYNC)
		fdflags |= WASI_FDFLAG_SYNC;
	/* (wasi-libc's isatty: a character device that cannot seek) */
	if (filetype == WASI_FILETYPE_CHARACTER_DEVICE && isatty((int)fd))
		rights &= ~(WASI_RIGHT_FD_SEEK | WASI_RIGHT_FD_TELL);
	memset(guest(address), 0, WASI_FDSTAT_SIZE);
	store8(address + 0, filetype);
	store16(address + 2, fdflags);
	store64(address + 8, rights);
	store64(address + 16, rights);
	return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_fdstat_set_flags(WASI_INSTANCE, u32 fd, u32 fdflags)
{
	int flags;

	(void)instance;
	flags = fcntl((int)fd, F_GETFL);
	if (flags < 0)
		return wasi_error();
	flags &= ~(O_APPEND | O_NONBLOCK);
	if (fdflags & WASI_FDFLAG_APPEND)
		flags |= O_APPEND;
	if (fdflags & WASI_FDFLAG_NONBLOCK)
		flags |= O_NONBLOCK;
	return wasi_result(fcntl((int)fd, F_SETFL, flags));
}

u32 w2c_wasi__snapshot__preview1_fd_fdstat_set_rights(WASI_INSTANCE, u32 fd, u64 base, u64 inheriting)
{
	(void)instance;
	(void)base;
	(void)inheriting;
	return wasi_result(fcntl((int)fd, F_GETFD));
}

u32 w2c_wasi__snapshot__preview1_fd_filestat_get(WASI_INSTANCE, u32 fd, u32 address)
{
	struct stat information;

	(void)instance;
	if (fstat((int)fd, &information) != 0)
		return wasi_error();
	store_filestat(address, &information);
	return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_filestat_set_size(WASI_INSTANCE, u32 fd, u64 size)
{
	(void)instance;
	if ((int64_t)size < 0)
		return WASI_EINVAL;
	return wasi_result(ftruncate((int)fd, (off_t)size));
}

/* the timespecs of utimensat for the guest's times and flags; 0, or a
WASI errno value */
static uint32_t file_times(struct timespec times[2], uint64_t access, uint64_t modification, uint32_t flags)
{
	if (((flags & WASI_FSTFLAG_ATIM) && (flags & WASI_FSTFLAG_ATIM_NOW)) ||
		((flags & WASI_FSTFLAG_MTIM) && (flags & WASI_FSTFLAG_MTIM_NOW)))
	{
		return WASI_EINVAL;
	}
	times[0].tv_sec = (time_t)(access / 1000000000ull);
	times[0].tv_nsec = (long)(access % 1000000000ull);
	if (flags & WASI_FSTFLAG_ATIM_NOW)
		times[0].tv_nsec = UTIME_NOW;
	else if (!(flags & WASI_FSTFLAG_ATIM))
		times[0].tv_nsec = UTIME_OMIT;
	times[1].tv_sec = (time_t)(modification / 1000000000ull);
	times[1].tv_nsec = (long)(modification % 1000000000ull);
	if (flags & WASI_FSTFLAG_MTIM_NOW)
		times[1].tv_nsec = UTIME_NOW;
	else if (!(flags & WASI_FSTFLAG_MTIM))
		times[1].tv_nsec = UTIME_OMIT;
	return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_filestat_set_times(WASI_INSTANCE, u32 fd, u64 access, u64 modification,
	u32 flags)
{
	struct timespec times[2];
	uint32_t error = file_times(times, access, modification, flags);

	(void)instance;
	if (error)
		return error;
	return wasi_result(futimens((int)fd, times));
}

u32 w2c_wasi__snapshot__preview1_fd_prestat_get(WASI_INSTANCE, u32 fd, u32 address)
{
	(void)instance;
	/* one preopen, "/"; wasi-libc looks from 3 up to the first EBADF */
	if (fd != PREOPEN_FD)
		return WASI_EBADF;
	store32(address, 0);
	store32(address + 4, 1);
	return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_prestat_dir_name(WASI_INSTANCE, u32 fd, u32 path, u32 length)
{
	(void)instance;
	if (fd != PREOPEN_FD)
		return WASI_EBADF;
	if (length < 1)
		return WASI_ENAMETOOLONG;
	store8(path, '/');
	return WASI_ESUCCESS;
}

/* ---------- directories

fd_readdir lists from a cookie, the number of entries before it. Each
directory being listed keeps a host DIR of its own (over a descriptor of
its own, so the guest's offset is the guest's), positioned where its last
call stopped; an entry that did not fit is kept for the next. */

#define READDIR_STREAMS 16

struct readdir_stream
{
	int fd; /* the guest's; -1 if unused */
	dev_t device;
	ino_t inode;
	DIR *directory;
	uint64_t position; /* the cookie of the next entry */
	struct dirent pending; /* the entry at position, read already */
	int has_pending;
	uint64_t used; /* for replacing the least recently used */
};

static struct readdir_stream readdir_streams[READDIR_STREAMS];
static pthread_mutex_t readdir_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t readdir_clock;
static int readdir_ready;

static void readdir_close(struct readdir_stream *stream)
{
	if (stream->directory)
		closedir(stream->directory);
	stream->directory = NULL;
	stream->fd = -1;
	stream->has_pending = 0;
}

static void readdir_forget(int fd)
{
	int index;

	pthread_mutex_lock(&readdir_lock);
	for (index = 0; readdir_ready && index < READDIR_STREAMS; index++)
	{
		if (readdir_streams[index].fd == fd)
			readdir_close(&readdir_streams[index]);
	}
	pthread_mutex_unlock(&readdir_lock);
}

/* the stream for fd, positioned at cookie; NULL with errno set (readdir_lock
held) */
static struct readdir_stream *readdir_stream(int fd, uint64_t cookie)
{
	struct readdir_stream *stream = NULL;
	struct stat information;
	int index;

	if (!readdir_ready)
	{
		for (index = 0; index < READDIR_STREAMS; index++)
			readdir_streams[index].fd = -1;
		readdir_ready = 1;
	}
	if (fstat(fd, &information) != 0)
		return NULL;
	if (!S_ISDIR(information.st_mode))
	{
		errno = ENOTDIR;
		return NULL;
	}
	for (index = 0; index < READDIR_STREAMS; index++)
	{
		struct readdir_stream *candidate = &readdir_streams[index];

		if (candidate->fd == fd)
		{
			/* (the descriptor may be another directory's now) */
			if (candidate->device == information.st_dev && candidate->inode == information.st_ino)
				stream = candidate;
			else
				readdir_close(candidate);
			break;
		}
	}
	if (!stream)
	{
		int own;

		stream = &readdir_streams[0];
		for (index = 0; index < READDIR_STREAMS; index++)
		{
			if (readdir_streams[index].fd < 0)
			{
				stream = &readdir_streams[index];
				break;
			}
			if (readdir_streams[index].used < stream->used)
				stream = &readdir_streams[index];
		}
		readdir_close(stream);
		own = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (own < 0)
			return NULL;
		stream->directory = fdopendir(own);
		if (!stream->directory)
		{
			close(own);
			return NULL;
		}
		stream->fd = fd;
		stream->device = information.st_dev;
		stream->inode = information.st_ino;
		stream->position = 0;
	}
	stream->used = ++readdir_clock;
	if (cookie != stream->position)
	{
		/* from the start again */
		rewinddir(stream->directory);
		stream->position = 0;
		stream->has_pending = 0;
		while (stream->position < cookie)
		{
			errno = 0;
			if (!readdir(stream->directory))
			{
				if (errno)
					return NULL;
				break;
			}
			stream->position++;
		}
	}
	return stream;
}

static uint8_t filetype_of_dirent(uint8_t type)
{
	switch (type)
	{
	case DT_BLK: return WASI_FILETYPE_BLOCK_DEVICE;
	case DT_CHR: return WASI_FILETYPE_CHARACTER_DEVICE;
	case DT_DIR: return WASI_FILETYPE_DIRECTORY;
	case DT_REG: return WASI_FILETYPE_REGULAR_FILE;
	case DT_SOCK: return WASI_FILETYPE_SOCKET_STREAM;
	case DT_LNK: return WASI_FILETYPE_SYMBOLIC_LINK;
	default: return WASI_FILETYPE_UNKNOWN;
	}
}

u32 w2c_wasi__snapshot__preview1_fd_readdir(WASI_INSTANCE, u32 fd, u32 buffer, u32 length, u64 cookie, u32 used)
{
	struct readdir_stream *stream;
	uint32_t written = 0;
	uint32_t result = WASI_ESUCCESS;

	(void)instance;
	pthread_mutex_lock(&readdir_lock);
	stream = readdir_stream((int)fd, cookie);
	if (!stream)
	{
		result = wasi_error();
		pthread_mutex_unlock(&readdir_lock);
		return result;
	}
	while (written < length)
	{
		uint8_t header[WASI_DIRENT_SIZE];
		uint32_t name_length, size, part;
		struct dirent *entry;

		if (!stream->has_pending)
		{
			errno = 0;
			entry = readdir(stream->directory);
			if (!entry)
			{
				if (errno)
					result = wasi_error();
				break;
			}
			memcpy(&stream->pending, entry, sizeof(stream->pending));
			stream->has_pending = 1;
		}
		entry = &stream->pending;
		name_length = (uint32_t)strlen(entry->d_name);
		memset(header, 0, sizeof(header));
		{
			uint64_t next = stream->position + 1;
			uint64_t inode = (uint64_t)entry->d_ino;

			memcpy(header + 0, &next, 8);
			memcpy(header + 8, &inode, 8);
			memcpy(header + 16, &name_length, 4);
			header[20] = filetype_of_dirent(entry->d_type);
		}
		/* as much as fits: wasi-libc asks again, with a larger buffer, for
		an entry that did not */
		size = WASI_DIRENT_SIZE + name_length;
		part = size < length - written ? size : length - written;
		memcpy(guest(buffer + written), header, part < WASI_DIRENT_SIZE ? part : WASI_DIRENT_SIZE);
		if (part > WASI_DIRENT_SIZE)
			memcpy(guest(buffer + written + WASI_DIRENT_SIZE), entry->d_name, part - WASI_DIRENT_SIZE);
		written += part;
		if (part < size)
			break;
		stream->has_pending = 0;
		stream->position++;
	}
	pthread_mutex_unlock(&readdir_lock);
	if (result == WASI_ESUCCESS)
		store32(used, written);
	return result;
}

/* ---------- paths */

u32 w2c_wasi__snapshot__preview1_path_open(WASI_INSTANCE, u32 fd, u32 lookup, u32 path, u32 path_length,
	u32 oflags, u64 rights, u64 inheriting, u32 fdflags, u32 opened)
{
	int flags = O_CLOEXEC;
	int readable = (rights & WASI_RIGHT_FD_READ) != 0;
	int writable = (rights & WASI_RIGHT_FD_WRITE) != 0;
	int result;
	GUEST_PATH(name, path, path_length);

	(void)instance;
	(void)inheriting;
	if (oflags & WASI_OFLAG_DIRECTORY)
	{
		flags |= O_DIRECTORY | O_RDONLY;
	}
	else
	{
		if (readable && writable)
			flags |= O_RDWR;
		else if (writable)
			flags |= O_WRONLY;
		else
			flags |= O_RDONLY;
	}
	if (oflags & WASI_OFLAG_CREAT)
		flags |= O_CREAT;
	if (oflags & WASI_OFLAG_EXCL)
		flags |= O_EXCL;
	if (oflags & WASI_OFLAG_TRUNC)
		flags |= O_TRUNC;
	if (fdflags & WASI_FDFLAG_APPEND)
		flags |= O_APPEND;
	if (fdflags & WASI_FDFLAG_NONBLOCK)
		flags |= O_NONBLOCK;
	if (fdflags & WASI_FDFLAG_DSYNC)
		flags |= O_DSYNC;
	if (fdflags & (WASI_FDFLAG_SYNC | WASI_FDFLAG_RSYNC))
		flags |= O_SYNC;
	if (!(lookup & WASI_LOOKUP_SYMLINK_FOLLOW))
		flags |= O_NOFOLLOW;
	result = openat((int)fd, name, flags, 0666);
	/* (a directory opened for reading only, as WASI may ask for one) */
	if (result < 0 && errno == EISDIR && !(oflags & (WASI_OFLAG_CREAT | WASI_OFLAG_TRUNC)) && writable)
		result = openat((int)fd, name, (flags & ~(O_RDWR | O_WRONLY)) | O_RDONLY, 0666);
	if (result < 0)
		return wasi_error();
	store32(opened, (uint32_t)result);
	return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_path_create_directory(WASI_INSTANCE, u32 fd, u32 path, u32 path_length)
{
	GUEST_PATH(name, path, path_length);

	(void)instance;
	return wasi_result(mkdirat((int)fd, name, 0777));
}

u32 w2c_wasi__snapshot__preview1_path_filestat_get(WASI_INSTANCE, u32 fd, u32 lookup, u32 path, u32 path_length,
	u32 address)
{
	struct stat information;
	GUEST_PATH(name, path, path_length);

	(void)instance;
	if (fstatat((int)fd, name, &information, (lookup & WASI_LOOKUP_SYMLINK_FOLLOW) ? 0 : AT_SYMLINK_NOFOLLOW) != 0)
		return wasi_error();
	store_filestat(address, &information);
	return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_path_filestat_set_times(WASI_INSTANCE, u32 fd, u32 lookup, u32 path,
	u32 path_length, u64 access, u64 modification, u32 flags)
{
	struct timespec times[2];
	uint32_t error;
	GUEST_PATH(name, path, path_length);

	(void)instance;
	error = file_times(times, access, modification, flags);
	if (error)
		return error;
	return wasi_result(utimensat((int)fd, name, times, (lookup & WASI_LOOKUP_SYMLINK_FOLLOW) ? 0 : AT_SYMLINK_NOFOLLOW));
}

u32 w2c_wasi__snapshot__preview1_path_link(WASI_INSTANCE, u32 old_fd, u32 lookup, u32 old_path, u32 old_length,
	u32 new_fd, u32 new_path, u32 new_length)
{
	GUEST_PATH(old_name, old_path, old_length);
	GUEST_PATH(new_name, new_path, new_length);

	(void)instance;
	return wasi_result(linkat((int)old_fd, old_name, (int)new_fd, new_name,
		(lookup & WASI_LOOKUP_SYMLINK_FOLLOW) ? AT_SYMLINK_FOLLOW : 0));
}

u32 w2c_wasi__snapshot__preview1_path_readlink(WASI_INSTANCE, u32 fd, u32 path, u32 path_length, u32 buffer,
	u32 length, u32 used)
{
	char target[PATH_MAX];
	ssize_t result;
	GUEST_PATH(name, path, path_length);

	(void)instance;
	result = readlinkat((int)fd, name, target, sizeof(target));
	if (result < 0)
		return wasi_error();
	if ((uint32_t)result > length)
		result = length;
	memcpy(guest(buffer), target, (size_t)result);
	store32(used, (uint32_t)result);
	return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_path_remove_directory(WASI_INSTANCE, u32 fd, u32 path, u32 path_length)
{
	GUEST_PATH(name, path, path_length);

	(void)instance;
	return wasi_result(unlinkat((int)fd, name, AT_REMOVEDIR));
}

u32 w2c_wasi__snapshot__preview1_path_rename(WASI_INSTANCE, u32 old_fd, u32 old_path, u32 old_length, u32 new_fd,
	u32 new_path, u32 new_length)
{
	GUEST_PATH(old_name, old_path, old_length);
	GUEST_PATH(new_name, new_path, new_length);

	(void)instance;
	return wasi_result(renameat((int)old_fd, old_name, (int)new_fd, new_name));
}

u32 w2c_wasi__snapshot__preview1_path_symlink(WASI_INSTANCE, u32 old_path, u32 old_length, u32 fd, u32 new_path,
	u32 new_length)
{
	GUEST_PATH(target, old_path, old_length);
	GUEST_PATH(name, new_path, new_length);

	(void)instance;
	return wasi_result(symlinkat(target, (int)fd, name));
}

u32 w2c_wasi__snapshot__preview1_path_unlink_file(WASI_INSTANCE, u32 fd, u32 path, u32 path_length)
{
	GUEST_PATH(name, path, path_length);

	(void)instance;
	return wasi_result(unlinkat((int)fd, name, 0));
}

/* ---------- waiting */

static void sleep_until(clockid_t clock, uint64_t deadline)
{
	for (;;)
	{
		uint64_t now = clock_gettime_nsec_np(clock);
		struct timespec delay;

		if (now >= deadline)
			return;
		delay.tv_sec = (time_t)((deadline - now) / 1000000000ull);
		delay.tv_nsec = (long)((deadline - now) % 1000000000ull);
		nanosleep(&delay, NULL);
	}
}

u32 w2c_wasi__snapshot__preview1_poll_oneoff(WASI_INSTANCE, u32 in, u32 out, u32 count, u32 events_address)
{
	struct pollfd *descriptors;
	uint32_t *descriptor_of; /* each subscription's index in descriptors, or ~0 */
	uint32_t descriptor_count = 0, event_count = 0, index;
	uint64_t timeout = UINT64_MAX; /* nanoseconds from now, of the earliest clock */
	uint32_t earliest = UINT32_MAX;
	uint64_t start = clock_gettime_nsec_np(CLOCK_MONOTONIC);

	(void)instance;
	if (!count)
		return WASI_EINVAL;
	descriptors = calloc(count, sizeof(*descriptors));
	descriptor_of = calloc(count, sizeof(*descriptor_of));
	if (!descriptors || !descriptor_of)
	{
		free(descriptors);
		free(descriptor_of);
		return WASI_ENOMEM;
	}
	for (index = 0; index < count; index++)
	{
		uint32_t subscription = in + index * WASI_SUBSCRIPTION_SIZE;
		uint8_t type = *guest(subscription + 8);

		descriptor_of[index] = UINT32_MAX;
		if (type == WASI_EVENTTYPE_CLOCK)
		{
			uint32_t id = load32(subscription + 16);
			uint64_t value = load64(subscription + 24);
			uint16_t flags = load16(subscription + 40);
			clockid_t clock;
			uint64_t remaining = value;

			if (!host_clock(id, &clock))
				clock = CLOCK_MONOTONIC;
			if (flags & WASI_SUBSCRIPTION_CLOCK_ABSTIME)
			{
				uint64_t now = clock_gettime_nsec_np(clock);

				remaining = value > now ? value - now : 0;
			}
			if (remaining < timeout || earliest == UINT32_MAX)
			{
				timeout = remaining;
				earliest = index;
			}
		}
		else if (type == WASI_EVENTTYPE_FD_READ || type == WASI_EVENTTYPE_FD_WRITE)
		{
			descriptors[descriptor_count].fd = (int)load32(subscription + 16);
			descriptors[descriptor_count].events = type == WASI_EVENTTYPE_FD_READ ? POLLIN : POLLOUT;
			descriptor_of[index] = descriptor_count++;
		}
	}
	if (!descriptor_count)
	{
		/* a sleep */
		if (earliest != UINT32_MAX && timeout)
			sleep_until(CLOCK_MONOTONIC, start + timeout);
	}
	else
	{
		for (;;)
		{
			uint64_t elapsed = clock_gettime_nsec_np(CLOCK_MONOTONIC) - start;
			int milliseconds = -1;
			int ready;

			if (earliest != UINT32_MAX)
			{
				uint64_t left = timeout > elapsed ? timeout - elapsed : 0;

				milliseconds = left / 1000000ull > INT32_MAX ? INT32_MAX : (int)((left + 999999ull) / 1000000ull);
			}
			ready = poll(descriptors, descriptor_count, milliseconds);
			if (ready >= 0 || errno != EINTR)
				break;
		}
	}
	/* the events: the descriptors ready, or else the earliest clock */
	for (index = 0; index < count; index++)
	{
		uint32_t subscription = in + index * WASI_SUBSCRIPTION_SIZE;
		uint32_t event = out + event_count * WASI_EVENT_SIZE;
		uint8_t type = *guest(subscription + 8);
		uint16_t error = WASI_ESUCCESS, flags = 0;
		uint64_t bytes = 0;

		if (descriptor_of[index] != UINT32_MAX)
		{
			struct pollfd *descriptor = &descriptors[descriptor_of[index]];

			if (!descriptor->revents)
				continue;
			if (descriptor->revents & POLLNVAL)
				error = WASI_EBADF;
			if (descriptor->revents & POLLHUP)
				flags |= WASI_EVENT_FD_READWRITE_HANGUP;
			if (type == WASI_EVENTTYPE_FD_READ)
			{
				int available = 0;

				if (ioctl(descriptor->fd, FIONREAD, &available) == 0 && available > 0)
					bytes = (uint64_t)available;
			}
		}
		else if (index != earliest || clock_gettime_nsec_np(CLOCK_MONOTONIC) - start < timeout)
		{
			continue;
		}
		memset(guest(event), 0, WASI_EVENT_SIZE);
		store64(event + 0, load64(subscription + 0));
		store16(event + 8, error);
		store8(event + 10, type);
		store64(event + 16, bytes);
		store16(event + 24, flags);
		event_count++;
	}
	free(descriptors);
	free(descriptor_of);
	store32(events_address, event_count);
	return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_sched_yield(WASI_INSTANCE)
{
	(void)instance;
	sched_yield();
	return WASI_ESUCCESS;
}

/* ---------- the process */

void w2c_wasi__snapshot__preview1_proc_exit(WASI_INSTANCE, u32 code)
{
	(void)instance;
	host_logf("the game exited (%u)", code);
	fflush(NULL);
	exit((int)code);
}

u32 w2c_wasi__snapshot__preview1_proc_raise(WASI_INSTANCE, u32 signal_number)
{
	(void)instance;
	(void)signal_number;
	return WASI_ENOSYS;
}

u32 w2c_wasi__snapshot__preview1_random_get(WASI_INSTANCE, u32 buffer, u32 length)
{
	(void)instance;
	arc4random_buf(guest(buffer), length);
	return WASI_ESUCCESS;
}

/* ---------- sockets (the platform layer's are the host's: posix_net.c) */

u32 w2c_wasi__snapshot__preview1_sock_accept(WASI_INSTANCE, u32 fd, u32 flags, u32 accepted)
{
	(void)instance;
	(void)fd;
	(void)flags;
	(void)accepted;
	return WASI_ENOSYS;
}

u32 w2c_wasi__snapshot__preview1_sock_recv(WASI_INSTANCE, u32 fd, u32 iovs, u32 count, u32 flags, u32 received,
	u32 out_flags)
{
	(void)instance;
	(void)fd;
	(void)iovs;
	(void)count;
	(void)flags;
	(void)received;
	(void)out_flags;
	return WASI_ENOSYS;
}

u32 w2c_wasi__snapshot__preview1_sock_send(WASI_INSTANCE, u32 fd, u32 iovs, u32 count, u32 flags, u32 sent)
{
	(void)instance;
	(void)fd;
	(void)iovs;
	(void)count;
	(void)flags;
	(void)sent;
	return WASI_ENOSYS;
}

u32 w2c_wasi__snapshot__preview1_sock_shutdown(WASI_INSTANCE, u32 fd, u32 how)
{
	(void)instance;
	(void)fd;
	(void)how;
	return WASI_ENOSYS;
}
