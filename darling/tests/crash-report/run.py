#!/usr/bin/env python3
"""Run the crash report tests in a Darling guest.

  DPREFIX=<prefix> run.py --build BUILD_DIR --src SUPERPROJECT_DIR --install INSTALL_PREFIX \
                          [--baseline INSTALL_PREFIX] [--out DIR]

Builds crash_report_test.c with the build tree's toolchain, then runs every case in one guest boot with
DARLING_CRASH_REPORT=1 and with it unset, and checks:

  * each report against the EXPECT lines the program printed before crashing (computed with dladdr, so the
    oracle does not share code with the reporter);
  * the guardrails: the block has exactly the lines process, signal, fault, pc, lr in that order, each of a fixed
    shape; pc and lr equal the oracle's answer in every case (an image path or a fixed phrase, nothing appended); no
    number of 6 or more digits and no hex address outside the process line; a NULL-relative offset below 64 KiB;
    nothing printed when the mode is off, the program has its own handler, or it is killed by SIGTERM; one complete
    block when two threads fault at once (the race case runs 8 times);
  * that the checker itself rejects doctored reports (--self-test runs only that, with no guest);
  * with --baseline, that the mode-off output (stdout, stderr, exit status) of every case is byte-identical
    on INSTALL_PREFIX and on the baseline prefix, and that the mode-on exit status equals the mode-off one.

Run it under the guest-slot lock; it ends every boot with `darling shutdown`. Exit status 0 only if all pass.
"""
import argparse
import os
import re
import shlex
import subprocess
import sys

RACE_RUNS = 8   # the race case is run in separate processes: which thread wins, and whether the loser cuts the report short, varies
CASES = ["null", "abort", "trap", "wild", "highbits", "library", "anon", "ill", "rodata", "thread"] + ["race%d" % i for i in range(RACE_RUNS)] + ["term", "handler"]
KEYS = ["process", "signal", "fault", "pc", "lr"]
BEGIN = "=== darling crash report v1 ==="
END = "=== end darling crash report ==="
HERE = os.path.dirname(os.path.abspath(__file__))


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def build_test(build, src, out):
    clang = "/usr/bin/clang"
    resource = sh([clang, "-print-resource-dir"]).stdout.strip()
    sdk = os.path.join(src, "Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include")
    obj = os.path.join(out, "crash_report_test.o")
    r = sh([clang, "-target", "aarch64-apple-darwin20", "-arch", "arm64", "-mmacosx-version-min=11.0", "-O0", "-nostdinc",
            "-isystem", resource + "/include", "-isystem", sdk, "-c", os.path.join(HERE, "crash_report_test.c"), "-o", obj])
    if r.returncode:
        sys.exit("compile failed:\n" + r.stderr[-3000:])
    link = sh(["ninja", "-C", build, "-t", "commands", "src/external/foundation/Foundation"]).stdout.strip().splitlines()[-1]
    dylib_files = [a for a in shlex.split(link) if a.startswith("-Wl,-dylib_file,")]
    exe = os.path.join(out, "crash_report_test")
    ld = os.path.join(build, "src/external/cctools-port/cctools/ld64/src/aarch64-apple-darwin20-ld")
    r = sh([clang, "-target", "aarch64-apple-darwin20", "-arch", "arm64", "-nostdlib", "-mmacosx-version-min=11.0",
            "-fuse-ld=" + ld, os.path.join(build, "src/external/csu/CMakeFiles/crt1.10.6.dir/start.S.o"), obj,
            os.path.join(build, "src/external/libsystem/libSystem.B.dylib"), *dylib_files, "-o", exe], cwd=build)
    if r.returncode:
        sys.exit("link failed:\n" + r.stderr[-3000:])
    return exe


def write_guest_script(path, exe, outdir):
    lines = ["#!/bin/bash", "ulimit -c 0", "cd /tmp"]
    for mode in ("on", "off"):
        for case in CASES:
            base = "/Volumes/SystemRoot%s/%s.%s" % (outdir, case, mode)
            env = "env DARLING_CRASH_REPORT=1 " if mode == "on" else "env "
            arg = re.sub(r"\d+$", "", case)   # race0..raceN are runs of the one "race" case
            lines.append("%s/Volumes/SystemRoot%s %s > %s.out 2> %s.err" % (env, exe, arg, base, base))
            lines.append("echo $? > %s.rc" % base)
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def run_guest(install, script, log, timeout=600):
    env = dict(os.environ, DARLING_INSTALL_PREFIX=install)
    launcher = os.path.join(install, "bin/darling")
    with open(log, "w") as lf:
        subprocess.run(["timeout", str(timeout), launcher, "shell", "/bin/bash", "/Volumes/SystemRoot" + script],
                       env=env, stdout=lf, stderr=subprocess.STDOUT)
        subprocess.run(["timeout", "120", launcher, "shutdown"], env=env, stdout=lf, stderr=subprocess.STDOUT)


def read(outdir, case, mode, ext, prefix=None):
    with open(os.path.join(outdir, "%s.%s.%s" % (case, mode, ext)), "rb") as f:
        text = f.read().decode("utf-8", "replace")
    # image paths printed by the program contain the install prefix, which differs between the two runs
    return text.replace(prefix, "<PREFIX>") if prefix else text


def check_report(case, out, err, rc, failures):
    def fail(msg):
        failures.append("%s on: %s" % (case, msg))

    expect = {}
    for line in out.splitlines():
        m = re.match(r"EXPECT (\S+) (.*)$", line)
        if m:
            expect[m.group(1)] = m.group(2)

    if case in ("handler", "term"):
        if BEGIN in err:
            fail("a report was printed although this is not a crash the process dies of")
        if case == "handler" and "handled" not in out:
            fail("the program's own handler did not run")
        if case == "term" and rc.strip() != "143":
            fail("the SIGTERM did not kill the process (exit status %s)" % rc.strip())
        return
    if err.count(BEGIN) != 1 or err.count(END) != 1:
        fail("expected exactly one report, got %d begin / %d end markers" % (err.count(BEGIN), err.count(END)))
        return
    block = err[err.index(BEGIN):err.index(END) + len(END)]
    body = block.splitlines()[1:-1]
    keys = [l.partition(": ")[0] for l in body]
    if keys != KEYS:
        fail("the block has the lines %r, expected exactly %r in this order" % (keys, KEYS))
        return
    fields = {l.partition(": ")[0]: l.partition(": ")[2] for l in body}

    shapes = {
        "process": r"pid \d+ tid \d+",
        "signal": r"SIG[A-Z]+ \(\d+\) si_code -?\d+( \([A-Z_]+\))?",
        "fault": r"n/a|not mapped|mapped|mapped, access not permitted|high bits set \(not a canonical address\)"
                 r"|unknown \(memory map unavailable\)|null-relative, offset \d{1,5}",
    }
    for key, shape in shapes.items():
        if not re.fullmatch(shape, fields[key]):
            fail("the %s line has an unexpected shape: %r" % (key, fields[key]))
    # only the process line may carry a long number; an address in decimal or hex anywhere else is a leak
    rest = "\n".join(l for l in body if not l.startswith("process: "))
    if re.search(r"\d{6,}", rest) or re.search(r"0x[0-9a-fA-F]{5,}", block) or "+0x" in block:
        fail("a long number or hex address in the block outside the process line")
    m = re.fullmatch(r"null-relative, offset (\d+)", fields["fault"])
    if m and int(m.group(1)) >= 0x10000:
        fail("null-relative offset 64 KiB or more")
    # pc and lr must equal what the oracle said, in every case: an image path or a fixed phrase, nothing appended
    for key in ("pc", "lr"):
        want = expect.get(key + "-image", expect.get(key))
        if want is None:
            fail("the program printed no expectation for %s" % key)
        elif fields[key] != want:
            fail("%s %r, expected %r" % (key, fields[key], want))

    if "signal" in expect and not fields.get("signal", "").startswith(expect["signal"] + " ("):
        fail("signal %r, expected %s" % (fields.get("signal"), expect["signal"]))
    if "fault" in expect and fields.get("fault") != expect["fault"]:
        fail("fault %r, expected %r" % (fields.get("fault"), expect["fault"]))
    if expect.get("tid") == "differs":
        m = re.match(r"pid (\d+) tid (\d+)$", fields.get("process", ""))
        if not m or m.group(1) == m.group(2):
            fail("tid equals pid on a secondary thread: %r" % fields.get("process"))
    if not expect:
        fail("the program printed no EXPECT lines")


def self_test():
    """The checker must reject doctored reports: a decimal pc, an image with an offset, a symbol with an offset,
    an extra register line, a long number in any field, a missing or reordered line."""
    out = "EXPECT signal SIGSEGV\nEXPECT fault null-relative, offset 8\nEXPECT pc-image /bin/prog\nEXPECT lr-image /bin/prog\n"
    good = ["process: pid 4242 tid 4242", "signal: SIGSEGV (11) si_code 1 (SEGV_MAPERR)", "fault: null-relative, offset 8",
            "pc: /bin/prog", "lr: /bin/prog"]

    def verdict(lines):
        failures = []
        check_report("null", out, "\n".join([BEGIN] + lines + [END]) + "\n", "139\n", failures)
        return failures

    problems = []
    if verdict(good):
        problems.append("the checker rejects a good report: %r" % verdict(good))
    doctored = {
        "decimal pc": good[:3] + ["pc: /bin/prog 281474976710655", good[4]],
        "image plus hex offset": good[:3] + ["pc: /bin/prog+0x1234", good[4]],
        "image plus decimal offset": good[:3] + ["pc: /bin/prog + 4660", good[4]],
        "symbol plus offset": good[:3] + ["pc: /bin/prog (main+8)", good[4]],
        "wrong lr": good[:4] + ["lr: /bin/other"],
        "extra register line": good + ["sp: 281473000000000"],
        "long number in the fault line": good[:2] + ["fault: null-relative, offset 8 at 1234567", good[3], good[4]],
        "missing lr": good[:4],
        "reordered": [good[0], good[2], good[1], good[3], good[4]],
        "hex address": good[:3] + ["pc: /bin/prog 0x1c0001234", good[4]],
    }
    for name, lines in doctored.items():
        if not verdict(lines):
            problems.append("the checker accepts a doctored report: %s" % name)
    for p in problems:
        print("SELF-TEST FAIL", p)
    return not problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true", help="only check that the checker rejects doctored reports")
    ap.add_argument("--build")
    ap.add_argument("--src")
    ap.add_argument("--install")
    ap.add_argument("--baseline")
    ap.add_argument("--out", default=os.path.join(HERE, "out"))
    a = ap.parse_args()
    if not self_test():
        sys.exit(1)
    if a.self_test:
        print("self-test ok")
        return
    if not (a.build and a.src and a.install):
        sys.exit("--build, --src and --install are required")
    if not os.environ.get("DPREFIX"):
        sys.exit("DPREFIX is not set: run under darling-slot.sh")
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    exe = build_test(a.build, a.src, out)

    runs = {"new": a.install}
    if a.baseline:
        runs["base"] = a.baseline
    dirs = {}
    for label, install in runs.items():
        d = os.path.join(out, label)
        os.makedirs(d, exist_ok=True)
        script = os.path.join(out, "guest-%s.sh" % label)
        write_guest_script(script, exe, d)
        run_guest(install, script, os.path.join(d, "guest.log"))
        dirs[label] = d

    failures = []
    for case in CASES:
        try:
            on = (read(dirs["new"], case, "on", "out"), read(dirs["new"], case, "on", "err"), read(dirs["new"], case, "on", "rc"))
            off = tuple(read(dirs["new"], case, "off", e, a.install) for e in ("out", "err", "rc"))
        except FileNotFoundError as e:
            failures.append("%s: the guest did not produce %s" % (case, e.filename))
            continue
        check_report(case, on[0], on[1], on[2], failures)
        if BEGIN in off[1]:
            failures.append("%s off: a report was printed with the mode off" % case)
        if on[2] != off[2]:
            failures.append("%s: exit status %s with the mode on, %s off" % (case, on[2].strip(), off[2].strip()))
        if "base" in dirs:
            base = tuple(read(dirs["base"], case, "off", e, a.baseline) for e in ("out", "err", "rc"))
            if base != off:
                failures.append("%s off: output differs from the baseline prefix" % case)
        print("%-9s rc=%s %s" % (case, off[2].strip(), "FAIL" if any(f.startswith(case) for f in failures) else "ok"))
    for f in failures:
        print("FAIL", f)
    print("crash-report: %s" % ("FAILED" if failures else "all cases ok"))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
