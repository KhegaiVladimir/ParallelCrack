#!/bin/bash
#
# ParallelCrack — Lotus single-node benchmark (1..48 processes).
#
# DO NOT run this until lotus_smoke.sbatch and the full-sweep correctness checks
# pass. It runs a strong-scaling study (fixed global workload) comparing the
# collective and asynchronous strategies on identical inputs, with a separate
# full-sweep experiment and early-termination experiment.
#
# It is deliberately conservative: one node, at most 48 tasks, runs strictly
# sequentially, submits no other jobs, spawns nothing in the background, has no
# retry loops, and requests no GPUs. If any correctness check fails it stops with
# a non-zero exit code.
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
PER_RUN_MINUTES="${PER_RUN_MINUTES:-2}"  # per-step SLURM time limit
DRYRUN="${DRYRUN:-0}"              # 1 = print planned commands, run nothing

STRATEGIES="collective async"
EXPERIMENTS="fullsweep early"

# ---------------------------------------------------------------------------
# Derived values.
# ---------------------------------------------------------------------------
case "${ALPHABET}" in
    lower)     ALPHA_SIZE=26 ;;
    alnum)     ALPHA_SIZE=62 ;;
    printable) ALPHA_SIZE=95 ;;
    *) echo "Unknown alphabet: ${ALPHABET}"; exit 2 ;;
esac

# keyspace = ALPHA_SIZE ^ LENGTH (fixed global workload), early target = middle.
KEYSPACE=1
i=0
while [ ${i} -lt ${LENGTH} ]; do
    KEYSPACE=$((KEYSPACE * ALPHA_SIZE))
    i=$((i + 1))
done
EARLY_INDEX=$((KEYSPACE / 2))

# Count the process-count list and the planned number of MPI launches.
NUM_PROCS=0
for _p in ${PROCS}; do NUM_PROCS=$((NUM_PROCS + 1)); done
NUM_STRAT=0
for _s in ${STRATEGIES}; do NUM_STRAT=$((NUM_STRAT + 1)); done
NUM_EXP=0
for _e in ${EXPERIMENTS}; do NUM_EXP=$((NUM_EXP + 1)); done
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

# Build arguments for one experiment type.
experiment_args() {
    case "$1" in
        fullsweep) echo "--full-sweep" ;;
        early)     echo "--index ${EARLY_INDEX}" ;;
    esac
}

csv_for() {
    case "$1" in
        fullsweep) echo "${CSV_FULLSWEEP}" ;;
        early)     echo "${CSV_EARLY}" ;;
    esac
}

# Validate one run's output against the experiment's expectation.
# Returns 0 if correct, 1 if not.
check_output() {
    exp="$1"
    out="$2"
    if [ "${exp}" = "fullsweep" ]; then
        got="$(echo "${out}" | grep -o 'candidates=[0-9]*' | cut -d= -f2)"
        [ "${got}" = "${KEYSPACE}" ]
    else
        f="$(echo "${out}" | grep -o 'found=[0-9]*' | cut -d= -f2)"
        [ "${f}" = "1" ]
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
                         "--batch ${BATCH} --poll ${POLL} ${eargs} --csv ${csv} --quiet"
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
echo "Wrote environment metadata to ${META}"

launches_done=0
correctness_failures=0

for exp in ${EXPERIMENTS}; do
    eargs="$(experiment_args "${exp}")"
    csv="$(csv_for "${exp}")"
    echo
    echo "### experiment: ${exp}  (csv: ${csv}) ###"
    for strat in ${STRATEGIES}; do
        for np in ${PROCS}; do
            rep=1
            while [ ${rep} -le ${REPEATS} ]; do
                echo "[run] exp=${exp} strat=${strat} np=${np} rep=${rep}/${REPEATS}"
                out="$(srun --mpi=pmix --time=${PER_RUN_MINUTES} -n ${np} ./parallelcrack \
                        --strategy ${strat} --alphabet ${ALPHABET} --length ${LENGTH} \
                        --batch ${BATCH} --poll ${POLL} ${eargs} --csv ${csv} --quiet)"
                rc=$?
                launches_done=$((launches_done + 1))
                echo "      exit=${rc} :: ${out}"

                if [ ${rc} -ne 0 ]; then
                    echo "      CORRECTNESS/RUN FAILURE: srun exit ${rc}. Stopping benchmark."
                    correctness_failures=1
                    break 3
                fi
                if ! check_output "${exp}" "${out}"; then
                    echo "      CORRECTNESS FAILURE: unexpected result for ${exp}. Stopping benchmark."
                    correctness_failures=1
                    break 3
                fi
                rep=$((rep + 1))
            done
        done
    done
done

echo
echo "launches executed: ${launches_done} of ${PLANNED_LAUNCHES} planned"

# Record the correctness outcome into the metadata.
if [ ${correctness_failures} -ne 0 ]; then
    echo "correctness_validation=FAILED" >> "${META}"
else
    echo "correctness_validation=PASSED" >> "${META}"
fi

# Archive results for transfer off the cluster (CSVs, logs, metadata).
ARCHIVE="results/parallelcrack_results_${JOB}.tar.gz"
tar czf "${ARCHIVE}" \
    "${META}" \
    ${CSV_FULLSWEEP} ${CSV_EARLY} \
    results/bench-${JOB}.out results/bench-${JOB}.err 2>/dev/null
echo "Wrote results archive: ${ARCHIVE}"
echo "Copy it to your Mac with, e.g.:"
echo "  scp <user>@lotus:\$(pwd)/${ARCHIVE} ./"

if [ ${correctness_failures} -ne 0 ]; then
    echo "BENCHMARK ABORTED ON CORRECTNESS FAILURE"
    exit 1
fi
echo "BENCHMARK COMPLETE"
exit 0
