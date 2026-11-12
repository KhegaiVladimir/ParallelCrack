#!/usr/bin/env bash
#
# Integration tests for ParallelCrack.
#
# These run the actual MPI program with mpirun and check the one-line (--quiet)
# output for each required scenario, for BOTH strategies. They use small process
# counts and a small keyspace so the whole suite finishes in seconds.
#
# Override the launcher if needed, e.g. on a cluster:
#     MPIRUN="srun" PROG=./parallelcrack bash tests/run_tests.sh
#
# By default we use mpirun with --oversubscribe so the suite runs on a laptop with
# fewer cores than the process counts it tests.

set -u

MPIRUN="${MPIRUN:-mpirun --oversubscribe}"
PROG="${PROG:-./parallelcrack}"

pass=0
fail=0

# Pull "key=value" out of the program's --quiet output line.
field() {
    # $1 = output text, $2 = key
    echo "$1" | grep -o "$2=[^ ]*" | head -1 | cut -d= -f2
}

# run_case "<description>" "<expect_key>" "<expect_value>" <np> <args...>
run_case() {
    local desc="$1"; shift
    local key="$1"; shift
    local want="$1"; shift
    local np="$1"; shift

    local out
    out="$($MPIRUN -np "$np" "$PROG" "$@" --quiet 2>&1)"
    local rc=$?
    local got
    got="$(field "$out" "$key")"

    if [ $rc -ne 0 ]; then
        echo "FAIL [$desc]: launcher returned rc=$rc"
        echo "      output: $out"
        fail=$((fail + 1))
    elif [ "$got" != "$want" ]; then
        echo "FAIL [$desc]: expected $key=$want but got $key=$got"
        echo "      output: $out"
        fail=$((fail + 1))
    else
        echo "ok   [$desc] ($key=$got, np=$np)"
        pass=$((pass + 1))
    fi
}

echo "== ParallelCrack integration tests =="
echo "   launcher: $MPIRUN"
echo "   program : $PROG"
echo

for strat in collective async; do
    echo "-- strategy: $strat --"

    # Found, across process counts (single / two / four processes).
    run_case "$strat: 1 proc finds target"  found 1 1 --strategy $strat --length 4
    run_case "$strat: 2 procs find target"  found 1 2 --strategy $strat --length 4
    run_case "$strat: 4 procs find target"  found 1 4 --strategy $strat --length 4

    # Target found by rank 0 (index 0 is owned by rank 0 under cyclic distribution).
    run_case "$strat: found by rank 0" finder_rank 0 4 --strategy $strat --length 4 --index 0

    # Target found by a worker (index 3 with 4 ranks is owned by rank 3).
    run_case "$strat: found by a worker" finder_rank 3 4 --strategy $strat --length 4 --index 3

    # Target not found: the whole keyspace (26^4 = 456976) is scanned.
    run_case "$strat: no match (found flag)" found 0 4 --strategy $strat --length 4 --no-match
    run_case "$strat: no match scans whole keyspace" candidates 456976 4 \
             --strategy $strat --length 4 --no-match

    # Keyspace smaller than the number of ranks (length 1 -> 26 candidates, 8 ranks).
    run_case "$strat: keyspace < ranks, correct finder" finder_rank 5 8 \
             --strategy $strat --length 1 --index 5

    # Uneven work distribution: 3 ranks do not evenly divide a 17576 keyspace, and
    # a full no-match scan must still evaluate every candidate exactly once.
    run_case "$strat: uneven distribution scans all" candidates 17576 3 \
             --strategy $strat --length 3 --no-match

    # File recovery: the found password must recover the encrypted file.
    run_case "$strat: file recovery verified" recovery 1 4 \
             --strategy $strat --length 4 --recover-file "${TMPDIR:-/tmp}/pc_test_recover"

    echo
done

# Cross-check: both strategies must agree on where the target is, for several
# indices and process counts. (Candidate counts legitimately differ between
# strategies, so we compare the deterministic finder_rank.)
echo "-- cross-check: collective and async agree on the finder --"
for np in 1 2 4 5; do
    for idx in 0 1 7 12345; do
        co="$($MPIRUN -np $np "$PROG" --strategy collective --length 4 --index $idx --quiet 2>&1)"
        as="$($MPIRUN -np $np "$PROG" --strategy async --length 4 --index $idx --quiet 2>&1)"
        c="$(field "$co" finder_rank)"
        a="$(field "$as" finder_rank)"
        if [ "$c" = "$a" ] && [ -n "$c" ]; then
            echo "ok   [agree np=$np idx=$idx finder_rank=$c]"
            pass=$((pass + 1))
        else
            echo "FAIL [disagree np=$np idx=$idx: collective=$c async=$a]"
            fail=$((fail + 1))
        fi
    done
done

# Full-sweep correctness: an exhaustive sweep must evaluate EXACTLY the whole
# keyspace, deterministically, for BOTH strategies and every process count
# (including counts that do not evenly divide the keyspace). This guards against
# the async early-termination race that previously undercounted the full sweep.
echo
echo "-- full-sweep: exhaustive scan evaluates exactly the keyspace --"
# lower alphabet: 26^4 = 456976, 26^3 = 17576.
for spec in "4 456976" "3 17576"; do
    set -- $spec
    len="$1"
    keyspace="$2"
    for strat in collective async; do
        for np in 1 2 3 4; do
            out="$($MPIRUN -np "$np" "$PROG" --strategy "$strat" --length "$len" \
                   --full-sweep --quiet 2>&1)"
            got="$(field "$out" candidates)"
            if [ "$got" = "$keyspace" ]; then
                echo "ok   [full-sweep $strat len=$len np=$np candidates=$got]"
                pass=$((pass + 1))
            else
                echo "FAIL [full-sweep $strat len=$len np=$np: expected $keyspace got $got]"
                echo "      output: $out"
                fail=$((fail + 1))
            fi
        done
    done
done

echo
echo "== integration summary: $pass passed, $fail failed =="
if [ $fail -eq 0 ]; then
    exit 0
else
    exit 1
fi
