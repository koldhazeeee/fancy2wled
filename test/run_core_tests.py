#!/usr/bin/env python3
"""Compile the firmware core for the PC and test it against the golden vectors.

    python test/run_core_tests.py

Needs a C++ compiler: uses g++/clang++ if present, otherwise `pip install ziglang` (python -m ziglang c++).
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def compiler():
    for name in ("g++", "clang++", "c++"):
        if shutil.which(name):
            return [name]
    try:
        import ziglang  # noqa: F401
        return [sys.executable, "-m", "ziglang", "c++"]
    except ImportError:
        sys.exit("No C++ compiler found. Install one, or: pip install ziglang")


def main():
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "make_golden.py")])
    exe = os.path.join(ROOT, "test", "test_core.exe" if os.name == "nt" else "test_core")
    cmd = compiler() + ["-std=c++11", "-O1", "-Wall", "-Wextra", "-Wno-nullability-completeness", "-I", os.path.join(ROOT, "include"),
                        os.path.join(ROOT, "src", "core.cpp"), os.path.join(ROOT, "test", "test_core.cpp"), "-o", exe]
    subprocess.check_call(cmd)
    try:
        rc = subprocess.call([exe, os.path.join(ROOT, "test", "golden.txt")], cwd=ROOT)
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
