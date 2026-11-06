#!/usr/bin/env python3
"""Compile the firmware core for the PC and test it against the golden vectors.

    python test/run_core_tests.py

Needs a C++ compiler: uses g++/clang++ if present, otherwise `pip install ziglang` (python -m ziglang c++).

The core parses data from the network (the Sync Box's frames, host names, uploaded firmware), so the test also feeds it thousands of random and damaged inputs in
exactly sized buffers and runs under sanitizers: AddressSanitizer + UndefinedBehaviorSanitizer with g++ / clang++ (any read past the end of an input, or undefined
behaviour, stops the test); with zig only UndefinedBehaviorSanitizer is available. `--no-sanitize` turns them off (for a compiler that cannot).
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def compiler():
    """(command, sanitizer flags): the sanitizers the compiler can do."""
    for name in ("g++", "clang++", "c++"):
        if shutil.which(name):
            return [name], ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer", "-g"]
    try:
        import ziglang  # noqa: F401
        return [sys.executable, "-m", "ziglang", "c++"], ["-fsanitize=undefined", "-fno-sanitize-recover=undefined"]      # zig cannot link AddressSanitizer
    except ImportError:
        sys.exit("No C++ compiler found. Install one, or: pip install ziglang")


def main():
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "make_golden.py")])
    exe = os.path.join(ROOT, "test", "test_core.exe" if os.name == "nt" else "test_core")
    cc, san = compiler()
    if "--no-sanitize" in sys.argv:
        san = []
    # -ffp-contract=off: no fused multiply-adds. core.cpp is forced to -O3 by a pragma and the tests are -O1; GCC fuses a * b + c into one instruction (one rounding) at -O3 but
    # not at -O1, so the optimised functions and the originals in test/legacy_core.h would differ by one unit in the last place for reasons that have nothing to do with what
    # they compute. With fusing off, "bit-identical" means the arithmetic is identical.
    cmd = cc + ["-std=c++11", "-O1", "-ffp-contract=off", "-Wall", "-Wextra", "-Wno-nullability-completeness"] + san + ["-I", os.path.join(ROOT, "include"),
                os.path.join(ROOT, "src", "core.cpp"), os.path.join(ROOT, "test", "test_core.cpp"), "-o", exe]
    subprocess.check_call(cmd)
    print("sanitizers: %s" % (" ".join(f for f in san if f.startswith("-fsanitize=")) or "none"))
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0", UBSAN_OPTIONS="print_stacktrace=1")      # leaks are not what this checks
    try:
        rc = subprocess.call([exe, os.path.join(ROOT, "test", "golden.txt")], cwd=ROOT, env=env)
    finally:
        for ext in ('', '.pdb', '.lib', '.ilk'):          # Windows builds leave debug files next to the exe
            for p in (exe + ext, os.path.splitext(exe)[0] + ext):
                if os.path.exists(p) and p != os.path.join(ROOT, 'test', 'golden.txt'):
                    os.remove(p)
        lib = os.path.join(ROOT, 'test', 'core.lib')
        if os.path.exists(lib):
            os.remove(lib)
    sys.exit(rc)


if __name__ == "__main__":
    main()
