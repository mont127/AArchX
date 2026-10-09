# AArchX `rust` branch

The whole x86-64 -> arm64 translator is being ported from C11 to Rust on this
branch, **strangler-fig style at the C ABI**: a Rust staticlib
(`rust/target/release/libocerz_rs.a`) is linked into the existing C build, and
C translation units are replaced one at a time by Rust modules exporting the
same `#[no_mangle] extern "C"` symbols. The existing test gates are the oracle.

## Layout

- `rust/` — crate `ocerz-rs` (staticlib, edition 2024, pinned nightly
  2026-10-08 for `#![feature(thread_local)]`).
- `rust/src/ported/<name>.rs` — the Rust port of `src/<name>.c` (or `.s`).
  Drop the file in and `src/<name>.o` stops being compiled — there is no
  shared list to edit. `ported/<name>/mod.rs` works for multi-file modules.
- `rust/src/ffi` — bindgen output over all of `include/ocerz/*.h`.
- `rust/src/inline.rs` — Rust copies of `static inline` header helpers.
- `rust/src/log.rs` — `ocerz_log!` / `ocerz_trace!` / `ocerz_fatal!`.
- `agents/` — `baseline.md` (what passes on this VM), `expected/` (exact FAIL
  lines the gate tolerates), `porting-guide.md`, `perf.md`, `status.md`.

## Porting a module

1. Pick an unclaimed file in `agents/status.md` and mark it `in progress`.
2. `mkdir`/write `rust/src/ported/<name>.rs`; every public symbol the C file
   exported becomes `#[unsafe(no_mangle)] pub unsafe extern "C"`.
3. Use `ffi::` types for shared structs — layouts are guaranteed identical.
4. `make -j12 ocerz` then `bash tools/rust_gate.sh --fast` (full gate before
   you push). See `agents/porting-guide.md` for the details that will bite you.

## Verify and measure

- `bash tools/rust_gate.sh` — builds, runs every test phase, fails on any NEW
  failure vs `agents/baseline.md`. `--fast` = units + guest both + diff + diff32.
- `bash tools/bench/decode_bench.sh` — decoder throughput + decode hash
  against the C reference at `~/AArchX-c` (leave that worktree alone).

## Commit / push rules

- Push only to `origin rust`. Never main, never any other branch.
- Many agents push concurrently: `git pull --rebase` immediately before
  `git push`, and never force-push.
- `git commit -s`, subject `area: what is now true` (`rust:` for crate/build),
  prose body. No pre-commit hooks; don't add any.
