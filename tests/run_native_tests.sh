#!/usr/bin/env bash
# Gate for the native execution mode (M2): the mode exists, it is selectable
# from the command line and from the environment, and with the shared cache
# switched off a dynamically linked guest binds every one of its system imports
# against a libSystem that ocerz synthesizes in memory, reaches main, calls
# real host code through those imports, and runs to completion.
#
# That last step is what M2 moved, and it is why the headline native case is no
# longer an exit code but an output comparison. Under M0 nothing answered a
# libSystem import at all: every one was collected at the end of the main
# image's fixups, printed as an "ocerz: native: no bridge for <sym> in <dylib>"
# line, and the process exited 71 without the guest running an instruction of
# its own. Under M1 the imports resolved to stubs inside a virtual
# /usr/lib/libSystem.B.dylib, so the guest ran, and the first call through one
# of those stubs printed a single line and exited 72. Under M2 the stub trap
# reads the arguments out of the guest's x86 register state, calls the real
# arm64 function linked into ocerz and puts the result back in RAX, so the call
# returns and the guest keeps going.
#
# An exit code cannot prove that. A guest that ran to completion having been
# handed nonsense by every bridged call exits 0 just as happily as one that was
# handed the right answers, so the native cases compare OUTPUT instead: the
# same binary, the same arguments, run under -native and under -cache, must
# write byte-identical stdout. Cache mode reaches the system libraries the way
# the rest of the suite does, through the real shared cache, so agreeing with
# it is agreeing with the real libSystem. xbench_dyn is the case worth having
# because its kernels checksum their own work: a memcpy that copied the wrong
# bytes or a strcmp that ordered two strings backwards changes the checksum,
# and the checksum is the only thing it prints. Two kernels are run rather than
# one, and each under both the JIT and -no-jit, so a bridge that only agrees
# with the translator it was debugged against is caught.
#
# The other native fixture is bridge_probe, compiled at test time because it is
# the only way to exercise the bridged calls xbench does not make. It checks
# its own results and prints one line per group -- str, mem, heap, env, misc,
# io -- each either "<group> ok" or "<group> bad:<hex>", where the hex is a
# bitmask of the checks in that group that did not hold, numbered in source
# order from bit 0. That keeps a failure legible without turning the output
# into a transcript, and keeps it exactly comparable against the cache-mode
# run. It writes those lines with write(2) and the last one with putchar and
# puts, so both paths out of a guest are covered and the buffered one is last
# in program order, where a flush at exit cannot reorder it.
#
# Two of its groups get their own cases. heap does a malloc/fill/read-back/free
# round trip, including one allocation large enough that no arena serves it,
# and env reads back a variable this script puts in the environment. Both are
# questions about the address model rather than about any one function: the
# first asks whether the guest can dereference memory the HOST heap handed it,
# the second whether it can dereference a pointer into the host's own
# environment. Native mode runs in the identity map, so both should be a
# formality, and either one failing means the map is not what the bridge
# assumes rather than that malloc or getenv is wrong.
#
# Keeping 71 and 72 alive still matters, and both now need their own fixture.
# 71 means nothing bound, so the loader never handed control to the guest at
# all; native_unbound pins it with a two-line program calling getpwnam, which
# is deliberately outside the virtual library's export list. 72 means
# everything bound and the guest ran, and what is missing is the bridge behind
# one export rather than the export itself; xbench_dyn no longer reaches it, so
# bridge_unimpl pins it with a program whose only import is printf, which the
# virtual library exports and the bridge deliberately does not implement -- it
# is variadic, Apple's arm64 passes variadic arguments on the stack where x86-64
# passes them in registers, and no fixed signature can say where the named
# arguments stop. That fixture's one import used to be qsort, until M5 gave a
# callback a way back into guest code. Being the only import, it is also the
# only symbol the bridge line can name, so that case pins the symbol as well as
# the shape of the line, and it checks the fixture's import table with nm first
# so that a toolchain which starts importing something else is reported as that
# rather than as a bridge naming the wrong export. Both fixtures skip where
# there is no x86_64 clang, the way the dynamic gate does.
#
# Cache mode must come out of all this untouched, and it is checked the way the
# rest of the suite checks translation: the JIT and the interpreter running the
# same command must agree byte for byte. That needs no Rosetta on the box.
#
# M4 adds the one fault the mode could not previously explain. A crossing runs
# the host's own arm64 code on a thread the guest is driving, so a guest that
# hands strlen a pointer it had no business handing it faults inside Apple's
# code, at an instruction pointer belonging to nothing the translator emitted.
# The crash handler used not to ask whether a crossing was in flight, so it
# blamed the guest anyway: the fault was either delivered as an access
# violation at whatever rip the crossing trapped from, which is an instruction
# that did nothing wrong, or handed to a JIT recovery that tried to rebuild
# guest state from a host pc that had never been in the arena. Both read as a
# translator bug. A crossing is now looked for before anything else attributes
# the fault, and one taken inside a crossing prints a report naming the library,
# the export, its signature, the host function, the signal, the faulting
# address and the host pc, says which side of the guest boundary that address
# fell on -- inside means the guest passed a bad pointer, outside means ocerz
# marshalled the crossing wrong -- and stops the process rather than delivering
# or recovering, because the thread is several native frames deep in code ocerz
# can neither resume nor unwind.
#
# bridge_fault_native pins that report, against a fixture whose only unusual
# act is calling strlen on a page of guest space nobody mapped.
# bridge_fault_not_guest pins the other half from a second run of the same
# fixture with OCERZ_FAULTLOG and OCERZ_SIGTRACE set, which are the two knobs
# that make the old attribution path say out loud that it ran: for the bad
# address neither may print, no guest-crash report may appear, and the line the
# fixture writes after the call must never be reached. A knob that had been
# renamed would be silent for the wrong reason and the case would pass on
# nothing, so it is paired with a control run of the frame-lowered fixture under
# the same knobs, whose fault at the same address really is the guest's: both
# knobs have to speak there before their silence over the crossing means
# anything. Both halves are needed,
# because a report that names the symbol correctly and then ALSO delivers the
# fault to the guest would satisfy a test that only looked for the report. The
# status is 139, which is what the misattributing path exited with too, so the
# status distinguishes nothing here and the message carries the whole
# difference.
#
# bridge_fault_cache is the regression guard that matters most, because cache
# mode is what runs Steam and Wine today and M4 has to be purely additive. The
# same fixture under -cache crosses no bridge at all -- strlen there is the real
# libSystem's, translated like everything else -- so it must still be an
# ordinary guest fault, reported as one at the address the guest actually
# dereferenced, with the JIT and the interpreter agreeing on the status, and
# with no bridged-call report anywhere in either.
#
# bridge_frame_lowered is the failure mode of a frame raised and not lowered. A
# fixture that makes a bridged call, sees it return, and only then faults in its
# own code has to be blamed the old way, because by then it is guest code
# faulting again. A missing lower would not show up here as a wrong answer; it
# would show up much later as some unrelated fault reported as being inside a
# call that had returned long before, which is a miserable thing to diagnose
# from the report alone.
#
# bridgelog covers OCERZ_BRIDGELOG, which prints one line per crossing naming
# the library, the export and its signature. The case checks that the lines
# appear, that they name the library and an export the fixture's own import
# table lists, and that stdout is byte-identical to the same run without the
# variable: a diagnostic that changes what it observes is worse than no
# diagnostic.
#
# M5 adds calls in the other direction. Until M5 every crossing went from guest
# code into native code, but qsort and bsearch take a function pointer and call
# it, and when the guest supplies one it names x86 code native code cannot jump
# to. Such an argument is now interned to a slot of a fixed bank of arm64
# trampolines, and native code calling the slot runs the guest function on the
# calling thread, below the native caller's stack pointer, and gets its result
# back. Five fixtures cover that end to end, compiled at test time like
# bridge_probe and importing only functions the bridge implements. The four that
# are meant to succeed check their own work and write a status line first,
# "<name> ok ..." or "<name> bad:<hex> ...", with the bits numbered in source
# order the way the probe's are, and each case's failure message says what every
# bit means.
#
# callback_qsort sorts three hundred distinct ints up, down and up again with
# two guest comparators, so sorted and the right permutation are one check: the
# result must be exactly -150..149. The comparators return plus or minus 65536
# rather than one, so a result cut down to a byte or a word on the way back
# reads as equality and the sort comes out wrong. callback_nested_bridge sorts
# strings with a comparator that calls strcmp, which is a bridged crossing
# inside a guest callback inside a bridged crossing, checks the order with a
# comparison of its own rather than with the strcmp under test, and then puts
# the sorted strings themselves, so the cache comparison sees the order too.
# callback_bsearch looks up every present key and a set of absent ones through
# a key whose layout differs from the element's, and its comparator checks that
# the key pointer arrives unchanged and the element pointer lands on an element,
# so arguments delivered in the wrong order or converted wrongly cannot find
# anything. callback_recursion gives each comparator a qsort of its own over a
# small local array, with the other comparator, twenty-four levels deep, so
# native and guest frames alternate on one thread; every level must be entered
# once and left once in order, run the comparator it was handed, sort its array,
# and find its caller's elements unchanged when the level below returns, which
# is what a guest stack placed on top of a live native frame would break. Every
# comparator also checks that it was entered with the stack aligned the way the
# System V ABI promises. All four run under the JIT and under -no-jit, which
# must agree byte for byte, and native output must equal cache mode's, where the
# same comparators are called by the real x86 libSystem.
#
# callback_guest_fault is the reason M5 clears the thread's bridge frame while
# guest code runs. Its comparator dereferences the same unmapped guest address
# the M4 fixtures use, 0x6000000000 and never 0x500000000, which is where
# ocerz_vm_call maps its readable return sentinel, so a read there succeeds and
# the case would fail for a reason unrelated to bridges. When the comparator
# faults, native qsort is still live on the host stack, and a frame left
# raised for that stretch would send the fault down M4's path: a BRIDGE-FAULT
# report blaming _qsort and an immediate stop, for a fault that is entirely the
# guest's, and one a guest with its own SIGSEGV handler is entitled to recover
# from. So the case demands exactly what bridge_frame_lowered demands of a
# fault in plain guest code -- a guest-crash report naming the address, status
# 139, the line after qsort never written -- under both engines, and fails on
# any BRIDGE-FAULT line at all.
#
# The callback cases skip where there is no x86_64 clang, like the others, but a
# callback fixture that fails to compile where a trivial x86_64 program compiles
# fine is a failure: skipping it would hide a broken fixture indefinitely.
#
# The cases that need a mappable shared cache are skipped, not failed, where
# there is none. The native cases still run there -- not needing a cache is the
# entire point of the mode -- but with nothing to compare against, the
# comparison halves of the native cases skip too, and what is left is the
# fixture's own self-check.

set -u
cd "$(dirname "$0")/.."
OCERZ=./ocerz
TMP="${TMPDIR:-/tmp}/ocerz_native.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

DYN=tests/guest/benchbin/xbench_dyn
STATIC=tests/guest/bin/exit42
KERNEL=depchain
KERNELS="depchain memcpy"
SCALE=1000
LIB=/usr/lib/libSystem.B.dylib
UNIMPL_SYM=_printf
BRIDGE_RE='^ocerz: bridge: [^ ]+ [^ ]+ not implemented$'
NOBIND='ocerz: native: no bridge for '
M0_SUMMARY='unresolved imports, no virtual frameworks are implemented yet'
PROBE_VAR=OCERZ_BRIDGE_PROBE
PROBE_VAL=bridge-probe-42
PROBE_BIN=""
UNIMPL_BIN=""
BADPTR_BIN=""
AFTER_BIN=""
BAD_GUEST_ADDR=0x6000000000
BADPTR_SYM=_strlen
BADPTR_SIG='L(p)'
BADPTR_MARK='badptr enter'
BADPTR_PAST='badptr returned'
AFTER_MARK='after bridged'
AFTER_PAST='after returned'
BRIDGE_FAULT_RE='^ocerz: BRIDGE-FAULT\[[0-9]+\] (SIGSEGV|SIGBUS) inside a bridged call'
BRIDGE_FAULT_STATUS=139
GUEST_FAULT_STATUS=139
GUEST_CRASH='ocerz: guest crash['
WILD_RE='ocerz: (WILD-FAULT-AV|WILD-WORKER-TERMINATE|gs0x320 WORKER-TERMINATE)'
FAULTLOG_MARK="FAULT-MAP addr=$BAD_GUEST_ADDR"
SIGTRACE_MARK="deliver addr=$BAD_GUEST_ADDR"
BRIDGELOG_RE='^ocerz: BRIDGELOG\[[0-9]+\] [^ ]+ [^ ]+ [^ ]+$'
BRIDGELOG_KERNEL=memcpy
BRIDGELOG_SYM=_memcpy
BRIDGELOG_SIG='p(ppL)'
X86_CLANG=0
CB_QSORT_BIN=""
CB_NESTED_BIN=""
CB_BSEARCH_BIN=""
CB_RECURSION_BIN=""
CB_FAULT_BIN=""
CB_FAULT_MARK='cbfault enter'
CB_FAULT_PAST='cbfault returned'

unset OCERZ_MODE
unset OCERZ_BRIDGE_PROBE_UNSET
unset OCERZ_BRIDGELOG

if [ ! -x "$OCERZ" ]; then
    echo "error: ocerz binary not found or not executable at $OCERZ" >&2
    echo "build it first with: make ocerz" >&2
    exit 2
fi
if [ ! -x "$DYN" ] || [ ! -x "$STATIC" ]; then
    echo "run_native_tests: SKIP (guest fixtures missing; run: make -C tests/guest)"
    exit 0
fi

pass=0
fail=0

TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN="gtimeout"
fi
NATIVE_TIMEOUT=60

run_bounded() {
    local out_file="$1" err_file="$2"
    shift 2
    if [ -n "$TIMEOUT_BIN" ]; then
        "$TIMEOUT_BIN" "${NATIVE_TIMEOUT}s" "$@" >"$out_file" 2>"$err_file" </dev/null
        return $?
    fi
    "$@" >"$out_file" 2>"$err_file" </dev/null &
    local pid=$!
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$NATIVE_TIMEOUT" ]; then
            kill -TERM "$pid" 2>/dev/null
            sleep 1
            kill -KILL "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            return 124
        fi
        sleep 1
        waited=$((waited + 1))
    done
    wait "$pid"
    return $?
}

record() {
    local name="$1" reason="$2" detail="${3:-}"
    if [ -z "$reason" ]; then
        echo "PASS $name${detail:+ ($detail)}"; pass=$((pass+1))
    else
        echo "FAIL $name ($reason)"; fail=$((fail+1))
    fi
}

cache_line_seen() {
    grep -q 'shared cache mapped' "$@"
}

bridge_sym() {
    grep -hE "$BRIDGE_RE" "$@" 2>/dev/null | head -1 | awk '{print $4}'
}

# empty on success, otherwise why this native run did not reach a clean exit 0
native_run_reason() {
    local rc="$1"
    shift
    if grep -Fq "$NOBIND" "$@" 2>/dev/null; then
        echo "an import went unresolved: $(grep -hF "$NOBIND" "$@" | head -1)"
    elif grep -qE "$BRIDGE_RE" "$@" 2>/dev/null; then
        echo "stopped at an unbridged export: $(grep -hE "$BRIDGE_RE" "$@" | head -1)"
    elif cache_line_seen "$@"; then
        echo "native mode mapped the shared cache"
    elif [ "$rc" -ne 0 ]; then
        echo "exit $rc, want 0"
    else
        echo ""
    fi
}

CACHE_OK=1
run_bounded "$TMP/probe.out" "$TMP/probe.err" "$OCERZ" "$DYN" "$KERNEL" 1
case $? in
    65|70) CACHE_OK=0 ;;
esac

NOUT="$TMP/native.$KERNEL.jit.out"
NERR="$TMP/native.$KERNEL.jit.err"
CACHE_OUT="$TMP/cache.$KERNEL.jit.out"

build_fixtures() {
    local src="$TMP/bridge_probe.c" bin="$TMP/bridge_probe"
    local usrc="$TMP/unimpl.c" ubin="$TMP/unimpl"
    local bsrc="$TMP/badptr.c" bbin="$TMP/badptr"
    local asrc="$TMP/after.c" abin="$TMP/after"

    cat > "$src" <<'EOC'
typedef __SIZE_TYPE__ bp_size;

void *malloc(bp_size);
void *calloc(bp_size, bp_size);
void *realloc(void *, bp_size);
void free(void *);
bp_size strlen(const char *);
bp_size strnlen(const char *, bp_size);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, bp_size);
char *strcpy(char *, const char *);
char *strncpy(char *, const char *, bp_size);
char *strcat(char *, const char *);
char *strchr(const char *, int);
char *strrchr(const char *, int);
char *strstr(const char *, const char *);
char *strdup(const char *);
void *memcpy(void *, const void *, bp_size);
void *memmove(void *, const void *, bp_size);
void *memset(void *, int, bp_size);
int memcmp(const void *, const void *, bp_size);
void *memchr(const void *, int, bp_size);
char *getenv(const char *);
long write(int, const void *, bp_size);
long read(int, void *, bp_size);
int close(int);
int puts(const char *);
int putchar(int);
int *__error(void);
int abs(int);
long labs(long);
int atoi(const char *);
long atol(const char *);
int isatty(int);
int getpid(void);
long time(long *);
unsigned long clock(void);

#define CK(cond) do { if (!(cond)) m |= bit; bit <<= 1; } while (0)

static int g_io_ok = 1;
static int g_bad;

static int sgn(int v)
{
    return v < 0 ? -1 : (v > 0 ? 1 : 0);
}

static void emit(const char *s, bp_size n)
{
    if (write(1, s, n) != (long)n)
        g_io_ok = 0;
}

static bp_size put_str(char *p, const char *s)
{
    bp_size i = 0;
    while (s[i]) {
        p[i] = s[i];
        i++;
    }
    return i;
}

static bp_size put_hex(char *p, unsigned v)
{
    const char *d = "0123456789abcdef";
    int i;
    for (i = 0; i < 8; i++)
        p[i] = d[(v >> (28 - 4 * i)) & 15];
    return 8;
}

static void line_status(const char *name, unsigned mask)
{
    char b[64];
    bp_size n = put_str(b, name);

    b[n++] = ' ';
    if (mask == 0) {
        n += put_str(b + n, "ok");
    } else {
        n += put_str(b + n, "bad:");
        n += put_hex(b + n, mask);
        g_bad = 1;
    }
    b[n++] = '\n';
    emit(b, n);
}

static const char s_a[] = "ocerz bridge probe";
static const char s_b[] = "ocerz bridge probes";

static unsigned group_str(void)
{
    unsigned m = 0, bit = 1;
    char buf[64], cp[16];
    char *d;

    CK(strlen(s_a) == 18);
    CK(strnlen(s_a, 5) == 5);
    CK(strnlen(s_a, 64) == 18);
    CK(sgn(strcmp(s_a, s_a)) == 0);
    CK(sgn(strcmp(s_a, s_b)) == -1);
    CK(sgn(strcmp(s_b, s_a)) == 1);
    CK(sgn(strncmp(s_a, s_b, 18)) == 0);
    CK(sgn(strncmp(s_a, s_b, 19)) == -1);
    CK(strcpy(buf, "ocerz") == buf);
    CK(strlen(buf) == 5);
    CK(strcat(buf, "-probe") == buf);
    CK(sgn(strcmp(buf, "ocerz-probe")) == 0);
    CK(strlen(buf) == 11);
    CK(strchr(buf, '-') == buf + 5);
    CK(strchr(buf, 'z') == buf + 4);
    CK(strchr(buf, 'q') == 0);
    CK(strrchr(buf, 'e') == buf + 10);
    CK(strstr(buf, "probe") == buf + 6);
    CK(strstr(buf, "zzz") == 0);
    memset(cp, 'x', sizeof cp);
    CK(strncpy(cp, s_a, 5) == cp);
    cp[5] = 0;
    CK(sgn(strcmp(cp, "ocerz")) == 0);
    d = strdup(s_a);
    CK(d != 0 && d != s_a && sgn(strcmp(d, s_a)) == 0);
    free(d);
    return m;
}

static unsigned char m_buf[512];

static unsigned group_mem(void)
{
    unsigned m = 0, bit = 1;
    unsigned char ref[64];
    int i;

    CK(memset(m_buf, 0xa5, sizeof m_buf) == m_buf);
    CK(m_buf[0] == 0xa5 && m_buf[511] == 0xa5);
    CK(memset(m_buf, 0, 64) == m_buf);
    for (i = 0; i < 64; i++)
        ref[i] = 0;
    CK(sgn(memcmp(m_buf, ref, 64)) == 0);
    CK(m_buf[64] == 0xa5);

    for (i = 0; i < 64; i++) {
        m_buf[i] = (unsigned char)(i * 7 + 3);
        ref[i] = (unsigned char)(i * 7 + 3);
    }
    CK(memcpy(m_buf + 256, m_buf, 64) == m_buf + 256);
    CK(sgn(memcmp(m_buf + 256, ref, 64)) == 0);
    ref[10] = (unsigned char)(ref[10] ^ 0xff);
    CK(sgn(memcmp(m_buf + 256, ref, 64)) != 0);
    ref[10] = (unsigned char)(ref[10] ^ 0xff);

    CK(memmove(m_buf + 8, m_buf, 64) == m_buf + 8);
    CK(sgn(memcmp(m_buf + 8, ref, 64)) == 0);
    CK(memmove(m_buf, m_buf + 8, 64) == m_buf);
    CK(sgn(memcmp(m_buf, ref, 64)) == 0);

    CK(memchr(m_buf, ref[20], 64) == m_buf + 20);
    CK(memchr(m_buf, 0, 64) == 0);
    return m;
}

static unsigned group_heap(void)
{
    unsigned m = 0, bit = 1;
    const bp_size big = 4u << 20;
    unsigned char *p, *q, *z;
    int i, ok;

    p = malloc(64);
    ok = p != 0;
    if (ok) {
        for (i = 0; i < 64; i++)
            p[i] = (unsigned char)(i ^ 0x5a);
        for (i = 0; i < 64; i++)
            if (p[i] != (unsigned char)(i ^ 0x5a))
                ok = 0;
    }
    CK(ok);
    free(p);

    q = malloc(big);
    ok = q != 0;
    if (ok) {
        q[0] = 0x11;
        q[big / 2] = 0x22;
        q[big - 1] = 0x33;
        memset(q + 4096, 0x7e, 4096);
        if (q[0] != 0x11 || q[big / 2] != 0x22 || q[big - 1] != 0x33)
            ok = 0;
        if (q[4096] != 0x7e || q[8191] != 0x7e)
            ok = 0;
    }
    CK(ok);
    free(q);

    z = calloc(128, 8);
    ok = z != 0;
    if (ok)
        for (i = 0; i < 1024; i++)
            if (z[i] != 0)
                ok = 0;
    CK(ok);
    free(z);

    p = malloc(32);
    ok = p != 0;
    if (ok) {
        for (i = 0; i < 32; i++)
            p[i] = (unsigned char)(i + 1);
        p = realloc(p, 8192);
        ok = p != 0;
        if (ok)
            for (i = 0; i < 32; i++)
                if (p[i] != (unsigned char)(i + 1))
                    ok = 0;
    }
    CK(ok);
    free(p);
    return m;
}

static unsigned group_env(void)
{
    unsigned m = 0, bit = 1;
    const char *v = getenv("OCERZ_BRIDGE_PROBE");

    CK(v != 0);
    CK(v != 0 && strlen(v) == 15);
    CK(v != 0 && sgn(strcmp(v, "bridge-probe-42")) == 0);
    CK(getenv("OCERZ_BRIDGE_PROBE_UNSET") == 0);
    return m;
}

static unsigned group_misc(void)
{
    unsigned m = 0, bit = 1;
    char rb[8];
    int *e;

    CK(abs(-7) == 7);
    CK(labs(-1234567890L) == 1234567890L);
    CK(atoi("-42") == -42);
    CK(atoi("  17xyz") == 17);
    CK(atol("1234567890") == 1234567890L);
    CK(getpid() > 0);
    CK(time(0) > 1600000000L);
    CK(clock() != (unsigned long)-1);
    CK(isatty(-1) == 0);
    CK(read(0, rb, sizeof rb) == 0);
    CK(close(-1) == -1);
    e = __error();
    CK(e != 0 && *e == 9);
    return m;
}

int main(void)
{
    line_status("str", group_str());
    line_status("mem", group_mem());
    line_status("heap", group_heap());
    line_status("env", group_env());
    line_status("misc", group_misc());
    putchar('i');
    putchar('o');
    puts(g_io_ok ? " ok" : " bad");
    return (g_bad || !g_io_ok) ? 1 : 0;
}
EOC

    cat > "$usrc" <<'EOC'
int printf(const char *, ...);
int main(void)
{
    printf("unimpl %d\n", 72);
    return 0;
}
EOC

    cat > "$bsrc" <<EOC
typedef __SIZE_TYPE__ bp_size;

bp_size strlen(const char *);
long write(int, const void *, bp_size);

static const char *volatile bp_bad = (const char *)${BAD_GUEST_ADDR}ull;
static volatile bp_size bp_len;

int main(void)
{
    write(1, "badptr enter\n", 13);
    bp_len = strlen(bp_bad);
    write(1, "badptr returned\n", 16);
    return bp_len != 0;
}
EOC

    cat > "$asrc" <<EOC
typedef __SIZE_TYPE__ bp_size;

bp_size strlen(const char *);
long write(int, const void *, bp_size);

static const char *volatile ap_good = "after";
static const unsigned char *volatile ap_bad = (const unsigned char *)${BAD_GUEST_ADDR}ull;
static volatile bp_size ap_len;
static volatile unsigned char ap_got;

int main(void)
{
    write(1, "after enter\n", 12);
    ap_len = strlen(ap_good);
    if (ap_len == 5)
        write(1, "after bridged\n", 14);
    ap_got = *ap_bad;
    write(1, "after returned\n", 15);
    return ap_got != 0;
}
EOC

    if clang -arch x86_64 -std=c11 -O1 -fno-builtin -fno-stack-protector \
            -o "$bin" "$src" >/dev/null 2>&1; then
        PROBE_BIN="$bin"
        printf 'str ok\nmem ok\nheap ok\nenv ok\nmisc ok\nio ok\n' > "$TMP/probe.want"
    fi
    if clang -arch x86_64 -std=c11 -O1 -fno-builtin -fno-stack-protector \
            -o "$ubin" "$usrc" >/dev/null 2>&1; then
        UNIMPL_BIN="$ubin"
    fi
    if clang -arch x86_64 -std=c11 -O1 -fno-builtin -fno-stack-protector \
            -o "$bbin" "$bsrc" >/dev/null 2>&1; then
        BADPTR_BIN="$bbin"
    fi
    if clang -arch x86_64 -std=c11 -O1 -fno-builtin -fno-stack-protector \
            -o "$abin" "$asrc" >/dev/null 2>&1; then
        AFTER_BIN="$abin"
    fi
}

build_callback_fixtures() {
    local name

    printf 'int main(void) { return 0; }\n' > "$TMP/cb_trivial.c"
    if clang -arch x86_64 -o "$TMP/cb_trivial" "$TMP/cb_trivial.c" >/dev/null 2>&1; then
        X86_CLANG=1
    fi

    cat > "$TMP/cb_common.h" <<'EOC'
typedef __SIZE_TYPE__ cb_size;
typedef __UINTPTR_TYPE__ cb_uptr;

long write(int, const void *, cb_size);
int puts(const char *);
int strcmp(const char *, const char *);
void qsort(void *, cb_size, cb_size, int (*)(const void *, const void *));
void *bsearch(const void *, const void *, cb_size, cb_size,
              int (*)(const void *, const void *));

#define CK(cond) do { if (!(cond)) m |= bit; bit <<= 1; } while (0)
#define CB_SKEWED() (((cb_uptr)__builtin_frame_address(0) & 15) != 0)

static char cb_buf[160];
static cb_size cb_len;

static void cb_str(const char *s)
{
    while (*s && cb_len < sizeof cb_buf - 1)
        cb_buf[cb_len++] = *s++;
}

static void cb_hex(unsigned v)
{
    const char *d = "0123456789abcdef";
    int i;
    for (i = 0; i < 8; i++) {
        char c[2] = { d[(v >> (28 - 4 * i)) & 15], 0 };
        cb_str(c);
    }
}

static void cb_dec(unsigned v)
{
    char t[11];
    int n = 10;
    t[10] = 0;
    do {
        t[--n] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    cb_str(t + n);
}

static void cb_begin(const char *name, unsigned mask)
{
    cb_len = 0;
    cb_str(name);
    if (mask == 0) {
        cb_str(" ok");
    } else {
        cb_str(" bad:");
        cb_hex(mask);
    }
}

static void cb_field(const char *key, unsigned v, int hex)
{
    cb_str(" ");
    cb_str(key);
    cb_str("=");
    if (hex)
        cb_hex(v);
    else
        cb_dec(v);
}

static void cb_end(void)
{
    cb_buf[cb_len++] = '\n';
    write(1, cb_buf, cb_len);
}
EOC

    cat > "$TMP/callback_qsort.c" <<'EOC'
#include "cb_common.h"

#define N 300

static int g_v[N];
static unsigned g_up, g_down, g_skew;

static int cmp_up(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;

    g_up++;
    if (CB_SKEWED())
        g_skew++;
    return x < y ? -65536 : (x > y ? 65536 : 0);
}

static int cmp_down(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;

    g_down++;
    if (CB_SKEWED())
        g_skew++;
    return x > y ? -65536 : (x < y ? 65536 : 0);
}

static int ascending(void)
{
    int i;
    for (i = 0; i < N; i++)
        if (g_v[i] != i - N / 2)
            return 0;
    return 1;
}

int main(void)
{
    unsigned m = 0, bit = 1, sum = 0;
    int i, ok;

    for (i = 0; i < N; i++)
        g_v[i] = (int)((unsigned)i * 7919u % N) - N / 2;

    qsort(g_v, N, sizeof g_v[0], cmp_up);
    CK(ascending());
    qsort(g_v, N, sizeof g_v[0], cmp_down);
    ok = 1;
    for (i = 0; i < N; i++)
        if (g_v[i] != N / 2 - 1 - i)
            ok = 0;
    CK(ok);
    qsort(g_v, N, sizeof g_v[0], cmp_up);
    CK(ascending());
    CK(g_up != 0 && g_down != 0);
    CK(g_skew == 0);

    for (i = 0; i < N; i++)
        sum = sum * 31u + (unsigned)g_v[i];
    cb_begin("qsort", m);
    cb_field("n", N, 0);
    cb_field("sum", sum, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/callback_nested_bridge.c" <<'EOC'
#include "cb_common.h"

static const char *const g_words[] = {
    "qsort", "strcmp", "bridge", "Bridge", "callback", "call", "", "zebra",
    "alpha", "alphabet", "alp", "~tilde", "0zero", "ocerz", "call", "guest",
    "x86", "arm64", "AArchX", "trampoline", "slot", "bank", "\xc3\xa9t\xc3\xa9",
    "comparator", "nested", "crossing", "frame", "Zulu", "zulu", "b", "a", "aa",
};
#define NW ((int)(sizeof g_words / sizeof g_words[0]))

static const char *g_sorted[NW];
static unsigned g_calls;

static int cmp_str(const void *a, const void *b)
{
    g_calls++;
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int own_cmp(const char *x, const char *y)
{
    while (*x && *x == *y) {
        x++;
        y++;
    }
    return (int)(unsigned char)*x - (int)(unsigned char)*y;
}

int main(void)
{
    unsigned m = 0, bit = 1;
    char seen[NW], line[512];
    cb_size n = 0;
    int i, j, ok;

    for (i = 0; i < NW; i++) {
        g_sorted[i] = g_words[i];
        seen[i] = 0;
    }
    qsort(g_sorted, NW, sizeof g_sorted[0], cmp_str);

    ok = 1;
    for (i = 1; i < NW; i++)
        if (own_cmp(g_sorted[i - 1], g_sorted[i]) > 0)
            ok = 0;
    CK(ok);
    ok = 1;
    for (i = 0; i < NW; i++) {
        for (j = 0; j < NW; j++)
            if (!seen[j] && g_words[j] == g_sorted[i])
                break;
        if (j == NW)
            ok = 0;
        else
            seen[j] = 1;
    }
    CK(ok);
    CK(g_calls != 0);

    cb_begin("nested", m);
    cb_field("words", (unsigned)NW, 0);
    cb_end();
    for (i = 0; i < NW; i++) {
        if (i)
            line[n++] = ',';
        for (j = 0; g_sorted[i][j]; j++)
            line[n++] = g_sorted[i][j];
    }
    line[n] = 0;
    puts(line);
    return m != 0;
}
EOC

    cat > "$TMP/callback_bsearch.c" <<'EOC'
#include "cb_common.h"

#define N 200

struct rec {
    int key;
    int val;
};

struct probe {
    char tag[4];
    int key;
};

static struct rec g_recs[N];
static struct probe g_probe;
static unsigned g_calls, g_bad_key, g_bad_elem, g_skew;

static int cmp_key(const void *k, const void *e)
{
    const struct probe *p = k;
    const struct rec *r = e;
    cb_uptr lo = (cb_uptr)g_recs, off = (cb_uptr)e - lo;

    g_calls++;
    if (p != &g_probe)
        g_bad_key++;
    if ((cb_uptr)e < lo || off >= sizeof g_recs || off % sizeof g_recs[0] != 0)
        g_bad_elem++;
    if (CB_SKEWED())
        g_skew++;
    return p->key < r->key ? -65536 : (p->key > r->key ? 65536 : 0);
}

static const struct rec *find(int key, cb_size n)
{
    g_probe.key = key;
    return bsearch(&g_probe, g_recs, n, sizeof g_recs[0], cmp_key);
}

int main(void)
{
    static const int kOutside[] = { -1000000, -251, 348, 1000000 };
    unsigned m = 0, bit = 1, found = 0, absent = 0;
    int i, ok;

    g_probe.tag[0] = 'k';
    g_probe.tag[1] = 'e';
    g_probe.tag[2] = 'y';
    for (i = 0; i < N; i++) {
        g_recs[i].key = 3 * i - 250;
        g_recs[i].val = i * 7 + 1;
    }

    ok = 1;
    for (i = 0; i < N; i++) {
        const struct rec *r = find(3 * i - 250, N);
        if (r == &g_recs[i] && r->val == i * 7 + 1)
            found++;
        else
            ok = 0;
    }
    CK(ok);

    ok = 1;
    for (i = 0; i < N; i++) {
        if (find(3 * i - 249, N) == 0)
            absent++;
        else
            ok = 0;
    }
    for (i = 0; i < 4; i++) {
        if (find(kOutside[i], N) == 0)
            absent++;
        else
            ok = 0;
    }
    CK(ok);
    CK(find(-250, 0) == 0);
    CK(g_calls != 0);
    CK(g_bad_key == 0);
    CK(g_bad_elem == 0);
    CK(g_skew == 0);

    cb_begin("bsearch", m);
    cb_field("found", found, 0);
    cb_field("absent", absent, 0);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/callback_recursion.c" <<'EOC'
#include "cb_common.h"

#define DEPTH 24

static int g_level;
static unsigned g_enter[DEPTH + 1], g_leave[DEPTH + 1];
static unsigned g_deepest, g_wrong_cmp, g_moved, g_unsorted, g_skew;
static unsigned g_trail = 17;

static int cmp_up(const void *a, const void *b);
static int cmp_down(const void *a, const void *b);

static void descend(int d)
{
    int v[3];
    int up = d & 1;

    v[0] = 3 * d + 2;
    v[1] = 3 * d;
    v[2] = 3 * d + 1;
    g_level = d;
    qsort(v, 3, sizeof v[0], up ? cmp_up : cmp_down);
    if (up ? !(v[0] < v[1] && v[1] < v[2]) : !(v[0] > v[1] && v[1] > v[2]))
        g_unsorted++;
    g_leave[d]++;
    g_trail = g_trail * 37u + (unsigned)d;
}

static int step(const void *a, const void *b, int up)
{
    int d = g_level;
    int x = *(const int *)a, y = *(const int *)b;

    if ((d & 1) != up)
        g_wrong_cmp++;
    if (CB_SKEWED())
        g_skew++;
    if (d >= 1 && d <= DEPTH && g_enter[d] == 0) {
        g_enter[d]++;
        g_trail = g_trail * 31u + (unsigned)d;
        if ((unsigned)d > g_deepest)
            g_deepest = (unsigned)d;
        if (d < DEPTH) {
            descend(d + 1);
            g_level = d;
            if (*(const int *)a != x || *(const int *)b != y)
                g_moved++;
        }
    }
    if (up)
        return x < y ? -65536 : (x > y ? 65536 : 0);
    return x > y ? -65536 : (x < y ? 65536 : 0);
}

static int cmp_up(const void *a, const void *b)
{
    return step(a, b, 1);
}

static int cmp_down(const void *a, const void *b)
{
    return step(a, b, 0);
}

int main(void)
{
    unsigned m = 0, bit = 1, want = 17;
    int d, ok;

    descend(1);

    ok = 1;
    for (d = 1; d <= DEPTH; d++)
        if (g_enter[d] != 1)
            ok = 0;
    CK(ok);
    ok = 1;
    for (d = 1; d <= DEPTH; d++)
        if (g_leave[d] != 1)
            ok = 0;
    CK(ok);
    CK(g_deepest == DEPTH);
    CK(g_wrong_cmp == 0);
    CK(g_moved == 0);
    CK(g_unsorted == 0);
    CK(g_skew == 0);
    for (d = 1; d <= DEPTH; d++)
        want = want * 31u + (unsigned)d;
    for (d = DEPTH; d >= 1; d--)
        want = want * 37u + (unsigned)d;
    CK(g_trail == want);

    cb_begin("recursion", m);
    cb_field("depth", g_deepest, 0);
    cb_field("trail", g_trail, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/callback_guest_fault.c" <<EOC
#include "cb_common.h"

static const int *volatile g_bad = (const int *)${BAD_GUEST_ADDR}ull;
static volatile int g_sink;
static int g_v[8] = { 5, 3, 7, 1, 8, 2, 6, 4 };

static int cmp_fault(const void *a, const void *b)
{
    g_sink = *g_bad;
    return *(const int *)a - *(const int *)b;
}

int main(void)
{
    write(1, "cbfault enter\n", 14);
    qsort(g_v, 8, sizeof g_v[0], cmp_fault);
    write(1, "cbfault returned\n", 17);
    return 0;
}
EOC

    for name in callback_qsort callback_nested_bridge callback_bsearch \
                callback_recursion callback_guest_fault; do
        clang -arch x86_64 -std=c11 -O1 -fno-builtin -fno-stack-protector \
                -o "$TMP/$name" "$TMP/$name.c" >"$TMP/$name.cc.log" 2>&1 || continue
        case $name in
            callback_qsort) CB_QSORT_BIN="$TMP/$name" ;;
            callback_nested_bridge) CB_NESTED_BIN="$TMP/$name" ;;
            callback_bsearch) CB_BSEARCH_BIN="$TMP/$name" ;;
            callback_recursion) CB_RECURSION_BIN="$TMP/$name" ;;
            callback_guest_fault) CB_FAULT_BIN="$TMP/$name" ;;
        esac
    done
}

run_probe() {
    local out="$1" err="$2"
    shift 2
    run_bounded "$out" "$err" env "$PROBE_VAR=$PROBE_VAL" "$OCERZ" "$@" "$PROBE_BIN"
}

probe_group() {
    local file="$1" group="$2"
    grep -E "^$group " "$file" 2>/dev/null | head -1
}

case_native_dyn() {
    local kern="$1" name="native_dyn_$1" reason="" rc_jit rc_nojit
    local jo="$TMP/native.$kern.jit.out" je="$TMP/native.$kern.jit.err"
    local no="$TMP/native.$kern.nojit.out" ne="$TMP/native.$kern.nojit.err"

    run_bounded "$jo" "$je" "$OCERZ" -v -native "$DYN" "$kern" "$SCALE"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$DYN" "$kern" "$SCALE"
    rc_nojit=$?

    reason="$(native_run_reason "$rc_jit" "$jo" "$je")"
    if [ -z "$reason" ]; then
        reason="$(native_run_reason "$rc_nojit" "$no" "$ne")"
        if [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif [ ! -s "$jo" ]; then
            reason="the guest ran but printed nothing"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit and no-jit stdout differ"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit out='$(head -1 "$jo")'"
}

case_cache_dyn() {
    local kern="$1" name="cache_dyn_$1" rc_jit rc_nojit reason=""
    local jo="$TMP/cache.$kern.jit.out" no="$TMP/cache.$kern.nojit.out"

    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_bounded "$jo" "$TMP/cache.$kern.jit.err" "$OCERZ" "$DYN" "$kern" "$SCALE"
    rc_jit=$?
    run_bounded "$no" "$TMP/cache.$kern.nojit.err" "$OCERZ" -no-jit "$DYN" "$kern" "$SCALE"
    rc_nojit=$?
    if [ "$rc_jit" -ne 0 ]; then
        reason="jit exit $rc_jit, want 0"
    elif [ "$rc_nojit" -ne 0 ]; then
        reason="no-jit exit $rc_nojit, want 0"
    elif [ ! -s "$jo" ]; then
        reason="no stdout"
    elif ! cmp -s "$jo" "$no"; then
        reason="jit and no-jit stdout differ"
    fi
    record "$name" "$reason" "out='$(head -1 "$jo")'"
}

case_native_matches_cache() {
    local kern="$1" name="native_matches_cache_$1" reason=""
    local nj="$TMP/native.$kern.jit.out" nn="$TMP/native.$kern.nojit.out"
    local cj="$TMP/cache.$kern.jit.out"

    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    if [ ! -s "$cj" ]; then
        reason="cache mode produced no stdout to compare against"
    elif [ ! -s "$nj" ]; then
        reason="native mode produced no stdout"
    elif ! cmp -s "$nj" "$cj"; then
        reason="native '$(head -1 "$nj")' != cache '$(head -1 "$cj")': a bridged call returned the wrong answer"
    elif ! cmp -s "$nn" "$cj"; then
        reason="native no-jit '$(head -1 "$nn")' != cache '$(head -1 "$cj")'"
    fi
    record "$name" "$reason" "out='$(head -1 "$cj")'"
}

case_native_bound() {
    local name=native_bound reason="" n
    n=$(grep -hF "$NOBIND" "$NOUT" "$NERR" 2>/dev/null | wc -l | tr -d ' ')
    if [ "$n" != "0" ]; then
        reason="$n imports did not bind against the virtual libSystem"
    elif grep -Fq "$M0_SUMMARY" "$NOUT" "$NERR"; then
        reason="the M0 unresolved-import summary is still printed"
    fi
    record "$name" "$reason" "unbound=$n"
}

case_bridge_probe_heap() {
    local name=bridge_probe_heap reason="" line
    if [ -z "$PROBE_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    line="$(probe_group "$TMP/probe_native.jit.out" heap)"
    if [ -z "$line" ]; then
        reason="the guest never printed a heap line"
    elif [ "$line" != "heap ok" ]; then
        reason="$line: the guest could not read back memory the host heap handed it, so the address model is not what the bridge assumes"
    fi
    record "$name" "$reason" "${line:-no line}"
}

case_bridge_probe_env() {
    local name=bridge_probe_env reason="" line
    if [ -z "$PROBE_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    line="$(probe_group "$TMP/probe_native.jit.out" env)"
    if [ -z "$line" ]; then
        reason="the guest never printed an env line"
    elif [ "$line" != "env ok" ]; then
        reason="$line: the guest could not read $PROBE_VAR through a pointer into the host's own environment, so the address model is not what the bridge assumes"
    fi
    record "$name" "$reason" "${line:-no line}"
}

case_bridge_probe_native() {
    local name=bridge_probe_native reason="" rc_jit rc_nojit
    local jo="$TMP/probe_native.jit.out" je="$TMP/probe_native.jit.err"
    local no="$TMP/probe_native.nojit.out" ne="$TMP/probe_native.nojit.err"

    if [ -z "$PROBE_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    rc_jit=$(cat "$TMP/probe_native.jit.rc")
    rc_nojit=$(cat "$TMP/probe_native.nojit.rc")

    reason="$(native_run_reason "$rc_jit" "$jo" "$je")"
    if [ -z "$reason" ]; then
        reason="$(native_run_reason "$rc_nojit" "$no" "$ne")"
        if [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$TMP/probe.want"; then
            reason="stdout is not the six all-ok lines: got '$(tr '\n' ' ' < "$jo")'"
        elif ! cmp -s "$no" "$TMP/probe.want"; then
            reason="no-jit stdout is not the six all-ok lines: got '$(tr '\n' ' ' < "$no")'"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit"
}

case_bridge_probe_cache() {
    local name=bridge_probe_cache reason="" rc
    local co="$TMP/probe_cache.out" ce="$TMP/probe_cache.err"
    local nj="$TMP/probe_native.jit.out"

    if [ -z "$PROBE_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_probe "$co" "$ce" -cache
    rc=$?
    if [ "$rc" -ne 0 ]; then
        reason="cache-mode exit $rc, want 0"
    elif ! cmp -s "$co" "$TMP/probe.want"; then
        reason="cache mode is not the six all-ok lines either: got '$(tr '\n' ' ' < "$co")'"
    elif ! cmp -s "$nj" "$co"; then
        reason="native '$(tr '\n' ' ' < "$nj")' != cache '$(tr '\n' ' ' < "$co")'"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_bridge_unimpl() {
    local name=bridge_unimpl rc reason="" sym imports
    local out="$TMP/bridge_unimpl.out" err="$TMP/bridge_unimpl.err"

    if [ -z "$UNIMPL_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -v -native "$UNIMPL_BIN"
    rc=$?
    sym=$(bridge_sym "$out" "$err")
    imports="$(nm -u "$UNIMPL_BIN" 2>/dev/null | awk '{print $1}' | tr '\n' ' ')"
    if [ -n "$imports" ] && [ "$imports" != "$UNIMPL_SYM " ]; then
        reason="the fixture imports '$imports' rather than $UNIMPL_SYM alone, so the symbol the bridge line names proves nothing"
    elif [ "$rc" -ne 72 ]; then
        reason="exit $rc, want 72"
    elif grep -Fq "$NOBIND" "$out" "$err"; then
        reason="an import went unresolved: $(grep -hF "$NOBIND" "$out" "$err" | head -1)"
    elif ! grep -qE "$BRIDGE_RE" "$out" "$err"; then
        reason="no 'ocerz: bridge: <dylib> <sym> not implemented' line"
    elif [ "$(grep -hE "$BRIDGE_RE" "$out" "$err" | head -1 | awk '{print $3}')" != "$LIB" ]; then
        reason="bridge named library $(grep -hE "$BRIDGE_RE" "$out" "$err" | head -1 | awk '{print $3}'), want $LIB"
    elif [ "$sym" != "$UNIMPL_SYM" ]; then
        reason="bridge named $sym, want $UNIMPL_SYM, the fixture's only import"
    fi
    record "$name" "$reason" "exit=$rc${sym:+ sym=$sym}"
}

bridge_stopped_reason() {
    if grep -Fq "$NOBIND" "$@" 2>/dev/null; then
        echo "an import went unresolved: $(grep -hF "$NOBIND" "$@" | head -1)"
    elif grep -qE "$BRIDGE_RE" "$@" 2>/dev/null; then
        echo "stopped at an unbridged export: $(grep -hE "$BRIDGE_RE" "$@" | head -1)"
    else
        echo ""
    fi
}

case_bridge_fault_native() {
    local name=bridge_fault_native rc reason=""
    local out="$TMP/badptr_native.out" err="$TMP/badptr_native.err"

    if [ -z "$BADPTR_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -v -native "$BADPTR_BIN"
    rc=$?
    reason="$(bridge_stopped_reason "$out" "$err")"
    if [ -n "$reason" ]; then
        :
    elif ! grep -Fq "$BADPTR_MARK" "$out"; then
        reason="the guest never reached the bad call"
    elif ! grep -qE "$BRIDGE_FAULT_RE" "$out" "$err"; then
        reason="no 'BRIDGE-FAULT ... inside a bridged call' report"
    elif ! grep -Fq "call=$LIB:$BADPTR_SYM" "$out" "$err"; then
        reason="the report does not name $LIB:$BADPTR_SYM"
    elif ! grep -Fq "sig='$BADPTR_SIG'" "$out" "$err"; then
        reason="the report does not name the signature $BADPTR_SIG"
    elif ! grep -qE 'host_fn=0x[0-9a-f]+ depth=[1-9]' "$out" "$err"; then
        reason="the report does not name the host function address and a crossing depth"
    elif ! grep -Fq "fault_addr=$BAD_GUEST_ADDR" "$out" "$err"; then
        reason="the report does not name $BAD_GUEST_ADDR as the faulting address"
    elif ! grep -qE 'host_pc=0x[0-9a-f]+' "$out" "$err"; then
        reason="the report does not name the host instruction pointer"
    elif ! grep -Fq "the guest passed a bad pointer to $BADPTR_SYM (guest addr $BAD_GUEST_ADDR)" "$out" "$err"; then
        reason="the report does not put $BAD_GUEST_ADDR in guest space and blame the guest's pointer"
    elif [ "$rc" -ne "$BRIDGE_FAULT_STATUS" ]; then
        reason="exit $rc, want $BRIDGE_FAULT_STATUS"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_bridge_fault_not_guest() {
    local name=bridge_fault_not_guest rc reason=""
    local out="$TMP/badptr_knobs.out" err="$TMP/badptr_knobs.err"
    local co="$TMP/after_knobs.out" ce="$TMP/after_knobs.err"

    if [ -z "$BADPTR_BIN" ] || [ -z "$AFTER_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    run_bounded "$out" "$err" env OCERZ_FAULTLOG=1 OCERZ_SIGTRACE=1 \
        "$OCERZ" -native "$BADPTR_BIN"
    rc=$?
    run_bounded "$co" "$ce" env OCERZ_FAULTLOG=1 OCERZ_SIGTRACE=1 \
        "$OCERZ" -native "$AFTER_BIN"
    reason="$(bridge_stopped_reason "$out" "$err")"
    if [ -n "$reason" ]; then
        :
    elif ! grep -qE "$BRIDGE_FAULT_RE" "$out" "$err"; then
        reason="the knobs changed the report away from a bridged-call fault"
    elif ! grep -Fq "$FAULTLOG_MARK" "$co" "$ce" || ! grep -Fq "$SIGTRACE_MARK" "$co" "$ce"; then
        reason="neither knob spoke for a plain guest fault at the same address, so their silence below proves nothing"
    elif grep -Fq "$GUEST_CRASH" "$out" "$err"; then
        reason="the fault is still reported as a guest crash at an unrelated rip: $(grep -hF "$GUEST_CRASH" "$out" "$err" | head -1 | cut -c1-120)"
    elif grep -Fq "$FAULTLOG_MARK" "$out" "$err"; then
        reason="OCERZ_FAULTLOG shows $BAD_GUEST_ADDR still went down the guest attribution path"
    elif grep -Fq "$SIGTRACE_MARK" "$out" "$err"; then
        reason="OCERZ_SIGTRACE shows the fault was still offered to the guest at $BAD_GUEST_ADDR"
    elif grep -qE "$WILD_RE" "$out" "$err"; then
        reason="a recovery path took the fault instead: $(grep -hE "$WILD_RE" "$out" "$err" | head -1 | cut -c1-120)"
    elif grep -Fq "$BADPTR_PAST" "$out"; then
        reason="the guest carried on past a fault taken inside a native frame"
    elif [ "$rc" -ne "$BRIDGE_FAULT_STATUS" ]; then
        reason="exit $rc, want $BRIDGE_FAULT_STATUS"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_bridge_fault_cache() {
    local name=bridge_fault_cache rc rc_nojit reason=""
    local out="$TMP/badptr_cache.out" err="$TMP/badptr_cache.err"
    local no="$TMP/badptr_cache.nojit.out" ne="$TMP/badptr_cache.nojit.err"

    if [ -z "$BADPTR_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -cache "$BADPTR_BIN"
    rc=$?
    run_bounded "$no" "$ne" "$OCERZ" -cache -no-jit "$BADPTR_BIN"
    rc_nojit=$?
    if grep -qE "$BRIDGE_FAULT_RE" "$out" "$err" "$no" "$ne"; then
        reason="cache mode printed a bridged-call report, and cache mode crosses no bridge"
    elif ! grep -Fq "$BADPTR_MARK" "$out"; then
        reason="the guest never reached the bad call"
    elif grep -Fq "$BADPTR_PAST" "$out"; then
        reason="the guest carried on past its own fault"
    elif ! grep -Fq "$GUEST_CRASH" "$out" "$err"; then
        reason="no guest-crash report: a plain guest fault stopped being reported as one"
    elif ! grep -Fq "guest_addr=$BAD_GUEST_ADDR" "$out" "$err"; then
        reason="the guest-crash report does not name $BAD_GUEST_ADDR as the address the guest dereferenced"
    elif [ "$rc" -ne "$GUEST_FAULT_STATUS" ]; then
        reason="exit $rc, want $GUEST_FAULT_STATUS"
    elif [ "$rc_nojit" -ne "$rc" ]; then
        reason="no-jit exit $rc_nojit, jit exit $rc: the two engines no longer agree on a guest fault"
    elif ! grep -Fq "$GUEST_CRASH" "$no" "$ne"; then
        reason="no-jit printed no guest-crash report"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_bridge_frame_lowered() {
    local name=bridge_frame_lowered rc reason=""
    local out="$TMP/after_native.out" err="$TMP/after_native.err"

    if [ -z "$AFTER_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -v -native "$AFTER_BIN"
    rc=$?
    reason="$(bridge_stopped_reason "$out" "$err")"
    if [ -n "$reason" ]; then
        :
    elif ! grep -Fq "$AFTER_MARK" "$out"; then
        reason="the bridged call did not return the answer the fixture expected"
    elif grep -Fq "$AFTER_PAST" "$out"; then
        reason="the guest carried on past its own fault"
    elif grep -qE "$BRIDGE_FAULT_RE" "$out" "$err"; then
        reason="a fault in guest code was blamed on a crossing that had already returned: $(grep -hE "$BRIDGE_FAULT_RE" "$out" "$err" | head -1)"
    elif ! grep -Fq "$GUEST_CRASH" "$out" "$err"; then
        reason="no guest-crash report for a fault the guest took in its own code"
    elif ! grep -Fq "guest_addr=$BAD_GUEST_ADDR" "$out" "$err"; then
        reason="the guest-crash report does not name $BAD_GUEST_ADDR"
    elif [ "$rc" -ne "$GUEST_FAULT_STATUS" ]; then
        reason="exit $rc, want $GUEST_FAULT_STATUS"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_bridgelog() {
    local name=bridgelog rc rc_plain reason="" n sym
    local lo="$TMP/bridgelog.out" le="$TMP/bridgelog.err"
    local po="$TMP/bridgelog_plain.out" pe="$TMP/bridgelog_plain.err"

    run_bounded "$po" "$pe" "$OCERZ" -v -native "$DYN" "$BRIDGELOG_KERNEL" "$SCALE"
    rc_plain=$?
    run_bounded "$lo" "$le" env OCERZ_BRIDGELOG=1 \
        "$OCERZ" -v -native "$DYN" "$BRIDGELOG_KERNEL" "$SCALE"
    rc=$?
    n=$(grep -cE "$BRIDGELOG_RE" "$le" 2>/dev/null | tr -d ' ')
    sym=$(grep -E "$BRIDGELOG_RE" "$le" 2>/dev/null | head -1 | awk '{print $4}')

    reason="$(native_run_reason "$rc_plain" "$po" "$pe")"
    if [ -n "$reason" ]; then
        reason="without OCERZ_BRIDGELOG: $reason"
    elif [ "$n" = "0" ]; then
        reason="OCERZ_BRIDGELOG printed no 'ocerz: BRIDGELOG[pid] <lib> <sym> <sig>' line"
    elif [ -n "$(grep -E "$BRIDGELOG_RE" "$le" | awk -v l="$LIB" '$3 != l' | head -1)" ]; then
        reason="a log line names library $(grep -E "$BRIDGELOG_RE" "$le" | awk -v l="$LIB" '$3 != l' | head -1 | awk '{print $3}'), want $LIB"
    elif [ -n "$(nm -u "$DYN" 2>/dev/null)" ] &&
         ! nm -u "$DYN" 2>/dev/null | awk '{print $1}' | grep -Fqx "$sym"; then
        reason="the log names $sym, which is not in the fixture's import table"
    elif [ -z "$(grep -E "$BRIDGELOG_RE" "$le" | awk -v s="$BRIDGELOG_SYM" -v g="$BRIDGELOG_SIG" '$4 == s && $5 == g' | head -1)" ]; then
        reason="no logged crossing is '$BRIDGELOG_SYM $BRIDGELOG_SIG', which the $BRIDGELOG_KERNEL kernel must make"
    elif [ "$rc" -ne "$rc_plain" ]; then
        reason="exit $rc with OCERZ_BRIDGELOG set, $rc_plain without"
    elif ! cmp -s "$lo" "$po"; then
        reason="stdout differs with OCERZ_BRIDGELOG set: the diagnostic changed what it was watching"
    fi
    record "$name" "$reason" "exit=$rc lines=$n"
}

callback_fixture_missing() {
    local name="$1" bin="$2"
    if [ -n "$bin" ]; then
        return 1
    fi
    if [ "$X86_CLANG" -ne 1 ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"
    else
        record "$name" "the fixture did not compile although x86_64 clang builds a trivial program: $( (grep -m1 -i 'error' "$TMP/$name.cc.log" || head -1 "$TMP/$name.cc.log") 2>/dev/null | cut -c1-160)"
    fi
    return 0
}

callback_run_reason() {
    local rc="$1" reason
    shift
    reason="$(native_run_reason "$rc" "$@")"
    if [ -z "$reason" ]; then
        echo ""
    elif grep -qE "$BRIDGE_FAULT_RE" "$@" 2>/dev/null; then
        echo "$reason; $(grep -hE "$BRIDGE_FAULT_RE" "$@" | head -1 | cut -c1-120)"
    elif grep -Fq "$GUEST_CRASH" "$@" 2>/dev/null; then
        echo "$reason; $(grep -hF "$GUEST_CRASH" "$@" | head -1 | cut -c1-120)"
    elif [ "$rc" -eq 124 ]; then
        echo "$reason: still running after ${NATIVE_TIMEOUT}s, so a callback never came back"
    else
        echo "$reason"
    fi
}

case_callback() {
    local name="$1" bin="$2" tag="$3" bits="$4" reason="" rc_jit rc_nojit rc_cache line
    local cache_note=""
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local co="$TMP/$name.cache.out" ce="$TMP/$name.cache.err"

    if callback_fixture_missing "$name" "$bin"; then
        return
    fi
    run_bounded "$jo" "$je" "$OCERZ" -v -native "$bin"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$bin"
    rc_nojit=$?
    line="$(head -1 "$jo")"

    reason="$(callback_run_reason "$rc_jit" "$jo" "$je")"
    if grep -q "^$tag bad:" "$jo"; then
        reason="'$line': the guest's own checks failed, where $bits"
    elif [ -z "$reason" ] && ! grep -q "^$tag ok" "$jo"; then
        reason="exit 0 without a '$tag ok' status line: got '${line:-nothing}'"
    fi

    if [ -z "$reason" ]; then
        reason="$(callback_run_reason "$rc_nojit" "$no" "$ne")"
        if grep -q "^$tag bad:" "$no"; then
            reason="no-jit: '$(head -1 "$no")': the guest's own checks failed, where $bits"
        elif [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit '$(tr '\n' ' ' < "$jo")' and no-jit '$(tr '\n' ' ' < "$no")' stdout differ"
        fi
    fi

    if [ -n "$reason" ]; then
        :
    elif [ "$CACHE_OK" -ne 1 ]; then
        cache_note=" cache=skipped"
    else
        run_bounded "$co" "$ce" "$OCERZ" -cache "$bin"
        rc_cache=$?
        if [ "$rc_cache" -ne 0 ]; then
            reason="cache-mode exit $rc_cache, want 0: '$(head -1 "$co")'"
        elif ! cmp -s "$jo" "$co"; then
            reason="native '$(tr '\n' ' ' < "$jo")' != cache '$(tr '\n' ' ' < "$co")': a callback crossing changed what the guest computed"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit out='$line'$cache_note"
}

callback_fault_reason() {
    local rc="$1" out="$2" err="$3" stopped
    stopped="$(bridge_stopped_reason "$out" "$err")"
    if [ -n "$stopped" ]; then
        echo "$stopped"
    elif ! grep -Fq "$CB_FAULT_MARK" "$out"; then
        echo "the guest never reached its qsort call"
    elif grep -qE "$BRIDGE_FAULT_RE" "$out" "$err"; then
        echo "the comparator's own read of $BAD_GUEST_ADDR was reported as a fault inside a bridged call ($(grep -hE "$BRIDGE_FAULT_RE" "$out" "$err" | head -1 | cut -c1-80)): M5 clears the thread's bridge frame while guest code runs precisely so that a fault in a callback is the guest's, and the frame was still raised when the comparator faulted"
    elif grep -Fq "$CB_FAULT_PAST" "$out"; then
        echo "qsort returned without the comparator faulting, so the callback never ran the guest's comparator"
    elif [ "$rc" -eq 124 ]; then
        echo "still running after ${NATIVE_TIMEOUT}s: the fault inside the callback hung instead of stopping the process"
    elif ! grep -Fq "$GUEST_CRASH" "$out" "$err"; then
        echo "no guest-crash report for a fault the guest took in its own comparator"
    elif ! grep -Fq "guest_addr=$BAD_GUEST_ADDR" "$out" "$err"; then
        echo "the guest-crash report does not name $BAD_GUEST_ADDR, the address the comparator read"
    elif [ "$rc" -ne "$GUEST_FAULT_STATUS" ]; then
        echo "exit $rc, want $GUEST_FAULT_STATUS"
    else
        echo ""
    fi
}

case_callback_guest_fault() {
    local name=callback_guest_fault reason="" rc_jit rc_nojit
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"

    if callback_fixture_missing "$name" "$CB_FAULT_BIN"; then
        return
    fi
    run_bounded "$jo" "$je" "$OCERZ" -v -native "$CB_FAULT_BIN"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$CB_FAULT_BIN"
    rc_nojit=$?
    reason="$(callback_fault_reason "$rc_jit" "$jo" "$je")"
    if [ -z "$reason" ]; then
        reason="$(callback_fault_reason "$rc_nojit" "$no" "$ne")"
        if [ -n "$reason" ]; then
            reason="no-jit: $reason"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit no-jit exit=$rc_nojit"
}

case_env_native() {
    local name=env_native rc reason="" out="$TMP/env_native.out" err="$TMP/env_native.err"
    run_bounded "$out" "$err" env OCERZ_MODE=native "$OCERZ" -v "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    reason="$(native_run_reason "$rc" "$out" "$err")"
    if [ -z "$reason" ] && ! cmp -s "$out" "$NOUT"; then
        reason="OCERZ_MODE=native stdout differs from the -native run"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_flag_beats_env() {
    local name=flag_beats_env rc reason="" out="$TMP/flag_beats_env.out" err="$TMP/flag_beats_env.err"
    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_bounded "$out" "$err" env OCERZ_MODE=native "$OCERZ" -v -cache "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        reason="exit $rc, want 0"
    elif ! cache_line_seen "$out" "$err"; then
        reason="-cache did not map the shared cache"
    elif [ -s "$CACHE_OUT" ] && ! cmp -s "$out" "$CACHE_OUT"; then
        reason="stdout differs from the plain cache-mode run"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_last_flag_native() {
    local name=last_flag_native rc reason="" out="$TMP/last_flag_native.out" err="$TMP/last_flag_native.err"
    run_bounded "$out" "$err" "$OCERZ" -v -cache -native "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    reason="$(native_run_reason "$rc" "$out" "$err")"
    if [ -z "$reason" ] && ! cmp -s "$out" "$NOUT"; then
        reason="'-cache -native' stdout differs from the -native run"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_last_flag_cache() {
    local name=last_flag_cache rc reason="" out="$TMP/last_flag_cache.out" err="$TMP/last_flag_cache.err"
    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -v -native -cache "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        reason="'-native -cache' exit $rc, want 0"
    elif ! cache_line_seen "$out" "$err"; then
        reason="'-native -cache' did not map the shared cache"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_bad_mode() {
    local name=bad_mode rc reason="" out="$TMP/bad_mode.out" err="$TMP/bad_mode.err"
    run_bounded "$out" "$err" env OCERZ_MODE=hybrid "$OCERZ" "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 64 ]; then
        reason="exit $rc, want 64"
    elif ! grep -q 'ocerz:' "$out" "$err"; then
        reason="refused without a named message"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_empty_mode() {
    local name=empty_mode rc reason="" out="$TMP/empty_mode.out" err="$TMP/empty_mode.err"
    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_bounded "$out" "$err" env OCERZ_MODE= "$OCERZ" -v "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        reason="exit $rc, want 0"
    elif ! cache_line_seen "$out" "$err"; then
        reason="an empty OCERZ_MODE did not fall back to cache mode"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_native_float() {
    local name=native_float rc reason="" src="$TMP/fp.c" bin="$TMP/fp"
    local out="$TMP/native_float.out" err="$TMP/native_float.err"
    local cout="$TMP/cache_float.out"
    cat > "$src" <<'EOC'
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void)
{
    double a = atof("3.5");
    double b = atof("-0.25");
    char *end = 0;
    double c = strtod("2.5e3xyz", &end);
    int ok = (a == 3.5) && (b == -0.25) && (c == 2500.0) && end && strcmp(end, "xyz") == 0;
    write(1, ok ? "fp ok\n" : "fp bad\n", ok ? 6 : 7);
    return ok ? 0 : 1;
}
EOC
    if ! clang -arch x86_64 -O1 -fno-stack-protector -o "$bin" "$src" >/dev/null 2>&1; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -native "$bin"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        reason="exit $rc, want 0; a double crossed the bridge wrongly"
    elif ! grep -q 'fp ok' "$out"; then
        reason="the guest computed the wrong value from a bridged double"
    elif [ "$CACHE_OK" -eq 1 ]; then
        run_bounded "$cout" "$TMP/cache_float.err" "$OCERZ" "$bin"
        cmp -s "$out" "$cout" || reason="native and cache disagree on a bridged double"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_native_unbound() {
    local name=native_unbound rc reason="" src="$TMP/unbound.c" bin="$TMP/unbound"
    local out="$TMP/native_unbound.out" err="$TMP/native_unbound.err"
    cat > "$src" <<'EOC'
#include <pwd.h>
int main(void) { return getpwnam("root") != 0; }
EOC
    if ! clang -arch x86_64 -fno-stack-protector -o "$bin" "$src" >/dev/null 2>&1; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -v -native "$bin"
    rc=$?
    if [ "$rc" -ne 71 ]; then
        reason="exit $rc, want 71"
    elif ! grep -Fq "${NOBIND}_getpwnam" "$out" "$err"; then
        reason="no 'no bridge for _getpwnam' line"
    elif ! grep -Fq "$M0_SUMMARY" "$out" "$err"; then
        reason="no unresolved-import summary line"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_native_static() {
    local name=native_static rc reason="" out="$TMP/native_static.out" err="$TMP/native_static.err"
    run_bounded "$out" "$err" "$OCERZ" -v -native "$STATIC"
    rc=$?
    if [ "$rc" -eq 71 ] || [ "$rc" -eq 72 ]; then
        reason="exit $rc: reached the loader instead of refusing a static image"
    elif [ "$rc" -ne 64 ]; then
        reason="exit $rc, want 64"
    elif ! grep -q 'ocerz:' "$out" "$err"; then
        reason="refused without a named message"
    fi
    record "$name" "$reason" "exit=$rc"
}

build_fixtures
build_callback_fixtures

if [ -n "$PROBE_BIN" ]; then
    run_probe "$TMP/probe_native.jit.out" "$TMP/probe_native.jit.err" -v -native
    echo $? > "$TMP/probe_native.jit.rc"
    run_probe "$TMP/probe_native.nojit.out" "$TMP/probe_native.nojit.err" -v -native -no-jit
    echo $? > "$TMP/probe_native.nojit.rc"
fi

for k in $KERNELS; do
    case_native_dyn "$k"
    case_cache_dyn "$k"
    case_native_matches_cache "$k"
done
case_native_bound
case_bridge_probe_heap
case_bridge_probe_env
case_bridge_probe_native
case_bridge_probe_cache
case_bridge_unimpl
case_bridge_fault_native
case_bridge_fault_not_guest
case_bridge_fault_cache
case_bridge_frame_lowered
case_bridgelog
case_callback callback_qsort "$CB_QSORT_BIN" qsort \
    "bit 0 is the first ascending sort, 1 the descending sort and 2 the second ascending sort coming out other than -150..149 in order, 3 a comparator that never ran, 4 a comparator entered on a misaligned stack"
case_callback callback_nested_bridge "$CB_NESTED_BIN" nested \
    "bit 0 is strings out of order after a comparator built on bridged strcmp, 1 a result that is not a permutation of the input, 2 a comparator that never ran"
case_callback callback_bsearch "$CB_BSEARCH_BIN" bsearch \
    "bit 0 is a present key not found at its own element, 1 an absent key found, 2 a search of zero elements finding something, 3 a comparator that never ran, 4 the key pointer arriving changed, 5 an element pointer off an element boundary, 6 a comparator entered on a misaligned stack"
case_callback callback_recursion "$CB_RECURSION_BIN" recursion \
    "bit 0 is a level not entered exactly once, 1 a level not left exactly once, 2 the nesting not reaching depth 24, 3 a level running the other comparator, 4 a caller's elements changing under a nested level, 5 a level's array coming back unsorted, 6 a comparator entered on a misaligned stack, 7 the levels entered or left out of order"
case_callback_guest_fault
case_env_native
case_flag_beats_env
case_last_flag_native
case_last_flag_cache
case_bad_mode
case_empty_mode
case_native_static
case_native_unbound
case_native_float

echo "----------------------------------------"
echo "native tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
