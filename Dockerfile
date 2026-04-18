FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    build-essential \
    curl \
    tar \
    gzip \
    ca-certificates \
    flex \
    bison \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
