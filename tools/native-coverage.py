#!/usr/bin/env python3
# native-coverage.py -- what native mode still lacks, ranked by how many real programs need it.
#
#   tools/native-coverage.py [--ocerz PATH] [--timeout SECS] [--anonymous] app-or-program...
#   tools/native-coverage.py --scan DIR...        every Intel or universal app bundle under DIR
#
# Each program is put through `ocerz -check` with OCERZ_CHECK_ALL=1, which loads
# and binds it and every library it loads at launch as native mode would, runs
# none of it, and prints each import with how it was answered (dyld.c,
# native_check_report): it crosses to the Mac's framework, ocerz answers it, it
# binds in the program's own code, or it binds to a stub, a placeholder or
# nothing.  The report, in Markdown on stdout, has a line per program and then
# every stub and missing import ranked by the number of programs that import it:
# the order in which bridging them would let the most programs run.  A universal
# app counts by its x86_64 slice, which imports what its Intel build would.
#
# What a static check cannot see: libraries a program dlopens later, and the
# Objective-C messages it sends, which cross by selector while it runs.  A stub
# a program imports but never calls costs nothing, so the ranking says what to
# look at first, not what is certain to break.
#
# Program names stay on the machine that ran this: --anonymous replaces them
# with numbers, for a report that is shared.
import argparse, collections, os, plistlib, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAPS = ("missing", "stub", "placeholder")


def is_swift(sym):
    """A Swift-mangled name ($s, or _T0 before Swift 5): API databases describe C declarations, so these never
    cross, and a program that needs one needs the Swift library it lives in."""
    return sym.startswith(("_$s", "_$S", "$s", "_T0"))


def bundle_executable(app):
    try:
        with open(os.path.join(app, "Contents", "Info.plist"), "rb") as f:
            exe = plistlib.load(f).get("CFBundleExecutable")
    except (OSError, plistlib.InvalidFileException, ValueError):
        return None
    path = os.path.join(app, "Contents", "MacOS", exe) if exe else None
    return path if path and os.path.isfile(path) else None


def archs(path):
    r = subprocess.run(["lipo", "-archs", path], capture_output=True, text=True)
    return r.stdout.split() if r.returncode == 0 else []


def scan(roots):
    found = []
    for root in roots:
        for dirpath, dirnames, _ in os.walk(root):
            for d in list(dirnames):
                if not d.endswith(".app"):
                    continue
                dirnames.remove(d)
                app = os.path.join(dirpath, d)
                exe = bundle_executable(app)
                if exe and "x86_64" in archs(exe):
                    found.append(app)
            if dirpath.count(os.sep) - root.count(os.sep) >= 3:
                dirnames[:] = []
    return sorted(set(found))


def check(ocerz, target, timeout):
    env = dict(os.environ, OCERZ_CHECK_ALL="1", OCERZ_NO_CACHE_HOLDER="1")
    try:
        r = subprocess.run([ocerz, "-check", target], env=env, capture_output=True, text=True, timeout=timeout,
                           stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return None, [], "the check took longer than %d s" % timeout
    rows = []
    for line in r.stdout.splitlines():
        f = line.split("\t")
        if len(f) == 5:
            rows.append(f)
    err = [l for l in r.stderr.splitlines() if l.strip()]
    return r.returncode, rows, err[-1] if err else ""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ocerz", default=os.path.join(HERE, "ocerz"))
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--scan", action="store_true", help="the arguments are directories to search for app bundles")
    ap.add_argument("--anonymous", action="store_true", help="number the programs instead of naming them")
    ap.add_argument("--top", type=int, default=60, help="how many gaps to list")
    ap.add_argument("targets", nargs="+")
    a = ap.parse_args()

    targets = scan(a.targets) if a.scan else a.targets
    if not targets:
        sys.exit("native-coverage: nothing to check")

    per = []
    refused = []
    gaps = collections.defaultdict(lambda: {"programs": set(), "reason": "", "kind": ""})
    nolib = collections.defaultdict(lambda: {"programs": set(), "names": set()})
    for n, t in enumerate(targets, 1):
        name = "program %d" % n if a.anonymous else os.path.basename(t.rstrip("/"))
        print("checking %s" % name, file=sys.stderr)
        rc, rows, err = check(a.ocerz, t, a.timeout)
        counts = collections.Counter(r[0] for r in rows)
        missing = [(lib, sym) for kind, lib, sym, _, _ in rows if kind == "missing"]
        other = sorted({sym for lib, sym in missing if not is_swift(sym) and "libswift" not in lib})
        if rc == 71:
            refused.append((name, len(missing) - len(other), other))
        per.append((name, rc, len(rows), counts, err))
        for kind, lib, sym, reason, _ in rows:
            if kind == "missing" and reason == "no-library":
                nolib[lib]["programs"].add(name)
                nolib[lib]["names"].add(sym)
            elif kind in GAPS:
                g = gaps[(lib, sym)]
                g["programs"].add(name)
                g["reason"] = reason
                g["kind"] = kind

    verdict = {0: "binds completely", 1: "binds; stops if it calls a stub", 71: "native mode refuses it"}
    print("# Native mode coverage\n")
    print("%d programs, checked with `ocerz -check`.\n" % len(per))
    print("| Program | Imports | Cross | ocerz | Own code | Stubs | Missing | Result |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |")
    for name, rc, total, c, err in per:
        result = verdict.get(rc, "did not load: %s" % err if err else "did not load (exit %s)" % rc)
        print("| %s | %d | %d | %d | %d | %d | %d | %s |" % (name, total, c["bridged"], c["ocerz"], c["guest"],
              c["stub"] + c["placeholder"], c["missing"], result))

    if refused:
        print("\n## Why programs are refused\n")
        print("| Program | Swift imports missing | Other imports missing |")
        print("| --- | ---: | --- |")
        for name, nswift, other in refused:
            shown = ", ".join("`%s`" % s for s in other[:4]) + (" and %d more" % (len(other) - 4) if len(other) > 4 else "")
            print("| %s | %d | %s |" % (name, nswift, shown or "none"))

    print("\n## Libraries native mode does not have\n")
    print("Every import from one of these is missing, so a program that links one is refused.\n")
    print("| Programs | Library | Names imported |")
    print("| ---: | --- | ---: |")
    for lib, g in sorted(nolib.items(), key=lambda kv: (-len(kv[1]["programs"]), kv[0])):
        print("| %d | %s | %d |" % (len(g["programs"]), lib, len(g["names"])))

    swift = collections.defaultdict(lambda: {"programs": set(), "names": set()})
    for (lib, sym), g in gaps.items():
        if is_swift(sym):
            swift[lib]["programs"] |= g["programs"]
            swift[lib]["names"].add(sym)
    print("\n## Swift names in libraries native mode has\n")
    print("Foundation, AppKit and others export Swift overlays beside their C and Objective-C interfaces; no API"
          " database can describe those.\n")
    print("| Programs | Library | Swift names imported |")
    print("| ---: | --- | ---: |")
    for lib, g in sorted(swift.items(), key=lambda kv: (-len(kv[1]["programs"]), kv[0])):
        print("| %d | %s | %d |" % (len(g["programs"]), os.path.basename(lib), len(g["names"])))

    ranked = sorted(((k, g) for k, g in gaps.items() if not is_swift(k[1])),
                    key=lambda kv: (-len(kv[1]["programs"]), kv[0][0], kv[0][1]))
    print("\n## What to bridge next\n")
    print("Each C or Objective-C stub, placeholder or missing name in a library native mode has, by the number of"
          " programs that import it.\n")
    print("| Programs | Name | Library | Kind | Why |")
    print("| ---: | --- | --- | --- | --- |")
    for (lib, sym), g in ranked[:a.top]:
        print("| %d | `%s` | %s | %s | %s |" % (len(g["programs"]), sym, os.path.basename(lib), g["kind"],
                                             g["reason"] or "-"))
    if len(ranked) > a.top:
        print("\n%d more not listed (--top)." % (len(ranked) - a.top))

    by_reason = collections.Counter()
    for (lib, sym), g in ranked:
        by_reason[g["reason"] or g["kind"]] += len(g["programs"])
    print("\n## C and Objective-C gaps by cause\n")
    print("| Cause | Imports (counted per program) |")
    print("| --- | ---: |")
    for reason, n in by_reason.most_common():
        print("| %s | %d |" % (reason, n))


if __name__ == "__main__":
    main()
