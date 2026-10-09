"""Execute shared x86-64 fixtures and compare sets of normalized translations.

Guest binaries and fixture sources come only from the reference tree. The
native runner's fixture builders are reused without running its test cases;
this avoids maintaining a second copy of its substantial embedded programs.
Each process gets a separate stream, so forked guests cannot interleave records.
Duplicates and translation order are irrelevant; all distinct block variants,
including missing variants and changed instruction boundaries, remain visible.
Cache-mode runs separately report one-sided system-cache RIP coverage: allocator
and thread scheduling choose different runtime paths even between C controls.
Guest/application RIPs and all variants at shared RIPs remain strict. Native
and Foundation command fixtures start in ordered-memory mode to avoid racing
the plain-to-ordered transition when background threads appear.
"""
from __future__ import annotations

import contextlib
from dataclasses import dataclass, field
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys

from audit import AuditError, diagnostics, normalized, records, run


@dataclass
class Fixture:
    name: str
    args: list
    env: dict = field(default_factory=dict)
    status: int = 0
    golden: bytes | None = None
    cache_coverage: bool = False


def fixtures(tree, work):
    log = work / "fixtures.log"
    run(["make", "-C", "tests/guest", "-j8", "all", "bench"], tree, log)
    run(["make", "apis"], tree, log, timeout=1800)
    cases = []
    for binary in sorted((tree / "tests/guest/bin").iterdir()):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            continue
        name = binary.name
        args = [str(binary.relative_to(tree))] + (["one", "two", "three"] if name == "args" else [])
        env = {"OCERZ_TEST_ASYNC_STOP_MS": "200"} if name in ("interrupt_test", "link_spin") else {}
        golden = tree / "tests/guest/expect" / (name + ".out")
        cases.append(Fixture("guest-" + name, args, env, 42 if name == "exit42" else 0,
                             golden.read_bytes() if golden.exists() else None))
    if not cases:
        raise AuditError("no guest binaries; cannot audit x64")
    guest_script = (tree / "tests/run_guest_tests.sh").read_text()
    nan = re.search(r'^NAN_TESTS="([^"]+)"', guest_script, re.M)
    if not nan:
        raise AuditError("cannot find guest NaN variants")
    for name in nan[1].split():
        for variant, extra in (("nan", {}), ("replay", {"OCERZ_FPB_FORCEREPLAY": "1"})):
            cases.append(Fixture(f"guest-{name}-{variant}", [str(tree / "tests/guest/bin" / name)],
                                 dict(OCERZ_NO_AFP="1", **extra),
                                 golden=(tree / "tests/guest/expect" / (name + ".out")).read_bytes()))
    for name in ("jit_ops", "jit_rmw", "jit_bt", "atomic_unaligned", "unaligned_order"):
        cases.append(Fixture("guest-" + name + "-ordered", [str(tree / "tests/guest/bin" / name)],
                             {"OCERZ_NO_PLAIN_MEM": "1"},
                             golden=(tree / "tests/guest/expect" / (name + ".out")).read_bytes()))
    inputs = work / "inputs"
    inputs.mkdir()
    (inputs / "lines").write_text("gamma\nalpha\nbeta\nalpha\n")
    (inputs / "probe.plist").write_text('<?xml version="1.0"?><plist version="1.0">'
                                       '<dict><key>audit</key><string>yes</string></dict></plist>')
    for name, args in (("ls", ["/bin/ls", "-1", str(inputs)]),
                       ("echo", ["/bin/echo", "emission-audit"]),
                       ("sort", ["/usr/bin/sort", str(inputs / "lines")]),
                       ("sw_vers", ["/usr/bin/sw_vers"]),
                       ("plutil", ["/usr/bin/plutil", "-p", str(inputs / "probe.plist")])):
        env = {"OCERZ_NO_PLAIN_MEM": "1"} if name in ("sw_vers", "plutil") else {}
        cases.append(Fixture("dynamic-" + name, ["-cache"] + args, env, cache_coverage=True))
    dynamic = work / "dynamic"
    dynamic.mkdir()
    low = re.findall(r'^run_low_golden_case (\S+) (\S+) (\S+)',
                     (tree / "tests/run_dynamic_tests.sh").read_text(), re.M)
    if not low or not any(name.startswith("dtest_") for name, _, _ in low):
        raise AuditError("low-image corpus missing dtest_* fixtures")
    sources = [(name, source, golden, True) for name, source, golden in low]
    sources += [("dtest_jcc_gap", "tests/dynamic/test_jcc_gap.c", "tests/dynamic/test_jcc_gap.out", False),
                ("tcache_work", "tests/dynamic/tcache_work.c", None, False),
                ("avx_fp", "tests/dynamic/avx_fp.c", None, False)]
    for name, source, golden, low_image in sources:
        binary = dynamic / name
        flags = ["-Wl,-no_pie", "-Wl,-pagezero_size,0x1000", "-Wl,-image_base,0x200000000"] if low_image else []
        run(["clang", "-arch", "x86_64", "-O2", "-Itests/guest", "-o", binary, source] + flags, tree, log)
        env = {"OCERZ_TSO_NARROW": "1"} if name == "dtso_narrow_low" else {}
        if "ordered" in name:
            env["OCERZ_NO_PLAIN_MEM"] = "1"
        cases.append(Fixture("dynamic-" + name, ["-cache", str(binary)], env, cache_coverage=True))
    native = work / "native"
    native.mkdir()
    source = (tree / "tests/run_native_tests.sh").read_text()
    marker = "\nbuild_runtime_class_fixture\n"
    if source.count(marker) != 1:
        raise AuditError("cannot locate native fixture builder boundary")
    source = source.split(marker)[0]
    replacements = {'cd "$(dirname "$0")/.."': "cd " + shlex.quote(str(tree)),
                    'TMP="${TMPDIR:-/tmp}/ocerz_native.$$"': "TMP=" + shlex.quote(str(native)),
                    "trap 'rm -rf \"$TMP\"' EXIT": ""}
    for old, new in replacements.items():
        if source.count(old) != 1:
            raise AuditError(f"native fixture setup changed: {old}")
        source = source.replace(old, new)
    source += "\n" + "\n".join(("build_callback_fixtures", "build_cf_fixtures", "build_objc_fixtures",
                                 "build_objc_class_fixtures", "build_app_fixtures", "build_block_fixtures",
                                 "build_sys_fixtures")) + "\n"
    driver = work / "native-fixtures.sh"
    driver.write_text(source)
    run(["bash", driver], tree, log)
    for name in ("sys_strings", "sys_strings_fault", "sys_mmap", "sys_jmp", "objc_classes",
                 "objc_foundation", "objc_view_render", "block_runtime", "block_dispatch", "block_foundation"):
        binary = native / name
        if not binary.is_file():
            raise AuditError(f"native fixture {name} did not build; see {native / (name + '.cc.log')}")
        env = {"OCERZ_NO_PLAIN_MEM": "1"}
        cases.append(Fixture("native-" + name, ["-native", str(binary)], env))
    cases.append(Fixture("cache-sys_strings_inplace", ["-cache", str(native / "sys_strings")], cache_coverage=True))
    app = native / "OcerzApp.app/Contents/MacOS/OcerzApp"
    if not app.is_file():
        raise AuditError("native app_bundle did not build")
    cases.append(Fixture("native-app_bundle", ["-native", str(app), "extra-argument"], {"OCERZ_NO_PLAIN_MEM": "1"}))
    return cases


def capture(binary, tree, work, fixture):
    work.mkdir()
    env = {k: v for k, v in os.environ.items() if not k.startswith("OCERZ_") and k not in ("JIT_AUDIT", "CFProcessPath")}
    env.update(OCERZ_TCACHE="roundtrip", OCERZ_JIT_AUDIT_X64="1", OCERZ_APIDB=str(tree / "runtime/apis"),
               OCERZ_JIT_EMIT_AUDIT=str(work / "blocks"), LC_ALL="C", TZ="UTC")
    env.update(fixture.env)
    command = [str(binary)] + fixture.args
    error = None
    with (work / "stdout").open("wb") as out, (work / "stderr").open("wb") as err:
        proc = subprocess.Popen(command, cwd=tree, env=env, stdin=subprocess.DEVNULL,
                                stdout=out, stderr=err, start_new_session=True)
        try:
            status = proc.wait(timeout=120)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
            status = 124
    output = (work / "stdout").read_bytes()
    if status != fixture.status:
        error = f"{fixture.name}: exit {status}, expected {fixture.status}; see {work}"
    elif fixture.golden is not None and output != fixture.golden:
        error = f"{fixture.name}: guest golden differs; see {work}"
    elif re.search(rb"^[a-z_]+ bad[: ]", output, re.M):
        error = f"{fixture.name}: fixture self-check failed; see {work}"
    settings = {k: v for k, v in env.items() if k.startswith("OCERZ_") or k in ("LC_ALL", "TZ")}
    (work / "command.json").write_text(json.dumps(dict(command=command, env=settings, status=status), indent=2))
    return sorted(work.glob("blocks.*")), error


def block_sets(paths):
    blocks = {}
    if not paths:
        raise AuditError("no x64 emission streams; preserve the audit hooks")
    for path in paths:
        for block in records(path):
            key = block.rip
            metadata = (block.relocs, tuple(i[:2] for i in block.insns))
            digest = hashlib.sha256(normalized(block) + repr(metadata).encode()).digest()
            blocks.setdefault(key, {})[digest] = block
    return blocks


def compare_sets(left, right, examples, label, limit=3, mismatches=None):
    count = bad = words = 0
    for key in sorted(left.keys() | right.keys()):
        ls, rs = left.get(key, {}), right.get(key, {})
        common = ls.keys() & rs.keys()
        count += len(common)
        words += sum(len(ls[d].code) // 4 for d in common)
        for ld, rd in itertools.zip_longest(sorted(ls.keys() - rs.keys()), sorted(rs.keys() - ls.keys())):
            lblock, rblock = ls.get(ld), rs.get(rd)
            count += 1
            bad += 1
            words += len((lblock or rblock).code) // 4
            if mismatches is not None:
                mismatches.append(dict(guest_rip=hex((lblock or rblock).rip),
                                       reference_digest=ld.hex() if ld else None,
                                       candidate_digest=rd.hex() if rd else None,
                                       reference_words=len(lblock.code) // 4 if lblock else None,
                                       candidate_words=len(rblock.code) // 4 if rblock else None))
            if len(examples) < limit:
                reason = "normalized arm64/relocation variant differs" if lblock and rblock else "missing block variant"
                examples.append((label, count, [reason], lblock, rblock))
    return count, bad, words


def cache_coverage(left, right):
    coverage = [sorted(rip for rip in a.keys() - b.keys()
                       if 0x7ff000000000 <= rip < 0x800000000000)
                for a, b in ((left, right), (right, left))]
    return [{rip: variants for rip, variants in blocks.items() if rip not in omitted}
            for blocks, omitted in zip((left, right), map(set, coverage))], coverage


def audit_x64(reference, candidate, work, examples):
    cases = fixtures(reference, work)
    count = bad = words = 0
    errors, results = [], []
    for i, fixture in enumerate(cases):
        print(f"Running x64 {i + 1}/{len(cases)} {fixture.name}", file=sys.stderr, flush=True)
        sets = []
        for name in ("reference", "candidate"):
            paths, error = capture(work / name / "ocerz", reference, work / name / fixture.name, fixture)
            if error:
                errors.append(name + " " + error)
            try:
                sets.append(block_sets(paths))
            except (AuditError, OSError) as exc:
                errors.append(f"{fixture.name} {name}: {exc}")
        if len(sets) != 2:
            print(f"  {fixture.name}: INCOMPLETE", file=sys.stderr)
            continue
        coverage = [[], []]
        if fixture.cache_coverage:
            sets, coverage = cache_coverage(*sets)
            if coverage != [[], []]:
                print(f"  cache-only coverage: {len(coverage[0])} reference / {len(coverage[1])} candidate RIPs",
                      file=sys.stderr)
        details, differing = [], []
        n, nb, nw = compare_sets(*sets, details, fixture.name, mismatches=differing)
        examples.extend(details[:max(0, 3 - len(examples))])
        if details:
            with (work / (fixture.name + ".diff")).open("w") as out, contextlib.redirect_stdout(out):
                diagnostics(details)
        count += n
        bad += nb
        words += nw
        results.append(dict(fixture=fixture.name, variants=n, mismatches=nb,
                            differing_blocks=differing,
                            reference_only_cache_rips=[hex(rip) for rip in coverage[0]],
                            candidate_only_cache_rips=[hex(rip) for rip in coverage[1]]))
        print(f"  {nb}/{n} variants differ", file=sys.stderr, flush=True)
        (work / "x64-results.json").write_text(json.dumps(results, indent=2))
    return count, bad, words, errors
