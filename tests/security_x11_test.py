#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Run hostile X11 input checks on a private display."""
import os
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="midnight-security-") as temporary:
    read_fd, write_fd = os.pipe()
    with open(os.path.join(temporary, "xvfb.log"), "w") as log:
        server = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0",
                                   "1280x720x24", "-nolisten", "tcp"],
                                  pass_fds=(write_fd,), stdout=log, stderr=subprocess.STDOUT)
        os.close(write_fd)
        try:
            with os.fdopen(read_fd) as stream:
                display = stream.readline().strip()
            if not display:
                raise RuntimeError("Xvfb failed to start")
            environment = os.environ.copy()
            environment["DISPLAY"] = ":" + display
            subprocess.run([sys.argv[1]], env=environment, check=True, timeout=15)
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
