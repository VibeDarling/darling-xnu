# Product-version build override

Configure the Darling superproject with
`-DDARLING_EMULATED_OS_PRODUCT_VERSION_OVERRIDE=26.5` to change the string
returned by `kern.osproductversion`, then rebuild and install libsystem_kernel.
Without it the default is `26.6` (`DARLING_DEFAULT_OS_PRODUCT_VERSION`), the
release of the macOS 26.6 applications and images Darling runs; the Swift 6.3
runtime refuses to start below 26.4. The override is a CMake cache setting, not
a runtime environment variable; reconfiguring without the option preserves a
previous override. Use `-UDARLING_EMULATED_OS_PRODUCT_VERSION_OVERRIDE` to
restore the default.

The default is not itself cached, so a build directory configured with an older
default picks up a new one on reconfigure. The option was previously named
`DARLING_EMULATED_OS_PRODUCT_VERSION` with default `26.0`: a cached `26.0` under
that name is ignored with a warning (it is the former default). Any other cached
value is moved once into the override with a warning, so removing the override
later restores the default.

The option supports reproducible version-gating experiments without editing
the source or changing the default for other builders. It does not implement
the APIs of the advertised macOS release. It also does not update the parent
repository's `SystemVersion.plist` or the kernel build identifier
(`kern.osversion`). The default here must equal the plist's `ProductVersion`;
for an override, also run `darling os-version set <version>` (or set
`DARLING_OS_VERSION`), otherwise applications using different version sources
can disagree.

Run `ruby darling/tests/product-version.rb` with Ruby, CMake and a native C/C++
compiler installed. The probe uses the actual CMake preamble and compiles
normal/dyld-configured executables to check defaults, overrides, cache
persistence, reset and an old build directory's cached `26.0`. This is not a
full Darling configure or guest sysctl test and does not claim compatibility
with the selected release.
