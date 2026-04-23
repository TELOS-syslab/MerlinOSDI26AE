#!/bin/bash
set -ue
# Install libCacheSim and CacheLib

# libCacheSim
pushd libCacheSim
mkdir -p _build
cd _build
cmake ..
make -j
popd

# libCacheSim for parameter tracking
pushd libCacheSim
mkdir -p _build2
cd _build2
cmake -DCMAKE_CXX_FLAGS="-DTRACK_PARAMETERS" -DCMAKE_C_FLAGS="-DTRACK_PARAMETERS" ..
make -j
popd

# CacheLib
pushd CacheLib
sudo docker build -t cachelib-ae .
sudo mkdir -p build-fbthrift/thrift/conformance/if
docker run --rm --cap-add=SYS_NICE -it -v "$(pwd)":/Merlin -w /Merlin cachelib-ae /bin/bash -lc "cd CacheLib/mybench && bash build.sh"
popd