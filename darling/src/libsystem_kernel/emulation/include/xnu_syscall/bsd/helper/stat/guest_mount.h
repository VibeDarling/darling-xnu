#ifndef DARLING_GUEST_MOUNT_H
#define DARLING_GUEST_MOUNT_H

// The guest sees a path tree, not the host's mount table, but /proc/self/mounts lists
// host mount points. This header maps them into what a guest process can name:
//  - the mount that holds the guest root (the prefix's "/") is the root volume and is
//    reported as "/", as on Darwin;
//  - mounts below the guest root keep their guest path;
//  - every other host mount lies outside the guest tree: statfs names it below
//    /Volumes/SystemRoot, where the guest can reach it, and getfsstat does not list it.

#include <sys/fcntl.h>

#include <darling/emulation/conversion/stat/common.h>
#include <darling/emulation/conversion/errno.h>
#include <darling/emulation/common/simple.h>
#include <darling/emulation/xnu_syscall/bsd/impl/fcntl/open.h>
#include <darling/emulation/xnu_syscall/bsd/impl/unistd/close.h>
#include <darling/emulation/linux_premigration/vchroot_expand.h>

// /proc/self/mounts is read in several passes and is not a snapshot: a mount that appears
// or goes away in between is seen by one pass and not the other. The helpers below keep
// about 8 KiB of the callers' stack (the view and one path), getfsstat about 6 KiB more.

extern __SIZE_TYPE__ strlen(const char* s);
extern int strncmp(const char* a, const char* b, __SIZE_TYPE__ n);
extern char* strcpy(char* dst, const char* src);
extern char* strtok_r(char* str, const char* delim, char** saveptr);
extern __SIZE_TYPE__ strlcpy(char* dst, const char* src, __SIZE_TYPE__ size);

struct guest_mount_view {
	char root[4096]; // host path of the guest's "/", no trailing slash
	long root_volume_length; // longest mount point holding root, -1 until one is seen
};

// Whether dir is the mount point or lies below it (whole path components).
static int guest_mount_contains(const char* mount, const char* dir)
{
	__SIZE_TYPE__ length = strlen(mount);
	if (length == 1 && mount[0] == '/')
		return 1;
	return strncmp(dir, mount, length) == 0 && (dir[length] == '\0' || dir[length] == '/');
}

// Returns 0 or a negative errno.
static int guest_mount_init(struct guest_mount_view* view)
{
	struct vchroot_expand_args args;
	args.flags = VCHROOT_FOLLOW;
	args.dfd = -100;
	strcpy(args.path, "/");
	int rv = vchroot_expand(&args);
	if (rv < 0)
		return rv;
	__SIZE_TYPE__ length = strlen(args.path);
	while (length > 1 && args.path[length - 1] == '/')
		args.path[--length] = '\0';
	strcpy(view->root, args.path);
	view->root_volume_length = -1;
	return 0;
}

// Feed every host mount point of /proc/self/mounts through this before naming any of them.
static void guest_mount_note(struct guest_mount_view* view, const char* mount)
{
	if (guest_mount_contains(mount, view->root) && (long)strlen(mount) >= view->root_volume_length)
		view->root_volume_length = (long)strlen(mount);
}

// The mount that holds the guest root. When several mounts share the deepest mount point
// the last one is on top, so callers keep the last match.
static int guest_mount_is_root_volume(const struct guest_mount_view* view, const char* mount)
{
	return guest_mount_contains(mount, view->root) && (long)strlen(mount) == view->root_volume_length;
}

// Name under which a guest sees host mounts that lie outside the guest root: the host's
// whole tree is reachable below it (the vchroot "exit path").
#define GUEST_MOUNT_HOST_ROOT "/Volumes/SystemRoot"

// Whether a host mount point is below the guest root, not the root itself (whole components).
static int guest_mount_below_root(const struct guest_mount_view* view, const char* mount)
{
	__SIZE_TYPE__ length = strlen(view->root);
	return guest_mount_contains(view->root, mount) && mount[length] == '/';
}

// Guest name of a host mount point, and whether getfsstat lists it (root volume or below the guest root).
static int guest_mount_name(const struct guest_mount_view* view, const char* mount, char* out, int* listed)
{
	if (guest_mount_is_root_volume(view, mount)) {
		strcpy(out, "/");
		*listed = 1;
	} else if (guest_mount_below_root(view, mount)) {
		strcpy(out, mount + strlen(view->root));
		*listed = 1;
	} else {
		strcpy(out, GUEST_MOUNT_HOST_ROOT);
		if (!(mount[0] == '/' && mount[1] == '\0'))
			strcpy(out + strlen(GUEST_MOUNT_HOST_ROOT), mount);
		*listed = 0;
	}
	return 0;
}

// Fills f_mntonname, f_mntfromname and f_fstypename of buf for the mount that holds host_path
// (a host path; the longest matching mount point, the last one when several share it), naming
// it as the guest does. Shared by statfs and fstatfs so both agree with getfsstat.
static int guest_mount_describe(const char* host_path, struct bsd_statfs64* buf)
{
	struct guest_mount_view view;
	struct simple_readline_buf rbuf;
	char line[512];
	int max_len = 0;

	int rv = guest_mount_init(&view);
	if (rv < 0)
		return errno_linux_to_bsd(rv);

	int fd = sys_open("/proc/self/mounts", O_RDONLY, 0);
	if (fd < 0)
		return errno_linux_to_bsd(fd);
	__simple_readline_init(&rbuf);

	buf->f_mntonname[0] = 0;
	buf->f_fstypename[0] = 0;
	buf->f_mntfromname[0] = 0;

	while (__simple_readline(fd, &rbuf, line, sizeof(line)))
	{
		char* saveptr;
		char* from = strtok_r(line, " ", &saveptr);
		if (from == NULL)
			continue;
		char* on = strtok_r(NULL, " ", &saveptr);
		if (on == NULL)
			continue;

		guest_mount_note(&view, on);

		// Whole path components: "/home/u/prefix2" is not below "/home/u/prefix".
		int len = strlen(on);
		if (!guest_mount_contains(on, host_path) || len < max_len)
			continue;

		char* type = strtok_r(NULL, " ", &saveptr);
		if (type == NULL)
			continue;

		max_len = len;
		strlcpy(buf->f_mntonname, on, sizeof(buf->f_mntonname));
		strlcpy(buf->f_fstypename, type, sizeof(buf->f_fstypename));
		strlcpy(buf->f_mntfromname, from, sizeof(buf->f_mntfromname));
	}
	close_internal(fd);

	if (buf->f_mntonname[0] != 0)
	{
		char name[sizeof(buf->f_mntonname)];
		int listed;

		if (guest_mount_name(&view, buf->f_mntonname, name, &listed) == 0)
			strlcpy(buf->f_mntonname, name, sizeof(buf->f_mntonname));
	}
	return 0;
}

#endif // DARLING_GUEST_MOUNT_H
