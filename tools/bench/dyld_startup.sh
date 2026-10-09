#!/bin/bash
# Startup wall time of dynamically linked x86 programs under each given tree's
# ocerz, in cache and native mode, with the translation cache off so every run
# binds, fixes up and translates from scratch.  Best and median of N runs per
# case, in milliseconds.  Default trees: this checkout and the pure-C
# reference at ~/AArchX-c.  N defaults to 9 (DYLD_BENCH_N).
set -u
cd "$(dirname "$0")/../.."
N=${DYLD_BENCH_N:-9}
TREES=("$@")
[ ${#TREES[@]} -eq 0 ] && TREES=("$(pwd)" "$HOME/AArchX-c")
CASES=(
    "/bin/echo hi"
    "/bin/ls /"
    "/usr/bin/sort /etc/hosts"
    "/usr/bin/sw_vers"
    "/usr/bin/plutil -help"
)
export OCERZ_TCACHE=off
python3 - "$N" "${#TREES[@]}" "${TREES[@]}" "${CASES[@]}" <<'PY'
import os, subprocess, sys, time, statistics
n = int(sys.argv[1]); nt = int(sys.argv[2])
trees = sys.argv[3:3 + nt]; cases = sys.argv[3 + nt:]
print("| mode | case | " + " | ".join(f"{os.path.basename(t)} best/median ms" for t in trees) + " |")
print("|---|---|" + "---|" * len(trees))
for mode in ("cache", "native"):
    env = dict(os.environ, OCERZ_MODE=mode)
    for case in cases:
        cells = []
        for t in trees:
            ts = []; rc = None
            for i in range(n + 1):
                t0 = time.perf_counter()
                p = subprocess.run([os.path.join(t, "ocerz")] + case.split(), env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                dt = (time.perf_counter() - t0) * 1000
                rc = p.returncode
                if i: ts.append(dt)
            cells.append(f"{min(ts):.1f} / {statistics.median(ts):.1f}" + ("" if rc == 0 else f" (rc={rc})"))
        print(f"| {mode} | `{case}` | " + " | ".join(cells) + " |")
PY
