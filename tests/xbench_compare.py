#!/usr/bin/env python3
"""Ocerz vs Rosetta over the xbench kernel suite (paired-delta method).

For each kernel: calibrate a scale so Rosetta takes ~TARGET s, then for REPS
reps time both engines at scale and scale/2 (order alternates), take the
delta (cancels process startup + JIT warmup) and report median ratio.
Exit 0 iff Ocerz is faster on every kernel."""
import os, subprocess, sys, time, statistics

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OCERZ = os.environ.get("OCERZ", os.path.join(REPO, "ocerz"))
XB = os.environ.get("XB", os.path.join(REPO, "tests/guest/benchbin/xbench"))   # XB=tests/guest/benchbin/xbench_dyn for the dynamically linked build (needs OCERZ_HOSTWQ=1)
REPS = int(os.environ.get("REPS", "3"))
TARGET = float(os.environ.get("TARGET", "0.6"))
DFLT = dict(icall=50000000, jtab=50000000, depchain=100000000, brmiss=50000000,
            memcpy=2000000, str=20000000, hash=20000, idiv=10000000, fpsse=30000000,
            fpvec=5000, chase=30000000, qsort=30, leafcall=50000000, mixed=20000, vm=500000)
KERNELS = os.environ.get("KERNELS", ",".join(DFLT)).split(",")

def run(engine, k, n):
    cmd = [XB, k, str(n)] if engine == "R" else [OCERZ, XB, k, str(n)]
    t0 = time.perf_counter()
    out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
    return time.perf_counter() - t0, out

def main():
    print(f"{'kernel':<10}{'scale':>12}{'Rosetta_s':>11}{'Ocerz_s':>10}{'ratio':>9}  verdict")
    print(f"{'------':<10}{'-----':>12}{'---------':>11}{'-------':>10}{'-----':>9}  -------")
    losing = 0
    for k in KERNELS:
        n = DFLT[k]
        for _ in range(4):                      # calibrate until Rosetta takes >= TARGET/2
            t, _ = run("R", k, n)
            if t >= TARGET * 0.5: break
            n = max(2, int(n * min(30.0, TARGET / max(t, 0.02))))
        half = max(1, n // 2)
        rd, od, ratios = [], [], []
        for rep in range(REPS):
            if rep % 2 == 0:
                rl, ro = run("R", k, half); ol, oo = run("O", k, half)
                rh, ro2 = run("R", k, n);   oh, oo2 = run("O", k, n)
            else:
                oh, oo2 = run("O", k, n);   rh, ro2 = run("R", k, n)
                ol, oo = run("O", k, half); rl, ro = run("R", k, half)
            if ro != oo or ro2 != oo2:
                print(f"{k}: OUTPUT MISMATCH rosetta={ro2!r} ocerz={oo2!r}"); sys.exit(2)
            r = max(rh - rl, 1e-3); o = max(oh - ol, 1e-3)
            rd.append(r); od.append(o); ratios.append(o / r)
        rm, om, ratio = statistics.median(rd), statistics.median(od), statistics.median(ratios)
        v = "WIN" if ratio < 1.0 else "LOSE"
        if v == "LOSE": losing += 1
        print(f"{k:<10}{n:>12}{rm:>11.4f}{om:>10.4f}{ratio:>8.3f}x  {v}")
    print()
    if losing:
        print(f"LOSING on {losing} kernel(s)"); sys.exit(1)
    print("Ocerz faster on every kernel"); sys.exit(0)

if __name__ == "__main__":
    main()
