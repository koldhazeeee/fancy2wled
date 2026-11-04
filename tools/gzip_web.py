"""Compresses the setup page's files (web/*) into web_gz/*.gz, which are what the firmware embeds and sends (Content-Encoding: gzip).
A 62 KB page becomes about 19 KB: it loads about three times faster over the board's Wi-Fi and takes less flash.

Runs as a PlatformIO pre-script (see platformio.ini), so a normal `pio run` always embeds the current files. It can also run on its own:
    python tools/gzip_web.py
The output is reproducible (no timestamp in the gzip header), a file is only rewritten when its content changed, and every file is
decompressed again and compared with the original before it is accepted.
"""
import gzip
import io
import os


def compress(data):
    buf = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=buf, compresslevel=9, mtime=0) as f:
        f.write(data)
    out = buf.getvalue()
    if gzip.decompress(out) != data:
        raise RuntimeError("gzip round trip failed")
    return out


def build(project_dir):
    """web/<name> -> web_gz/<name>.gz for every file; returns [(name, raw bytes, compressed bytes)]."""
    src = os.path.join(project_dir, "web")
    dst = os.path.join(project_dir, "web_gz")
    os.makedirs(dst, exist_ok=True)
    wanted = set()
    result = []
    for name in sorted(os.listdir(src)):
        path = os.path.join(src, name)
        if not os.path.isfile(path):
            continue
        with open(path, "rb") as f:
            raw = f.read()
        gz = compress(raw)
        target = os.path.join(dst, name + ".gz")
        wanted.add(name + ".gz")
        old = None
        if os.path.exists(target):
            with open(target, "rb") as f:
                old = f.read()
        if old != gz:
            with open(target, "wb") as f:
                f.write(gz)
        result.append((name, len(raw), len(gz)))
    for name in os.listdir(dst):                      # a web file that was removed or renamed
        if name.endswith(".gz") and name not in wanted:
            os.remove(os.path.join(dst, name))
    return result


try:
    Import("env")                                      # PlatformIO (SCons) provides this
    build(env.subst("$PROJECT_DIR"))                   # noqa: F821
except NameError:
    if __name__ == "__main__":
        for n, a, b in build(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")):
            print("%-16s %7d -> %6d bytes" % (n, a, b))
