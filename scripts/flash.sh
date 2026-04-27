#!/bin/bash
# Run flash-cache experiments for Figure 15.
#
# Input traces are read from ./CacheTrace/cloudphysics. For each trace, this
# script runs multiple policies, cache sizes, and DRAM ratios with bounded
# concurrency controlled by MAX_JOBS.
set -euo pipefail

dir="./CacheTrace/cloudphysics"
output_root="./data/flash"
flash_bin="./libCacheSim/_build/bin/flash"

MAX_JOBS="${MAX_JOBS:-2}"

# Avoid launching more concurrent jobs than available CPU cores.
if command -v nproc >/dev/null 2>&1; then
    cpu_n=$(nproc)
    if [ "$MAX_JOBS" -gt "$cpu_n" ]; then
        MAX_JOBS="$cpu_n"
    fi
fi

mkdir -p "$output_root"
stage_root=$(mktemp -d "$output_root/.flash_stage.XXXXXX")
cleanup() {
    rm -rf "$stage_root"
}
trap cleanup EXIT

run_one() {
    local path="$1"
    local size="$2"
    local algo="$3"
    local extra_args="$4"

    local stage_dir="$stage_root/${size}"
    mkdir -p "$stage_dir"

    local trace_name
    trace_name=$(basename "$path")
    local suffix
    if [ -n "$extra_args" ]; then
        suffix=$(printf '%s' "$extra_args" | tr '=,/' '---' | tr -cd '[:alnum:]._-')
    else
        suffix="base"
    fi

    local outfile="$stage_dir/${algo}__${trace_name}__${suffix}.txt"

    # The flash simulator prints a long log; the final line contains the summary
    # consumed by process_flash.py.
    if [ -n "$extra_args" ]; then
        "$flash_bin" "$path" oracleGeneral "$algo" "$size" -e "$extra_args" \
            | tail -n 1 > "$outfile"
    else
        "$flash_bin" "$path" oracleGeneral "$algo" "$size" \
            | tail -n 1 > "$outfile"
    fi
}

export dir output_root flash_bin stage_root
export -f run_one

pids=()

wait_for_slot() {
    # Keep at most MAX_JOBS background simulator processes alive.
    while [ "${#pids[@]}" -ge "$MAX_JOBS" ]; do
        local new_pids=()
        for pid in "${pids[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                new_pids+=("$pid")
            else
                wait "$pid" || true
            fi
        done
        pids=("${new_pids[@]}")
        sleep 1
    done
}

for path in "$dir"/*; do
    [ -e "$path" ] || continue
    [ -f "$path" ] || continue

    for size in 0.01 0.1; do
        mkdir -p "$output_root/${size}"

        # FIFO has no DRAM-ratio parameter.
        wait_for_slot
        bash -c 'run_one "$1" "$2" "$3" "$4"' _ \
            "$path" "$size" "fifo" "" &
        pids+=("$!")

        for dram in 0.001 0.01 0.1; do
            # S3FIFO uses a FIFO admission region and ghost region.
            wait_for_slot
            bash -c 'run_one "$1" "$2" "$3" "$4"' _ \
                "$path" "$size" "s3fifo" \
                "fifo-size-ratio=$dram,ghost-size-ratio=0.90,move-to-main-threshold=2" &
            pids+=("$!")

            # Merlin uses the DRAM ratio as the filter-size ratio.
            wait_for_slot
            bash -c 'run_one "$1" "$2" "$3" "$4"' _ \
                "$path" "$size" "merlin" \
                "filter-size-ratio=$dram,staging-size-ratio=0.05,ghost-size-ratio=1.00" &
            pids+=("$!")

            # ARCfix and Cacheus use p as the tunable DRAM/history parameter.
            wait_for_slot
            bash -c 'run_one "$1" "$2" "$3" "$4"' _ \
                "$path" "$size" "arcfix" \
                "p=$dram" &
            pids+=("$!")

            wait_for_slot
            bash -c 'run_one "$1" "$2" "$3" "$4"' _ \
                "$path" "$size" "cacheus" \
                "p=$dram" &
            pids+=("$!")
        done
    done
done

for pid in "${pids[@]}"; do
    wait "$pid"
done

for size in 0.01 0.1; do
    stage_dir="$stage_root/${size}"
    [ -d "$stage_dir" ] || continue
    for algo in fifo s3fifo merlin arcfix cacheus; do
        final_file="$output_root/${size}/${algo}.txt"
        tmp_final="$stage_root/${size}/${algo}.merged"
        : > "$tmp_final"
        find "$stage_dir" -maxdepth 1 -type f -name "${algo}__*.txt" | sort | while read -r part; do
            cat "$part" >> "$tmp_final"
        done
        mv "$tmp_final" "$final_file"
    done
done
