#!/bin/bash
# Build libCacheSim and the CacheLib benchmark environment for the artifact.
#
# Output:
#   - libCacheSim/_build/
#   - libCacheSim/_build2/
#   - CacheLib/build-*/
#   - CacheLib/opt/
#   - CacheLib/mybench/_build/
#
# This script intentionally builds two libCacheSim variants: one normal build
# for the main evaluation and one TRACK_PARAMETERS build for parameter-tracking
# experiments.
set -ue

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

# Github proxy for building CacheLib
git config --global http.proxy http://127.0.0.1:20171
git config --global https.proxy http://127.0.0.1:20171

# CacheLib install
pushd CacheLib
mkdir -p build-fbthrift/thrift/conformance/if
./contrib/build.sh -O -j -v
popd