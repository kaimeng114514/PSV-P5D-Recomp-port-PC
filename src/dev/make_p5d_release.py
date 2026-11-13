#!/usr/bin/env python3
"""make_p5d_release.py: build the P5D port's single-file release exe (plan B).

    python make_p5d_release.py [--staging DIR] [--out FILE] [--skip-copy]

The exe is dev/p5d_sfx_stub.c's self-extractor built for Windows with the payload appended: the port's own source
(pipeline, recompiler, runtime, decrypt, packs, third_party), the launcher (P5D Windows移植版启动器.exe + SDL3.dll), an embedded
Python (the embeddable CPython 3.10 plus capstone/pycryptodome/tomllib), the Windows compiler the stages build the game
with (mingw64) and the SDL3/zlib/ffmpeg development files (toolchain). Nothing of the game ships: the player supplies
their own copy and the Vita firmware.

The archive is a private format the stub reads: see dev/p5d_sfx_stub.c. Compression is raw deflate (zlib).
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))       # C:\dev\nativ_src
WORK = r"C:\dev\tmp\m1448"
LAUNCHER_DIR = os.path.join(ROOT, "build", "p5d_launcher")
MINGW = r"C:\dev\mingw64"
TOOLCHAIN = r"C:\dev\nativ_toolchain"
PYEMBED = os.path.join(WORK, "pyembed")
ZLIB = os.path.join(TOOLCHAIN, "zlib")

SITECUSTOMIZE = """\"\"\"P5D 移植版: the embedded Python's path setup.

The embeddable CPython is driven by python310._pth, and with that file Python ignores PYTHONPATH entirely, so the
stages that launch each other (pipeline/packs.py, pipeline/licenses.py, runtime/build.py, decrypt/vitadecrypt...)
would not find their own folders. Add the code folders here, for every Python start.
\"\"\"
import os
import sys

_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
for _d in ("pipeline", "runtime", "recompiler", "decrypt"):
    _p = os.path.join(_root, _d)
    if os.path.isdir(_p) and _p not in sys.path:
        sys.path.append(_p)
"""

RELEASE_README = """# P5D 移植版 (Persona 5 Dancing in Starlight, PC)

This is the free fan PC port of Persona 5 Dancing in Starlight (PCSH10109, Asia 1.00): the original game's code is
recompiled ahead of time and runs natively on Windows, with keyboard/mouse and gamepad support (the game's own
hints show the keys).

**You need:** your own copy of the game (a Vita dump, including MaiDump format), and the PS Vita system software
(PSVUPDAT.PUP 3.74). No game data or firmware is included or distributed.

**Use:** run P5D Windows移植版启动器.exe. On the first run it unpacks its runtime (~1 minute) and shows the launcher:

1. Game ROM: pick your game folder (the one that holds eboot.bin and data/).
2. System firmware: pick PSVUPDAT.PUP, or a folder that already holds the decrypted firmware (version.txt, os0/, vs0/).
3. Start building: the game is decrypted, analysed, recompiled and compiled (first build: 10-30 minutes).
4. Resolution: original 960x544 / 720p / 1080p / 2K 2560x1440 / 4K 3840x2160. The choice also sets the game's
   internal render scale (4K renders at 4x, 2K at 3x, 1080p at 2x), so a higher resolution costs GPU time.
5. Launch game.

A new launcher version rebuilds the game once on its next start (the launcher checks the build automatically,
a few minutes); later starts are instant.

**Version 0.1.1. Controls (keyboard and mouse):** arrow keys move the menus; Enter/Space confirms; Backspace goes back; Esc opens
the pause menu (and leaves it through the game's own back). In the pause menu the mouse wheel selects, the left
button confirms and the right button returns; in the setting pages Esc/Backspace/right click return too (the port
sends the game's own back chord). Q/E are per-screen: on the COMMUNITY screens they are the L/R shoulders (character switch), on the
song list and the settings page they are the D-pad left/right. If a screen ever maps them wrongly, press **F3**
to cycle the Q/E mode (auto / L-R shoulders / D-pad) - the choice is announced on screen and remembered.
COLLECTION watch mode: Enter enters/leaves it, WASD is the left stick (move), the arrow keys the right stick (rotate), the mouse wheel zooms and Alt resets; Ctrl is Triangle (random outfit).
The dance stage uses Q/A/Z (up/left/down) and D (right), O/K/M (triangle/circle/cross) and J (square); Space is the
scratch gesture. F1 shows the on-screen stats; F11 reloads input.ini after editing it. A gamepad is plug and play
(the hints switch back to the original button badges when a pad is used).

All building happens on your own computer; nothing is uploaded. The game data and save files stay in
%LOCALAPPDATA%\P5D-Port\P5D\library; the shared settings (resolution, render scale, fullscreen, ...) live in
%APPDATA%\P5D-Port\P5D\settings.ini.

Licensed under GPL-2.0 (see LICENSE); third-party licences are listed in THIRD_PARTY.md.
"""

RELEASE_VERSION = "v0.1.1-p5d"
PROVISION_VERSION = "2"   # bump when the packaging rules themselves change (part of the payload stamp)   # written into the payload's version.txt: ready builds against this, so a launch doesn't rebuild
FNV_OFFSET, FNV_PRIME = 1469598103934665603, 1099511628211


def fnv1a64(data, h=FNV_OFFSET):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def copy_tree(src, dst, skip=()):
    n = 0
    for base, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in skip]
        rel = os.path.relpath(base, src)
        out = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(out, exist_ok=True)
        for f in files:
            s = os.path.join(base, f)
            shutil.copy2(s, os.path.join(out, f))
            n += 1
    return n


def stage(staging):
    if os.path.isdir(staging):
        shutil.rmtree(staging)
    os.makedirs(staging)
    n = 0
    # the port's source: only the repository's tracked files (the stages run them; no build outputs, no __pycache__)
    tracked = subprocess.run(["git", "-C", ROOT, "ls-files", "-z"], capture_output=True, check=True).stdout.split(b"\0")
    want = ("pipeline/", "recompiler/", "runtime/", "decrypt/", "packs/PCSH10109/", "third_party/", "assets/p5d/")   # only this port's pack and its own brand: nothing of the other games or Nativ's
    for raw in tracked:
        if not raw:
            continue
        f = raw.decode("utf-8")
        if any(f.startswith(w) for w in want) or f in ("README.md", "LICENSE", "THIRD_PARTY.md", "requirements.txt"):
            src = os.path.join(ROOT, f)
            if os.path.isfile(src):
                dst = os.path.join(staging, f)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(src, dst)
                n += 1
    # a staged source's own #include "..." must be staged too: the release ships only the repository's tracked files, so a
    # new generated header never added to git (runtime/ui/banner_art.h, M1636) made every player's build fail with
    # "banner_art.h: No such file or directory" while the dev tree built fine. Check the sources the shipped (minimal) build
    # compiles, resolving each include against the including file's directory. Fail the packaging instead of the player.
    import posixpath, re as _re
    staged = set()
    for base, dirs, files in os.walk(staging):
        for f in files:
            staged.add(os.path.relpath(os.path.join(base, f), staging).replace(os.sep, "/"))
    missing = {}
    pat = _re.compile(rb'#\s*include\s*"([^"]+)"')
    for rel in sorted(staged):
        if not rel.endswith((".c", ".h", ".cc", ".cpp", ".hpp")):
            continue
        if rel.startswith(("third_party/", "runtime/tests/")) or rel == "runtime/ui/overlay.cpp":
            continue   # not part of the shipped minimal host (its generated/shipped headers come from other packages)
        try:
            data = open(os.path.join(staging, rel), "rb").read()
        except OSError:
            continue
        for m in pat.finditer(data):
            inc = m.group(1).decode("utf-8", "replace")
            cand = posixpath.normpath(posixpath.join(posixpath.dirname(rel), inc))
            # the build's -I paths resolve a bare header name from elsewhere in the tree (window.c -> "rt.h" is
            # runtime/rt.h), so a staged file of that name anywhere satisfies it
            base = posixpath.basename(inc)
            if cand in staged or inc in staged or any(x == base or x.endswith("/" + base) for x in staged):
                continue
            missing.setdefault(rel, set()).add(inc)
    if missing:
        lines = ["the payload is missing headers that its shipped sources include:"]
        for k in sorted(missing):
            lines.append("  " + k + " -> " + ", ".join(sorted(missing[k])))
        raise SystemExit("\n".join(lines))
    # the stages' version stamp: an install's version.txt is what ready builds against (no git in the release).
    # It MUST change with the source this payload ships, or a player's game build never refreshes (the launcher
    # kept running an older operation logic for exactly that reason, reported 2026-11-07): release + HEAD + the
    # working tree's diff hash (uncommitted source changes are copied too).
    head = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    # hash only what changes the GAME BUILD (runtime + the pack): a launcher-only update must not rebuild the game.
    # hash the STAGED payload (not the working tree: an untracked file in the tree but missing from the payload made
    # M1636's stamp claim a source the players never received), plus PROVISION_VERSION so a change to the packaging rules
    # itself (this include check, M1641) refreshes an existing install's build.
    h = hashlib.sha1(b"payload-provision-" + PROVISION_VERSION.encode() + b"\n")
    for d0 in ("runtime", os.path.join("packs", "PCSH10109")):
        base = os.path.join(staging, d0)
        for dp, dn, fn in os.walk(base):
            dn[:] = sorted(x for x in dn if x != "__pycache__" and not x.startswith("."))
            for f in sorted(fn):
                if f.endswith(".pyc") or f.startswith("."): continue
                fp = os.path.join(dp, f)
                h.update(os.path.relpath(fp, staging).replace(os.sep, "/").encode())
                h.update(open(fp, "rb").read())
    stamp = RELEASE_VERSION + ("-" + head if head else "") + "+" + h.hexdigest()[:12]
    with open(os.path.join(staging, "version.txt"), "w", newline="\n") as v:
        v.write(stamp + "\n")
    print("payload version stamp:", stamp)
    # the player-facing readme (the repository's is Nativ's project readme)
    with open(os.path.join(staging, "README.md"), "w", encoding="utf-8", newline="\n") as v:
        v.write(RELEASE_README)
    # the launcher
    for f in ("P5D Windows移植版启动器.exe", "SDL3.dll"):
        shutil.copy2(os.path.join(LAUNCHER_DIR, f), os.path.join(staging, f))
        n += 1
    # the bundled Python (embeddable CPython + the three wheels the stages import)
    n += copy_tree(PYEMBED, os.path.join(staging, "python"))
    # a ._pth file makes Python ignore PYTHONPATH: the stage children get their folders from sitecustomize instead
    io_path = os.path.join(staging, "python", "Lib", "site-packages", "sitecustomize.py")
    os.makedirs(os.path.dirname(io_path), exist_ok=True)
    with open(io_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(SITECUSTOMIZE)
    n += 1
    # the Windows compiler and the development libraries
    n += copy_tree(MINGW, os.path.join(staging, "mingw64"), skip=("share", "doc", "man", "info", "locale", "opt", "var"))
    for d in ("SDL3", "zlib", "ffmpeg"):
        n += copy_tree(os.path.join(TOOLCHAIN, d), os.path.join(staging, "toolchain", d))
    print(f"staged {n} files")
    return n


def pack(staging, stub, out):
    files = []
    for base, dirs, names in os.walk(staging):
        dirs.sort()
        for f in sorted(names):
            full = os.path.join(base, f)
            files.append((os.path.relpath(full, staging).replace(os.sep, "/"), full))
    total = sum(os.path.getsize(p) for _, p in files)
    print(f"payload: {len(files)} files, {total / 2**20:.1f} MB raw")
    h = FNV_OFFSET
    with open(out, "wb") as o:
        with open(stub, "rb") as s:
            o.write(s.read())
        offset = o.tell()
        head = b"P5DPAY01" + struct.pack("<I", len(files))
        o.write(head); h = fnv1a64(head, h)
        done = 0
        for name, full in files:
            raw = open(full, "rb").read()
            co = zlib.compressobj(9, zlib.DEFLATED, -15)
            comp = co.compress(raw) + co.flush()
            nb = name.encode("utf-8")
            rec = struct.pack("<H", len(nb)) + nb + struct.pack("<QQ", len(raw), len(comp))
            o.write(rec); h = fnv1a64(rec, h); o.write(comp); h = fnv1a64(comp, h)
            done += 1
            if done % 500 == 0:
                print(f"  {done}/{len(files)} files, {o.tell() / 2**20:.0f} MB written", flush=True)
        footer = struct.pack("<QQ", offset, h) + b"P5DEND01"
        o.write(footer)
    print(f"release exe: {out} ({os.path.getsize(out) / 2**20:.1f} MB, version {h:016X})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--staging", default=os.path.join(WORK, "payload"))
    ap.add_argument("--out", default=os.path.join(WORK, "P5D Windows移植版启动器.exe"))
    ap.add_argument("--skip-copy", action="store_true")
    a = ap.parse_args()
    stub = os.path.join(WORK, "p5d_stub.exe")
    if True:   # always rebuild the stub so icon/resource changes take effect
        inc = os.path.join(ZLIB, "include")
        lib = os.path.join(ZLIB, "lib", "libz.a")
        gcc = os.environ.get("RECOMP_HOST_CC") or r"C:\dev\mingw64\bin\gcc.exe"
        # icon resource: the release exe itself must show the P5D icon on the desktop
        windres = os.path.join(os.path.dirname(gcc), "windres.exe")
        ico = os.path.join(ROOT, "assets", "p5d", "p5d.ico")
        extra = []
        if os.path.isfile(windres) and os.path.isfile(ico):
            rc = os.path.join(WORK, "p5d_stub.rc")
            res = os.path.join(WORK, "p5d_stub_res.o")
            with open(rc, "w", encoding="ascii") as f:
                f.write('1 ICON "%s"\n' % ico.replace("\\", "/"))
            subprocess.run([windres, rc, "-O", "coff", "-o", res], check=True)
            extra = [res]
        cmd = [gcc, "-O2", "-mwindows", "-o", stub, os.path.join(ROOT, "dev", "p5d_sfx_stub.c"),
               "-I" + inc] + extra + [lib, "-lcomctl32", "-ldwmapi", "-static"]
        print("build stub:", " ".join(cmd))
        subprocess.run(cmd, check=True)
    if not a.skip_copy:
        stage(a.staging)
    pack(a.staging, stub, a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
