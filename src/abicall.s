/*
 * Making an arm64 call whose argument registers arrive as data.
 *
 * This is the last step of a bridged call.  src/abi.c has already decided, from
 * the signature, which host register every guest argument belongs in, and hands
 * the answer over as two arrays of eight words plus a block of bytes for
 * whatever overflowed.  All that is left is to put those arrays into the real
 * registers and branch, which no C prototype can express and so lives here.
 *
 * ---- why the incoming arguments move before the outgoing ones load ----
 * The registers to be loaded are the registers the call arrives in.  fn is in
 * x0, the two arrays in x1 and x2, the overflow block in x3 and x4, the two
 * result slots in x5 and x6.  Loading x0 from the array destroys fn; loading x1
 * destroys the array still being read; loading x5 and x6 destroys where the
 * result has to go.  So everything still needed is relocated into x19 to x23,
 * which a callee must preserve, before a single outgoing register is touched,
 * and the eight pairs then load in one run with nothing live in x0 to x7.  Done
 * by halves instead - load some arguments, then reach for another incoming
 * pointer - the call is correct for the arities whose registers happen not to
 * overlap and quietly wrong for the rest, which is the failure this shape
 * exists to rule out rather than to test for.
 *
 * ---- the stack ----
 * Overflow arguments sit at sp and upwards as the callee is entered, so the
 * block is copied to the low end of a freshly cut area and sp points at its
 * first byte.  sp must be a multiple of 16 throughout, not merely at the call,
 * because an access based on a misaligned sp faults on this platform; the area
 * is therefore rounded up to 16 before sp moves, which also covers a zero-byte
 * block, and sp comes back from the frame pointer rather than by unwinding the
 * same arithmetic.  The size is a byte count and is copied as one, tail
 * included: Apple gives a stacked argument only as much room as its type needs
 * rather than the eight bytes the generic AArch64 standard reserves, so a lone
 * stacked float or int is four bytes and the block need not be a multiple of
 * eight.  That packing rule is for an ordinary callee only.  A variadic callee
 * on this platform is the opposite case: everything past the named arguments
 * goes on the stack in eight-byte slots with the floating-point registers
 * untouched, which is a different layout entirely and not one the caller above
 * can describe, so a variadic function is refused rather than called from here.
 *
 * ---- why the v registers take a 64-bit load ----
 * Each word of the second array is loaded with an ldp of d registers, which
 * places it in the low half of the v register and zeroes the upper half.  That
 * is what a floating-point argument register has to look like from either side:
 * a double is the whole 64 bits, and a float is the low 32, which is exactly
 * the s register the callee reads.  A 128-bit q load would consume two words
 * per register instead of one, putting the second in an upper lane no argument
 * ever occupies and leaving the odd-numbered registers holding whatever lies
 * eight words past the end of the array.
 *
 * A null result slot means the caller does not want that half of the result,
 * which is what a void return, and any return that touches only one bank, asks
 * for.
 *
 * ---- the callback bank ----
 * The rest of the file runs the other way: native code calling guest code.  A
 * native function such as qsort receives a function pointer and calls it, and
 * when the guest supplied that pointer it names x86 code the native side cannot
 * jump to.  So it is handed the address of one slot in a bank of arm64
 * trampolines instead, and src/abi.c remembers which guest function and which
 * signature that slot stands for.
 *
 * The bank is assembled, not generated while the process runs.  Writing a
 * trampoline from inside a native callback would contend with the translator
 * for the one writable JIT arena, a forked child abandons that arena while
 * native code may still hold pointers into it, and an audio callback on a
 * real-time thread cannot wait for any of it.  Every slot is the same two
 * instructions, an adr of the slot's own address into x16 and a branch to the
 * common entry, so the whole bank is one repeated block and a slot's index is
 * its distance from the start of the bank divided by eight.  x16 is the
 * intra-procedure-call scratch register a linker veneer is allowed to clobber,
 * so borrowing it between the call and the common entry is legal.  The end
 * symbol is an alternate entry of the same atom, because a separate atom could
 * be placed anywhere by the linker and the distance between the two would stop
 * meaning the bank's length.
 *
 * The common entry is an ordinary AAPCS64 function to whoever called the slot.
 * It takes the incoming stack pointer before its own frame moves it, because
 * that is where the caller's stacked arguments begin, then saves x0..x7 and the
 * low 64 bits of v0..v7 in the same representation the forward caller loads, so
 * a float is the low half of its word.  The dispatcher gets the slot index, the
 * two arrays, the caller's stack and two words to write the result into, and
 * the entry loads x0 and d0 from those words on the way out.  Its frame is
 * exactly 160 bytes, a record plus sixteen argument words plus two result
 * words, so sp is a multiple of sixteen at every instruction and not merely at
 * the call.
 */
.section __TEXT,__text,regular,pure_instructions
.globl _ocerz_abi_call_native
.p2align 2

_ocerz_abi_call_native:
    .cfi_startproc
    stp     x23, x22, [sp, #-64]!
    .cfi_def_cfa_offset 64
    stp     x21, x20, [sp, #16]
    str     x19, [sp, #32]
    stp     x29, x30, [sp, #48]
    add     x29, sp, #48
    .cfi_def_cfa w29, 16
    .cfi_offset w30, -8
    .cfi_offset w29, -16
    .cfi_offset w19, -32
    .cfi_offset w20, -40
    .cfi_offset w21, -48
    .cfi_offset w22, -56
    .cfi_offset w23, -64

    mov     x19, x0
    mov     x20, x1
    mov     x21, x2
    mov     x22, x5
    mov     x23, x6

    mov     x9, x3
    mov     x10, x4
    add     x11, x4, #15
    and     x11, x11, #0xfffffffffffffff0
    sub     sp, sp, x11
    mov     x12, sp

Lword:
    cmp     x10, #8
    b.lo    Ltail
    ldr     x13, [x9], #8
    str     x13, [x12], #8
    sub     x10, x10, #8
    b       Lword

Ltail:
    cbz     x10, Largs
Lbyte:
    ldrb    w13, [x9], #1
    strb    w13, [x12], #1
    subs    x10, x10, #1
    b.ne    Lbyte

Largs:
    ldp     d0, d1, [x21]
    ldp     d2, d3, [x21, #16]
    ldp     d4, d5, [x21, #32]
    ldp     d6, d7, [x21, #48]
    ldp     x0, x1, [x20]
    ldp     x2, x3, [x20, #16]
    ldp     x4, x5, [x20, #32]
    ldp     x6, x7, [x20, #48]
    mov     x8, #0
    blr     x19

    cbz     x22, Lnoint
    str     x0, [x22]
Lnoint:
    cbz     x23, Lnofp
    str     d0, [x23]
Lnofp:
    sub     sp, x29, #48
    ldp     x29, x30, [sp, #48]
    ldr     x19, [sp, #32]
    ldp     x21, x20, [sp, #16]
    ldp     x23, x22, [sp], #64
    ret
    .cfi_endproc

.p2align 2
.globl _ocerz_abi_callback_bank
_ocerz_abi_callback_bank:
.rept 4096
    adr     x16, .
    b       _ocerz_abi_callback_common
.endr
.globl _ocerz_abi_callback_bank_end
.alt_entry _ocerz_abi_callback_bank_end
_ocerz_abi_callback_bank_end:

.p2align 2
_ocerz_abi_callback_common:
    .cfi_startproc
    mov     x17, sp
    stp     x29, x30, [sp, #-160]!
    .cfi_def_cfa_offset 160
    .cfi_offset w30, -152
    .cfi_offset w29, -160
    mov     x29, sp
    .cfi_def_cfa w29, 160
    stp     x0, x1, [sp, #16]
    stp     x2, x3, [sp, #32]
    stp     x4, x5, [sp, #48]
    stp     x6, x7, [sp, #64]
    stp     d0, d1, [sp, #80]
    stp     d2, d3, [sp, #96]
    stp     d4, d5, [sp, #112]
    stp     d6, d7, [sp, #128]
    stp     xzr, xzr, [sp, #144]
    adrp    x0, _ocerz_abi_callback_bank@PAGE
    add     x0, x0, _ocerz_abi_callback_bank@PAGEOFF
    sub     x0, x16, x0
    lsr     x0, x0, #3
    add     x1, sp, #16
    add     x2, sp, #80
    mov     x3, x17
    add     x4, sp, #144
    add     x5, sp, #152
    bl      _ocerz_abi_callback_dispatch
    ldr     x0, [sp, #144]
    ldr     d0, [sp, #152]
    ldp     x29, x30, [sp], #160
    ret
    .cfi_endproc

.subsections_via_symbols
