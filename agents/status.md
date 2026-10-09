# Port status (rust branch)

| file | lines | owner | status |
|---|---|---|---|
| a64emit.c | 947 | - | C |
| abi.c | 1877 | native1 | in progress |
| apidb.c | 1108 | native1 | in progress |
| blocks.c | 914 | native1 | in progress |
| bridge.c | 3379 | native2 | in progress |
| cache.c | 1496 | - | C |
| cpu.c | 163 | decode-agent | in progress |
| decode.c | 4435 | decode-agent | in progress |
| dyld.c | 5840 | - | C |
| dyldapi.c | 2636 | dyld-agent | in progress |
| flags.c | 230 | - | ported |
| flags_live.c | 283 | - | C |
| globals.c | 16 | - | ported |
| interp.c | 1913 | - | C |
| interp_ext.c | 763 | - | C |
| interp_sse.c | 2542 | - | C |
| jit.c | 2844 | - | C, split core; see jit-split.md |
| jit_cache.c | 3185 | - | C, split cache/lifecycle/fault recovery |
| jit_control.c | 2070 | - | C, split control/callouts |
| jit_flags.c | 2778 | - | C, split flags/liveness/branches |
| jit_fp.c | 3203 | - | C, split FP batches/x87 |
| jit_integer.c | 2678 | jit-integer-agent | in progress |
| jit_memory.c | 2184 | - | C, split addressing/guards |
| jit_simd.c | 3121 | - | C, split SIMD emitters |
| jit_tcache.c | 1063 | - | C, split translation-cache serialization |
| loader.c | 378 | - | C |
| main.c | 419 | - | C |
| mem.c | 2342 | - | C |
| objcbridge.c | 3543 | native2 | in progress |
| objcclass.c | 1609 | native2 | in progress |
| stack.c | 137 | - | C |
| sysbridge.c | 2077 | native2 | in progress |
| syscall.c | 8906 | devin-9aa5caef | in progress |
| tcache.c | 863 | - | C |
| vdylib.c | 1424 | native1 | in progress |
| vm.c | 4376 | - | C |
| x87.c | 1277 | - | C |
| abicall.s | 237 | native1 | in progress |
| leaf.s | 598 | native1 | in progress |
| objcguard.s | 60 | native2 | ported |
