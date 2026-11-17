"""The inspect follower (seq/inspect, docs/sequencer.md) against the real sequencer: it restores the
sequencer's snapshot, applies the feed, and answers inspects from the soft-confirmed state. Its
reports must be the ones the machine gives once the same inputs are final, it must survive its own
restart and the sequencer's, and inspects must leave no trace. Run: pytest tests/sequencer -v
"""
import json
import time

import harness as h
import pytest
from conftest import IMAGE, PK1, SUBMITTER, USER, http

STRANGER = "0x00000000000000000000000000000000000000Aa"  # an account the ledger never saw


def query(method, params=()) -> bytes:
    return json.dumps({"method": method, "params": list(params)}).encode()


def wait_for(what, cond, anvil=None, tries=240):
    """Polls cond(); with anvil, mines one block per second (frames and safety need L1 progress)."""
    for _ in range(tries):
        v = cond()
        if v:
            return v
        if anvil is not None:
            anvil.mine(1)
        time.sleep(1.0 if anvil is not None else 0.1)
    pytest.fail(f"timed out waiting for {what}")


def head(sequencer) -> int:
    return sequencer.json("/history")["history"]["head"]


def digest(sequencer):
    status, _, body = sequencer.get("/finalized_state/digest")
    return json.loads(body) if status == 200 else None


def post_user_op(sequencer, deployment, data: bytes):
    fee = sequencer.json("/fee")
    nonce = sequencer.json(f"/nonce?sender={USER}")["next_nonce"]
    sig = h.sign_user_op(PK1, nonce, fee["suggested_max_fee"], data, chain=31337, app=deployment.app)
    status, _, body = http(sequencer.url + "/tx", {
        "message": {"nonce": nonce, "max_fee": fee["suggested_max_fee"], "data": "0x" + data.hex()},
        "signature": "0x" + sig.hex(), "sender": USER})
    assert status == 200, body
    return nonce


def caught_up(follower, sequencer):
    return follower.count() == head(sequencer)


def test_the_follower_answers_from_the_soft_state_like_the_machine(anvil, deployment, sequencer, follower):
    # traffic of its own, on top of whatever the sequencer already holds: a deposit and an echo
    # as direct inputs, then a signed user op
    before = head(sequencer)
    anvil.send(PK1, deployment.token, "mint(address,uint256)", USER, "500000")
    anvil.send(PK1, deployment.token, "approve(address,uint256)", deployment.erc20_portal, "500000")
    anvil.send(PK1, deployment.erc20_portal, "depositErc20Tokens(address,address,uint256,bytes)",
               deployment.token, deployment.app, "500000", "0x")
    anvil.send(PK1, deployment.input_box, "addInput(address,bytes)", deployment.app, "0x" + b"follower".hex())
    wait_for("the directs to execute", lambda: head(sequencer) >= before + 2, anvil)
    wait_for("the follower to apply the directs", lambda: caught_up(follower, sequencer))

    # a user op is visible as soon as the sequencer executed it: no batch, no L1 block needed
    nonce = post_user_op(sequencer, deployment, b"\x43")
    wait_for("the user op in the follower's state",
             lambda: int.from_bytes(follower.reports(query("ledger_getNonce", [USER]))[0], "big") == nonce + 1)
    count = head(sequencer)
    assert follower.count() == count
    assert int.from_bytes(follower.reports(query("ledger_getNonce", [USER]))[0], "big") == \
        sequencer.json(f"/nonce?sender={USER}")["next_nonce"]

    # the answers, and that asking leaves the state alone
    payloads = [query("ledger_getBalance", [USER, deployment.token]),
                query("ledger_getBalance", [SUBMITTER, deployment.token]),
                query("ledger_getTotalSupply", [deployment.token]),
                query("ledger_getNonce", [USER]),
                query("qa_fee", [sequencer.json("/fee")["fee"]]),
                query("ledger_getBalance", [STRANGER, deployment.token]),  # an error report
                query("no_such_method"),
                b"{not json",
                b"ping"]
    answers = [follower.reports(p) for p in payloads]
    assert all(len(a) == 1 for a in answers)
    user, submitter, supply = (int.from_bytes(a[0], "big") for a in answers[:3])
    assert user > 0 and submitter > 0 and user + submitter == supply, (user, submitter, supply)
    assert b"error" in answers[5][0] and b"error" in answers[6][0] and b"error" in answers[7][0]
    assert answers[8][0] == b"ping"
    state = json.loads(follower.reports(b"state")[0])
    assert state["executed_input_count"] == count and state["state_backend"] == "file" and "scheduler" not in state
    for _ in range(100):
        for p in payloads:
            follower.inspect(p)
    assert json.loads(follower.reports(b"state")[0]) == state
    assert [follower.reports(p) for p in payloads] == answers

    # the route
    assert follower.inspect(b"state", app=USER)[0] == 404
    assert follower.inspect(b"x" * ((2 << 20) + 1))[0] == 413
    assert http(follower.url + "/healthz")[0] == 200

    # once those inputs are final, the machine that replays them from L1 gives the same reports
    final = wait_for("the checkpoint covering the user op",
                     lambda: (d := digest(sequencer)) and d["executed_input_count"] >= count and d, anvil)
    assert final["executed_input_count"] == count == follower.count()
    logs = json.loads(anvil.cast("logs", "--from-block", str(deployment.block), "--to-block",
                                 str(final["inclusion_block"]), "--address", deployment.input_box,
                                 "InputAdded(address,uint256,bytes)", deployment.app, "--json"))
    m = h.Machine(image=IMAGE)
    try:
        for log in logs:
            r = m.advance(b"", raw=h.abi_bytes_arg(bytes.fromhex(log["data"][2:]), 0))
            assert r.accepted, r.text()
        for p, answer in zip(payloads, answers):
            assert m.inspect(p).reports == answer, p
        machine_state = json.loads(m.inspect(b"state").reports[0])
    finally:
        m.close()
    # `state` is the engine's own view: the host has no scheduler and a file behind the ledger
    assert machine_state.pop("state_backend") == "nvram" and machine_state.pop("scheduler")
    assert state.pop("state_backend") == "file"
    assert machine_state == state


def test_the_follower_survives_restarts(anvil, deployment, sequencer, follower):
    wait_for("the follower to be current", lambda: caught_up(follower, sequencer))
    count = follower.count()
    state = follower.reports(b"state")[0]

    # its own restart: nothing was kept, the snapshot plus the feed give the same state
    follower.restart()
    wait_for("the restarted follower", lambda: follower.count() == count)
    assert follower.reports(b"state")[0] == state

    # the sequencer's restart: the follower keeps answering, then follows again
    sequencer.restart()
    assert follower.count() >= 0, "the follower stopped answering while the sequencer was away"
    anvil.send(PK1, deployment.input_box, "addInput(address,bytes)", deployment.app, "0x" + b"after".hex())
    wait_for("the input after the restart", lambda: head(sequencer) > count, anvil)
    wait_for("the follower after the sequencer's restart", lambda: caught_up(follower, sequencer))
    assert follower.count() > count
    nonce = post_user_op(sequencer, deployment, b"\x44")
    wait_for("the user op after the restart",
             lambda: int.from_bytes(follower.reports(query("ledger_getNonce", [USER]))[0], "big") == nonce + 1)
    assert follower.proc.poll() is None
