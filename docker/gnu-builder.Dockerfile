FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        curl \
        file \
        flex \
        bison \
        make \
        patch \
        perl \
        python3 \
        tar \
        texinfo \
        unzip \
        xz-utils \
        bzip2 \
        zip \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
