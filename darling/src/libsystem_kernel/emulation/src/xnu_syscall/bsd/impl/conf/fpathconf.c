#include <darling/emulation/xnu_syscall/bsd/impl/conf/fpathconf.h>

#include <sys/errno.h>

#include <darling/emulation/common/base.h>
#include <darling/emulation/conversion/errno.h>
#include <darling/emulation/linux_premigration/linux-syscalls/linux.h>

enum {
	BSD_PC_LINK_MAX = 1,
	BSD_PC_MAX_CANON,
	BSD_PC_MAX_INPUT,
	BSD_PC_NAME_MAX,
	BSD_PC_PATH_MAX,
	BSD_PC_PIPE_BUF,
	BSD_PC_CHOWN_RESTRICTED,
	BSD_PC_NO_TRUNC,
	BSD_PC_VDISABLE,
	BSD_PC_NAME_CHARS_MAX,
	BSD_PC_CASE_SENSITIVE,
	BSD_PC_CASE_PRESERVING,
	BSD_PC_EXTENDED_SECURITY_NP,
	BSD_PC_AUTH_OPAQUE_NP,
	BSD_PC_2_SYMLINKS,
	BSD_PC_ALLOC_SIZE_MIN,
	BSD_PC_ASYNC_IO,
	BSD_PC_FILESIZEBITS,
	BSD_PC_PRIO_IO,
	BSD_PC_REC_INCR_XFER_SIZE,
	BSD_PC_REC_MAX_XFER_SIZE,
	BSD_PC_REC_MIN_XFER_SIZE,
	BSD_PC_REC_XFER_ALIGN,
	BSD_PC_SYMLINK_MAX,
	BSD_PC_SYNC_IO,
	BSD_PC_XATTR_SIZE_BITS
};

long sys_fpathconf(int fd, int name)
{
	switch (name) {
	case BSD_PC_LINK_MAX: return 8;
	case BSD_PC_MAX_CANON: return 255;
	case BSD_PC_MAX_INPUT: return 255;
	case BSD_PC_NAME_MAX: return 255;
	case BSD_PC_PATH_MAX: return 4096;
	case BSD_PC_PIPE_BUF: return 4096;
	case BSD_PC_CHOWN_RESTRICTED: return 1;
	case BSD_PC_NO_TRUNC: return 1;
	case BSD_PC_VDISABLE: return 0;

	// Names 10 to 26 follow xnu bsd/sys/unistd.h. The values marked "xnu" are the constants
	// vn_pathconf() in bsd/vfs/vfs_vnops.c answers for every filesystem. The rest xnu asks of
	// the filesystem, and the guest's files live on Linux filesystems, so those values are
	// author choices (GUESS) that describe them. Where a choice is derived it says from what.
	case BSD_PC_NAME_CHARS_MAX: return 255; // GUESS: counts bytes, like _PC_NAME_MAX
	// Derived: vchroot lookups fall back to case-insensitive matching when the exact name is
	// missing (vchroot_userspace.c, icase_enabled), so the guest's namespace is case
	// insensitive and case preserving.
	case BSD_PC_CASE_SENSITIVE: return 0;
	case BSD_PC_CASE_PRESERVING: return 1; // GUESS: Linux filesystems keep the name as created
	case BSD_PC_EXTENDED_SECURITY_NP: return 0; // GUESS: no ACLs through this bridge
	case BSD_PC_AUTH_OPAQUE_NP: return 0; // GUESS: authorization is done by the host kernel, not opaque to the guest
	case BSD_PC_2_SYMLINKS: return 1; // xnu
	case BSD_PC_ALLOC_SIZE_MIN: return 1; // xnu
	case BSD_PC_ASYNC_IO: return 1; // xnu
	case BSD_PC_FILESIZEBITS: return 64; // GUESS: Linux off_t is 64 bit
	case BSD_PC_PRIO_IO: return 0; // xnu
	case BSD_PC_REC_INCR_XFER_SIZE: return 4096; // xnu
	case BSD_PC_REC_MAX_XFER_SIZE: return 65536; // xnu
	case BSD_PC_REC_MIN_XFER_SIZE: return 4096; // xnu
	case BSD_PC_REC_XFER_ALIGN: return 4096; // xnu
	case BSD_PC_SYMLINK_MAX: return 255; // xnu
	case BSD_PC_SYNC_IO: return 0; // xnu
	// Derived: xnu answers AD_XATTR_SIZE_BITS (bsd/sys/xattr.h, 31) for a filesystem without
	// native extended attributes (vfs_vnops.c); use that value.
	case BSD_PC_XATTR_SIZE_BITS: return 31;
	default:
		// 0, negative, _PC_MIN_HOLE_SIZE (27, filesystem specific) and unknown names.
		return -EINVAL;
	}
}
