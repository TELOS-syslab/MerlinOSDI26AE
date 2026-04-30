FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TERM=xterm

WORKDIR /Merlin

RUN apt-get update && apt-get install -y \
    autoconf \
    bc \
    binutils-dev \
    bison \
    build-essential \
    cmake \
    flex \
    g++ \
    git \
    libaio-dev \
    libboost-all-dev \
    libbz2-dev \
    libdouble-conversion-dev \
    libdwarf-dev \
    libelf-dev \
    libevent-dev \
    libgflags-dev \
    libgoogle-glog-dev \
    libiberty-dev \
    libjemalloc-dev \
    liblz4-dev \
    liblzma-dev \
    libnuma-dev \
    libsnappy-dev \
    libsodium-dev \
    libssl-dev \
    libtool \
    wget \
    libunwind-dev \
    liburing-dev \
    libxxhash-dev \
    make \
    ninja-build \
    numactl \
    pkg-config \
    python3 \
    python3-pip \
    sudo \
    xxhash \
    zlib1g-dev \
    libgoogle-perftools-dev \
    libglib2.0-dev libunwind-dev \
    build-essential google-perftools xxhash \
    && rm -rf /var/lib/apt/lists/*

COPY . /Merlin

RUN bash scripts/install_dependency.sh

CMD ["/bin/bash"]
