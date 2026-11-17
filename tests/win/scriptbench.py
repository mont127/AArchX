#!/usr/bin/env python3
"""scriptbench.py [label=path/to/ocerz ...]: Wine's script engines under Rosetta and under each ocerz named.

WINE is the wine binary of an x86-64 Wine build and WINEPREFIX a prefix that is already set up.  Wine's own
cscript runs each script in tests/win/scriptbench (JScript objects and calls, JScript's library, a VBScript
loop): Windows code throughout, and each prints how long its loop took.  The engines take turns, REPS times
(default 3), after one short run each to start their wineserver and warm the files; the best time of each is
printed with its ratio to Rosetta's.  ROUNDS scales the work (default 1, about a second a script under
Rosetta) and ONLY=objects.js,... runs some of them.

    WINE=/path/to/wine/bin/wine WINEPREFIX=~/prefix python3 tests/win/scriptbench.py new=./ocerz
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
WINE = os.environ.get("WINE")
if not WINE or not os.environ.get("WINEPREFIX"):
    sys.exit(__doc__)
SERVER = os.path.join(os.path.dirname(WINE), "wineserver")
REPS = int(os.environ.get("REPS", "3"))
SCALE = float(os.environ.get("ROUNDS", "1"))
ROUNDS = {"objects.js": 300, "library.js": 600, "loop.vbs": 300}
env = dict(os.environ, WINEDEBUG="-all", WINEDLLOVERRIDES="winemenubuilder.exe=d;mscoree,mshtml=")
engines = [("rosetta", None)] + [tuple(a.split("=", 1)) for a in sys.argv[1:]]
only = os.environ.get("ONLY")


def run(ocerz, script, rounds):
    cmd = ([ocerz] if ocerz else []) + [WINE, "cscript", "//nologo", os.path.join(HERE, "scriptbench", script), str(rounds)]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=900)
    out = r.stdout.replace("\r", "").strip().splitlines()
    return (int(out[-1].split()[-2]), out[-1].rsplit(" ", 2)[0]) if out and out[-1].endswith("ms") else (None, None)


def stop(ocerz):
    subprocess.run(([ocerz] if ocerz else []) + [SERVER, "-k"], capture_output=True, env=env)


for script, base_rounds in ROUNDS.items():
    if only and script not in only.split(","):
        continue
    rounds = max(1, int(base_rounds * SCALE))
    best, answers = {}, set()
    for rep in range(REPS):
        for name, ocerz in engines:
            run(ocerz, script, 5)
            ms, answer = run(ocerz, script, rounds)
            stop(ocerz)
            if ms is None:
                continue
            answers.add(answer)
            if best.get(name) is None or ms < best[name]:
                best[name] = ms
    base = best.get("rosetta")
    cells = []
    for name, _ in engines:
        ms = best.get(name)
        ratio = "" if name == "rosetta" or not base or not ms else " (%.2fx)" % (ms / base)
        cells.append("%s %s ms%s" % (name, ms if ms is not None else "failed", ratio))
    print("%-11s %s%s" % (script, "   ".join(cells), "" if len(answers) <= 1 else "   ANSWERS DIFFER: %s" % sorted(answers)), flush=True)
