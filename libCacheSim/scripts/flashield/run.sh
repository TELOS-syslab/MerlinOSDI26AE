#!/bin/bash

DATA_DIR="/mnt/data/gjh/bin_trace/cloudphysics/"
BASE_CMD="python3 /home/gjh/flashield/flashield.py"
GLOBAL_OPTS="--cache-size 0.01 --disk-cache-type=FIFO --use-obj-size true"

########################################
# 1. 第一阶段：对 cloudphysics 中每个文件并行执行
########################################
echo "==> 第一阶段：对 ${DATA_DIR} 中的所有文件并行执行 ram-size-ratio=0.001"
mkdir /home/gjh/flashield/results/0.01/dram0.001
cd /home/gjh/flashield/results/0.01/dram0.001
ls ${DATA_DIR} | while read filename; do
    FILE_PATH="${DATA_DIR}/${filename}"
    echo "${BASE_CMD} ${FILE_PATH} --ram-size-ratio=0.001 ${GLOBAL_OPTS}"
done | xargs -P 106 -I CMD bash -c "CMD"

echo "==> 第一阶段完成"

######
# 2. 第二阶段 
######

echo "==> 第二阶段：对 ${DATA_DIR} 中的所有文件并行执行 ram-size-ratio=0.01"
mkdir /home/gjh/flashield/results/0.01/dram0.01
cd /home/gjh/flashield/results/0.01/dram0.01
ls ${DATA_DIR} | while read filename; do
    FILE_PATH="${DATA_DIR}/${filename}"
    echo "${BASE_CMD} ${FILE_PATH} --ram-size-ratio=0.01 ${GLOBAL_OPTS}"
done | xargs -P 106 -I CMD bash -c "CMD"

echo "==> 第二阶段完成"

######
# 3. 第三阶段 
######

echo "==> 第三阶段：对 ${DATA_DIR} 中的所有文件并行执行 ram-size-ratio=0.10"
mkdir /home/gjh/flashield/results/0.01/dram0.1
cd /home/gjh/flashield/results/0.01/dram0.1
ls ${DATA_DIR} | while read filename; do
    FILE_PATH="${DATA_DIR}/${filename}"
    echo "${BASE_CMD} ${FILE_PATH} --ram-size-ratio=0.10 ${GLOBAL_OPTS}"
done | xargs -P 106 -I CMD bash -c "CMD"

echo "==> 第三阶段完成"