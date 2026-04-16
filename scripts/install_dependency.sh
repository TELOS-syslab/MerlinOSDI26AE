#!/bin/bash
# Install dependencies for Merlin
set -ue
# Install libCacheSim dependency
sudo apt-get update

sudo apt install -yqq build-essential google-perftools xxhash
sudo apt install -yqq libglib2.0-dev libunwind-dev
sudo apt install -yqq libgoogle-perftools-dev

# Install cmake 3.28 or higher
sudo apt install -y python3-venv python3-pip
python3 -m venv ~/.local/cmake-venv
~/.local/cmake-venv/bin/pip install --upgrade pip
~/.local/cmake-venv/bin/pip install cmake
echo 'export PATH="$HOME/.local/cmake-venv/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc

setup_xgboost() {
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