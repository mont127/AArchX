#!/usr/bin/env bash
# AArchX.app's CrossOver functions.  What they write is shell scripts: two that
# stand in for CrossOver's wineloader and wineserver, and one in place of each
# launcher of a switched bottle.  app/AArchX.m is built with AARCHX_APP_TEST,
# which swaps the window for a command line over those functions, and the
# scripts it writes are run here against a CrossOver and a home that are both
# made in a temporary directory: stand-ins that print their own name and their
# arguments, one to a line.  Nothing of the real home is read or written
# (CFFIXED_USER_HOME and HOME), CrossOver is the CrossOverPath setting, and the
# command the launchers carry names a bottle that does not exist, so a real
# CrossOver could do nothing with it if a mistake here ever reached one.
#
# The cases are the ways the scripts went wrong on a real Mac: a bottle that
# went on running under the ocerz of whichever AArchX had switched it, however
# old, and launchers that died at once because CrossOver's command named the
# place CrossOver had run from before it was moved to Applications.
set -u
cd "$(dirname "$0")/.."
repo=$PWD
work=$(mktemp -d /tmp/ocerz-app.XXXXXX)
work=$(cd "$work" && pwd -P)
fail=0
passed=0

pass() { echo "PASS app $1"; passed=$((passed + 1)); }
flunk() { echo "FAIL app $1: $2"; fail=$((fail + 1)); }
expect() { # name, wanted, got
    if [ "$2" = "$3" ]; then pass "$1"; else flunk "$1" "wanted '$(echo "$2" | tr '\n' '|')' got '$(echo "$3" | tr '\n' '|')'"; fi
}

if ! clang -arch arm64 -fobjc-arc -O1 -Wall -Wextra -Wno-unused-parameter -DAARCHX_APP_TEST -framework AppKit \
        -o "$work/apptest" app/AArchX.m 2> "$work/build.log"; then
    echo "FAIL app test build: $(head -3 "$work/build.log" | tr '\n' ' ')"
    exit 1
fi

home="$work/home"
cx="$work/cx/CrossOver.app"
cxbin="$cx/Contents/SharedSupport/CrossOver/bin"
bottle="$home/Library/Application Support/CrossOver/Bottles/Game"
shims="$home/Library/Application Support/AArchX/CrossOver/bin"
shims_native="$shims-native"
launcher="$home/Applications/CrossOver/Game/Prog.app"
command="$bottle/desktopdata/cxmenu/StartMenu.C^5E3A_users/Programs/Some Game/Prog.lnk"
mkdir -p "$home" "$cxbin" "$bottle" "$launcher/Contents/MacOS" "$(dirname "$command")"

stand_in() { # path, name: a program that prints its name and its arguments, one to a line
    printf '#!/bin/sh\necho "%s"\nfor a in "$@"; do echo "$a"; done\n' "$2" > "$1"
    chmod 755 "$1"
}
stand_in "$cxbin/wine" WINE
stand_in "$cxbin/wineloader" REAL-WINELOADER
stand_in "$cxbin/wineserver" REAL-WINESERVER
stand_in "$work/ocerz-a" OCERZ-A
stand_in "$work/ocerz-b" OCERZ-B

app() { env CFFIXED_USER_HOME="$home" HOME="$home" "$work/apptest" "$@" -CrossOverPath "$cx"; }
write_command() { # the wine it names
    printf '#!/bin/sh\nexec "%s" --bottle "aarchx-test-no-such-bottle" --check --wait-children --start "C:/Program Files/Some Game/game.exe" "$@"\n' "$1" > "$command"
    chmod 755 "$command"
}
run_launcher() { env HOME="$home" "$launcher/Contents/MacOS/Menu Helper" "$@" 2>&1; }
wanted_wine=$(printf 'WINE\n--bottle\naarchx-test-no-such-bottle\n--check\n--wait-children\n--start\nC:/Program Files/Some Game/game.exe')

# ---- the two scripts in front of CrossOver's wineloader and wineserver
app shims "$work/ocerz-a" || flunk "shims written" "the app could not write them"
got=$(env HOME="$home" CX_ROOT="${cxbin%/bin}" "$shims/wineloader" one "two words" 2>&1)
expect "wineloader runs under the ocerz that wrote it, arguments whole" \
    "$(printf 'OCERZ-A\n--\n%s/wineloader\none\ntwo words' "$cxbin")" "$got"
got=$(env HOME="$home" CX_ROOT="${cxbin%/bin}" "$shims/wineserver" -k 2>&1)
expect "wineserver runs under it too" "$(printf 'OCERZ-A\n--\n%s/wineserver\n-k' "$cxbin")" "$got"
got=$(env HOME="$home" CX_ROOT="${cxbin%/bin}" "$shims_native/wineloader" one "two words" 2>&1)
expect "the second pair of scripts runs it under ocerz -native" \
    "$(printf 'OCERZ-A\n-native\n--\n%s/wineloader\none\ntwo words' "$cxbin")" "$got"
got=$(env HOME="$home" CX_ROOT="${cxbin%/bin}" "$shims_native/wineserver" -k 2>&1)
expect "its wineserver too" "$(printf 'OCERZ-A\n-native\n--\n%s/wineserver\n-k' "$cxbin")" "$got"

mv "$work/ocerz-a" "$work/ocerz-a.gone"
mkdir -p "$home/.aarchx"
stand_in "$home/.aarchx/ocerz" OCERZ-INSTALLED
got=$(env HOME="$home" CX_ROOT="${cxbin%/bin}" "$shims/wineloader" x 2>&1)
expect "an ocerz that has gone gives way to an installed one" "$(printf 'OCERZ-INSTALLED\n--\n%s/wineloader\nx' "$cxbin")" "$got"
rm -rf "$home/.aarchx"
installed=""
for o in /Applications/AArchX.app/Contents/Resources/aarchx/ocerz /opt/homebrew/bin/ocerz /usr/local/bin/ocerz; do
    [ -x "$o" ] && installed=$o
done
if [ -n "$installed" ]; then
    echo "SKIP app with no ocerz anywhere the bottle runs as CrossOver runs it (this Mac has $installed)"
else
    got=$(env HOME="$home" CX_ROOT="${cxbin%/bin}" "$shims/wineloader" x 2>&1)
    expect "with no ocerz anywhere the bottle runs as CrossOver runs it" "$(printf 'REAL-WINELOADER\nx')" "$got"
fi
mv "$work/ocerz-a.gone" "$work/ocerz-a"

# ---- the bottle's BinPath
printf '[Bottle]\n"Description" = "a game"\n\n[Wine]\n"Version" = "win10_64"\n' > "$bottle/cxbottle.conf"
cp "$bottle/cxbottle.conf" "$work/conf.before"
app uses "$bottle/cxbottle.conf" && flunk "a fresh bottle is not switched" "it reads as switched"
app bottle "$bottle/cxbottle.conf" on || flunk "bottle switched on" "refused"
if app uses "$bottle/cxbottle.conf" &&
   grep -q '^"BinPath" = "/.*/home/Library/Application Support/AArchX/CrossOver/bin:${CX_ROOT}/bin"$' "$bottle/cxbottle.conf"; then
    pass "switching a bottle on sets its BinPath to the two scripts and CrossOver's own bin"
else
    flunk "switching a bottle on sets its BinPath" "$(grep BinPath "$bottle/cxbottle.conf")"
fi
if app native "$bottle/cxbottle.conf"; then flunk "a bottle switched on" "reads as native without being asked"; else pass "a bottle switched on is in cache mode"; fi
app bottle "$bottle/cxbottle.conf" on native || flunk "bottle switched to native" "refused"
if app uses "$bottle/cxbottle.conf" && app native "$bottle/cxbottle.conf" && [ "$(grep -c BinPath "$bottle/cxbottle.conf")" = 1 ] &&
   grep -q '^"BinPath" = "/.*/home/Library/Application Support/AArchX/CrossOver/bin-native:${CX_ROOT}/bin"$' "$bottle/cxbottle.conf"; then
    pass "Native points the bottle's one BinPath at the scripts that pass -native"
else
    flunk "Native" "$(grep BinPath "$bottle/cxbottle.conf")"
fi
app bottle "$bottle/cxbottle.conf" on
if app uses "$bottle/cxbottle.conf" && ! app native "$bottle/cxbottle.conf"; then pass "unticking Native leaves the bottle under AArchX in cache mode"; else flunk "unticking Native" "$(grep BinPath "$bottle/cxbottle.conf")"; fi
app bottle "$bottle/cxbottle.conf" on native
app bottle "$bottle/cxbottle.conf" off
if cmp -s "$bottle/cxbottle.conf" "$work/conf.before"; then pass "switching it off leaves cxbottle.conf as it was"; else flunk "switching off" "cxbottle.conf differs"; fi
printf '[Wine]\n"BinPath" = "/somewhere/else"\n' > "$work/other.conf"
cp "$work/other.conf" "$work/other.before"
if app bottle "$work/other.conf" on; then flunk "a BinPath someone else set" "was overwritten"; else
    if cmp -s "$work/other.conf" "$work/other.before"; then pass "a BinPath someone else set is refused and left alone"; else flunk "a BinPath someone else set" "file changed"; fi
fi
app bottle "$bottle/cxbottle.conf" on

# ---- the launchers
cat > "$launcher/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>Menu Helper</string>
<key>CXHelperAppBottleName</key><string>Game</string>
<key>CrossOverHelperCommand</key><string>"$command"</string>
</dict></plist>
EOF
stand_in "$launcher/Contents/MacOS/Menu Helper" ORIGINAL
cp "$launcher/Contents/MacOS/Menu Helper" "$work/original.before"
write_command "$cxbin/wine"
app launchers Game on || flunk "launchers switched" "refused"
if grep -q "^# Written by AArchX.app" "$launcher/Contents/MacOS/Menu Helper" &&
   cmp -s "$launcher/Contents/MacOS/Menu Helper.cx-original" "$work/original.before"; then
    pass "a switched launcher is a script, with CrossOver's own beside it"
else
    flunk "a switched launcher" "is not the script, or the original was not kept"
fi
expect "opened with no arguments it runs CrossOver's command" "$wanted_wine" "$(run_launcher)"
expect "a process serial number from Finder counts as no arguments" "$wanted_wine" "$(run_launcher -psn_0_12345)"
expect "with arguments CrossOver's own launcher gets them" "$(printf 'ORIGINAL\n--some\nargument given')" "$(run_launcher --some "argument given")"

write_command "$work/private/var/folders/xx/T/AppTranslocation/ABCD/d/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine"
got=$(run_launcher); rc=$?
expect "a command naming a CrossOver that has moved goes to the one that is here" "$wanted_wine" "$got"
[ "$rc" = 0 ] || flunk "a command naming a CrossOver that has moved" "exit $rc"

mv "$command" "$command.gone"
expect "with no command at all CrossOver's own launcher runs" "ORIGINAL" "$(run_launcher)"
mv "$command.gone" "$command"

stand_in "$launcher/Contents/MacOS/Menu Helper" ORIGINAL-WRITTEN-AGAIN
cp "$launcher/Contents/MacOS/Menu Helper" "$work/original.again"
if app launchers Game on && grep -q "^# Written by AArchX.app" "$launcher/Contents/MacOS/Menu Helper" &&
   cmp -s "$launcher/Contents/MacOS/Menu Helper.cx-original" "$work/original.again"; then
    pass "a launcher CrossOver wrote again is switched again, and is the original from then on"
else
    flunk "a launcher CrossOver wrote again" "was not switched, or the older original was kept"
fi
app launchers Game off
if cmp -s "$launcher/Contents/MacOS/Menu Helper" "$work/original.again" && [ ! -e "$launcher/Contents/MacOS/Menu Helper.cx-original" ]; then
    pass "switching off puts CrossOver's launcher back byte for byte"
else
    flunk "switching off" "the launcher is not CrossOver's own again"
fi

# ---- every start of the app writes the scripts again
app refresh -OcerzPath "$work/ocerz-b"
got=$(env HOME="$home" CX_ROOT="${cxbin%/bin}" "$shims/wineloader" y 2>&1)
expect "a start of the app points the bottles at its own ocerz" "$(printf 'OCERZ-B\n--\n%s/wineloader\ny' "$cxbin")" "$got"
if grep -q "^# Written by AArchX.app" "$launcher/Contents/MacOS/Menu Helper"; then
    pass "and switches the launchers of a switched bottle again"
else
    flunk "a start of the app" "left a switched bottle's launcher as CrossOver wrote it"
fi
fresh="$work/fresh-home"
mkdir -p "$fresh"
env CFFIXED_USER_HOME="$fresh" HOME="$fresh" "$work/apptest" refresh -OcerzPath "$work/ocerz-b" -CrossOverPath "$cx"
if [ -e "$fresh/Library" ]; then flunk "a start of the app with no switched bottle" "wrote into the home"; else pass "a start of the app writes nothing for someone who never switched a bottle"; fi

# ---- how a run in native mode ended: the exit code alone does not say, ocerz's own last lines do
printf 'a program that\nexits 71 by itself\n' > "$work/own71.log"
printf 'ocerz: native: no bridge for _swift_once in /usr/lib/swift/libswiftCore.dylib\nocerz: native: 50 unresolved imports, which no virtual library exports\nocerz: native: to run it translated instead, supply an x86_64 build\n' > "$work/refused.log"
printf 'the program printed this\nocerz: bridge: /usr/lib/libSystem.B.dylib _stat64 not implemented\nocerz:   called from 0x7c6e08be5d\n' > "$work/stopped.log"
if app log "$work/refused.log" "unresolved imports, which no virtual library exports" && app log "$work/stopped.log" " not implemented" &&
   ! app log "$work/own71.log" "unresolved imports, which no virtual library exports" && ! app log "$work/own71.log" " not implemented" &&
   ! app log "$work/no-such.log" " not implemented"; then
    pass "a refusal and a missing call are told from a program's own exit by ocerz's last lines"
else
    flunk "reading the end of a run's log" "took one kind of ending for another"
fi

# ---- a copy of the app that will not stay where it is
if app passing "/private/var/folders/xx/T/AppTranslocation/ABCD/d/AArchX.app/Contents/Resources/aarchx/ocerz"; then
    pass "an ocerz inside a translocated copy counts as passing"
else
    flunk "a translocated copy" "counts as one that stays"
fi
if app passing "$work/ocerz-a"; then flunk "an ocerz on an ordinary disk" "counts as passing"; else pass "an ocerz on an ordinary disk stays"; fi
mkdir -p "$work/image/AArchX.app" "$work/mount"
cp "$work/ocerz-a" "$work/image/AArchX.app/ocerz"
if hdiutil create -quiet -srcfolder "$work/image" -fs HFS+ -format UDZO -ov "$work/image.dmg" 2> /dev/null &&
   hdiutil attach -quiet -nobrowse -readonly -mountpoint "$work/mount" "$work/image.dmg" 2> /dev/null; then
    if app passing "$work/mount/AArchX.app/ocerz"; then pass "an ocerz on an opened disk image counts as passing"; else flunk "an opened disk image" "counts as a place that stays"; fi
    hdiutil detach -quiet "$work/mount" 2> /dev/null
else
    echo "SKIP app disk image case (hdiutil could not make or open an image)"
fi

echo "app tests: $passed passed, $fail failed; work: $work"
[ "$fail" = 0 ]
