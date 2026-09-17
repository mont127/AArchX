#!/usr/bin/env python3
"""
scan_headers.py -- choose the headers in tools/sdkgen/headers/libSystem.h.

    tools/sdkgen/scan_headers.py          # rewrite the include list for the current SDK

libSystem has no umbrella header, so sdkgen parses one of its own, and this is
how that one is chosen when the SDK changes.  Every header under the SDK's
usr/include is a candidate except those in EXCLUDE, which are the headers of
other libraries the SDK ships beside libSystem and the machine directories that
only make sense included through machine/.  A candidate is kept only if it
compiles on its own, after the PRELUDE headers a number of BSD headers assume,
for both x86_64 and arm64.  The survivors are then added in sorted order to a
growing translation unit, sixty-four at a time, and a group that breaks the
unit for either architecture is split in half until the header that breaks it
is found and dropped, so every header kept compiles beside every header kept
before it.  The result is deterministic for a given SDK.

Only the lines after the file's leading comment block are rewritten; the
comment is prose kept by hand.  sdkgen itself refuses to run on an umbrella
that has an error, so this script is what to reach for when it does.
"""
import concurrent.futures
import os
import re
import subprocess
import sys

TOOLDIR = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(TOOLDIR, "headers", "libSystem.h")

PRELUDE = [
    "sys/types.h", "stdarg.h", "stddef.h", "stdint.h", "stdbool.h", "stdio.h",
    "sys/socket.h", "netinet/in.h", "netinet/in_systm.h", "mach/mach.h",
]

EXCLUDE_DIRS = [
    "c++", "net-snmp", "apache2", "apr-1", "libxml", "libxml2", "Spatial", "unicode",
    "libxslt", "libexslt", "AppleArchive", "objc", "simd", "pcap", "cups", "curl",
    "odmodule", "sasl", "ffi", "EndpointSecurity", "tidy", "krb5", "hvf", "gssapi",
    "readline", "libDER", "xar", "editline", "infiniband", "i386", "arm", "arm64",
    "architecture", "pexpert", "_modules", "Darwin.swiftcrossimport",
    "xlocale.swiftcrossimport", "nfs",
]

EXCLUDE_FILES = [
    "AppleEXR", "AppleTextureEncoder", "SystemHealthClient", "SystemHealthManager", "Xplugin",
    "bzlib", "com_err", "compression", "curses", "dtrace", "eti", "expat", "expat_config",
    "expat_external", "form", "gssapi", "histedit", "iconv", "krb5", "lber", "lber_types",
    "ldap", "ldap_cdefs", "ldap_features", "ldap_schema", "ldap_utf8", "ldif", "libcharset",
    "libmanagedconfigurationfiles", "localcharset", "menu", "nc_tparm", "ncurses",
    "ncurses_dll", "panel", "pcap-bpf", "pcap-namedb", "pcap", "profile", "slapi-plugin",
    "sqlite3", "sqlite3ext", "tcl", "tclDecls", "tclPlatDecls", "tclTomMath",
    "tclTomMathDecls", "term", "term_entry", "termcap", "tic", "tk", "tkDecls",
    "tkIntXlibDecls", "tkMacOSX", "tkPlatDecls", "unctrl", "util", "xcselect", "zconf", "zlib",
]


def sdk():
    path = subprocess.check_output(["xcrun", "--show-sdk-path"]).decode().strip()
    version = subprocess.check_output(["xcrun", "--show-sdk-version"]).decode().strip()
    return path, version


def compiles(sysroot, version, headers):
    src = "".join("#include <%s>\n" % h for h in PRELUDE + headers).encode()
    for arch in ("x86_64", "arm64"):
        r = subprocess.run(
            ["clang", "-target", "%s-apple-macos%s" % (arch, version), "-isysroot", sysroot,
             "-std=gnu17", "-fsyntax-only", "-w", "-x", "c", "-"],
            input=src, capture_output=True)
        if r.returncode != 0:
            return False
    return True


def main():
    sysroot, version = sdk()
    include = os.path.join(sysroot, "usr", "include")
    candidates = []
    for dirpath, _, files in os.walk(include):
        for name in files:
            if not name.endswith(".h"):
                continue
            rel = os.path.relpath(os.path.join(dirpath, name), include)
            top = rel.split(os.sep)[0]
            if os.sep in rel and top in EXCLUDE_DIRS:
                continue
            if os.sep not in rel and name[:-2] in EXCLUDE_FILES:
                continue
            candidates.append(rel)
    candidates.sort()

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        alone = list(pool.map(lambda h: compiles(sysroot, version, [h]), candidates))
    standalone = [h for h, ok in zip(candidates, alone) if ok]

    kept = []
    dropped = []

    def add(group):
        if compiles(sysroot, version, kept + group):
            kept.extend(group)
        elif len(group) == 1:
            dropped.append(group[0])
        else:
            half = len(group) // 2
            add(group[:half])
            add(group[half:])

    for i in range(0, len(standalone), 64):
        add(standalone[i:i + 64])

    text = open(TARGET).read()
    m = re.match(r"(/\*.*?\*/\n)", text, re.S)
    if not m:
        sys.exit("scan_headers.py: %s has no leading comment block" % TARGET)
    with open(TARGET, "w") as out:
        out.write(m.group(1))
        for h in PRELUDE + kept:
            out.write("#include <%s>\n" % h)

    print("scan_headers.py: %d candidates, %d compile alone, %d kept" %
          (len(candidates), len(standalone), len(kept)))
    for h in sorted(set(candidates) - set(standalone)):
        print("  does not compile alone: %s" % h)
    for h in dropped:
        print("  conflicts with the headers before it: %s" % h)


if __name__ == "__main__":
    main()
