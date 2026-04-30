#!/bin/bash

DATA_DIR="./CacheTrace/cloudphysics/"
BASE_CMD="python3 ./scripts/flashield/flashield.py"
GLOBAL_OPTS="--cache-size 0.01 --disk-cache-type=FIFO --use-obj-size true"

########################################
# 1. Stage one: run every cloudphysics trace in parallel
########################################
echo "==> Stage one: run all files in ${DATA_DIR} with ram-size-ratio=0.001"
mkdir /home/gjh/flashield/results/0.01/dram0.001
cd /home/gjh/flashield/results/0.01/dram0.001
ls ${DATA_DIR} | while read filename; do
    FILE_PATH="${DATA_DIR}/${filename}"
    echo "${BASE_CMD} ${FILE_PATH} --ram-size-ratio=0.001 ${GLOBAL_OPTS}"
done | xargs -P 106 -I CMD bash -c "CMD"

echo "==> Stage one complete"

######
# 2. Stage two
######

echo "==> Stage two: run all files in ${DATA_DIR} with ram-size-ratio=0.01"
mkdir /home/gjh/flashield/results/0.01/dram0.01
cd /home/gjh/flashield/results/0.01/dram0.01
ls ${DATA_DIR} | while read filename; do
    FILE_PATH="${DATA_DIR}/${filename}"
    echo "${BASE_CMD} ${FILE_PATH} --ram-size-ratio=0.01 ${GLOBAL_OPTS}"
done | xargs -P 106 -I CMD bash -c "CMD"

echo "==> Stage two complete"

######
# 3. Stage three
######

echo "==> Stage three: run all files in ${DATA_DIR} with ram-size-ratio=0.10"
mkdir /home/gjh/flashield/results/0.01/dram0.1
cd /home/gjh/flashield/results/0.01/dram0.1
ls ${DATA_DIR} | while read filename; do
    FILE_PATH="${DATA_DIR}/${filename}"
    echo "${BASE_CMD} ${FILE_PATH} --ram-size-ratio=0.10 ${GLOBAL_OPTS}"
done | xargs -P 106 -I CMD bash -c "CMD"

echo "==> Stage three complete"
