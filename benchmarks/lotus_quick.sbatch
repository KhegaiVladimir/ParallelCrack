#!/bin/bash
#
# ParallelCrack — fast Lotus benchmark (1,2,4,8 processes, ~10 minutes).
#
# A small, quick strong-scaling run for producing portfolio charts. Same hardened
# logic as lotus_benchmark.sbatch (stop-on-failure, config validation, CSV gating,
# correctness checks) but with a smaller plan: 8 allocated tasks, 10-minute limit,
# process counts 1/2/4/8, 3 repetitions. Default plan = 2 experiments x 2
# strategies x 4 process counts x 3 repeats = 48 MPI launches.
#
# Safety: one node, at most 8 tasks, strictly sequential, no background work, no
# nested job submission, no retries, no GPUs. Any failed run or correctness check
# stops ALL remaining work and the script exits non-zero. Partial results are
# preserved for debugging but never labelled as a completed benchmark.
#
# Inspect first:        cat benchmarks/lotus_quick.sbatch
# Preview (no compute): DRYRUN=1 bash benchmarks/lotus_quick.sbatch
# Submit (you do this): sbatch benchmarks/lotus_quick.sbatch
#
#SBATCH --job-name=parallelcrack_quick
#SBATCH --partition=compute
#SBATCH --nodes=1
#SBATCH --ntasks=8
#SBATCH --ntasks-per-node=8
#SBATCH --time=00:10:00
#SBATCH --output=results/quick-%j.out
#SBATCH --error=results/quick-%j.err
#
# NOTE on resources: --partition=compute comes from the COMP 380 Lab 2/Lab 3 batch
# files. This uses ONE node and only 8 tasks. Confirm the partition name with
# `sinfo` before submitting. No account and no GPU are requested.

set -u

# ---------------------------------------------------------------------------
# Configuration (override from the environment before submitting if needed).
# ---------------------------------------------------------------------------
PROCS="${PROCS:-1 2 4 8}"           # quick single-node process counts
REPEATS="${REPEATS:-3}"             # repetitions per configuration
LENGTH="${LENGTH:-5}"               # candidate length (deterministic workload)
ALPHABET="${ALPHABET:-lower}"       # lower | alnum | printable
BATCH="${BATCH:-2000}"             # batch size (identical across strategies)
POLL="${POLL:-1}"                  # async polling interval (batches)
PER_RUN_MINUTES="${PER_RUN_MINUTES:-2}"  # per-step SLURM time limit (minutes)
DRYRUN="${DRYRUN:-0}"              # 1 = print planned commands, run nothing

STRATEGIES="collective async"
EXPERIMENTS="fullsweep early"

MAX_TASKS=8           # this quick job allocates 8 tasks
MAX_REPEATS=100       # sanity bound
MAX_LENGTH=12         # SEARCH_MAX_LENGTH in the application
MAX_PER_RUN_MINUTES=10
LONG_MAX=9223372036854775807   # 64-bit signed max, for overflow checks

# ---------------------------------------------------------------------------
# Small helpers.
# ---------------------------------------------------------------------------
die() { echo "CONFIG ERROR: $*" >&2; exit 2; }

is_pos_int() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
    esac
    [ "$1" -gt 0 ]
}

# ---------------------------------------------------------------------------
# Validate every configuration value BEFORE computing anything or launching srun.
# ---------------------------------------------------------------------------
case "${ALPHABET}" in
    lower)     ALPHA_SIZE=26 ;;
    alnum)     ALPHA_SIZE=62 ;;
    printable) ALPHA_SIZE=95 ;;
    *) die "ALPHABET must be lower|alnum|printable (got '${ALPHABET}')" ;;
esac

is_pos_int "${LENGTH}"          || die "LENGTH must be a positive integer (got '${LENGTH}')"
[ "${LENGTH}" -le ${MAX_LENGTH} ] || die "LENGTH must be 1..${MAX_LENGTH} (got ${LENGTH})"
is_pos_int "${REPEATS}"         || die "REPEATS must be a positive integer (got '${REPEATS}')"
[ "${REPEATS}" -le ${MAX_REPEATS} ] || die "REPEATS must be 1..${MAX_REPEATS} (got ${REPEATS})"
is_pos_int "${BATCH}"           || die "BATCH must be a positive integer (got '${BATCH}')"
is_pos_int "${POLL}"            || die "POLL must be a positive integer (got '${POLL}')"
is_pos_int "${PER_RUN_MINUTES}" || die "PER_RUN_MINUTES must be a positive integer"
[ "${PER_RUN_MINUTES}" -le ${MAX_PER_RUN_MINUTES} ] || \
    die "PER_RUN_MINUTES must be 1..${MAX_PER_RUN_MINUTES} (got ${PER_RUN_MINUTES})"

[ -n "${PROCS// /}" ] || die "PROCS is empty"
TASK_CAP=${MAX_TASKS}
if [ -n "${SLURM_NTASKS:-}" ] && [ "${SLURM_NTASKS}" -lt ${TASK_CAP} ]; then
    TASK_CAP=${SLURM_NTASKS}
fi
for p in ${PROCS}; do
    is_pos_int "${p}"       || die "PROCS entry '${p}' is not a positive integer"
    [ "${p}" -le ${MAX_TASKS} ] || die "PROCS entry '${p}' exceeds this quick job's limit of ${MAX_TASKS}"
    [ "${p}" -le ${TASK_CAP} ]  || die "PROCS entry '${p}' exceeds allocated tasks (${TASK_CAP})"
done

# keyspace = ALPHA_SIZE ^ LENGTH, with overflow detection at each multiply.
KEYSPACE=1
i=0
while [ ${i} -lt ${LENGTH} ]; do
    if [ "${KEYSPACE}" -gt $((LONG_MAX / ALPHA_SIZE)) ]; then
        die "keyspace (${ALPHA_SIZE}^${LENGTH}) overflows 64-bit integer; reduce LENGTH or ALPHABET"
    fi
    KEYSPACE=$((KEYSPACE * ALPHA_SIZE))
    i=$((i + 1))
done
EARLY_INDEX=$((KEYSPACE / 2))

NUM_PROCS=0; for _p in ${PROCS}; do NUM_PROCS=$((NUM_PROCS + 1)); done
NUM_STRAT=0; for _s in ${STRATEGIES}; do NUM_STRAT=$((NUM_STRAT + 1)); done
NUM_EXP=0;   for _e in ${EXPERIMENTS}; do NUM_EXP=$((NUM_EXP + 1)); done
PLANNED_LAUNCHES=$((NUM_EXP * NUM_STRAT * NUM_PROCS * REPEATS))

echo "=================================================================="
echo " ParallelCrack QUICK benchmark plan"
echo "  experiments : ${EXPERIMENTS}"
echo "  strategies  : ${STRATEGIES}"
echo "  process set : ${PROCS}"
echo "  repetitions : ${REPEATS}"
echo "  workload    : --alphabet ${ALPHABET} --length ${LENGTH} --batch ${BATCH} --poll ${POLL}"
echo "  keyspace    : ${KEYSPACE} (full-sweep target = unmatchable)"
echo "  early index : ${EARLY_INDEX} (early-termination target)"
echo "  launcher    : srun --mpi=pmix --time=${PER_RUN_MINUTES}"
echo "  PLANNED MPI LAUNCHES: ${PLANNED_LAUNCHES}"
echo "=================================================================="

if [ "${DRYRUN}" != "1" ] && [ -z "${SLURM_JOB_ID:-}" ]; then
    echo "Refusing to run outside a SLURM allocation (this would run on a login node)."
    echo "Submit with: sbatch benchmarks/lotus_quick.sbatch"
    echo "Or preview without computing: DRYRUN=1 bash benchmarks/lotus_quick.sbatch"
    exit 2
fi

mkdir -p results
JOB="${SLURM_JOB_ID:-dryrun}"
CSV_FULLSWEEP="results/quick_fullsweep_${JOB}.csv"
CSV_EARLY="results/quick_early_${JOB}.csv"
META="results/quick_meta_${JOB}.txt"
RUNLOG="results/quick_run_${JOB}.log"
RECOVER_BASE="results/.quick_recover_${JOB}"
ROW_TMP="results/.quick_row_${JOB}.csv"
: > "${RUNLOG}"

log() { printf '%s\n' "$*"; printf '%s\n' "$*" >> "${RUNLOG}"; }

experiment_args() {
    case "$1" in
        fullsweep) echo "--full-sweep" ;;
        early)     echo "--index ${EARLY_INDEX} --recover-file ${RECOVER_BASE}" ;;
    esac
}

csv_for() {
    case "$1" in
        fullsweep) echo "${CSV_FULLSWEEP}" ;;
        early)     echo "${CSV_EARLY}" ;;
    esac
}

field() { echo "$1" | grep -o "$2=[0-9-]*" | head -1 | cut -d= -f2; }

check_output() {
    exp="$1"; out="$2"; np="$3"
    found="$(field "${out}" found)"
    if [ "${exp}" = "fullsweep" ]; then
        cand="$(field "${out}" candidates)"
        [ "${cand}" = "${KEYSPACE}" ] && [ "${found}" = "0" ]
    else
        fidx="$(field "${out}" found_index)"
        fr="$(field "${out}" finder_rank)"
        rec="$(field "${out}" recovery)"
        expected_owner=$((EARLY_INDEX % np))
        [ "${found}" = "1" ] && [ "${fidx}" = "${EARLY_INDEX}" ] && \
        [ "${fr}" = "${expected_owner}" ] && [ "${rec}" = "1" ]
    fi
}

if [ "${DRYRUN}" = "1" ]; then
    echo
    echo "DRY RUN — the following ${PLANNED_LAUNCHES} commands WOULD run (nothing is executed):"
    for exp in ${EXPERIMENTS}; do
        eargs="$(experiment_args "${exp}")"
        for strat in ${STRATEGIES}; do
            for np in ${PROCS}; do
                rep=1
                while [ ${rep} -le ${REPEATS} ]; do
                    echo "srun --mpi=pmix --time=${PER_RUN_MINUTES} -n ${np} ./parallelcrack" \
                         "--strategy ${strat} --alphabet ${ALPHABET} --length ${LENGTH}" \
                         "--batch ${BATCH} --poll ${POLL} ${eargs} --csv <validated> --quiet"
                    rep=$((rep + 1))
                done
            done
        done
    done
    echo
    echo "DRY RUN complete. Re-run with sbatch (no DRYRUN) to execute."
    exit 0
fi

module load OpenSSL/3
make || { echo "BUILD FAILED"; exit 1; }

{
    echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "git_commit=$(git rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "git_describe=$(git describe --tags --always --dirty 2>/dev/null || echo unknown)"
    echo "mpicc_version=$(mpicc --version 2>&1 | head -1)"
    echo "srun_version=$(srun --version 2>&1 | head -1)"
    echo "openssl_version=$(openssl version 2>&1 | head -1)"
    echo "slurm_job_id=${SLURM_JOB_ID:-none}"
    echo "slurm_nnodes=${SLURM_NNODES:-?}"
    echo "slurm_ntasks=${SLURM_NTASKS:-?}"
    echo "slurm_nodelist=${SLURM_JOB_NODELIST:-?}"
    echo "cpu_model=$(lscpu 2>/dev/null | grep -i 'model name' | head -1 | cut -d: -f2 | sed 's/^ *//')"
    echo "cpu_sockets=$(lscpu 2>/dev/null | grep -i '^Socket' | head -1 | awk '{print $2}')"
    echo "cpu_cores_per_socket=$(lscpu 2>/dev/null | grep -i 'Core(s) per socket' | awk '{print $4}')"
    echo "experiments=${EXPERIMENTS}"
    echo "strategies=${STRATEGIES}"
    echo "procs=${PROCS}"
    echo "repeats=${REPEATS}"
    echo "length=${LENGTH} alphabet=${ALPHABET} batch=${BATCH} poll=${POLL}"
    echo "keyspace=${KEYSPACE} early_index=${EARLY_INDEX}"
    echo "planned_launches=${PLANNED_LAUNCHES}"
} > "${META}"
log "Wrote environment metadata to ${META}"

launches_done=0

run_one() {
    exp="$1"; strat="$2"; np="$3"; realcsv="$4"
    eargs="$(experiment_args "${exp}")"

    rm -f "${ROW_TMP}"
    out="$(srun --mpi=pmix --time=${PER_RUN_MINUTES} -n ${np} ./parallelcrack \
            --strategy ${strat} --alphabet ${ALPHABET} --length ${LENGTH} \
            --batch ${BATCH} --poll ${POLL} ${eargs} --csv "${ROW_TMP}" --quiet)"
    rc=$?
    log "      exit=${rc} :: ${out}"

    if [ ${rc} -ne 0 ]; then
        log "      RUN FAILURE: srun exit ${rc}"
        return 1
    fi
    if ! check_output "${exp}" "${out}" "${np}"; then
        log "      CORRECTNESS FAILURE: unexpected result for ${exp} (np=${np})"
        return 1
    fi

    if [ -s "${ROW_TMP}" ]; then
        if [ ! -s "${realcsv}" ]; then
            head -1 "${ROW_TMP}" > "${realcsv}"
        fi
        tail -n +2 "${ROW_TMP}" >> "${realcsv}"
    fi
    rm -f "${ROW_TMP}"
    return 0
}

run_all() {
    for exp in ${EXPERIMENTS}; do
        csv="$(csv_for "${exp}")"
        log ""
        log "### experiment: ${exp}  (csv: ${csv}) ###"
        for strat in ${STRATEGIES}; do
            for np in ${PROCS}; do
                rep=1
                while [ ${rep} -le ${REPEATS} ]; do
                    log "[run] exp=${exp} strat=${strat} np=${np} rep=${rep}/${REPEATS}"
                    launches_done=$((launches_done + 1))
                    if ! run_one "${exp}" "${strat}" "${np}" "${csv}"; then
                        return 1
                    fi
                    rep=$((rep + 1))
                done
            done
        done
    done
    return 0
}

run_all
status=$?

log ""
log "launches executed: ${launches_done} of ${PLANNED_LAUNCHES} planned"

rm -f "${RECOVER_BASE}".plain "${RECOVER_BASE}".enc "${RECOVER_BASE}".recovered "${ROW_TMP}" 2>/dev/null

if [ ${status} -ne 0 ]; then
    echo "correctness_validation=FAILED" >> "${META}"
    log "benchmark_status=ABORTED"
else
    echo "correctness_validation=PASSED" >> "${META}"
    log "benchmark_status=COMPLETE"
fi

ARCHIVE="results/parallelcrack_quick_${JOB}.tar.gz"
archive_list="${META} ${RUNLOG}"
[ -f "${CSV_FULLSWEEP}" ] && archive_list="${archive_list} ${CSV_FULLSWEEP}"
[ -f "${CSV_EARLY}" ]     && archive_list="${archive_list} ${CSV_EARLY}"
tar czf "${ARCHIVE}" ${archive_list}
log "Wrote results archive (CSVs + metadata + run log): ${ARCHIVE}"
log "After this job EXITS, also collect the SLURM logs:"
log "  cp results/quick-${JOB}.out results/quick-${JOB}.err .   # complete once the job has exited"
log "Copy the archive to your Mac with, e.g.:"
log "  scp <user>@lotus:\$(pwd)/${ARCHIVE} ./"

if [ ${status} -ne 0 ]; then
    log "QUICK BENCHMARK ABORTED ON FAILURE — partial results preserved (NOT a completed benchmark)"
    exit 1
fi
log "QUICK BENCHMARK COMPLETE"
exit 0
