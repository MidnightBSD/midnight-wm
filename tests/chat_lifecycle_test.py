#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Run Chat lifecycle checks with private storage, display and session bus."""
import contextlib
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="midnight-chat-lifecycle-") as temporary:
    root = Path(temporary)
    environment = os.environ.copy()
    for key in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID", "SSH_AUTH_SOCK", "SSH_AGENT_PID"):
        environment.pop(key, None)
    for key, directory in (("HOME", "home"), ("XDG_CONFIG_HOME", "config"),
                           ("XDG_CONFIG_DIRS", "system-config"), ("XDG_DATA_HOME", "data"),
                           ("XDG_CACHE_HOME", "cache"), ("XDG_STATE_HOME", "state"),
                           ("XDG_RUNTIME_DIR", "runtime")):
        (root / directory).mkdir(mode=0o700)
        environment[key] = str(root / directory)
    environment.update(G_DEBUG="fatal-criticals", NO_AT_BRIDGE="1")
    read_fd, write_fd = os.pipe()
    with (root / "xvfb.log").open("w") as log:
        server = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0",
                                   "1280x720x24", "-nolisten", "tcp"],
                                  pass_fds=(write_fd,), stdout=log, stderr=subprocess.STDOUT)
        os.close(write_fd)
        try:
            with os.fdopen(read_fd) as stream:
                display = stream.readline().strip()
            if not display:
                raise RuntimeError("Xvfb failed to start")
            environment["DISPLAY"] = ":" + display
            process = subprocess.Popen(["dbus-run-session", "--", sys.argv[1]],
                                       env=environment, start_new_session=True)
            try:
                if process.wait(timeout=50):
                    raise RuntimeError("Chat lifecycle checks failed")
            finally:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(process.pid, signal.SIGTERM)
                process.wait(timeout=5)
        finally:
            server.terminate()
            server.wait(timeout=5)
