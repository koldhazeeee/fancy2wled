"""The build step that compresses the setup page (tools/gzip_web.py): every web file gets a .gz that decompresses to exactly the original,
the output is reproducible, an unchanged file is not rewritten, a removed file's .gz is removed, and the page really gets smaller.
    python test/test_gzip_web.py
"""
import gzip
import importlib.util
import os
import shutil
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
spec = importlib.util.spec_from_file_location("gzip_web", os.path.join(ROOT, "tools", "gzip_web.py"))
gw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gw)

passed = failed = 0


def check(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
    else:
        failed += 1
        print("FAIL: " + name + ((" -- " + str(detail)) if detail else ""))


work = tempfile.mkdtemp(prefix="gzipweb_")
try:
    shutil.copytree(os.path.join(ROOT, "web"), os.path.join(work, "web"))
    names = sorted(n for n in os.listdir(os.path.join(work, "web")) if os.path.isfile(os.path.join(work, "web", n)))
    res = gw.build(work)
    out = os.path.join(work, "web_gz")
    check("every web file got a .gz and nothing else", sorted(os.listdir(out)) == sorted(n + ".gz" for n in names), os.listdir(out))
    for n in names:
        raw = open(os.path.join(work, "web", n), "rb").read()
        gz = open(os.path.join(out, n + ".gz"), "rb").read()
        check(n + ": decompresses to the original", gzip.decompress(gz) == raw)
        check(n + ": has no timestamp in the gzip header (reproducible)", gz[4:8] == b"\0\0\0\0", gz[:10].hex())
    sizes = {n: (a, b) for n, a, b in res}
    check("the setup page is at least 2.5 times smaller", sizes["index.html"][0] / sizes["index.html"][1] > 2.5, sizes["index.html"])

    stamp = {n: os.stat(os.path.join(out, n + ".gz")).st_mtime_ns for n in names}
    first = {n: open(os.path.join(out, n + ".gz"), "rb").read() for n in names}
    gw.build(work)
    check("a second run gives identical bytes", all(open(os.path.join(out, n + ".gz"), "rb").read() == first[n] for n in names))
    check("an unchanged file is not rewritten", all(os.stat(os.path.join(out, n + ".gz")).st_mtime_ns == stamp[n] for n in names))

    with open(os.path.join(work, "web", "style.css"), "ab") as f:
        f.write(b"/* changed */\n")
    gw.build(work)
    check("a changed file is compressed again", gzip.decompress(open(os.path.join(out, "style.css.gz"), "rb").read()).endswith(b"/* changed */\n"))
    check("only the changed file was rewritten", os.stat(os.path.join(out, "index.html.gz")).st_mtime_ns == stamp["index.html"])

    os.remove(os.path.join(work, "web", "screen.html"))
    gw.build(work)
    check("the .gz of a removed web file is removed", not os.path.exists(os.path.join(out, "screen.html.gz")))
finally:
    shutil.rmtree(work, ignore_errors=True)

print("%d checks passed, %d failed" % (passed, failed))
print("ALL PASSED" if not failed else "SOME FAILED")
sys.exit(1 if failed else 0)
