#!/bin/bash
set -ue
# Install libCacheSim and CacheLib

# libCacheSim
pushd libCacheSim
mkdir _build
cd _build
cmake ..
make -j
popd

# CacheLib
pushd CacheLib
docker build -t cachelib-ae .
sudo mkdir -p build-fbthrift/thrift/conformance/if
popd