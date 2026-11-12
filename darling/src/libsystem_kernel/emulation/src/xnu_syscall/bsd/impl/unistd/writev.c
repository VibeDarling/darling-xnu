#include <darling/emulation/xnu_syscall/bsd/impl/unistd/writev.h>

#include <darling/emulation/common/base.h>
#include <darling/emulation/conversion/errno.h>
#include <darling/emulation/linux_premigration/linux-syscalls/linux.h>
#include <darling/emulation/xnu_syscall/bsd/helper/bsdthread/cancelable.h>
#include <darling/emulation/conversion/network/duct.h>
#include <darling/emulation/conversion/network/recvmsg.h>
#include <darling/emulation/conversion/duct_errno.h>
#include <darling/emulation/xnu_syscall/bsd/helper/network/nosigpipe.h>

long sys_writev(int fd, struct iovec* iovp, unsigned int len)
{
	CANCELATION_POINT();
	return sys_writev_nocancel(fd, iovp, len);
}

long sys_writev_nocancel(int fd, struct iovec* iovp, unsigned int len)
{
	int ret;

	// A send with MSG_NOSIGNAL keeps SIGPIPE away from a socket that opted out of it.
	if (nosigpipe_get(fd)) {
		struct linux_msghdr msg = { .msg_iov = iovp, .msg_iovlen = len };
		ret = LINUX_SYSCALL(__NR_sendmsg, fd, &msg, LINUX_MSG_NOSIGNAL);
		if (ret != -LINUX_ENOTSOCK)
			return ret < 0 ? errno_linux_to_bsd(ret) : ret;
	}

	ret = LINUX_SYSCALL3(__NR_writev, fd, iovp, len);
	if (ret < 0)
		return errno_linux_to_bsd(ret);

	return ret;
}
