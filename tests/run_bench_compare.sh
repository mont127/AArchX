#!/bin/zsh
# Ocerz-vs-Rosetta throughput gate for the benchmark kernels; engine order alternates between reps.

set -u
zmodload zsh/datetime || {
    print -u2 "run_bench_compare: zsh/datetime is required"
    exit 2
}

REPO_ROOT=${0:A:h:h}
OCERZ=${OCERZ:-$REPO_ROOT/ocerz}
BENCH_DIR=$REPO_ROOT/tests/guest/benchbin
BENCH=$BENCH_DIR/bench
FIBN=$BENCH_DIR/fibn
MEMN=$BENCH_DIR/memn

REPS=5
PASS_RATIO=1.0
WORKLOAD_CSV=call,alu,mem,br

usage() {
    cat <<'EOF'
usage: tests/run_bench_compare.sh [options]

Options:
  --reps N             Measured repetitions per workload (default: 5)
  --workloads LIST     Comma-separated call,alu,mem,br subset
  --pass-ratio R       Pass only when Ocerz/Rosetta < R (default: 1.0)
  --ocerz PATH         Ocerz executable (default: ./ocerz or $OCERZ)
  -h, --help           Show this help

Metrics:
  call                 median paired ratio of fibn(42)-fibn(38)
  alu                  median Ocerz wall time / median Rosetta wall time
  mem                  median paired ratio of memn(400M)-memn(40M)
  br                   median Ocerz wall time / median Rosetta wall time

Exit status: 0 all ratios pass; 1 one or more ratios miss; 2 harness error.
EOF
}

die() {
    print -u2 "run_bench_compare: $*"
    exit 2
}

while (( $# )); do
    case $1 in
    --reps)
        (( $# >= 2 )) || die "--reps requires a value"
        REPS=$2
        shift 2
        ;;
    --workloads)
        (( $# >= 2 )) || die "--workloads requires a value"
        WORKLOAD_CSV=$2
        shift 2
        ;;
    --pass-ratio)
        (( $# >= 2 )) || die "--pass-ratio requires a value"
        PASS_RATIO=$2
        shift 2
        ;;
    --ocerz)
        (( $# >= 2 )) || die "--ocerz requires a value"
        OCERZ=$2
        shift 2
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        die "unknown option: $1"
        ;;
    esac
done

[[ $REPS == <-> ]] && (( REPS > 0 )) || die "--reps must be a positive integer"
[[ $PASS_RATIO =~ '^[0-9]+([.][0-9]+)?$' ]] || die "--pass-ratio must be a positive number"
(( PASS_RATIO > 0.0 )) || die "--pass-ratio must be greater than zero"

typeset -a WORKLOADS
WORKLOADS=("${(@s:,:)WORKLOAD_CSV}")
(( ${#WORKLOADS} > 0 )) || die "--workloads must not be empty"

typeset -A SEEN
for workload in $WORKLOADS; do
    case $workload in
    call|alu|mem|br) ;;
    *) die "unknown workload '$workload' (expected call, alu, mem, or br)" ;;
    esac
    [[ -z ${SEEN[$workload]:-} ]] || die "duplicate workload '$workload'"
    SEEN[$workload]=1
done

[[ -x $OCERZ ]] || die "Ocerz executable not found: $OCERZ (build with: make ocerz)"
for binary in $BENCH $FIBN $MEMN; do
    [[ -x $binary ]] || die "benchmark not found: $binary (build with: make -C tests/guest bench)"
done

if [[ ${OCERZ:A} == $REPO_ROOT/ocerz ]] && \
        ! make -q -C "$REPO_ROOT" ocerz >/dev/null 2>&1; then
    die "Ocerz binary is stale (rebuild with: make ocerz)"
fi
if ! make -q -C "$REPO_ROOT/tests/guest" bench >/dev/null 2>&1; then
    die "benchmark binaries are stale (rebuild with: make -C tests/guest bench)"
fi

sha256() {
    shasum -a 256 "$1" | awk '{print $1}'
}

OCERZ_SHA=$(sha256 "$OCERZ")
BENCH_SHA=$(sha256 "$BENCH")
FIBN_SHA=$(sha256 "$FIBN")
MEMN_SHA=$(sha256 "$MEMN")

print "Ocerz vs Rosetta benchmark"
print "  host:       $(sw_vers -productVersion 2>/dev/null || print unknown) / $(uname -m)"
print "  reps:       $REPS (plus one correctness warmup)"
print "  pass rule:  Ocerz/Rosetta < $PASS_RATIO for every selected workload"
print "  workloads:  ${(j:,:)WORKLOADS}"
print "  ocerz:      $OCERZ"
print "  ocerz sha:  $OCERZ_SHA"
print "  bench sha:  $BENCH_SHA"
print "  fibn sha:   $FIBN_SHA"
print "  memn sha:   $MEMN_SHA"
print

typeset CAPTURE_OUT
integer CAPTURE_RC

capture_engine() {
    local engine=$1
    local binary=$2
    shift 2

    if [[ $engine == rosetta ]]; then
        CAPTURE_OUT=$("$binary" "$@" 2>/dev/null)
        CAPTURE_RC=$?
    else
        CAPTURE_OUT=$("$OCERZ" "$binary" "$@" 2>/dev/null)
        CAPTURE_RC=$?
    fi
}

check_case() {
    local label=$1
    local binary=$2
    shift 2
    local rosetta_out ocerz_out
    integer rosetta_rc ocerz_rc

    capture_engine rosetta "$binary" "$@"
    rosetta_out=$CAPTURE_OUT
    rosetta_rc=$CAPTURE_RC
    capture_engine ocerz "$binary" "$@"
    ocerz_out=$CAPTURE_OUT
    ocerz_rc=$CAPTURE_RC

    if (( rosetta_rc != ocerz_rc )) || [[ $rosetta_out != $ocerz_out ]]; then
        print -u2 "FAIL preflight $label"
        print -u2 "  Rosetta: exit=$rosetta_rc output=${(q)rosetta_out}"
        print -u2 "  Ocerz:   exit=$ocerz_rc output=${(q)ocerz_out}"
        exit 2
    fi
    (( rosetta_rc == 0 )) || die "preflight $label returned exit $rosetta_rc under both engines"
    printf '  %-12s PASS  output=%s\n' "$label" "${(q)rosetta_out}"
}

print "Correctness preflight and warmup"
for workload in $WORKLOADS; do
    case $workload in
    call)
        check_case fib38 "$FIBN" 38
        check_case fib42 "$FIBN" 42
        ;;
    alu)
        check_case alu "$BENCH" alu
        ;;
    mem)
        check_case mem40M "$MEMN" 40000000
        check_case mem400M "$MEMN" 400000000
        ;;
    br)
        check_case br "$BENCH" br
        ;;
    esac
done
print

typeset -F 9 LAST_TIME

time_engine() {
    local engine=$1
    local binary=$2
    shift 2
    local start=$EPOCHREALTIME
    integer rc

    if [[ $engine == rosetta ]]; then
        "$binary" "$@" >/dev/null 2>/dev/null
        rc=$?
    else
        "$OCERZ" "$binary" "$@" >/dev/null 2>/dev/null
        rc=$?
    fi
    local stop=$EPOCHREALTIME
    (( rc == 0 )) || die "$engine timing command failed with exit $rc: $binary $*"
    LAST_TIME=$(( stop - start ))
    (( LAST_TIME > 0.0 )) || die "$engine timing command produced non-positive elapsed time"
}

median() {
    local -a sorted
    sorted=("${(@on)@}")
    integer n=${#sorted}

    (( n > 0 )) || die "internal error: median of no samples"
    if (( n % 2 )); then
        REPLY=${sorted[$(( n / 2 + 1 ))]}
    else
        REPLY=$(( (sorted[n / 2] + sorted[n / 2 + 1]) / 2.0 ))
    fi
}

typeset -a RESULT_NAMES RESULT_MODES RESULT_ROSETTA RESULT_OCERZ RESULT_RATIOS RESULT_STATUS

record_result() {
    local name=$1 mode=$2 rosetta=$3 ocerz=$4 ratio=$5
    local outcome=FAIL
    (( ratio < PASS_RATIO )) && outcome=PASS
    RESULT_NAMES+=($name)
    RESULT_MODES+=($mode)
    RESULT_ROSETTA+=($rosetta)
    RESULT_OCERZ+=($ocerz)
    RESULT_RATIOS+=($ratio)
    RESULT_STATUS+=($outcome)
}

run_direct() {
    local name=$1
    local binary=$2
    shift 2
    local -a rosetta_samples ocerz_samples
    integer rep
    local rosetta_time ocerz_time ratio

    print "Timing $name (wall-time medians)"
    for (( rep = 1; rep <= REPS; rep++ )); do
        if (( rep % 2 )); then
            time_engine rosetta "$binary" "$@"; rosetta_time=$LAST_TIME
            time_engine ocerz "$binary" "$@"; ocerz_time=$LAST_TIME
            local order=R/O
        else
            time_engine ocerz "$binary" "$@"; ocerz_time=$LAST_TIME
            time_engine rosetta "$binary" "$@"; rosetta_time=$LAST_TIME
            local order=O/R
        fi
        rosetta_samples+=($rosetta_time)
        ocerz_samples+=($ocerz_time)
        ratio=$(( ocerz_time / rosetta_time ))
        printf '  rep %-2d order=%-3s Rosetta=%.6fs Ocerz=%.6fs ratio=%.4fx\n' \
            $rep $order $rosetta_time $ocerz_time $ratio
    done

    median $rosetta_samples; rosetta_time=$REPLY
    median $ocerz_samples; ocerz_time=$REPLY
    ratio=$(( ocerz_time / rosetta_time ))
    record_result $name median $rosetta_time $ocerz_time $ratio
    print
}

run_delta() {
    local name=$1
    local binary=$2
    local low=$3
    local high=$4
    local -a rosetta_deltas ocerz_deltas paired_ratios
    integer rep
    local rlow rhigh olow ohigh rdelta odelta ratio

    print "Timing $name (paired delta: $high - $low)"
    for (( rep = 1; rep <= REPS; rep++ )); do
        if (( rep % 2 )); then
            time_engine rosetta "$binary" $low; rlow=$LAST_TIME
            time_engine ocerz "$binary" $low; olow=$LAST_TIME
            time_engine rosetta "$binary" $high; rhigh=$LAST_TIME
            time_engine ocerz "$binary" $high; ohigh=$LAST_TIME
            local order=Rlow/Olow/Rhigh/Ohigh
        else
            time_engine ocerz "$binary" $high; ohigh=$LAST_TIME
            time_engine rosetta "$binary" $high; rhigh=$LAST_TIME
            time_engine ocerz "$binary" $low; olow=$LAST_TIME
            time_engine rosetta "$binary" $low; rlow=$LAST_TIME
            local order=Ohigh/Rhigh/Olow/Rlow
        fi
        rdelta=$(( rhigh - rlow ))
        odelta=$(( ohigh - olow ))
        (( rdelta > 0.0 && odelta > 0.0 )) || \
            die "$name rep $rep produced a non-positive delta (Rosetta=$rdelta, Ocerz=$odelta)"
        ratio=$(( odelta / rdelta ))
        rosetta_deltas+=($rdelta)
        ocerz_deltas+=($odelta)
        paired_ratios+=($ratio)
        printf '  rep %-2d order=%-25s Rdelta=%.6fs Odelta=%.6fs ratio=%.4fx\n' \
            $rep $order $rdelta $odelta $ratio
    done

    median $rosetta_deltas; rdelta=$REPLY
    median $ocerz_deltas; odelta=$REPLY
    median $paired_ratios; ratio=$REPLY
    record_result $name paired-delta $rdelta $odelta $ratio
    print
}

for workload in $WORKLOADS; do
    case $workload in
    call) run_delta call "$FIBN" 38 42 ;;
    alu)  run_direct alu "$BENCH" alu ;;
    mem)  run_delta mem "$MEMN" 40000000 400000000 ;;
    br)   run_direct br "$BENCH" br ;;
    esac
done

[[ $(sha256 "$OCERZ") == $OCERZ_SHA ]] || die "Ocerz binary changed during the benchmark"
[[ $(sha256 "$BENCH") == $BENCH_SHA ]] || die "bench binary changed during the benchmark"
[[ $(sha256 "$FIBN") == $FIBN_SHA ]] || die "fibn binary changed during the benchmark"
[[ $(sha256 "$MEMN") == $MEMN_SHA ]] || die "memn binary changed during the benchmark"

print "Summary"
printf '%-8s %-13s %12s %12s %10s %6s\n' workload metric Rosetta_s Ocerz_s ratio status
printf '%-8s %-13s %12s %12s %10s %6s\n' -------- ------------- ------------ ------------ ---------- ------

integer failures=0 i
for (( i = 1; i <= ${#RESULT_NAMES}; i++ )); do
    printf '%-8s %-13s %12.6f %12.6f %9.4fx %6s\n' \
        ${RESULT_NAMES[i]} ${RESULT_MODES[i]} ${RESULT_ROSETTA[i]} \
        ${RESULT_OCERZ[i]} ${RESULT_RATIOS[i]} ${RESULT_STATUS[i]}
    [[ ${RESULT_STATUS[i]} == PASS ]] || (( failures++ ))
done

print
if (( failures )); then
    print "FAIL: $failures workload(s) did not satisfy Ocerz/Rosetta < $PASS_RATIO"
    exit 1
fi
print "PASS: every workload satisfied Ocerz/Rosetta < $PASS_RATIO"
exit 0
