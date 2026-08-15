#!/bin/zsh
# xbench_compare.sh — Ocerz vs Rosetta over the xbench kernel suite.
# Paired-delta method: time(scale) - time(scale/2) for both engines, cancels
# process startup and JIT warmup; engine order alternates each rep.
set -u
zmodload zsh/datetime
REPO=${0:A:h:h}
OCERZ=${OCERZ:-$REPO/ocerz}
XB=$REPO/tests/guest/benchbin/xbench
REPS=${REPS:-3}
KERNELS=${KERNELS:-icall,jtab,depchain,brmiss,memcpy,str,hash,idiv,fpsse,fpvec,chase,qsort,leafcall,mixed,vm}
TARGET=${TARGET:-0.8}   # seconds of Rosetta time at full scale
typeset -A DFLT
DFLT=(icall 50000000 jtab 50000000 depchain 100000000 brmiss 50000000 memcpy 2000000 str 20000000 hash 20000 idiv 10000000 fpsse 30000000 fpvec 5000 chase 30000000 qsort 30 leafcall 50000000 mixed 20000 vm 500000)

t_engine() { # engine kernel scale -> REPLY seconds
  local e=$1 k=$2 n=$3 s0 s1
  s0=$EPOCHREALTIME
  if [[ $e == R ]]; then "$XB" $k $n >/dev/null 2>&1; else "$OCERZ" "$XB" $k $n >/dev/null 2>&1; fi
  s1=$EPOCHREALTIME; REPLY=$(( s1 - s0 ))
}
median() { local -a s; s=("${(@on)@}"); local n=${#s}; if (( n % 2 )); then REPLY=${s[$((n/2+1))]}; else REPLY=$(( (s[n/2] + s[n/2+1]) / 2.0 )); fi }

printf '%-10s %10s %10s %10s %10s %8s\n' kernel scale Rosetta_s Ocerz_s ratio verdict
printf '%-10s %10s %10s %10s %10s %8s\n' ------ ----- --------- ------- ----- -------
integer fails=0
for k in ${(s:,:)KERNELS}; do
  # calibrate: scale so that Rosetta full run ~= TARGET seconds
  local n=${DFLT[$k]}
  t_engine R $k $n; local t=$REPLY
  (( t > 0.05 )) || t=0.05
  n=$(( n * TARGET / t )); n=${n%.*}; (( n < 2 )) && n=2
  local half=$(( n / 2 ))
  local -a rr oo ratios
  for (( rep=1; rep<=REPS; rep++ )); do
    if (( rep % 2 )); then
      t_engine R $k $half; local rl=$REPLY; t_engine O $k $half; local ol=$REPLY
      t_engine R $k $n;    local rh=$REPLY; t_engine O $k $n;    local oh=$REPLY
    else
      t_engine O $k $n;    local oh=$REPLY; t_engine R $k $n;    local rh=$REPLY
      t_engine O $k $half; local ol=$REPLY; t_engine R $k $half; local rl=$REPLY
    fi
    local rd=$(( rh - rl )) od=$(( oh - ol ))
    (( rd > 0.001 )) || rd=0.001; (( od > 0.001 )) || od=0.001
    rr+=($rd); oo+=($od); ratios+=($(( od / rd )))
  done
  median $rr; local rm=$REPLY; median $oo; local om=$REPLY; median $ratios; local ratio=$REPLY
  local v=WIN; (( ratio >= 1.0 )) && { v=LOSE; (( fails++ )); }
  printf '%-10s %10d %10.4f %10.4f %9.3fx %8s\n' $k $n $rm $om $ratio $v
done
print; (( fails )) && { print "LOSING on $fails kernel(s)"; exit 1; }; print "Ocerz faster on every kernel"; exit 0
