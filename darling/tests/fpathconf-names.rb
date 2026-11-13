# Native Linux test of the actual sys_fpathconf: every _PC_* name that xnu defines in
# bsd/sys/unistd.h (except the filesystem specific _PC_MIN_HOLE_SIZE) must be answered,
# with the constants vn_pathconf() in bsd/vfs/vfs_vnops.c returns for all filesystems.
# Usage: ruby darling/tests/fpathconf-names.rb [path/to/fpathconf.c]   (an older copy is the negative control)
require 'tmpdir'
require 'open3'
repo = File.expand_path('../..', __dir__)
source = File.read(ARGV[0] || File.join(repo, 'darling/src/libsystem_kernel/emulation/src/xnu_syscall/bsd/impl/conf/fpathconf.c'))
source = source.lines.reject { |line| line.start_with?('#include') }.join
unistd = File.read(File.join(repo, 'bsd/sys/unistd.h'))
numbers = unistd.scan(/^#define\s+(_PC_[A-Z0-9_]+)\s+(\d+)/).to_h { |name, value| [name, value.to_i] }
abort 'no _PC_ names found' if numbers.size < 27
# Expected values: the xnu constants, then the answers documented in fpathconf.c (case insensitive because the
# vchroot lookup falls back to case-insensitive matching; xattr bits are xnu's AD_XATTR_SIZE_BITS).
expected = {
  '_PC_LINK_MAX' => 8, '_PC_MAX_CANON' => 255, '_PC_MAX_INPUT' => 255, '_PC_NAME_MAX' => 255,
  '_PC_PATH_MAX' => 4096, '_PC_PIPE_BUF' => 4096, '_PC_CHOWN_RESTRICTED' => 1, '_PC_NO_TRUNC' => 1,
  '_PC_VDISABLE' => 0, '_PC_NAME_CHARS_MAX' => 255, '_PC_CASE_SENSITIVE' => 0, '_PC_CASE_PRESERVING' => 1,
  '_PC_EXTENDED_SECURITY_NP' => 0, '_PC_AUTH_OPAQUE_NP' => 0, '_PC_2_SYMLINKS' => 1, '_PC_ALLOC_SIZE_MIN' => 1,
  '_PC_ASYNC_IO' => 1, '_PC_FILESIZEBITS' => 64, '_PC_PRIO_IO' => 0, '_PC_REC_INCR_XFER_SIZE' => 4096,
  '_PC_REC_MAX_XFER_SIZE' => 65536, '_PC_REC_MIN_XFER_SIZE' => 4096, '_PC_REC_XFER_ALIGN' => 4096,
  '_PC_SYMLINK_MAX' => 255, '_PC_SYNC_IO' => 0, '_PC_XATTR_SIZE_BITS' => 31
}
missing = expected.keys - numbers.keys
abort "names missing from unistd.h: #{missing}" unless missing.empty?
checks = expected.map { |name, value| "  check(#{numbers[name]}, #{value}, \"#{name}\");" }.join("\n")
program = <<~C
  #include <errno.h>
  #include <stdio.h>
  static int failures;
  #{source}
  static void check(int name, long want, const char *label) {
    long got = sys_fpathconf(-1, name);
    if (got != want) { printf("FAIL %s (%d): got %ld, want %ld\\n", label, name, got, want); failures++; }
  }
  int main(void) {
  #{checks}
    check(0, -EINVAL, "name 0");
    check(#{numbers['_PC_MIN_HOLE_SIZE']}, -EINVAL, "_PC_MIN_HOLE_SIZE");
    check(1000, -EINVAL, "unknown name");
    check(-1, -EINVAL, "negative name");
    if (failures) return 1;
    puts("fpathconf names OK");
    return 0;
  }
C
Dir.mktmpdir('fpathconf-names') do |dir|
  File.write(File.join(dir, 'test.c'), program)
  out, status = Open3.capture2e('cc', '-Wall', '-o', File.join(dir, 'test'), File.join(dir, 'test.c'))
  abort out unless status.success?
  out, status = Open3.capture2e(File.join(dir, 'test'))
  puts out
  exit status.exitstatus
end
