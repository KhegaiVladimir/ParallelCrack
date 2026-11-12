#!/bin/bash
#
# ParallelCrack — Lotus single-node benchmark (1..48 processes).
#
# DO NOT run this until lotus_smoke.sbatch and the full-sweep correctness checks
# pass. It runs a strong-scaling study (fixed global workload) comparing the
# collective and asynchronous strategies on identical inputs, with a separate
# full-sweep experiment and early-termination experiment.
#
# Safety: one node, at most 48 tasks, strictly sequential, no background work, no
# nested job submission, no retries, no GPUs. Any failed run or correctness check
# stops ALL remaining work and the script exits non-zero. Partial results are
# preserved for debugging but never labelled as a completed benchmark.
#
# Inspect first:        cat benchmarks/lotus_benchmark.sbatch
# Preview (no compute): DRYRUN=1 bash benchmarks/lotus_benchmark.sbatch
# Submit (you do this): sbatch benchmarks/lotus_benchmark.sbatch
#
#SBATCH --job-name=parallelcrack_bench
#SBATCH --partition=compute
#SBATCH --nodes=1
#SBATCH --ntasks=48
#SBATCH --ntasks-per-node=48
#SBATCH --time=00:30:00
#SBATCH --output=results/bench-%j.out
#SBATCH --error=results/bench-%j.err
#
# NOTE on resources: --partition=compute and 48 cores/node come from the COMP 380
# Lab 2/Lab 3 batch files. This uses ONE node only. Confirm the partition name and
# that nodes really have 48 usable cores with `sinfo`/`scontrol show node` before
# submitting. No account and no GPU are requested. This is NOT the professor's
# 35-node / 1680-task configuration and must not be scaled up to it.

set -u

# ---------------------------------------------------------------------------
# Configuration (override any of these from the environment before submitting).
# To shrink the run, reduce PROCS, e.g.  PROCS="1 2 4"  or  REPEATS=3.
# ---------------------------------------------------------------------------
PROCS="${PROCS:-1 2 4 8 16 32 48}"   # single-node process counts only
REPEATS="${REPEATS:-5}"              # repetitions per configuration
LENGTH="${LENGTH:-5}"               # candidate length (fixed global workload)
ALPHABET="${ALPHABET:-lower}"       # lower | alnum | printable
BATCH="${BATCH:-2000}"             # batch size (identical across strategies)
POLL="${POLL:-1}"                  # async polling interval (batches)
PER_RUN_MINUTES="${PER_RUN_MINUTES:-2}"  # per-step SLURM time limit (minutes)
DRYRUN="${DRYRUN:-0}"              # 1 = print planned commands, run nothing

STRATEGIES="collective async"
EXPERIMENTS="fullsweep early"

MAX_TASKS=48          # single-node policy cap
MAX_REPEATS=100       # sanity bound
MAX_LENGTH=12         # SEARCH_MAX_LENGTH in the application
MAX_PER_RUN_MINUTES=30
LONG_MAX=9223372036854775807   # 64-bit signed max, for overflow checks

# ---------------------------------------------------------------------------
# Small helpers.
# ---------------------------------------------------------------------------
die() { echo "CONFIG ERROR: $*" >&2; exit 2; }

is_pos_int() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;   # empty or contains a non-digit
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

# PROCS: non-empty, each a positive integer, each <= the single-node cap and (when
# known) <= the tasks actually allocated to this job.
[ -n "${PROCS// /}" ] || die "PROCS is empty"
TASK_CAP=${MAX_TASKS}
if [ -n "${SLURM_NTASKS:-}" ] && [ "${SLURM_NTASKS}" -lt ${TASK_CAP} ]; then
    TASK_CAP=${SLURM_NTASKS}
fi
for p in ${PROCS}; do
    is_pos_int "${p}"       || die "PROCS entry '${p}' is not a positive integer"
    [ "${p}" -le ${MAX_TASKS} ] || die "PROCS entry '${p}' exceeds the single-node limit of ${MAX_TASKS}"
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
EARLY_INDEX=$((KEYSPACE / 2))   # fixed, reproducible early-termination target

# Count the process-count list and the planned number of MPI launches.
NUM_PROCS=0; for _p in ${PROCS}; do NUM_PROCS=$((NUM_PROCS + 1)); done
NUM_STRAT=0; for _s in ${STRATEGIES}; do NUM_STRAT=$((NUM_STRAT + 1)); done
NUM_EXP=0;   for _e in ${EXPERIMENTS}; do NUM_EXP=$((NUM_EXP + 1)); done
PLANNED_LAUNCHES=$((NUM_EXP * NUM_STRAT * NUM_PROCS * REPEATS))

echo "=================================================================="
echo " ParallelCrack single-node benchmark plan"
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

# Refuse to run heavy work on a login node (srun needs a real allocation).
if [ "${DRYRUN}" != "1" ] && [ -z "${SLURM_JOB_ID:-}" ]; then
    echo "Refusing to run outside a SLURM allocation (this would run on a login node)."
    echo "Submit with: sbatch benchmarks/lotus_benchmark.sbatch"
    echo "Or preview without computing: DRYRUN=1 bash benchmarks/lotus_benchmark.sbatch"
    exit 2
fi

mkdir -p results
JOB="${SLURM_JOB_ID:-dryrun}"
CSV_FULLSWEEP="results/fullsweep_${JOB}.csv"
CSV_EARLY="results/early_${JOB}.csv"
META="results/meta_${JOB}.txt"
RUNLOG="results/run_${JOB}.log"
RECOVER_BASE="results/.recover_${JOB}"   # early-termination file-recovery check
ROW_TMP="results/.row_${JOB}.csv"        # per-run CSV, gated into the real CSV
: > "${RUNLOG}"

# log to BOTH the SLURM stdout and our own complete run log (which we archive).
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

# Pull one "key=value" field out of a program output line.
field() { echo "$1" | grep -o "$2=[0-9-]*" | head -1 | cut -d= -f2; }

# Validate one run's output. $1=experiment $2=output $3=nprocs. Returns 0 if correct.
check_output() {
    exp="$1"; out="$2"; np="$3"
    found="$(field "${out}" found)"
    if [ "${exp}" = "fullsweep" ]; then
        # Must scan the ENTIRE keyspace and find nothing (deterministic workload).
        cand="$(field "${out}" candidates)"
        [ "${cand}" = "${KEYSPACE}" ] && [ "${found}" = "0" ]
    else
        # Must find the exact target index, at its owning rank, and the recovered
        # file must verify.
        fidx="$(field "${out}" found_index)"
        fr="$(field "${out}" finder_rank)"
        rec="$(field "${out}" recovery)"
        expected_owner=$((EARLY_INDEX % np))
        [ "${found}" = "1" ] && [ "${fidx}" = "${EARLY_INDEX}" ] && \
        [ "${fr}" = "${expected_owner}" ] && [ "${rec}" = "1" ]
    fi
}

# ---------------------------------------------------------------------------
# Dry run: print every planned command and exit without computing anything.
# ---------------------------------------------------------------------------
if [ "${DRYRUN}" = "1" ]; then
    echo
    echo "DRY RUN — the following ${PLANNED_LAUNCHES} commands WOULD run (nothing is executed):"
    for exp in ${EXPERIMENTS}; do
        eargs="$(experiment_args "${exp}")"
        csv="$(csv_for "${exp}")"
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

# ---------------------------------------------------------------------------
# Real run.
# ---------------------------------------------------------------------------
module load OpenSSL/3
make || { echo "BUILD FAILED"; exit 1; }

# Record the exact environment so results are reproducible and attributable.
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

# Run one configuration once: launch, validate, and only then commit its CSV row.
# Returns non-zero on any run failure or correctness failure.
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

    # Commit the validated row: write the header once, then append the data row.
    # Failed runs never reach this point, so the CSV holds validated rows only.
    if [ -s "${ROW_TMP}" ]; then
        if [ ! -s "${realcsv}" ]; then
            head -1 "${ROW_TMP}" > "${realcsv}"
        fi
        tail -n +2 "${ROW_TMP}" >> "${realcsv}"
    fi
    rm -f "${ROW_TMP}"
    return 0
}

# Run everything. A single `return 1` here stops ALL loops immediately -- there is
# no multi-level `break` to miscount.
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

# Clean up transient recovery files from the early-termination experiment.
rm -f "${RECOVER_BASE}".plain "${RECOVER_BASE}".enc "${RECOVER_BASE}".recovered "${ROW_TMP}" 2>/dev/null

if [ ${status} -ne 0 ]; then
    echo "correctness_validation=FAILED" >> "${META}"
    log "benchmark_status=ABORTED"
else
    echo "correctness_validation=PASSED" >> "${META}"
    log "benchmark_status=COMPLETE"
fi

# Archive the files THIS script fully controls and has finished writing: the
# complete run log, the metadata, and the validated CSVs. The live SLURM
# bench-<job>.out/.err are still open while this runs, so they are intentionally
# NOT put in the archive here -- collect them separately after the job exits (see
# the printed instructions below).
ARCHIVE="results/parallelcrack_results_${JOB}.tar.gz"
archive_list="${META} ${RUNLOG}"
[ -f "${CSV_FULLSWEEP}" ] && archive_list="${archive_list} ${CSV_FULLSWEEP}"
[ -f "${CSV_EARLY}" ]     && archive_list="${archive_list} ${CSV_EARLY}"
tar czf "${ARCHIVE}" ${archive_list}
log "Wrote results archive (CSVs + metadata + run log): ${ARCHIVE}"
log "After this job EXITS, also collect the SLURM logs and (optionally) fold them in:"
log "  tar rf \"${ARCHIVE%.gz}\" results/bench-${JOB}.out results/bench-${JOB}.err  # or keep them alongside"
log "Copy the archive to your Mac with, e.g.:"
log "  scp <user>@lotus:\$(pwd)/${ARCHIVE} ./"

if [ ${status} -ne 0 ]; then
    log "BENCHMARK ABORTED ON FAILURE — partial results preserved for debugging (NOT a completed benchmark)"
    exit 1
fi
log "BENCHMARK COMPLETE"
exit 0
