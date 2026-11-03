# syntax=docker.io/docker/dockerfile:1
# qa-wallet-tester-app guest build.
#   base     -> root drive (alpine + cartesi guest tools); target of [drives.root]
#   seq-rust -> cross-builds the sequencer scheduler fold (seq/guest, pure Rust) for riscv64 musl on
#               the build platform; no riscv C toolchain needed, bindgen needs libclang
#   builder  -> compiles the static riscv64 binaries (qa-wallet, qa-wallet-seq)
#   app      -> app drive of the classic variant (qa-wallet); target of [drives.app] in cartesi.toml
#   app-seq  -> app drive of the sequencer variant (qa-wallet-seq); target of [drives.appseq] in cartesi-seq.toml
ARG IMAGE_NAME=riscv64/alpine
ARG IMAGE_TAG=3.22.3
ARG RUST_IMAGE=rust:1.95-bookworm
ARG MACHINE_GUEST_TOOLS_VERSION=0.18.0-r1
ARG MACHINE_ASSET_TOOLS_VERSION=0.1.0-alpha.11
ARG MACHINE_ASSET_TOOLS_DEV_TAR=https://github.com/Mugen-Builders/machine-asset-tools/releases/download/v${MACHINE_ASSET_TOOLS_VERSION}/machine-asset-tools_musl_riscv64_dev_v${MACHINE_ASSET_TOOLS_VERSION}.tar.gz
ARG MACHINE_ASSET_TOOLS_DEV_SHA256=746a0c950c7df37b67789c6059a229ee6e2e63a9df390cb79e872ffaf8f1c656
ARG NLOHMANN_JSON_VERSION=3.12.0
ARG NLOHMANN_JSON_SHA256=aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63

################################################################################
FROM --platform=linux/riscv64 ${IMAGE_NAME}:${IMAGE_TAG} AS base

ARG MACHINE_GUEST_TOOLS_VERSION
ADD --chmod=644 https://cartesi.github.io/linux-packages/apk/keys/cartesi-apk-key.rsa.pub /etc/apk/keys/cartesi-apk-key.rsa.pub
RUN echo "https://cartesi.github.io/linux-packages/apk/stable" >> /etc/apk/repositories \
 && apk update \
 && apk add --no-cache cartesi-machine-guest-tools=${MACHINE_GUEST_TOOLS_VERSION}

################################################################################
FROM --platform=$BUILDPLATFORM ${RUST_IMAGE} AS seq-rust

RUN apt-get update && apt-get install -y --no-install-recommends clang libclang-dev && rm -rf /var/lib/apt/lists/* \
 && rustup target add riscv64gc-unknown-linux-musl

WORKDIR /opt/build/seq
COPY seq/Cargo.toml seq/Cargo.lock seq/rust-toolchain.toml ./
COPY seq/guest ./guest
COPY seq/host ./host
# APPLICATION_ENGINE_LIB stays unset: the engine symbols are resolved by the riscv64 link below.
# bindgen parses application-engine.h for riscv64 with the <stdint.h> of seq/guest/bindgen-include.
RUN BINDGEN_EXTRA_CLANG_ARGS_riscv64gc_unknown_linux_musl="-nostdinc -isystem /opt/build/seq/guest/bindgen-include" \
    cargo build --locked --profile guest --target riscv64gc-unknown-linux-musl -p qawallet-seq-guest

################################################################################
FROM base AS builder

ARG MACHINE_GUEST_TOOLS_VERSION
RUN apk add --no-cache alpine-sdk cartesi-machine-guest-libcmt-dev=${MACHINE_GUEST_TOOLS_VERSION}

ARG MACHINE_ASSET_TOOLS_DEV_TAR
ARG MACHINE_ASSET_TOOLS_DEV_SHA256
ADD --checksum=sha256:${MACHINE_ASSET_TOOLS_DEV_SHA256} ${MACHINE_ASSET_TOOLS_DEV_TAR} /tmp/libcma-dev.tar.gz
RUN tar -xzf /tmp/libcma-dev.tar.gz -C / && rm /tmp/libcma-dev.tar.gz

ARG NLOHMANN_JSON_VERSION
ARG NLOHMANN_JSON_SHA256
ADD --checksum=sha256:${NLOHMANN_JSON_SHA256} https://github.com/nlohmann/json/releases/download/v${NLOHMANN_JSON_VERSION}/json.hpp /usr/include/nlohmann/json.hpp

WORKDIR /opt/build/app
COPY Makefile .
COPY include ./include
COPY src ./src
COPY seq/guest/include ./seq/guest/include
COPY --from=seq-rust /opt/build/seq/target/riscv64gc-unknown-linux-musl/guest/libqawallet_seq_guest.a ./build/guest/seq/libqawallet_seq_guest.a
RUN make guest guest-seq

################################################################################
FROM --platform=linux/riscv64 scratch AS app
COPY --from=builder /opt/build/app/build/guest/qa-wallet /qa-wallet

################################################################################
FROM --platform=linux/riscv64 scratch AS app-seq
COPY --from=builder /opt/build/app/build/guest/qa-wallet-seq /qa-wallet-seq
