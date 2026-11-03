"""Fixtures for the end-to-end sequencer test: anvil with the rollups-contracts v3.0.0-alpha.10
devnet state, the application deployed with cartesi.toml's withdrawal config, the host genesis
dump, and the host sequencer (`setup` then `run`). Everything is a stock tool: anvil, cast,
cartesi-machine, the binaries this repo builds (docs/sequencer.md). Missing pieces skip the tests.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "machine"))
import harness as h  # noqa: E402

ROOT = h.ROOT
DUMP_DIR = ROOT / "third-party" / "cartesi-rollups-contracts-3.0.0-alpha.10-anvil-1.5.1"
IMAGE = Path(os.environ.get("QA_IMAGE", ROOT / ".cartesi" / "image"))
SEQUENCER_BIN = Path(os.environ.get("QA_SEQUENCER_BIN", ROOT / "seq" / "target" / "release" / "qa-wallet-sequencer"))
GENESIS_BIN = Path(os.environ.get("QA_GENESIS_BIN", ROOT / "build" / "host" / "qa-wallet-genesis"))

# anvil accounts: 0 deploys, 1 is the user, 9 is the batch submitter (the address in the image)
PK0 = "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80"
A0 = "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266"
PK1 = h.SIGNER_KEY
USER = h.SIGNER
SUBMITTER = h.SEQUENCER
SUBMITTER_KEY = h.SEQUENCER_KEY
CHAIN_ID = 31337
# anvil stamps a block with max(now, previous + 1): a burst of transactions runs a few seconds
# ahead of the wall clock, and the sequencer treats a safe head >= seconds-per-block ahead of its
# clock as an unusable L1 view, so the block time it is told is generous.
SECONDS_PER_BLOCK = 6
# cartesi.toml [withdrawal.config]
WITHDRAWAL_CONFIG = f"({A0},0,17,560,0xB4D253c7a110241561B3eD6d632846dF7d4e9Af7)"


def address_of(name: str) -> str:
    return (DUMP_DIR / "deployments" / "31337" / f"{name}.txt").read_text().strip()


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run(*args, **kw) -> str:
    p = subprocess.run([str(a) for a in args], capture_output=True, text=True, **kw)
    if p.returncode != 0:
        raise RuntimeError(f"{args[0]} failed ({p.returncode}): {p.stderr.strip()}\n{p.stdout.strip()}")
    return p.stdout.strip()


def http(url: str, body: dict | None = None, timeout=10):
    """GET or JSON POST; returns (status, headers, bytes). HTTP errors are returned, not raised."""
    req = urllib.request.Request(url, method="POST" if body is not None else "GET")
    data = None
    if body is not None:
        req.add_header("content-type", "application/json")
        data = json.dumps(body).encode()
    try:
        with urllib.request.urlopen(req, data=data, timeout=timeout) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


@dataclass
class Anvil:
    rpc: str
    proc: subprocess.Popen

    def cast(self, *args) -> str:
        return run("cast", *args, "--rpc-url", self.rpc)

    def send(self, key: str, to: str, sig: str, *args) -> dict:
        out = run("cast", "send", "--rpc-url", self.rpc, "--private-key", key, "--json", to, sig, *args)
        receipt = json.loads(out)
        assert receipt.get("status") in ("0x1", 1, "1"), receipt
        return receipt

    def mine(self, n: int = 1):
        """Mines `n` blocks stamped with the wall clock. The sequencer stops when the safe head is
        more than --seconds-per-block ahead of its own clock, so blocks are never back-dated or
        batched with artificial intervals here; callers pace mining in real time."""
        run("cast", "rpc", "--rpc-url", self.rpc, "anvil_mine", hex(n))

    def block_number(self) -> int:
        return int(self.cast("block-number"))


@pytest.fixture(scope="session")
def anvil():
    for tool in ("anvil", "cast", "cartesi-machine", "cartesi-machine-stored-hash"):
        if shutil.which(tool) is None:
            pytest.skip(f"{tool} not on PATH")
    if not (DUMP_DIR / "state.json").is_file():
        pytest.skip(f"no devnet state at {DUMP_DIR} (make seq-deps)")
    port = free_port()
    rpc = f"http://127.0.0.1:{port}"
    logs = tempfile.mkdtemp(prefix="qa-wallet-e2e-")
    proc = subprocess.Popen(["anvil", "--host", "127.0.0.1", "--port", str(port), "--slots-in-an-epoch", "1",
                             "--load-state", str(DUMP_DIR / "state.json")],
                            stdout=open(os.path.join(logs, "anvil.log"), "w"), stderr=subprocess.STDOUT)
    a = Anvil(rpc, proc)
    for _ in range(100):
        try:
            a.block_number()
            break
        except RuntimeError:
            time.sleep(0.2)
    else:
        proc.kill()
        pytest.fail("anvil did not start: " + open(os.path.join(logs, "anvil.log")).read()[-2000:])
    # the dump's blocks are months old; the sequencer refuses a safe head older than
    # l1-read-stale-after-blocks * seconds-per-block of wall-clock time, so bring the chain to now
    run("cast", "rpc", "--rpc-url", rpc, "evm_setNextBlockTimestamp", str(int(time.time())))
    a.mine(1)
    yield a
    proc.terminate()
    proc.wait(timeout=10)


@dataclass
class Deployment:
    app: str
    template_hash: str
    input_box: str
    erc20_portal: str
    token: str
    block: int


@pytest.fixture(scope="session")
def deployment(anvil):
    if not (IMAGE / "config.json").is_file() or not h.image_has_range(IMAGE, "config"):
        pytest.skip(f"{IMAGE} is not a stored sequencer-variant machine (cartesi build -c cartesi-seq.toml)")
    f, _ = h.Machine(image=IMAGE).read_range("config", IMAGE)
    pinned = f.read(40).hex()
    f.close()
    if pinned[:40] != SUBMITTER[2:].lower() or pinned[40:] not in ("00" * 20, address_of("TestUsdc")[2:].lower()):
        pytest.skip(f"the image pins config 0x{pinned}, the test signs as {SUBMITTER} with TestUsdc")
    template_hash = run("cartesi-machine-stored-hash", IMAGE)
    factory = address_of("ApplicationFactory")
    input_box = address_of("InputBox")
    sig = "newApplication(address,address,bytes32,address,(address,uint8,uint8,uint64,address))(address)"
    app = anvil.cast("call", factory, sig, "0x" + "00" * 20, A0, template_hash, input_box, WITHDRAWAL_CONFIG)
    receipt = anvil.send(PK0, factory, sig, "0x" + "00" * 20, A0, template_hash, input_box, WITHDRAWAL_CONFIG)
    assert anvil.cast("code", app) != "0x"
    return Deployment(app=app, template_hash=template_hash, input_box=input_box,
                      erc20_portal=address_of("Erc20Portal"), token=address_of("TestUsdc"),
                      block=int(receipt["blockNumber"], 16))


@pytest.fixture(scope="session")
def genesis_dump(tmp_path_factory):
    """The host genesis for the image's batch submitter; must equal the machine's ranges."""
    if not GENESIS_BIN.is_file():
        pytest.skip(f"{GENESIS_BIN} missing (make host)")
    path = tmp_path_factory.mktemp("genesis") / "state"
    env = {k: v for k, v in os.environ.items() if not k.startswith("QA_")}
    env["QA_LOG"] = "0"
    subprocess.run([str(GENESIS_BIN), str(path), "--sequencer", SUBMITTER], check=True, env=env)
    return path


@dataclass
class Sequencer:
    url: str
    proc: subprocess.Popen
    data_dir: str
    log: str
    outputs: list = field(default_factory=list)

    def get(self, route: str):
        return http(self.url + route)

    def json(self, route: str) -> dict:
        status, _, body = self.get(route)
        assert status == 200, (route, status, body[:300])
        return json.loads(body)


@pytest.fixture(scope="session")
def sequencer(anvil, deployment, genesis_dump, tmp_path_factory):
    if not SEQUENCER_BIN.is_file():
        pytest.skip(f"{SEQUENCER_BIN} missing (make seq-host)")
    work = tmp_path_factory.mktemp("sequencer")
    data_dir = str(work / "data")
    log = open(work / "sequencer.log", "w")
    env = {k: v for k, v in os.environ.items() if not k.startswith("QA_")}
    env.update({"RUST_LOG": os.environ.get("RUST_LOG", "info"), "QA_LOG": "0", "QA_ENGINE_WORKDIR": str(work),
                "CARTESI_SEQUENCER_FEE_ORACLE_FIXED_LOG_GAS_PRICE": "0"})
    subprocess.run([str(SEQUENCER_BIN), "--state-file", str(genesis_dump), "setup", "--data-dir", data_dir,
                    "--eth-rpc-url", anvil.rpc, "--chain-id", str(CHAIN_ID), "--app-address", deployment.app,
                    "--batch-submitter-address", SUBMITTER, "--seconds-per-block", str(SECONDS_PER_BLOCK)],
                   check=True, env=env, stdout=log, stderr=subprocess.STDOUT)
    port = free_port()
    url = f"http://127.0.0.1:{port}"
    proc = subprocess.Popen([str(SEQUENCER_BIN), "run", "--http-addr", f"127.0.0.1:{port}", "--data-dir", data_dir,
                             "--eth-rpc-url", anvil.rpc, "--batch-submitter-private-key", SUBMITTER_KEY,
                             "--max-batch-open-seconds", "3", "--batch-submitter-idle-poll-interval-ms", "300",
                             "--batch-submitter-confirmation-depth", "1", "--poll-interval-ms", "300",
                             "--seconds-per-block", str(SECONDS_PER_BLOCK)],
                            env=env, stdout=log, stderr=subprocess.STDOUT)
    s = Sequencer(url, proc, data_dir, str(work / "sequencer.log"))
    for _ in range(300):
        if proc.poll() is not None:
            pytest.fail("sequencer exited: " + open(s.log).read()[-4000:])
        try:
            status, _, _ = s.get("/readyz")
            if status == 200:
                break
        except (urllib.error.URLError, ConnectionError, OSError):
            pass
        time.sleep(0.2)
    else:
        proc.kill()
        pytest.fail("sequencer never became ready: " + open(s.log).read()[-4000:])
    yield s
    proc.terminate()
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
