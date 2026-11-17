# qa-wallet-tester-app

A deliberately small C++ Cartesi Rollups application: a **libcma ledger and nothing else**. It
credits every portal deposit, executes libcma's withdrawal and transfer inputs (vouchers out),
answers libcma's balance inspects, keeps the withdrawable balances of one ERC-20 in an
**accounts drive** the v3 contracts can prove for emergency withdrawals, and **echoes any other
advance input back as a notice** with the exact input bytes. It comes in two variants: the
**classic** rollup driver, and the **sequencer** variant integrated with the
[Cartesi sequencer](https://github.com/cartesi/sequencer) `v0.1.0-alpha.10` like its canonical
app, where the same engine runs inside the machine behind the sequencer's scheduler fold and on the
host inside the sequencer binary ([docs/sequencer.md](docs/sequencer.md)).

It is the ledger-only sibling of the QA tester application (catalog
[`Mugen-Builders/cartesi-qa`](https://github.com/Mugen-Builders/cartesi-qa)) for the pinned stack:
node `v2.0.0-alpha.13`, contracts `v3.0.0-alpha.10`, emulator `0.22.0`, Cartesi CLI
`2.0.0-alpha.38` (SDK `0.12.0-alpha.44`), libcma (machine-asset-tools) `v0.1.0-alpha.11`,
machine-guest-tools `0.18.0`.

It is modelled on [`machine-asset-tools/sample_apps/wallet_app`](https://github.com/Mugen-Builders/machine-asset-tools)
(libcma + libcmt) but split in two: **`libqawallet`** processes inputs and knows nothing about the
rollup device; the drivers (`src/main.cpp`, `src/seq_main.cpp`) own the device. The library
implements the sequencer's
[`application-engine.h`](https://github.com/cartesi/sequencer/blob/v0.1.0-alpha.10/bindings/c-app-engine/include/application-engine.h)
C ABI (`src/lib/app_engine.cpp`): direct inputs and validated user ops in, vouchers and notices
drained out, the state in one memory range, `(count, clock)` progress in the state.

## What an input does

| advance input | effect | completion |
|---|---|---|
| sender is a portal (Ether, ERC-20, ERC-721, ERC-1155 single/batch) | decoded with libcma, credited to the sender's wallet account; JSON report with the decoded fields | accept; reject with a report when the payload does not decode or the accounts drive overflows (`uint96`) |
| one of libcma's 10 selectors: `WithdrawEther/Erc20/Erc721/Erc1155Single/Erc1155Batch`, `TransferEther/Erc20/Erc721/Erc1155Single/Erc1155Batch` | ledger debit; a withdrawal emits libcma's voucher, a transfer emits a JSON report | accept; reject with `{"error":…,"code":…}` on malformed arguments or overdraft |
| anything else (unknown selector, fewer than 4 bytes, empty) | **one notice with the exact input bytes** | accept (an input that fits the 2 MiB RX buffer always fits the TX buffer as a notice; a sink failure would reject with `{"error":"unable to emit echo notice","code":-105}`) |

Inspects: libcma's `ledger_getBalance` / `ledger_getTotalSupply`, `ledger_getNonce` (sequencer
user-op nonce), `qa_fee` (the sequencer fee for an exponent), the raw `state` report; any other
non-JSON query is echoed back as one report, any other JSON method rejects with an error report.
[docs/inputs.md](docs/inputs.md) has the encodings.

## Documentation

No helper scripts: every job is done with the provided tools, and the docs say how.

- [docs/build.md](docs/build.md): prerequisites, `cartesi build`/`hash`/`run`, the state NVRAM and
  the `accounts_drive_start_index` check, addresses and `QA_*` environment, CI and release artifacts.
- [docs/inputs.md](docs/inputs.md): encoding inputs with `cast`, sending them, deposits, the echo
  rule, inspect methods, what the node reports.
- [docs/accounts-drive.md](docs/accounts-drive.md): ledger layout, reading and proving the accounts
  drive of a snapshot with libcma's `account-driver-reader` and the emulator.
- [docs/sequencer.md](docs/sequencer.md): the sequencer variant: what differs, the state layout,
  building, genesis for a deployment (cycle-0 image + replaced `config` range), running the
  host sequencer, user ops, inspects against it (`qa-wallet-inspect`), tests and release assets.
- [docs/sequencer-kit.md](docs/sequencer-kit.md): the README of the devnet kit (`make seq-kit`, a
  release asset): host binaries + images + devnet state, and the anvil walkthrough.

## Layout

```
cartesi.toml               Cartesi CLI config (classic): drives root/app, [nvrams.state], machine env, [withdrawal.config]
cartesi-seq.toml           Cartesi CLI config (sequencer): [drives.app] -> app-seq target, [nvrams.config], the qa-wallet-seq entrypoint
Dockerfile                 base (alpine + guest tools 0.18.0) / seq-rust (fold cross-build) / builder / app, app-seq
Makefile                   guest build, host build (libcmt mock + libcma), host-test, deps, seq-* targets
include/qawallet/*.h       public API: types, sink, config, ledger, state, fee, engine; the vendored
                           application-engine.h and the seq_ext.h extensions
src/lib/*.cpp              the library (app_engine.cpp = the C seam)
src/main.cpp               classic rollup driver (libcmt): sink, accept/reject yields
src/seq_main.cpp           sequencer guest driver: device + the scheduler fold (seq/guest) over the seam
src/genesis_main.cpp       qa-wallet-genesis: the host genesis dump
seq/                       cargo workspace: guest (the fold as a C staticlib), host (qa-wallet-sequencer), inspect (qa-wallet-inspect, the inspect follower)
tests/host/                C++ unit tests + mock-rollup end-to-end (`make host-test`)
tests/machine/             pytest driving the stored templates with the host cartesi-machine (both variants)
tests/sequencer/           pytest: anvil + host sequencer + inspect follower + machine replay end to end
docs/                      how to build, send inputs, prove the accounts drive and run the sequencer variant
```

## Quick start

```sh
make deps && make host-test        # host: unit checks + mock e2e (needs g++ 14, cast)
cartesi build && cartesi hash      # classic template with the state NVRAM (needs Docker + Cartesi CLI)
pytest tests/machine -v            # machine tests of the variant in .cartesi/image (cartesi-machine 0.22.0, cast)
cartesi run --epoch-length 10 --claim-staging-period 0

make seq-deps seq-host seq-test    # sequencer variant: host binary + fold tests (Rust 1.95, libclang, cmake)
make seq-inspect seq-inspect-test  # the inspect follower of the host sequencer and its tests
make seq-image && cartesi hash     # cartesi build -c cartesi-seq.toml (replaces .cartesi/image)
make seq-cycle0 seq-kit            # cycle-0 image, then build/qa-wallet-seq-devnet-linux-<arch>.tar.gz (docs/sequencer-kit.md)
pytest tests/machine/test_seq_machine.py tests/sequencer -v
```

## Design decisions

- No application commands: the only inputs that act are libcma's (deposits by sender, the 10
  withdraw/transfer selectors by their first 4 bytes). Everything else is echoed as a notice, so
  any payload can be sent and observed as an output. A payload that *starts* like a ledger input
  but does not decode is a ledger error (reject with a report), never an echo.
- One engine behind the sequencer's `application-engine.h`; the classic driver and the sequencer
  guest driver differ only in who decides the order (arrival vs the sequencer's scheduler fold
  compiled from `sequencer-core`). The fee port is bit-exact with `sequencer-core/src/fee.rs`
  (tested for every exponent).
- Sequencer variant: every direct input is accepted, a rejected ledger input is a counted no-op
  (its report stays, its queued outputs are dropped). The batch submitter address and the
  accounts-drive ERC-20 come from a 4 KiB `config` NVRAM read at boot and pinned in the state at
  genesis. Nothing is machine-only: the seam carries every output (notices and vouchers).
- ABI selectors for advance inputs; libcma JSON for inspect; raw `state` query reserved.
- Addresses via machine env with devnet defaults (token configurable).
- Every portal deposit is credited; only the configured ERC-20 is on the accounts drive.
- State in one NVRAM, unchanged from the tester (a tester dump opens here): accounts drive +
  single-asset ledger file-backed at its start (readable and provable by libcma's
  `account-driver-reader`), multi-asset ledger in libcma's file-mode layout after it (so a host dump
  reopens), then the meta page (progress, submitter, token) and the nonce table; no custom
  accounts-drive tooling, no helper scripts. The tester's deposit-policy byte stays reserved (zero).
- 2^17 accounts (machine-tool default). Withdrawals are libcma's plain vouchers (`transfer`,
  `safeTransferFrom`); the emergency path is the on-chain accounts-drive proof.
- Reject on the real machine never returns (host restores the snapshot); the driver's reject path
  reads the next request from the yield reply only so the libcmt mock e2e can continue.

## Known gaps

- Not verified in this sandbox: `cartesi run` + node JSON-RPC observations, foreclosure and
  `prove accounts-drive` end to end (needs the CLI's compose stack); the sequencer variant's full
  stack with the rollups node is a documented manual procedure (docs/sequencer.md).
- The sequencer's cockroach-recovery exporter is not drilled: a machine checkpoint's `state`
  range is the native dump format, nothing else is needed.
- ERC-721 / ERC-1155 withdrawals use libcma's vouchers (`safeTransferFrom` from the app contract);
  there is no emergency builder for them in contracts alpha.10.
- Upstream tool limits are listed in [docs/accounts-drive.md](docs/accounts-drive.md#tool-status-alpha11).
