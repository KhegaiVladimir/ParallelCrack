#!/usr/bin/env bash
#
# Sweep ParallelCrack across a set of process counts and record one CSV row per
# run. Repeats each configuration a few times so the plotting script can average
# out noise. This script only MEASURES -- it never fabricates numbers.
#
# Example:
#   benchmarks/run.sh --strategy collective --length 5 --full-sweep \
#       --procs "1 2 4 8" --repeats 3 --csv results/collective.csv
#
# On Lotus, run this from inside a job allocation and point it at srun:
#   MPIRUN="srun" benchmarks/run.sh --strategy async --length 6 --full-sweep \
#       --procs "1 2 4 8 16 32 48" --csv results/async.csv
#
# NOTE: this does NOT submit SLURM jobs for you. See benchmarks/README.md for the
# sbatch wrapper.

set -euo pipefail

# Defaults.
STRATEGY="collective"
LENGTH=5
ALPHABET="lower"
BATCH=2000
PROCS="1 2 4 8"
REPEATS=3
MODE="--full-sweep"   # benchmark the worst case (whole keyspace) by default
CSV="results/bench.csv"
PROG="${PROG:-./parallelcrack}"
MPIRUN="${MPIRUN:-mpirun --oversubscribe}"

usage() {
    echo "Usage: $0 [options]"
    echo "  --strategy <collective|async>   (default: $STRATEGY)"
    echo "  --length <N>                     candidate length (default: $LENGTH)"
    echo "  --alphabet <lower|alnum|printable> (default: $ALPHABET)"
    echo "  --batch <N>                      batch size (default: $BATCH)"
    echo "  --procs \"<list>\"                 process counts (default: \"$PROCS\")"
    echo "  --repeats <N>                    runs per process count (default: $REPEATS)"
    echo "  --full-sweep                     target at last index (default)"
    echo "  --no-match                       target absent (longest possible search)"
    echo "  --index <K>                      specific target index"
    echo "  --csv <PATH>                     output CSV (default: $CSV)"
}

INDEX_ARG=""
while [ $# -gt 0 ]; do
    case "$1" in
        --strategy) STRATEGY="$2"; shift 2 ;;
        --length)   LENGTH="$2"; shift 2 ;;
        --alphabet) ALPHABET="$2"; shift 2 ;;
        --batch)    BATCH="$2"; shift 2 ;;
        --procs)    PROCS="$2"; shift 2 ;;
        --repeats)  REPEATS="$2"; shift 2 ;;
        --full-sweep) MODE="--full-sweep"; shift ;;
        --no-match)   MODE="--no-match"; shift ;;
        --index)      MODE=""; INDEX_ARG="--index $2"; shift 2 ;;
        --csv)      CSV="$2"; shift 2 ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "Unknown option: $1"; usage; exit 1 ;;
    esac
done

mkdir -p "$(dirname "$CSV")"

echo "ParallelCrack benchmark sweep"
echo "  strategy : $STRATEGY"
echo "  length   : $LENGTH  alphabet: $ALPHABET  batch: $BATCH"
echo "  procs    : $PROCS   repeats : $REPEATS"
echo "  mode     : ${MODE:-$INDEX_ARG}"
echo "  csv      : $CSV"
echo "  launcher : $MPIRUN"
echo

for np in $PROCS; do
    for run in $(seq 1 "$REPEATS"); do
        echo "  -> np=$np run=$run"
        $MPIRUN -np "$np" "$PROG" \
            --strategy "$STRATEGY" \
            --length "$LENGTH" \
            --alphabet "$ALPHABET" \
            --batch "$BATCH" \
            $MODE $INDEX_ARG \
            --csv "$CSV" --quiet > /dev/null
    done
done

echo
echo "Done. Rows appended to $CSV"
echo "Plot with: python3 benchmarks/plot.py $CSV --out ${CSV%.csv}.png"
