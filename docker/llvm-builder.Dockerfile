FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    build-essential \
    ca-certificates \
    cmake \
    curl \
    file \
    git \
    gzip \
    libedit-dev \
    libffi-dev \
    libgmp-dev \
    libmpfr-dev \
    libncurses-dev \
    libxml2-dev \
    libzstd-dev \
    ninja-build \
    patch \
    python3 \
    python3-dev \
    swig \
    tar \
    xz-utils \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
