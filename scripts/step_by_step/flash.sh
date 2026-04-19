#!/bin/bash

dir="/mnt/data/gjh/cloudphysics"
output_dir="data/flash_results"
flash_bin="_build/bin/flash"

mkdir -p "$output_dir"

task_file="/tmp/flash_tasks.txt"
rm -f "$task_file"
# 生成任务列表：每个任务 = 一次 flash 调用（只取最后一行）
for size in 0.01 0.1 do
    output_dir = "data/flash_results/size_${size}"
    mkdir -p "$output_dir"

    for path in "$dir"/*; do
        # FIFO baseline
        echo "$flash_bin \"$path\" oracleGeneral FIFO "$size" | tail -n 1 >> $output_dir/fifo.txt" >> "$task_file"

        for dram in 0.001 0.01 0.1; do

            # s3fifo
            echo "$flash_bin \"$path\" oracleGeneral s3fifo "$size" -e \"fifo-size-ratio=$dram,ghost-size-ratio=0.90,move-to-main-threshold=2\" | tail -n 1 >> $output_dir/s3fifo.txt" >> "$task_file"

            # merlin ————（已按你的要求更新）————
            echo "$flash_bin \"$path\" oracleGeneral merlin "$size" -e \"filter-size-ratio=$dram,staging-size-ratio=0.05,ghost-size-ratio=1.00\" | tail -n 1 >> $output_dir/merlin.txt" >> "$task_file"

            # arcfix
            echo "$flash_bin \"$path\" oracleGeneral arcfix "$size" -e \"p=$dram\" | tail -n 1 >> $output_dir/arcfix.txt" >> "$task_file"

            # cacheus
            echo "$flash_bin \"$path\" oracleGeneral cacheus "$size" -e \"p=$dram\" | tail -n 1 >> $output_dir/cacheus.txt" >> "$task_file"
        done
    done
done


echo "总任务数：$(wc -l < "$task_file")"
echo "开始并行执行（最多 106 核）..."

# 并行执行，最多 100 个任务同时运行
cat "$task_file" | xargs -I CMD -P 106 bash -c 'CMD'

echo "全部完成！"

