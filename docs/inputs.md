# Sending inputs, deposits and inspects

Advance inputs are ABI calldata; Foundry's `cast` encodes them and the Cartesi CLI (or `cast send`)
delivers them. The tests use exactly the same `cast` invocations (`tests/machine/harness.py`,
`tests/host/mock_e2e.sh`), so what is written here is what is exercised.

## Encode

```sh
cast calldata 'WithdrawErc20(address,uint256,bytes)' $TOKEN 1000000 0x                 # ledger debit -> Voucher(token.transfer)
cast calldata 'WithdrawEther(uint256,bytes)' 5 0x                                       # -> Voucher(sender, 5 wei, empty)
cast calldata 'TransferErc20(address,bytes32,uint256,bytes)' $TOKEN 0x<32-byte id> 10 0x # ledger move, JSON report
0x$(xxd -p -c0 payload.bin)                                                             # anything else: echoed back as a notice
```

Numbers are decimal or `0x` hex; `bytes` arguments are `0x` hex (`0x` for empty).
`cast sig '<signature>'` prints a selector; libcma's are listed in
[machine-asset-tools' README](https://github.com/Mugen-Builders/machine-asset-tools#parser-function-selectors).

## Send

```sh
cartesi send generic --input 0x…                                             # devnet, interactive account pick
cast send $INPUT_BOX 'addInput(address,bytes)' $APP 0x… --private-key $PK    # any chain
```

Deposits go through the portals, never through `addInput` directly:

```sh
cartesi deposit erc20 --token $TOKEN --amount 1000000 --exec-layer-data 0xdeadbeef
cast send $ERC20_PORTAL 'depositErc20Tokens(address,address,uint256,bytes)' $TOKEN $APP 1000000 0xdeadbeef --private-key $PK
```

When feeding the machine directly (the tests, `cartesi-machine --cmio-advance-state`), the portal
payload is `abi.encodePacked` and the input is the `EvmAdvance(...)` frame the InputBox would emit:

```sh
cast abi-encode --packed '(address,address,uint256,bytes)' $TOKEN $SENDER 1000000 0xdeadbeef     # ERC-20 portal payload
cast abi-encode --packed '(address,uint256,bytes)' $SENDER 5 0x                                   # Ether portal payload (empty execLayerData = 0x)
cast abi-encode --packed '(address,address,uint256,bytes)' $NFT $SENDER 9 \
    "$(cast abi-encode 'f(bytes,bytes)' 0x01 0xaa)"                                                # ERC-721: abi(baseLayerData, execLayerData)
cast calldata 'EvmAdvance(uint256,address,address,uint256,uint256,uint256,uint256,bytes)' \
    31337 $APP $SENDER $BLOCK $TIMESTAMP 0 $INDEX 0x<payload>                                      # chainId, app, sender, block, time, prevRandao, index
```

## What an advance input does

The engine looks at the sender first, then at the first 4 bytes of the payload:

| input | effect | completion |
|---|---|---|
| sender is a portal (`QA_*_PORTAL`) | libcma decodes the deposit; the sender's wallet account is credited: the configured token on the accounts drive (`uint96`), everything else (Ether, other ERC-20s, ERC-721, ERC-1155) in the multi-asset ledger; a JSON report echoes the decoded fields (`exec_layer_data`, `base_layer_data` in hex), `accounts_drive`, `credited` | accept; reject with `{"error":"unable to decode deposit: …"}` or the ledger's error (overflow) |
| `WithdrawEther(uint256,bytes)`, `WithdrawErc20(address,uint256,bytes)`, `WithdrawErc721(address,uint256,bytes)`, `WithdrawErc1155Single(address,uint256,uint256,bytes)`, `WithdrawErc1155Batch(address,uint256[],uint256[],bytes)` | ledger debit + libcma voucher (`Voucher(token.transfer)`, `safeTransferFrom`, …; Ether: a value voucher to the sender) | accept; reject with `{"error":"malformed ledger input: …","code":…}` when libcma cannot decode the arguments, or with the ledger's message on overdraft / unknown account |
| `TransferEther(bytes32,uint256,bytes)`, `TransferErc20(address,bytes32,uint256,bytes)`, `TransferErc721(address,bytes32,uint256,bytes)`, `TransferErc1155Single(address,bytes32,uint256,uint256,bytes)`, `TransferErc1155Batch(address,bytes32,uint256[],uint256[],bytes)` | ledger move to a 32-byte account id (a wallet address left-padded with 12 zero bytes, or a virtual account); `{"transfer":"ok",…}` report, no output | accept; reject as above |
| anything else: an unknown selector, fewer than 4 bytes, an empty payload | **one notice whose payload is the exact input bytes** (`Notice(bytes)`), no report | accept (a device failure to emit the notice would reject with `{"error":"unable to emit echo notice","code":-105}`; it cannot happen for size reasons, see below) |

The only distinction is the selector: a payload that starts with one of the 10 libcma selectors is
a ledger input and is never echoed; its arguments are libcma's business.

## Inspect

The payload is JSON `{"method": …, "params": […]}`:

```sh
curl -s -X POST http://localhost:6751/inspect/$APP -d '{"method":"ledger_getBalance","params":["0x<user>","'$TOKEN'"]}'
```

| query | report |
|---|---|
| `ledger_getBalance [account, token?, tokenId?]`, `ledger_getTotalSupply [token?, tokenId?]` | libcma: 32-byte big-endian amount; an account the ledger has never seen rejects the inspect with `Account not found` (libcma semantics, as in the sample wallet) |
| `ledger_getNonce [address]` | the user-op nonce the engine expects (sequencer), 32-byte big-endian |
| `qa_fee [exponent]` | the sequencer fee `fee::to_linear(exponent)` in token units, 32-byte big-endian |
| the raw payload `state` (the sequencer's `STATE_INSPECT_QUERY`) | JSON: progress, rejected count, the pinned sequencer address and token, the nonce count, the keccak of the accounts drive and, in the sequencer variant, `scheduler.next_batch_nonce` / `scheduler.queued_directs` |
| any other non-JSON payload | echoed back as one report (a full 2 MiB query works) |
| any other JSON method | reject with `{"error":"unknown inspect: …"}` |

## Sequencer variant

Under the scheduler every input above is a **direct input** that executes when a batch frame
covers its block (or when it is overdue), a rejection becomes a counted no-op, and signed user ops
carry the same payloads (an echo or a ledger input): [sequencer.md](sequencer.md).

## What to expect on the node

| input | emulator CLI says | input status |
|---|---|---|
| deposit, withdrawal, transfer, echo | `Manual yield rx-accepted (1)` | `ACCEPTED` (outputs: the voucher or the echo notice; reports kept) |
| overdraft, malformed ledger input, undecodable deposit, echo larger than the TX buffer | `Manual yield rx-rejected (2)` | `REJECTED` (reports kept, alpha.13; state reverted) |

Node caps per input: 65,536 outputs and 65,536 reports. The largest payload an echo can carry is
2,096,832 bytes: the `EvmAdvance` frame (selector, 8 head words, length, padded payload) must fit
the 2 MiB RX buffer, and the notice that echoes it is seven words shorter, so an echo never
overflows the 2 MiB TX buffer. One byte more is refused by the emulator/node before the app sees
it (`Input … exceeds …`). A rejected input reverts the whole state on the machine, the
`rejected_count` of the `state` report included (it only grows on the libcmt mock).
