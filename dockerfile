FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TERM=xterm

WORKDIR /CacheLib

RUN apt-get update && apt-get install -y \
    git \
    sudo \
    build-essential \
    cmake \
    ninja-build \
    bash \
    pkg-config \
    autoconf \
    libtool \
    bc \
    && rm -rf /var/lib/apt/lists/*

COPY . /CacheLib

RUN git submodule update --init --recursive

RUN cd cachelib/external/liburing && \
    ./configure --prefix=/usr/local && \
    make -j$(nproc) && \
    make install && \
    ldconfig

CMD ["/bin/bash"]
