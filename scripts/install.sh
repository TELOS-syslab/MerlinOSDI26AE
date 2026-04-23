#!/bin/bash
# Build libCacheSim locally and bake CacheLib into the Docker image.
set -ue

pushd libCacheSim
mkdir -p _build
cd _build
cmake ..
make -j
popd

pushd libCacheSim
mkdir -p _build2
cd _build2
cmake -DCMAKE_CXX_FLAGS="-DTRACK_PARAMETERS" -DCMAKE_C_FLAGS="-DTRACK_PARAMETERS" ..
make -j
popd

pushd CacheLib
sudo docker build -t cachelib-ae .
popd
