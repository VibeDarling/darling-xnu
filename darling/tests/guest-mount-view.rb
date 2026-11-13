# Native Linux test of the actual getfsstat64, statfs64 and fstatfs64 emulation: the guest must see its own volume
# view. The root volume is the mount holding the guest root and is named "/" (as on Darwin), mounts below
# the guest root keep their guest path, and other host mounts are not listed by getfsstat.
# Usage: ruby darling/tests/guest-mount-view.rb [path/to/getfsstat.c path/to/statfs.c [path/to/fstatfs.c]]   (older copies are the negative control)
require 'tmpdir'
repo = File.expand_path('../..', __dir__)
emulation = File.join(repo, 'darling/src/libsystem_kernel/emulation')
impl = File.join(emulation, 'src/xnu_syscall/bsd/impl/stat')
strip = ->(text) { text.lines.reject { |line| line.start_with?('#include') }.join }
getfsstat = strip.call(File.read(ARGV[0] || File.join(impl, 'getfsstat.c')))
statfs = strip.call(File.read(ARGV[1] || File.join(impl, 'statfs.c')))
fstatfs = strip.call(File.read(ARGV[2] || File.join(impl, 'fstatfs.c')))
helper = strip.call(File.read(File.join(emulation, 'include/xnu_syscall/bsd/helper/stat/guest_mount.h')))
common_h = File.read(File.join(emulation, 'include/conversion/stat/common.h'))
bsd_statfs64 = common_h[/^struct bsd_statfs64\n\{.*?^\};/m] or abort 'struct bsd_statfs64 not found'
linux_statfs64 = common_h[/^struct linux_statfs64\n\{.*?^\};/m] or abort 'struct linux_statfs64 not found'
# The real converter, so the test sees what the emulation does to a buffer it is given.
convert = File.read(File.join(emulation, 'src/conversion/stat/common.c'))[/^void statfs_linux_to_bsd64\(.*?^\}/m] or abort 'converter not found'
program = <<~C
  #include <assert.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <stdint.h>
  #include <stdio.h>
  #include <stdlib.h>
  #include <string.h>
  #include <sys/types.h>
  #include <sys/syscall.h>
  #include <unistd.h>
  #{bsd_statfs64}
  #{linux_statfs64}
  static int statfs_flags_linux_to_bsd(long flags) { return (int)flags; }
  #{convert}
  struct simple_readline_buf { int unused; };
  struct vchroot_expand_args { char path[4096]; unsigned int flags; int dfd; };
  struct vchroot_unexpand_args { char path[4096]; };
  #define VCHROOT_FOLLOW 1
  #define LINUX_SYSCALL2(nr, a, b) mock_statfs(nr, (long)(a), (struct linux_statfs64 *)(b))
  #define LINUX_SYSCALL3(nr, a, b, c) 0
  #define LINUX_SYSCALL4(nr, a, b, c, d) mock_readlink((const char *)(b), (char *)(c))
  #define __simple_sprintf sprintf
  static int mock_statfs(long nr, long a, struct linux_statfs64 *l) { if (nr != __NR_fstatfs && strstr((const char *)a, "broken")) return -2; /* a mount that cannot be queried */ memset(l, 0, sizeof *l); l->f_bsize = 4096; return 0; }
  static long mock_readlink(const char *proc, char *out) { /* fd 3 and fd 4 as the host sees them */
    const char *target = !strcmp(proc, "/proc/self/fd/3") ? "/home/u/prefix/proc/1" : !strcmp(proc, "/proc/self/fd/4") ? "/home/u/prefix2/x" : "/etc";
    strcpy(out, target); return (long)strlen(target); }
  #define close_internal close
  #define errno_linux_to_bsd(error) (error)
  #define get_perthread_wd() (-100)
  #define sys_open(path, flags, mode) open(getenv("MOUNTS"), flags)
  static void __simple_readline_init(struct simple_readline_buf *b) { (void)b; }
  static char *__simple_readline(int fd, struct simple_readline_buf *b, char *line, size_t size) {
    (void)b; size_t n = 0; char c;
    while (n + 1 < size && read(fd, &c, 1) == 1) { if (c == '\\n') { line[n] = 0; return line; } line[n++] = c; }
    line[n] = 0; return n ? line : NULL;
  }
  static int vchroot_expand(struct vchroot_expand_args *a) { if (!strcmp(a->path, "/")) strcpy(a->path, "/home/u/prefix/"); return 0; }
  #{helper}
  #{getfsstat}
  #{statfs}
  #{fstatfs}
  static struct bsd_statfs64 entries[16];
  static int find(int count, const char *on) { for (int i = 0; i < count; i++) if (!strcmp(entries[i].f_mntonname, on)) return i; return -1; }
  static const char *on_for(const char *path) {
    static struct bsd_statfs64 s; memset(&s, 0xa5, sizeof s);
    assert(sys_statfs64(path, &s) == 0); return s.f_mntonname;
  }
  static long fstatfs_on(int fd, char *on) { struct bsd_statfs64 s; memset(&s, 0xa5, sizeof s); long r = sys_fstatfs64(fd, &s); strcpy(on, s.f_mntonname); return r; }
  int main(void) {
    /* /proc/self/mounts: root volume twice at one mount point (the last wins), procfs below the guest root,
       a mount that cannot be queried, a sibling directory whose name starts like the guest root. */
    assert(sys_getfsstat64(NULL, 0, 0) == 2);
    memset(entries, 0xa5, sizeof entries);
    long n = sys_getfsstat64(entries, sizeof entries, 0);
    assert(n == 2);
    int root = find(n, "/"), proc = find(n, "/proc");
    assert(root >= 0 && proc >= 0);
    assert(!strcmp(entries[root].f_mntfromname, "overlay2") && !strcmp(entries[root].f_fstypename, "overlay"));
    assert(!strcmp(entries[proc].f_mntfromname, "proc") && !strcmp(entries[proc].f_fstypename, "proc"));
    assert(entries[root].f_bsize == 4096 && entries[root].f_owner == 0); /* the entry is cleared before it is filled */
    assert(entries[2].f_bsize == (uint32_t)0xa5a5a5a5); /* nothing written past the listed volumes */
    /* A buffer for one entry gets one entry; the count with and without a buffer agrees. */
    assert(sys_getfsstat64(entries, sizeof entries[0], 0) == 1);
    assert(!strcmp(on_for("/home/u/prefix/Applications"), "/"));
    assert(!strcmp(on_for("/home/u/prefix/proc/1"), "/proc"));
    assert(!strcmp(on_for("/home/other"), "/Volumes/SystemRoot/home")); /* a shallower mount holding the root is not the root volume */
    assert(!strcmp(on_for("/home/u/prefix2/x"), "/Volumes/SystemRoot/home/u/prefix2")); /* whole components: not below the guest root */
    /* A path with no mount of its own must not be taken for one below the overlay: "/home/u/prefixfoo" is not "/home/u/prefix" plus a component. */
    assert(!strcmp(on_for("/home/u/prefixfoo/y"), "/Volumes/SystemRoot/home"));
    assert(!strcmp(on_for("/tmp/x"), "/Volumes/SystemRoot/tmp"));
    assert(!strcmp(on_for("/etc/hosts"), "/Volumes/SystemRoot")); /* host "/" */
    /* fstatfs agrees with statfs for the same objects. */
    char on[1024];
    assert(fstatfs_on(3, on) == 0 && !strcmp(on, "/proc"));
    assert(fstatfs_on(4, on) == 0 && !strcmp(on, "/Volumes/SystemRoot/home/u/prefix2"));
    assert(fstatfs_on(5, on) == 0 && !strcmp(on, "/Volumes/SystemRoot"));
    puts("PASS: root volume is /, below-root mounts keep guest paths, host mounts are hidden from getfsstat, fstatfs agrees");
    return 0;
  }
C
Dir.mktmpdir('guest-mount-view') do |dir|
  File.write(File.join(dir, 'mounts'), <<~M)
    /dev/sda1 / ext4 rw 0 0
    tmpfs /dev/shm tmpfs rw 0 0
    /dev/sdb1 /home btrfs rw 0 0
    overlay /home/u/prefix overlay rw 0 0
    proc /home/u/prefix/proc proc rw 0 0
    tmpfs /home/u/prefix/broken tmpfs rw 0 0
    /dev/sdd1 /home/u/prefix2 ext4 rw 0 0
    tmpfs /tmp tmpfs rw 0 0
    overlay2 /home/u/prefix overlay rw 0 0
  M
  File.write(File.join(dir, 'test.c'), program)
  abort 'compile failed' unless system(ENV.fetch('CC', 'clang'), '-std=gnu11', '-Wall', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', File.join(dir, 'test.c'), '-o', File.join(dir, 'test'))
  abort 'test failed' unless system({ 'MOUNTS' => File.join(dir, 'mounts') }, File.join(dir, 'test'), rlimit_core: 0)
end
