# -*- coding: utf-8 -*-
"""check_text_encoding.py - reject non-UTF8 / double-encoded (mojibake) text files.

Why: README/_MD_INDEX were once rewritten through PowerShell redirection
(UTF-16/ANSI), which double-encoded Chinese text (e.g. "鈥?" / "锛" / "鍙").
This guard fails the release audit if such files exist again.

Usage: py tools\check_text_encoding.py [root]
Exit: 0 = pass, 1 = problems.
"""
import io, os, sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEXT_EXT = {".md", ".py", ".ps1", ".json", ".txt", ".yml", ".yaml", ".cpp", ".h", ".hpp", ".c"}
# escaped so this guard never flags itself (codepoints of typical GBK-double-encoded sequences)
MARKS = ("\u9225", "\u951b", "\u9458", "\u9531\u65a4\u62f7", "\u9419", "\u9428",
         "\u9286", "\u6490", "\u6d93", "\u59dd", "\u6769", "\u93c4", "\u7f02", "\u935f")
SKIP_DIRS = {".git", "build", "out", "node_modules", "third_party", "_archive"}

def main():
    bad = []
    for dp, dns, fns in os.walk(ROOT):
        dns[:] = [d for d in dns if d not in SKIP_DIRS]
        for fn in fns:
            ext = os.path.splitext(fn)[1].lower()
            if ext not in TEXT_EXT:
                continue
            p = os.path.join(dp, fn)
            rel = os.path.relpath(p, ROOT)
            if rel.replace("\\", "/").endswith("tools/check_text_encoding.py"):
                continue
            try:
                b = io.open(p, "rb").read()
            except Exception as e:
                bad.append((rel, "read error: %s" % e)); continue
            if b.startswith(b"\xff\xfe") or b.startswith(b"\xfe\xff"):
                bad.append((rel, "UTF-16 BOM (PowerShell redirection?)")); continue
            if b.startswith(b"\xef\xbb\xbf"):
                b = b[3:]
            try:
                s = b.decode("utf-8")
            except UnicodeDecodeError as e:
                bad.append((rel, "not UTF-8: %s" % e)); continue
            n = sum(s.count(m) for m in MARKS)
            if n >= 40:   # real double-encoding produces hundreds+; docs may quote a few examples
                bad.append((rel, "mojibake markers=%d (double-encoded text)" % n))
    if bad:
        print("[ENCODING] FAIL: %d file(s)" % len(bad))
        for rel, why in bad[:50]:
            print("  - %s: %s" % (rel, why))
        return 1
    print("[ENCODING] PASS: all text files are clean UTF-8")
    return 0

if __name__ == "__main__":
    sys.exit(main())
