#!/usr/bin/env python3
"""Watchdogged launcher for anything that may spawn emulated Wine processes.

    tests/safe_run.py [--timeout S] [--max-procs N] [--max-rss-gb G] [--log FILE] -- cmd args...

The command runs in its own process group (setpgrp).  Every 0.3 s the whole
group is inspected (ps): if the wall clock, the number of processes in the
group, or their summed RSS exceeds the limits, the entire group gets SIGKILL
and the reason is printed.  Rationale: on 2026-08-17 an unwatched wineboot
under ocerz went into Wine's service respawn loop (17 explorer.exe at 4-8 GB
each), took the machine to >100 GB and killed WindowServer.  Never run a
Wine session under ocerz without this.
"""
import os, sys, time, signal, subprocess, argparse

PATTERN = None      # also track/kill any process whose argv[0] basename equals this (Wine daemonizes out of the group)
SELF = os.getpid()
ANCESTORS = set()   # never touch our own ancestors (the invoking shell)

def find_ancestors():
    anc = set(); pid = os.getppid()
    for _ in range(64):
        if pid <= 1: break
        anc.add(pid)
        try:
            out = subprocess.run(["ps", "-o", "ppid=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
            pid = int(out)
        except Exception:
            break
    return anc

def group_stats(pgid):
    """(count, rss_bytes, names, pids) of the live (non-zombie) group members
    plus any process matching PATTERN (wineserver/services escape the group)"""
    try:
        out = subprocess.run(["ps", "-axo", "pid=,pgid=,rss=,stat=,command="], capture_output=True, text=True).stdout
    except Exception:
        return 0, 0, [], []
    n = 0; rss = 0; names = []; pids = []
    for line in out.splitlines():
        parts = line.split(None, 4)
        if len(parts) < 4: continue
        try:
            pid, pg, r = int(parts[0]), int(parts[1]), int(parts[2])
        except ValueError:
            continue
        cmd = parts[4] if len(parts) > 4 else "?"
        if pid == SELF or pid in ANCESTORS or parts[3].startswith("Z"): continue
        argv0 = cmd.split()[0].rsplit("/", 1)[-1] if cmd else "?"
        if pg == pgid or (PATTERN and argv0 == PATTERN):
            n += 1; rss += phys_footprint(pid, r * 1024); names.append(argv0); pids.append(pid)
    return n, rss, names, pids

_LIBC = None
def phys_footprint(pid, fallback):
    """The kernel's physical footprint of PID (what Activity Monitor calls
    Memory): private, compressed and IOKit pages, not the shared-cache file
    pages every emulated process also has resident.  ps's RSS counts those in
    each process, so fifteen Wine processes summed to 12 GB of RSS while the
    machine still had 9 GB free.  Falls back to the RSS handed in."""
    global _LIBC
    try:
        if _LIBC is None:
            import ctypes
            _LIBC = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
            _LIBC.proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
        import ctypes
        buf = ctypes.create_string_buffer(1024)
        if _LIBC.proc_pid_rusage(pid, 0, buf) != 0:
            return fallback
        return int.from_bytes(buf.raw[72:80], "little")   # rusage_info_v0.ri_phys_footprint
    except Exception:
        return fallback

def kill_group(pgid, leader):
    """SIGKILL every live process in the group; repeat until none is left
    (children spawned between passes are caught).  killpg() is EPERM inside
    this sandbox, so the pids are enumerated and killed one by one."""
    for _ in range(20):
        n, _rss, _names, pids = group_stats(pgid)
        if leader.poll() is None and leader.pid not in pids: pids.append(leader.pid)
        if not pids: return
        for pid in pids:
            try: os.kill(pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError): pass
        time.sleep(0.1)
        leader.poll()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--max-procs", type=int, default=8)
    ap.add_argument("--max-rss-gb", type=float, default=12.0, help="cap on the group's summed physical footprint")
    ap.add_argument("--log", default=None, help="redirect the command's stdout+stderr here")
    ap.add_argument("--pattern", default="ocerz", help="also track/kill processes whose argv[0] basename is this (default: ocerz)")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd
    if cmd and cmd[0] == "--": cmd = cmd[1:]
    if not cmd:
        print("safe_run: no command", file=sys.stderr); return 2
    global PATTERN, ANCESTORS
    PATTERN = a.pattern or None
    ANCESTORS = find_ancestors()
    out = open(a.log, "wb") if a.log else None
    p = subprocess.Popen(cmd, preexec_fn=os.setpgrp,
                         stdout=out if out else None, stderr=subprocess.STDOUT if out else None)
    pgid = p.pid   # setpgrp: pgid == pid of the leader (same session: killpg allowed on macOS)
    t0 = time.time(); reason = None; peak_n = 0; peak_rss = 0
    while True:
        rc = p.poll()
        n, rss, names, _pids = group_stats(pgid)
        peak_n = max(peak_n, n); peak_rss = max(peak_rss, rss)
        if rc is not None and n == 0:
            break
        el = time.time() - t0
        if el > a.timeout: reason = "timeout %.0fs" % el
        elif n > a.max_procs: reason = "process count %d > %d (%s)" % (n, a.max_procs, ",".join(sorted(set(names))))
        elif rss > a.max_rss_gb * (1 << 30): reason = "group RSS %.1f GB > %.1f GB" % (rss / (1 << 30), a.max_rss_gb)
        if reason:
            kill_group(pgid, p)
            break
        if rc is not None and n > 0:
            # leader exited but the group/pattern set lives on (wineserver,
            # services): allow 3 s of teardown, then kill the rest
            main._lingering = getattr(main, "_lingering", 0) + 1
            if main._lingering > 10:
                kill_group(pgid, p)
                break
        time.sleep(0.3)
    if out: out.close()
    n, rss, names, _pids = group_stats(pgid)
    if n and not reason:
        reason = "leader done, killed %d lingering (%s)" % (n, ",".join(sorted(set(names))))
        kill_group(pgid, p)
        n, rss, names, _pids = group_stats(pgid)
    print("safe_run: rc=%s elapsed=%.1fs peak_procs=%d peak_rss=%.2fGB left=%d%s" %
          (p.returncode, time.time() - t0, peak_n, peak_rss / (1 << 30), n,
           (" KILLED: " + reason) if reason else ""), file=sys.stderr)
    return 99 if reason else (p.returncode or 0)

if __name__ == "__main__":
    sys.exit(main())
