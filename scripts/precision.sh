#!/bin/bash
# Run precision experiments for Merlin, Cacheus, and ARC and write CSV-style
# summaries under data/precision.
#
# Input:
#   - libCacheSim/_build2/bin/cachesim
#   - CacheTrace/twitter/... and CacheTrace/fiu/... traces
# Output:
#   - data/precision/twitter.dat
#   - data/precision/fiu.dat
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

# Keep generated precision tables in a dedicated output directory.
mkdir -p "$ROOT/data/precision"
STAGE_DIR=$(mktemp -d "$ROOT/data/precision/.precision_stage.XXXXXX")
cleanup_stage() {
  rm -rf "$STAGE_DIR"
}
trap cleanup_stage EXIT

init_out() {
  local out_file="$1"

  # All precision outputs use the same three-column schema.
  printf 'algo, hit, precision\n' > "$out_file"
}

run_case() {
  # Run one cache simulator configuration, capture the full log, then extract
  # the algorithm-specific hit and precision fields into the requested output.
  local trace="$1"
  local algo="$2"
  local cache_ratio="$3"
  local out_file="$4"
  local log="$TMP_DIR/${algo}_$(basename "$trace").log"

  echo "[RUN] $algo $trace ratio=$cache_ratio"
  "$ROOT/libCacheSim/_build2/bin/cachesim" \
    "$ROOT/$trace" \
    oracleGeneral "$algo" "$cache_ratio" --ignore-obj-size=true \
    | tee "$log"

  python3 - "$algo" "$log" "$out_file" <<'PY'
import re
import sys
from pathlib import Path

# Arguments are passed by the shell wrapper so the parser can stay independent
# of any repository-relative paths.
algo = sys.argv[1]
log_path = Path(sys.argv[2])
out_path = Path(sys.argv[3])
lines = [line.strip() for line in log_path.read_text().splitlines() if line.strip()]
prec_lines = [line for line in lines if 'precision' in line and 'hit' in line]
if not prec_lines:
    raise SystemExit(f'failed to find precision line in {log_path}')

# The simulator may print multiple progress lines; the final matching line is
# the summary for this run.
line = prec_lines[-1]
rows = []

if algo == 'merlin':
    m = re.search(r'average hit in core:\s*([0-9.]+)\s+precision\s+([0-9.]+)', line)
    if not m:
        raise SystemExit(f'failed to parse merlin line: {line}')
    rows.append(('merlin', m.group(1), m.group(2)))
elif algo == 'cacheus':
    # Cacheus reports separate precision values for objects promoted from the
    # LRU and LFU ghost queues; emit one row for each source so the plotter can
    # distinguish the recency and frequency components.
    matches = re.findall(r'object from (lru|lfu) ghost \d+, average hit ([0-9.]+), precision ([0-9.]+)', line)
    if len(matches) != 2:
        raise SystemExit(f'failed to parse cacheus line: {line}')
    name_map = {'lru': 'cacheus-r', 'lfu': 'cacheus-f'}
    for src, hit, precision in matches:
        rows.append((name_map[src], hit, precision))
elif algo == 'arc':
    # ARC reports the same metric split by T1 and T2 ghost queues, matching the
    # recency/frequency split used in the output labels.
    matches = re.findall(r'object from (t1|t2) ghost \d+, average hit ([0-9.]+), precision ([0-9.]+)', line)
    if len(matches) != 2:
        raise SystemExit(f'failed to parse arc line: {line}')
    name_map = {'t1': 'arc-r', 't2': 'arc-f'}
    for src, hit, precision in matches:
        rows.append((name_map[src], hit, precision))
else:
    raise SystemExit(f'unsupported algo {algo}')

with out_path.open('a') as f:
    # Append rows so multiple algorithms can share one trace-level data file.
    for row in rows:
        f.write(f'{row[0]}, {row[1]}, {row[2]}\n')
PY
}

TWITTER_OUT="$ROOT/data/precision/twitter.dat"
FIU_OUT="$ROOT/data/precision/fiu.dat"
TWITTER_STAGE="$STAGE_DIR/twitter.dat"
FIU_STAGE="$STAGE_DIR/fiu.dat"

init_out "$TWITTER_STAGE"
init_out "$FIU_STAGE"

# FIU uses a larger 20% cache ratio to match the artifact experiment setup.
run_case "CacheTrace/fiu/fiu_webmail.cs.fiu.edu-110108-113008.oracleGeneral.zst" merlin 0.2 "$FIU_STAGE"
run_case "CacheTrace/fiu/fiu_webmail.cs.fiu.edu-110108-113008.oracleGeneral.zst" cacheus 0.2 "$FIU_STAGE"
run_case "CacheTrace/fiu/fiu_webmail.cs.fiu.edu-110108-113008.oracleGeneral.zst" arc 0.2 "$FIU_STAGE"

# Twitter uses a 10% cache ratio for the Figure 17 precision comparison.
run_case "CacheTrace/twitter/cluster8.oracleGeneral.zst" merlin 0.1 "$TWITTER_STAGE"
run_case "CacheTrace/twitter/cluster8.oracleGeneral.zst" cacheus 0.1 "$TWITTER_STAGE"
run_case "CacheTrace/twitter/cluster8.oracleGeneral.zst" arc 0.1 "$TWITTER_STAGE"

mv "$TWITTER_STAGE" "$TWITTER_OUT"
mv "$FIU_STAGE" "$FIU_OUT"

echo "wrote $TWITTER_OUT"
echo "wrote $FIU_OUT"
