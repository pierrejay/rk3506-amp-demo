# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025 Pierre Jay

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Luckfox Lyra SDK build dependencies
# The official wiki misses several critical packages - this list works.
RUN apt-get update && apt-get install -y \
    git ssh make gcc libssl-dev liblz4-tool expect expect-dev \
    g++ patchelf chrpath gawk texinfo diffstat binfmt-support \
    qemu-user-static live-build bison flex fakeroot cmake \
    gcc-multilib g++-multilib unzip device-tree-compiler \
    ncurses-dev libgucharmap-2-90-dev bzip2 expat gpgv2 \
    cpp-aarch64-linux-gnu libgmp-dev libmpc-dev bc \
    python-is-python3 python2 rsync curl wget ca-certificates \
    file cpio sed grep findutils tar gzip bzip2 xz-utils patch \
    perl python3 python3-dev sudo bsdmainutils scons \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user (UID 1000 for volume permissions)
ARG USER_NAME=builder
ARG USER_UID=1000
ARG USER_GID=1000

RUN groupadd -g ${USER_GID} ${USER_NAME} && \
    useradd -m -u ${USER_UID} -g ${USER_NAME} -s /bin/bash ${USER_NAME} && \
    echo "${USER_NAME} ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers

USER ${USER_NAME}

# Configure git (required by repo tool)
RUN git config --global user.email "builder@localhost" && \
    git config --global user.name "Builder"

WORKDIR /workspace

CMD ["/bin/bash"]
