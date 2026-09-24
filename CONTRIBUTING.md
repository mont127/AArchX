# Contributing to AArchX

AArchX (the binary is called `ocerz`) runs x86-64 Mac programs on Apple silicon. This page says how the tree is worked on, so that a change fits in on the first try.

## Conduct and security

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md). Security problems are reported privately, as [SECURITY.md](SECURITY.md) describes, never in a public issue. Issues use the templates under `.github/ISSUE_TEMPLATE`, and a pull request fills in `.github/pull_request_template.md`.

## Read first

- [docs/atlas](docs/atlas/index.html) is a guided tour of the source for someone who has never seen it: what each piece is, how a program flows through it, why each mechanism exists, and how to read its diagnostics. No C is assumed.
- Every source file opens with a prose block that explains what the file owns and why it is shaped the way it is. Read that before the code.
- [docs/testing.md](docs/testing.md) describes the gates; [docs/reference.md](docs/reference.md) the flags, environment variables and exit codes.

## Build and run

    make -j
    ./ocerz version
    ./ocerz tests/guest/bin/hello
    ./ocerz /Applications/SomeApp.app/Contents/MacOS/SomeApp

`make apis` generates native mode's API databases under `runtime/apis` from your own macOS SDK; they are derived from the SDK, so they are never committed. `make check` runs it first, then every gate: the unit harnesses, the guest programs under the interpreter and under the JIT, the x86-64 and i386 differential gates (interpreter and JIT must agree byte for byte), the dynamic tests against the shared cache, and the native-mode tests. It takes about twenty minutes and must be green before a change is sent. Do not build or run other ocerz work beside it while it runs; the dynamic suite is timing-sensitive under load.

## What a change needs

1. **A test that pins it.** Every behavioural change carries the test that would have failed before it. Guest programs go in `tests/guest` (add the name to `tests/guest/Makefile`, to `NAMES` in `tests/run_guest_tests.sh`, and a golden under `tests/guest/expect/`); programs that need real libraries go in `tests/dynamic` and are registered in `tests/run_dynamic_tests.sh`; C-level tests go in `tests/unit`; native-mode fixtures live in `tests/run_native_tests.sh`. Take expectations from a real run under Rosetta or natively, not from memory.
2. **A green `make check`.** Then commit the code and tests, and commit the rebuilt `tests/unit/bin` binaries separately as `tests: rebuild unit binaries`; they are tracked.
3. **No inline comments.** Each file explains itself in one prose block at the top, written in the present tense, and that block is updated when the file's behaviour changes. The reason for a change goes in the commit message and in that prose, not beside the line.
4. **A commit message that states the rule, not the activity.** The subject is `area: what is now true`, for example `jit: a chain that cannot reach goes through a veneer instead of being dropped`, where the area is one of `jit`, `syscall`, `dyld`, `dyldapi`, `mem`, `vm`, `interp`, `decode`, `cache`, `native`, `tests`, `docs`. The body is prose: the symptom, what was measured, the mechanism, the fix, and the test.
5. **The README stays true.** A claim it makes that a change invalidates is corrected in the same change.

## Sign-off

Commits are signed off under the [Developer Certificate of Origin](https://developercertificate.org/) (`git commit -s`), which states that you wrote the change or have the right to submit it under the project's license. No other trailers are used.

## Finding a bug

The workflow the tree is built for: run the failing program with `-no-jit` first (if the symptom persists, it is not the JIT); reproduce it in the smallest program that shows it, under ocerz and under Rosetta or natively as the control; then narrow with the environment switches, which come in three families: `OCERZ_NO_<FEATURE>` turns one optimization off, `OCERZ_<AREA>LOG` and `OCERZ_PERFSTAT` print diagnostics, and `OCERZ_INTERP_LO`/`HI` and friends select a range. They are read once, at first use.

## What to work on

Compatibility is measured with x86-only software, never with programs that also ship an arm64 build. The known gaps are listed in [docs/compatibility.md](docs/compatibility.md); the largest are Swift-based programs in both modes and the frameworks native mode has no API database for yet.

## License

AArchX is licensed under the GNU Lesser General Public License, version 2.1 or later. By contributing you agree that your contribution is licensed the same way.
