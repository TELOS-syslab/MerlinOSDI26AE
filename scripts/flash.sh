#!/bin/bash
set -euo pipefail

dir="./raw_data/cloudphysics"
output_root="./data/flash"
flash_bin="./libCacheSim/_build/bin/flash"

MAX_JOBS="${MAX_JOBS:-2}"

if command -v nproc >/dev/null 2>&1; then
    cpu_n=$(nproc)
    if [ "$MAX_JOBS" -gt "$cpu_n" ]; then
        MAX_JOBS="$cpu_n"
    fi
fi

mkdir -p "$output_root"

run_one() {
    local path="$1"
    local size="$2"
    local algo="$3"
    local extra_args="$4"

    local output_dir="$output_root/${size}"
    mkdir -p "$output_dir"

    local outfile
    outfile="$output_dir/${algo}.txt"

    if [ -n "$extra_args" ]; then
        "$flash_bin" "$path" oracleGeneral "$algo" "$size" -e "$extra_args" \
            | tail -n 1 >> "$outfile"
    else
        "$flash_bin" "$path" oracleGeneral "$algo" "$size" \
            | tail -n 1 >> "$outfile"
    fi
}

export dir output_root flash_bin
export -f run_one

pids=()

wait_for_slot() {
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

        # FIFO
        wait_for_slot
        bash -c 'run_one "$1" "$2" "$3" "$4"' _ \
            "$path" "$size" "fifo" "" &
        pids+=("$!")

        for dram in 0.001 0.01 0.1; do
            # s3fifo
            wait_for_slot
            bash -c 'run_one "$1" "$2" "$3" "$4"' _ \
                "$path" "$size" "s3fifo" \
                "fifo-size-ratio=$dram,ghost-size-ratio=0.90,move-to-main-threshold=2" &
            pids+=("$!")

            # merlin
            wait_for_slot
            bash -c 'run_one "$1" "$2" "$3" "$4"' _ \
                "$path" "$size" "merlin" \
                "filter-size-ratio=$dram,staging-size-ratio=0.05,ghost-size-ratio=1.00" &
            pids+=("$!")

            # arcfix
            wait_for_slot
            bash -c 'run_one "$1" "$2" "$3" "$4"' _ \
                "$path" "$size" "arcfix" \
                "p=$dram" &
            pids+=("$!")

            # cacheus
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