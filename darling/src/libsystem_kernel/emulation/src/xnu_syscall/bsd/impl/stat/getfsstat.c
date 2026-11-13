#include <darling/emulation/xnu_syscall/bsd/impl/stat/getfsstat.h>

#include <stddef.h>
#include <sys/fcntl.h>

#include <darling/emulation/conversion/stat/common.h>
#include <darling/emulation/xnu_syscall/bsd/helper/stat/guest_mount.h>
#include <darling/emulation/common/base.h>
#include <darling/emulation/conversion/errno.h>
#include <darling/emulation/common/simple.h>
#include <darling/emulation/xnu_syscall/bsd/impl/fcntl/open.h>
#include <darling/emulation/xnu_syscall/bsd/impl/unistd/close.h>
#include <darling/emulation/linux_premigration/linux-syscalls/linux.h>

extern char *strtok_r(char *str, const char *delim, char **saveptr);
extern void *memset(void *s, int c, __SIZE_TYPE__ n);
extern unsigned long strlcpy(char* dst, const char* src, unsigned long size);

long sys_getfsstat(struct bsd_statfs* buf, int bufsize, int flags)
{
#ifdef __x86_64__
	return sys_getfsstat64((struct bsd_statfs64*) buf, bufsize, flags);
#else
#	warning Not implemented
	return 0;
#endif
}

// Next well formed line of /proc/self/mounts; returns 0 at the end of the file.
static int next_mount(int fd, struct simple_readline_buf* rbuf, char* line, __SIZE_TYPE__ size, char** from, char** on, char** type)
{
	while (__simple_readline(fd, rbuf, line, size))
	{
		char* saveptr;

		*from = strtok_r(line, " ", &saveptr);
		if (*from == NULL)
			continue;
		*on = strtok_r(NULL, " ", &saveptr);
		if (*on == NULL)
			continue;
		*type = strtok_r(NULL, " ", &saveptr);
		if (*type == NULL)
			continue;
		return 1;
	}
	return 0;
}

// Lists the volumes a guest can see: the root volume as "/" and the mounts below the guest
// root (see guest_mount.h), not the host's whole mount table. This is a workaround for
// callers that walk the list, not Darwin behaviour: statfs can name a mount that is not
// listed here.
long sys_getfsstat64(struct bsd_statfs64* buf, int bufsize, int flags)
{
	struct guest_mount_view view;
	struct simple_readline_buf rbuf;
	char line[512];
	char *from, *on, *type;
	int fd, count = 0, root_slot = -1;

	int rv = guest_mount_init(&view);
	if (rv < 0)
		return errno_linux_to_bsd(rv);

	fd = sys_open("/proc/self/mounts", O_RDONLY, 0);
	if (fd < 0)
		return errno_linux_to_bsd(fd);
	__simple_readline_init(&rbuf);
	while (next_mount(fd, &rbuf, line, sizeof(line), &from, &on, &type))
		guest_mount_note(&view, on);
	close_internal(fd);

	fd = sys_open("/proc/self/mounts", O_RDONLY, 0);
	if (fd < 0)
		return errno_linux_to_bsd(fd);
	__simple_readline_init(&rbuf);
	while (next_mount(fd, &rbuf, line, sizeof(line), &from, &on, &type))
	{
		char name[sizeof(buf->f_mntonname)];
		int listed;
		int is_root = guest_mount_is_root_volume(&view, on);
		// The last mount at the root volume's mount point is the one on top.
		int slot = is_root && root_slot >= 0 ? root_slot : count;

		if (guest_mount_name(&view, on, name, &listed) < 0 || !listed)
			continue;

		// A mount that cannot be queried is not listed, with or without a buffer, so the
		// count a caller sizes its buffer with matches what the second call fills.
		struct linux_statfs64 lbuf;
#ifdef __NR_statfs64
		if (LINUX_SYSCALL2(__NR_statfs64, on, &lbuf) < 0)
#else
		if (LINUX_SYSCALL2(__NR_statfs, on, &lbuf) < 0)
#endif
			continue;

		if (buf != NULL)
		{
			if (bufsize < sizeof(struct bsd_statfs64) * (slot + 1))
				break;

			statfs_linux_to_bsd64(&lbuf, &buf[slot]);
			strlcpy(buf[slot].f_mntfromname, from, sizeof(buf[slot].f_mntfromname));
			strlcpy(buf[slot].f_mntonname, name, sizeof(buf[slot].f_mntonname));
			strlcpy(buf[slot].f_fstypename, type, sizeof(buf[slot].f_fstypename));
		}

		if (slot == count)
			count++;
		if (is_root)
			root_slot = slot;
	}

	close_internal(fd);
	return count;
}
