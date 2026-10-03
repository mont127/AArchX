# Compatibility

Everything here was run by hand and dated. None of it is part of an automated
gate, so treat it as "this was seen working on that day" rather than as a
guarantee. What *is* gated is described under [Testing](testing.md).

"Works" means the program did its job: a command-line tool printed what it
prints natively, an application drew its main window and stayed up.

## Which macOS

**Both macOS 26 and 27 run cache mode.** macOS 27 keeps the x86-64 shared cache
in the Rosetta cryptex, `/System/Volumes/Preboot/Cryptexes/Rosetta`, and macOS
26 keeps it in `/System/Volumes/Preboot/Cryptexes/OS`. AArchX looks in the
Rosetta cryptex first and falls back to the OS one. From 2026-09-19 to
2026-10-03 it looked only in the Rosetta cryptex, so cache mode did not start on
macOS 26 at all in that window. Each row below says where it was measured.

**On macOS 27 the bundled applications are arm64 only.** Chess, Calculator,
Dictionary, Font Book, Grapher, Digital Color Meter, Activity Monitor, Console,
TextEdit, Preview and Script Editor ship without an x86-64 slice, so there is
nothing to translate: `ocerz` says so and exits, and a program running under it
that starts one of them gets the native binary, as it would under Rosetta.
Safari, Steam, Discord and Brawlhalla still carry Intel code and can be run on
macOS 27.

## Command-line tools

Command-line tools are the most reliable case. Sixteen are checked against their
native output byte for byte by `tools/apptest.sh cli`, and all sixteen match:
`uname`, `sw_vers`, `echo`, `ls`, `id`, `basename`, `wc`, `sort`, `uniq`,
`head`, `grep`, `file`, `xxd`, `nm`, `plutil` and `openssl` (`version` and
`dgst -sha256`). A wider sweep of 74 system tools run with read-only arguments
in cache mode, each compared against its own x86-64 slice under Rosetta, agreed
on every one, `sed`, `awk`, `otool`, `shasum`, `curl`, `sqlite3` and `perl`
among them. The only disagreements were tools whose output is meant to change
between two runs, such as `ps` and `vm_stat`.

For scale: 622 of the 969 executables in `/usr/bin` and `/bin` had an x86-64
slice on the machine that sweep ran on. The rest are arm64 only and there is
nothing to translate.

`sw_vers` failed on macOS 27 until 2026-09-22, and the cause was not what it
looked like. CoreFoundation names Foundation as an *upward* dependency, which is
how a library declares the back edge of a dependency cycle, and AArchX left
upward links out of the initializer walk entirely. Foundation's code was
reachable anyway, because the whole shared cache is mapped, so nothing looked
missing; what never ran was Foundation's own initializer, and the first
`+[NSString stringWithFormat:]` recursed until the 8 MB guest stack ran out. An
upward dependency is now initialized after the image that declares it, which is
dyld's own rule. `OCERZ_NO_UPWARD_INIT=1` restores the old behaviour, and
`tests/dynamic/upward_init.c` is the regression test.

On macOS 26.7.1, `sw_vers` currently crashes under AArchX with a stack overflow
in which `malloc` and `malloc_zone_malloc` call each other. A build from
2026-09-18 also overflows its stack there, so the problem is older than the
macOS 27 work; the 16-of-16
result above was measured on macOS 26.6 and on macOS 27. The other fifteen tools
match on macOS 26.7.1.

`/usr/bin/python3` on a Mac without Xcode is a stub that asks `xcrun` to find
the real interpreter, and `xcrun` has no x86-64 slice to load, so it fails
before any Python runs. A real Python installation does not go through that
path.

**Ollama.** The command-line binary (`Contents/Resources/ollama`, Go with cgo)
works as of 2026-09-13: `ollama --version` prints what it prints natively,
`ollama serve` answers its HTTP API (`/api/version`, `/api/tags`,
`/api/show`), and `llama-server --list-devices` lists the same devices. That
took AVX2 (Go turns on its AVX2 paths under Rosetta without checking CPUID),
`sigaltstack` reporting `SS_DISABLE`, `dlopen` refusing arm64-only dylibs, bare
`@loader_path` rpaths, and constructors in programs that do not link
CoreFoundation. The menu-bar app runs too: in a 30-second run it started its
own server, served its settings page to its window and shut down cleanly on
SIGTERM. Nobody has run a model under AArchX yet.

## Applications, cache mode

| Application | Result | Measured |
| --- | --- | --- |
| Chess | works and plays: board window, and the `sjeng` engine subprocess answers moves | macOS 26.6, 2026-09-11 |
| Calculator, Dictionary, Font Book, Grapher, Digital Color Meter | work | macOS 26.6, 2026-09-11 |
| Activity Monitor, Console | window on screen; each logs one optional library that is not in the x86-64 cache | macOS 26.6, 2026-09-11 |
| TextEdit, Preview, Script Editor | run, and open a document window when given a file | macOS 26.6, 2026-09-11 |
| Safari | runs and browses | 2026-09-21, confirmed by the author |
| Steam, the x86-64 macOS client | works with `-cef-disable-gpu`; see [below](#steam-the-macos-client) | 2026-09-12 |
| Discord | the Electron client reaches its signed-in app view, seven processes as under Rosetta | 2026-09-22, confirmed by the author |
| Brawlhalla | reaches its menus, launched from a running Steam client; see [Games](#games) | 2026-09-22, confirmed by the author |
| Photoshop | stops during startup: an initializer runs while libobjc holds its runtime lock, and the recursive acquisition aborts | 2026-09-22 |
| Photos | aborted on a graphics call whose causes were fixed by 2026-09-22; not run again since | 2026-09-13 |

**Safari** took the longest. It used to start and never show a window: WebKit's
allocator could not suspend a thread, because AArchX's thread-suspension
emulation was missing the main thread and the worker threads AArchX started
ended without running the guest's thread-exit path. Both were fixed on
2026-09-13, and the WebKit work continued through macOS 27.

**Discord**'s renderers died the moment they started until `mach_vm_region`
began answering from the guest's own map: Electron asks about a region it
expects to be read-only and stops when the answer says otherwise.

**Photoshop** stopped, but finding where produced four general fixes on
2026-09-22: upward dependencies are initialized, a dylib with no export trie is
indexed from its symbol table (21 of the 60 dylibs Photoshop ships are of that
kind), imports declared with `-undefined dynamic_lookup` are allowed to be
missing, and every unimplemented dyld API answers zero in both result
registers.

**Photos** aborted because `CGLChoosePixelFormat` returned 10002 for every
attribute set. There were three causes, all in AArchX's loader: a symlinked
plugin path the cache check rejected, `dlsym` on a shared-cache image searching
the whole cache, and the GPU bug described next.

## The GPU

Until 2026-09-22 a translated program saw no GPU at all.
`MTLCreateSystemDefaultDevice` returned nil and `MTLCopyAllDevices` returned no
devices, where Rosetta and a native arm64 build both report `Apple M5`. OpenGL
then had only its software renderer to offer: `CGLQueryRendererInfo` listed
`0x1020400` and not the accelerated `0x1027f00`, so `CGLChoosePixelFormat`
failed for every accelerated attribute set.

One bug caused all of it. In cache mode the dyld APIs answer
`_dyld_image_count`, `_dyld_get_image_name` and `_dyld_get_image_header` from
the image list AArchX keeps. `dlopen` of a dylib on disk added to that list;
`dlopen` of an image in the shared cache returned its mach header and added
nothing. The library was loaded and its symbols resolved, while every walk of
the image list said it was not there. libobjc keys its per-image queries off
that list, and Metal, which loads its GPU driver that way, concluded there was
no device.

A cache image is now registered along with its dependencies at the end of the
`dlopen`, after the Objective-C mapping and the initializers. The order matters:
libobjc calls back into the dyld APIs while it maps an image, and an image
already in the list when that callback arrives is one it takes as handled,
which would cost the new framework its categories.
`tests/dynamic/dlopen_image_list.c` holds the check. Metal,
`CGLQueryRendererInfo`, `CGLChoosePixelFormat` and `CGLCreateContext` now
return what they return under Rosetta, with `GL_RENDERER` reading `Apple M5`.

## Steam, the macOS client

The x86-64 macOS Steam client comes up with its full UI, confirmed 2026-09-12:
the bootstrapper, `ipcserver`, the client and the CEF web helper with its GPU,
utility and renderer processes, all translated. After its first update the
client lives in Application Support:

```sh
./ocerz "$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steam_osx" -cef-disable-gpu
```

CEF runs without GPU acceleration there and renders through SwiftShader, which
works and is slower. It has not been retried with GPU acceleration since the
GPU fix above. Some launches stall before the web helper starts its child
processes, with the JIT lock held inside translation; relaunching gets past it.

What it took, roughly in order:

- **System V semaphores** for Steam's tier0 threading, and later `shmat` and
  `shmdt`, through which the client talks to a running game.
- **An absolute executable path**, because Steam derives its bundle root from it.
- **`dlopen` of the executable** returning the image that is already loaded.
- **C++ initializers in dependency order** rather than by load address.
- **A large guest arena.** Chromium's PartitionAlloc reserves 32 GB at startup
  and JavaScriptCore's Gigacage asks for 128 GB, so the arena is 256 GB.
- **`ipcserver` under AArchX.** `steam_osx` starts it with `launchctl load -S
  Background` from a plist it writes, which would have launchd run it natively.
  AArchX rewrites that plist on the way through, putting itself in front of
  the program, and enables the job's label first, because a legacy `launchctl
  unload` leaves it disabled and every later load fails.
- **Three JIT fixes for the web helper.** The JIT lock never parks, so a lost
  wakeup cannot wedge translation; a page that keeps being invalidated is
  retried after 4096 refusals instead of waiting for 1.5 quiet seconds that hot
  ICU and Foundation pages never had; and a fault from a retired translation
  simply recovers instead of invalidating the page again.
- **Renderer fixes.** A self-loop compare whose loop head reads the flags is no
  longer fused (libc++'s partition loop starts with a `jbe` on the previous
  iteration's compare, `tests/dynamic/cef_partition.s`); the 386 MB CEF
  framework is matched by device and inode so the sandboxed helpers do not load
  it twice; and upward links in `dylib_use_command` form stopped counting as
  initializer-order edges, which had crashed Steam's `hardwareupdater`.
- **A mock keychain.** `Steam Helper` is launched with `--use-mock-keychain`:
  otherwise Chromium reads its "Steam Safe Storage" item, which does not trust
  the `ocerz` binary, and macOS asks for the login password once per helper
  process. Cookies CEF stores under AArchX are therefore encrypted with the mock
  key and are not readable by a native Steam, nor the reverse.
  `OCERZ_NO_MOCK_KEYCHAIN=1` turns this off.

**In native mode,** on macOS 27 on 2026-09-21, the same client came up as well:
the login window, then the 1280 by 800 Steam window with its menus and store
view, the GPU process running hardware ANGLE over the host's own OpenGL, and two
renderers. From the web helper starting to the main window took 23 s in native
mode and 34 s in cache mode on the same machine; the arm64 Steam takes 2 to 4 s.
`hardwareupdater` still fails to run its script there, and several imports Steam
has not reached yet are stubs. See [Native mode in depth](native-mode.md).

## Games

Games are launched the way Steam launches them under a compatibility tool: set
the game's launch options to `/path/to/ocerz %command%`. Steam then passes the
game's `.app` bundle, and AArchX runs the executable its `Info.plist` names. It
also honours `DYLD_INSERT_LIBRARIES` in cache mode, which is how Steam injects
its loader and overlay: AArchX moves the variable aside so the host's dyld does
not load the arm64 slices into AArchX itself, then opens the libraries in the
guest before the game's entry point. `OCERZ_FPS=1` prints the frame rate once a
second, since Steam's own overlay counter relies on dyld interposing, which
AArchX does not do.

**Brawlhalla**, an Adobe AIR game, reaches its menus against a running Steam
client (2026-09-22). Measured that day against Rosetta, with one Steam client
for both: the first frame took 5.3 s against 0.54 s. Its main thread used to
saturate a core where Rosetta uses a quarter to half of one. Three JIT changes
that evening brought it to about three quarters of a core while doing 75% more
work per second, and at the menu it now spends most of its time waiting on the
display, as under Rosetta:

- a return whose return-stack entry was lost no longer leaves the translated
  body;
- a chain whose branch cannot reach its target goes through a veneer instead of
  being dropped;
- a global load or store is resolved at translation time, four host
  instructions instead of fourteen or eighteen.

## Wine and i386

Wine 11.8 runs x86-64 and i386 PE applications through AArchX. With a WoW64 prefix,
32-bit Notepad and WineMine load `winemac.drv`, open titled Cocoa windows and
stay up:

```sh
WINE="/path/to/Wine Devel.app/Contents/Resources/wine"
export WINEARCH=wow64
export WINEPREFIX="$HOME/.wine-ocerz"

./ocerz "$WINE/bin/wine" wineboot -u
./ocerz "$WINE/bin/wine" notepad
```

Wine boots to `cmd /c ver` in about fourteen seconds. Rosetta runs i386 PE code
through WoW64 too, so AArchX's i386 support is parity rather than something
new; standalone i386 Mach-O programs are not supported by current macOS at all.
The i386 side has its own differential gate of twenty thousand generated cases.

A few things it took, because they explain behaviour a Wine user may notice:

- **A fixed arena base.** Every Wine process writes its syscall dispatcher's
  address into one page that the whole prefix shares, which is harmless only if
  `ntdll.so` loads at the same address in every process. A process whose loader
  is Wine therefore reserves its arena at a fixed base and falls back to the
  usual placement only if that range is taken.
- **Segment selectors as CPU state.** Until 2026-09-05 a 32-bit exception ran
  its dispatcher as 64-bit code, and every Steam process died within a second.
- **Two fixes for windows that closed themselves.** A CoreSpotlight category
  that never attached threw inside a dispatch block, and an IOSurface page the
  kernel mapped for the process sat at an address the guest could not see. The
  first is why CoreSpotlight is in the default Objective-C preload list.
- **Memory.** Steam is nineteen emulated processes, and each used to carry
  private copies of every file it mapped, because guest pages are 4 KB and host
  pages 16 KB. A private file mapping whose offset and address agree modulo
  16 KB now maps its aligned interior straight from the file, and a translated
  block keeps a small reference per instruction instead of its full decode. A
  WineMine process went from 1.1 GB to 714 MB.

**Windows Steam** reaches its main window. On 2026-09-27 that took about 33 s,
where Rosetta takes 11 to 12 s; with the translation store warm from an earlier
run it took 34 to 35 s against 47 s from an empty store. In one measured startup
translation took about a quarter of the CPU time, which is what the store saves
on later runs. Translated code is also slower in Wine's memory map than in the
others, because every memory access there checks its range. On 2026-10-01 it was run on stock Wine Staging
11.16 with DXMT as well, which needed thread-specific data, signals and the
cross-process memory calls Chromium's sandbox makes to be translated correctly.

**Counter-Strike 2** under Wine with D3DMetal now loads D3DMetal and reaches
its own window (2026-09-26), after a `dlopen` of an `@rpath` path was resolved
against the calling image and a sandbox check made from a Wine thread's low
stack stopped failing. It then stops on a fatal "Multiple entity classes have
the same designer name" error that Rosetta never reaches.
`OCERZ_EXE_ENV` exists to narrow that down inside `cs2.exe` alone.

## Applications, native mode

Native mode runs a narrower set, and it is growing. Image Capture opens its main
window and quits cleanly, Stickies opens a note, and the x86-64 Steam client
comes up as described [above](#steam-the-macos-client). What stops the rest is
listed in [Native mode in depth](native-mode.md#what-native-mode-cannot-run-yet).

## Known limitations

These apply to cache mode, or to both modes. Native mode's own are in
[Native mode in depth](native-mode.md).

- Application compatibility is incomplete; unsupported system calls and
  framework behaviour remain.
- Cache mode's `dlopen` resolves `@executable_path`, `@loader_path` and `@rpath`
  against the calling image, runs `+load` methods, and its `dlsym` on a handle
  searches the image's dependencies as dyld does (2026-09-26 and 27). Not
  rechecked since: whether `_dyld_register_func_for_add_image` callbacks are
  called for a `dlopen`, whether `dlopen(NULL)`'s handle finds the program's own
  symbols, `RTLD_LOCAL`, and `dlerror` after a failed `dlsym`. Native mode does
  all of these the way dyld does.
- `proc_pidpath` and `proc_name` name the guest executable only when a process
  asks about itself in cache mode; other AArchX processes appear as `ocerz`.
- x87 uses 64-bit doubles rather than 80-bit extended precision.
- The JIT translates most VEX code: AVX2 integer, move and broadcast
  instructions, FMA, 256-bit packed arithmetic, the variable blends, `rorx`,
  `shlx`, `shrx`, `sarx`, `mulx`, `andn`, and `blsr`, `blsmsk`, `blsi` and
  `bzhi` when their flags are dead. Still interpreted: 256-bit byte and integer
  shuffles, unpacks, permutes and lane inserts, `vptest`, the immediate blends,
  mask-producing compares, gathers, `bextr`, `pdep` and `pext`.
- MMX instructions always run in the interpreter, and the MMX registers are
  kept apart from the x87 stack, so `FXSAVE` and signal frames do not carry
  them.
- The approximate `RCP` and `RSQRT` results are not implemented. SSE rounding
  modes are: the guest's MXCSR rounding control drives the host's.
- Guest protection changes are resolved on the host's 16 KB page boundaries.
- An asynchronous signal reaches a guest thread only when that thread next makes
  a system call (or, in native mode, a bridged call), so a thread spinning in
  its own code never sees one. A signal aimed at another thread reaches its
  guest handler only for the signals AArchX mirrors onto the host, such as
  `SIGUSR1`, `SIGTERM` and `SIGALRM`, not for fault signals such as `SIGSEGV`.
- A NaN produced inside a floating-point batch that faults before its check
  keeps the arm64 payload.

## Reporting something that does not work

A failure that names a symbol or a library is the useful kind: it says exactly
what is missing. Run with `-v` and `OCERZ_DLPATH=1`, keep the first `ocerz:`
line, and see [Troubleshooting](troubleshooting.md) for what each kind of report
means.
