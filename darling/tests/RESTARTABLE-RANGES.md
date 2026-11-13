# Restartable ranges with several threads

`restartable-ranges-guest.c` is a Darwin arm64 or arm64e executable for a Darling guest with the
candidate libsystem_kernel. It registers one range around an assembly spin loop, runs threads in
the loop and checks `task_restartable_ranges_synchronize`:

| phase | what it shows |
|---|---|
| `n` | one thread: success without stopping anything |
| `s` | control: threads inside the range, no synchronize, no restart |
| `p` | four spinners: each is restarted by every call; X13, D8 and the Z flag survive |
| `c` | three threads synchronizing at once do not stop each other |
| `x` | threads exiting while synchronize lists and stops them |
| `b` | sleeping, condvar-, read- and mach_msg-blocked threads are not disturbed and finish afterwards |
| `o` | a thread making `task_threads` calls (out-of-line server replies) is not disturbed by the signals |
| `g` | a signal handler running while the interrupted PC is inside the range: the return restarts it |

Build (arm64; the arm64e variant uses `tests/arm64e-link` of the dyld repository), then run it in an
isolated prefix with `DYLD_LIBRARY_PATH` pointing at a directory holding the candidate
`libsystem_kernel.dylib`:

    clang -target aarch64-apple-darwin20 -isysroot <SDK> -O1 -fno-stack-protector -c restartable-ranges-guest.c
    # link with the in-tree ld64 against the prefix's libSystem
    darling shell restartable-ranges-guest [PHASES [ALARM_SECONDS]]

The test does not prove that `synchronize` waits for every acknowledgement before it returns: a version
that only sent the signals would usually pass too. It prints `PASS restartable-ranges-guest` and exits 0, or `FAIL` lines. A hang ends in a watchdog
line naming the phase. x86_64 is not covered: the spin routine is arm64 assembly.
