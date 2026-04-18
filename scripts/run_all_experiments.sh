#!/bin/bash
set -ue
# Run all experiments of Merlin in libCacheSim.


# Run the CacheLib experiment.
pushd CacheLib
## Generate Trace
python3 mybench/data_genmix.py 

## Run
docker 