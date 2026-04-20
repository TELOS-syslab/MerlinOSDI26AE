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
docker run --rm --cap-add=SYS_NICE -it -v "$(pwd)":/Merlin -w /Merlin cachelib-ae /bin/bash -lc "cd /Merlin/CacheLib/mybench && bash build.sh"
popd