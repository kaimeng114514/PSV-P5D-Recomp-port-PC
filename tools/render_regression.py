# -*- coding: utf-8 -*-
"""P5D PC 原生渲染回归（帧图哈希 + 图像级判据）。

为什么不能只看 MD5（ARCH_1059 的教训）：空白图也会"稳定地相同"。
本脚本对每帧检查四类判据：
  ① 尺寸 = 960x544（游戏原生横屏，ARCH_10 实证）
  ② **非背景像素数/占比**（角色是否真的画出来了）
  ③ **纯黑像素占比 ≈ 0**（"黑带"回归，ARCH_1066/1067）
  ④ SHA256（同路径内的**逐位**稳定性）

用法：
  py -3 render_regression.py --make-baseline     # 生成基线 baseline.json
  py -3 render_regression.py                     # 与基线比对，输出 PASS/FAIL
"""
import sys, os, json, glob, hashlib, subprocess

GL = r"C:\dev\p5d_build\_glprobe\glcompare.exe"
OUT = r"C:\dev\out\p5d_regress"
BASE = os.path.join(OUT, "baseline.json")
ASPECT = "1.764705882"
# 固定的一组输入（含相机，见 ARCH_1061/1068）
CASES = [
    ("t20.0", r"C:\dev\out\p5d_seq2\q3.p5dgpu"),
    ("t20.1", r"C:\dev\out\p5d_seq2\q4.p5dgpu"),
    ("t19.9", r"C:\dev\out\p5d_seq2\q2.p5dgpu"),
]
MAX_PURE_BLACK = 0.005      # 纯黑占比上限 0.5%
MIN_NONBG      = 0.005      # 非背景占比下限 0.5%
EXPECT_W, EXPECT_H = 960, 544


def measure(path):
    from PIL import Image
    import numpy as np
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)
    mx = a.max(axis=2)
    return {
        "w": int(a.shape[1]), "h": int(a.shape[0]),
        "nonbg": int((mx > 20).sum()),
        "nonbg_ratio": round(float((mx > 20).mean()), 6),
        "pure_black_ratio": round(float((mx < 2).mean()), 6),
        "sha256": hashlib.sha256(open(path, "rb").read()).hexdigest(),
    }


def run_case(name, pdg):
    if not os.path.exists(pdg):
        return None, "输入缺失: " + pdg
    bmp = os.path.join(OUT, name + ".bmp")
    rgb = os.path.join(OUT, name + ".rgb")
    env = dict(os.environ); env["P5D_ASPECT"] = ASPECT
    r = subprocess.run([GL, "--shade", pdg, rgb, bmp], capture_output=True, env=env)
    if not os.path.exists(bmp):
        return None, "渲染失败 rc=%d" % r.returncode
    m = measure(bmp)
    # ---- 图像级判据（独立于基线的硬性检查）----
    errs = []
    if (m["w"], m["h"]) != (EXPECT_W, EXPECT_H):
        errs.append("尺寸 %dx%d != %dx%d" % (m["w"], m["h"], EXPECT_W, EXPECT_H))
    if m["pure_black_ratio"] > MAX_PURE_BLACK:
        errs.append("纯黑占比 %.4f > %.4f（黑带回归！）" % (m["pure_black_ratio"], MAX_PURE_BLACK))
    if m["nonbg_ratio"] < MIN_NONBG:
        errs.append("非背景占比 %.4f < %.4f（角色没画出来？）" % (m["nonbg_ratio"], MIN_NONBG))
    return m, errs


def main():
    # CLEANUP_4688: the .p5dgpu captured traces under C:\dev\out were removed by the disk cleanup
    # (regenerable by re-capturing with the GXM backend). Skip instead of failing when absent.
    if not os.path.exists(GL):
        print("RENDER_REGRESSION_SKIP (glcompare missing: %s)" % GL); return
    missing = [p for _, p in CASES if not os.path.exists(p)]
    if missing:
        print("RENDER_REGRESSION_SKIP (captured input missing: %s)" % ", ".join(missing)); return
    os.makedirs(OUT, exist_ok=True)
    make = "--make-baseline" in sys.argv
    base = {}
    if not make and os.path.exists(BASE):
        base = json.load(open(BASE, encoding="utf-8"))

    all_ok = True
    results = {}
    for name, pdg in CASES:
        m, errs = run_case(name, pdg)
        if m is None:
            print("  [%s] FAIL: %s" % (name, errs)); all_ok = False; continue
        results[name] = m
        line = "  [%s] %dx%d 非背景=%d(%.2f%%) 纯黑=%.4f%%" % (
            name, m["w"], m["h"], m["nonbg"], 100 * m["nonbg_ratio"], 100 * m["pure_black_ratio"])
        if errs:
            all_ok = False
            print(line + "  FAIL: " + "; ".join(errs))
        else:
            print(line + "  OK")
        if base.get(name):
            b = base[name]
            if b["sha256"] != m["sha256"]:
                print("         (逐位与基线不同: 非背景 %d -> %d)" % (b["nonbg"], m["nonbg"]))

    if make:
        json.dump(results, open(BASE, "w", encoding="utf-8"), indent=2)
        print("  已写入基线 %s" % BASE)
    print("RENDER_REGRESSION_%s" % ("PASS" if all_ok else "FAIL"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
