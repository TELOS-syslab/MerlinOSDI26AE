#!/bin/bash
# Install system and third-party dependencies for the Merlin artifact.
#
# This script is written for Ubuntu 22.04. It installs packages required by
# libCacheSim, then builds newer versions of zstd, XGBoost, and LightGBM from
# source because the simulator links against these libraries.
set -ue

# Install a recent CMake in a user-local virtual environment. The PATH update is
# appended to ~/.bashrc so subsequent shells use this CMake first.
pip install "cmake>=3.28,<4"

# Install liburing, a dependency of CacheLib.
pushd CacheLib/cachelib/external/liburing
./configure --prefix=/usr/local
make -j"$(nproc)"
make install
ldconfig
popd

setup_xgboost() {
    # XGBoost is used by libCacheSim's learning-based policy components.
    pushd /tmp/
	git clone --recursive https://github.com/dmlc/xgboost
	pushd xgboost
	mkdir build
	pushd build
	cmake ..
	make -j $(nproc)
	sudo make install
	popd
	popd
	popd
}

setup_lightgbm() {
    # LightGBM is another optional learner required by the simulator build.
    pushd /tmp/
	
	git clone --recursive https://github.com/microsoft/LightGBM
	pushd LightGBM
	mkdir build
	pushd build
	cmake ..
	make -j $(nproc)
	sudo make install
	popd
	popd
	popd
}

setup_zstd() {
    # The public traces are zstd-compressed; build the version expected by the
    # artifact rather than relying on distribution packages.
    pushd /tmp/
    wget https://github.com/facebook/zstd/releases/download/v1.5.0/zstd-1.5.0.tar.gz
    tar xvf zstd-1.5.0.tar.gz;
    pushd zstd-1.5.0/build/cmake/
    mkdir _build;
    pushd _build/;
    cmake ..
    make -j $(nproc)
    sudo make install
	popd
	popd
	popd
}

# We have a proxy port for Github due to the network issues.
# If you don't need this, or you don't have one proxy, please delete them.
export http_proxy=http://127.0.0.1:20171
export https_proxy=http://127.0.0.1:20171

git config --global http.proxy http://127.0.0.1:20171
git config --global https.proxy http://127.0.0.1:20171

setup_zstd
# setup_xgboost
# setup_lightgbm