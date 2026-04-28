FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TERM=xterm

WORKDIR /CacheLib

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
    libunwind-dev \
    liburing-dev \
    libxxhash-dev \
    make \
    ninja-build \
    numactl \
    pkg-config \
    python3 \
    sudo \
    xxhash \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /CacheLib

RUN cd cachelib/external/liburing && \
    ./configure --prefix=/usr/local && \
    make -j"$(nproc)" && \
    make install && \
    ldconfig

# fbthrift conformance codegen expects this output directory to already exist.
RUN mkdir -p /CacheLib/build-fbthrift/thrift/conformance/if

# # Build and install CacheLib dependencies into /CacheLib/opt/cachelib inside the image.
# RUN ./contrib/build.sh -O -j -v

CMD ["/bin/bash"]
