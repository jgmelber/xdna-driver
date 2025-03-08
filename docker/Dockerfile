FROM ubuntu:24.04

USER root
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt upgrade --assume-yes && \
    apt install software-properties-common --assume-yes && \
    add-apt-repository ppa:ubuntu-toolchain-r/test && \
    apt install --assume-yes \
    	git build-essential cmake ninja-build \
    	curl wget tar unzip \
    	libssl-dev libelf-dev && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Create a working directory
WORKDIR /workspace

# add a 'github' user with the same UID and GID used by github actions
RUN groupadd -r github -g 127 && \
   useradd -r -g github -u 1001 -m -d /home/github -s /sbin/nologin github

# github mounts /home/runner/work as /__w, map it back to /home/runner/work
# in the container so that ${{ github.workspace }} is correct.
RUN mkdir -p /__w && mkdir -p /home/runner && ln -sf /__w /home/runner/work
