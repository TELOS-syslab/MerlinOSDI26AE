#!/bin/bash
# Install system and third-party dependencies for the Merlin artifact.
#
# This script is written for Ubuntu 22.04. It installs packages required by
# libCacheSim, then builds newer versions of zstd, XGBoost, and LightGBM from
# source because the simulator links against these libraries.
set -ue
# Install libCacheSim package dependencies.
sudo apt-get update

sudo apt install -yqq build-essential google-perftools xxhash
sudo apt install -yqq libglib2.0-dev libunwind-dev
sudo apt install -yqq libgoogle-perftools-dev

# Install a recent CMake in a user-local virtual environment. The PATH update is
# appended to ~/.bashrc so subsequent shells use this CMake first.
sudo apt install -y python3-venv python3-pip
python3 -m venv ~/.local/cmake-venv
~/.local/cmake-venv/bin/pip install --upgrade pip
~/.local/cmake-venv/bin/pip install cmake
echo 'export PATH="$HOME/.local/cmake-venv/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc

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

setup_zstd
setup_xgboost
setup_lightgbm
