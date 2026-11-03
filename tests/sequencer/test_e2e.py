"""The whole sequencer setup on a devnet: the host sequencer (libqawallet behind the C seam) and
the machine (the fold in the guest) must agree byte for byte. Flow: deposit + command as direct
inputs through the InputBox, a signed user op through POST /tx, batches posted by the submitter,
then every InputBox input replayed through the stored machine with cartesi-machine and the two
NVRAMs compared with the sequencer's finalized state. Run: pytest tests/sequencer -v
"""
import hashlib
import json
import time

import harness as h
import pytest
from conftest import IMAGE, PK1, SUBMITTER, USER, http


def h_post(url, body):
    return http(url, body)

def wait_for(what, cond, anvil, tries=240):
    """Mines one wall-clock block per second (frames and safety need L1 progress) until cond()
    is truthy."""
    for _ in range(tries):
        v = cond()
        if v:
            return v
        anvil.mine(1)
        time.sleep(1.0)
    pytest.fail(f"timed out waiting for {what}")


def digest(sequencer):
    status, _, body = sequencer.get("/finalized_state/digest")
    return json.loads(body) if status == 200 else None


def inspect_json(m, method, params=()):
    r = m.inspect(json.dumps({"method": method, "params": list(params)}).encode())
    assert r.returncode == 0 and len(r.reports) == 1, r.text()
    return r.reports[0]


def test_host_genesis_equals_the_machine_ranges(genesis_dump):
    m = h.Machine(image=IMAGE)
    try:
        machine_bytes = m.state_bytes()
    finally:
        m.close()
    host_bytes = open(genesis_dump, "rb").read()
    assert len(host_bytes) == len(machine_bytes) == 64 << 20
    assert host_bytes == machine_bytes, "qa-wallet-genesis must reproduce the genesis machine's state range"


def test_sequencer_pins_the_deployment(sequencer, deployment):
    history = sequencer.json("/history")
    dep = history["deployment"]
    assert dep["app_address"].lower() == deployment.app.lower()
    assert dep["input_box_address"].lower() == deployment.input_box.lower()
    assert dep["batch_submitter_address"].lower() == SUBMITTER.lower()
    assert sequencer.json("/domain") == {"name": "CartesiAppSequencer", "version": "1", "chainId": 31337,
                                         "verifyingContract": h.checksum(deployment.app)}
    assert sequencer.json(f"/nonce?sender={USER}")["next_nonce"] == 0


def test_direct_inputs_user_ops_and_batches_agree_with_the_machine(anvil, deployment, sequencer):
    # 1. a deposit and an echoed input through the InputBox (direct inputs)
    anvil.send(PK1, deployment.token, "mint(address,uint256)", USER, "1000000")
    anvil.send(PK1, deployment.token, "approve(address,uint256)", deployment.erc20_portal, "1000000")
    anvil.send(PK1, deployment.erc20_portal, "depositErc20Tokens(address,address,uint256,bytes)",
               deployment.token, deployment.app, "1000000", "0x")
    anvil.send(PK1, deployment.input_box, "addInput(address,bytes)", deployment.app,
               "0x" + bytes(range(16)).hex())  # not a ledger input: echoed as a notice
    # frames advance after five newly-safe blocks: the lane drains both directs
    wait_for("the directs to execute", lambda: sequencer.json("/history")["history"]["head"] >= 2, anvil)
    # the batch closes (max-batch-open-seconds), lands, confirms and becomes the accepted checkpoint
    first = wait_for("the first finalized checkpoint",
                     lambda: (d := digest(sequencer)) and d["executed_input_count"] >= 2 and d, anvil)
    assert sequencer.json("/history")["accepted_checkpoint"]["next_batch_nonce"] >= 1  # time-closed empty batches count too

    # 2. a signed user op through POST /tx (the fee comes from the deposited balance)
    fee = sequencer.json("/fee")
    data = b"\x42"  # echoed as a notice
    nonce = sequencer.json(f"/nonce?sender={USER}")["next_nonce"]
    sig = h.sign_user_op(PK1, nonce, fee["suggested_max_fee"], data, chain=31337, app=deployment.app)
    status, _, body = h_post(sequencer.url + "/tx", {
        "message": {"nonce": nonce, "max_fee": fee["suggested_max_fee"], "data": "0x" + data.hex()},
        "signature": "0x" + sig.hex(), "sender": USER})
    assert status == 200, (body, fee)
    assert json.loads(body)["ok"] is True
    assert sequencer.json(f"/nonce?sender={USER}")["next_nonce"] == nonce + 1
    # a replay of the same nonce is refused by validation
    status, _, body = h_post(sequencer.url + "/tx", {
        "message": {"nonce": nonce, "max_fee": fee["suggested_max_fee"], "data": "0x" + data.hex()},
        "signature": "0x" + sig.hex(), "sender": USER})
    assert status == 422, body
    final = wait_for("the user op's checkpoint",
                     lambda: (d := digest(sequencer)) and d["executed_input_count"] >= 3 and d, anvil)
    assert final["inclusion_block"] > first["inclusion_block"]

    # 3. replay every InputBox input up to the checkpoint through the stored machine
    logs = json.loads(anvil.cast("logs", "--from-block", str(deployment.block), "--to-block",
                                 str(final["inclusion_block"]), "--address", deployment.input_box,
                                 "InputAdded(address,uint256,bytes)", deployment.app, "--json"))
    inputs = []
    for log in logs:
        raw = bytes.fromhex(log["data"][2:])
        inputs.append(h.abi_bytes_arg(raw, 0))
    senders = [i[4 + 32 + 32 + 12:4 + 32 + 32 + 32].hex() for i in inputs]  # EvmAdvance msgSender
    assert senders.count(SUBMITTER[2:].lower()) >= 2, "two batches from the submitter are expected"
    assert len(inputs) >= 4
    m = h.Machine(image=IMAGE)
    try:
        for raw in inputs:
            r = m.advance(b"", raw=raw)
            assert r.accepted, r.text()
        machine_bytes = m.state_bytes()
        assert hashlib.sha256(machine_bytes).hexdigest() == final["sha256"], \
            "the machine's state range differs from the sequencer's finalized state"
        status, headers, body = sequencer.get("/finalized_state")
        assert status == 200 and body == machine_bytes
        assert int(headers.get("X-Executed-Input-Count", headers.get("x-executed-input-count"))) == final["executed_input_count"]
        # the ledger facts behind those bytes
        fee_units = int.from_bytes(inspect_json(m, "qa_fee", [fee["fee"]]), "big")
        assert int.from_bytes(inspect_json(m, "ledger_getBalance", [USER, deployment.token]), "big") == 1000000 - fee_units
        assert int.from_bytes(inspect_json(m, "ledger_getBalance", [SUBMITTER, deployment.token]), "big") == fee_units
        assert int.from_bytes(inspect_json(m, "ledger_getNonce", [USER]), "big") == 1
        state = json.loads(m.inspect(b"state").reports[0])
        assert state["executed_input_count"] == final["executed_input_count"]
        assert state["scheduler"]["next_batch_nonce"] == sequencer.json("/history")["accepted_checkpoint"]["next_batch_nonce"]
        f, drive = m.read_state_drive()
        rec = f.read(32)
        f.close()
        assert drive["start"] >> 22 == 560 and int.from_bytes(rec[:12], "little") == 1000000 - fee_units
    finally:
        m.close()
