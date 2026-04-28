#!/bin/bash
# Build libCacheSim and the CacheLib benchmark environment for the artifact.
#
# Output:
#   - libCacheSim/_build
#   - libCacheSim/_build2
#   - Docker image: cachelib-ae
#
# This script intentionally builds two libCacheSim variants: one normal build
# for the main evaluation and one TRACK_PARAMETERS build for parameter-tracking
# experiments.
set -euo pipefail

find_docker_cmd() {
  if docker info >/dev/null 2>&1; then
    DOCKER_CMD=(docker)
  elif command -v sudo >/dev/null 2>&1 && sudo -n docker info >/dev/null 2>&1; then
    DOCKER_CMD=(sudo docker)
  else
    echo "Docker is unavailable for the current user. Use a user in the docker group or configure passwordless sudo for docker." >&2
    return 1
  fi
}

build_libcachesim_variant() {
  local build_dir="$1"
  shift
  pushd libCacheSim >/dev/null
  mkdir -p "$build_dir"
  cd "$build_dir"
  cmake "$@" ..
  make -j"$(nproc)"
  popd >/dev/null
}

# Normal libCacheSim build used by evaluation.py and flash.sh.
build_libcachesim_variant _build

# libCacheSim build with additional parameter-tracking instrumentation.
build_libcachesim_variant _build2 \
  -DCMAKE_CXX_FLAGS="-DTRACK_PARAMETERS" \
  -DCMAKE_C_FLAGS="-DTRACK_PARAMETERS"

find_docker_cmd
pushd CacheLib >/dev/null
"${DOCKER_CMD[@]}" build -t cachelib-ae .
mkdir -p build-fbthrift/thrift/conformance/if
"${DOCKER_CMD[@]}" run --rm --cap-add=SYS_NICE \
  -v "$(pwd)":/CacheLib \
  cachelib-ae /bin/bash -lc '
    set -euo pipefail
    cd /CacheLib
    find . -maxdepth 1 -type d -name "build-*" -exec rm -rf {} +
    mkdir -p build-fbthrift/thrift/conformance/if
    ./contrib/build.sh -O -j -v
  '
popd >/dev/null
