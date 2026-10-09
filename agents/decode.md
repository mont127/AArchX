# Rust x86 decoder

`src/decode.c` is ported to the Rust static library without intended changes
to the C ABI, decode semantics, side-effect order, or partial writes on errors.
The exported entry points and formatter live in the `decode` module:

- `rust/src/ported/decode/mod.rs`: decode state, fetch and operand helpers,
  ModRM/addressing, size and ALU helpers, VEX handling, decode exports, and
  shared state.
- `onebyte.rs`: one-byte opcode groups and dispatch.
- `map0f.rs`: `0F` map and SSE/MMX helpers.
- `map0f38.rs`: `0F 38` and `0F 3A` maps, BMI, MOVBE, maskmov, and gather.
- `x87.rs`: x87 decoding and opcode tables.
- `names.rs`: opcode names and instruction formatting.

The port zero-initializes C locals that were left uninitialized, including
temporary operands and ModRM records. No uninitialized-read discrepancy or
intentional C behavior deviation was found. The `said` flag is an
`AtomicBool`; lazy opcode-name initialization uses `Once`. A few opcode tables
use Rust `c_int` where the C declarations use `uint16_t` or `int`; the stored
values and behavior are unchanged.

## Shared decode-tool fixes

These were committed separately before the decoder port:

- `0a2df70`: `decode_bench.sh` excludes object files for every module under
  `rust/src/ported`. Otherwise a stale C `src/decode.o` (or another ported
  object) could be linked into the Rust tree, yielding a false hash match or
  duplicate symbols.
- `1651c97`: `decodiff-cmp.sh` builds and links the Rust decoder and the
  tree's remaining core objects when decode is ported. Its old `touch
  src/decode.c && make src/decode.o` path tested C again and recreated a stale
  decoder object.
- `6bb7c98`: the benchmark performs best-of-five timing sweeps with hashing
  disabled, then a separate hashed sweep for counts and the equivalence hash.
  The inlined FNV work dominated the old reported time (about 80%); the output
  retains `ns_per_insn` and adds `ns_per_insn_hashed`.

## Optimization trials

All runs use the same 64 MiB shared-cache window. Hashes/counts matched for
every retained or trial implementation; unhashed numbers are best-of-five
`ns/insn`, and hashed numbers come from the separate hash sweep.

1. **Inline hot helpers and use unaligned multi-byte fetches** (`03c3ba1`).
   `fetch16/32/64` perform one `read_unaligned` and `from_le` after the same
   length check. In the step run, C was x64/i386 `18.9/16.8` unhashed and
   `115.7/112.1` hashed; Rust was `17.2/16.0` and `111.2/115.1`.
2. **Output-template initialization** was tried and reverted. Two trial runs
   measured C `19.2/17.7`, Rust `18.0/16.7`, then C `19.4/17.7`, Rust
   `18.3/16.7` (x64/i386, unhashed). They did not establish a consistent gain.
3. **Conditional RIP-relative fixup** was tried and reverted. Two trial runs
   measured C `17.6/15.9`, Rust `17.3/16.1`, then C `18.3/17.0`, Rust
   `17.5/16.3` (x64/i386, unhashed); the shortcut had no consistent gain.
4. **Prefix-class table and common no-prefix path** (`885446b`). The first
   paired run measured C `18.2/16.9` and Rust `15.3/13.1` unhashed; hashed
   times were C `116.3/108.3` and Rust `103.6/98.8` (x64/i386).
   Prefix ordering, REX resets, `last_f23`, and truncation/too-long behavior
   remain unchanged.
5. **Release codegen/bounds audit** (`36355e0`). `fixup_riprel` now uses
   unchecked operand access under the decoder's `nops <= 3` invariant. Two
   paired runs measured C `18.2/16.3`, Rust `16.1/14.7`, then C `16.3/15.0`,
   Rust `14.2/13.2` (x64/i386, unhashed). This is faster in both modes in
   both runs, but the observed paired improvement (about 11.5–12.9% x64 and
   9.8–12.0% i386) is below the 15% target. A small ALU-dispatch rewrite was
   measured and dropped because it did not improve on the preceding run.

The optimized measurements and faithful-port comparison are recorded in
`agents/perf.md`. In release codegen, the inlined one-byte `match op` uses an
indirect branch through a generated jump table; `alu_rm_r` uses a switch-table
byte lookup. After the unchecked fixup, a relocation scan found no panic or
bounds-check calls in decoder symbols. Remaining conditional operand indexes
are restricted to 0/1, and output initialization indexes the fixed three
operand slots.

## C-string and integration gotchas

- Opcode names, register names, formatter formats, and other literal strings
  passed to C use `c"..."` literals. Generated FMA names are written with
  `snprintf` into static 16-byte buffers. The `DECODE-NULL` diagnostic has an
  explicit trailing NUL and is passed as `*const c_char`. Formatter temporary
  strings are NUL-terminated by `snprintf` before use as `%s`.
- `decodiff.c` defines the three base globals (`low_base`, `top_base`, and
  `guest_base`) that decode error diagnostics read.
- Remove stale `src/<ported-module>.o` and `.d` files before linking or running
  a gate. A stale C object can invalidate benchmark or diff results even when
  the Rust source is correct.
