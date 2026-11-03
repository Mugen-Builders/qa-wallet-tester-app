"""Machine tests: run the stored template under the host cartesi-machine CLI, one input per
process, and check outputs byte for byte. Needs `.cartesi/image` (`cartesi build`) and Foundry's
`cast`. Run: /opt/venv/bin/pytest tests/machine -v   (or any python3 + pytest)
"""
import json
import os

import pytest

import harness as h

RX_BUFFER = TX_BUFFER = 2 * 1024 * 1024
# The largest echo payload: an EvmAdvance input is selector + 8 head words + length + the payload
# padded to 32 bytes, and must fit the 2 MiB RX buffer. The notice that echoes it is 7 words
# shorter, so an echo can never overflow the TX buffer (the -105 path exists only for the sink).
MAX_ECHO = (RX_BUFFER - 4 - 8 * 32 - 32) // 32 * 32  # 2,096,832 bytes
MAX_OUTPUT = 4 + 32 + 32 + MAX_ECHO  # the ABI-encoded Notice(bytes) of that echo


IMAGE = os.environ.get("QA_IMAGE", str(h.ROOT / ".cartesi" / "image"))


@pytest.fixture(scope="module")
def m():
    if not os.path.isdir(IMAGE):
        pytest.skip(f"no stored machine at {IMAGE} (cartesi build)")
    if h.image_has_range(IMAGE, "config"):
        pytest.skip(f"{IMAGE} is the sequencer variant (test_seq_machine.py covers it)")
    machine = h.Machine(image=IMAGE)
    yield machine
    machine.close()


def balance(m, account, token=None):
    params = [account] + ([token] if token else [])
    q = json.dumps({"method": "ledger_getBalance", "params": params}).encode()
    r = m.inspect(q)
    assert r.returncode == 0, r.text()
    assert len(r.reports) == 1, r.reports
    return int.from_bytes(r.reports[0], "big")


def test_evm_advance_matches_cast():
    for payload in (b"", b"\x01", bytes(range(64)), bytes(range(33))):
        assert h.evm_advance(payload, h.USER, block=7, index=3) == h.evm_advance_cast(payload, h.USER, block=7, index=3)


# ---------------------------------------------------------------- ledger / deposits / vouchers
def test_erc20_deposit_is_credited_on_the_accounts_drive(m):
    payload = h.packed_deposit("erc20", h.TOKEN, h.USER, 1000000, bytes.fromhex("deadbeef"))
    r = m.advance(payload, sender=h.ERC20_PORTAL)
    assert r.accepted, r.text()
    assert r.outputs == []
    rep = json.loads(r.reports[0])
    assert rep["credited"] is True and rep["accounts_drive"] is True
    assert rep["exec_layer_data"] == "0xdeadbeef" and rep["amount"] == "1000000"
    assert balance(m, h.USER, h.TOKEN) == 1_000_000


def test_other_assets_are_credited_off_the_accounts_drive(m):
    other = "0x" + "99" * 20
    r = m.advance(h.packed_deposit("erc20", other, h.USER, 77), sender=h.ERC20_PORTAL)
    assert r.accepted and json.loads(r.reports[0])["accounts_drive"] is False
    r = m.advance(h.packed_deposit("ether", h.USER, 5), sender=h.ETHER_PORTAL)
    assert r.accepted and json.loads(r.reports[0])["deposit"] == "ether"
    r = m.advance(h.packed_deposit("erc721", "0x" + "33" * 20, h.USER, 9, b"\x01", b"\xaa"), sender=h.ERC721_PORTAL)
    assert r.accepted and json.loads(r.reports[0])["base_layer_data"] == "0x01"
    assert balance(m, h.USER, other) == 77
    assert balance(m, h.USER) == 5


def test_malformed_deposit_is_rejected_with_a_report(m):
    r = m.advance(b"\x01\x02\x03", sender=h.ERC20_PORTAL)
    assert not r.accepted and "rx-rejected" in r.text()
    assert r.outputs == [] and b"unable to decode deposit" in r.reports[0]


def test_plain_erc20_withdrawal_emits_a_transfer_voucher(m):
    r = m.advance(h.cmd("WithdrawErc20", h.TOKEN, 400000, b""))
    assert r.accepted, r.text()
    assert len(r.outputs) == 1 and r.outputs[0][:4] == h.VOUCHER_SEL
    body = r.outputs[0][4:]
    assert body[12:32].hex() == h.TOKEN[2:].lower()          # destination = token
    assert int.from_bytes(body[32:64], "big") == 0            # value
    call = h.abi_bytes_arg(body, 2)
    assert call[:4].hex() == "a9059cbb"                       # transfer(address,uint256)
    assert call[16:36].hex() == h.USER[2:].lower()
    assert int.from_bytes(call[36:68], "big") == 400000
    assert balance(m, h.USER, h.TOKEN) == 600_000


def test_ether_withdrawal_emits_a_value_voucher(m):
    r = m.advance(h.cmd("WithdrawEther", 2, b""))
    assert r.accepted, r.text()
    assert len(r.outputs) == 1 and r.outputs[0][:4] == h.VOUCHER_SEL
    body = r.outputs[0][4:]
    assert body[12:32].hex() == h.USER[2:].lower()            # destination = the user
    assert int.from_bytes(body[32:64], "big") == 2            # value in wei
    assert balance(m, h.USER) == 3


def test_transfer_moves_a_balance_to_an_account_id(m):
    virtual = "0x" + "cc" + "00" * 31
    r = m.advance(h.cmd("TransferErc20", h.TOKEN, virtual, 100000, b""))
    assert r.accepted, r.text()
    assert r.outputs == [] and json.loads(r.reports[0])["transfer"] == "ok"
    assert balance(m, h.USER, h.TOKEN) == 500_000
    assert balance(m, virtual, h.TOKEN) == 100_000


def test_overdraft_is_rejected_and_keeps_the_report(m):
    r = m.advance(h.cmd("WithdrawErc20", h.TOKEN, 500001, b""))
    assert not r.accepted and "rx-rejected" in r.text()
    rep = json.loads(r.reports[0])
    assert rep["code"] < 0 and "error" in rep, rep
    assert balance(m, h.USER, h.TOKEN) == 500_000


def test_malformed_ledger_input_is_rejected_not_echoed(m):
    r = m.advance(bytes.fromhex("4f94d3420102"))  # WithdrawErc20 selector with garbage arguments
    assert not r.accepted and "rx-rejected" in r.text()
    assert r.outputs == [] and b"malformed ledger input" in r.reports[0]


def test_accounts_drive_record_matches_LibUsdAccount(m):
    f, drive = m.read_state_drive()
    rec = f.read(32)
    f.close()
    assert drive["start"] % (1 << 22) == 0
    assert int.from_bytes(rec[:12], "little") == 500_000
    assert rec[12:32].hex() == h.USER[2:].lower()


# ---------------------------------------------------------------- echo
def test_unknown_input_is_echoed_as_a_notice(m):
    data = bytes.fromhex("deadbeef") + bytes(range(256)) * 200  # an unknown selector and 51204 bytes
    r = m.advance(data)
    assert r.accepted and len(r.outputs) == 1 and r.reports == [], r.text()
    assert h.notice_payload(r.outputs[0]) == data


def test_empty_and_short_inputs_are_echoed(m):
    r = m.advance(b"")
    assert r.accepted and len(r.outputs) == 1 and h.notice_payload(r.outputs[0]) == b""
    r = m.advance(b"\x01\x02\x03")
    assert r.accepted and len(r.outputs) == 1 and h.notice_payload(r.outputs[0]) == b"\x01\x02\x03"


def test_largest_input_is_echoed_intact(m):
    data = bytes(i & 0xFF for i in range(MAX_ECHO))
    frame = len(h.evm_advance(data, h.USER))
    assert frame <= RX_BUFFER < frame + 32  # the next 32-byte word would not fit
    r = m.advance(data, timeout=1800)
    assert r.accepted, r.text()[-800:]
    assert len(r.outputs) == 1 and len(r.outputs[0]) == MAX_OUTPUT
    assert h.notice_payload(r.outputs[0]) == data
    # one byte more does not fit the RX buffer: the emulator refuses the input before the app sees
    # it (no echo, no report; the CLI still exits 0 on this emulator build)
    r = m.advance(data + b"\x00", commit=False)
    assert "exceeds" in r.text() and r.outputs == [] and r.reports == []
    # the machine keeps serving
    assert balance(m, h.USER, h.TOKEN) == 500_000


# ---------------------------------------------------------------- inspect
def test_inspect_state_echo_and_unknown_method(m):
    r = m.inspect(b"state")
    assert r.returncode == 0
    st = json.loads(r.reports[0])
    # a rejected input reverts the whole state on the real machine, its counter included
    assert st["executed_input_count"] >= 10 and st["rejected_count"] == 0
    assert st["token"] == h.TOKEN.lower() and "deposit_policy" not in st
    big = bytes(range(256)) * (TX_BUFFER // 256)
    r = m.inspect(big)
    assert r.returncode == 0 and r.reports == [big]
    r = m.inspect(json.dumps({"method": "qa_reports", "params": [3, 8, 2]}).encode())
    assert "rx-rejected" in r.text() and b"unknown inspect" in r.reports[0]
    r = m.inspect(json.dumps({"method": "ledger_getBalance", "params": ["0x" + "01" * 20, h.TOKEN]}).encode())
    assert "rx-rejected" in r.text() and b"error" in r.reports[0]  # an account the ledger never saw
