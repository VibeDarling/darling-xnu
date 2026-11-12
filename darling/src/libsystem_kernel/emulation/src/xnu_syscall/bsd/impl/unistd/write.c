#include <darling/emulation/xnu_syscall/bsd/impl/unistd/write.h>

#include <stddef.h>

#include <darling/emulation/common/base.h>
#include <darling/emulation/conversion/errno.h>
#include <darling/emulation/linux_premigration/linux-syscalls/linux.h>
#include <darling/emulation/xnu_syscall/bsd/helper/bsdthread/cancelable.h>
#include <darling/emulation/conversion/network/duct.h>
#include <darling/emulation/conversion/duct_errno.h>
#include <darling/emulation/xnu_syscall/bsd/helper/network/nosigpipe.h>

long sys_write(int fd, const void* mem, int len)
{
	CANCELATION_POINT();
	return sys_write_nocancel(fd, mem, len);
}

__attribute__((visibility("default")))
long __write_for_xtrace(int fd, const void* mem, int len)
{
	return sys_write_nocancel(fd, mem, len);
}


long sys_write_nocancel(int fd, const void* mem, int len)
{
	int ret;

	// A send with MSG_NOSIGNAL keeps SIGPIPE away from a socket that opted out of it.
	if (nosigpipe_get(fd)) {
		ret = LINUX_SYSCALL(__NR_sendto, fd, mem, len, LINUX_MSG_NOSIGNAL, NULL, 0);
		if (ret != -LINUX_ENOTSOCK)
			return ret < 0 ? errno_linux_to_bsd(ret) : ret;
	}

	ret = LINUX_SYSCALL3(__NR_write, fd, mem, len);
	if (ret < 0)
		return errno_linux_to_bsd(ret);

	return ret;
}
