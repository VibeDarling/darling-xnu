# Native Linux test of the actual getattrlist helper (both the path and the descriptor instantiation):
# a volume request for ATTR_VOL_UUID must be answered, in the legacy layout and with ATTR_CMN_RETURNED_ATTRS.
# Usage: ruby darling/tests/getattrlist-volume-uuid.rb [path/to/getattrlist_generic.c]   (an older copy is the negative control)
require 'tmpdir'
root = File.expand_path('../src/libsystem_kernel/emulation', __dir__)
source = File.read(ARGV[0] || File.join(root, 'include/xnu_syscall/bsd/helper/xattr/getattrlist_generic.c'))
source = source.lines.reject { |line| line.start_with?('#include') }.join
header = File.read(File.join(root, 'include/conversion/xattr/getattrlist.h'))
helpers = %w[mountpoint.h mountroot.h].map { |name| File.read(File.join(root, 'include/xnu_syscall/bsd/helper/xattr', name)).lines.reject { |line| line.start_with?('#include') }.join }.join
program = <<~C
  #define _GNU_SOURCE
  #include <assert.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <stdint.h>
  #include <stddef.h>
  #include <stdio.h>
  #include <stdlib.h>
  #include <string.h>
  #include <sys/stat.h>
  #include <sys/syscall.h>
  #include <unistd.h>
  #{header}
  #define linux_stat stat
  #define st_ctime_nsec st_ctim.tv_nsec
  #define st_mtime_nsec st_mtim.tv_nsec
  #define st_atime_nsec st_atim.tv_nsec
  #define strrchr declared_by_the_source_strrchr
  #define LINUX_AT_SYMLINK_NOFOLLOW AT_SYMLINK_NOFOLLOW
  #define LINUX_O_RDONLY O_RDONLY
  #define LINUX_O_DIRECTORY O_DIRECTORY
  #define LINUX_O_CLOEXEC O_CLOEXEC
  #define LINUX_O_NOFOLLOW O_NOFOLLOW
  #define VCHROOT_FOLLOW 1
  #define atfd(fd) (fd)
  #define sys_dup dup
  #define close_internal close
  #define errno_linux_to_bsd(error) (error)
  #define __simple_sprintf sprintf
  #define LINUX_SYSCALL(...) ({ long r=syscall(__VA_ARGS__); r<0 ? -errno : r; })
  #define FUNC_NAME test_getattrlist
  struct linux_dirent64 { uint64_t ino; int64_t off; unsigned short d_reclen; unsigned char type; char d_name[]; };
  struct vchroot_expand_args { int flags,dfd; char path[4096]; };
  struct vchroot_fdpath_args { int fd; char *path; unsigned maxlen; };
  static int vchroot_fdpath(struct vchroot_fdpath_args *a) { (void)a; return -ENOTSUP; }
  static int vchroot_expand(struct vchroot_expand_args *v) { (void)v; return 0; }
  #{helpers}
  #{source}
  #define ATTR_VOL_UUID_BIT 0x00040000u
  #define ATTR_VOL_INFO_BIT 0x80000000u
  #define RETURNED_ATTRS_BIT 0x80000000u
  static long volume_call(const char *path, uint32_t common, uint32_t vol, void *out, size_t size) {
    struct xnu_attrlist attrs = {.bitmapcount = 5, .commonattr = common, .volattr = vol};
    int fd = open(path, O_RDONLY | O_DIRECTORY); assert(fd >= 0);
    long rc;
  #if HAS_PATH
    rc = test_getattrlist(-100, path, &attrs, out, size, 0);
  #else
    rc = test_getattrlist(fd, &attrs, out, size, 0);
  #endif
    close(fd);
    return rc;
  }
  /* RFC 9562: version nibble 8, variant 10xx; layout documented in the helper: bytes 0..5 are the low 48 bits of st_dev. */
  static void check_shape(const unsigned char *u, const char *path) {
    struct stat st; assert(stat(path, &st) == 0);
    assert((u[6] >> 4) == 8 && (u[8] & 0xc0) == 0x80);
    for (int i = 0; i < 6; i++) assert(u[i] == (unsigned char)((uint64_t)st.st_dev >> (8 * i)));
  }
  static void legacy_unchecked(const char *path, unsigned char *uuid) {
    unsigned char out[64]; memset(out, 0xa5, sizeof out);
    assert(volume_call(path, 0, ATTR_VOL_UUID_BIT | ATTR_VOL_INFO_BIT, out, sizeof out) == 0);
    memcpy(uuid, out + 4, 16);
  }
  static void legacy(const char *path, unsigned char *uuid) {
    unsigned char out[64]; memset(out, 0xa5, sizeof out);
    assert(volume_call(path, 0, ATTR_VOL_UUID_BIT | ATTR_VOL_INFO_BIT, out, sizeof out) == 0);
    uint32_t length; memcpy(&length, out, 4);
    assert(length == 4 + 16 && out[20] == 0xa5);
    memcpy(uuid, out + 4, 16);
    check_shape(uuid, path);
  }
  /* Legacy layout order: CAPABILITIES (32 bytes), UUID (16), ATTRIBUTES (40), INFO (0). */
  static void ordered(const char *path, const unsigned char *want) {
    unsigned char out[160]; memset(out, 0xa5, sizeof out);
    assert(volume_call(path, 0, 0x00020000u | ATTR_VOL_UUID_BIT | 0x40000000u | ATTR_VOL_INFO_BIT, out, sizeof out) == 0);
    uint32_t length; memcpy(&length, out, 4);
    assert(length == 4 + 32 + 16 + 40 && out[92] == 0xa5);
    assert(memcmp(out + 4 + 32, want, 16) == 0);
  }
  static void returned(const char *path, const unsigned char *want) {
    unsigned char out[128]; memset(out, 0xa5, sizeof out);
    assert(volume_call(path, RETURNED_ATTRS_BIT, ATTR_VOL_UUID_BIT | ATTR_VOL_INFO_BIT, out, sizeof out) == 0);
    uint32_t length, set[5]; memcpy(&length, out, 4); memcpy(set, out + 4, sizeof set);
    assert(set[1] & ATTR_VOL_UUID_BIT);
    assert(length == 4 + sizeof set + 16);
    assert(memcmp(out + 4 + sizeof set, want, 16) == 0);
  }
  int main(void) {
    unsigned char a[16], b[16], again[16];
    legacy("/", a); legacy("/", again); assert(memcmp(a, again, 16) == 0); /* stable while mounted */
    legacy("/proc", b); assert(memcmp(a, b, 16) != 0); /* different mounts differ */
    returned("/", a); returned("/proc", b);
    ordered("/", a); ordered("/proc", b);
    /* A path inside a volume gives the volume root's value, not a value of its own. */
    unsigned char inner[16]; legacy_unchecked("/proc/sys", inner); assert(memcmp(inner, b, 16) == 0);
    puts("PASS: ATTR_VOL_UUID is answered, stable per mount, distinct across mounts, in both layouts");
    return 0;
  }
C
Dir.mktmpdir('getattrlist-volume-uuid') do |dir|
  input = File.join(dir, 'test.c'); output = File.join(dir, 'test')
  File.write(input, program)
  [0, 1].each do |has_path|
    abort 'compile failed' unless system(ENV.fetch('CC', 'clang'), '-std=gnu11', '-Wall', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', "-DHAS_PATH=#{has_path}", input, '-o', output)
    puts "HAS_PATH=#{has_path}"
    abort 'regression failed' unless system(output, rlimit_core: 0)
  end
end
