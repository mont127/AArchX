#!/usr/bin/env bash
# Generates the decoder cross-validation corpus using the host x86_64 assembler as the length oracle.

set -euo pipefail

MIN_ENTRIES=250

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "$script_dir/.." && pwd)"
out="${1:-$repo_dir/tests/corpus/corpus.inc}"

die() {
    printf 'gen_corpus.sh: fatal: %s\n' "$*" >&2
    exit 1
}

command -v clang  >/dev/null 2>&1 || die "clang not found in PATH"
command -v nm     >/dev/null 2>&1 || die "nm not found in PATH"
command -v otool  >/dev/null 2>&1 || die "otool not found in PATH"

read -r -d '' INSN_LIST <<'EOF' || true
add r/m8 r8 ;; add %cl, (%rax)
add r8 r/m8 ;; add (%rax), %cl
add r/m16 r16 ;; add %cx, (%rax)
add r/m32 r32 ;; add %ecx, (%rax)
add r/m64 r64 ;; add %rcx, (%rax)
add r32 r/m32 ;; add (%rax), %ecx
add r64 r/m64 ;; add (%rax), %rcx
add al imm8 ;; add $0x12, %al
add ax imm16 ;; add $0x1234, %ax
add eax imm32 ;; add $0x12345678, %eax
add rax imm32 ;; add $0x12345678, %rax
add r/m32 imm32 ;; addl $0x12345678, (%rax)
add r/m32 imm8 ;; addl $0x7, (%rax)
add r/m64 imm8 ;; addq $0x7, (%rax)
add r/m16 imm16 ;; addw $0x1234, (%rax)
adc r/m32 r32 ;; adc %ecx, (%rax)
adc eax imm32 ;; adc $0x12345678, %eax
sub r/m32 r32 ;; sub %ecx, (%rax)
sub eax imm32 ;; sub $0x12345678, %eax
sub r/m64 r64 ;; sub %rcx, %rdx
sbb r/m32 r32 ;; sbb %ecx, (%rax)
sbb eax imm32 ;; sbb $0x12345678, %eax
and r/m32 r32 ;; and %ecx, (%rax)
and eax imm32 ;; and $0x12345678, %eax
and r/m8 imm8 ;; andb $0x7, (%rax)
or r/m32 r32 ;; or %ecx, (%rax)
or eax imm32 ;; or $0x12345678, %eax
xor r/m32 r32 ;; xor %ecx, %edx
xor eax imm32 ;; xor $0x12345678, %eax
xor r/m64 r64 ;; xor %rcx, %rdx
cmp r/m32 r32 ;; cmp %ecx, (%rax)
cmp r32 r/m32 ;; cmp (%rax), %ecx
cmp eax imm32 ;; cmp $0x12345678, %eax
cmp al imm8 ;; cmp $0x12, %al
cmp r/m8 imm8 ;; cmpb $0x12, (%rax)
test r/m32 r32 ;; test %ecx, (%rax)
test eax imm32 ;; test $0x12345678, %eax
test al imm8 ;; test $0x12, %al
test r/m8 imm8 ;; testb $0x12, (%rax)
inc r/m32 ;; incl (%rax)
inc r/m64 ;; incq %rax
dec r/m32 ;; decl (%rax)
dec r/m8 ;; decb (%rax)
neg r/m32 ;; negl (%rax)
not r/m32 ;; notl (%rax)
neg r/m64 ;; neg %rax
mul r/m32 ;; mull (%rax)
mul r/m8 ;; mulb (%rax)
imul r/m32 ;; imull (%rax)
imul r32 r/m32 ;; imul (%rax), %ecx
imul r32 r/m32 imm8 ;; imul $0x7, %ecx, %eax
imul r32 r/m32 imm32 ;; imul $0x12345678, %ecx, %eax
imul r64 r/m64 imm32 ;; imul $0x12345678, %rcx, %rax
div r/m32 ;; divl (%rax)
idiv r/m32 ;; idivl (%rax)
idiv r/m64 ;; idiv %rax

mov r/m8 r8 ;; mov %cl, (%rax)
mov r/m32 r32 ;; mov %ecx, (%rax)
mov r32 r/m32 ;; mov (%rax), %ecx
mov r/m64 r64 ;; mov %rcx, %rdx
mov r/m32 imm32 ;; movl $0x12345678, (%rax)
mov r32 imm32 ;; mov $0x12345678, %ecx
mov r64 imm64 ;; movabs $0x1122334455667788, %rax
mov r/m64 imm32 ;; movq $0x12345678, (%rax)
mov al moffs8 ;; movabs 0x1122334455667788, %al
mov eax moffs32 ;; movabs 0x1122334455667788, %eax
mov moffs32 eax ;; movabs %eax, 0x1122334455667788
mov r/m8 imm8 ;; movb $0x12, (%rax)
lea r32 m ;; lea 0x10(%rax), %ecx
lea r64 m ;; lea 0x10(%rax,%rcx,4), %rdx
xchg r/m32 r32 ;; xchg %ecx, (%rax)
xchg eax r32 ;; xchg %ecx, %eax
bswap r32 ;; bswap %eax
bswap r64 ;; bswap %rax

movzx r16 r/m8 ;; movzbw %al, %cx
movzx r32 r/m8 ;; movzbl %al, %ecx
movzx r64 r/m8 ;; movzbq %al, %rcx
movzx r32 r/m16 ;; movzwl %ax, %ecx
movzx r64 r/m16 ;; movzwq %ax, %rcx
movzx r32 m8 ;; movzbl (%rax), %ecx
movsx r16 r/m8 ;; movsbw %al, %cx
movsx r32 r/m8 ;; movsbl %al, %ecx
movsx r64 r/m8 ;; movsbq %al, %rcx
movsx r32 r/m16 ;; movswl %ax, %ecx
movsx r64 r/m16 ;; movswq %ax, %rcx
movsx r32 m8 ;; movsbl (%rax), %ecx
movslq r64 r/m32 ;; movslq %ecx, %rax
movslq r64 m32 ;; movslq (%rax), %rcx

ea direct reg ;; mov %ecx, %eax
ea [reg] ;; mov (%rbx), %eax
ea [reg+disp8] ;; mov 0x10(%rbx), %eax
ea [reg+disp32] ;; mov 0x12345678(%rbx), %eax
ea [reg+reg*scale] ;; mov (%rbx,%rcx,4), %eax
ea [reg*scale+disp32] ;; mov 0x12345678(,%rcx,8), %eax
ea [rip+disp32] ;; mov 0x100(%rip), %eax
ea [r12] ;; mov (%r12), %eax
ea [r12+reg*scale] ;; mov (%r12,%rcx,2), %eax
ea [r13] ;; mov (%r13), %eax
ea [r13+disp8] ;; mov 0x8(%r13), %eax
ea [rbp+disp0] ;; mov 0x0(%rbp), %eax
ea [rbp+disp32] ;; mov 0x12345678(%rbp), %eax
ea [rsp] ;; mov (%rsp), %eax
ea [rsp+disp8] ;; mov 0x8(%rsp), %eax
ea [r12+r13*scale] ;; mov (%r12,%r13,8), %eax

reg ah ;; mov %ah, %al
reg bh ;; mov %bh, %bl
reg ch ;; add %ch, %dh
reg spl ;; mov %spl, %al
reg sil ;; mov %sil, %dil
reg bpl ;; mov %bpl, %al
reg dil ;; add %dil, %sil

seg fs load ;; mov %fs:0x0, %eax
seg gs load ;; mov %gs:0x10, %rax
seg fs store ;; mov %eax, %fs:0x8
seg gs disp32 ;; mov %gs:0x12345678, %rax

rex r8b r9b ;; add %r8b, %r9b
rex r8w r9w ;; add %r8w, %r9w
rex r8d r9d ;; add %r8d, %r9d
rex r8 r9 ;; add %r8, %r9
rex r15b r14b ;; mov %r15b, %r14b
rex r10 mem_r11 ;; add %r10, (%r11)
rex movb r8b mem ;; movb %r8b, (%r9)
rex mov r15 r14 ;; mov %r15, %r14

addr32 mov ;; addr32 mov (%ebx), %edx
addr32 lea ;; addr32 lea 0x4(%ebx), %ecx

shl cl ;; shl %cl, %eax
shl imm8 ;; shl $0x3, %eax
shl 1 ;; shl %eax
shl r/m64 cl ;; shl %cl, %rax
shr cl ;; shr %cl, %eax
shr imm8 ;; shr $0x3, %eax
sar cl ;; sar %cl, %eax
sar imm8 ;; sar $0x3, %eax
rol imm8 ;; rol $0x2, %eax
rol cl ;; rol %cl, %eax
ror cl ;; ror %cl, %eax
rcl imm8 ;; rcl $0x1, %eax
rcr cl ;; rcr %cl, %eax
shld imm8 ;; shld $0x4, %ecx, %eax
shld cl ;; shld %cl, %ecx, %eax
shrd imm8 ;; shrd $0x4, %ecx, %eax
shrd cl ;; shrd %cl, %ecx, %eax

bt r/m32 r32 ;; bt %ecx, %eax
bt r/m32 imm8 ;; btl $0x3, %eax
bts r/m32 r32 ;; bts %ecx, %eax
btr r/m32 imm8 ;; btrl $0x3, %eax
btc r/m32 r32 ;; btc %ecx, %eax
bsf r32 r/m32 ;; bsf %ecx, %eax
bsr r32 r/m32 ;; bsr %ecx, %eax
popcnt r32 r/m32 ;; popcnt %ecx, %eax
popcnt r64 r/m64 ;; popcnt %rcx, %rax
tzcnt r32 r/m32 ;; tzcnt %ecx, %eax
lzcnt r32 r/m32 ;; lzcnt %ecx, %eax

je rel ;; je .Lhome
jne rel ;; jne .Lhome
jl rel ;; jl .Lhome
jg rel ;; jg .Lhome
jle rel ;; jle .Lhome
jge rel ;; jge .Lhome
jb rel ;; jb .Lhome
ja rel ;; ja .Lhome
jbe rel ;; jbe .Lhome
jae rel ;; jae .Lhome
jo rel ;; jo .Lhome
jno rel ;; jno .Lhome
js rel ;; js .Lhome
jns rel ;; jns .Lhome
jp rel ;; jp .Lhome
jnp rel ;; jnp .Lhome
jmp rel ;; jmp .Lhome
call rel ;; call .Lhome
jmp r/m64 ;; jmp *%rax
jmp m64 ;; jmp *(%rax)
call r/m64 ;; call *%rax
call m64 ;; call *(%rax)
jrcxz rel ;; jrcxz .Lhome
loop rel ;; loop .Lhome
loope rel ;; loope .Lhome
loopne rel ;; loopne .Lhome
ret ;; ret
ret imm16 ;; ret $0x10
leave ;; leave
int3 ;; int3
int imm8 ;; int $0x80
ud2 ;; ud2
hlt ;; hlt

setcc e ;; sete %al
setcc ne ;; setne (%rax)
setcc l ;; setl %cl
cmovcc e r32 ;; cmove %ecx, %eax
cmovcc ne r64 ;; cmovne %rcx, %rax
cmovcc l mem ;; cmovl (%rax), %ecx

push r64 ;; push %rax
push r12 ;; push %r12
push imm8 ;; push $0x10
push imm32 ;; push $0x12345678
push r/m64 ;; pushq (%rax)
pop r64 ;; pop %rbx
pop r13 ;; pop %r13
pop r/m64 ;; popq (%rax)
pushf ;; pushf
popf ;; popf
lahf ;; lahf
sahf ;; sahf

movs b ;; movsb
movs w ;; movsw
movs l ;; movsl
movs q ;; movsq
rep movs b ;; rep movsb
rep movs q ;; rep movsq
stos b ;; stosb
stos l ;; stosl
rep stos q ;; rep stosq
lods b ;; lodsb
lods q ;; lodsq
scas b ;; scasb
repne scas b ;; repne scasb
cmps b ;; cmpsb
repe cmps b ;; repe cmpsb

cbw ;; cbtw
cwde ;; cwtl
cdqe ;; cltq
cwd ;; cwtd
cdq ;; cltd
cqo ;; cqto
clc ;; clc
stc ;; stc
cmc ;; cmc
cld ;; cld
std ;; std

xadd r/m32 r32 ;; xadd %ecx, (%rax)
lock xadd r/m32 r32 ;; lock xadd %ecx, (%rax)
cmpxchg r/m32 r32 ;; cmpxchg %ecx, (%rax)
lock cmpxchg r/m32 r32 ;; lock cmpxchg %ecx, (%rax)
cmpxchg8b m64 ;; cmpxchg8b (%rax)
cmpxchg16b m128 ;; cmpxchg16b (%rax)
lock cmpxchg16b m128 ;; lock cmpxchg16b (%rax)
lock add r/m32 r32 ;; lock add %ecx, (%rax)
lock inc r/m32 ;; lock incl (%rax)

syscall ;; syscall
cpuid ;; cpuid
rdtsc ;; rdtsc
rdtscp ;; rdtscp
pause ;; pause
nop ;; nop
nop r/m16 ;; nopw 0x0(%rax)
nop r/m32 ;; nopl 0x0(%rax)
endbr64 ;; endbr64
mfence ;; mfence
lfence ;; lfence
sfence ;; sfence
xgetbv ;; xgetbv
ldmxcsr m32 ;; ldmxcsr (%rax)
stmxcsr m32 ;; stmxcsr (%rax)
fxsave m ;; fxsave (%rax)
fxrstor m ;; fxrstor (%rax)
prefetcht0 m8 ;; prefetcht0 (%rax)
prefetchnta m8 ;; prefetchnta (%rax)
clflush m8 ;; clflush (%rax)
emms ;; emms

fld st ;; fld %st(1)
fld m32 ;; flds (%rax)
fld m64 ;; fldl (%rax)
fld m80 ;; fldt (%rax)
fst st ;; fst %st(1)
fstp st ;; fstp %st(1)
fild m16 ;; filds (%rax)
fild m32 ;; fildl (%rax)
fistp m32 ;; fistpl (%rax)
fldcw m16 ;; fldcw (%rax)
fnstcw m16 ;; fnstcw (%rax)
fnstsw ax ;; fnstsw %ax
fnstsw m16 ;; fnstsw (%rax)
fxch st ;; fxch %st(1)
fchs ;; fchs
fabs ;; fabs
fadd st ;; fadd %st(1), %st
faddp st ;; faddp %st, %st(1)
fiadd m32 ;; fiaddl (%rax)
fsub st ;; fsub %st(1), %st
fsubr st ;; fsubr %st(1), %st
fmul st ;; fmul %st(1), %st
fdiv st ;; fdiv %st(1), %st
fcom st ;; fcom %st(1)
fcomp st ;; fcomp %st(1)
fcompp ;; fcompp
fcomi st ;; fcomi %st(1), %st
fucomi st ;; fucomi %st(1), %st
fucom st ;; fucom %st(1)
ftst ;; ftst
fldz ;; fldz
fld1 ;; fld1
fldpi ;; fldpi
fsqrt ;; fsqrt
frndint ;; frndint
fsin ;; fsin
fcos ;; fcos
fninit ;; fninit
fnclex ;; fnclex
ffree st ;; ffree %st(1)
fincstp ;; fincstp
fdecstp ;; fdecstp

movups xmm m ;; movups (%rax), %xmm0
movups m xmm ;; movups %xmm0, (%rax)
movaps xmm xmm ;; movaps %xmm1, %xmm0
movapd xmm xmm ;; movapd %xmm1, %xmm0
movdqa xmm m ;; movdqa (%rax), %xmm0
movdqu xmm m ;; movdqu (%rax), %xmm0
movss xmm m ;; movss (%rax), %xmm0
movsd xmm m ;; movsd (%rax), %xmm0
movd xmm r32 ;; movd %ecx, %xmm0
movd r32 xmm ;; movd %xmm0, %ecx
movq xmm r64 ;; movq %rcx, %xmm0
movq xmm m64 ;; movq (%rax), %xmm0
movq xmm xmm ;; movq %xmm1, %xmm0
movlps xmm m ;; movlps (%rax), %xmm0
movhps xmm m ;; movhps (%rax), %xmm0
movlhps xmm xmm ;; movlhps %xmm1, %xmm0
movhlps xmm xmm ;; movhlps %xmm1, %xmm0
movmskps r32 xmm ;; movmskps %xmm0, %eax
movmskpd r32 xmm ;; movmskpd %xmm0, %eax
pmovmskb r32 xmm ;; pmovmskb %xmm0, %eax
movshdup xmm xmm ;; movshdup %xmm1, %xmm0
movsldup xmm xmm ;; movsldup %xmm1, %xmm0
movddup xmm xmm ;; movddup %xmm1, %xmm0
movnti m32 r32 ;; movnti %ecx, (%rax)

addps xmm xmm ;; addps %xmm1, %xmm0
addpd xmm xmm ;; addpd %xmm1, %xmm0
addss xmm xmm ;; addss %xmm1, %xmm0
addsd xmm xmm ;; addsd %xmm1, %xmm0
subps xmm xmm ;; subps %xmm1, %xmm0
subpd xmm xmm ;; subpd %xmm1, %xmm0
subss xmm xmm ;; subss %xmm1, %xmm0
subsd xmm xmm ;; subsd %xmm1, %xmm0
mulps xmm xmm ;; mulps %xmm1, %xmm0
mulpd xmm xmm ;; mulpd %xmm1, %xmm0
mulss xmm xmm ;; mulss %xmm1, %xmm0
mulsd xmm xmm ;; mulsd %xmm1, %xmm0
divps xmm xmm ;; divps %xmm1, %xmm0
divpd xmm xmm ;; divpd %xmm1, %xmm0
divss xmm xmm ;; divss %xmm1, %xmm0
divsd xmm xmm ;; divsd %xmm1, %xmm0
minps xmm xmm ;; minps %xmm1, %xmm0
minpd xmm xmm ;; minpd %xmm1, %xmm0
minss xmm xmm ;; minss %xmm1, %xmm0
minsd xmm xmm ;; minsd %xmm1, %xmm0
maxps xmm xmm ;; maxps %xmm1, %xmm0
maxpd xmm xmm ;; maxpd %xmm1, %xmm0
maxss xmm xmm ;; maxss %xmm1, %xmm0
maxsd xmm xmm ;; maxsd %xmm1, %xmm0
sqrtps xmm xmm ;; sqrtps %xmm1, %xmm0
sqrtpd xmm xmm ;; sqrtpd %xmm1, %xmm0
sqrtss xmm xmm ;; sqrtss %xmm1, %xmm0
sqrtsd xmm xmm ;; sqrtsd %xmm1, %xmm0
rsqrtps xmm xmm ;; rsqrtps %xmm1, %xmm0
rsqrtss xmm xmm ;; rsqrtss %xmm1, %xmm0
rcpps xmm xmm ;; rcpps %xmm1, %xmm0
rcpss xmm xmm ;; rcpss %xmm1, %xmm0

andps xmm xmm ;; andps %xmm1, %xmm0
andnps xmm xmm ;; andnps %xmm1, %xmm0
orps xmm xmm ;; orps %xmm1, %xmm0
xorps xmm xmm ;; xorps %xmm1, %xmm0
andpd xmm xmm ;; andpd %xmm1, %xmm0
xorpd xmm xmm ;; xorpd %xmm1, %xmm0
pand xmm xmm ;; pand %xmm1, %xmm0
pandn xmm xmm ;; pandn %xmm1, %xmm0
por xmm xmm ;; por %xmm1, %xmm0
pxor xmm xmm ;; pxor %xmm1, %xmm0

cmpps xmm imm8 ;; cmpps $0x0, %xmm1, %xmm0
cmppd xmm imm8 ;; cmppd $0x1, %xmm1, %xmm0
cmpss xmm imm8 ;; cmpss $0x2, %xmm1, %xmm0
cmpsd xmm imm8 ;; cmpsd $0x3, %xmm1, %xmm0
comiss xmm xmm ;; comiss %xmm1, %xmm0
comisd xmm xmm ;; comisd %xmm1, %xmm0
ucomiss xmm xmm ;; ucomiss %xmm1, %xmm0
ucomisd xmm xmm ;; ucomisd %xmm1, %xmm0
pcmpeqb xmm xmm ;; pcmpeqb %xmm1, %xmm0
pcmpeqw xmm xmm ;; pcmpeqw %xmm1, %xmm0
pcmpeqd xmm xmm ;; pcmpeqd %xmm1, %xmm0
pcmpeqq xmm xmm ;; pcmpeqq %xmm1, %xmm0
pcmpgtb xmm xmm ;; pcmpgtb %xmm1, %xmm0
pcmpgtw xmm xmm ;; pcmpgtw %xmm1, %xmm0
pcmpgtd xmm xmm ;; pcmpgtd %xmm1, %xmm0
pcmpgtq xmm xmm ;; pcmpgtq %xmm1, %xmm0

cvtsi2ss xmm r32 ;; cvtsi2ss %ecx, %xmm0
cvtsi2sd xmm r64 ;; cvtsi2sd %rcx, %xmm0
cvtss2si r32 xmm ;; cvtss2si %xmm0, %eax
cvtsd2si r64 xmm ;; cvtsd2si %xmm0, %rax
cvttss2si r32 xmm ;; cvttss2si %xmm0, %eax
cvttsd2si r32 xmm ;; cvttsd2si %xmm0, %eax
cvtss2sd xmm xmm ;; cvtss2sd %xmm1, %xmm0
cvtsd2ss xmm xmm ;; cvtsd2ss %xmm1, %xmm0
cvtps2pd xmm xmm ;; cvtps2pd %xmm1, %xmm0
cvtpd2ps xmm xmm ;; cvtpd2ps %xmm1, %xmm0
cvtdq2ps xmm xmm ;; cvtdq2ps %xmm1, %xmm0
cvtps2dq xmm xmm ;; cvtps2dq %xmm1, %xmm0
cvttps2dq xmm xmm ;; cvttps2dq %xmm1, %xmm0
cvtdq2pd xmm xmm ;; cvtdq2pd %xmm1, %xmm0
cvtpd2dq xmm xmm ;; cvtpd2dq %xmm1, %xmm0
cvttpd2dq xmm xmm ;; cvttpd2dq %xmm1, %xmm0

paddb xmm xmm ;; paddb %xmm1, %xmm0
paddw xmm xmm ;; paddw %xmm1, %xmm0
paddd xmm xmm ;; paddd %xmm1, %xmm0
paddq xmm xmm ;; paddq %xmm1, %xmm0
psubb xmm xmm ;; psubb %xmm1, %xmm0
psubw xmm xmm ;; psubw %xmm1, %xmm0
psubd xmm xmm ;; psubd %xmm1, %xmm0
psubq xmm xmm ;; psubq %xmm1, %xmm0
paddsb xmm xmm ;; paddsb %xmm1, %xmm0
paddsw xmm xmm ;; paddsw %xmm1, %xmm0
paddusb xmm xmm ;; paddusb %xmm1, %xmm0
paddusw xmm xmm ;; paddusw %xmm1, %xmm0
psubsb xmm xmm ;; psubsb %xmm1, %xmm0
psubsw xmm xmm ;; psubsw %xmm1, %xmm0
psubusb xmm xmm ;; psubusb %xmm1, %xmm0
psubusw xmm xmm ;; psubusw %xmm1, %xmm0
pmullw xmm xmm ;; pmullw %xmm1, %xmm0
pmulld xmm xmm ;; pmulld %xmm1, %xmm0
pmulhw xmm xmm ;; pmulhw %xmm1, %xmm0
pmulhuw xmm xmm ;; pmulhuw %xmm1, %xmm0
pmuludq xmm xmm ;; pmuludq %xmm1, %xmm0
pmaddwd xmm xmm ;; pmaddwd %xmm1, %xmm0
pavgb xmm xmm ;; pavgb %xmm1, %xmm0
pavgw xmm xmm ;; pavgw %xmm1, %xmm0
pmaxub xmm xmm ;; pmaxub %xmm1, %xmm0
pmaxsw xmm xmm ;; pmaxsw %xmm1, %xmm0
pminub xmm xmm ;; pminub %xmm1, %xmm0
pminsw xmm xmm ;; pminsw %xmm1, %xmm0
pmaxsb xmm xmm ;; pmaxsb %xmm1, %xmm0
pmaxsd xmm xmm ;; pmaxsd %xmm1, %xmm0
pmaxuw xmm xmm ;; pmaxuw %xmm1, %xmm0
pmaxud xmm xmm ;; pmaxud %xmm1, %xmm0
pminsb xmm xmm ;; pminsb %xmm1, %xmm0
pminsd xmm xmm ;; pminsd %xmm1, %xmm0
pminuw xmm xmm ;; pminuw %xmm1, %xmm0
pminud xmm xmm ;; pminud %xmm1, %xmm0
psadbw xmm xmm ;; psadbw %xmm1, %xmm0
pabsb xmm xmm ;; pabsb %xmm1, %xmm0
pabsw xmm xmm ;; pabsw %xmm1, %xmm0
pabsd xmm xmm ;; pabsd %xmm1, %xmm0

packsswb xmm xmm ;; packsswb %xmm1, %xmm0
packssdw xmm xmm ;; packssdw %xmm1, %xmm0
packuswb xmm xmm ;; packuswb %xmm1, %xmm0
packusdw xmm xmm ;; packusdw %xmm1, %xmm0
punpcklbw xmm xmm ;; punpcklbw %xmm1, %xmm0
punpcklwd xmm xmm ;; punpcklwd %xmm1, %xmm0
punpckldq xmm xmm ;; punpckldq %xmm1, %xmm0
punpcklqdq xmm xmm ;; punpcklqdq %xmm1, %xmm0
punpckhbw xmm xmm ;; punpckhbw %xmm1, %xmm0
punpckhwd xmm xmm ;; punpckhwd %xmm1, %xmm0
punpckhdq xmm xmm ;; punpckhdq %xmm1, %xmm0
punpckhqdq xmm xmm ;; punpckhqdq %xmm1, %xmm0
pshufd xmm imm8 ;; pshufd $0x1b, %xmm1, %xmm0
pshuflw xmm imm8 ;; pshuflw $0x1b, %xmm1, %xmm0
pshufhw xmm imm8 ;; pshufhw $0x1b, %xmm1, %xmm0
pshufb xmm xmm ;; pshufb %xmm1, %xmm0
palignr xmm imm8 ;; palignr $0x4, %xmm1, %xmm0
shufps xmm imm8 ;; shufps $0x1b, %xmm1, %xmm0
shufpd xmm imm8 ;; shufpd $0x1, %xmm1, %xmm0
unpcklps xmm xmm ;; unpcklps %xmm1, %xmm0
unpckhps xmm xmm ;; unpckhps %xmm1, %xmm0
unpcklpd xmm xmm ;; unpcklpd %xmm1, %xmm0
unpckhpd xmm xmm ;; unpckhpd %xmm1, %xmm0

psllw xmm xmm ;; psllw %xmm1, %xmm0
pslld xmm xmm ;; pslld %xmm1, %xmm0
psllq xmm xmm ;; psllq %xmm1, %xmm0
psrlw xmm xmm ;; psrlw %xmm1, %xmm0
psrld xmm xmm ;; psrld %xmm1, %xmm0
psrlq xmm xmm ;; psrlq %xmm1, %xmm0
psraw xmm xmm ;; psraw %xmm1, %xmm0
psrad xmm xmm ;; psrad %xmm1, %xmm0
psllw imm8 ;; psllw $0x4, %xmm0
pslld imm8 ;; pslld $0x4, %xmm0
psllq imm8 ;; psllq $0x4, %xmm0
psrlw imm8 ;; psrlw $0x4, %xmm0
psrld imm8 ;; psrld $0x4, %xmm0
psrlq imm8 ;; psrlq $0x4, %xmm0
psraw imm8 ;; psraw $0x4, %xmm0
psrad imm8 ;; psrad $0x4, %xmm0
pslldq imm8 ;; pslldq $0x4, %xmm0
psrldq imm8 ;; psrldq $0x4, %xmm0

pextrb imm8 ;; pextrb $0x1, %xmm0, %eax
pextrw imm8 ;; pextrw $0x2, %xmm0, %eax
pextrd imm8 ;; pextrd $0x3, %xmm0, %eax
pextrq imm8 ;; pextrq $0x1, %xmm0, %rax
pinsrb imm8 ;; pinsrb $0x1, %eax, %xmm0
pinsrw imm8 ;; pinsrw $0x2, %eax, %xmm0
pinsrd imm8 ;; pinsrd $0x3, %eax, %xmm0
pinsrq imm8 ;; pinsrq $0x1, %rax, %xmm0
extractps imm8 ;; extractps $0x1, %xmm0, %eax
insertps imm8 ;; insertps $0x8, %xmm1, %xmm0

ptest xmm xmm ;; ptest %xmm1, %xmm0
pmovzxbw xmm xmm ;; pmovzxbw %xmm1, %xmm0
pmovzxbd xmm xmm ;; pmovzxbd %xmm1, %xmm0
pmovzxbq xmm xmm ;; pmovzxbq %xmm1, %xmm0
pmovzxwd xmm xmm ;; pmovzxwd %xmm1, %xmm0
pmovzxwq xmm xmm ;; pmovzxwq %xmm1, %xmm0
pmovzxdq xmm xmm ;; pmovzxdq %xmm1, %xmm0
pmovsxbw xmm xmm ;; pmovsxbw %xmm1, %xmm0
pmovsxbd xmm xmm ;; pmovsxbd %xmm1, %xmm0
pmovsxbq xmm xmm ;; pmovsxbq %xmm1, %xmm0
pmovsxwd xmm xmm ;; pmovsxwd %xmm1, %xmm0
pmovsxwq xmm xmm ;; pmovsxwq %xmm1, %xmm0
pmovsxdq xmm xmm ;; pmovsxdq %xmm1, %xmm0
roundps imm8 ;; roundps $0x3, %xmm1, %xmm0
roundpd imm8 ;; roundpd $0x3, %xmm1, %xmm0
roundss imm8 ;; roundss $0x1, %xmm1, %xmm0
roundsd imm8 ;; roundsd $0x2, %xmm1, %xmm0
pblendw imm8 ;; pblendw $0x6, %xmm1, %xmm0
blendps imm8 ;; blendps $0x4, %xmm1, %xmm0
blendpd imm8 ;; blendpd $0x5, %xmm1, %xmm0
EOF

work="$(mktemp -d "${TMPDIR:-/tmp}/ocerz_corpus.XXXXXX")"
trap 'rm -rf "$work"' EXIT
asm="$work/corpus.s"
obj="$work/corpus.o"
manifest="$work/manifest.txt"

{
    printf '.text\n'
    printf '.globl _start\n'
    printf '_start:\n'
    idx=0
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        mnem="${line%% ;; *}"
        asmsrc="${line#* ;; }"
        [ "$mnem" != "$line" ] || die "malformed list line (no ' ;; '): $line"
        label="$(printf 'oc_%06d' "$idx")"
        printf '%s: %s\n' "$label" "$asmsrc"
        printf '%s\t%s\n' "$label" "$mnem" >>"$manifest"
        idx=$((idx + 1))
    done <<<"$INSN_LIST"
    printf '.Lhome:\n'
    printf '    nop\n'
} >"$asm"

n_listed="$(wc -l <"$manifest" | tr -d ' ')"
[ "$n_listed" -ge "$MIN_ENTRIES" ] || die "only $n_listed instructions listed, need >= $MIN_ENTRIES"

clang -arch x86_64 -c "$asm" -o "$obj" \
    || die "assembly failed (clang -arch x86_64 -c)"

sec_size_hex="$(otool -l "$obj" \
    | awk '/sectname __text/{f=1} f&&/^ *size /{print $2; exit}')"
[ -n "$sec_size_hex" ] || die "could not read __text section size from otool -l"
sec_size=$((sec_size_hex))
[ "$sec_size" -gt 0 ] || die "__text section size parsed as zero"

otool -t "$obj" \
    | awk 'NR>2 {for(i=2;i<=NF;i++) if($i ~ /^[0-9a-fA-F][0-9a-fA-F]$/) print $i}' \
    >"$work/bytes.txt"
n_bytes="$(wc -l <"$work/bytes.txt" | tr -d ' ')"
[ "$n_bytes" -eq "$sec_size" ] \
    || die "byte stream length ($n_bytes) != section size ($sec_size)"

home_off_hex="$(nm "$obj" | awk '$3 == ".Lhome" {print $1; exit}')"
[ -n "$home_off_hex" ] || die "could not find .Lhome label offset via nm"
home_off=$((16#$home_off_hex))
[ "$home_off" -gt 0 ] || die ".Lhome offset parsed as zero"
[ "$home_off" -lt "$sec_size" ] \
    || die ".Lhome offset ($home_off) not before section end ($sec_size)"

nm "$obj" \
    | awk '$3 ~ /^oc_[0-9]+$/ {print $1, $3}' \
    | while read -r offhex label; do
        printf '%d %s\n' "$((16#$offhex))" "$label"
      done \
    | sort -n >"$work/offsets.txt"

n_off="$(wc -l <"$work/offsets.txt" | tr -d ' ')"
[ "$n_off" -eq "$n_listed" ] \
    || die "boundary label count ($n_off) != listed instructions ($n_listed)"

emit_inc() {
    printf '/*\n'
    printf ' * tests/corpus/corpus.inc\n'
    printf ' *\n'
    printf ' * GENERATED by tools/gen_corpus.sh -- do not edit by hand.\n'
    printf ' *\n'
    printf ' * Decoder cross-validation corpus: one X() row per x86_64\n'
    printf ' * instruction, of the form X("mnemonic", LEN, byte, byte, ...),\n'
    printf ' * where LEN and the bytes are exactly what the host x86_64\n'
    printf ' * assembler emitted. The consuming test (tests/unit/test_corpus.c)\n'
    printf ' * defines X and decodes each byte string, asserting that\n'
    printf ' * ocerz_decode succeeds and reports out.len == LEN. Branch\n'
    printf ' * displacement bytes may be assembler placeholders; only the\n'
    printf ' * encoded length is under test.\n'
    printf ' *\n'
    printf ' * Regenerate with: tools/gen_corpus.sh\n'
    printf ' */\n'

    awk -v sec="$home_off" \
        -v manifest="$manifest" -v bytes="$work/bytes.txt" '
    BEGIN {
        n = 0;
        while ((getline mline < manifest) > 0) {
            split(mline, mp, "\t");
            mnem[mp[1]] = mp[2];
        }
        i = 0;
        while ((getline bline < bytes) > 0) {
            byteval[i++] = bline;
        }
        total_bytes = i;
    }
    {
        off[n] = $1; lab[n] = $2; n++;
    }
    END {
        for (k = 0; k < n; k++) {
            start = off[k];
            end = (k + 1 < n) ? off[k + 1] : sec;
            len = end - start;
            if (len <= 0) {
                printf("gen_corpus.sh: fatal: zero/negative length for %s\n", lab[k]) > "/dev/stderr";
                exit 3;
            }
            if (len > 15) {
                printf("gen_corpus.sh: fatal: length %d > 15 for %s\n", len, lab[k]) > "/dev/stderr";
                exit 3;
            }
            m = mnem[lab[k]];
            if (m == "") {
                printf("gen_corpus.sh: fatal: no mnemonic for %s\n", lab[k]) > "/dev/stderr";
                exit 3;
            }
            printf("X(\"%s\", %d", m, len);
            for (b = start; b < end; b++) {
                if (b >= total_bytes) {
                    printf("gen_corpus.sh: fatal: byte index %d past stream\n", b) > "/dev/stderr";
                    exit 3;
                }
                printf(", 0x%s", byteval[b]);
            }
            printf(")\n");
        }
    }
    ' "$work/offsets.txt"
}

mkdir -p "$(dirname "$out")"
tmp_out="$work/out.inc"
emit_inc >"$tmp_out"

n_rows="$(grep -c '^X(' "$tmp_out" || true)"
[ "$n_rows" -eq "$n_listed" ] \
    || die "emitted rows ($n_rows) != listed instructions ($n_listed)"
[ "$n_rows" -ge "$MIN_ENTRIES" ] \
    || die "emitted only $n_rows rows, need >= $MIN_ENTRIES"

mv "$tmp_out" "$out"
printf 'gen_corpus.sh: wrote %s (%d instructions)\n' "$out" "$n_rows"
