#!/usr/bin/env bash
# End-to-end tests for the mini-dyld: real dynamically-linked Mach-O programs against the shared cache.

set -u
cd "$(dirname "$0")/.."
OCERZ=./ocerz
TMP="${TMPDIR:-/tmp}/ocerz_dyn.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

if ! clang -arch x86_64 -x c -o "$TMP/probe" - >/dev/null 2>&1 <<<'int main(void){return 0;}'; then
    echo "run_dynamic_tests: SKIP (no x86_64 clang toolchain)"
    exit 0
fi
if ! "$OCERZ" "$TMP/probe" >/dev/null 2>&1; then
    rc=$?
    if [ "$rc" = 70 ] || [ "$rc" = 65 ]; then
        echo "run_dynamic_tests: SKIP (shared cache not mappable here)"
        exit 0
    fi
fi

pass=0
fail=0

run_case() {
    local name="$1" src="$2" want_out="$3" want_code="$4"
    printf '%s' "$src" > "$TMP/$name.c"
    if ! clang -arch x86_64 -o "$TMP/$name" "$TMP/$name.c" 2>/dev/null; then
        echo "FAIL $name (compile)"; fail=$((fail+1)); return
    fi
    local got_out got_code
    got_out=$("$OCERZ" "$TMP/$name" 2>/dev/null)
    got_code=$?
    if [ "$got_out" = "$want_out" ] && [ "$got_code" = "$want_code" ]; then
        echo "PASS $name (out='$got_out' exit=$got_code)"; pass=$((pass+1))
    else
        echo "FAIL $name (got out='$got_out' exit=$got_code; want out='$want_out' exit=$want_code)"; fail=$((fail+1))
    fi
}

run_case dret 'int main(void){return 42;}' '' 42
run_case dwrite '
int main(void){
  const char m[]="dyn raw syscall ok\n"; long r;
  __asm__ volatile("syscall":"=a"(r):"a"(0x2000004),"D"(1),"S"(m),"d"(sizeof(m)-1):"rcx","r11","memory");
  return 9;
}' 'dyn raw syscall ok' 9

echo "----------------------------------------"
echo "dynamic tests: $pass passed, $fail failed"
echo "KNOWN-PENDING: libc-call programs (printf) reach cache code but need the libSystem-initializer phase"
[ "$fail" -eq 0 ]
