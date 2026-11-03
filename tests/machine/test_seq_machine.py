"""Machine tests of the sequencer variant: the stored template built from cartesi-seq.toml, driven
one input per process with the host cartesi-machine like test_machine.py. Inputs from the batch
submitter (anvil account 9) are SSZ batches; everything else is a direct input that waits in the
scheduler's queue until a frame covers it. Needs `.cartesi/image` from `cartesi build -c
cartesi-seq.toml` (or QA_IMAGE), cartesi-machine 0.22.0 and Foundry's cast.
"""
import json
import os
import shutil
import subprocess
import tempfile

import pytest

import harness as h

IMAGE = os.environ.get("QA_IMAGE", str(h.ROOT / ".cartesi" / "image"))
FEE_100 = 2  # fee::to_linear(100) in token units (seq/guest/tests/fold.rs pins every exponent)


@pytest.fixture(scope="module")
def m():
    if not os.path.isdir(IMAGE):
        pytest.skip(f"no stored machine at {IMAGE} (cartesi build -c cartesi-seq.toml)")
    if not h.image_has_range(IMAGE, "config"):
        pytest.skip(f"{IMAGE} is not the sequencer variant (no `config` range)")
    machine = h.Machine(image=IMAGE)
    yield machine
    machine.close()


def inspect_json(m, method, params=()):
    q = json.dumps({"method": method, "params": list(params)}).encode()
    r = m.inspect(q)
    assert r.returncode == 0, r.text()
    assert len(r.reports) == 1, r.reports
    return r.reports[0]


def balance(m, account, token=None):
    return int.from_bytes(inspect_json(m, "ledger_getBalance", [account] + ([token] if token else [])), "big")


def state(m):
    r = m.inspect(b"state")
    assert r.returncode == 0, r.text()
    return json.loads(r.reports[0])


def covering_batch(m, nonce, safe_block, user_ops=(), fee_price=0, block=None):
    """A batch from the submitter with one frame; included `block` defaults to safe_block + 5."""
    payload = h.batch(nonce, [h.frame(safe_block, fee_price, user_ops)])
    r = m.advance(payload, sender=h.SEQUENCER, block=block if block is not None else safe_block + 5)
    assert r.accepted, r.text()
    return r


# ---------------------------------------------------------------- layout
def test_ranges_keep_the_accounts_drive_provable(m):
    cfg = h.image_config(m.image)
    ranges = {d["label"]: d for d in cfg["nvram"]}
    assert ranges["state"]["start"] == 0x8C000000 and ranges["state"]["length"] == 64 << 20
    assert ranges["state"]["start"] >> 22 == 560  # [withdrawal.config].accounts_drive_start_index
    assert ranges["config"]["start"] == 0x90000000 and ranges["config"]["length"] == 4096
    f, _ = m.read_range("config")
    cfgrange = f.read()
    f.close()
    assert cfgrange[:20].hex() == h.SEQUENCER[2:].lower() and cfgrange[20:40].hex() == h.TOKEN[2:].lower()
    assert not any(cfgrange[40:])
    st = state(m)
    assert st["sequencer_address"] == h.SEQUENCER.lower() and st["token"] == h.TOKEN.lower()
    assert st["executed_input_count"] == 0 and st["scheduler"] == {"next_batch_nonce": 0, "queued_directs": 0}


# ---------------------------------------------------------------- scheduling
def test_deposit_waits_for_a_covering_frame(m):
    payload = h.packed_deposit("erc20", h.TOKEN, h.USER, 1000000, bytes.fromhex("deadbeef"))
    r = m.advance(payload, sender=h.ERC20_PORTAL, block=100)
    assert r.accepted and r.reports == [] and r.outputs == [], r.text()
    assert state(m)["scheduler"] == {"next_batch_nonce": 0, "queued_directs": 1}
    f, _ = m.read_state_drive()
    assert not any(f.read(32))  # record 0 still empty: nothing executed yet
    f.close()

    # a frame below the deposit's block does not cover it
    covering_batch(m, 0, 99)
    assert state(m)["scheduler"] == {"next_batch_nonce": 1, "queued_directs": 1}
    r = covering_batch(m, 1, 100)
    rep = json.loads(r.reports[0])
    assert rep["credited"] is True and rep["accounts_drive"] is True and rep["exec_layer_data"] == "0xdeadbeef"
    st = state(m)
    assert st["scheduler"] == {"next_batch_nonce": 2, "queued_directs": 0}
    assert st["executed_input_count"] == 1 and st["last_block"] == 100
    assert balance(m, h.USER, h.TOKEN) == 1_000_000
    f, drive = m.read_state_drive()
    rec = f.read(32)
    f.close()
    assert drive["start"] % (1 << 22) == 0
    assert int.from_bytes(rec[:12], "little") == 1_000_000 and rec[12:32].hex() == h.USER[2:].lower()


def test_invalid_batches_are_dropped_with_a_report(m):
    r = m.advance(b"\xff\xee\xdd", sender=h.SEQUENCER, block=110)
    assert r.accepted and r.reports == [b"scheduler dropped invalid batch"], r.text()
    wrong_nonce = h.batch(7, [h.frame(110)])
    r = m.advance(wrong_nonce, sender=h.SEQUENCER, block=111)
    assert r.accepted and r.reports == [b"scheduler dropped invalid batch"]
    above_inclusion = h.batch(2, [h.frame(200)])
    r = m.advance(above_inclusion, sender=h.SEQUENCER, block=112)
    assert r.accepted and r.reports == [b"scheduler dropped invalid batch"]
    assert state(m)["scheduler"]["next_batch_nonce"] == 2


def test_direct_inputs_run_in_order_when_drained(m):
    echo = bytes(range(64))  # not a ledger input: echoed as a notice when drained
    r = m.advance(echo, block=120)
    assert r.accepted and r.outputs == []
    r = m.advance(h.cmd("WithdrawErc20", h.TOKEN, 400000, b""), block=121)
    assert r.accepted and r.outputs == []
    assert state(m)["scheduler"]["queued_directs"] == 2
    r = covering_batch(m, 2, 121)
    assert [o[:4] for o in r.outputs] == [h.NOTICE_SEL, h.VOUCHER_SEL]
    assert h.notice_payload(r.outputs[0]) == echo
    body = r.outputs[1][4:]
    assert body[12:32].hex() == h.TOKEN[2:].lower()
    assert balance(m, h.USER, h.TOKEN) == 600_000
    assert state(m)["executed_input_count"] == 3


def test_rejected_ledger_inputs_are_counted_no_ops(m):
    before = state(m)
    r = m.advance(h.cmd("WithdrawErc20", h.TOKEN, 600_001, b""), block=130)  # overdraft
    assert r.accepted
    r = m.advance(bytes.fromhex("4f94d3420102"), block=131)  # WithdrawErc20 selector, malformed
    assert r.accepted
    r = covering_batch(m, 3, 131)
    assert r.accepted, r.text()  # the machine is still serving: nothing was rejected at the device
    assert r.outputs == []
    assert any(b'"error"' in rep for rep in r.reports)
    assert any(b"malformed ledger input" in rep for rep in r.reports)
    st = state(m)
    assert st["executed_input_count"] == before["executed_input_count"] + 2
    assert st["rejected_count"] == before["rejected_count"] + 2
    assert balance(m, h.USER, h.TOKEN) == 600_000


def test_transfers_move_balances_without_outputs(m):
    virtual = "0x" + "cc" + "00" * 31
    r = m.advance(h.cmd("TransferErc20", h.TOKEN, virtual, 100000, b""), block=140)
    assert r.accepted
    r = covering_batch(m, 4, 140)
    assert r.outputs == [] and any(b'"transfer":"ok"' in rep for rep in r.reports)
    assert balance(m, h.USER, h.TOKEN) == 500_000
    assert balance(m, virtual, h.TOKEN) == 100_000


def test_signed_user_op_executes_and_pays_the_submitter(m):
    # the signer needs a fee balance: deposit, then drain
    r = m.advance(h.packed_deposit("erc20", h.TOKEN, h.SIGNER, 1000), sender=h.ERC20_PORTAL, block=150)
    assert r.accepted
    covering_batch(m, 5, 150)
    assert balance(m, h.SIGNER, h.TOKEN) == 1000
    assert int.from_bytes(inspect_json(m, "ledger_getNonce", [h.SIGNER]), "big") == 0
    assert int.from_bytes(inspect_json(m, "qa_fee", [100]), "big") == FEE_100

    data = b"\x42"  # echoed as a notice
    op = h.user_op(0, 100, data, h.sign_user_op(h.SIGNER_KEY, 0, 100, data))
    replay = op
    unaffordable = h.user_op(1, 1, data, h.sign_user_op(h.SIGNER_KEY, 1, 1, data))  # max_fee below the frame fee
    r = covering_batch(m, 6, 160, user_ops=[op, replay, unaffordable], fee_price=100)
    assert len(r.outputs) == 1 and h.notice_payload(r.outputs[0]) == b"\x42"
    assert int.from_bytes(inspect_json(m, "ledger_getNonce", [h.SIGNER]), "big") == 1
    assert balance(m, h.SIGNER, h.TOKEN) == 1000 - FEE_100
    assert balance(m, h.SEQUENCER, h.TOKEN) == FEE_100
    st = state(m)
    assert st["nonce_count"] == 1 and st["last_block"] == 160

    # a business failure (overdraft) still consumes the nonce and the fee
    bad = h.cmd("WithdrawErc20", h.TOKEN, 5000, b"")
    op1 = h.user_op(1, 100, bad, h.sign_user_op(h.SIGNER_KEY, 1, 100, bad))
    r = covering_batch(m, 7, 161, user_ops=[op1], fee_price=100)
    assert r.outputs == [] and any(b'"error"' in rep for rep in r.reports)
    assert int.from_bytes(inspect_json(m, "ledger_getNonce", [h.SIGNER]), "big") == 2
    assert balance(m, h.SIGNER, h.TOKEN) == 1000 - 2 * FEE_100


def test_stale_batch_is_skipped_and_overdue_directs_are_forced(m):
    nonce = state(m)["scheduler"]["next_batch_nonce"]
    r = m.advance(b"\x07", block=200)
    assert r.accepted and state(m)["scheduler"]["queued_directs"] == 1
    stale = h.batch(nonce, [h.frame(200)])
    r = m.advance(stale, sender=h.SEQUENCER, block=200 + h.MAX_WAIT_BLOCKS)
    assert r.accepted and r.reports == []
    # the overdue direct ran before the batch was judged; the stale batch kept the nonce
    assert len(r.outputs) == 1 and h.notice_payload(r.outputs[0]) == b"\x07"
    assert state(m)["scheduler"] == {"next_batch_nonce": nonce, "queued_directs": 0}
    # and the same batch nonce is still accepted afterwards
    covering_batch(m, nonce, 200 + h.MAX_WAIT_BLOCKS, block=200 + h.MAX_WAIT_BLOCKS + 1)
    assert state(m)["scheduler"]["next_batch_nonce"] == nonce + 1


def test_inspects_never_leave_the_accepted_state(m):
    rep = inspect_json(m, "qa_exception", ["0x0102"])  # unknown method: its error report, no yield
    assert b"unknown inspect" in rep
    r = m.inspect(b"anything else")
    assert r.returncode == 0 and r.reports == [b"anything else"]
    st = state(m)
    assert st["sequencer_address"] == h.SEQUENCER.lower() and "scheduler" in st


def test_comparison_bytes_are_the_state_range(m):
    data = m.state_bytes()
    assert len(data) == 64 << 20
    assert data[:12] == (500_000).to_bytes(12, "little")  # record 0: the user's drive balance
    meta = data[60 << 20:]  # region C: meta page then the nonce table
    assert meta[:8] == b"QATESTR1"
    assert int.from_bytes(meta[16:24], "little") == state(m)["executed_input_count"]
    assert meta[48:68].hex() == h.SEQUENCER[2:].lower() and meta[68:88].hex() == h.TOKEN[2:].lower()


# ---------------------------------------------------------------- the deployer's genesis flow
CYCLE0 = os.environ.get("QA_CYCLE0", str(h.ROOT / "build" / "seq" / "cycle0"))


def test_replaced_config_range_pins_another_submitter_and_token():
    """docs/sequencer.md: boot the cycle-0 image with a replaced `config` range to the first yield;
    the genesis then pins that submitter and token, and the host genesis tool reproduces it."""
    if not os.path.isdir(CYCLE0):
        pytest.skip(f"no cycle-0 image at {CYCLE0} (make seq-cycle0)")
    genesis_bin = h.ROOT / "build" / "host" / "qa-wallet-genesis"
    submitter = "0x3C44CdDdB6a900fa2b585dd299e03d12FA4293BC"  # anvil account 2
    token = "0x" + "77" * 20
    work = tempfile.mkdtemp(prefix="qa-wallet-config-")
    try:
        config = os.path.join(work, "config.bin")
        with open(config, "wb") as f:
            f.write(bytes.fromhex(submitter[2:]) + bytes.fromhex(token[2:]))
            f.truncate(4096)
        genesis = os.path.join(work, "genesis")
        p = subprocess.run(["cartesi-machine", f"--load={CYCLE0}", f"--replace-memory-range=label:config,data_filename:{config}",
                            "--assert-rolling-template", "--final-hash", f"--store={genesis}"],
                           capture_output=True, text=True, timeout=900)
        assert p.returncode == 0 and "rx-accepted" in p.stdout + p.stderr, p.stdout + p.stderr
        m = h.Machine(image=genesis)
        try:
            st = state(m)
            assert st["sequencer_address"] == submitter.lower() and st["token"] == token
            # a deposit of the configured token lands on the accounts drive, TestUsdc does not
            r = m.advance(h.packed_deposit("erc20", token, h.USER, 5), sender=h.ERC20_PORTAL, block=10)
            assert r.accepted
            r = m.advance(h.batch(0, [h.frame(10)]), sender=submitter, block=15)
            assert r.accepted and json.loads(r.reports[0])["accounts_drive"] is True
            machine_bytes = m.state_bytes(genesis)
        finally:
            m.close()
        if genesis_bin.is_file():
            host = os.path.join(work, "host-genesis")
            env = {k: v for k, v in os.environ.items() if not k.startswith("QA_")}
            env["QA_LOG"] = "0"
            subprocess.run([str(genesis_bin), host, "--sequencer", submitter, "--token", token], check=True, env=env)
            assert open(host, "rb").read() == machine_bytes
    finally:
        shutil.rmtree(work, ignore_errors=True)
