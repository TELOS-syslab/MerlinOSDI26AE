#!/bin/bash
# Install system and third-party dependencies for the Merlin artifact.
#
# This script is written for Ubuntu 22.04. It installs packages required by
# libCacheSim, then builds newer versions of zstd, XGBoost, and LightGBM from
# source because the simulator links against these libraries.
set -euo pipefail

have_passwordless_sudo() {
  command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1
}

run_privileged_or_skip() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  elif have_passwordless_sudo; then
    sudo "$@"
  else
    echo "Skipping privileged command because sudo is unavailable: $*" >&2
    return 0
  fi
}

cmake_install_or_skip() {
  if [ "$(id -u)" -eq 0 ] || have_passwordless_sudo; then
    run_privileged_or_skip make install
  else
    echo "Skipping privileged install step because sudo is unavailable: make install" >&2
    return 0
  fi
}

has_any_file() {
  for path in "$@"; do
    if [ -e "$path" ]; then
      return 0
    fi
  done
  return 1
}

run_privileged_or_skip apt-get update
run_privileged_or_skip apt install -yqq build-essential google-perftools xxhash
run_privileged_or_skip apt install -yqq libglib2.0-dev libunwind-dev
run_privileged_or_skip apt install -yqq libgoogle-perftools-dev
run_privileged_or_skip apt install -y python3-venv python3-pip

if [ ! -x "$HOME/.local/cmake-venv/bin/cmake" ]; then
  python3 -m venv ~/.local/cmake-venv
  ~/.local/cmake-venv/bin/pip install --upgrade pip
  ~/.local/cmake-venv/bin/pip install cmake
fi
if ! grep -Fqx 'export PATH="$HOME/.local/cmake-venv/bin:$PATH"' ~/.bashrc 2>/dev/null; then
  echo 'export PATH="$HOME/.local/cmake-venv/bin:$PATH"' >> ~/.bashrc
fi
export PATH="$HOME/.local/cmake-venv/bin:$PATH"

setup_xgboost() {
  if has_any_file "$HOME/.local/lib/libxgboost.so" "/usr/local/lib/libxgboost.so"; then
    echo "Skipping xgboost: existing installation detected."
    return 0
  fi
  pushd /tmp/ >/dev/null
  rm -rf xgboost
  git clone --recursive https://github.com/dmlc/xgboost
  pushd xgboost >/dev/null
  mkdir -p build
  pushd build >/dev/null
  cmake ..
  make -j "$(nproc)"
  cmake_install_or_skip
  popd >/dev/null
  popd >/dev/null
  popd >/dev/null
}

setup_lightgbm() {
  if has_any_file "$HOME/.local/lib/lib_lightgbm.so" "/usr/local/lib/lib_lightgbm.so"; then
    echo "Skipping LightGBM: existing installation detected."
    return 0
  fi
  pushd /tmp/ >/dev/null
  rm -rf LightGBM
  git clone --recursive https://github.com/microsoft/LightGBM
  pushd LightGBM >/dev/null
  mkdir -p build
  pushd build >/dev/null
  cmake ..
  make -j "$(nproc)"
  cmake_install_or_skip
  popd >/dev/null
  popd >/dev/null
  popd >/dev/null
}

setup_zstd() {
  if has_any_file "$HOME/.local/lib/libzstd.so" "/usr/local/lib/libzstd.so"; then
    echo "Skipping zstd: existing installation detected."
    return 0
  fi
  pushd /tmp/ >/dev/null
  rm -rf zstd-1.5.0 zstd-1.5.0.tar.gz
  wget https://github.com/facebook/zstd/releases/download/v1.5.0/zstd-1.5.0.tar.gz
  tar xvf zstd-1.5.0.tar.gz
  pushd zstd-1.5.0/build/cmake/ >/dev/null
  mkdir -p _build
  pushd _build/ >/dev/null
  cmake ..
  make -j "$(nproc)"
  cmake_install_or_skip
  popd >/dev/null
  popd >/dev/null
  popd >/dev/null
}

setup_zstd
setup_xgboost
setup_lightgbm
