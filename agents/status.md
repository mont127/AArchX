# Port status (rust branch)

| file | lines | owner | status |
|---|---|---|---|
| a64emit.c | 947 | runtime (ee47e957) | ported |
| abi.c | 1877 | native1 | in progress |
| apidb.c | 1108 | native1 | ported |
| blocks.c | 914 | native1 | in progress |
| bridge.c | 3379 | native2 | in progress |
| cache.c | 1496 | runtime (ee47e957) | in progress |
| cpu.c | 163 | decode-agent | ported |
| decode.c | 4435 | decode-agent | in progress |
| dyld.c | 5840 | dyld-agent | ported |
| dyldapi.c | 2636 | dyld-agent | ported |
| flags.c | 230 | - | ported |
| flags_live.c | 283 | interp-agent | ported |
| globals.c | 16 | - | ported |
| interp.c | 1913 | interp-agent | in progress |
| interp_ext.c | 763 | interp-agent | ported |
| interp_sse.c | 2542 | interp-agent | in progress |
| jit.c | 2844 | - | C, split core; see jit-split.md |
| jit_cache.c | 3185 | devin-4a0e3105 | in progress |
| jit_control.c | 2070 | jit-control-agent | in progress |
| jit_flags.c | 2778 | jit-flags-agent | in progress |
| jit_fp.c | 3203 | jit-fp-agent | ported |
| jit_integer.c | 2678 | jit-integer-agent | ported: rust/src/ported/jit_integer.rs, emit audit MATCH, no C shim (agents/jit-integer.md) |
| jit_memory.c | 2184 | jit-memory-agent | ported: rust/src/ported/jit_memory.rs, emit audit MATCH, no C shim (agents/jit-memory.md) |
| jit_simd.c | 3121 | jit-simd-agent | in progress |
| jit_tcache.c | 1063 | jit-tcache-agent | in progress |
| loader.c | 378 | runtime (ee47e957) | in progress |
| main.c | 419 | runtime (ee47e957) | in progress |
| mem.c | 2342 | memvm | ported |
| objcbridge.c | 3543 | native2 | in progress |
| objcclass.c | 1609 | native2 | ported |
| stack.c | 137 | runtime (ee47e957) | in progress |
| sysbridge.c | 2077 | native2 | ported |
| syscall.c | 8906 | devin-9aa5caef | in progress |
| tcache.c | 863 | runtime (ee47e957) | in progress |
| vdylib.c | 1424 | native1 | in progress |
| vm.c | 4376 | memvm | in progress |
| x87.c | 1277 | interp-agent | ported |
| abicall.s | 237 | native1 | ported |
| leaf.s | 598 | native1 | ported |
| objcguard.s | 60 | native2 | ported |

## Tip breakages

- At detached tip commit `ff3e398`, `datomic_counter-no-jit` timed out (exit 124; expected `OK`) in the full gate. The current dynamic failure list is identical.
- `3283d71`: `tests/run_native_framework_tests.sh` fails at the compat check because `compat.jit.err` gains a stray `:/:\capacity overflow` line before the identity-arena log; reproduced on a clean tip checkout after `make apis`. Rust modules on that tip: `cpu`, `flags`, `flags_live`, `globals`, `objcguard`.
