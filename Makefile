# qa-wallet-tester-app
#
#   make                 guest build (inside the riscv64 builder image; what the Dockerfile runs)
#   make host            native build against the libcmt mock + libcma built from source
#   make host-test       native unit tests + mock-rollup end-to-end check (needs cast)
#   make image           `cartesi build` (needs Docker with riscv64 emulation; see docs/build.md)
#
# Sequencer variant (docs/sequencer.md):
#   make seq-host        the sequencer binary with libqawallet linked (seq/host, cargo + libclang + cmake)
#   make seq-test        cargo tests of the scheduler fold over the C seam (fee port, batches, user ops)
#   make seq-guest-lib   cross-builds the fold staticlib for riscv64 on the host (the Dockerfile does the same)
#   make seq-image       `cartesi build -c cartesi-seq.toml`
#   make seq-cycle0      the machine stored at mcycle 0 (release asset), from the seq build's config
#   make seq-kit         build/qa-wallet-seq-devnet-linux-<arch>.tar.gz: binaries + images + devnet state (docs/sequencer-kit.md)
#
# Host builds expect the pinned dependencies under third-party/ (see `make deps`).

CXX ?= g++
AR  ?= ar
ARCH := $(shell uname -m)

WARN_CXXFLAGS = -Wall -Wextra -Wpedantic -Wformat -Werror=format-security -Wno-missing-field-initializers
OPT_CXXFLAGS ?= -O2
HARDEN_CXXFLAGS = -fhardened -fno-strict-aliasing -fno-strict-overflow
CXXFLAGS += -std=c++20 $(WARN_CXXFLAGS) $(OPT_CXXFLAGS) -Iinclude $(EXTRA_CXXFLAGS)

LIB_SRC := src/lib/types.cpp src/lib/config.cpp src/lib/ledger.cpp src/lib/state.cpp src/lib/fee.cpp src/lib/engine.cpp src/lib/app_engine.cpp
MAIN_SRC := src/main.cpp
SEQ_MAIN_SRC := src/seq_main.cpp
GENESIS_SRC := src/genesis_main.cpp

# ---------------------------------------------------------------------------- guest (riscv64)
# Headers and libraries come from the packages installed in the builder stage:
#   /usr/include/libcmt, /usr/include/libcma, /usr/include/nlohmann, libcmt.a, libcma.a
guest_OBJDIR := build/guest
guest_OBJ := $(patsubst %.cpp,$(guest_OBJDIR)/%.o,$(LIB_SRC))
guest_MAIN_OBJ := $(guest_OBJDIR)/src/main.o
guest_SEQ_MAIN_OBJ := $(guest_OBJDIR)/src/seq_main.o
guest_LIB := $(guest_OBJDIR)/libqawallet.a
guest_BIN := $(guest_OBJDIR)/qa-wallet
guest_SEQ_BIN := $(guest_OBJDIR)/qa-wallet-seq
guest_SEQ_FOLD := $(guest_OBJDIR)/seq/libqawallet_seq_guest.a

ifeq ($(ARCH),riscv64)
all: guest
else
all: host
endif

guest: $(guest_BIN)

$(guest_OBJDIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(HARDEN_CXXFLAGS) -Iseq/guest/include -MMD -MP -c -o $@ $<

$(guest_LIB): $(guest_OBJ)
	$(AR) rcs $@ $^

$(guest_BIN): $(guest_MAIN_OBJ) $(guest_LIB)
	$(CXX) $(CXXFLAGS) $(HARDEN_CXXFLAGS) -static -o $@ $(guest_MAIN_OBJ) $(guest_LIB) -l:libcma.a -l:libcmt.a

# The sequencer guest: the C++ driver, the engine, the Rust fold (built by the Dockerfile's seq-rust
# stage or `make seq-guest-lib`) and the C libraries, linked as one group since the fold calls the
# engine and the driver calls both.
guest-seq: $(guest_SEQ_BIN)

$(guest_SEQ_BIN): $(guest_SEQ_MAIN_OBJ) $(guest_LIB) $(guest_SEQ_FOLD)
	$(CXX) $(CXXFLAGS) $(HARDEN_CXXFLAGS) -static -Wl,--gc-sections -o $@ $(guest_SEQ_MAIN_OBJ) \
	  -Wl,--start-group $(guest_LIB) $(guest_SEQ_FOLD) -l:libcma.a -l:libcmt.a -Wl,--end-group

# ---------------------------------------------------------------------------- host (native)
GUEST_TOOLS_DIR ?= third-party/machine-guest-tools
LIBCMA_DIR      ?= third-party/machine-asset-tools
host_LIBCMT_A   := $(GUEST_TOOLS_DIR)/sys-utils/libcmt/build/mock/libcmt.a
host_LIBCMA_A   := $(LIBCMA_DIR)/build/$(ARCH)/libcma.a
host_INC := -I$(GUEST_TOOLS_DIR)/sys-utils/libcmt/include -I$(LIBCMA_DIR)/include -I$(LIBCMA_DIR)/third-party

host_OBJDIR := build/host
host_OBJ := $(patsubst %.cpp,$(host_OBJDIR)/%.o,$(LIB_SRC))
host_MAIN_OBJ := $(host_OBJDIR)/src/main.o
host_LIB := $(host_OBJDIR)/libqawallet.a
host_ENGINE_LIB := $(host_OBJDIR)/libqawallet-host.a
host_BIN := $(host_OBJDIR)/qa-wallet
host_GENESIS := $(host_OBJDIR)/qa-wallet-genesis
host_TEST := $(host_OBJDIR)/unit-tests

host: $(host_BIN) $(host_GENESIS) $(host_TEST) $(host_ENGINE_LIB)

$(host_GENESIS): $(host_OBJDIR)/src/genesis_main.o $(host_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $< $(host_LIB) $(host_LIBCMA_A) $(host_LIBCMT_A)

# One archive with the engine, libcma and libcmt's ABI/keccak helpers (the mock build; libcma's
# parser uses them): what the sequencer links (APPLICATION_ENGINE_LIB) and what the cargo tests of
# seq/guest link.
$(host_ENGINE_LIB): $(host_LIB) $(host_LIBCMA_A) $(host_LIBCMT_A)
	printf 'create %s\naddlib %s\naddlib %s\naddlib %s\nsave\nend\n' $@ $(host_LIB) $(host_LIBCMA_A) $(host_LIBCMT_A) | $(AR) -M

$(host_OBJDIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(host_INC) -g -MMD -MP -c -o $@ $<

$(host_LIB): $(host_OBJ)
	$(AR) rcs $@ $^

$(host_BIN): $(host_MAIN_OBJ) $(host_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(host_MAIN_OBJ) $(host_LIB) $(host_LIBCMA_A) $(host_LIBCMT_A)

$(host_TEST): $(host_OBJDIR)/tests/host/unit_tests.o $(host_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $< $(host_LIB) $(host_LIBCMA_A) $(host_LIBCMT_A)

host-test: host
	$(host_TEST)
	tests/host/mock_e2e.sh $(host_BIN)

# Pinned dependency sources for host builds (network).
GUEST_TOOLS_TAG ?= v0.18.0
LIBCMA_TAG ?= v0.1.0-alpha.11
deps:
	@mkdir -p third-party
	@test -d $(GUEST_TOOLS_DIR) || git clone -q --depth 1 --branch $(GUEST_TOOLS_TAG) https://github.com/cartesi/machine-guest-tools.git $(GUEST_TOOLS_DIR)
	@test -d $(LIBCMA_DIR) || git clone -q --depth 1 --branch $(LIBCMA_TAG) https://github.com/Mugen-Builders/machine-asset-tools.git $(LIBCMA_DIR)
	$(MAKE) -C $(GUEST_TOOLS_DIR)/sys-utils/libcmt mock
	$(MAKE) -C $(LIBCMA_DIR) third-party libcma

# The sequencer variant adds the rollups-contracts devnet (anvil state + deployment addresses,
# tests/sequencer and the devnet kit) and the pinned crates.
ANVIL_DUMP := cartesi-rollups-contracts-3.0.0-alpha.10-anvil-1.5.1
ANVIL_DUMP_URL := https://github.com/cartesi/rollups-contracts/releases/download/v3.0.0-alpha.10/$(ANVIL_DUMP).tar.gz
ANVIL_DUMP_SHA256 := fb38dc6e1faf238a152453dfd351ae5899d99fd35e9c675b4dce97cd6b05a68b
ANVIL_DUMP_DIR := third-party/$(ANVIL_DUMP)
seq-deps:
	@mkdir -p third-party
	@test -f $(ANVIL_DUMP_DIR).tar.gz || curl -fsSL -o $(ANVIL_DUMP_DIR).tar.gz $(ANVIL_DUMP_URL)
	echo "$(ANVIL_DUMP_SHA256)  $(ANVIL_DUMP_DIR).tar.gz" | sha256sum -c -
	@test -f $(ANVIL_DUMP_DIR)/state.json || (mkdir -p $(ANVIL_DUMP_DIR) && tar -xzf $(ANVIL_DUMP_DIR).tar.gz -C $(ANVIL_DUMP_DIR))
	cargo fetch --locked --manifest-path $(SEQ_MANIFEST)

# ---------------------------------------------------------------------------- image
image:
	cartesi build

# ---------------------------------------------------------------------------- sequencer (seq/)
# cargo needs libclang for bindgen (apt libclang-dev, or LIBCLANG_PATH) and cmake for the host.
SEQ_MANIFEST := seq/Cargo.toml
SEQ_ENGINE_ENV := APPLICATION_ENGINE_LIB=$(abspath $(host_ENGINE_LIB)) APPLICATION_ENGINE_HEADER=$(abspath include/qawallet/application-engine.h)
SEQ_HOST_BIN := seq/target/release/qa-wallet-sequencer

seq-host: $(host_ENGINE_LIB)
	$(SEQ_ENGINE_ENV) cargo build --locked --release --manifest-path $(SEQ_MANIFEST) -p qawallet-seq-host

seq-test: $(host_ENGINE_LIB)
	QAWALLET_LIB_DIR=$(abspath $(host_OBJDIR)) cargo test --locked --manifest-path $(SEQ_MANIFEST) -p qawallet-seq-guest

# bindgen parses application-engine.h for the riscv64 target on the build host: it only needs
# <stdint.h>, served from seq/guest/bindgen-include so no host libc header is involved.
BINDGEN_RISCV_ARGS ?= -nostdinc -isystem $(abspath seq/guest/bindgen-include)
seq-guest-lib:
	BINDGEN_EXTRA_CLANG_ARGS_riscv64gc_unknown_linux_musl="$(BINDGEN_RISCV_ARGS)" \
	  cargo build --locked --profile guest --target riscv64gc-unknown-linux-musl --manifest-path $(SEQ_MANIFEST) -p qawallet-seq-guest
	@mkdir -p $(guest_OBJDIR)/seq
	cp seq/target/riscv64gc-unknown-linux-musl/guest/libqawallet_seq_guest.a $(guest_SEQ_FOLD)

# The 4 KiB config range of the devnet build: the batch submitter (anvil account 9) and the
# accounts-drive token (TestUsdc), each 20 bytes, then zeros.
DEVNET_SEQUENCER ?= a0Ee7A142d267C1f36714E4a8F75612F20a79720
DEVNET_TOKEN ?= 7a051EDffC0884cd88d4a377F4C87BE074CF6c81
seq/config-devnet.bin:
	printf '%s%s' "$(DEVNET_SEQUENCER)" "$(DEVNET_TOKEN)" | xxd -r -p > $@ && truncate -s 4096 $@

seq-image: seq/config-devnet.bin
	cartesi build -c cartesi-seq.toml

# Stores the seq build's machine at mcycle 0 (docs/sequencer.md): the CLI config is reduced to the
# fields a fresh instantiation needs, then the machine is built but not run.
SEQ_CYCLE0 := build/seq/cycle0
LINUX_BIN ?= /usr/share/cartesi-machine/images/linux.bin
# jq: keep the boot strings and the ranges, name their files; drop everything a fresh machine recomputes.
SEQ_CYCLE0_FILTER := (.config // .) | {dtb: {bootargs: .dtb.bootargs, init: .dtb.init, entrypoint: .dtb.entrypoint}, ram: {length: .ram.length, backing_store: {data_filename: $$linux}}, flash_drive: (.flash_drive | map({label, start, length, read_only, backing_store: {data_filename: ($$root + "/" + .label + (if .label == "root" then ".ext2" else ".sqfs" end))}})), nvram: (.nvram | map({label, start, length, read_only} + (if .label == "config" then {backing_store: {data_filename: ($$root + "/config.raw")}} else {} end)))}
seq-cycle0:
	@mkdir -p build/seq && rm -rf $(SEQ_CYCLE0)
	jq --arg root "$(abspath .cartesi)" --arg linux "$(LINUX_BIN)" '$(SEQ_CYCLE0_FILTER)' .cartesi/image/config.json > build/seq/cycle0.json
	cartesi-machine --load-config=build/seq/cycle0.json,format:json --max-mcycle=0 --initial-hash --store=$(SEQ_CYCLE0)

# ---------------------------------------------------------------------------- devnet kit
# One archive with everything an anvil-based test of the sequencer variant needs: the host
# binaries, the devnet genesis image and the cycle-0 image, the host genesis dump, the config
# range, the devnet anvil state and addresses, and the instructions (docs/sequencer-kit.md).
# Needs `make seq-host seq-image seq-cycle0` first (the image is read from SEQ_IMAGE).
SEQ_IMAGE ?= .cartesi/image
SEQ_KIT := qa-wallet-seq-devnet-linux-$(ARCH)
SEQ_KIT_DIR := build/$(SEQ_KIT)
seq-kit: seq/config-devnet.bin
	rm -rf $(SEQ_KIT_DIR) && mkdir -p $(SEQ_KIT_DIR)/bin $(SEQ_KIT_DIR)/image $(SEQ_KIT_DIR)/config $(SEQ_KIT_DIR)/state $(SEQ_KIT_DIR)/devnet
	cp $(SEQ_HOST_BIN) $(host_GENESIS) $(SEQ_KIT_DIR)/bin/
	cp -r $(SEQ_IMAGE) $(SEQ_KIT_DIR)/image/genesis-devnet
	cp -r $(SEQ_CYCLE0) $(SEQ_KIT_DIR)/image/cycle0
	cartesi-machine-stored-hash $(SEQ_IMAGE) > $(SEQ_KIT_DIR)/image/template-hash.txt
	cp seq/config-devnet.bin $(SEQ_KIT_DIR)/config/config-devnet.bin
	QA_LOG=0 $(host_GENESIS) $(SEQ_KIT_DIR)/state/genesis-devnet --sequencer 0x$(DEVNET_SEQUENCER) --token 0x$(DEVNET_TOKEN)
	cmp $(SEQ_KIT_DIR)/state/genesis-devnet $(SEQ_IMAGE)/$$(jq -r '(.config // .).nvram[] | select(.label == "state") | .backing_store.data_filename' $(SEQ_IMAGE)/config.json)
	cp $(ANVIL_DUMP_DIR)/state.json $(SEQ_KIT_DIR)/devnet/ && cp -r $(ANVIL_DUMP_DIR)/deployments $(SEQ_KIT_DIR)/devnet/
	cp docs/sequencer-kit.md $(SEQ_KIT_DIR)/README.md
	cd $(SEQ_KIT_DIR) && find . -type f ! -name SHA256SUMS -print0 | LC_ALL=C sort -z | xargs -0 sha256sum > SHA256SUMS
	tar -C build --sparse --sort=name --owner=0 --group=0 --numeric-owner -czf build/$(SEQ_KIT).tar.gz $(SEQ_KIT)
	cd build && sha256sum $(SEQ_KIT).tar.gz | tee $(SEQ_KIT).tar.gz.sha256

-include $(wildcard build/host/*/*.d build/host/*/*/*.d build/guest/*/*.d build/guest/*/*/*.d)

clean:
	rm -rf build

.PHONY: all guest guest-seq host host-test deps seq-deps image clean seq-host seq-test seq-guest-lib seq-image seq-cycle0 seq-kit
