/*
 * Moving a call from the x86-64 System V ABI to the arm64 AAPCS64 one.
 *
 * The bridge's first version read arguments straight out of RDI, RSI and the
 * rest, which is right only for functions whose arguments are all integers or
 * pointers.  As soon as a double appears the two ABIs stop agreeing: x86-64 puts
 * it in XMM0 while arm64 wants it in V0, and each side counts its integer and
 * its floating-point arguments in separate sequences, so one double in the
 * middle of a signature shifts nothing on one side and everything on the other.
 * Spilled arguments differ again, since the two disagree about which arguments
 * overflow to the stack and in what order.  So the mapping has to be computed
 * from a signature rather than assumed.
 *
 * A signature is written as a result class followed by its argument classes in
 * parentheses, for example i(pp) for strcmp, p(pLL) for memcpy, d(d) for sin and
 * v() for a function taking and returning nothing.  The classes are:
 *
 *     v  nothing, valid only as a result
 *     b  8-bit signed          B  8-bit unsigned
 *     h  16-bit signed         H  16-bit unsigned
 *     i  32-bit signed         u  32-bit unsigned
 *     l  64-bit signed         L  64-bit unsigned
 *     p  pointer, converted between the guest and host views
 *     f  32-bit float          d  64-bit double
 *     {...}  a structure by value, described below
 *
 * Reading the guest side means walking the arguments in order, handing each to
 * the next free integer register of RDI, RSI, RDX, RCX, R8, R9 or the next free
 * XMM of XMM0 to XMM7 according to its class, and taking anything past those
 * from the guest stack above the return address.  Writing the host side is the
 * same walk against x0 to x7 and v0 to v7, with its own overflow onto a real
 * stack the assembly caller builds.  Because the two walks consume their
 * registers independently, the nth argument rarely lands in the nth register on
 * either side, which is the whole reason this file exists.
 *
 * The 8- and 16-bit classes exist because CoreFoundation passes a Boolean, B,
 * and a UniChar, H, almost everywhere, and a narrow integer is where the two
 * ABIs disagree about who owns the upper bits.  System V leaves every bit of a
 * register or stack slot above the argument's own width undefined.  clang's
 * x86 caller extends a char or short to 32 bits all the same, with movsbl, and
 * the callee clang compiles relies on it, but a caller built by any other
 * compiler owes nothing.  Apple's arm64 makes the caller extend an argument
 * narrower than 32 bits to 32 and lets the callee trust it, and makes the
 * callee extend a narrow result to 32 and lets the caller trust that.  clang
 * bears both out: sxtb or and #0xff before a call, sxtw straight off w0 inside
 * the callee, sxtb w0, w0 before a return, and sxtw x0, w0 after the call with
 * no second extension, where its x86 caller does re-extend a result with
 * movsbq %al.  Neither side promises anything above bit 31 of an arm64 register
 * holding a narrow value.  So a narrow value is only ever read as its own 8 or
 * 16 bits, whether from a guest register, a guest stack slot, a native x
 * register, a native stack byte, or rax or x0 on the way back, and it is
 * extended to all 64 bits, by sign for b and h and by zero for B and H, before
 * it is written anywhere.  i and u are treated the same way at 32 bits.
 *
 * On the stack the two ABIs differ again.  System V gives every stacked argument
 * an eightbyte.  Apple packs a stacked argument at its own size and alignment,
 * which clang shows on both sides of a call.  Eight ints then char, short,
 * char, int are stored with strb [sp], strh [sp, #2], strb [sp, #4] and
 * str w [sp, #8], and read back with ldrsb, ldrsh, ldrsb and ldrsw at the same
 * offsets.  Eight ints then b, h, B, H, i land at 0, 2, 4, 6 and 8, read with
 * ldrsb, ldrsh, ldrb, ldrh and ldrsw.  Eight longs then b, i, B, l, h, L land at
 * 0, 4, 8, 16, 24 and 32.  Eight doubles, eight ints, then B, d, h, f, B land at
 * 0, 8, 16, 20 and 24.  The bytes between packed arguments are padding the
 * caller never writes.
 *
 * Floating-point values travel as raw bit patterns rather than as C doubles, so
 * that nothing is silently converted on the way through.  A float occupies the
 * low 32 bits of its slot, which is exactly where the arm64 s register that a
 * float argument is passed in lives inside its v register.
 *
 * The guest may have put the machine in a non-default rounding mode through
 * MXCSR, which the JIT honours by driving the host FPCR.  A host function
 * compiled for the default mode must not inherit that, so the crossing saves the
 * rounding mode, restores the default for the duration of the call, and puts the
 * guest's back afterwards.  ocerz_abi_round_swap is that save and restore: it
 * reads FPCR and writes it only when the rounding bits differ from the mode
 * asked for, returning what it read, so the common crossing, made in the
 * default mode by a callee that leaves the mode alone, never writes FPCR.
 * ocerz_abi_round_of_mxcsr gives the FPCR rounding bits for an MXCSR, which is
 * what a callback into guest code swaps in.
 *
 * Most crossings need none of the machinery below.  ocerz_abi_register_only
 * answers whether a signature is scalars only, with no more integer arguments
 * than System V's six registers and no more floating-point ones than its eight,
 * which also fit Apple's eight and eight; ocerz_abi_perform_registers makes such
 * a crossing straight from the guest's registers into the host's, with no stack
 * block, and ocerz_abi_perform takes that path itself whenever it applies.
 * ocerz_abi_xmm_contract names, as bit masks, the xmm registers a signature's
 * arguments are read from and the ones its result is written to, counting a
 * structure argument as all eight argument registers and a structure result as
 * xmm0 and xmm1, which is what lets the JIT spill no others.
 *
 * ---- structures by value ----
 * Objective-C passes and returns structures by value all the time: -frame
 * returns a CGRect, -rangeOfString: an NSRange, and CGPoint, CGSize,
 * NSEdgeInsets and CGAffineTransform travel the same way.  A structure is
 * written in braces holding its members' classes in order, a member being one
 * of the scalar classes b B h H i u l L p f d or another structure, so CGPoint
 * is {dd}, CGRect {{dd}{dd}}, NSRange {LL}, CGAffineTransform {dddddd}, and
 * -[NSView convertRect:toView:] is {{dd}{dd}}(pp{{dd}{dd}}p).  A structure may
 * be a result or an argument, including inside a callback's signature.  v, c
 * and an empty pair of braces are refused inside one, as is a structure with
 * more than sixteen scalar members once its nesting is flattened, which is
 * CATransform3D's count, or one nested more than eight deep.  A union has no
 * notation.  The layout is the natural C one: each member at the next multiple
 * of its alignment, which for every scalar class is its size, a nested
 * structure aligned to its largest member, and the whole rounded up to its own
 * alignment.  {bd} is sixteen bytes with d at 8, {bhb} six with h at 2 and the
 * last b at 4, {{db}b} twenty-four with the last b at 16, and no two members
 * ever share bytes or straddle an eightbyte boundary.
 *
 * Parsed, a structure's class is '{', in OcerzAbiSig.ret or arg[i], and its
 * layout is beside it in ret_struct or arg_struct[i]: size, alignment, and the
 * flattened scalar members, each with its class and its offset from the start
 * of the outermost structure.  Nesting survives only in those offsets, and the
 * offsets are all either ABI looks at.  A signature built by hand rather than
 * parsed is checked before any call is made from it, and a '{' whose layout
 * has no members, members that overlap, sit off their alignment or run past
 * its end, or an eightbyte of a structure of sixteen bytes or less that no
 * member starts in, is refused.
 *
 * A structure crosses as bytes.  It is gathered from wherever one ABI put it
 * into a buffer laid out as the structure, a p member is converted between the
 * guest and host views in place, and the buffer is scattered to wherever the
 * other ABI wants it.  Neither ABI extends a narrow member: clang's arm64 caller
 * passes {h} holding -3 as w0 = 0xfffd and the callee does its own sxth, and
 * the x86 callee of {B} reads %dil with movzbl.  So a member is never extended,
 * and the bytes of a register word or stack eightbyte past the structure's end
 * are written as zero on either side, never passed on.
 *
 * On the x86-64 side a structure of more than sixteen bytes is class MEMORY,
 * as one with unaligned fields would be, which the notation cannot describe.
 * As an argument it is copied onto the stack, its eightbytes taking their place
 * in order among the other stacked arguments; as a result the caller passes a
 * pointer to space for it in rdi, ahead of every integer argument, and the
 * callee hands the same pointer back in rax.  A smaller structure is one or two
 * eightbytes, each INTEGER if any member starting in it is an integer or a
 * pointer and SSE otherwise.  If the free integer and SSE registers do not
 * cover all of its eightbytes, the whole structure goes on the stack and the
 * registers stay free for the arguments after it.  A result puts its INTEGER
 * eightbytes in rax then rdx and its SSE ones in xmm0 then xmm1, in order, so
 * {dL} comes back in xmm0 and rax and {Ld} in rax and xmm0.  clang -arch x86_64
 * shows all of it.  {B} is edi = 0x81 and {h} edi = 0xfffd.  {LL} is rdi and
 * rsi, returned in rax and rdx.  {ff} is both floats in the low eight bytes of
 * xmm0, read back with movshdup; {fff} is two in xmm0 and the third in xmm1;
 * {fi} is one INTEGER eightbyte, rdi = 0x93fc00000 for {1.5f, 9}.  {df} is
 * xmm0 and xmm1.  {fffff}, {ddd}, {dddd}, {ddddd}, {{dd}{dd}}, {LLL} and {pdi}
 * are MEMORY: {LLL}(LLLLLL) takes rdi as its result pointer, the first five
 * longs in rsi to r9 and the sixth at stack 0; LLLLLLL{LLL}L puts the seventh
 * long at 0, the structure at 8 to 31 and the last long at 32.  LLLLL{LL}L
 * stacks {LL} at 0 and 8 and still passes the last long in r9.
 * ddddddd{dd}d stacks {dd} and passes the last double in xmm7, and six doubles
 * then {fff} then f put {fff} in xmm6 and xmm7 and f at 0.  {dL} after five
 * longs is xmm0 and r9 with the following long at 0, and after six longs is
 * stacked at 0 and 8 with a following double in xmm0.  Eight longs then {B},
 * {h}, {i}, {B}, a char, {bhb}, {fi} and {BBB} put the last two longs at 0 and 8
 * and the rest at 16, 24, 32, 40, 48, 56, 64 and 72, one eightbyte each.
 *
 * On Apple's arm64 side a homogeneous floating-point aggregate, a structure
 * whose flattened members are one to four and all f or all d, goes in
 * consecutive v registers, one member each, if that many are free, and
 * otherwise on the stack, after which no argument uses a v register.  Any
 * other structure of more than sixteen bytes is copied by the caller into
 * memory the caller owns, and a pointer to the copy is passed as an integer
 * argument.  Any other structure of sixteen bytes or less takes one or two x
 * registers if that many are free, and otherwise goes on the stack, after which
 * no argument uses an x register.  A result that is such an aggregate comes
 * back in d0 to d3 or s0 to s3, one of sixteen bytes or less in x0 and x1, and
 * a larger one through a buffer the caller passes in x8; the callee need not
 * return the buffer's address.  On the stack a small non-aggregate structure is
 * a block of eight or sixteen bytes aligned to 8 whatever its own size and
 * alignment, the caller writing whole words, while an aggregate is its members
 * at their own size, aligned to its member's alignment.  clang -arch arm64
 * shows all of it.  {ff}, {fff} and {ffff} are s0 to s3, {ddd}, {dddd} and
 * {{dd}{dd}} d0 to d3, both as arguments and as results.  {fffff}, {ddddd},
 * {LLL} and {pdi} are copied to the caller's frame with the address in x0,
 * which the callee reads through, and are returned through x8.  {df} is not an
 * aggregate and is x0 holding the double's bits and w1 the float's, returned
 * the same way.  Seven longs then {LL} then a long stack {LL} at 0 and 8 and the
 * long at 16, leaving x7 unused; seven longs then {iL} then an int put the int
 * at 16.  Seven doubles then {dd} then a double stack {dd} at 0 and 8 and the
 * double at 16, leaving d7 unused; six doubles then {fff} then a float put
 * {fff} at 0, 4 and 8 and the float at 12.  Seven longs then {LLL} then a long
 * pass the pointer in x7 and the long at 0; eight longs, a char, {LLL} and a
 * char put the char at 0, the pointer at 8 and the char at 16.  Eight longs then
 * {B}, {h}, {i}, {B}, a char, {bhb}, {fi} and {BBB} put them at 0, 8, 16, 24,
 * 32, 40, 48 and 56, {bhb} after the char being aligned to 8 and not to its
 * own 2.  Eight doubles then {ff}, {f}, {dd}, a float, {fff}, a char and {d} put
 * them at 0, 8, 16, 32, 36, w0 and 48.  Eight longs then an int, {Li}, a char,
 * {fff}, a char and {df} put the int at 0, {Li} at 8, the chars at 24 and 25,
 * {fff} in s0 to s2 and {df} at 32.  Eight longs, eight doubles, then a char,
 * {ff}, a char, {dd} and a float put them at 0, 4, 12, 16 and 32.
 *
 * The two classifications disagree often enough that nothing may be assumed.
 * {ddd}, {dddd} and {{dd}{dd}} are MEMORY on x86 and three or four registers on
 * arm64; {ff} is one xmm on x86 and two v registers on arm64; {df} is two xmm on
 * x86 and two x registers on arm64; {fffff} is MEMORY on x86 and a pointer to a
 * copy on arm64; and a spilled structure leaves the registers free for later
 * arguments on x86 while on arm64 it closes them.  A copy made for an arm64
 * callee lives in the OcerzAbiCall that ocerz_abi_read_guest fills, whose x
 * words then point into it, so the call must be made from that same
 * OcerzAbiCall without moving it.  A native result returned through x8 lands in
 * the OcerzAbiCall too and is copied to the guest's result pointer afterwards.
 *
 * ---- calls in the other direction ----
 * Some native functions take a function pointer and call it: qsort calls its
 * comparator, a run loop calls its observer.  When the guest supplies one, the
 * pointer is x86 code, and native code cannot jump to it.  So an argument of
 * class c is not converted like a pointer.  It carries its own signature in
 * braces, as in v(pLLc{i(pp)}) for qsort, and it is interned: the guest function
 * and that signature are bound to one slot of a fixed bank of 65536 arm64
 * trampolines, and the slot's address is what the native callee receives.
 * Interning the same function with the same signature twice returns the same
 * address, so a native library that compares callback pointers still sees one
 * function.  A null pointer stays null.  A nested signature may not itself name
 * a callback, and is at most 47 characters, which leaves room for structures on
 * both sides: {{dd}{dd}}(pp{{dd}{dd}}p) is already 25.  A signature interned
 * directly, as an Objective-C method's is, may be up to 255 characters, and
 * ocerz_abi_callback_sig answers the parsed signature and guest function a
 * slot's address is bound to, the same signature for every slot of one
 * notation.
 *
 * Not every function pointer a guest passes is guest code.  In native mode an
 * exported variable such as kCFTypeArrayCallBacks is CoreFoundation's own, so a
 * guest that copies it copies native retain and release functions, and a
 * pointer read out of such a structure may come straight back where a callback
 * is expected.  Interning one of those would have the translator decode arm64
 * instructions as x86.  So every c argument goes through
 * ocerz_abi_callback_convert, which interns only what ocerz_abi_is_guest_code
 * calls guest code and hands anything else back unchanged.  That test goes by
 * address.  Inside the guest reservation is guest code.  Inside the host shared
 * cache is not.  Anywhere else is guest code exactly when host dyld does not
 * know the address, because every image the guest runs is mapped by ocerz's
 * own loader and never registered with dyld.  ocerz's own binary is a host
 * image under that rule, which is why a bank slot handed back to a callback
 * parameter reaches native code as itself and not as a trampoline to a
 * trampoline.  The converter's answer is guest-visible, a trampoline being
 * given through ocerz_h2g, so the bridge can write it into a guest structure
 * and a crossing can hand it to native code through ocerz_g2h.
 *
 * The bank is ordinary code assembled into ocerz, not code generated at run time.
 * Generating a trampoline from inside a native callback would race the
 * translator for the one writable JIT arena, a fork child abandons that arena
 * while native code may still hold pointers into it, and a real-time audio
 * callback cannot wait for code generation at all.  Each slot is two
 * instructions and finds its own index from its own address, so the bank is one
 * repeated block and exhausting it is a named refusal, never a silent reuse.
 *
 * When native code calls a slot, the dispatcher reads that signature against the
 * native caller's x0..x7, v0..v7, x8 and stacked arguments, packed the way
 * Apple's arm64 packs them, places the values where the System V ABI puts them
 * for the guest, and runs the guest function on the calling thread below its
 * own stack pointer, past the red zone.  A structure argument is read from x
 * registers, v registers, stack bytes or the copy an x word points at, and
 * placed in guest registers or guest stack eightbytes; a MEMORY one is copied
 * into the guest's stacked arguments.  A guest function returning a MEMORY
 * structure is given space for it just below the dispatcher's stack top, the
 * guest stack region the call runs on, and a pointer to that space in rdi.  The
 * guest runs with its own rounding mode, not the default the enclosing native
 * call was given.  While it runs, the thread is executing guest code again, so
 * it must not count as inside a bridged call: a guest fault there is an
 * ordinary guest fault with an ordinary recovery, and reporting it as a
 * native-code fault would kill a process that was fine.  The result is
 * converted back and returned in x0 or v0, a structure result in x0 and x1, in
 * d0 to d3, or copied into the buffer the native caller passed in x8.
 *
 * A callback can also arrive on a thread with no guest personality at all, one
 * a native framework created for itself, such as a libdispatch worker.  The
 * dispatcher gives that thread one through ocerz_thread_attach on its first
 * callback and reuses it on every one after, and refuses by name only when
 * there is no guest process to attach it to.
 *
 * ---- variadic calls ----
 * A signature has nowhere to say where a function's named arguments stop, and
 * the two ABIs part company exactly there.  System V does not: a variadic
 * argument is wherever a fixed one of its class would be, the next integer
 * register or the next xmm and then the next stack eightbyte, so a variadic
 * callee's arguments are found by continuing the walk over the named ones.
 * Apple's arm64 does: the named arguments go where a fixed callee's would, and
 * every variadic one after them goes on the stack in an eight-byte slot of its
 * own, in order, a double included, with no floating-point register used.  An
 * arm64 va_list is nothing but a pointer to such slots.  clang -arch arm64 shows
 * all of it: sink("x", 1, 2.5, 3, 4.5) puts "x" in x0 and the rest at sp, sp+8,
 * sp+16 and sp+24, where a fixed-arity callee takes x0, x1, x2, d0 and d1.
 *
 * So what the engine offers a variadic crossing is the guest half as a cursor,
 * and the caller, which alone knows from a format string or a sentinel what the
 * variadic arguments are, builds the slots.  ocerz_abi_va_start walks the named
 * signature the way ocerz_abi_read_guest does, a MEMORY result's pointer and
 * each structure's eightbytes included, and leaves the cursor at the first
 * variadic argument.  ocerz_abi_va_arg takes the next one of class i, u, l, L,
 * p or d, which are all the default argument promotions leave, and gives it as
 * a whole slot: i and u extended from their low 32 bits, p converted to the host
 * view with null kept null, d as its raw bits.  A variadic argument is never a
 * float, a narrow integer or a structure, so any other class is refused.  The
 * slots then go to a v-form function as its va_list, or, for a variadic callee
 * called directly, after the named arguments' own stacked bytes, which end on an
 * eight-byte boundary since nstack counts whole eightbytes;
 * ocerz_abi_call_native copies a stack block of any length.
 *
 * ocerz_abi_postfork_child puts the interning lock back to its initial state in
 * a fork child.  Interned slots are complete before their address escapes, so
 * a slot another thread was halfway through interning at the fork is one the
 * child never saw, and the child interns it again if it needs it.
 */
#ifndef OCERZ_ABI_H
#define OCERZ_ABI_H

#include "ocerz/types.h"
#include "ocerz/cpu.h"

#include <arm_acle.h>

#define OCERZ_ABI_MAX_ARGS 16
#define OCERZ_ABI_MAX_STACK 64
#define OCERZ_ABI_CB_MAX 48
#define OCERZ_ABI_CALLBACK_SLOTS 65536
#define OCERZ_ABI_CALLBACK_NOTATION_MAX 256
#define OCERZ_ABI_CALLBACK_STRIDE 8
#define OCERZ_ABI_STRUCT_MEMBERS 16
#define OCERZ_ABI_STRUCT_BYTES 256

typedef struct OcerzAbiStruct {
    uint16_t size;
    uint8_t align;
    uint8_t nmember;
    char member[OCERZ_ABI_STRUCT_MEMBERS];
    uint8_t offset[OCERZ_ABI_STRUCT_MEMBERS];
} OcerzAbiStruct;

typedef struct OcerzAbiSig {
    char ret;
    char arg[OCERZ_ABI_MAX_ARGS];
    char cb[OCERZ_ABI_MAX_ARGS][OCERZ_ABI_CB_MAX];
    int nargs;
    OcerzAbiStruct ret_struct;
    OcerzAbiStruct arg_struct[OCERZ_ABI_MAX_ARGS];
} OcerzAbiSig;

typedef struct OcerzAbiCall {
    uint64_t x[8];
    uint64_t v[8];
    uint64_t stack[OCERZ_ABI_MAX_STACK];
    int nx;
    int nv;
    int nstack;
    int nmem;
    void *x8;
    uint64_t guest_ret;
    uint64_t rx[2];
    uint64_t rv[4];
    uint64_t ret[OCERZ_ABI_STRUCT_BYTES / 8];
    uint64_t mem[OCERZ_ABI_MAX_ARGS * OCERZ_ABI_STRUCT_BYTES / 8];
} OcerzAbiCall;

typedef struct OcerzAbiVaList {
    int gi;
    int gf;
    int gslot;
} OcerzAbiVaList;

int ocerz_abi_parse(const char *notation, OcerzAbiSig *out);

int ocerz_abi_read_guest(const OcerzAbiSig *sig, const OcerzCPU *cpu,
                         OcerzAbiCall *call);

int ocerz_abi_va_start(const OcerzAbiSig *named, const OcerzCPU *cpu, OcerzAbiVaList *va);
int ocerz_abi_va_arg(OcerzAbiVaList *va, const OcerzCPU *cpu, char cls, uint64_t *out);

void ocerz_abi_write_result(const OcerzAbiSig *sig, OcerzCPU *cpu,
                            const OcerzAbiCall *call);

int ocerz_abi_perform(const OcerzAbiSig *sig, const void *fn, OcerzCPU *cpu);

int ocerz_abi_register_only(const OcerzAbiSig *sig);
int ocerz_abi_perform_registers(const OcerzAbiSig *sig, const void *fn, OcerzCPU *cpu);
void ocerz_abi_xmm_contract(const OcerzAbiSig *sig, uint16_t *in, uint16_t *out);

#define OCERZ_ABI_ROUND_MASK 0xc00000ull
#define OCERZ_ABI_ROUND_NEAREST 0ull

static inline uint64_t ocerz_abi_round_swap(uint64_t mode)
{
    uint64_t fpcr = __arm_rsr64("fpcr");
    if ((fpcr & OCERZ_ABI_ROUND_MASK) != mode)
        __arm_wsr64("fpcr", (fpcr & ~OCERZ_ABI_ROUND_MASK) | mode);
    return fpcr;
}

static inline uint64_t ocerz_abi_round_of_mxcsr(uint32_t mxcsr)
{
    static const uint64_t mode[4] = { 0ull, 0x800000ull, 0x400000ull, 0xc00000ull };
    return mode[(mxcsr >> 13) & 3];
}

void ocerz_abi_call_native(const void *fn, const uint64_t *x, const uint64_t *v,
                           const uint64_t *stack, uint64_t stackbytes, void *x8,
                           uint64_t *out_x, uint64_t *out_v);

void *ocerz_abi_callback_intern(uint64_t guest_fn, const char *notation);
const OcerzAbiSig *ocerz_abi_callback_sig(const void *slot_address, uint64_t *guest_fn);

int ocerz_abi_is_guest_code(uint64_t gptr);

int ocerz_abi_callback_convert(uint64_t gptr, const char *notation, uint64_t *out);

void ocerz_abi_callback_dispatch(unsigned slot, const uint64_t *x, const uint64_t *v,
                                 const uint8_t *stack, void *x8, uint64_t *out_x,
                                 uint64_t *out_v);

void ocerz_abi_postfork_child(void);

extern const char ocerz_abi_callback_bank[];
extern const char ocerz_abi_callback_bank_end[];

#endif
