# Configure/build native probes from the real emulation CMake preamble.
# Does not configure or run a complete Darling build.
# Usage: ruby darling/tests/product-version.rb [path/to/emulation/CMakeLists.txt]
require 'tmpdir'
require 'open3'

source = ARGV[0] || File.expand_path('../src/libsystem_kernel/emulation/CMakeLists.txt', __dir__)
text = File.read(source)
marker = "# include src/startup for rtsig.h"
abort 'CMake preamble boundary not found' unless text.include?(marker)
preamble = text.split(marker, 2).first

Dir.mktmpdir('product-version') do |dir|
  File.write(File.join(dir, 'CMakeLists.txt'), <<~CMAKE)
    cmake_minimum_required(VERSION 3.16)
    #{preamble}
    add_executable(normal probe.c)
    add_executable(dyld probe.c)
    target_compile_definitions(dyld PRIVATE VARIANT_DYLD)
  CMAKE
  File.write(File.join(dir, 'probe.c'), <<~C)
    #include <stdio.h>
    #include <string.h>
    int main(int argc, char **argv) {
        if (argc != 2 || strcmp(EMULATED_OSPRODUCTVERSION, argv[1]) != 0)
            return 1;
        if (strcmp(EMULATED_RELEASE, "25.6.0") != 0 ||
            strcmp(EMULATED_VERSION, "Darwin Kernel Version 25.6.0") != 0 ||
            strcmp(EMULATED_OSVERSION, "20G1120") != 0 ||
            strcmp(EMULATED_SYSNAME, "Darwin") != 0)
            return 2;
        puts(EMULATED_OSPRODUCTVERSION);
        return 0;
    }
  C
  build = File.join(dir, 'build')
  run = lambda do |*args|
    output, status = Open3.capture2e(*args)
    abort "Command failed: #{args.join(' ')}\n#{output}" unless status.success?
  end
  cache = File.join(build, 'CMakeCache.txt')
  # A build directory configured before the default moved holds the former cache
  # entry; nil leaves the cache as the build left it.
  legacy = lambda do |value|
    next unless value
    lines = File.readlines(cache)
    # A cache entry is its `//` docstring line(s) followed by NAME:TYPE=value.
    while (i = lines.index { |l| l.start_with?('DARLING_EMULATED_OS_PRODUCT_VERSION:') })
      lines.delete_at(i)
      lines.delete_at(i -= 1) while i > 0 && lines[i - 1].start_with?('//')
    end
    lines << "DARLING_EMULATED_OS_PRODUCT_VERSION:STRING=#{value}\n"
    File.write(cache, lines.join)
  end
  override = 'DARLING_EMULATED_OS_PRODUCT_VERSION_OVERRIDE'
  [
    ['26.6', nil, []],
    ['26.5', nil, ["-D#{override}=26.5"]],
    ['26.5', nil, []], # Reconfiguration must retain the cached override.
    ['12.4', nil, ["-D#{override}=12.4"]],
    ['26.6', nil, ["-U#{override}"]],
    ['26.6', '26.0', []], # An old build directory caches the former default and must not pin it.
    ['26.6', nil, []], # and the entry is gone afterwards.
    ['26.5', '26.5', []], # A former explicit override is still honoured, once: it moves to the override,
    ['12.4', nil, ["-D#{override}=12.4"]], # a new override replaces it,
    ['26.6', nil, ["-U#{override}"]], # and removing that restores the default.
    ['12.4', '26.5', ["-D#{override}=12.4"]], # An override given with the former entry wins,
    ['26.6', nil, ["-U#{override}"]]
  ].each do |expected, legacy_value, flags|
    run.call('cmake', '-S', dir, '-B', build) unless File.exist?(cache)
    legacy.call(legacy_value)
    run.call('cmake', '-S', dir, '-B', build, *flags)
    run.call('cmake', '--build', build, '--parallel', '2')
    %w[normal dyld].each { |variant| run.call(File.join(build, variant), expected) }
    puts "PASS: #{expected} in normal/dyld (legacy cache #{legacy_value.inspect}, #{flags.inspect}); other identity definitions unchanged"
  end
end
