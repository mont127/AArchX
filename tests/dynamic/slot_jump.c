/*
 * Jumps through a rip-relative slot, the shape of a Mach-O stub's jmp *GOT,
 * which src/jit.c chains straight to the slot's target when the slot still
 * holds what it held at translation, and whose target's flag liveness it
 * follows.
 *   - A loop calls through the slot and counts down with dec, which keeps CF:
 *     with the target known, CF is dead there and is not rebuilt.  The slot is
 *     rewritten between runs, to another target and back, so a chain to the
 *     old target must be left for the new one.
 *   - A carry set or cleared by a cmp before an inc reaches a setc on the
 *     other side of a slot jump, through the inc's flag record, and a sete
 *     after it reads the inc's own ZF: the jump's target is what makes both
 *     live, and the setc asks the record for CF before anything else.
 * Against Rosetta.
 */
#include <stdint.h>
#include <stdio.h>

__asm__(
    ".data\n"
    ".p2align 3\n"
    "_call_slot: .quad 0\n"
    "_carry_slot: .quad 0\n"
    ".text\n"
    ".p2align 4\n"
    "_via_slot:\n"
    "    jmp *_call_slot(%rip)\n"
    ".p2align 4\n"
    "_f_triple:\n"
    "    lea 1(%rdi,%rdi,2), %eax\n"
    "    ret\n"
    ".p2align 4\n"
    "_f_plus7:\n"
    "    lea 7(%rdi), %eax\n"
    "    inc %eax\n"
    "    ret\n"
    ".p2align 4\n"
    "_count_calls:\n"            /* rdi = start value, rsi = count; returns the last value */
    "    push %rbx\n"
    "    mov %rsi, %rbx\n"
    "    mov %edi, %eax\n"
    "1:  mov %eax, %edi\n"
    "    call _via_slot\n"
    "    dec %rbx\n"
    "    jne 1b\n"
    "    pop %rbx\n"
    "    ret\n"
    ".p2align 4\n"
    "_carry_inc:\n"              /* rdi = 0 sets CF, else clears it; rsi = value to inc */
    "    xor %edx, %edx\n"
    "    mov %rsi, %rax\n"
    "    cmp $1, %rdi\n"           /* CF = (rdi < 1), kept in the compare's record */
    "    inc %rax\n"               /* ZF from the inc, CF from before it */
    "    jmp *_carry_slot(%rip)\n"
    ".p2align 4\n"
    "_flags_tail:\n"
    "    setc %cl\n"               /* CF first, from the inc's record */
    "    sete %dl\n"
    "    movzbl %cl, %ecx\n"
    "    movzbl %dl, %edx\n"
    "    lea (%rcx,%rdx,4), %rdx\n"
    "    lea (%rdx,%rax,2), %rax\n"
    "    ret\n");

extern uint64_t call_slot, carry_slot;
extern char via_slot[], f_triple[], f_plus7[], flags_tail[];
uint32_t count_calls(uint32_t start, uint64_t n);
uint64_t carry_inc(uint64_t k, uint64_t v);

int main(void)
{
    uint32_t got[6];
    call_slot = (uint64_t)(uintptr_t)f_triple;
    got[0] = count_calls(1, 3000000);
    call_slot = (uint64_t)(uintptr_t)f_plus7;
    got[1] = count_calls(1, 3000000);
    call_slot = (uint64_t)(uintptr_t)f_triple;
    got[2] = count_calls(5, 3000000);
    for (int r = 0; r < 3; r++) {
        call_slot = (uint64_t)(uintptr_t)(r & 1 ? f_plus7 : f_triple);
        got[3 + r] = count_calls((uint32_t)r, 100000);
    }
    printf("calls %u %u %u %u %u %u\n", got[0], got[1], got[2], got[3], got[4], got[5]);

    carry_slot = (uint64_t)(uintptr_t)flags_tail;
    uint64_t sum = 0;
    for (int r = 0; r < 200000; r++)
        sum += carry_inc((uint64_t)(r & 1), (uint64_t)r) * 3 + carry_inc((uint64_t)((r >> 1) & 1), ~0ull);
    printf("carry %llu %llu %llu %llu %llu\n", (unsigned long long)carry_inc(0, 5), (unsigned long long)carry_inc(1, 5),
           (unsigned long long)carry_inc(1, ~0ull), (unsigned long long)carry_inc(0, ~0ull), (unsigned long long)sum);
    return 0;
}
