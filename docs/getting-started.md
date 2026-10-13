# Getting started

## What you need

- **An Apple Silicon Mac.** AArchX emits arm64 code and uses arm64 features
  directly; it does not run on an Intel Mac.
- **macOS 26 or 27.** Those are the releases it is developed and tested on.
  Earlier releases are untried rather than known-broken.
- **The Xcode Command Line Tools**, for `clang` and `make`, if you build
  AArchX yourself. `xcode-select --install` installs them. The prebuilt below
  does not need them.
- **Rosetta installed, for cache mode only.** Cache mode binds guest programs
  against Apple's x86-64 shared cache, and the Rosetta package is what ships
  that cache. AArchX maps and fixes up the cache itself and never calls
  Rosetta's translator; it only needs the file to be on disk. If you do not
  want Rosetta installed at all, [native mode](modes.md) needs nothing from it.

Two more things are needed only to run the full test suite: the Python
`capstone` package for the i386 differential gate, and a working `clang -arch
x86_64` for the fixtures the dynamic and native gates build.

## Install the prebuilt

```sh
curl -fsSL https://mont127.github.io/AArchX/install.sh | sh
```

The script downloads the latest release's `aarchx-macos-arm64.tar.gz`, checks
its SHA-256 and unpacks it into `~/.aarchx`: `ocerz`, the API databases native
mode reads (`runtime/apis`, `runtime/apis32`) and the guest C++ runtime
(`runtime/guest`). It moves [AArchX.app](#the-aarchx-app) into
`~/Applications`, and adds `~/.aarchx/bin` to `PATH` in `~/.zprofile` or
`~/.bash_profile`. It asks for no password and touches nothing else. Because
curl does not quarantine what it downloads, Gatekeeper does not stop the
unsigned binary.

| Variable | Effect |
| --- | --- |
| `AARCHX_HOME=<dir>` | install there instead of `~/.aarchx` |
| `AARCHX_VERSION=v0.7` | install that release instead of the latest |
| `AARCHX_NO_MODIFY_PATH=1` | leave the shell profile alone |
| `AARCHX_NO_APP=1` | leave AArchX.app in the install directory instead of `~/Applications` |

Pass them to the `sh` side of the pipe: `curl -fsSL ... | AARCHX_HOME=/opt/aarchx sh`.
Running the script again updates the install, and
`curl -fsSL https://mont127.github.io/AArchX/install.sh | sh -s -- --uninstall`
removes it and the `PATH` line. The translation cache in
`~/Library/Caches/ocerz` is left behind; delete it too if you want the space back.

With Homebrew, the same prebuilt is a cask in the `mont127/aarchx` tap:

```sh
brew install --cask mont127/aarchx/aarchx
```

It links `ocerz` into Homebrew's `bin` and puts AArchX.app in `/Applications`.
The cask removes the quarantine flag Homebrew puts on its downloads. `brew upgrade --cask aarchx` updates it, and
`brew uninstall --zap --cask aarchx` removes it along with the translation cache.

The prebuilt has no Steam bridge database and no 32-bit GNU C++ runtime, so
[32-bit Mac games from Steam](32-bit-steam-games.md) still need the build below.
If you download the archive with a browser instead, remove the quarantine flag
before running it: `xattr -dr com.apple.quarantine aarchx`.

## The AArchX app

AArchX.app is a window around `ocerz` for running programs without Terminal.
It is separate from the terminal version: `ocerz` works the same with or without
it, and the app only finds and starts it.

The app keeps a **library** of your programs: Intel Mac apps and command-line
programs, 32-bit Mac programs, and Windows `.exe` and `.msi` files. Each tile
shows the program's icon and name, and says what it is or that it is running.

To add programs:
- drop them on the window: one program dropped is added and run, several are
  only added;
- click **+**;
- right-click a program in Finder and choose **Open With → AArchX**, or drop it
  on the app's Dock icon, which adds it and runs it.

To use a tile:
- double-click it to run the program;
- right-click it to run it in native mode, stop it, open its last log, show it
  in Finder or remove it from the library.

**Activity** lists every run, with how it ended and when it started. Its logs
are in `~/Library/Logs/AArchX`.

How the app picks the mode:
- **Native mode**, in the bottom bar, is `-native`.
- The app switches to native mode by itself for a 32-bit program, which only
  [m32](m32.md) runs, and when Rosetta's x86-64 cache is missing.
- It refuses an Apple silicon app, which runs without AArchX.

A Windows program runs through Wine, as `ocerz <wine> <program>`:
- The **Windows programs** menu in the bottom bar lists the x86-64 Wine
  builds the app finds:
  - WineHQ's Stable, Staging and Devel apps, in `/Applications`,
    `~/Applications` or MacNdCheese's own copies;
  - Whisky's Wine;
  - the Game Porting Toolkit;
  - a `wine` in Homebrew's `bin`.
- The program starts in its own folder, in cache mode.
- It uses a prefix of the app's own, `~/Library/Application Support/AArchX/Wine`,
  so no other Wine setup's prefix is touched. `WINEPREFIX`, or
  `defaults write io.github.mont127.aarchx WinePrefix <dir>`, picks another.
- An `.msi` is installed with `msiexec /i`.
- **Stop** also stops the prefix's `wineserver`, which ends every Windows
  program running in that prefix.

With CrossOver installed, **CrossOver Bottles…** in the bottom bar lists its
bottles, each with a box. A ticked bottle runs its Windows programs under
AArchX instead of Rosetta, started from CrossOver or from their launchers as
usual. Ticking does two things, and unticking undoes both:
- **The bottle.** It sets the bottle's `[Wine] "BinPath"`, CrossOver's
  documented way to use another Wine, to two small scripts in
  `~/Library/Application Support/AArchX/CrossOver/bin`. They run CrossOver's
  `wineloader` and `wineserver` under `ocerz`, or directly if `ocerz` is gone.
- **The launchers.** CrossOver writes a launcher app for each program into
  `~/Applications/CrossOver`. Normally that launcher is also Wine's alternate
  loader, which means the program's window processes run inside it, under
  Rosetta, and that arrangement does not run under `ocerz` yet. So each of the
  bottle's launchers keeps its own executable beside it as
  `<name>.cx-original`, and a short script in its place starts the program
  through CrossOver's `wine` without the alternate loader. Every process then
  runs under AArchX.

Unticking deletes the scripts and renames the originals back, so the launchers
are byte for byte as CrossOver wrote them. Nothing inside CrossOver.app changes.
While a bottle is ticked:
- its programs show Wine's own Dock icon instead of the launcher's;
- links such as `steam://` from a browser do not reach the program through its
  launcher.

If CrossOver writes a launcher again, it is back on Rosetta until you next open
**CrossOver Bottles…**, which switches it again. Quit a bottle's programs before
switching it; the change applies when the bottle next starts.

For games, [MacNdCheese](https://github.com/mont127/MacNdCheese) sets up a whole
Wine environment, with DXMT, DXVK and Steam, and can run its bottles under
AArchX too.

It looks for `ocerz` in this order:
1. inside its own bundle;
2. beside itself;
3. in `~/.aarchx`;
4. in Homebrew's `bin`.

To point it at another `ocerz`, run
`defaults write io.github.mont127.aarchx OcerzPath /path/to/ocerz`.

How to get it:
- **The one-line installer** puts it in `~/Applications`.
- **The Homebrew cask** puts it in `/Applications`.
- **`AArchX.zip`** on each release is the app with `ocerz` and its runtime
  inside, for anyone who would rather not use Terminal at all. It is not
  notarized, so macOS refuses to open it the first time:
  1. Move it to Applications.
  2. Try to open it.
  3. Click **Open Anyway** in System Settings → Privacy & Security.
- **`make app`** builds it from source.

## Build

```sh
make -j
./ocerz version        # AArchX 0.7.3
```

The build is C11 with `-arch arm64 -O2`, takes well under a minute, and has no
dependencies outside the system toolchain.

## Run something

```sh
./ocerz tests/guest/bin/hello
```

That is a freestanding x86-64 test binary from this repository. For a real
program, give AArchX the executable, or an application bundle, in which case it
runs the executable the bundle's `Info.plist` names:

```sh
./ocerz /Applications/Some.app
./ocerz /Applications/Some.app/Contents/MacOS/Some
./ocerz /usr/bin/perl -e 'print "hello\n"'
```

Arguments after the program are passed to it. If an argument of yours would be
mistaken for one of AArchX's own options, put `--` before the program, or name
the program with `-path`:

```sh
./ocerz -- ./tool -v
./ocerz -path ./tool -v
```

## Run something in native mode

```sh
make apis
./ocerz -native ./some_x86_64_tool
```

Native mode reads its API databases from `runtime/apis` next to the `ocerz`
binary. They are derived from the macOS SDK, so they are not in the
repository; `make apis` generates them from your own SDK. Run native mode from
the source tree, or keep that directory beside a copied binary. See [Modes](modes.md) for what native mode is and when to choose it.

## Check that everything works

```sh
make check
```

This builds the unit and guest tests and runs every gate: the guest suite under
the interpreter and under the JIT, the x86-64 differential gate, the i386
differential gate, the dynamic-linking suite and the native-mode suite. It takes
roughly half an hour and prints a count per phase. [Testing](testing.md)
describes what each phase proves.

## What to expect

A command-line tool that stays inside the C library, and most system
applications, run and produce byte-identical output to the same program under
Rosetta. Something large and modern may stop with a named message rather than
run wrongly: AArchX would rather tell you which call it cannot make than guess.
[Compatibility](compatibility.md) lists what is known to work, and
[Troubleshooting](troubleshooting.md) explains how to read a failure.

Nothing needs to be installed, and nothing is modified outside the process:
AArchX is a single binary that maps a program into its own address space and
runs it.
