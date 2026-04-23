#!/bin/bash
# Build libCacheSim and the CacheLib benchmark environment for the artifact.
#
# This script intentionally builds two libCacheSim variants: one normal build
# for the main evaluation and one TRACK_PARAMETERS build for parameter-tracking
# experiments.
set -ue
# Install libCacheSim and CacheLib

# Normal libCacheSim build used by evaluation.py and flash.sh.
pushd libCacheSim
mkdir -p _build
cd _build
cmake ..
make -j
popd

# libCacheSim build with additional parameter-tracking instrumentation.
pushd libCacheSim
mkdir -p _build2
cd _build2
cmake -DCMAKE_CXX_FLAGS="-DTRACK_PARAMETERS" -DCMAKE_C_FLAGS="-DTRACK_PARAMETERS" ..
make -j
popd

# CacheLib benchmark build runs inside the Docker image to keep its dependency
# stack isolated from the host.
pushd CacheLib
sudo docker build -t cachelib-ae .
sudo mkdir -p build-fbthrift/thrift/conformance/if
docker run --rm --cap-add=SYS_NICE -it -v "$(pwd)":/Merlin -w /Merlin cachelib-ae /bin/bash -lc "cd CacheLib/mybench && bash build.sh"
popd
