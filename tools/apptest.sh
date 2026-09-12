#!/usr/bin/env bash
# apptest.sh -- launch real macOS applications and command-line tools under
# ocerz and classify what happens to each.
#
#   tools/apptest.sh                 # GUI apps + CLI tools
#   tools/apptest.sh gui             # GUI apps only
#   tools/apptest.sh cli             # CLI tools only (differential vs native)
#   tools/apptest.sh one <binary> [args...]     # a single program, verbose
#   SECS=40 tools/apptest.sh gui     # give each GUI app longer
#
# A GUI application has no output to diff, so the verdict is survival: it is
# killed after SECS and "ALIVE" means it got that far without dying. A
# command-line tool is a real differential -- its stdout must match the same
# binary run natively through Rosetta, byte for byte.
#
# uname is expected to differ: an x86_64 process must be told it is x86_64, so
# the machine field reads x86_64 under ocerz and arm64 natively. The runner
# knows about that one case and does not count it as a failure. It also knows
# every reason ocerz prints when it gives up, so a failure is never silent.
# There is no timeout(1) on a stock macOS box, hence run_bounded; the guest
# test scripts carry the same fallback.
set -u
cd "$(dirname "$0")/.."
OCERZ=./ocerz
SECS="${SECS:-20}"
[ -x "$OCERZ" ] || { echo "build first: make -j" >&2; exit 2; }

run_bounded() {
    local s=$1; shift
    "$@" & local p=$!
    ( sleep "$s"; kill -KILL $p 2>/dev/null ) >/dev/null 2>&1 & local w=$!
    wait $p 2>/dev/null; local r=$?
    kill $w 2>/dev/null; wait $w 2>/dev/null
    return $r
}

WHY='fatal|Terminating|unrecognized selector|WILD-|self-signal|unimplemented BSD|GUEST-ABORT'
pass=0; fail=0; skip=0

has_x86() { lipo -archs "$1" 2>/dev/null | grep -q x86_64; }

gui_one() {
    local name="$1" bin="$2"
    [ -f "$bin" ] || { printf '  %-22s SKIP  (no binary)\n' "$name"; skip=$((skip+1)); return; }
    has_x86 "$bin" || { printf '  %-22s SKIP  (arm64-only)\n' "$name"; skip=$((skip+1)); return; }
    run_bounded "$SECS" "$OCERZ" "$bin" >/tmp/apptest.out 2>/tmp/apptest.err
    local rc=$? why
    why=$(grep -aoE "$WHY.*" /tmp/apptest.err | tail -1 | cut -c1-72)
    if [ "$rc" = 137 ]; then
        printf '  %-22s ALIVE at %ss\n' "$name" "$SECS"; pass=$((pass+1))
    else
        printf '  %-22s DIED  rc=%-4s %s\n' "$name" "$rc" "$why"; fail=$((fail+1))
    fi
}

cli_one() {
    local bin="$1"; shift
    [ -x "$bin" ] || { printf '  %-30s SKIP\n' "$bin $*"; skip=$((skip+1)); return; }
    has_x86 "$bin" || { printf '  %-30s SKIP (arm64-only)\n' "$bin $*"; skip=$((skip+1)); return; }
    local native got rc
    native=$("$bin" "$@" 2>/dev/null)
    got=$(run_bounded "$SECS" "$OCERZ" "$bin" "$@" 2>/tmp/apptest.err); rc=$?
    if [ "$bin" = /usr/bin/uname ]; then
        native=${native%arm64}; got=${got%x86_64}
    fi
    if [ "$native" = "$got" ]; then
        printf '  %-30s PASS\n' "$bin $*"; pass=$((pass+1))
    else
        printf '  %-30s FAIL  rc=%s  %s\n' "$bin $*" "$rc" \
               "$(grep -aoE "$WHY.*" /tmp/apptest.err | tail -1 | cut -c1-60)"
        fail=$((fail+1))
    fi
}

do_gui() {
    echo "== GUI applications (${SECS}s each) =="
    local app base bin
    for app in /System/Applications/*.app /System/Applications/Utilities/*.app /Applications/*.app; do
        [ -d "$app" ] || continue
        base=$(basename "$app" .app)
        bin="$app/Contents/MacOS/$base"
        [ -f "$bin" ] || continue
        gui_one "$base" "$bin"
    done
}

do_cli() {
    echo "== command-line tools (stdout must match native) =="
    cli_one /usr/bin/uname -a
    cli_one /usr/bin/sw_vers
    cli_one /bin/echo hello world
    cli_one /bin/ls /usr
    cli_one /usr/bin/id -u
    cli_one /usr/bin/basename /a/b/c.txt
    cli_one /usr/bin/wc -l README.md
    cli_one /usr/bin/sort README.md
    cli_one /usr/bin/uniq README.md
    cli_one /usr/bin/head -3 README.md
    cli_one /usr/bin/grep -c include Makefile
    cli_one /usr/bin/file /bin/ls
    cli_one /usr/bin/xxd -l 32 Makefile
    cli_one /usr/bin/nm -gU /usr/lib/libSystem.B.dylib
    cli_one /usr/bin/openssl version
    cli_one /usr/bin/plutil -p /System/Library/CoreServices/SystemVersion.plist
}

case "${1:-all}" in
  one)  shift; echo "== $* =="
        run_bounded "$SECS" "$OCERZ" "$@"; echo "rc=$?"; exit 0 ;;
  gui)  do_gui ;;
  cli)  do_cli ;;
  all)  do_gui; echo; do_cli ;;
  *)    sed -n '2,16p' "$0"; exit 2 ;;
esac

echo "----------------------------------------"
echo "apptest: $pass ok, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
