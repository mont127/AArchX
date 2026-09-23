# Compatibility

Everything here was run by hand and dated. None of it is part of an automated
gate, so treat it as "this was seen working on that day" rather than as a
guarantee. What *is* gated is described under [Testing](testing.md).

## Command-line tools

Command-line tools are the most reliable case. A sweep of 74 system tools run
with read-only arguments in cache mode, each compared against its own x86-64
slice running under Rosetta, agreed on every one: `uname`, `ls`, `id`, `wc`,
`sort`, `grep`, `sed`, `awk`, `file`, `xxd`, `nm`, `otool`, `openssl`,
`shasum`, `curl`, `sqlite3`, `perl` and the rest. The only disagreements were
tools whose output is meant to change between two runs, such as `ps` and
`vm_stat`. Ollama's Go binary runs both its command line and its HTTP server.

For scale: 622 of the 969 executables in `/usr/bin` and `/bin` have an x86-64
slice at all on this machine. The rest are arm64-only and there is nothing to
translate.

Two exceptions are worth naming. `sw_vers` fails on macOS 27, where Apple
rewrote it in Swift; see below. And `/usr/bin/python3` on a Mac without Xcode
is a stub that asks `xcrun` to find the real interpreter, and `xcrun` has no
x86-64 slice to load, so it fails before any Python runs. A real Python
installation does not go through that path.



## Applications, cache mode

Confirmed on Apple Silicon with macOS 26.6 in September 2026. "Works" means the
application drew its main window and stayed up.

| Application | Result |
| --- | --- |
| Chess | works and plays, including its engine subprocess |
| Calculator, Dictionary, Font Book, Grapher, Digital Color Meter | work |
| Activity Monitor, Console | window on screen; each logs one optional library that is not in the x86-64 cache |
| TextEdit, Preview, Script Editor | run, and open a document when given one |
| Safari | runs and browses, confirmed by the author on 21 September 2026 |
| Steam, the x86-64 client | works: the bootstrapper, `ipcserver`, the client, and the CEF web helper with its GPU, utility and renderer processes |

Steam needs `-cef-disable-gpu` on the build that was tested, and takes
noticeably longer to reach its window than the native client does.

Safari was the hardest of these and took the longest. It used to start and
never show a window, because WebKit's allocator could not suspend a thread;
the thread-suspension emulation was missing the main thread, and the worker
threads AArchX started were ending without running the guest's thread-exit
path. Both were fixed, and the WebKit work continued through macOS 27.

Photos aborted on a graphics call whose cause was found and fixed; it has not
been run again since.

## Applications, native mode

Native mode runs a narrower set, and it is growing. Image Capture opens its main
window and Stickies a note. In September 2026 the x86-64 Steam client came up in
native mode as well, on macOS 27: the login window, then the Steam window with
its menus and store view, with the GPU process running hardware-accelerated
graphics through the Mac's own OpenGL.

## Wine and i386

AArchX runs i386 PE code inside Wine's WoW64 process, including the 32/64-bit
transitions that Wine uses. Wine boots to `cmd /c ver` in about fourteen
seconds. The i386 side has its own differential gate of twenty thousand
generated cases.

## What does not work yet

The honest summary is that a program using one of these stops with a message
naming what it needs, rather than running and producing wrong answers.

**Native mode** has the larger gaps:

- **Swift.** Swift classes and the frameworks' Swift overlays are refused, which
  rules out most newer system applications.
- **WebKit and GameKit**, and any library without an API database. Missing
  imports are named at startup.
- **C++ across the boundary.** Guest C++ works with the optional guest runtime,
  including exceptions between guest frames, but guest C++ objects are not
  interchangeable with native arm64 C++ objects, and an exception cannot unwind
  through a bridge frame.
- **Some variadic and runtime calls.** Most of the `scanf` family, `err`, and
  several Objective-C method-replacement functions are still stubs.

**Swift, in both modes.** Native mode refuses Swift outright. Cache mode should
in principle translate it like any other x86 code, and `sw_vers` shows it does
not always: on macOS 27 that tool is a Swift program, and under AArchX it loads
`libswiftCore.dylib` and then faults at the bottom of its stack with a repeating
frame pattern, which is what unbounded recursion looks like. It fails the same
way under the interpreter, so it is not a translation bug, and the cause is not
yet known. This gap grows on its own as Apple moves more system software onto
Swift.

**Both modes** are limited by the same two things: this is experimental software
of version 0.2-dev, and only what someone has actually run is known to run.

## Reporting something that does not work

A failure that names a symbol or a library is the useful kind: it says exactly
what is missing. Run with `-v` and `OCERZ_DLPATH=1`, keep the first `ocerz:`
line, and see [Troubleshooting](troubleshooting.md) for what each kind of report
means.
