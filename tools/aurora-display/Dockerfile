FROM debian:trixie
RUN apt-get update && apt-get install -y --no-install-recommends build-essential bc bison flex libssl-dev libelf-dev clang llvm lld libclang-dev bindgen dwarves python3 python3-venv rsync cpio kmod git curl ca-certificates zstd xz-utils pkg-config device-tree-compiler && rm -rf /var/lib/apt/lists/*
WORKDIR /src
