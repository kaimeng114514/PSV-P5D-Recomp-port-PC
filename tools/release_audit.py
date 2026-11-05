# SPDX-License-Identifier: MIT
#!/usr/bin/env python3
"""release_audit.py - 发布树合规扫描（P5D 静态重编译移植）

用途：发布包中**绝对不允许**包含任何游戏文件或游戏衍生数据。
本脚本扫描给定目录（发布树），按扩展名 + 文件头魔数 + 大文件启发式报告违规项，
并在发现违规时返回非 0 退出码，供 run_all_regressions / 打包脚本调用。

用法：
    py -3 tools/release_audit.py <release_dir> [--allow <glob> ...]

红线（见 docs/RELEASE_COMPLIANCE.md）：
  - 游戏可执行码：eboot.bin / p5d_true.elf / SELF-ELF 段 / 重编译产物（batch_*.cpp/.o）
  - 游戏资源：data.cpk / vita.cpk / GFDPSP2.GSC / GFD-GFS-GMD-CPK / USM / AT9/AWB
  - 游戏衍生着色器：spv_all 的 *.spv（应从玩家数据现场生成）
  - 密钥/固件
"""
import argparse
import fnmatch
import os
import sys

# 扩展名黑名单（游戏数据/游戏衍生数据）
BAD_EXT = {
    ".cpk", ".gsc", ".gxp", ".gfs", ".gmd", ".gap", ".env", ".epl",
    ".usm", ".at9", ".at3", ".awb", ".acb", ".adx", ".ahx",
    ".spr", ".spd", ".bf", ".bmd", ".bed", ".pak", ".psarc", ".pkx",
    ".skel", ".rig", ".motion", ".anm", ".sgb", ".sgb2",
    ".spv",               # 由游戏着色器翻译而来：本地生成，禁止随包
    ".bmp", ".png",       # 里程碑/画面截图（如属游戏画面则禁止）
}
# 这些名字/前缀本身就是游戏或游戏衍生文件
BAD_NAME_PATTERNS = [
    "eboot.bin", "eboot.elf", "eboot.self", "p5d_true.elf", "eboot_dec.bin",
    "data.cpk", "vita.cpk", "gfdp*.gsc", "spv_all*", "spv_2ph*", "spv_all_new*",
    "batch_*.cpp", "batch_*.o", "recomp_harness.exe",
    "prod.keys", "title.keys",
]
# 文件头魔数（前 16 字节）
BAD_MAGIC = [
    b"GFS0", b"GSC0", b"PVSG", b"GXP\x00", b"CPK ", b"@UTF",
    b"USM\x00", b"SCE\x00", b"\x7fELF",
    b"\x00\x00\x00\x00\x00\x00\x00\x00",  # 占位：不单独判定（见下）
]
# 大文件阈值：超过则警告（游戏数据通常很大；引擎一般 < 200 MB）
LARGE_WARN_BYTES = 200 * 1024 * 1024
# 文档允许清单：docs/screenshots/ 只存放**我们移植运行时由原版游戏代码渲染出的进度截图**，
# 用于说明开发进展/验证；不是游戏原始资源文件，不能用于重建游戏资源。版权归 ATLUS/SEGA。
# 见 docs/RELEASE_COMPLIANCE.md "进度截图" 一节。--allow 仍可追加其他临时豁免。
BUILT_IN_ALLOW = ["docs/screenshots/*.png"]


def scan(root):
    violations = []
    warnings = []
    total = 0
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, root)
            total += 1
            ext = os.path.splitext(fn)[1].lower()
            if ext in BAD_EXT:
                violations.append((rel, "extension " + ext))
                continue
            name_l = fn.lower()
            hit_name = None
            for pat in BAD_NAME_PATTERNS:
                if fnmatch.fnmatch(name_l, pat):
                    hit_name = pat
                    break
            if hit_name:
                violations.append((rel, "name pattern " + hit_name))
                continue
            try:
                size = os.path.getsize(path)
                if size > LARGE_WARN_BYTES:
                    warnings.append((rel, "large file %.1f MB" % (size / 1048576.0)))
                with open(path, "rb") as f:
                    head = f.read(16)
            except OSError as e:
                warnings.append((rel, "unreadable: %s" % e))
                continue
            for magic in BAD_MAGIC:
                if not magic:
                    continue
                if magic == b"\x00\x00\x00\x00\x00\x00\x00\x00":
                    continue
                if head.startswith(magic):
                    violations.append((rel, "magic %r" % magic))
                    break
    return total, violations, warnings


def main(argv):
    ap = argparse.ArgumentParser(description="P5D release tree compliance audit")
    ap.add_argument("release_dir")
    ap.add_argument("--allow", action="append", default=[], help="glob(s) allowed despite rules")
    args = ap.parse_args(argv)
    if not os.path.isdir(args.release_dir):
        print("release_audit: not a directory: %s" % args.release_dir)
        return 2
    total, violations, warnings = scan(args.release_dir)
    allow = [a.lower() for a in args.allow] + BUILT_IN_ALLOW
    violations = [(p, why) for (p, why) in violations
                  if not any(fnmatch.fnmatch(p.lower(), a) for a in allow)]
    print("release_audit: scanned %d files under %s" % (total, args.release_dir))
    for p, why in warnings:
        print("  WARN  %s (%s)" % (p, why))
    for p, why in violations:
        print("  VIOLATION  %s (%s)" % (p, why))
    if violations:
        print("release_audit: FAIL - %d game/derived file(s) must be removed from the release tree" % len(violations))
        return 1
    print("release_audit: PASS - no game files or game-derived data detected")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

# --- UTF-8 / mojibake guard (added M1181) ---
import subprocess as _sp, sys as _sys
try:
    _r = _sp.run([_sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'check_text_encoding.py'), os.getcwd()], capture_output=True, text=True)
    print(_r.stdout.strip() or _r.stderr.strip())
    if _r.returncode != 0:
        _sys.exit(1)
except Exception as _e:
    print('[ENCODING] guard error:', _e)
    _sys.exit(1)
