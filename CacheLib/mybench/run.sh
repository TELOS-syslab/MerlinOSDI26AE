#!/bin/bash 
set -euo pipefail

# data is generated using libCacheSim/scripts/data_gen.py
# python3 libCacheSim/scripts/data_gen.py -m 1000000 -n 100000000 --alpha 1.0 --bin-output zipf1.0_1_100.oracleGeneral.bin

usage() {
   echo "$0 <algo> <cache size in MB> <hashpower>"
   exit
}

algo=${1:-"flex"}
sz_base=${2:-"4000"}
hp_base=${3:-"21"}
file_path=${4:-"zipf1.0_1_400.oracleGeneral.bin"}

#echo "############## flex 6400 25 16"
#./_build/flex zipf1.0_1_100.oracleGeneral.bin 6400 25 16 #| tail -n 10
#echo "############## s3fifo 6400 25 16"
#./_build/s3fifo zipf1.0_1_100.oracleGeneral.bin 6400 25 16 #| tail -n 10
#numactl --membind=0 ./_build/flex zipf1.0_1_100.oracleGeneral.bin 1000 22 2 | tail -n 4

for nThread in 1 2 4 8 16 24 32 48 64 128; do 
    sz=$(echo "${sz_base} * ${nThread}" | bc)
    hp=$(echo "${hp_base} + l(${nThread})/l(2)" | bc -l | cut -d'.' -f1)
    echo "############## ${algo} ${nThread} threads, cache size $sz MB, hashpower $hp"
    numactl --membind=0 ./_build/${algo} $file_path $sz $hp ${nThread} | tail -n 1
done

