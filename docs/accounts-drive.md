# Ledger, accounts drive and proofs

The state is one 64 MiB NVRAM, `/dev/uio0` (`[nvrams.state]` in `cartesi.toml`), stored as
`000000008c000000-4000000.bin` in the template and in every node snapshot:

| offset | content | libcma mode |
|---|---|---|
| 0 … 4 MiB | **accounts drive**: 2^17 records of 32 bytes `[uint96 LE balance \| 20-byte address]` = `LibUsdAccount` encoding (`log2_max_num_of_accounts = 17`, `log2_leaves_per_account = 0`) | file-backed, create-or-open (`cma_ledger_init_single_file` on the device, offset 0) |
| 4 … 32 MiB | single-asset ledger heap (account maps, virtual balances) + libcma's integrity footer | same region |
| 32 … 60 MiB | multi-asset ledger for every other asset (Ether, other ERC-20s, ERC-721, ERC-1155), never withdrawable on-chain | file-mode layout: `cma_ledger_init_file` at this offset on the host; in the machine `cma_ledger_init_buffer` over the heap part plus the footer written by the app (a UIO range cannot be mapped at an offset) |
| 60 MiB … +4 KiB | meta page: progress, rejected count, a reserved byte, batch submitter, token, nonce count | the app's own ([sequencer.md](sequencer.md)) |
| 60 MiB + 4 KiB … 64 MiB | user-op nonce table (32-byte slots) | the app's own |

Why region B is laid out this way: a UIO device cannot be mapped at a byte offset (the mmap offset
selects a map index; measured: `EINVAL` at offset 32 MiB), `/dev/mem` is closed by
`CONFIG_IO_STRICT_DEVMEM` on the Cartesi kernel, so libcma's file-backed mode can only own the
region at offset 0 of the range; and libcma's buffer mode creates a fresh ledger on every init, so a
host sequencer reopening a dump needs file mode. The app therefore reproduces file mode's bytes in
the machine (buffer mode sized to the footer offset + the footer), which the host unit tests pin
against libcma. The asset of region A is pinned in the meta page at genesis (the `config` range of
the sequencer variant, `QA_TOKEN` otherwise). Empty records are all-zero (libcma compacts on
withdraw-to-zero) and the `uint96` balance width matches the contract's limit.

## Read and prove the accounts drive of a stored snapshot

Only provided tools: libcma's **`account-driver-reader`** (machine-asset-tools `v0.1.0-alpha.11`)
for the records and the account proof inside the 4 MiB accounts drive, and the emulator CLI for
the proof of that drive up to the machine root.

### Build the reader (host, amd64/arm64)

```sh
make deps                                                      # clones machine-asset-tools v0.1.0-alpha.11 into third-party/
sudo apt-get install -y libslirp0                              # libcartesi's runtime dependency
make -C third-party/machine-asset-tools account-driver-reader  # -> third-party/machine-asset-tools/build/$(uname -m)/tools/account-driver-reader
```

### Dump and prove

```sh
SNAP=<stored machine dir>                                  # a node snapshot or .cartesi/image
cp $SNAP/000000008c000000-4000000.bin state.bin            # the state NVRAM; copy it, the tool opens it read-write
OPTS="--single --n-accounts 131072 --mem-length 33554432 --token-address $TOKEN"
account-driver-reader $OPTS --dump state.bin | jq .        # every non-empty record (owner, uint96 balance, index, drive_offset)
account-driver-reader $OPTS --dump-proof state.bin $USER | jq .
                                                           # account proof: index, 32-byte record (target_data), 17 siblings,
                                                           # root_hash = accounts-drive root  (-> `withdraw`)
cartesi-machine --load=$SNAP --max-mcycle=0 \
    --initial-proof=address:0x8c000000,log2_size:22,filename:drive-root-proof.json
                                                           # drive-root proof (42 siblings up to the machine root, hashes
                                                           # base64)  (-> `prove-drive-root`). The proof is written, then
                                                           # the CLI exits with a harmless "mcycle is past".
```

The chain closes when `--dump-proof`'s `root_hash` equals the `target_hash` of the emulator proof,
whose `root_hash` equals `cartesi-machine-stored-hash $SNAP`.

The node's own tool reads the same objects:
`cartesi-rollups-machine-tool prove accounts-drive --accounts-drive-start-index 560` (node
`v2.0.0-alpha.13`) writes `drive-root-proof.json` and `account-proof.json` for a flash-drive
accounts drive.

### Tool status (alpha.11)

- Fixed in alpha.11: `--dump` and `--dump-proof` now emit valid JSON (`drive_offset` is a quoted
  hex string, the `"root_hash"` key is well formed), so they can be piped into `jq`.
- Still open upstream: `--dump-full-proof` links emulator `0.20.0` (`MACHINE_EMULATOR_VERSION ?=
  0.20.0`, header `cartesi-machine/machine-c-api.h`, renamed `cm.h` in 0.21) and cannot load
  `0.21` snapshots (`archive_version 7`); use the emulator's `--initial-proof` above instead.
- `cartesi-rollups-machine-tool prove accounts-drive` only searches the stored machine's
  `flash_drive` list and reports `accounts drive not found` for an NVRAM range; the emulator proof
  above is the same object it would have produced.
- Region B (32 … 60 MiB) has libcma's file-mode layout: `account-driver-reader` could read it
  with the multi-asset options at that offset of `000000008c000000-4000000.bin`.
