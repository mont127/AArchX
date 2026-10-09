# Porting guide: C -> Rust at the ABI

## Mechanics

- `rust/src/ported/<name>.rs` replaces `src/<name>.c`; `ported/<name>/mod.rs`
  is the multi-file form. The Makefile computes `PORTED` from that directory
  alone — a port never edits the Makefile or any shared list.
- Build.rs regenerates `ffi` from `include/ocerz/*.h` on every change and
  autodiscovers `ported/`; touching your file is enough.
- `cargo build` always runs from `make` (it is incremental); the crate is
  pinned to `nightly-2026-10-08` via `rust/rust-toolchain.toml` — cargo only
  picks that up when invoked inside `rust/`, which the Makefile does.

## Rules that keep ports faithful

- Shared structs come from `crate::ffi` (bindgen output with layout tests
  on). Never redeclare a struct; if bindgen misses a type, fix `build.rs`.
- Exported functions: `#[unsafe(no_mangle)] pub unsafe extern "C" fn` with the
  exact C signature, `*mut`/`*const ffi::Type` for pointers, `c_int` for int.
- C `static` functions become private Rust `fn`/`unsafe fn`.
- File-scope statics become `#[unsafe(no_mangle)] pub static mut` with the
  same initializers. `__thread` statics become
  `#[unsafe(no_mangle)] #[thread_local] pub static mut` — proven on Mach-O
  TLV (C `extern __thread` links against the Rust symbol, see `globals.rs`).
- To read a TLS var still owned by C:
  `unsafe extern "C" { #[thread_local] static mut x: c_int; }`.
- `static inline` header helpers are NOT bound by bindgen — use or add the
  copy in `rust/src/inline.rs`.
- `#define` constants with a cast (e.g. `((uint64_t)1 << 0)`) are skipped by
  bindgen too — the flag masks live in `inline.rs` as `pub const`.
- Logging: `ocerz_log!` / `ocerz_trace!` / `ocerz_fatal!` from `log.rs` take a
  C format literal + args and call `libc::fprintf`; honour `ocerz_verbose`.
- Prefer raw pointers + `unsafe` over refactors when semantics are subtle.
  C `size`/index arithmetic that can go negative must be `i64`/`i32`, not
  `usize`/`u32`, and overflow-prone arithmetic needs `wrapping_*` (release
  already disables overflow checks, but debug builds should still match C).
- Mirror the C file's top prose block as a `//!` module doc. No inline
  comments beyond what the C file had.
- If other C files reach a symbol through a non-header `extern` declaration,
  grep the tree for it before you port — the symbol must still be exported
  with the exact same name and signature.

## .s files

Replace `src/<name>.s` by `ported/<name>.rs` containing `global_asm!` with the
same assembly; the Makefile filters `.s` the same way as `.c`.

## Verifying

`make -j12 ocerz` must link with zero undefined symbols, then
`bash tools/rust_gate.sh --fast`; run the full gate before pushing. Decode
equivalence/perf: `bash tools/bench/decode_bench.sh` must print HASH MATCH.
