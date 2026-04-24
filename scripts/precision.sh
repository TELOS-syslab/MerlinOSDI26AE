#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT


mkdir -p "$ROOT/data/precision"

init_out() {
  local out_file="$1"
  printf 'algo, hit, precision\n' > "$out_file"
}

run_case() {
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

algo = sys.argv[1]
log_path = Path(sys.argv[2])
out_path = Path(sys.argv[3])
lines = [line.strip() for line in log_path.read_text().splitlines() if line.strip()]
prec_lines = [line for line in lines if 'precision' in line and 'hit' in line]
if not prec_lines:
    raise SystemExit(f'failed to find precision line in {log_path}')
line = prec_lines[-1]
rows = []

if algo == 'merlin':
    m = re.search(r'average hit in core:\s*([0-9.]+)\s+precision\s+([0-9.]+)', line)
    if not m:
        raise SystemExit(f'failed to parse merlin line: {line}')
    rows.append(('merlin', m.group(1), m.group(2)))
elif algo == 'cacheus':
    matches = re.findall(r'object from (lru|lfu) ghost \d+, average hit ([0-9.]+), precision ([0-9.]+)', line)
    if len(matches) != 2:
        raise SystemExit(f'failed to parse cacheus line: {line}')
    name_map = {'lru': 'cacheus-r', 'lfu': 'cacheus-f'}
    for src, hit, precision in matches:
        rows.append((name_map[src], hit, precision))
elif algo == 'arc':
    matches = re.findall(r'object from (t1|t2) ghost \d+, average hit ([0-9.]+), precision ([0-9.]+)', line)
    if len(matches) != 2:
        raise SystemExit(f'failed to parse arc line: {line}')
    name_map = {'t1': 'arc-r', 't2': 'arc-f'}
    for src, hit, precision in matches:
        rows.append((name_map[src], hit, precision))
else:
    raise SystemExit(f'unsupported algo {algo}')

with out_path.open('a') as f:
    for row in rows:
        f.write(f'{row[0]}, {row[1]}, {row[2]}\n')
PY
}

TWITTER_OUT="$ROOT/data/precision/twitter.dat"
FIU_OUT="$ROOT/data/precision/fiu.dat"

init_out "$TWITTER_OUT"
init_out "$FIU_OUT"

run_case "CacheTrace/twitter/cluster8.oracleGeneral.zst" merlin 0.1 "$TWITTER_OUT"
run_case "CacheTrace/twitter/cluster8.oracleGeneral.zst" cacheus 0.1 "$TWITTER_OUT"
run_case "CacheTrace/twitter/cluster8.oracleGeneral.zst" arc 0.1 "$TWITTER_OUT"

run_case "CacheTrace/fiu/fiu_webmail.cs.fiu.edu-110108-113008.oracleGeneral.zst" merlin 0.2 "$FIU_OUT"
run_case "CacheTrace/fiu/fiu_webmail.cs.fiu.edu-110108-113008.oracleGeneral.zst" cacheus 0.2 "$FIU_OUT"
run_case "CacheTrace/fiu/fiu_webmail.cs.fiu.edu-110108-113008.oracleGeneral.zst" arc 0.2 "$FIU_OUT"

echo "wrote $TWITTER_OUT"
echo "wrote $FIU_OUT"
