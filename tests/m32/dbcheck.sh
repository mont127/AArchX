#!/bin/bash
# Spot checks of m32's generated i386 database (runtime/apis32, make apis32).
set -euo pipefail
cd "$(dirname "$0")/../.."
d=runtime/apis32/macos/$(ls runtime/apis32/macos | tail -1)
want() { grep -qxF "$2" "$d/$1.api" || { echo "missing in $1: $2"; grep -E "^[a-z0-9]+ $(echo "$2" | awk '{print $2}') " "$d/$1.api" | sed 's/^/  have: /'; exit 1; }; }
want libSystem.B.dylib 'fn32 _strlen strlen u(s) L(p)'
want libSystem.B.dylib 'fn32 _sinf sinf f(f) f(f)'
want libSystem.B.dylib 'fn32 _lseek lseek l(ili) l(ili)'
want libSystem.B.dylib 'fn32 _getenv getenv s(s) p(p)'
want libSystem.B.dylib 'fn32 _strtol strtol i(sPi) l(ppi)'
want libSystem.B.dylib 'fn32 _time time i(W) l(p)'
want libSystem.B.dylib 'bad32 _printf variadic'
want libSystem.B.dylib 'data32 ___stdoutp __stdoutp 4 8 ptr'
want CoreFoundation 'fn32 _CFStringGetLength CFStringGetLength i(p) l(p)'
want CoreFoundation 'fn32 _CFArrayCreate CFArrayCreate p(pQiQ) p(pplp)'
want CoreFoundation 'data32 _kCFAllocatorDefault kCFAllocatorDefault 4 8 ptr'
# declared for i386 only and still exported by arm64 HIToolbox: crossed with an arm64 declaration made from the
# i386 one (tools/sdkgen, "declared for i386 only")
want Carbon 'fn32 _CreateNewWindow CreateNewWindow i(uupP) i(uupp)'
want Carbon 'fn32 _ShowWindow ShowWindow v(p) v(p)'
want Carbon 'fn32 _GetWindowBounds GetWindowBounds i(pHp) i(pHp)'
echo "dbcheck ok"
