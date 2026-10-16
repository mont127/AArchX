/*
 * i386 (32-bit) Mach-O guests, "m32".
 *
 * Design: docs/m32.md.  The guest runs as i386 in a
 * flat 4 GB window (ocerz_mem_init(0, 4 GB)): guest address g is host ocerz_guest_base + g, and the guest never
 * holds a host address.  Imports from system libraries are addresses inside the DYLDAPI trap window; the
 * interpreter and the JIT route every address there to ocerz_dyldapi_dispatch, which hands 32-bit cpus to
 * m32_trap.  The offset of an address in the window is the export's id.
 */
#ifndef OCERZ_M32_H
#define OCERZ_M32_H

#include <string.h>
#include "ocerz/cpu.h"
#include "ocerz/mem.h"
#include "ocerz/dyldapi.h"

struct OcerzVM;

#define M32_WINDOW       0x100000000ull
#define M32_CS           0x0f             /* LDT selector 1, TI, RPL 3: 32-bit code */
#define M32_HANDLE_LO    0xE0000000u      /* handle cells, 8 bytes each */
#define M32_HANDLE_HI    0xF0000000u
#define M32_TOP_LO       0xF0000000u      /* reserved, never mapped */
#define M32_TRAP(id)     ((uint32_t)(OCERZ_DYLDAPI_LO + (id)))
#define M32_ID_SENTINEL  0xfff0           /* return address of calls into the guest */
#define M32_ID_EXIT      0xfff4           /* main()'s return address: exit(eax) */
#define M32_ID_TLV       0xfff8           /* __tlv_bootstrap thunk */
#define M32_ID_DYLD      0xfffc           /* the entry of the dyld a self-loading program maps (src/m32_selfload.c) */
#define M32_ID_FIRST     0x0010           /* first export id */
#define M32_ID_LIMIT     0xfff0

typedef int (*M32Special)(struct OcerzVM *vm, OcerzCPU *cpu);
typedef struct M32SpecialEntry { const char *name; M32Special fn; } M32SpecialEntry;   /* tables end with NULL */
extern const M32SpecialEntry m32_stdio_specials[], m32_cf_specials[], m32_thread_specials[], m32_dyld_specials[];
extern const M32SpecialEntry m32_zlib_specials[];   /* src/m32_zlib.c */
extern const M32SpecialEntry m32_unwind_specials[];   /* src/m32_unwind.c: C++ exceptions */
extern const M32SpecialEntry m32_gl_specials[];   /* src/m32_gl.c: mapped GL buffers */
extern const M32SpecialEntry m32_audio_specials[];   /* src/m32_audio.c: CoreAudio render callbacks */
extern const M32SpecialEntry m32_atsu_specials[];   /* src/m32_atsu.c: ATSUI text and ATS fonts */
extern const M32SpecialEntry m32_qd_specials[];     /* src/m32_qd.c: QuickDraw ports, GWorlds and drawing */
extern uint32_t m32_argc_var, m32_argv_var, m32_environ_var, m32_progname_var;   /* guest variables */
int m32_printf_slots(const char *fmt, uint32_t ap, uint64_t *slots, int max, const char *who);

int m32_is_i386(const char *path);
int m32_run(struct OcerzVM *vm, const char *path, int argc, char **argv, char **envp);
extern int m32_active;   /* set by m32_run: the process is an i386 Mach-O program, not Wine's WoW64 */
int m32_trap(struct OcerzVM *vm, OcerzCPU *cpu);
/* src/m32_selfload.c: programs that load themselves, as a UPX-packed executable does */
int m32_raw_syscall(struct OcerzVM *vm, OcerzCPU *cpu, int sysenter);   /* sysenter, or int 0x80 */
int m32_dyld_entry(struct OcerzVM *vm, OcerzCPU *cpu);                   /* the M32_ID_DYLD trap */
uint32_t m32_mmap(uint32_t want, uint32_t len, int prot, int flags, int fd, uint64_t off);   /* src/m32_libsystem.c */
void m32_selfload_begin(const char *path);                                 /* m32_run, before the program starts */
void *m32_carbon_wrap(const char *symbol, void *fn);   /* src/m32_carbon.c: the host function an export calls */
M32Special m32_carbon_special(const char *symbol);      /* src/m32_carbon.c: the UPP calls, by name pattern */
extern const M32SpecialEntry m32_carbon_specials[]; /* src/m32_carbon.c: Carbon event parameters */
uint32_t m32_qd_window_port(void *window);             /* src/m32_qd.c: a window's QuickDraw port, made if need be */
uint32_t m32_qd_current_port(void);                    /* the current QuickDraw port, or 0 */
void m32_qd_set_current_port(uint32_t port);           /* SetPort, for host code calling guest drawing */
void m32_qd_quiet(int on);                              /* drawing asks no view to redraw while on (a view drawing) */
void m32_qd_show_window_rect(void *window, void *cg_context, double x, double y, double w, double h);   /* port pixels */
extern const M32SpecialEntry m32_controls_specials[]; /* src/m32_controls.c: menus and controls arm64 dropped */
extern const M32SpecialEntry m32_tsearch_specials[];  /* src/m32_tsearch.c: tsearch's trees, in guest memory */
void m32_qd_window_gone(void *window);                 /* src/m32_qd.c: DisposeWindow drops the window's port */
void *m32_qd_port_window(uint32_t port);               /* a QuickDraw port's host window, or NULL */
int m32_qd_port_pixels(uint32_t port, int *w, int *h, uint32_t *rowbytes, uint32_t *pixels);   /* a GWorld's */
extern const M32SpecialEntry m32_agl_specials[];    /* src/m32_agl.c: AGL on the arm64 AGL, ports as drawables */
int m32_fastcall(struct OcerzVM *vm, OcerzCPU *cpu, uint32_t trap);   /* a trap made from inside a block */
uint32_t m32_export_special(const char *name, M32Special fn);
/* the Steam bridge (src/m32_steam.c): a steamclient.dylib with no i386 slice opens as the host's own */
uint32_t m32_steam_dlopen(const char *path);
uint32_t m32_steam_dlsym(uint32_t handle, const char *sym);
/* imports the JIT calls straight from a stub block (m32_leaf.c): nargs 4-byte cdecl arguments, the pointers among
 * them (bit k: argument k) passed as host addresses.  simd: the host code it calls may use SIMD registers, so the
 * JIT saves the guest's xmm around it.  tramp: called as m32_leaf_tramp(leaf, raw arguments...), which converts the
 * pointer arguments with m32_host as m32_cross does and masks the byte ones before calling fn */
typedef struct M32Leaf { const char *name; void *fn; uint8_t nargs, ptrs, ret, simd, tramp, bytes; } M32Leaf;
uint64_t m32_leaf_tramp(const M32Leaf *l, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5, uint64_t a6);
enum { M32_LEAF_I32, M32_LEAF_I64, M32_LEAF_ARG0, M32_LEAF_VOID };
const M32Leaf *m32_leaf_for(uint32_t trap);

static inline void *m32_h(uint32_t g) { return (void *)(uintptr_t)(ocerz_guest_base + g); }
static inline uint32_t m32_rd(uint32_t g) { uint32_t v; memcpy(&v, m32_h(g), 4); return v; }
static inline void m32_wr(uint32_t g, uint32_t v) { memcpy(m32_h(g), &v, 4); }
static inline int m32_in_window(const void *h) { return (uint64_t)(uintptr_t)h - ocerz_guest_base < M32_WINDOW; }
static inline uint32_t m32_g(const void *h) { return (uint32_t)((uint64_t)(uintptr_t)h - ocerz_guest_base); }
/* cdecl argument n (0-based) of the call being trapped: [esp + 4 + 4n] */
static inline uint32_t m32_arg(const OcerzCPU *cpu, int n) { return m32_rd((uint32_t)cpu->gpr[OCERZ_RSP] + 4 + 4 * n); }
static inline uint64_t m32_arg64(const OcerzCPU *cpu, int n) { return m32_arg(cpu, n) | (uint64_t)m32_arg(cpu, n + 1) << 32; }

void m32_ret(OcerzCPU *cpu, uint32_t eax, uint32_t edx, int pop_extra);
void m32_ret_st0(OcerzCPU *cpu, double v);
static inline void m32_ret64(OcerzCPU *cpu, uint64_t v) { m32_ret(cpu, (uint32_t)v, (uint32_t)(v >> 32), 0); }

/* exports: every import from a system library gets an id and a trap address */
typedef struct M32Export {
    char *lib;
    char *name;
    M32Special special;
    int kind;                 /* M32_EX_* */
    const char *reason;
    const char *variant;      /* the import's suffix ($INODE64, ...) or NULL */
    char *host_symbol;
    void *host_fn;            /* functions: the host entry point; data: the host variable */
    char *gnote, *hnote;      /* fn32 notations */
    struct M32Sig *gsig, *hsig;
    uint32_t data;            /* data imports: the guest variable */
    int gsize, hsize;
    char *data_kind;
    int file_ret;             /* returns a FILE *: the guest gets m32_file's twin */
} M32Export;
enum { M32_EX_UNRESOLVED, M32_EX_SPECIAL, M32_EX_FN, M32_EX_BAD, M32_EX_DATA };

uint32_t m32_static_alloc(uint32_t size, uint32_t align);   /* zeroed guest memory kept for the whole run */
#define M32_TSD_KEYS 768   /* Darwin's thread-specific data slots */
void m32_tsd_set(uint32_t key, uint32_t value);   /* src/m32_thread.c: the calling thread's gs:[key * 4] */
uint32_t m32_static_code(uint32_t size);                    /* the same, for i386 code m32 generates */
/* the guest heap (src/m32_mem.c): a dlmalloc mspace inside the window */
int m32_heap_init(void);
uint32_t m32_map(uint64_t n);   /* a read-write mapping below the handle cells, or 0 */
uint32_t m32_malloc(uint32_t n);
uint32_t m32_calloc(uint32_t n, uint32_t size);
uint32_t m32_realloc(uint32_t g, uint32_t n);
void m32_free(uint32_t g);
uint32_t m32_memalign(uint32_t align, uint32_t n);
uint32_t m32_msize(uint32_t g);
uint32_t m32_strdup_host(const char *s);   /* a guest-heap copy the guest may free */

/* the pointer rule (src/m32_handle.c) */
uint32_t m32_handle(void *host);          /* NULL->0, in window->offset, twin->guest, else a handle cell */
void *m32_host(uint32_t g);               /* 0->NULL, handle->host, alias->host, else the window address */
int m32_is_handle(uint32_t g);
void m32_alias(uint32_t g, void *host);   /* guest-resident object g stands for host */
uint32_t m32_handle_count(void);
uint32_t m32_cstring(const char *host);   /* guest copy of a host C string (in-window strings stay put) */
uint32_t m32_handle_twin_lookup(void *host);   /* the guest object aliased to host, or 0 */
uint32_t m32_file(void *host_file);   /* src/m32_stdio.c: the guest FILE standing for a host one */
void m32_hud_start(void);             /* src/m32_bridge.c: OCERZ_M32_HUD's overlay */
void m32_activate_soon(void);         /* src/m32.c: the game's app comes to the front */
uint32_t m32_export_hostfn(void *fn, const char *gnote, const char *hnote);   /* host function as guest address */

int m32_cross(struct OcerzVM *vm, OcerzCPU *cpu, M32Export *e);          /* src/m32_cross.c */
int m32_export_sigs(M32Export *e);   /* parses e->gsig and e->hsig once; 0 when the notations do not parse */
extern __thread void *m32_cross_self;
int m32_call_native_catching(const void *fn, const uint64_t *x, const uint64_t *v, const uint64_t *stack,
                             uint64_t stackbytes, void *x8, uint64_t *out_x, uint64_t *out_v, void **exception);
void *m32_callback(uint32_t guest_fn, const char *guest_sig, const char *host_sig);   /* src/m32_callback.c */
uint32_t m32_call(struct OcerzVM *vm, uint32_t fn, const uint32_t *args, int nargs, uint32_t *edx, double *st0);
OcerzCPU *m32_thread_attach(struct OcerzVM *vm);   /* a 32-bit guest cpu for a thread the host started */
extern int m32_started;                              /* the main cpu is running (before: loader calls use it) */
int m32_tlv_trap(struct OcerzVM *vm, OcerzCPU *cpu);   /* src/m32_thread.c */
void m32_save_errno(OcerzCPU *cpu);
uint32_t m32_special_data(const char *symbol, void *host_var);   /* guest variable for a special data import, or 0 */
void m32_refresh_data(void);   /* re-read host pointer variables that changed (NSApp) */

uint32_t m32_export(const char *lib, const char *name);     /* trap address for an import */
M32Export *m32_export_by_id(uint32_t id);
M32Special m32_special(const char *symbol);                 /* NULL when none */
void m32_log_once(const char *what, const char *name);
extern int m32_log_imports;

#endif
