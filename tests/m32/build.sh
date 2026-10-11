#!/bin/bash
# Builds the i386 (m32) test programs. The macOS SDK has no i386 libraries any more, so the stub .tbd
# files the linker needs are generated from the programs' own undefined symbols, each assigned to the SDK library
# whose .tbd lists it (libSystem otherwise). A program's extra link arguments live in <name>.link (one line).
set -euo pipefail
export DEVELOPER_DIR=${M32_DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}   # Xcode 26's ld still links i386 (-ld_classic); CLT 27's and Xcode-beta's do not
cd "$(dirname "$0")"
SDK=$(xcrun --show-sdk-path)
CFLAGS=(-arch i386 -mmacosx-version-min=10.8 -isysroot "$SDK" -O1 -g -Wno-deprecated)
mkdir -p bin obj sdk/usr/lib sdk/Frameworks
for f in *.c; do [ -e "$f" ] || continue; xcrun clang "${CFLAGS[@]}" -c "$f" -o "obj/${f%.*}.o"; done
for f in *.m; do [ -e "$f" ] || continue; xcrun clang "${CFLAGS[@]}" -include m32_prelude.h -c "$f" -o "obj/${f%.*}.o"; done
for f in *.cpp; do [ -e "$f" ] || continue; xcrun clang++ "${CFLAGS[@]}" -c "$f" -o "obj/${f%.*}.o"; done
for f in *.s; do [ -e "$f" ] || continue; xcrun clang -arch i386 -c "$f" -o "obj/${f%.*}.o"; done
syms=$(for o in obj/*.o; do xcrun nm -u "$o"; done | sort -u | grep -vE '^_(m32t_|m32dl_|main$)' | grep -vE '^(__Z|___gxx_personality|___cxa_(demangle|throw|rethrow|allocate_exception|free_exception|begin_catch|end_catch|call_unexpected|guard_))' || true)
# each import goes to the SDK library whose .tbd lists it (classes by objc-classes), else libSystem
echo "$syms" > obj/imports.txt
python3 - "$SDK" obj/imports.txt <<'PY'
import os, re, sys
sdk = sys.argv[1]
libs = [  # stub path, install name, SDK tbd
    ("sdk/usr/lib/libobjc.tbd", "/usr/lib/libobjc.A.dylib", "usr/lib/libobjc.A.tbd"),
    ("sdk/Frameworks/CoreFoundation.framework/CoreFoundation.tbd", "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "System/Library/Frameworks/CoreFoundation.framework/CoreFoundation.tbd"),
    ("sdk/Frameworks/Foundation.framework/Foundation.tbd", "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation", "System/Library/Frameworks/Foundation.framework/Foundation.tbd"),
    ("sdk/Frameworks/AppKit.framework/AppKit.tbd", "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit", "System/Library/Frameworks/AppKit.framework/AppKit.tbd"),
    ("sdk/Frameworks/CoreGraphics.framework/CoreGraphics.tbd", "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics", "System/Library/Frameworks/CoreGraphics.framework/CoreGraphics.tbd"),
    ("sdk/Frameworks/CoreServices.framework/CoreServices.tbd", "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices", "System/Library/Frameworks/CoreServices.framework/CoreServices.tbd"),
    ("sdk/Frameworks/AudioToolbox.framework/AudioToolbox.tbd", "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox", "System/Library/Frameworks/AudioToolbox.framework/AudioToolbox.tbd"),
]
def lists(text, key):
    out = set()
    for m in re.finditer(key + r":\s*\[(.*?)\]", text, re.S):
        out.update(w.strip().strip("'\"") for w in m.group(1).replace("\n", " ").split(","))
    return out
owned = {l[0]: [] for l in libs}
system = []
index = []
for stub, inst, tbd in libs:
    t = open(os.path.join(sdk, tbd)).read()
    index.append((stub, lists(t, "symbols"), lists(t, "objc-classes")))
for sym in open(sys.argv[2]).read().split():
    for stub, syms, classes in index:
        if sym in syms or (sym.startswith(".objc_class_name_") and sym[17:] in classes):
            owned[stub].append(sym); break
    else:
        system.append(sym)
def write(path, inst, names):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    body = ", ".join("'%s'" % n for n in names) if names else "'_m32_placeholder_" + os.path.basename(path).split(".")[0] + "'"
    open(path, "w").write("--- !tapi-tbd\ntbd-version: 4\ntargets: [ i386-macos ]\ninstall-name: '%s'\ncurrent-version: 1000\nexports:\n  - targets: [ i386-macos ]\n    symbols: [ %s ]\n...\n" % (inst, body))
write("sdk/usr/lib/libSystem.tbd", "/usr/lib/libSystem.B.dylib", system + ["dyld_stub_binder"])
for stub, inst, tbd in libs:
    write(stub, inst, owned[stub])
PY
LD=(-arch i386 -mmacosx-version-min=10.8 -nostdlib -Wl,-ld_classic -Lsdk/usr/lib -Fsdk/Frameworks -lSystem)
OBJC=(-lobjc -framework CoreFoundation -framework Foundation -framework AppKit -framework CoreGraphics -framework CoreServices)
# guest dylibs (lib*.c) are built first so programs can link against them
for f in lib*.c; do [ -e "$f" ] || continue
  xcrun clang "${LD[@]}" -dynamiclib -install_name "@executable_path/${f%.c}.dylib" "obj/${f%.c}.o" -o "bin/${f%.c}.dylib" 2>&1 | grep -vE "ld_classic is deprecated|i386 architecture is deprecated" || true; done
for f in *.c *.cpp *.m; do [ -e "$f" ] || continue; n=${f%.*}; case $n in lib*) continue;; esac
  extra=(); [ -f "$n.link" ] && read -ra extra < "$n.link"
  xcrun clang "${LD[@]}" "obj/$n.o" "${OBJC[@]}" -Wl,-dead_strip_dylibs ${extra[@]+"${extra[@]}"} -o "bin/$n" 2>&1 | grep -vE "ld_classic is deprecated|i386 architecture is deprecated" || true
  [ -f "bin/$n" ] || { echo "build.sh: link failed for $n" >&2; exit 1; }; done
