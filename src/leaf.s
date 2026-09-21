/*
 * String and memory routines that translated code calls with the guest's
 * registers left where they are.
 *
 * In native mode a guest call to strlen reaches a stub in a synthesized library
 * and crosses into the host's strlen.  The crossing costs far more than the
 * routine: every pinned guest register goes out to the cpu structure and comes
 * back, the arguments are read out of it again into an argument array, the
 * rounding mode and errno are swapped twice, and the block that made the call is
 * re-entered through its return check.  For a routine that reads a few dozen
 * bytes and returns a number, that is nine tenths of the time spent.  The
 * routines here do the same work under a contract that needs none of it, so
 * src/jit.c can call one from inside the stub's block with a single branch.
 *
 * ---- the contract ----
 * The first three guest arguments are read where full pinning keeps them: rdi in
 * x28, rsi in x27, rdx in x23.  The result is written to x21, which is rax.
 * Nothing else the translation relies on is touched: a routine changes x9 to x15,
 * v0 to v3 and the flags, and nothing more.  It makes no call, uses no stack and
 * never writes x30.  The guest base is zero wherever these are used, so a guest
 * pointer is the host address and is dereferenced as it arrives.
 *
 * Leaving x30 alone is what keeps a fault exact.  A guest that hands strlen a bad
 * pointer faults at a program counter inside this file, which belongs to no
 * translated block, but x30 still names the instruction after the branch that
 * came here, and every pinned register is as the guest left it.  src/vm.c turns a
 * program counter between ocerz_leaf_lo and ocerz_leaf_hi into that calling site
 * and recovers the guest state as if the stub's own instruction had faulted; the
 * same substitution lets a suspended thread report its registers.  Because rax is
 * written by the last instructions before the return, which touch no memory, a
 * fault never observes half a result.
 *
 * ---- reading sixteen bytes at a time ----
 * A scan for a terminator may not know where the string ends, so it may not read
 * past the page holding the last byte it is entitled to.  The scanning routines
 * read whole sixteen-byte blocks at addresses that are multiples of sixteen.
 * Such a block never spans a page of any size, and every block read holds at
 * least one byte the caller owns, so the bytes before the start of the string or
 * after its terminator that ride along come from a page already known to be
 * there.  The first block is read from the multiple of sixteen at or below the
 * pointer and the matches that lie before the pointer are shifted out.
 *
 * The two-string comparisons cannot align both pointers at once.  They read
 * unaligned blocks while both pointers are at least sixteen bytes short of a
 * 4096-byte boundary and compare byte by byte for one block's worth otherwise.
 * 4096 is the guest's page size and divides the host's, so the test is
 * conservative for both.  memcmp and bcmp know their length and read blocks only
 * while sixteen bytes remain.
 *
 * A comparison of sixteen bytes leaves 0xff or 0 in each byte of a vector.  shrn
 * by four packs that into a 64-bit value holding four bits per byte, which goes
 * to a general register, and the index of the first interesting byte is its
 * count of trailing zeros divided by four.
 *
 * ---- declining ----
 * Every routine answers in x9 as well as in x21: zero when the work is done,
 * and one when the routine declines, having changed nothing the guest can
 * tell, so that the caller does what it would have done without the routine.
 * In native mode that is the ordinary crossing.  In cache mode the routine
 * stands at the entry of Apple's own x86 function, and declining runs that
 * function's translation.  memmove is the one routine that declines by
 * itself, below.  The other source is src/vm.c: in cache mode a fault inside
 * a routine is not reported to the guest at all.  The handler writes one into
 * x9 and resumes at the instruction after the branch, and the fault then
 * happens again inside the translated x86 routine, where every consequence of
 * it, from the registers a handler sees to a resumed copy carrying on from
 * the byte it stopped at, is already what the guest expects.
 *
 * ---- the routines that write ----
 * memmove, which also answers for memcpy, and memset store into guest memory.
 * A store may land on a page whose code has been translated, which the fault
 * handler resolves by retiring those translations and letting the store run
 * again; the caller compares the retire count from before the call with the
 * one after and does not return through its shadow stack when they differ.
 *
 * memmove declines a move whose ranges overlap without being the same range.
 * The reason is what a fault means here.  A fault inside a routine leads, in
 * either mode, to the whole call being made again from its original arguments:
 * in native mode the guest sees a fault at the stub, and a handler that repairs
 * the page and returns re-enters the stub; in cache mode the x86 routine starts
 * from its entry.  Reading the
 * same bytes twice is harmless, and so is copying between ranges that do not
 * touch or filling a range with one byte, because doing part of the work and
 * then all of it leaves what doing it once leaves.  An overlapping move is the
 * one case where it does not: the part already moved has overwritten source
 * bytes the second run would read.  Up to sixty-four bytes every load is made
 * before any store; beyond that the copy runs forward in blocks of sixty-four
 * and finishes with the last sixty-four bytes of the source, which the ranges
 * being disjoint is what makes safe.  memset stores its byte the same way.
 * The translator sends lengths above 16384 to the host's routines, which earn
 * their crossing there.
 *
 * ---- the results ----
 * Each routine returns what the host's own would: strlen and strnlen a length,
 * strchr and memchr a pointer or zero, and the comparisons the difference of the
 * first two bytes that differ, as unsigned bytes, extended to 64 bits the way
 * src/abi.c extends an int on its way to rax.  bcmp shares memcmp's code, since
 * a caller of bcmp may only ask whether the result is zero.  memmove and memset
 * return their first argument.
 *
 * ---- ocerz_leaf_call ----
 * The last routine is for tests.  It is an ordinary C function that places three
 * values in x28, x27 and x23, calls a routine from this file, and returns what
 * the routine left in x21, saving and restoring the registers the C calling
 * convention says it must.  It also copies x9 to ocerz_leaf_declined.
 */

.text
.globl _ocerz_leaf_lo
.globl _ocerz_leaf_hi
.globl _ocerz_leaf_strlen
.globl _ocerz_leaf_strnlen
.globl _ocerz_leaf_strcmp
.globl _ocerz_leaf_strncmp
.globl _ocerz_leaf_memcmp
.globl _ocerz_leaf_strchr
.globl _ocerz_leaf_memchr
.globl _ocerz_leaf_memmove
.globl _ocerz_leaf_memset
.globl _ocerz_leaf_call
.globl _ocerz_leaf_declined

.p2align 2
_ocerz_leaf_lo:

.p2align 4
_ocerz_leaf_strlen:
    bic     x10, x28, #15
    ldr     q0, [x10]
    cmeq    v0.16b, v0.16b, #0
    shrn    v0.8b, v0.8h, #4
    fmov    x11, d0
    and     x12, x28, #15
    lsl     x12, x12, #2
    lsr     x11, x11, x12
    cbz     x11, 1f
    rbit    x11, x11
    clz     x11, x11
    lsr     x21, x11, #2
    mov     x9, #0
    ret
1:
    ldr     q0, [x10, #16]!
    cmeq    v0.16b, v0.16b, #0
    shrn    v0.8b, v0.8h, #4
    fmov    x11, d0
    cbz     x11, 1b
    rbit    x11, x11
    clz     x11, x11
    sub     x10, x10, x28
    add     x21, x10, x11, lsr #2
    mov     x9, #0
    ret

.p2align 4
_ocerz_leaf_strnlen:
    cbz     x27, 4f
    adds    x14, x28, x27
    csinv   x14, x14, xzr, cc
    bic     x10, x28, #15
    ldr     q0, [x10]
    cmeq    v0.16b, v0.16b, #0
    shrn    v0.8b, v0.8h, #4
    fmov    x11, d0
    and     x12, x28, #15
    lsl     x12, x12, #2
    lsr     x11, x11, x12
    lsl     x11, x11, x12
    cbnz    x11, 2f
1:
    add     x10, x10, #16
    cmp     x10, x14
    b.hs    3f
    ldr     q0, [x10]
    cmeq    v0.16b, v0.16b, #0
    shrn    v0.8b, v0.8h, #4
    fmov    x11, d0
    cbz     x11, 1b
2:
    rbit    x11, x11
    clz     x11, x11
    add     x10, x10, x11, lsr #2
    sub     x10, x10, x28
    cmp     x10, x27
    csel    x21, x10, x27, lo
    mov     x9, #0
    ret
3:
    mov     x21, x27
    mov     x9, #0
    ret
4:
    mov     x21, #0
    mov     x9, #0
    ret

.p2align 4
_ocerz_leaf_strcmp:
    mov     x10, x28
    mov     x11, x27
1:
    and     x12, x10, #4095
    and     x13, x11, #4095
    mov     x14, #4080
    cmp     x12, x14
    ccmp    x13, x14, #2, ls
    b.hi    3f
    ldr     q0, [x10]
    ldr     q1, [x11]
    cmeq    v2.16b, v0.16b, #0
    cmeq    v0.16b, v0.16b, v1.16b
    orn     v0.16b, v2.16b, v0.16b
    shrn    v0.8b, v0.8h, #4
    fmov    x12, d0
    cbnz    x12, 2f
    add     x10, x10, #16
    add     x11, x11, #16
    b       1b
2:
    rbit    x12, x12
    clz     x12, x12
    lsr     x12, x12, #2
    ldrb    w13, [x10, x12]
    ldrb    w14, [x11, x12]
    sub     w13, w13, w14
    sxtw    x21, w13
    mov     x9, #0
    ret
3:
    mov     x15, #16
4:
    ldrb    w13, [x10], #1
    ldrb    w14, [x11], #1
    cmp     w13, w14
    b.ne    5f
    cbz     w13, 5f
    subs    x15, x15, #1
    b.ne    4b
    b       1b
5:
    sub     w13, w13, w14
    sxtw    x21, w13
    mov     x9, #0
    ret

.p2align 4
_ocerz_leaf_strncmp:
    mov     x10, x28
    mov     x11, x27
    mov     x15, x23
1:
    cmp     x15, #16
    b.lo    3f
    and     x12, x10, #4095
    and     x13, x11, #4095
    mov     x14, #4080
    cmp     x12, x14
    ccmp    x13, x14, #2, ls
    b.hi    3f
    ldr     q0, [x10]
    ldr     q1, [x11]
    cmeq    v2.16b, v0.16b, #0
    cmeq    v0.16b, v0.16b, v1.16b
    orn     v0.16b, v2.16b, v0.16b
    shrn    v0.8b, v0.8h, #4
    fmov    x12, d0
    cbnz    x12, 2f
    add     x10, x10, #16
    add     x11, x11, #16
    sub     x15, x15, #16
    b       1b
2:
    rbit    x12, x12
    clz     x12, x12
    lsr     x12, x12, #2
    ldrb    w13, [x10, x12]
    ldrb    w14, [x11, x12]
    sub     w13, w13, w14
    sxtw    x21, w13
    mov     x9, #0
    ret
3:
    mov     x9, #16
4:
    cbz     x15, 6f
    ldrb    w13, [x10], #1
    ldrb    w14, [x11], #1
    cmp     w13, w14
    b.ne    5f
    cbz     w13, 5f
    sub     x15, x15, #1
    subs    x9, x9, #1
    b.ne    4b
    b       1b
5:
    sub     w13, w13, w14
    sxtw    x21, w13
    mov     x9, #0
    ret
6:
    mov     x21, #0
    mov     x9, #0
    ret

.p2align 4
_ocerz_leaf_memcmp:
    mov     x10, x28
    mov     x11, x27
    mov     x15, x23
    cmp     x15, #16
    b.lo    3f
1:
    ldr     q0, [x10]
    ldr     q1, [x11]
    cmeq    v0.16b, v0.16b, v1.16b
    shrn    v0.8b, v0.8h, #4
    fmov    x12, d0
    cmn     x12, #1
    b.ne    2f
    add     x10, x10, #16
    add     x11, x11, #16
    sub     x15, x15, #16
    cmp     x15, #16
    b.hs    1b
    b       3f
2:
    mvn     x12, x12
    rbit    x12, x12
    clz     x12, x12
    lsr     x12, x12, #2
    ldrb    w13, [x10, x12]
    ldrb    w14, [x11, x12]
    sub     w13, w13, w14
    sxtw    x21, w13
    mov     x9, #0
    ret
3:
    cbz     x15, 5f
    ldrb    w13, [x10], #1
    ldrb    w14, [x11], #1
    cmp     w13, w14
    b.ne    4f
    sub     x15, x15, #1
    b       3b
4:
    sub     w13, w13, w14
    sxtw    x21, w13
    mov     x9, #0
    ret
5:
    mov     x21, #0
    mov     x9, #0
    ret

.p2align 4
_ocerz_leaf_strchr:
    dup     v1.16b, w27
    bic     x10, x28, #15
    ldr     q0, [x10]
    cmeq    v2.16b, v0.16b, v1.16b
    cmeq    v0.16b, v0.16b, #0
    orr     v0.16b, v0.16b, v2.16b
    shrn    v0.8b, v0.8h, #4
    fmov    x11, d0
    and     x12, x28, #15
    lsl     x12, x12, #2
    lsr     x11, x11, x12
    lsl     x11, x11, x12
    cbnz    x11, 2f
1:
    ldr     q0, [x10, #16]!
    cmeq    v2.16b, v0.16b, v1.16b
    cmeq    v0.16b, v0.16b, #0
    orr     v0.16b, v0.16b, v2.16b
    shrn    v0.8b, v0.8h, #4
    fmov    x11, d0
    cbz     x11, 1b
2:
    rbit    x11, x11
    clz     x11, x11
    add     x10, x10, x11, lsr #2
    ldrb    w13, [x10]
    and     w14, w27, #255
    cmp     w13, w14
    csel    x21, x10, xzr, eq
    mov     x9, #0
    ret

.p2align 4
_ocerz_leaf_memchr:
    cbz     x23, 3f
    adds    x14, x28, x23
    csinv   x14, x14, xzr, cc
    dup     v1.16b, w27
    bic     x10, x28, #15
    ldr     q0, [x10]
    cmeq    v0.16b, v0.16b, v1.16b
    shrn    v0.8b, v0.8h, #4
    fmov    x11, d0
    and     x12, x28, #15
    lsl     x12, x12, #2
    lsr     x11, x11, x12
    lsl     x11, x11, x12
    cbnz    x11, 2f
1:
    add     x10, x10, #16
    cmp     x10, x14
    b.hs    3f
    ldr     q0, [x10]
    cmeq    v0.16b, v0.16b, v1.16b
    shrn    v0.8b, v0.8h, #4
    fmov    x11, d0
    cbz     x11, 1b
2:
    rbit    x11, x11
    clz     x11, x11
    add     x10, x10, x11, lsr #2
    cmp     x10, x14
    csel    x21, x10, xzr, lo
    mov     x9, #0
    ret
3:
    mov     x21, #0
    mov     x9, #0
    ret

.p2align 4
_ocerz_leaf_memmove:
    mov     x10, x28
    mov     x11, x27
    mov     x12, x23
    sub     x13, x10, x11
    sub     x14, x11, x10
    cbz     x13, 7f
    cmp     x13, x12
    ccmp    x14, x12, #0, hs
    b.lo    8f
    cmp     x12, #16
    b.hi    3f
    cmp     x12, #8
    b.lo    1f
    ldr     x13, [x11]
    add     x14, x11, x12
    ldur    x14, [x14, #-8]
    str     x13, [x10]
    add     x15, x10, x12
    stur    x14, [x15, #-8]
    b       7f
1:
    tbz     x12, #2, 2f
    ldr     w13, [x11]
    add     x14, x11, x12
    ldur    w14, [x14, #-4]
    str     w13, [x10]
    add     x15, x10, x12
    stur    w14, [x15, #-4]
    b       7f
2:
    cbz     x12, 7f
    lsr     x15, x12, #1
    ldrb    w13, [x11]
    ldrb    w14, [x11, x15]
    add     x9, x11, x12
    ldurb   w9, [x9, #-1]
    strb    w13, [x10]
    strb    w14, [x10, x15]
    add     x13, x10, x12
    sturb   w9, [x13, #-1]
    b       7f
3:
    cmp     x12, #32
    b.hi    4f
    ldr     q0, [x11]
    add     x13, x11, x12
    ldur    q1, [x13, #-16]
    str     q0, [x10]
    add     x14, x10, x12
    stur    q1, [x14, #-16]
    b       7f
4:
    cmp     x12, #64
    b.hi    5f
    ldp     q0, q1, [x11]
    add     x13, x11, x12
    ldp     q2, q3, [x13, #-32]
    stp     q0, q1, [x10]
    add     x14, x10, x12
    stp     q2, q3, [x14, #-32]
    b       7f
5:
    add     x13, x11, x12
    add     x14, x10, x12
    sub     x15, x14, #64
6:
    ldp     q0, q1, [x11]
    ldp     q2, q3, [x11, #32]
    add     x11, x11, #64
    stp     q0, q1, [x10]
    stp     q2, q3, [x10, #32]
    add     x10, x10, #64
    cmp     x10, x15
    b.lo    6b
    ldp     q0, q1, [x13, #-64]
    ldp     q2, q3, [x13, #-32]
    stp     q0, q1, [x14, #-64]
    stp     q2, q3, [x14, #-32]
7:
    mov     x21, x28
    mov     x9, #0
    ret
8:
    mov     x9, #1
    ret

.p2align 4
_ocerz_leaf_memset:
    mov     x10, x28
    mov     x12, x23
    dup     v0.16b, w27
    cmp     x12, #16
    b.hi    3f
    fmov    x13, d0
    cmp     x12, #8
    b.lo    1f
    str     x13, [x10]
    add     x14, x10, x12
    stur    x13, [x14, #-8]
    b       7f
1:
    tbz     x12, #2, 2f
    str     w13, [x10]
    add     x14, x10, x12
    stur    w13, [x14, #-4]
    b       7f
2:
    cbz     x12, 7f
    lsr     x15, x12, #1
    strb    w13, [x10]
    strb    w13, [x10, x15]
    add     x14, x10, x12
    sturb   w13, [x14, #-1]
    b       7f
3:
    cmp     x12, #32
    b.hi    4f
    str     q0, [x10]
    add     x14, x10, x12
    stur    q0, [x14, #-16]
    b       7f
4:
    cmp     x12, #64
    b.hi    5f
    stp     q0, q0, [x10]
    add     x14, x10, x12
    stp     q0, q0, [x14, #-32]
    b       7f
5:
    add     x14, x10, x12
    sub     x15, x14, #64
6:
    stp     q0, q0, [x10]
    stp     q0, q0, [x10, #32]
    add     x10, x10, #64
    cmp     x10, x15
    b.lo    6b
    stp     q0, q0, [x14, #-64]
    stp     q0, q0, [x14, #-32]
7:
    mov     x21, x28
    mov     x9, #0
    ret

.p2align 2
_ocerz_leaf_hi:

.p2align 4
_ocerz_leaf_call:
    stp     x29, x30, [sp, #-64]!
    mov     x29, sp
    stp     x21, x23, [sp, #16]
    stp     x27, x28, [sp, #32]
    mov     x16, x0
    mov     x28, x1
    mov     x27, x2
    mov     x23, x3
    blr     x16
    adrp    x1, _ocerz_leaf_declined@PAGE
    str     x9, [x1, _ocerz_leaf_declined@PAGEOFF]
    mov     x0, x21
    ldp     x21, x23, [sp, #16]
    ldp     x27, x28, [sp, #32]
    ldp     x29, x30, [sp], #64
    ret

.data
.p2align 3
_ocerz_leaf_declined:
    .quad   0
