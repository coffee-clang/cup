FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    build-essential \
    curl \
    tar \
    gzip \
    xz-utils \
    ca-certificates \
    flex \
    bison \
    texinfo \
    python3 \
    make \
    file \
    patch \
    libgmp-dev \
    libmpfr-dev \
    libmpc-dev \
    libisl-dev \
    zlib1g-dev \
    libexpat1-dev \
    libncurses-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work