#!/bin/bash
#
# ParallelCrack — Lotus smoke test (4 processes, one node, 5 minutes).
#
# This is a SAFETY-FIRST sanity check, not a benchmark. It runs one collective
# and one asynchronous full-sweep on a small deterministic keyspace and verifies
# that each evaluates the whole keyspace. It submits no other jobs, spawns nothing
# in the background, and never loops over process counts.
#
# Submit it yourself (this file never submits itself):
#     sbatch benchmarks/lotus_smoke.sbatch
#
# Inspect it first:
#     cat benchmarks/lotus_smoke.sbatch
#
#SBATCH --job-name=parallelcrack_smoke
#SBATCH --partition=compute
#SBATCH --nodes=1
#SBATCH --ntasks=4
#SBATCH --ntasks-per-node=4
#SBATCH --time=00:05:00
#SBATCH --output=results/smoke-%j.out
#SBATCH --error=results/smoke-%j.err
#
# NOTE on resources: --partition=compute and 48 cores/node come from the COMP 380
# Lab 2/Lab 3 batch files. This smoke test deliberately uses only 1 node and 4
# tasks. Confirm the partition name and core count with `sinfo` on Lotus before
# submitting; adjust the two lines above if your cluster differs. No account or
# GPU is requested.

set -u

# Refuse to run heavy work on a login node: srun needs a real allocation.
if [ -z "${SLURM_JOB_ID:-}" ]; then
    echo "This script must be run via sbatch (inside a SLURM allocation), not directly."
    echo "Submit it with: sbatch benchmarks/lotus_smoke.sbatch"
    exit 2
fi

module load OpenSSL/3

mkdir -p results

# Build (idempotent). A failed build is a failed smoke test.
make || { echo "BUILD FAILED"; exit 1; }

# Small, fast, deterministic keyspace: lower-case, length 4 => 26^4 = 456976.
ALPHABET="lower"
LENGTH=4
EXPECT_KEYSPACE=456976
fail=0

echo "=============================================="
echo " ParallelCrack smoke test"
echo "  job id   : ${SLURM_JOB_ID}"
echo "  nodes    : ${SLURM_NNODES}   tasks: ${SLURM_NTASKS}"
echo "  node list: ${SLURM_JOB_NODELIST:-unknown}"
echo "  workload : --alphabet ${ALPHABET} --length ${LENGTH} --full-sweep"
echo "  launcher : srun --mpi=pmix -n 4"
echo "=============================================="

# Run one strategy and verify its full sweep evaluates the whole keyspace.
run_one() {
    strategy="$1"
    echo "-- ${strategy}: full sweep (expect candidates=${EXPECT_KEYSPACE}) --"
    out="$(srun --mpi=pmix -n 4 ./parallelcrack \
             --strategy "${strategy}" --alphabet "${ALPHABET}" --length "${LENGTH}" \
             --full-sweep --quiet)"
    rc=$?
    echo "   output : ${out}"
    echo "   srun exit code: ${rc}"
    if [ ${rc} -ne 0 ]; then
        echo "   FAIL: srun returned non-zero"
        fail=1
        return
    fi
    got="$(echo "${out}" | grep -o 'candidates=[0-9]*' | cut -d= -f2)"
    if [ "${got}" != "${EXPECT_KEYSPACE}" ]; then
        echo "   FAIL: candidates=${got}, expected ${EXPECT_KEYSPACE}"
        fail=1
    else
        echo "   ok: full sweep evaluated the whole keyspace"
    fi
}

run_one collective
run_one async

echo "=============================================="
if [ ${fail} -ne 0 ]; then
    echo " SMOKE TEST FAILED"
    exit 1
fi
echo " SMOKE TEST PASSED"
exit 0
