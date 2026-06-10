# Wine bring-up under ocerz — architecture & phased plan

Goal: run real x86_64 Wine (Wine Devel 11.8, `~/Wine Devel.app/Contents/Resources/wine`)
under ocerz faithfully — `./ocerz bin/wine notepad` produces a notepad window, with
every x86_64 process (wine loader, ntdll, wineserver, the PE chorus) emulated by ocerz.
No pokes, no skipped semantics.

Status today (2026-06-10): ocerz runs `bin/wine` end-to-end (PIE launcher), dlopens
`ntdll.so` from disk, runs Wine's unix-side init through `init_paths()` (the dladdr
slot-0x60 fix), and with `WINEARCH=wow64` execs the right loader. It stops at mapping
`lib/wine/x86_64-unix/wine` ("cannot map segments"). Everything below is grounded in a
7-agent audit (2026-06-10) with kernel-level A/B tests; load-bearing claims were
adversarially re-verified.

---

## 1. Verified ground truth

### The wine loader binary (lib/wine/x86_64-unix/wine, Wine 11.8)
- Non-PIE MH_EXECUTE (flags NOUNDEFS|DYLDLINK|TWOLEVEL, **no MH_PIE**), LC_MAIN
  (entryoff 0x750), classic LC_DYLD_INFO with **rebase_size 0** and absolute pointers
  baked into __DATA (incl. the `wine_main_preload_info` array {0x1000,0x1fffff000},
  {0x7ff000000000,0x1ff0000}). **Slide must be 0. Nothing can relocate this binary.**
- Segments: __PAGEZERO 4KB; WINE_RESERVE vmaddr 0x1000 vmsize 0x1fffff000 (zero-fill,
  filesize 0, initprot 0x3 — NOT prot-NONE in the load command); __TEXT @0x200000000;
  __DATA @0x200001000; __LINKEDIT @0x200002000; WINE_TOP_DOWN @0x7ff000000000
  vmsize 0x1ff0000 (~32MB only).
- Linked with `-no_pie,-image_base,0x200000000,-no_huge,-no_fixup_chains,
  -segaddr,WINE_RESERVE,0x1000,-segaddr,WINE_TOP_DOWN,0x7ff000000000,-pagezero_size,0x1000`
  (configure.ac:940-948). No wine-preloader exists in this build
  (HAVE_WINE_PRELOADER undef; preloader_exec == plain execv).

### Native runtime layout (vmmap of live notepad.exe, pid-verified)
- The kernel maps each segment at its own vmaddr; **no contiguous hull exists** —
  the ~140TB [0x1000, 0x7ff001ff0000) span ocerz computes is an ocerz artifact
  (src/dyld.c:370 one-span design), not a Wine requirement.
- WINE_RESERVE lives as `---/rwx SM=NUL` (8GB, PROT_NONE reservation); Wine punches
  real content in afterwards. notepad.exe PE at its native base 0x140000000 inside it.
- Unix .so dylibs land just above 8GB (ntdll.so @0x208677000 etc. — dyld-chosen,
  position-independent, anywhere is fine). PE builtin DLLs file-mapped in
  0x6ffffe410000–0x6ffffffe9000 (25 DLLs; ntdll.dll @0x6ffffff40000).
- user_shared_data at fixed **0x7ffe0000**, syscall-dispatcher page at 0x7ffe1000
  (absolute asm refs to 0x7ffe028a); first TEBs forced below 2GB.
- Process model: every Windows process (wineserver, services.exe, 2x winedevice.exe,
  plugplay.exe, svchost.exe, explorer.exe, rpcss.exe, notepad.exe) is a separate host
  process reparented to launchd. 15 threads in notepad.exe at +6s.

### Host arm64 address-space law (kernel A/B-tested on this machine, Darwin 25.5)
- An arm64 process can NEVER have VA below ~4GB: binaries with __PAGEZERO < 4GB are
  killed at exec (EBADMACHO/SIGKILL — tested 0x1000..0x10000000); runtime MAP_FIXED
  below ~0x108000000 fails ENOMEM. **There is no linker flag escape.**
- The host arm64e dyld shared region occupies ~[0x182000000, 0x2E0000000) → EACCES.
  **Wine's __TEXT address 0x200000000 is inside it.**
- Mappable (verified): 0x300000000+, 0x500000000 (12GB block ok), 0x6ffffe410000 (PE
  arena — identity OK), 0x7ff000000000 (WINE_TOP_DOWN — identity OK), 0x8000000000,
  0x10000000000, 0x600000000000. NOT mappable: 0x7ffe0000, 0x4000000000 (EACCES band).
- Consequence: **pure identity mapping cannot serve Wine's guest VA below
  0x2E0000000.** Identity remains valid for everything ocerz does today (arena
  0x300000000+, guest cache 0x7ff8…, PE arena, WINE_TOP_DOWN).

### TEB / %gs (installed binary, not the source tree)
- The installed ntdll.so contains **5 `movl $0x3000003` sites** (_thread_set_tsd_base)
  → Wine Devel 11.8 is mainline-style: it swaps gs.base between pthread-TSD and TEB
  around syscalls. ocerz already implements machdep trap 3 (`cpu->gs_base = rdi`,
  src/syscall.c:1732-1744, per-thread OcerzCPU field) — the swap mechanism is FREE.
- Caveat: the ~/wine-11-d3dmetal SOURCE tree is MNC-patched (HACK 22: no swap, TEB
  mirrored to TSD slots 6/11). Use the source tree for orientation only; the runtime
  target is the installed 11.8 semantics.
- `mac_thread_gsbase` learns the pthread TSD base via
  thread_info(mach_thread_self(), THREAD_IDENTIFIER_INFO).thread_handle — under ocerz
  this MIG call is forwarded to the HOST arm64 thread and returns the wrong value.
  Must be intercepted to return the guest gs_base.

### wine ↔ wineserver IPC (source-verified, MNC tree ≈ mainline here)
- AF_UNIX socket /tmp/.wine-<uid>/server-<dev>-<ino>/socket; per-thread request pipe
  (from server via SCM_RIGHTS), reply+wait pipes (client-created, write ends passed
  via sendmsg SCM_RIGHTS, dlls/ntdll/unix/server.c:968/974/1008-1014). Requests are
  plain write()/writev() structs. No request shm. /dev/ntsync fast-sync is Linux-only.
- Two shared mmaps: KUSER_SHARED_DATA (server keeps a MAP_SHARED file mapping it
  writes; client maps the same file at 0x7ffe0000) and the shared session block
  (win32u winstation.c:106-131).
- Real Mach IPC exists: wineserver bootstrap_register2's a port named by server_dir;
  each client bootstrap_look_up's it and mach_msg_send's its TASK PORT
  (server.c:1478-1518). Server uses it for: per-thread signal delivery
  (mach_port_extract_right(task, client mach_thread_self name) + __pthread_kill —
  thread suspend is SIGUSR1-based, no thread_suspend), ReadProcessMemory/
  WriteProcessMemory (mach_vm_read_overwrite/mach_vm_write + task_suspend/resume),
  and debug registers ONLY via thread_get/set_state(x86_DEBUG_STATE) — which
  wineserver SKIPS when sysctl.proc_translated=1 (is_rosetta(), server/mach.c:77-85).
  Precedent: under real Rosetta, debug registers are zeros. ocerz can do the same.
- wineserver itself: plain x86_64 PIE binary, single-threaded kqueue/kevent loop,
  self-pipe signal handlers, fcntl lock file.
- Client thread IDs given to the server are mach_thread_self() NAMES — ocerz guest
  threads are 1:1 host threads and forwards thread_self, so the right extraction
  works on real host threads. Any future thread-multiplexing design would break this.

### Wine's unix-side signal contract
- sigaction handlers for INT/FPE/ABRT/QUIT/USR1/TRAP/SEGV/ILL/BUS (+SIGSYS on Apple),
  all SA_ONSTACK with per-thread sigaltstack. NO Mach exception handling anywhere.
  PAGE_GUARD/stack growth/Win32 exceptions all ride on guest SEGV delivery.

### ocerz gaps found (src/syscall.c audit)
- Guest signal delivery essentially nonexistent: sigaction records 8 bytes (handler)
  only (:1056-1070); sigprocmask/sigaltstack are no-ops; **no sigreturn (#184)**; no
  frame construction; guest SEGV = _exit(139) via vm.c crash_handler. Single biggest
  missing subsystem.
- sendmsg/recvmsg (#27/#28 cancellable) are HARD-FATAL; only the _nocancel variants
  forward, and only the top-level msghdr pointer is translated (interior msg_iov/
  msg_name/msg_control rely on identity).
- posix_spawn (#244) ignores the XNU args-desc entirely → POSIX_SPAWN_SETEXEC
  silently becomes spawn-and-continue, and guest argv[0] is dropped (:546-582).
- kevent64 (#369) missing (fatal); kevent_qos a success no-op; __semwait_signal/
  sem_wait_nocancel return 0 instantly (sleeps are no-ops — latent spin/race source).
- mmap/mach_vm MAP_FIXED accepted only inside the 4GB identity arena (kernel-chosen
  base, observed 0x300000000); commit bitmap covers only the arena and pg_index
  UNDERFLOWS for addresses below arena_lo (wild indexing).
- fork is real and faithful (:513-533); execve self-re-exec is solid (:618-670);
  dlopen-from-disk + classic fixups already proven by ntdll.so.
- DYLDAPI_DISK_MAX=64 vs Wine's 29 unix .so files + deps — audit capacity.
- map_segments excludes only `vmaddr==0 && initprot==0` (__PAGEZERO) from its span;
  WINE_RESERVE (initprot 0x3, filesize 0) is included → the 140TB span. Zero-fill
  reservation segments must key off **filesize==0**, not initprot.

---

## 2. The architecture decision: split address space (identity + low shadow)

Guest VA spans [0x1000 .. 0x7ff8xxxxxxxx] (WINE_RESERVE bottom to shared cache top) —
nearly the whole 47-bit space, so no single global guest_base offset can fit; and the
host forbids guest-identity below 0x2E0000000. Therefore:

- **Identity window (unchanged): guest VA >= 0x300000000.** Arena, guest dyld cache
  (0x7ff8…), PE builtin arena (0x6ffffe…, verified mappable), WINE_TOP_DOWN
  (0x7ff000000000, verified mappable) all stay guest==host.
- **Low shadow window: guest VA [0, 0x300000000) is backed by a 12GB host block**
  reserved PROT_NONE at a fixed, probed host base (0x8000000000 candidate; 0x500000000
  and 0x10000000000 verified fallbacks; 0x4000000000 is EACCES — probe at startup).
  `g2h(g) = g < 0x300000000 ? g + low_delta : g` — one predictable branch; h2g
  mirrors it for the shadow host range. This covers WINE_RESERVE (0x1000–0x200000000),
  the loader __TEXT/__DATA/__LINKEDIT (0x200000000–0x20000a000), user_shared_data
  (0x7ffe0000), the dispatcher page (0x7ffe1000), and sub-2GB TEBs.
- **The shadow base must be deterministic and inherited**: first ocerz picks it,
  children get it via env (e.g. OCERZ_LOWBASE) across the execve/posix_spawn re-exec,
  so every cooperating ocerz process (wine clients AND wineserver) shares one
  low_delta. That makes cross-process address translation (mach_vm_read/write
  emulation, remote-thread signal targeting) a constant-offset rule.
- Faithfulness note: WINE_RESERVE must be REALLY reserved (12GB PROT_NONE host block
  exists; guest mmap bookkeeping marks [0x1000,0x200000000) reserved), because ntdll's
  VM layer trusts wine_main_preload_info; lying about reservations is exactly the
  class of hack this project forbids.

Why not alternatives:
- Rebuilding ocerz with small __PAGEZERO: kernel kills small-pagezero arm64 binaries
  at exec. Dead.
- Running ocerz as an x86_64/Rosetta host binary: defeats the project.
- Software MMU for everything: unnecessary — only the low 12GB needs translation.

---

## 3. What must change, by subsystem

### M — Memory (src/mem.c, src/dyld.c, src/syscall.c mmap paths)
- M1. Reserve the low shadow block at init (probe fixed candidates; export
  OCERZ_LOWBASE; PROT_NONE; MAP_NORESERVE).
- M2. g2h/h2g branch + commit bitmap generalized to two ranges (arena + shadow);
  fix pg_index underflow for sub-arena addresses.
- M3. map_segments: per-segment mapping mode (no contiguous hull); zero-fill segments
  (filesize==0) become PROT_NONE reservations committed lazily; slide=0 mode for
  non-PIE mains (drop the is_pie fatal when fixed placement succeeds); accept
  WINE_TOP_DOWN-style identity-fixed reservations.
- M4. sys_mmap/sys_munmap/sys_mprotect + mach_vm_allocate/map/protect/deallocate:
  accept MAP_FIXED in the shadow window and in registered guest reservations
  (WINE_TOP_DOWN at identity); keep arena behavior unchanged.
- M5. Audit every raw `(uintptr_t)` guest-pointer cast (dyldapi.c, dyld.c, syscall.c
  ptr_mask single-level translation) for shadow-window correctness; route through
  ocerz_g2h. Interior-pointer translation needed where guest structs can live in low
  memory (iovec arrays, msghdr internals).

### L — Loader/dyld surface
- L1. dlsym("wine_main_preload_info") on the main-executable handle (export-trie of a
  non-PIE main) must work — virtual_init depends on it.
- L2. Disk-dylib capacity: raise/dynamize DYLDAPI_DISK_MAX (29 wine .so + deps).
- L3. ntdll.so dlopens siblings lazily (win32u.so, winemac.so …) — already-working
  path, re-verify under the shadow split.

### S — Syscalls (src/syscall.c)
- S1. sendmsg/recvmsg #27/#28: forward with full msghdr deep-translation (msg_name,
  msg_iov array + each iov_base, msg_control) — SCM_RIGHTS fd passing is the
  wineserver lifeline. Same deep-translation for the _nocancel pair.
- S2. posix_spawn: parse the XNU _posix_spawn_args_desc; honor POSIX_SPAWN_SETEXEC
  (degenerate to execve semantics), preserve guest argv[0], pass OCERZ_LOWBASE.
- S3. kevent64; real __semwait_signal/sem_wait_nocancel (actual waiting); keep
  kevent/kqueue forwarding.
- S4. sigprocmask with real per-thread state (needed once signals exist).

### G — Guest signal delivery (the big one; src/syscall.c + src/vm.c + src/cpu)
- G1. Full sigaction record (handler, sa_mask, sa_flags, sa_tramp), per-thread
  sigaltstack, per-thread mask.
- G2. Host→guest async delivery: install host handlers for guest-registered async
  signals (SIGUSR1/USR2/INT/QUIT…); on arrival, interrupt the target guest thread's
  interpreter loop, build the x86_64 signal frame (ucontext64+mcontext from OcerzCPU,
  sigaltstack switch, mask application), run the guest handler via the guest
  sa_tramp protocol.
- G3. sigreturn (#184): restore OcerzCPU from the guest mcontext.
- G4. Synchronous fault conversion: guest SEGV/BUS/TRAP/ILL/FPE in guest code →
  guest handler (same frame machinery) instead of _exit(139); keep the crash dump
  for unhandled faults. This unlocks PAGE_GUARD stack growth and Win32 exceptions —
  notepad will not survive without it.
- G5. SIGSYS handler registration (Wine registers it on Apple).

### K — Mach/MIG guest-thread semantics
- K1. Intercept thread_info(THREAD_IDENTIFIER_INFO) (MIG over mach_msg) for guest
  threads → thread_handle = that thread's cpu->gs_base.
- K2. thread_get/set_state(x86_DEBUG_STATE): return zeros / accept-and-ignore,
  matching the real-Rosetta precedent (wineserver skips it for translated processes
  anyway — consider surfacing sysctl.proc_translated=1 to guests; decide in phase).
- K3. mach_vm_read_overwrite/mach_vm_write on a REMOTE task (wineserver →client):
  translate remote low-window addresses by the shared low_delta (both sides run under
  ocerz with the same OCERZ_LOWBASE). task_suspend/resume forward as-is.
- K4. bootstrap_register2/bootstrap_look_up/mach_msg task-port send: forwarded
  mach_msg should already carry these; verify the task port the server receives is
  usable for K3 (it's the real host task port — it is).

### P — Multi-process
- P1. wineserver under ocerz: needs S1, S3, fork (have), fcntl locks (verify),
  kqueue loop (have), self-pipe signals (needs G2 minimal: handlers + pipe write).
- P2. Scaffold option for de-risking client work: temporarily run wineserver native
  under Rosetta (plain x86_64 binary; is_rosetta()=true conveniently disables the
  debug-register path). End state remains everything-under-ocerz; the scaffold also
  breaks K3 (native server would read un-translated low-window addresses), so it is
  only viable until the first ReadProcessMemory — use deliberately, then retire.

### U — GUI (winemac.drv)
- winemac.so links AppKit/Metal/OpenGL/QuartzCore/Carbon. The Cocoa window milestone
  (2026-06-10) already proves AppKit-under-ocerz; apple_main_thread parks the main
  thread in CFRunLoop and runs Wine on a second thread — matches ocerz's proven
  HOSTWQ/run-loop capabilities. Expect new walls (CGS/Metal surfaces) but no known
  architectural blocker.

---

## 4. Phases, milestones, exit criteria

Each phase ends make-check-green with the GUI window test still 6/6.

- **Phase 0 — Groundwork (small).** Startup probe choosing the shadow base;
  OCERZ_LOWBASE plumbing through execve/posix_spawn; decide guest-visible
  proc_translated answer; capacity audit (DYLDAPI_DISK_MAX, fd tables).
  Exit: probes logged, env inherited across re-exec, design constants fixed.
- **Phase 1 — Memory architecture (M1–M5, M-L1/L2).** The wine loader maps at
  slide 0, WINE_RESERVE/TOP_DOWN reserved, init_reserved_areas re-mmaps them,
  ntdll.so dlopens, virtual_init completes (preload_info consumed, user_shared_data
  + dispatcher page at 0x7ffe0000/0x7ffe1000 in the shadow window).
  Exit: `ocerz bin/wine notepad` advances past "cannot map segments" to a
  server/signal-era wall; a minimal fixed-address non-PIE test binary maps+runs.
- **Phase 2 — Syscall surface (S1–S4).** Exit: wine reaches server_init_process,
  spawns wineserver (under ocerz, or scaffold if P2 chosen), completes the socket +
  SCM_RIGHTS handshake; `wineserver` under ocerz idles in its kqueue loop.
- **Phase 3 — Guest signals (G1–G5).** Exit: targeted tests — guest sigaltstack
  SEGV handler runs and sigreturns; SIGUSR1 suspend round-trip; then wineboot.exe
  completes prefix init under ocerz (the real integration test).
- **Phase 4 — Mach guest-thread semantics (K1–K4).** Exit: services.exe/
  winedevice.exe/explorer.exe startup chorus runs; GetThreadContext-class APIs
  behave per Rosetta precedent; cross-process R/W verified via the low_delta rule.
- **Phase 5 — GUI (U).** Exit: notepad.exe window visible from inside ocerz
  (`ps` shows it as an ocerz process). Stretch: typing + saving a file.
- **Phase 6 — Purity & hardening.** Retire any scaffold; wineserver under ocerz
  if deferred; perf passes (JIT coverage of hot Wine paths); 16-probe suite stays
  green; document the wine recipe.

Sequencing notes: Phases 1→2→3 are strictly ordered; 4 overlaps 3; 5 only needs
1–4 partially (winemac loads late). Phase 3 is the largest single body of new code
and has value far beyond Wine (any program using signals).

---

## 5. Risk register

- R1 (high, architectural): low-shadow g2h breaks hidden identity assumptions
  (raw casts, single-level ptr_mask). Mitigation: M5 audit + a debug mode poisoning
  the low guest range mapping until explicitly mapped.
- R2 (high): guest signal frames must match Darwin x86_64 layout exactly
  (ucontext64/mcontext64/sigtramp ABI); Wine's handlers read mcontext deeply.
  Mitigation: differential tests against a native x86_64 signal program (Rosetta
  ground truth) before integrating.
- R3 (medium): wineserver↔client Mach flows under double-ocerz (task ports, port
  right extraction) — believed pass-through-clean since guest threads are 1:1 host
  threads; verify early with a 2-process ocerz IPC probe.
- R4 (medium): instant-return sleep stubs cause spins/races in Wine's startup;
  fix in S3 before debugging "mystery hangs".
- R5 (medium): performance — Wine startup executes orders of magnitude more code
  than the Cocoa test (which takes ~20s). Expect minutes at first; JIT hot paths.
- R6 (low): this Wine install is MNC/D3DMetal-patched (winemac.so/opengl32.so
  replaced); if its hacks assume real-Rosetta quirks, behavior may diverge —
  compare against a stock Wine if confusing walls appear.
- R7 (low): macOS updates move the protected VA bands (0x4000000000 EACCES today);
  the Phase-0 probe must re-run every boot, never hard-code.

## 6. Cross-checks for the implementer

- The native loader was watched mapping WINE_RESERVE as `---/rwx SM=NUL` — ocerz's
  PROT_NONE reservation matches native semantics, not an approximation.
- WINE_TOP_DOWN is ~32MB, not gigabytes; identity-map it.
- bin/wine (PIE launcher) needs none of this — only the x86_64-unix loader does.
- i386-unix does not exist; i386 PE coverage is new-WoW64 (wow64.dll/wow64cpu.dll
  present, i386-windows has 821 PE files) — 32-bit apps come much later via WoW64,
  not via a 32-bit unix loader.
- Audit transcripts: /tmp/a5_vmmap.txt, /tmp/a5_pstree.txt, /tmp/a5_lsof.txt
  (native run), /tmp/shadow_probe.c, /tmp/ocerz_pz_test.c (kernel A/B tests);
  workflow wf_c8a882d2-4c9 (2026-06-10).
