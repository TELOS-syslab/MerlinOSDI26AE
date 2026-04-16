#!/bin/bash
set -ue
# Install libCacheSim and CacheLib

# libCacheSim
pushd libCacheSim/scripts;
bash ./install_libcachesim.sh
popd;

# CacheLib
