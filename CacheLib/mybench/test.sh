#!/bin/bash 
# python3 data_gen.py -m 1000000 -n 40000000 --alpha 1.0 --bin-output zipf1.0_1_400.oracleGeneral.bin

# echo "run lru as baseline"
# ./_build/lru zipf1.0_1_400.oracleGeneral.bin 64000 25 16
# echo "run strictlru as baseline 2"
# ./_build/strictlru zipf1.0_1_400.oracleGeneral.bin 64000 25 16
echo "run s3fifo"
./_build/s3fifo $1 6400 25 16
echo "run flex"
./_build/flex $1 6400 25 16
# echo "run car"
# ./_build/car zipf1.0_1_400.oracleGeneral.bin 64000 25 16  
# echo "run arc"
# ./_build/arc zipf1.0_1_400.oracleGeneral.bin 64000 25 16