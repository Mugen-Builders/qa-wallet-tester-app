# qa-wallet-tester-app sequencer devnet kit

Everything an anvil-based test of the sequencer variant of qa-wallet-tester-app needs, built from
one commit of [qa-wallet-tester-app](https://github.com/Mugen-Builders/qa-wallet-tester-app) against the Cartesi
sequencer `v0.1.0-alpha.10` and rollups-contracts `v3.0.0-alpha.10`. Nothing here is a script:
every step below is a stock tool invocation.

```
bin/qa-wallet-sequencer      the host sequencer with libqawallet linked in (linux, glibc, dynamic libstdc++)
bin/qa-wallet-inspect        follows a running sequencer and answers inspects from its soft state
bin/qa-wallet-genesis        writes a host genesis state for a batch submitter and a token
image/genesis-devnet/        the stored machine booted to its first yield with the devnet config: the template
image/template-hash.txt      its hash (what the application is deployed with)
image/cycle0/                the same machine stored at mcycle 0: replace the config range for another deployment
config/config-devnet.bin     the 4 KiB config range: bytes 0..20 the batch submitter, 20..40 the ERC-20, rest zero
state/genesis-devnet         the host genesis dump (64 MiB, sparse): byte-equal to the template's state range
devnet/state.json            anvil state with rollups-contracts v3.0.0-alpha.10 deployed
devnet/deployments/31337/    the devnet addresses (InputBox, ApplicationFactory, Erc20Portal, TestUsdc, ...)
SHA256SUMS                   sha256sum -c SHA256SUMS
```

Devnet values (anvil's default accounts):

| | address | key |
|---|---|---|
| batch submitter (account 9) | `0xa0Ee7A142d267C1f36714E4a8F75612F20a79720` | `0x2a871d0798f97d79848a013d4936a73bf4cc922c825d33c1cf7073dff6d409c6` |
| deployer / owner (account 0) | `0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266` | `0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80` |
| a user (account 1) | `0x70997970C51812dc3A010C7d01b50e0d17dc79C8` | `0x59c6995e998f97a5a0044966f0945389dc9e86dae88c7a8412f4603b6b78690d` |
| accounts-drive token (TestUsdc) | `0x7a051EDffC0884cd88d4a377F4C87BE074CF6c81` | mintable by anyone |

Tools: Foundry `anvil` and `cast` (1.4.x), `cartesi-machine` 0.22.0 (the stored images carry
their RAM, no kernel image is needed), `jq`, `curl`, `xxd`.

## 1. The devnet

```sh
anvil --host 127.0.0.1 --port 8545 --slots-in-an-epoch 1 --block-time 1 --load-state devnet/state.json &
cast rpc --rpc-url http://127.0.0.1:8545 evm_setNextBlockTimestamp $(date +%s)   # the dump's blocks are months old
sleep 3                                                                           # two wall-clock blocks before `setup` reads the safe head
```

`--slots-in-an-epoch 1` makes the `safe` block follow `latest` closely; `--block-time 1` mines a
block per second so frames (every five newly-safe blocks) and batch confirmations progress by
themselves. The sequencer refuses a safe head older than `l1-read-stale-after-blocks *
seconds-per-block` of wall-clock time, hence the timestamp and the pause: `setup` pins the
`safe` block it sees, which must already be a wall-clock one.

## 2. Deploy the application

The template hash is `image/template-hash.txt`; the withdrawal config is the one of
`cartesi-seq.toml` (guardian = account 0, 2^17 accounts of one leaf, accounts drive at index 560,
the devnet `RefundOutputBuilder`):

```sh
RPC=http://127.0.0.1:8545
PK0=0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80
FACTORY=$(cat devnet/deployments/31337/ApplicationFactory.txt)
INPUT_BOX=$(cat devnet/deployments/31337/InputBox.txt)
HASH=$(cat image/template-hash.txt)
CONFIG="(0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266,0,17,560,$(cat devnet/deployments/31337/RefundOutputBuilder.txt))"
SIG='newApplication(address,address,bytes32,address,(address,uint8,uint8,uint64,address))(address)'
APP=$(cast call --rpc-url $RPC $FACTORY "$SIG" 0x0000000000000000000000000000000000000000 0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266 $HASH $INPUT_BOX "$CONFIG")
cast send --rpc-url $RPC --private-key $PK0 $FACTORY "$SIG" 0x0000000000000000000000000000000000000000 0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266 $HASH $INPUT_BOX "$CONFIG"
echo $APP
```

## 3. The sequencer

`setup` once (L1 read only, takes the genesis dump), then `run` (holds the submitter key).
`CARTESI_SEQUENCER_FEE_ORACLE_FIXED_LOG_GAS_PRICE=0` is the fixed fee oracle for a devnet; the
short batch/poll settings are for tests, the defaults are production values (a batch stays open
for two hours).

```sh
export CARTESI_SEQUENCER_FEE_ORACLE_FIXED_LOG_GAS_PRICE=0
cp --sparse=always state/genesis-devnet genesis.state                      # setup reads it once; keep the original
bin/qa-wallet-sequencer --state-file genesis.state setup --data-dir data --eth-rpc-url $RPC --chain-id 31337 \
    --app-address $APP --batch-submitter-address 0xa0Ee7A142d267C1f36714E4a8F75612F20a79720
bin/qa-wallet-sequencer run --http-addr 127.0.0.1:3000 --data-dir data --eth-rpc-url $RPC \
    --batch-submitter-private-key 0x2a871d0798f97d79848a013d4936a73bf4cc922c825d33c1cf7073dff6d409c6 \
    --max-batch-open-seconds 3 --batch-submitter-idle-poll-interval-ms 300 --batch-submitter-confirmation-depth 1 --poll-interval-ms 300 &
curl -s http://127.0.0.1:3000/readyz; curl -s http://127.0.0.1:3000/history | jq .deployment
```

The engine reads its `QA_*` configuration (portal addresses, sizes) from the environment the way
the machine reads `[machine.env]`; the defaults are the devnet values, so leave `QA_*` unset. The
submitter and the token come from the genesis dump.

For inspects against the sequencer's soft-confirmed state, run the follower next to it (it restores
the sequencer's latest snapshot and applies its feed; nothing to set up, nothing kept on disk):

```sh
bin/qa-wallet-inspect --sequencer-url http://127.0.0.1:3000 --http-addr 127.0.0.1:3001 &
curl -s -X POST http://127.0.0.1:3001/inspect/$APP -d state | jq -r '.reports[0].payload' | xxd -r -p | jq .
```

## 4. Inputs

Direct inputs are InputBox inputs and portal deposits; every libcma withdrawal/transfer and every
echoed payload is accepted as a direct input. A deposit and an echoed input from account 1:

```sh
PK1=0x59c6995e998f97a5a0044966f0945389dc9e86dae88c7a8412f4603b6b78690d
USER=0x70997970C51812dc3A010C7d01b50e0d17dc79C8
TOKEN=$(cat devnet/deployments/31337/TestUsdc.txt); PORTAL=$(cat devnet/deployments/31337/Erc20Portal.txt)
cast send --rpc-url $RPC --private-key $PK1 $TOKEN 'mint(address,uint256)' $USER 1000000
cast send --rpc-url $RPC --private-key $PK1 $TOKEN 'approve(address,uint256)' $PORTAL 1000000
cast send --rpc-url $RPC --private-key $PK1 $PORTAL 'depositErc20Tokens(address,address,uint256,bytes)' $TOKEN $APP 1000000 0x
cast send --rpc-url $RPC --private-key $PK1 $INPUT_BOX 'addInput(address,bytes)' $APP 0xdeadbeef   # echoed as a notice
sleep 8; curl -s http://127.0.0.1:3000/history | jq .history.head           # 2 once the frame drained both
curl -s http://127.0.0.1:3000/finalized_state/digest | jq .                  # after the batch landed and confirmed
```

A user op is a JSON `POST /tx` signed with EIP-712 (domain `CartesiAppSequencer`/`1`/chain id/app,
type `UserOp(uint32 nonce,uint16 max_fee,bytes data)`); the sender pays the frame fee in the
accounts-drive token, so deposit first:

```sh
SEQ=http://127.0.0.1:3000
NONCE=$(curl -s "$SEQ/nonce?sender=$USER" | jq .next_nonce); FEE=$(curl -s $SEQ/fee | jq .suggested_max_fee)
DATA=0x42                                                                    # echoed as a notice
TYPED='{"types":{"EIP712Domain":[{"name":"name","type":"string"},{"name":"version","type":"string"},{"name":"chainId","type":"uint256"},{"name":"verifyingContract","type":"address"}],"UserOp":[{"name":"nonce","type":"uint32"},{"name":"max_fee","type":"uint16"},{"name":"data","type":"bytes"}]},"primaryType":"UserOp","domain":{"name":"CartesiAppSequencer","version":"1","chainId":31337,"verifyingContract":"'$APP'"},"message":{"nonce":'$NONCE',"max_fee":'$FEE',"data":"'$DATA'"}}'
SIG=$(cast wallet sign --private-key $PK1 --data "$TYPED")
curl -s -X POST $SEQ/tx -H 'content-type: application/json' \
    -d '{"message":{"nonce":'$NONCE',"max_fee":'$FEE',"data":"'$DATA'"},"signature":"'$SIG'","sender":"'$USER'"}'
curl -s "$SEQ/nonce?sender=$USER"
```

The input vocabulary (libcma `Withdraw*`/`Transfer*`, the echo rule, the inspects `state`,
`ledger_getNonce`, `qa_fee`, ...) is documented in the repository's `docs/inputs.md`.

## 5. Check the machine against the sequencer

Every InputBox input (deposits included: the portals go through the InputBox) replayed through the
template must reproduce the sequencer's finalized state. The input payload is the `bytes` of each
`InputAdded(address,uint256,bytes)` event: the full `EvmAdvance` calldata the machine consumes:

```sh
HEAD=$(curl -s $SEQ/finalized_state/digest | jq -r .inclusion_block)        # replay up to the accepted checkpoint
mkdir -p replay && n=0
cast logs --rpc-url $RPC --from-block 0 --to-block $HEAD --address $INPUT_BOX 'InputAdded(address,uint256,bytes)' $APP --json \
    | jq -r '.[].data' | while read -r data; do
        cast abi-decode 'f()(bytes)' "$data" | sed 's/^0x//' | xxd -r -p > replay/input-$n.bin; n=$((n + 1)); done
N=$(ls replay/input-*.bin | wc -l)
cartesi-machine --load=image/genesis-devnet --revert-mode=none --assert-rolling-template --final-hash --store=replay/machine \
    --cmio-advance-state=input:replay/input-%i.bin,input_index_begin:0,input_index_end:$N,output:replay/input-%i-output-%o.bin,report:replay/input-%i-report-%o.bin,format:json
STATE=$(jq -r '(.config // .).nvram[] | select(.label == "state") | .backing_store.data_filename' replay/machine/config.json)
sha256sum replay/machine/$STATE; curl -s $SEQ/finalized_state/digest | jq -r .sha256     # equal
curl -s -o finalized.state $SEQ/finalized_state && cmp finalized.state replay/machine/$STATE && echo same bytes
```

(`InputAdded`'s `bytes` is the ABI-encoded log data, which `cast abi-decode` unwraps; the payload
is the `EvmAdvance` calldata the machine consumes, batches from the submitter included. Newer
0.22.0 emulator builds spell the index range `input_file_index_begin` / `input_file_index_end`:
`cartesi-machine --help` lists the names, and `tests/machine/harness.py` picks them the same way.) The
`state` inspect of the replayed machine (`--cmio-inspect-state`) shows `scheduler.next_batch_nonce`,
equal to `accepted_checkpoint.next_batch_nonce` of `GET /history`.

## 6. Another batch submitter or token

The cycle-0 image boots to the genesis of whatever its `config` range says; the host genesis for
the same values is byte-equal to that machine's state range:

```sh
SEQUENCER=0x...; TOKEN=0x...                                                  # 20 zero bytes of token = TestUsdc
printf '%s%s' "${SEQUENCER#0x}" "${TOKEN#0x}" | xxd -r -p > config.bin && truncate -s 4096 config.bin
cartesi-machine --load=image/cycle0 --replace-memory-range=label:config,data_filename:$PWD/config.bin \
    --assert-rolling-template --final-hash --store=genesis
cartesi-machine-stored-hash genesis                                           # deploy with this hash
QA_LOG=0 bin/qa-wallet-genesis state --sequencer $SEQUENCER --token $TOKEN     # the sequencer's --state-file
cmp state genesis/$(jq -r '(.config // .).nvram[] | select(.label == "state") | .backing_store.data_filename' genesis/config.json) && echo same
```

A machine whose `config` range disagrees with an initialised state refuses to start.

## Rebuilding the kit

`make seq-host seq-inspect seq-image seq-cycle0 seq-kit` in the repository (docs/sequencer.md); the
`build-and-release` GitHub workflow builds it on every push, on demand and on every `v*` tag.
