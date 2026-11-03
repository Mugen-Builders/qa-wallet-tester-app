#!/bin/bash
# End-to-end check of the qa-wallet binary on the libcmt mock io driver (no machine needed).
# Inputs are encoded with Foundry's `cast`, exactly as docs/inputs.md tells humans to do.
# Usage: tests/host/mock_e2e.sh <path-to-qa-wallet>
set -euo pipefail
BIN=$(realpath "$1")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

TOKEN=0x7a051EDffC0884cd88d4a377F4C87BE074CF6c81
ERC20_PORTAL=0x3332DE61a8BB9aC84893b2f552Fe81C9a6dC5419
USER=0x1111111111111111111111111111111111111111
VIRTUAL=0xcc00000000000000000000000000000000000000000000000000000000000000   # a 32-byte account id
APP=0xabababababababababababababababababababab

# advance <index> <sender> <0x payload>: the EvmAdvance(...) input the InputBox would emit, as bytes
advance() {
    cast calldata 'EvmAdvance(uint256,address,address,uint256,uint256,uint256,uint256,bytes)' \
        31337 $APP "$2" 1 1700000000 0 "$1" "$3" | xxd -r -p
}
deposit=$(cast abi-encode --packed '(address,address,uint256,bytes)' $TOKEN $USER 1000000 0xdeadbeef)
advance 0 $ERC20_PORTAL "$deposit" > in0.bin
advance 1 $USER 0xdeadbeef0102 > in1.bin                                                   # not a ledger input: echoed
advance 2 $USER "$(cast calldata 'WithdrawErc20(address,uint256,bytes)' $TOKEN 400000 0x)" > in2.bin
advance 3 $USER "$(cast calldata 'WithdrawErc20(address,uint256,bytes)' $TOKEN 1000000 0x)" > in3.bin   # overdraft
advance 4 $USER "$(cast calldata 'TransferErc20(address,bytes32,uint256,bytes)' $TOKEN $VIRTUAL 100000 0x)" > in4.bin
advance 5 $USER 0x4f94d3420102 > in5.bin                                                   # WithdrawErc20 selector, malformed
printf '{"method":"ledger_getBalance","params":["%s","%s"]}' $USER $TOKEN > in6.bin
printf 'state' > in7.bin
advance 8 $USER 0x > in8.bin                                                               # empty: echoed as an empty notice
printf 'hello' > in9.bin                                                                   # non-JSON inspect: echoed as a report

truncate -s 0 state.bin
export QA_STATE_DEVICE="$WORK/state.bin" QA_MERKLE_PATH="$WORK/merkle" QA_LOG=1
export QA_SINGLE_ACCOUNTS=1024 QA_REGION_A_LENGTH=$((2*1024*1024)) QA_REGION_B_LENGTH=$((2*1024*1024))
export QA_MULTI_ACCOUNTS=256 QA_MULTI_ASSETS=16 QA_MULTI_BALANCES=1024
export CMT_INPUTS="0:in0.bin,0:in1.bin,0:in2.bin,0:in3.bin,0:in4.bin,0:in5.bin,1:in6.bin,1:in7.bin,0:in8.bin,1:in9.bin"
set +e
"$BIN" "$WORK/state.bin" > run.log 2>&1
RC=$?
set -e
echo "--- qa-wallet exited with $RC (the mock returns ENODATA after the last input)"
grep -q "completion" run.log || { cat run.log; exit 1; }
grep -E "completion|reject|echo" run.log | sed 's/^/    /'

fail() { echo "FAIL: $*"; cat run.log; exit 1; }
[ -f in0.report-0.bin ] || fail "deposit report missing"
grep -q '"credited":true' in0.report-0.bin || fail "deposit not credited: $(cat in0.report-0.bin)"
grep -q '"accounts_drive":true' in0.report-0.bin || fail "deposit not on the accounts drive"
[ ! -f in0.output-0.bin ] || fail "a deposit must not produce an output"
# echo: Notice(bytes) = selector c258d6e5 + offset + length + padded data; 6 bytes -> 100 bytes
[ -f in1.output-0.bin ] || fail "echo notice missing"
[ "$(xxd -p -l 4 in1.output-0.bin)" = "c258d6e5" ] || fail "echo is not a Notice"
[ "$(stat -c %s in1.output-0.bin)" = "100" ] || fail "echo notice size"
[ "$(xxd -p -s 68 -l 6 in1.output-0.bin)" = "deadbeef0102" ] || fail "echo bytes differ"
[ ! -f in1.report-0.bin ] || fail "an echo must not produce a report"
[ -f in2.output-0.bin ] || fail "WithdrawErc20 voucher missing"
[ "$(xxd -p -l 4 in2.output-0.bin)" = "237a816f" ] || fail "WithdrawErc20 is not a Voucher"
[ -f in3.report-0.bin ] && grep -q '"error"' in3.report-0.bin || fail "overdraft report missing"
[ ! -f in3.output-0.bin ] || fail "an overdraft must not produce an output"
[ -f in4.report-0.bin ] && grep -q '"transfer":"ok"' in4.report-0.bin || fail "transfer report missing"
[ -f in5.report-0.bin ] && grep -q 'malformed ledger input' in5.report-0.bin || fail "malformed input must be rejected with a report"
[ ! -f in5.output-0.bin ] || fail "a malformed ledger input must not be echoed"
[ -f in6.report-0.bin ] || fail "balance report missing"
BAL=$(xxd -p -c 64 in6.report-0.bin); [ "$BAL" = "$(printf '%064x' 500000)" ] || fail "balance after deposit/withdrawal/transfer should be 500000, got 0x$BAL"
grep -q '"executed_input_count":4' in7.report-0.bin || fail "state report: $(cat in7.report-0.bin)"
grep -q '"rejected_count":2' in7.report-0.bin || fail "state report rejected count: $(cat in7.report-0.bin)"
[ -f in8.output-0.bin ] && [ "$(stat -c %s in8.output-0.bin)" = "68" ] || fail "empty input must be echoed as an empty notice"
[ -f in9.report-0.bin ] && [ "$(cat in9.report-0.bin)" = "hello" ] || fail "non-JSON inspect must be echoed as a report"
# accounts-drive record 0 = LibUsdAccount: uint96 little-endian balance (500000 = 0x07a120) + address
REC=$(xxd -p -c 32 -l 32 state.bin)
[ "$REC" = "20a107000000000000000000${USER#0x}" ] || fail "accounts-drive record 0 is $REC"
echo "    accounts-drive record 0 ok: 500000 $USER"
echo "mock e2e passed"
