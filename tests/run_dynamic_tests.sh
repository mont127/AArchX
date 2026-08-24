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

TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN="gtimeout"
fi
DYNAMIC_TIMEOUT=30

run_bounded() {
    local out_file="$1" err_file="$2"
    shift 2
    if [ -n "$TIMEOUT_BIN" ]; then
        "$TIMEOUT_BIN" "${DYNAMIC_TIMEOUT}s" "$@" >"$out_file" 2>"$err_file"
        return $?
    fi
    "$@" >"$out_file" 2>"$err_file" &
    local pid=$!
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$DYNAMIC_TIMEOUT" ]; then
            kill -TERM "$pid" 2>/dev/null
            sleep 1
            kill -KILL "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            return 124
        fi
        sleep 1
        waited=$((waited + 1))
    done
    wait "$pid"
    return $?
}

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

run_file_case() {
    local name="$1" src="$2" want_out="$3"
    if ! clang -arch x86_64 -pthread -o "$TMP/$name" "$src" 2>/dev/null; then
        echo "FAIL $name (compile)"; fail=$((fail+1)); return
    fi

    local mode got_out got_code out_file err_file
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$TMP/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$TMP/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"
            fail=$((fail+1))
        fi
    done
}

run_case dret 'int main(void){return 42;}' '' 42
run_case dwrite '
int main(void){
  const char m[]="dyn raw syscall ok\n"; long r;
  __asm__ volatile("syscall":"=a"(r):"a"(0x2000004),"D"(1),"S"(m),"d"(sizeof(m)-1):"rcx","r11","memory");
  return 9;
}' 'dyn raw syscall ok' 9

run_case dlinkver '
#include <stdint.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
extern const struct mach_header_64 _mh_execute_header;
static int same(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}
int main(void) {
  const struct mach_header_64 *h = &_mh_execute_header;
  const unsigned char *p = (const unsigned char *)(h + 1);
  uint32_t expected_link = 0, expected_runtime = 0;
  for (uint32_t i = 0; i < h->ncmds; ++i) {
    const struct load_command *lc = (const void *)p;
    if (lc->cmd == LC_LOAD_DYLIB || lc->cmd == LC_LOAD_WEAK_DYLIB ||
        lc->cmd == LC_REEXPORT_DYLIB || lc->cmd == LC_LOAD_UPWARD_DYLIB) {
      const struct dylib_command *dc = (const void *)p;
      const char *name = (const char *)p + dc->dylib.name.offset;
      if (same(name, "/usr/lib/libSystem.B.dylib"))
        expected_link = dc->dylib.current_version;
    }
    p += lc->cmdsize;
  }
  for (uint32_t image = 0; image < _dyld_image_count(); ++image) {
    h = (const struct mach_header_64 *)_dyld_get_image_header(image);
    if (!h)
      continue;
    p = (const unsigned char *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; ++i) {
      const struct load_command *lc = (const void *)p;
      if (lc->cmd == LC_ID_DYLIB) {
        const struct dylib_command *dc = (const void *)p;
        const char *name = (const char *)p + dc->dylib.name.offset;
        if (same(name, "/usr/lib/libSystem.B.dylib"))
          expected_runtime = dc->dylib.current_version;
      }
      p += lc->cmdsize;
    }
  }
  int32_t got_link = NSVersionOfLinkTimeLibrary("System");
  int32_t got_runtime = NSVersionOfRunTimeLibrary("System");
  return expected_link && expected_runtime &&
         (uint32_t)got_link == expected_link &&
         (uint32_t)got_runtime == expected_runtime ? 0 : 91;
}' '' 0

run_file_case dfork_signal tests/dynamic/fork_signal.c 'fork signal ok'

echo "----------------------------------------"
echo "dynamic tests: $pass passed, $fail failed"
echo "KNOWN-PENDING: libc-call programs (printf) reach cache code but need the libSystem-initializer phase"
[ "$fail" -eq 0 ]
