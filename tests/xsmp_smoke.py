#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise the XSMP companion with real libSM clients and private authority."""
import argparse
import base64
import configparser
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import struct
import sys
import tempfile
import threading
import time


def child(args):
    manager = os.environ["SESSION_MANAGER"]
    authority = Path(os.environ["ICEAUTHORITY"])
    assert all(item.startswith(("local/", "unix/")) for item in manager.split(",")), manager
    assert authority.is_file() and authority.stat().st_mode & 0o777 == 0o600
    assert authority.parent.stat().st_mode & 0o777 == 0o700
    (args.output / "owned.json").write_text(json.dumps({"authority": str(authority), "manager": manager, "pid": os.getppid()}))
    rejected = os.environ.copy()
    rejected["ICEAUTHORITY"] = str(args.output / "no-authority")
    subprocess.run([str(args.fixture), "reject"], env=rejected, check=True, timeout=5)
    # An incomplete pre-auth frame must not keep the companion from serving
    # another client. It must also leave the desktop child free to run.
    address = manager.split(",")[0].split(":", 1)[1]
    if address.startswith("@"):
        address = "\0" + address[1:]
    with socket.socket(socket.AF_UNIX) as peer:
        peer.connect(address)
        peer.sendall(b"l")
        subprocess.run([str(args.fixture), "normal"], check=True, timeout=15)
    # A peer sending bytes more often than SO_RCVTIMEO must still be cut off by
    # the whole-message deadline. Advertise a long pre-auth setup payload.
    with socket.socket(socket.AF_UNIX) as peer:
        peer.connect(address)
        order = b'l' if sys.byteorder == 'little' else b'B'
        peer.sendall(b'\0\0' + order + b'\0' * 5)
        peer.sendall(bytes([0, 1, 1, 1]) + struct.pack('=I', 8192))
        disconnected = threading.Event()
        def trickle():
            for _ in range(40):
                try:
                    peer.sendall(b'\0')
                except OSError:
                    disconnected.set()
                    return
                time.sleep(.1)
        sender = threading.Thread(target=trickle)
        sender.start()
        try:
            assert disconnected.wait(1.5), "trickling pre-auth peer evaded message deadline"
        finally:
            try:
                peer.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sender.join(timeout=5)
            assert not sender.is_alive()
    subprocess.run([str(args.fixture), "phase2"], check=True, timeout=15)
    subprocess.run([str(args.fixture), "signal", str(os.getppid())], check=True, timeout=15)
    stalled = subprocess.Popen([str(args.fixture), "hang"])
    try:
        subprocess.run([str(args.fixture), "normal"], check=True, timeout=15)
        assert stalled.wait(timeout=10) == 0, "unresponsive checkpoint was not bounded"
    finally:
        if stalled.poll() is None:
            stalled.terminate()
            stalled.wait(timeout=5)
    subprocess.run([str(args.fixture), "abrupt"], check=True, timeout=15)
    # Nested launchers must preserve the existing manager and authority.
    subprocess.run([str(args.binary), "--", str(args.fixture), "normal"], check=True, timeout=15)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/midnightbsd-session"))
    parser.add_argument("--fixture", type=Path, default=Path("build/xsmp_client_fixture"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.fixture = args.fixture.resolve()
    if args.child:
        child(args)
        return
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-xsmp-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    with tempfile.TemporaryDirectory(prefix="midnight-xsmp-") as temporary:
        root = Path(temporary)
        for name in ("home", "runtime", "config", "config-system", "data", "state"):
            (root / name).mkdir()
        (root / "runtime").chmod(0o700)
        original = root / "original-authority"
        original.write_text("preserve original authority\n")
        env = os.environ.copy()
        for name in ("SESSION_MANAGER", "DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID", "SSH_AUTH_SOCK", "SSH_AGENT_PID"):
            env.pop(name, None)
        env.update(HOME=str(root / "home"), XDG_RUNTIME_DIR=str(root / "runtime"),
                   XDG_CONFIG_HOME=str(root / "config"), XDG_CONFIG_DIRS=str(root / "config-system"),
                   XDG_DATA_HOME=str(root / "data"), XDG_STATE_HOME=str(root / "state"), ICEAUTHORITY=str(original))
        with (args.output / "companion.log").open("w") as log:
            # Supply an existing bus for this run; a separate run below tests
            # automatic bus bootstrapping and complete argv preservation.
            result = subprocess.run(["dbus-run-session", "--", str(args.binary), "--", sys.executable,
                                     str(Path(__file__).resolve()), "--child", "--binary", str(args.binary),
                                     "--fixture", str(args.fixture), "--output", str(args.output)],
                                    env=env, stdout=log, stderr=subprocess.STDOUT, timeout=70)
        if result.returncode:
            print((args.output / "companion.log").read_text(), file=sys.stderr)
            raise RuntimeError("XSMP protocol exercise failed")
        owned = json.loads((args.output / "owned.json").read_text())
        assert not Path(owned["authority"]).exists() and not Path(owned["authority"]).parent.exists()
        for network in owned["manager"].split(","):
            address = network.split(":", 1)[1]
            if not address.startswith("@"):
                assert not Path(address).exists(), "owned ICE socket leaked"
        assert original.read_text() == "preserve original authority\n"
        assert not list((root / "runtime").glob("midnight-session-*")), "authority directory leaked"
        marker = root / "argv marker"
        subprocess.run([str(args.binary), "--", sys.executable, "-c",
                        "from pathlib import Path; import sys; Path(sys.argv[1]).write_text(sys.argv[2])",
                        str(marker), "literal argument with spaces"], env=env, check=True, timeout=10)
        assert marker.read_text() == "literal argument with spaces", "bus wrapper changed argv"
        inherited = env.copy()
        inherited["SESSION_MANAGER"] = "local/existing:/not-contacted"
        subprocess.run([str(args.binary), "--", sys.executable, "-c",
                        "import os,sys; assert os.environ['SESSION_MANAGER']==sys.argv[1]; assert os.environ['ICEAUTHORITY']==sys.argv[2]",
                        inherited["SESSION_MANAGER"], str(original)], env=inherited, check=True, timeout=10)
        # A nonexistent program must propagate 127 and release owned resources.
        result = subprocess.run([str(args.binary), "--", "/no/such/midnight-xsmp-program"],
                                env=env, capture_output=True, timeout=10)
        assert result.returncode == 127
        assert not list((root / "runtime").glob("midnight-session-*"))
        # Signal the actual companion inside a pre-existing bus, and verify that
        # it forwards termination to its owned child and cleans authority state.
        ready = root / "ready.json"
        with (args.output / "termination.log").open("w") as log:
            process = subprocess.Popen(["dbus-run-session", "--", str(args.binary), "--", sys.executable, "-c",
                "import json,os,sys,time; from pathlib import Path; Path(sys.argv[1]).write_text(json.dumps({'pid':os.getppid(),'authority':os.environ['ICEAUTHORITY']})); time.sleep(30)",
                str(ready)], env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 5
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(.05)
                assert ready.exists(), "owned child did not start"
                info = json.loads(ready.read_text())
                os.kill(info["pid"], signal.SIGTERM)
                assert process.wait(timeout=10) == 128 + signal.SIGTERM
                assert not Path(info["authority"]).exists()
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
        assert original.read_text() == "preserve original authority\n"
        # Global checkpoints persist only when opted in. An unsuccessful wrapper
        # exit prevents an additional final checkpoint from replacing the exact
        # protocol result being inspected here.
        config = root / "config/midnight-wm/config.ini"
        config.parent.mkdir(parents=True, exist_ok=True)
        config.write_text("[Session]\nsave_application_checkpoints=true\n")
        snapshot = root / "state/midnight-wm/applications.ini"
        def checkpoint(mode):
            result = subprocess.run([str(args.binary), "--", sys.executable, "-c",
                "import subprocess,sys; subprocess.run(sys.argv[1:],check=True); sys.exit(7)",
                str(args.fixture), mode], env=env, capture_output=True, timeout=25)
            assert result.returncode == 7, result.stderr.decode(errors="replace")
        checkpoint("normal")
        assert snapshot.is_file() and snapshot.stat().st_mode & 0o777 == 0o600
        assert snapshot.parent.stat().st_mode & 0o777 == 0o700
        keys = configparser.ConfigParser(interpolation=None)
        keys.read(snapshot)
        assert keys.getint("Applications", "count") == 1
        assert keys.getint("Application 0", "argc") == 3
        assert base64.b64decode(keys["Application 0"]["arg2"]) == b"literal $(not-a-command) with spaces"
        values = [base64.b64decode(value) for key, value in keys["Application 0"].items()
                  if key not in ("style", "argc")]
        assert not any(b"must-not-persist" in value or b"SESSION_SECRET" in value for value in values)
        original_checkpoint = snapshot.read_bytes()
        checkpoint("fail-save")
        assert snapshot.read_bytes() == original_checkpoint, "failed client save replaced checkpoint"
        checkpoint("stall-save")
        assert snapshot.read_bytes() == original_checkpoint, "timed-out client replaced checkpoint"
        checkpoint("recover-save")
        assert snapshot.read_bytes() == original_checkpoint, "late save authorized another checkpoint"
        snapshot.write_text("invalid checkpoint: preserve me\n")
        checkpoint("normal")
        assert snapshot.read_text() == "invalid checkpoint: preserve me\n", "malformed state overwritten"
        snapshot.unlink()
        checkpoint("never")
        keys.read(snapshot)
        assert keys.getint("Applications", "count") == 0, "RestartNever was persisted"
        snapshot.unlink()
        config.write_text("[Session]\nsave_application_checkpoints=false\n")
        checkpoint("normal")
        assert not snapshot.exists(), "application checkpoints must default to disabled"
        print("Application checkpoints: private persistence, literal argv, excluded properties, failures/timeouts, malformed preservation, RestartNever and opt-in passed.")
        print("XSMP local authentication, registration, checkpoints/phase 2, property round trips, timeouts, disconnects, inherited manager, bus bootstrap, argv, exit status and cleanup passed.")


if __name__ == "__main__":
    main()
