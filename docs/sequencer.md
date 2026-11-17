# The sequencer variant

qa-wallet-app has a second build integrated with the [Cartesi sequencer](https://github.com/cartesi/sequencer)
(`v0.1.0-alpha.10`) the way its `examples/canonical-app` is. The same engine (`libqawallet`) runs
in two places:

- **inside the machine** (`qa-wallet-seq`, `cartesi-seq.toml`): the C++ driver owns the rollup
  device and hands every advance input to the sequencer's canonical scheduler fold
  (`sequencer_core::scheduler::Scheduler`, compiled into `seq/guest`), which reaches the engine
  through the sequencer's `application-engine.h` C ABI (`src/lib/app_engine.cpp`);
- **on the host** (`qa-wallet-sequencer`, `seq/host`): the sequencer's generic C-application host
  with `libqawallet` linked through the same ABI, giving soft confirmations, posting batches to the
  InputBox and keeping checkpoints.

Inputs from the batch submitter are SSZ batches; every other sender is a **direct input** that
waits in the scheduler's queue until a frame's `safe_block` covers it, or until it is overdue
(`MAX_WAIT_BLOCKS = 1200` blocks). Like the canonical wallet, every libcma withdrawal/transfer is
accepted as a direct input, not only deposits; so is every echoed payload. Signed user ops
(`POST /tx`) carry the same payloads.

## What changes against the classic variant

| | classic (`cartesi.toml`) | sequencer (`cartesi-seq.toml`) |
|---|---|---|
| execution order | every input runs on arrival | scheduler fold: directs drained at frames or when overdue, batches validated (nonce, safe blocks, staleness) |
| overdraft, malformed ledger input, undecodable deposit | `RX_REJECTED`, state reverted by the host | **counted no-op**: the input is included, progress advances, the error report stays, queued notices/vouchers are dropped |
| echo notices, withdrawal vouchers | outputs | the same outputs, carried by the C ABI to the host sequencer; nothing is machine-only, state and outputs are identical on both sides |
| user ops | none | validated (nonce, fee balance) and executed; the fee `fee::to_linear(fee)` in the accounts-drive token goes to the batch submitter's account; a business failure still consumes the nonce and the fee |
| sender metadata | full `EvmAdvance` | the seam carries sender, block and payload; the guest driver passes timestamp/index/chain/app for reports and ERC-721/1155 voucher encoding, the host has zeros (state never depends on them) |
| deployment data | `[machine.env]` | the batch submitter and the accounts-drive ERC-20 come from the 4 KiB `config` range and are pinned in the state at genesis; the host engine takes the pinned token over `QA_TOKEN` |

Progress (`executed_input_count`, `last_executed_safe_block`), the batch submitter address, the
token and the user-op nonces live in the state range (below), so a restarted host engine and the
machine always agree.

## State layout

One 64 MiB NVRAM, `state` (`/dev/uio0` at `0x8c000000`): it is the host engine's dump
(`state_file_in_dump`), the bytes `GET /finalized_state` serves and what the tests compare:

```
[0, 32 MiB)        region A: single-asset ledger for the configured ERC-20 (cma_ledger_init_single_file, libcma file-backed)
                   [0, 4 MiB) accounts drive = 2^17 LibUsdAccount records, provable on-chain (index 560)
[32 MiB, 60 MiB)   region B: multi-asset ledger for every other asset, not withdrawable on-chain (libcma file-mode layout)
[60 MiB, +4 KiB)   region C: meta page   (include/qawallet/state.h)
[60 MiB + 4 KiB, 64 MiB)      nonce table, 32-byte slots
config (/dev/uio1, 0x90000000, 4 KiB): bytes 0..20 the batch submitter, 20..40 the ERC-20 (zero = QA_TOKEN / TestUsdc), the rest zero
```

Meta page: `QATESTR1` magic, layout version 1, `executed_input_count` (u64 @16),
`last_executed_safe_block` (u64 @24), `rejected_count` (u64 @32), a reserved zero byte @40 (the
tester's deposit policy; the layout is the tester's, so its dumps open here), sequencer address
(20 B @48), token (20 B @68), `nonce_count` (u64 @88), little-endian. The nonce
table is open addressing on the address's low 32 bits with linear probing, refusing new senders at
50 % load (a refused sender is reported as an uncoverable fee).

Region B has libcma's *file-mode* layout so the host reopens a dump with `cma_ledger_init_file` at
offset 32 MiB. A UIO range cannot be mapped at a byte offset, so the machine builds the same bytes
with `cma_ledger_init_buffer` over the heap part and writes libcma's 32-byte footer (version,
length, static prefix, canary) itself (`src/lib/ledger.cpp`); `make host-test` pins that against
libcma's own file mode byte for byte and reopens a machine-built region on the host path.

## Build

```sh
make deps seq-deps                 # pinned C libraries, the devnet anvil state, cargo fetch
make host-test                     # engine + C seam unit tests and the mock e2e
make seq-host                      # seq/target/release/qa-wallet-sequencer (needs libclang, cmake)
make seq-inspect                   # seq/target/release/qa-wallet-inspect, the inspect follower (same needs)
make seq-test                      # cargo tests of the fold over the seam (every fee exponent, batches, user ops)
make seq-image                     # cartesi build -c cartesi-seq.toml  -> .cartesi/image (devnet genesis)
cartesi hash                       # the devnet template hash
make seq-cycle0                    # build/seq/cycle0: the same machine stored at mcycle 0
pytest tests/machine/test_seq_machine.py -v
pytest tests/sequencer -v          # anvil + host sequencer + machine replay (below)
```

Host prerequisites on top of [build.md](build.md): Rust 1.95.0 with the
`riscv64gc-unknown-linux-musl` target (`seq/rust-toolchain.toml`), `libclang-dev` (bindgen; or
`LIBCLANG_PATH`), `cmake` and `build-essential` (the sequencer's SQLite and TLS crates).
`cartesi build` cross-compiles the fold in the Dockerfile's `seq-rust` stage (`rust:1.95-bookworm`
on the build platform), then links it with Alpine's riscv64 toolchain; `make seq-guest-lib` does the
Rust half on the host. The `.cartesi/` directory holds one variant at a time (`make image` vs
`make seq-image`); CI archives between builds.

`include/qawallet/application-engine.h` is a verbatim copy of the tag's header
(sha256 `5b0a1bbe44012d4b45aa82335236da8f39428118b9eb2bbf33152b9d5123280d`); `seq/Cargo.lock`
is the sequencer's own lockfile pruned to these crates. Bump both together.

## Genesis for a deployment

The batch submitter address and the accounts-drive ERC-20 are deployment data. The release ships
the machine **stored at mcycle 0** (`qa-wallet-seq-cycle0.tar.gz`) with the devnet values in its
`config` range, and the devnet genesis (`qa-wallet-seq-genesis-devnet.tar.gz`). For another
deployment:

```sh
SEQUENCER=0x...                                                   # the key the sequencer submits batches from
TOKEN=0x...                                                       # the ERC-20 of the accounts drive (or 20 zero bytes: QA_TOKEN / TestUsdc)
printf '%s%s' "${SEQUENCER#0x}" "${TOKEN#0x}" | xxd -r -p > config.bin && truncate -s 4096 config.bin
tar -xzf qa-wallet-seq-cycle0.tar.gz -C cycle0                    # one directory with config.json inside
cartesi-machine --load=cycle0 --replace-memory-range=label:config,data_filename:$PWD/config.bin \
    --assert-rolling-template --final-hash --store=genesis        # boots to the first yield: the genesis
cartesi-machine-stored-hash genesis                               # the template hash to deploy with
```

The first boot reads the range, writes the genesis state into the blank NVRAM (the meta page pins
both addresses, then the engine's first open lays out the libcma ledgers; a file genesis on the
host does both at once, the bytes are the same) and yields. The host side needs the same genesis:

```sh
qa-wallet-genesis state --sequencer $SEQUENCER --token $TOKEN     # 64 MiB sparse file (QA_* env as in [machine.env])
cmp state genesis/$(jq -r '(.config // .).nvram[] | select(.label == "state") | .backing_store.data_filename' genesis/config.json) \
    && echo same                                                  # the stored state range is the host dump
```

A machine booted with a `config` range that differs from what an initialised state pinned refuses
to start (`the state was created for another batch submitter or token`).
`tests/machine/test_seq_machine.py::test_replaced_config_range_pins_another_submitter_and_token`
drills exactly this flow on the cycle-0 image.

Reproducing the cycle-0 image from a build (what `make seq-cycle0` does): the CLI always boots to
the first yield, so the stored config is reduced to the fields a fresh instantiation needs
(`dtb.{bootargs,init,entrypoint}`, RAM, the drives and NVRAMs with their `.cartesi/` files; the
blank `state` NVRAM needs no file) and instantiated with `cartesi-machine --load-config=...
--max-mcycle=0 --store=...`. CI checks that booting that image to its yield prints the hash of
`cartesi hash`.

## Deploy and run

Deploy with the withdrawal config of `cartesi-seq.toml` (foreclosure is unchanged: the accounts
drive is still the first 4 MiB of the `state` range at index 560):

```sh
cast send $FACTORY 'newApplication(address,address,bytes32,address,(address,uint8,uint8,uint64,address))(address)' \
    0x0000000000000000000000000000000000000000 $OWNER $TEMPLATE_HASH $INPUT_BOX \
    "($GUARDIAN,0,17,560,$BUILDER)" --private-key $PK              # or cartesi run -c cartesi-seq.toml on the devnet
```

Then the sequencer (`setup` once, L1-read-only; `run` holds the key). The engine reads its
`QA_*` configuration (portals, sizes) from the environment exactly like the machine's
`[machine.env]`; on another chain set the same values for both. The token and the submitter come
from the genesis dump.

```sh
export CARTESI_SEQUENCER_FEE_ORACLE_FIXED_LOG_GAS_PRICE=0           # devnet: a fixed fee oracle
qa-wallet-sequencer --state-file state setup --data-dir data --eth-rpc-url http://127.0.0.1:8545 \
    --chain-id 31337 --app-address $APP --batch-submitter-address $SEQUENCER
qa-wallet-sequencer run --http-addr 127.0.0.1:3000 --data-dir data --eth-rpc-url http://127.0.0.1:8545 \
    --batch-submitter-private-key $SEQUENCER_KEY
```

Direct inputs are InputBox inputs and portal deposits as in [inputs.md](inputs.md); a user op is a
JSON `POST /tx` signed with EIP-712 (`UserOp(uint32 nonce,uint16 max_fee,bytes data)`, domain
`CartesiAppSequencer`/`1`/chain id/app address):

```sh
NONCE=$(curl -s "$SEQ/nonce?sender=$USER" | jq .next_nonce); FEE=$(curl -s $SEQ/fee | jq .suggested_max_fee)
DATA=0x42                                                         # not a ledger input: echoed as a notice (or a libcma withdrawal/transfer)
TYPED='{"types":{"EIP712Domain":[{"name":"name","type":"string"},{"name":"version","type":"string"},{"name":"chainId","type":"uint256"},{"name":"verifyingContract","type":"address"}],"UserOp":[{"name":"nonce","type":"uint32"},{"name":"max_fee","type":"uint16"},{"name":"data","type":"bytes"}]},"primaryType":"UserOp","domain":{"name":"CartesiAppSequencer","version":"1","chainId":31337,"verifyingContract":"'$APP'"},"message":{"nonce":'$NONCE',"max_fee":'$FEE',"data":"'$DATA'"}}'
SIG=$(cast wallet sign --private-key $PK --data "$TYPED")
curl -s -X POST $SEQ/tx -H 'content-type: application/json' \
    -d '{"message":{"nonce":'$NONCE',"max_fee":'$FEE',"data":"'$DATA'"},"signature":"'$SIG'","sender":"'$USER'"}'
```

The sender pays `fee::to_linear(frame fee)` units of the accounts-drive token (`qa_fee` inspect
prints the amount for an exponent), so deposit first. `ledger_getNonce [address]` reports the
nonce the engine expects; the `state` inspect report shows `scheduler.next_batch_nonce` and
`scheduler.queued_directs` in the machine.

### Inspects against the sequencer: `qa-wallet-inspect`

The sequencer has no inspect route. `qa-wallet-inspect` (`seq/inspect`, `make seq-inspect`) is a
separate process that follows it and answers inspects from the **soft-confirmed** state: the same
engine archive behind the same seam, advanced with the sequencer's own inputs.

```sh
qa-wallet-inspect --sequencer-url http://127.0.0.1:3000 --http-addr 127.0.0.1:3001
curl -s -X POST http://127.0.0.1:3001/inspect/$APP -d '{"method":"ledger_getBalance","params":["'$USER'","'$TOKEN'"]}'
# {"status":"Accepted","exception_payload":null,"reports":[{"payload":"0x…"}],"processed_input_count":7}
```

How it works:

1. It downloads `GET /latest_snapshot`, opens it with `application_engine_from_dump` (the engine
   takes its private working copy in `QA_ENGINE_WORKDIR`; the download is deleted) and checks the
   input count against the snapshot's `X-Executed-Input-Count`.
2. It subscribes to `/ws/subscribe` with the snapshot's era, recovery generation and that count,
   and applies every message as the sequencer's own restart replay does: a direct input with
   `{sender, block_number, payload}`, a user op with `{sender, fee, data}` and its `safe_block`,
   each through `sequencer_core`'s `execute_*` (exactly one input of progress or a panic). A user
   op is first validated under its signed nonce; the offset must be the engine's input count.
3. `POST /inspect/{application}` hands the raw body (up to 2 MiB) to the engine's inspect
   (`qawallet_live_inspect`) and returns the reports in the rollups node's JSON shape.
   `processed_input_count` is the engine's input count behind the answer.

Nothing is persisted: a restart is a new snapshot. What to expect:

| situation | behaviour |
|---|---|
| before the first snapshot is applied, and until the history the sequencer had at that moment is replayed | `503` (`/readyz` too; `/healthz` is `200` while the process runs) |
| the socket drops, the sequencer restarts or is away | the last state keeps answering; the follower reconnects from its own input count (pings every 15 s, 45 s of silence is a dead socket) |
| the sequencer refuses the position with `409` (it recovered and replaced soft history, the era changed, history is unavailable) | the state is discarded, `503` until a new snapshot is followed; **`processed_input_count` can go down**: answers given from the replaced suffix were about inputs that no longer exist |
| a feed gap, a user op that does not validate, an undecodable message, an engine error, a snapshot that does not open | the state is discarded and restored; after 3 such restores in a row the process exits non-zero rather than serve a state it cannot vouch for |
| `{application}` is not the sequencer's app address | `404` |
| a body over 2 MiB / more than 16 inspects in flight | `413` / `429` |

Differences from an inspect on the machine, all deliberate:

- the answer is about the sequencer's optimistic state, ahead of what L1 has settled;
- `state` is the host engine's report: `"state_backend":"file"` and no `scheduler` object (the
  follower has no scheduler: queued directs are not on the feed);
- `status` is always `Accepted`, like `qa-wallet-seq` in the machine: a rejected query (unknown
  method, `Account not found`, bad JSON) is told by its `{"error":…}` report. The engine's
  inspect entry does not return the completion, and the engine is not changed for this;
- every other report is byte for byte the machine's once the same inputs are final
  (`tests/sequencer/test_inspect_follower.py`).

Configuration: `--sequencer-url` (`http://` only, like the sequencer's own client; the feed is
`ws://` on the same address), `--http-addr` (default `127.0.0.1:3001`; the route has no
authentication, so keep it on a trusted interface), `--work-dir` for the snapshot download (default
`QA_ENGINE_WORKDIR`, else the system's temporary directory; during a restore it holds the archive and its unpacked
state, about twice the state's 64 MiB, next to the engine's sparse working copy), `--max-snapshot-bytes` (default 1 GiB), each also as `QA_INSPECT_*`. The `QA_*`
layout variables must be the sequencer's: with other sizes the snapshot does not open and the log
says so.

### Full stack on the devnet (manual)

`cartesi run -c cartesi-seq.toml --epoch-length 10 --claim-staging-period 0` brings up anvil and
the rollups node with the seq image; deploy nothing else (the CLI deploys the app with the withdrawal
config), read the app address from `cartesi status`, run the sequencer against
`http://127.0.0.1:8545` with anvil account 9's key, and send inputs. The node sees the same inputs
the test replays and must reach the same NVRAM bytes; `cartesi foreclose` + the accounts-drive proof
of [accounts-drive.md](accounts-drive.md) work unchanged on the `state` range. This procedure is
not part of the automated tests (they stop at the machine replay).

## Tests

- `make host-test`: meta page and nonce table, the fee port vectors, user-op validation/execution,
  the C seam round trip (`from_dump` leaves the source untouched, `create_dump` is restorable,
  rejected ledger inputs are counted no-ops, echoes queue as notices).
- `make seq-test` (`seq/guest/tests/fold.rs`): `fee::to_linear` equals `sequencer_core::fee` for
  **every** exponent; directs wait for a covering frame; wrong nonce/undecodable/safe-block-above
  batches are rejected; stale batches are skipped after the overdue direct runs; signed user ops
  execute once, pay the submitter and are silently skipped below the frame fee.
- `tests/machine/test_seq_machine.py`: the stored image under `cartesi-machine`, one input per
  process: ranges and index 560, deposits waiting for a frame, invalid batches reported as
  `scheduler dropped invalid batch`, ordered drains (echo, withdrawal), rejected ledger inputs as
  counted no-ops, transfers, signed user ops and fees, stale batches, inspects, the comparison
  bytes; and, when `build/seq/cycle0` exists, the deployer flow (replaced `config` range with
  another submitter and token, booted to the yield, equal to `qa-wallet-genesis --sequencer …
  --token …`).
- `make seq-inspect-test` (`seq/inspect/src/tests.rs`): feed messages execute in order and gaps,
  malformed fields and non-validating user ops are refused; inspects leave the state untouched;
  snapshot archives with traversal, extra, duplicate, non-regular, oversized or truncated entries
  are refused; the follower against a stub sequencer: `503` before a state, snapshot + backlog +
  live suffix, the route's `404`/`413`/`405`, resume after a dropped socket, and a
  `STALE_GENERATION` refusal answered by a fresh snapshot of the replaced history.
- `tests/sequencer/test_inspect_follower.py`: `qa-wallet-inspect` against the real sequencer: a
  user op visible before any batch, reports equal to the machine's after the L1 replay, inspects
  without trace, the follower's restart and the sequencer's.
- `tests/sequencer/test_e2e.py`: anvil with the rollups-contracts alpha.10 state, the app deployed
  with the withdrawal config, `qa-wallet-genesis` equal to the machine's ranges, `setup`/`run`,
  a deposit and an echoed input through the InputBox, a signed user op through `POST /tx`, batches
  landing from the submitter, every InputBox input replayed through the stored machine and the
  `state` range compared with `GET /finalized_state` (digest and bytes), plus the ledger facts
  behind them.
  The devnet state's blocks are months old, so the fixture first stamps a block with the wall clock,
  then mines one block per second: the sequencer stops when its safe head is older than
  `l1-read-stale-after-blocks * seconds-per-block` or more than `seconds-per-block` ahead of its
  own clock (the run uses `--seconds-per-block 6` to tolerate a burst of transactions).

## Release assets

`qa-wallet-seq-cycle0.tar.gz` (machine at mcycle 0, devnet submitter and token in the `config` range),
`qa-wallet-seq-genesis-devnet.tar.gz` (booted to the yield, the devnet template),
`qa-wallet-seq-app.sqfs`, and `qa-wallet-sequencer-linux-amd64.tar.gz` (the host sequencer and
`qa-wallet-genesis`), with sha256 files, next to the classic assets.

### The devnet kit

`make seq-kit` (after `make seq-host seq-inspect seq-image seq-cycle0`) packs everything an anvil-based test
needs into one archive, `build/qa-wallet-seq-devnet-linux-<arch>.tar.gz`: the host binaries, the
devnet template and the cycle-0 image, the host genesis dump (checked byte-equal to the template's
state range while packing), the `config` range, the rollups-contracts devnet state and addresses,
`SHA256SUMS`, and [sequencer-kit.md](sequencer-kit.md) as its `README.md`: the step-by-step
instructions (anvil, deployment, `setup`/`run`, direct inputs and user ops, replaying the InputBox
through the template and comparing with `GET /finalized_state`, another submitter from the cycle-0
image). The `build-and-release` workflow uploads the kit on every run (push or
`workflow_dispatch`) and attaches it to every `v*` release. The host binaries are glibc-dynamic builds of
the runner (`ubuntu-24.04`, glibc 2.39) and need `libstdc++`.
