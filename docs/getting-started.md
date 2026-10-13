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
(`runtime/guest`). It then adds `~/.aarchx/bin` to `PATH` in `~/.zprofile` or
`~/.bash_profile`. It asks for no password and touches nothing else. Because
curl does not quarantine what it downloads, Gatekeeper does not stop the
unsigned binary.

| Variable | Effect |
| --- | --- |
| `AARCHX_HOME=<dir>` | install there instead of `~/.aarchx` |
| `AARCHX_VERSION=v0.7` | install that release instead of the latest |
| `AARCHX_NO_MODIFY_PATH=1` | leave the shell profile alone |

Pass them to the `sh` side of the pipe: `curl -fsSL ... | AARCHX_HOME=/opt/aarchx sh`.
Running the script again updates the install, and
`curl -fsSL https://mont127.github.io/AArchX/install.sh | sh -s -- --uninstall`
removes it and the `PATH` line. The translation cache in
`~/Library/Caches/ocerz` is left behind; delete it too if you want the space back.

With Homebrew, the same prebuilt is a cask in the `mont127/aarchx` tap:

```sh
brew install --cask mont127/aarchx/aarchx
```

It links `ocerz` into Homebrew's `bin`. The cask removes the quarantine flag
Homebrew puts on its downloads. `brew upgrade --cask aarchx` updates it, and
`brew uninstall --zap --cask aarchx` removes it along with the translation cache.

The prebuilt has no Steam bridge database and no 32-bit GNU C++ runtime, so
[32-bit Mac games from Steam](32-bit-steam-games.md) still need the build below.
If you download the archive with a browser instead, remove the quarantine flag
before running it: `xattr -dr com.apple.quarantine aarchx`.

## Build

```sh
make -j
./ocerz version        # AArchX 0.7
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
