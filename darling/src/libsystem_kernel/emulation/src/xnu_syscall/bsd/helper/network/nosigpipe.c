#include <darling/emulation/xnu_syscall/bsd/helper/network/nosigpipe.h>

#include <sys/errno.h>

#define NOSIGPIPE_MAX_FD 65536

static unsigned long nosigpipe_bits[NOSIGPIPE_MAX_FD / (8 * sizeof(unsigned long))];

#define NOSIGPIPE_WORD(fd) (&nosigpipe_bits[(unsigned int) (fd) / (8 * sizeof(unsigned long))])
#define NOSIGPIPE_BIT(fd) (1UL << ((unsigned int) (fd) % (8 * sizeof(unsigned long))))

int nosigpipe_set(int fd, bool enabled)
{
	if (fd < 0)
		return -EBADF;
	if (fd >= NOSIGPIPE_MAX_FD)
		return -ENOTSUP;

	if (enabled)
		__atomic_fetch_or(NOSIGPIPE_WORD(fd), NOSIGPIPE_BIT(fd), __ATOMIC_RELAXED);
	else
		__atomic_fetch_and(NOSIGPIPE_WORD(fd), ~NOSIGPIPE_BIT(fd), __ATOMIC_RELAXED);
	return 0;
}

bool nosigpipe_get(int fd)
{
	if (fd < 0 || fd >= NOSIGPIPE_MAX_FD)
		return false;
	return (__atomic_load_n(NOSIGPIPE_WORD(fd), __ATOMIC_RELAXED) & NOSIGPIPE_BIT(fd)) != 0;
}

void nosigpipe_closed(int fd)
{
	nosigpipe_set(fd, false);
}
