#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
C_TREE="${C_TREE:-$HOME/AArchX-c}"
RUST_TREE="${RUST_TREE:-$ROOT}"
C_BIN="$C_TREE/ocerz"
RUST_BIN="$RUST_TREE/ocerz"
STATIC_GUEST="${STATIC_GUEST:-$ROOT/tests/guest/bin/hello}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/main_parity.XXXXXX")"
trap 'rc=$?; if [ "$rc" -eq 0 ]; then rm -rf "$TMP"; else echo "parity artifacts: $TMP" >&2; fi' EXIT

for tree in "$C_TREE" "$RUST_TREE"; do
    if ! (cd "$tree" && make -s ocerz) >"$TMP/build.log" 2>&1; then
        cat "$TMP/build.log" >&2
        echo "build failed in $tree" >&2
        exit 1
    fi
done

if [ ! -f "$STATIC_GUEST" ]; then
    echo "static guest not found: $STATIC_GUEST" >&2
    exit 1
fi
dd if="$STATIC_GUEST" of="$TMP/truncated" bs=1 count=64 2>/dev/null
mkdir -p "$TMP/Hello.app/Contents/MacOS"
cp "$STATIC_GUEST" "$TMP/Hello.app/Contents/MacOS/Hello"
printf '%s\n' \
    '<?xml version="1.0" encoding="UTF-8"?>' \
    '<plist version="1.0"><dict><key>CFBundleExecutable</key><string>Hello</string></dict></plist>' \
    >"$TMP/Hello.app/Contents/Info.plist"

compare_case() {
    local name=$1
    shift
    local -a env_specs=()
    while [ "${1:-}" != "--" ]; do
        env_specs+=("$1")
        shift
    done
    shift
    local -a args=("$@")

    for label in c rust; do
        local bin tree err_file rc
        if [ "$label" = c ]; then
            bin=$C_BIN
            tree=$C_TREE
        else
            bin=$RUST_BIN
            tree=$RUST_TREE
        fi
        err_file="$TMP/${name}_${label}.stderr_file"
        : >"$err_file"
        local -a env_cmd=(env -i "HOME=$HOME" "PATH=$PATH" "TMPDIR=$TMP" "OCERZ_TCACHE=off")
        local spec
        for spec in "${env_specs[@]}"; do
            spec="${spec//@ERRFILE@/$err_file}"
            env_cmd+=("$spec")
        done
        set +e
        (cd "$tree" && "${env_cmd[@]}" "$bin" "${args[@]}") \
            >"$TMP/${name}_${label}.out" 2>"$TMP/${name}_${label}.err"
        rc=$?
        set -e
        printf '%s\n' "$rc" >"$TMP/${name}_${label}.rc"
        cp "$err_file" "$TMP/${name}_${label}.stderr_file.out"
        python3 - "$TMP/${name}_${label}.out" "$TMP/${name}_${label}.err" \
            "$TMP/${name}_${label}.stderr_file.out" "$C_BIN" "$RUST_BIN" <<'PY'
from pathlib import Path
import sys

paths = [Path(p) for p in sys.argv[1:4]]
binary_paths = [p.encode() for p in sys.argv[4:]]
for path in paths:
    data = path.read_bytes()
    for binary in binary_paths:
        data = data.replace(binary, b"<ocerz>")
    path.write_bytes(data)
PY
    done

    local mismatch=0
    for suffix in out err rc stderr_file.out; do
        if ! cmp -s "$TMP/${name}_c.$suffix" "$TMP/${name}_rust.$suffix"; then
            mismatch=1
        fi
    done
    if [ "$mismatch" -ne 0 ]; then
        if [ "$name" = native-echo ] \
            && [ "$(tr -d '\n' <"$TMP/native-echo_c.rc")" = 71 ] \
            && [ "$(tr -d '\n' <"$TMP/native-echo_rust.rc")" = 0 ] \
            && rg -q 'native: [0-9]+ unresolved imports' "$TMP/native-echo_c.err" \
            && cmp -s "$TMP/native-echo_rust.out" <(printf 'hi\n') \
            && [ ! -s "$TMP/native-echo_rust.err" ]; then
            echo "KNOWN BASELINE DIVERGENCE: native-echo (C dyld lacks bridges; Rust runs successfully)"
            return 0
        fi
        for suffix in out err rc stderr_file.out; do
            if ! cmp -s "$TMP/${name}_c.$suffix" "$TMP/${name}_rust.$suffix"; then
                echo "PARITY MISMATCH: $name ($suffix)" >&2
                diff -u "$TMP/${name}_c.$suffix" "$TMP/${name}_rust.$suffix" >&2 || true
            fi
        done
        return 1
    fi
    printf 'PARITY MATCH: %s exit=%s\n' "$name" "$(tr -d '\n' <"$TMP/${name}_c.rc")"
}

compare_case no-args -- 
compare_case version -- version
compare_case unknown-flag -- -unknown
compare_case missing-file -- "$TMP/missing-image"
compare_case non-mach-o -- /etc/hosts
compare_case truncated-image -- "$TMP/truncated"
compare_case verbose-static -- -no-jit -v "$STATIC_GUEST"
compare_case trace-static -- -no-jit -trace "$STATIC_GUEST"
compare_case no-jit-static -- -no-jit "$STATIC_GUEST"
compare_case native-echo -- -native /bin/echo hi
compare_case cache-echo -- -cache /bin/echo hi
compare_case mode-bogus OCERZ_MODE=bogus -- /bin/echo hi
compare_case double-dash -- -- /bin/echo -- marker
compare_case stderr-file OCERZ_STDERR_FILE=@ERRFILE@ --
compare_case exe-env OCERZ_EXE_ENV=env:OCERZ_PARITY_MARK=ok -- /usr/bin/env
compare_case nojit-exe OCERZ_NOJIT_EXE=echo -- /bin/echo hi
compare_case strace-exe OCERZ_STRACE_EXE=hello -- -no-jit "$STATIC_GUEST"
compare_case bundle -- "$TMP/Hello.app"

python3 - "$C_BIN" "$RUST_BIN" <<'PY'
import os
import subprocess
import sys
import time

for label, binary in (("c", sys.argv[1]), ("rust", sys.argv[2])):
    env = {
        "HOME": os.environ["HOME"],
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "OCERZ_TCACHE": "off",
    }
    start = time.monotonic_ns()
    for _ in range(20):
        subprocess.run(
            [binary, "/bin/echo", "hi"], env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True,
        )
    elapsed = time.monotonic_ns() - start
    print(f"startup tree={label} runs=20 ms_per_start={elapsed / 20 / 1e6:.3f}")
PY
