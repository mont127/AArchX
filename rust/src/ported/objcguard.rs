//! Catching a native exception where a crossing returns to the guest.
//!
//! A native method that raises an Objective-C exception unwinds the host stack,
//! and above the native frames that stack holds ocerz's own: the send handler,
//! the dispatcher and the thread's entry, none of which catches anything, so
//! without this the runtime found no handler and terminated, even when the
//! guest had wrapped the send in @try.  ocerz_objc_guarded calls body(ctx) in a
//! frame whose personality is ocerz_objc_guard_personality (src/objcbridge.c).
//! The unwinder's search finds a handler here; its cleanup phase runs every
//! native frame's cleanups on the way, as it would for any catch, and then
//! resumes at the landing pad, whose address ocerz_objc_guard_pad holds, since
//! no global label may sit inside the procedure, with the exception in x0, where
//! ocerz_objc_guard_landed claims the object and the call answers 1.  An
//! exception the guard passes on comes back through here as a rethrow, and the
//! personality lets that one by.
//!
//! x19 holds the address the caught object is stored through.  It is a
//! callee-saved register, so the unwinder restores it to this frame's value
//! before the landing pad runs.  The frame is 32 bytes, saving x19 and x20 as a
//! pair to keep sp a multiple of sixteen.
//!
//! This is the Rust port of src/objcguard.s: the same assembly verbatim via
//! global_asm!, since the unwind info must be byte-identical to what the C
//! build's assembler emitted.

core::arch::global_asm!(
    r#"
.section __TEXT,__text,regular,pure_instructions
.globl _ocerz_objc_guarded
.p2align 2

_ocerz_objc_guarded:
    .cfi_startproc
    .cfi_personality 155, _ocerz_objc_guard_personality
    stp     x20, x19, [sp, #-32]!
    .cfi_def_cfa_offset 32
    stp     x29, x30, [sp, #16]
    add     x29, sp, #16
    .cfi_def_cfa w29, 16
    .cfi_offset w30, -8
    .cfi_offset w29, -16
    .cfi_offset w19, -24
    .cfi_offset w20, -32

    mov     x19, x2
    mov     x9, x0
    mov     x0, x1
    blr     x9
    mov     w0, #0
Lguard_out:
    ldp     x29, x30, [sp, #16]
    ldp     x20, x19, [sp], #32
    ret

Lguard_pad:
    mov     x1, x19
    bl      _ocerz_objc_guard_landed
    b       Lguard_out
    .cfi_endproc

.section __DATA,__const
.globl _ocerz_objc_guard_pad
.p2align 3
_ocerz_objc_guard_pad:
    .quad   Lguard_pad
"#
);
