#!/bin/bash
# Run CacheLib mybench throughput experiments and write plot-ready summaries.
#
# Input:
#   - CacheLib/mybench sources and build scripts
#   - trace file (default: CacheTrace/mix.oracleGeneral.bin)
# Output:
#   - data/throughput/woback/*.dat or data/throughput/wback/*.dat
#
# Keep mybench logic in CacheLib/mybench and call it through this wrapper to
# avoid maintaining duplicated benchmark scripts in multiple directories.
set -euo pipefail

MODE=${1:-woback}
TRACE=${2:-CacheTrace/mix.oracleGeneral.bin}
CACHE_MB_BASE=${3:-4000}
HASHPOWER_BASE=${4:-21}
THREADS=${THREADS:-"1 2 4 8 16 24 32 48 64 128"}
ALGOS=${ALGOS:-"merlin s3fifo arc lru"}

case "$MODE" in
  woback)
    BUILD_SCRIPT=build.sh
    OUT_DIR=data/throughput/woback
    ;;
  wback)
    BUILD_SCRIPT=build_with_backend.sh
    OUT_DIR=data/throughput/wback
    ;;
  *)
    echo "usage: $0 [woback|wback] [trace] [base_cache_mb] [base_hashpower]" >&2
    exit 2
    ;;
esac

ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$ROOT/$OUT_DIR"

sudo docker run --rm --cap-add=SYS_NICE \
  -v "$ROOT":/Merlin \
  -w /Merlin \
  -e MODE="$MODE" \
  -e TRACE="$TRACE" \
  -e CACHE_MB_BASE="$CACHE_MB_BASE" \
  -e HASHPOWER_BASE="$HASHPOWER_BASE" \
  -e THREADS="$THREADS" \
  -e ALGOS="$ALGOS" \
  -e BUILD_SCRIPT="$BUILD_SCRIPT" \
  -e OUT_DIR="$OUT_DIR" \
  cachelib-ae /bin/bash -lc '
set -euo pipefail
# Build and run inside the container so toolchain/runtime are consistent.
cd /Merlin/CacheLib/mybench
bash "$BUILD_SCRIPT"
mkdir -p "/Merlin/$OUT_DIR"

for algo in $ALGOS; do
  : > "/Merlin/$OUT_DIR/$algo.dat"
  for thread in $THREADS; do
    cache_mb=$(echo "${CACHE_MB_BASE} * ${thread}" | bc)
    hashpower=$(echo "${HASHPOWER_BASE} + l(${thread})/l(2)" | bc -l | cut -d"." -f1)
    log="/tmp/${MODE}_${algo}_${thread}.log"
    echo "[RUN] $algo threads=$thread trace=$TRACE cache=${cache_mb}MB hashpower=$hashpower"
    numactl --membind=0 "./_build/$algo" "/Merlin/$TRACE" "$cache_mb" "$hashpower" "$thread" | tee "$log"
    # Parse the final summary line emitted by the benchmark binary.
    last=$(grep "^cachelib " "$log" | tail -n 1 || true)
    if [ -z "$last" ]; then
      echo "failed to parse result for $algo thread $thread" >&2
      exit 1
    fi
    miss=$(printf "%s\n" "$last" | sed -n "s/.*miss ratio \([0-9.]*\).*/\1/p")
    throughput=$(printf "%s\n" "$last" | sed -n "s/.*throughput \([0-9.]*\) MQPS.*/\1/p")
    if [ -z "$miss" ] || [ -z "$throughput" ]; then
      echo "failed to extract miss/throughput from: $last" >&2
      exit 1
    fi
    printf "%s %s %s %s\n" "$algo" "$thread" "$miss" "$throughput" | tee -a "/Merlin/$OUT_DIR/$algo.dat"
  done
done
'
