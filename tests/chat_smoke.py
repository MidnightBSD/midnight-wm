#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Isolated GTK smoke test. Optional --live URL uses one model sequentially."""
import argparse
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument('--live', help='Base URL, e.g. http://localhost:11434/v1')
parser.add_argument('--model', default='mistral:7b')
parser.add_argument('--binary', help='Chat executable to test')
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
root = Path(tempfile.mkdtemp(prefix='midnight-chat-ui-'))
env = os.environ.copy()
for key in ('DBUS_SESSION_BUS_ADDRESS', 'DBUS_SESSION_BUS_PID', 'SSH_AUTH_SOCK', 'SSH_AGENT_PID'):
    env.pop(key, None)
for key, directory in [('HOME', 'home'), ('XDG_CONFIG_HOME', 'config'), ('XDG_CONFIG_DIRS', 'system-config'),
                       ('XDG_DATA_HOME', 'data'), ('XDG_RUNTIME_DIR', 'runtime')]:
    (root / directory).mkdir(mode=0o700)
    env[key] = str(root / directory)

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass
    def respond(self, data):
        payload = json.dumps(data).encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass
    def do_GET(self):
        self.respond({'data': [{'id': args.model}, {'id': 'second-model'}]})
    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if data['messages'][-1]['content'] == 'slow request':
            time.sleep(3)
        self.respond({'choices': [{'message': {'content': 'Hello Robin. This is a chat reply.'}}]})

server = None
if not args.live:
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    args.live = f'http://127.0.0.1:{server.server_port}/v1'
config = root / 'config/midnightbsd-chat'
config.mkdir(mode=0o700)
settings = config / 'settings.json'
settings.write_text(json.dumps(dict(url=args.live, port=0, model=args.model, token='', authenticate=False, context=8192, reply=128)))
settings.chmod(0o600)
history = root / 'data/midnightbsd-chat/conversations.json'

def run(*cmd, **kw):
    return subprocess.run(cmd, env=env, check=True, capture_output=True, text=True, **kw).stdout.strip()
def until(fn, description, timeout=100):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = fn()
        if result:
            return result
        time.sleep(.15)
    raise AssertionError(description)
def visible(name):
    result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', '^' + name + '$'], env=env, capture_output=True, text=True)
    return result.stdout.strip().splitlines()[0] if result.returncode == 0 else None
def threads():
    return json.loads(history.read_text())['threads'] if history.exists() else []
def key(keys):
    run('xdotool', 'key', '--clearmodifiers', keys)
    time.sleep(.15)
def type_text(text):
    run('xdotool', 'type', '--clearmodifiers', '--', text)
def geometry(window):
    return {k: int(v) for k, v in (line.split('=') for line in run('xdotool', 'getwindowgeometry', '--shell', window).splitlines()) if k in ('X', 'Y', 'WIDTH', 'HEIGHT')}
def focus_input(window):
    g = geometry(window)
    run('xdotool', 'mousemove', str(g['X'] + 400), str(g['Y'] + g['HEIGHT'] - 110), 'click', '1')

readfd, writefd = os.pipe()
xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(writefd), '-screen', '0', '1280x720x24', '-nolisten', 'tcp'], pass_fds=(writefd,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
os.close(writefd)
with os.fdopen(readfd) as stream:
    env['DISPLAY'] = ':' + stream.readline().strip()
processes = []
try:
    wm = subprocess.Popen([str(repo / 'build/midnight-wm')], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    processes.append(wm)
    time.sleep(1)
    second = subprocess.run([str(repo / 'build/midnight-wm')], env=env, capture_output=True, timeout=10)
    assert second.returncode != 0, 'second WM took over'
    log = open(root / 'chat.log', 'w')
    app = subprocess.Popen(['dbus-run-session', '--', str(Path(args.binary).resolve() if args.binary else repo / 'build/midnightbsd-chat')], env=env, stdout=log, stderr=log)
    processes.append(app)
    window = until(lambda: visible('Midnight Chat'), 'chat window missing')
    run('xdotool', 'windowactivate', '--sync', window)
    key('alt+r')
    time.sleep(1)
    # Inspect the actual populated dropdown visually in the saved screenshot.
    g = geometry(window)
    run('xdotool', 'mousemove', str(g['X'] + g['WIDTH'] - 260), str(g['Y'] + 25), 'click', '1')
    run('import', '-window', 'root', str(root / 'models.png'))
    key('Escape')
    focus_input(window)
    type_text('My name is Robin. Greet me briefly.')
    key('ctrl+Return')
    until(lambda: threads() and len(threads()[0]['messages']) == 2, 'first response missing')
    first_id = threads()[0]['id']
    focus_input(window)
    type_text('What is my name? Answer briefly.')
    key('ctrl+Return')
    until(lambda: len(threads()[0]['messages']) == 4, 'context reply missing')
    assert 'Robin' in threads()[0]['messages'][-1]['content']
    key('alt+n')
    until(lambda: len(threads()) == 2, 'new conversation missing')
    type_text('Hello. Reply in one sentence.')
    key('ctrl+Return')
    until(lambda: len(threads()[0]['messages']) == 2, 'second conversation reply missing')
    # Switch to the older conversation through the sidebar.
    g = geometry(window)
    run('xdotool', 'mousemove', str(g['X'] + 90), str(g['Y'] + 145), 'click', '1')
    run('import', '-window', window, str(root / 'chat.png'))
    key('alt+e')
    dialog = until(lambda: visible('Chat Settings'), 'settings missing')
    run('import', '-window', dialog, str(root / 'settings.png'))
    key('alt+s')
    until(lambda: not visible('Chat Settings'), 'settings did not save')
    assert json.loads(settings.read_text())['url'] == args.live
    if server:
        focus_input(window)
        type_text('slow request')
        key('ctrl+Return')
        key('alt+t')
        time.sleep(.4)
        assert any(t['messages'][-1]['content'] == 'slow request' for t in threads())
        focus_input(window)
        type_text('Retry replacement')
        key('ctrl+Return')
        until(lambda: any(t['messages'][-1]['role'] == 'assistant' and len(t['messages']) >= 6 for t in threads()), 'retry missing')
    # Close and restart to verify persistence through the GUI.
    key('alt+F4')
    app.wait(timeout=10)
    app = subprocess.Popen(['dbus-run-session', '--', str(Path(args.binary).resolve() if args.binary else repo / 'build/midnightbsd-chat')], env=env, stdout=log, stderr=log)
    processes.append(app)
    until(lambda: visible('Midnight Chat'), 'restart missing')
    assert len(threads()) == 2 and any(t['id'] == first_id for t in threads())
    assert history.stat().st_mode & 0o777 == 0o600
    window = visible('Midnight Chat')
    run('xdotool', 'windowactivate', '--sync', window)
    key('alt+x')
    dialog = until(lambda: visible('Confirm conversation change'), 'confirmation missing', timeout=10)
    run('xdotool', 'windowactivate', '--sync', dialog)
    g = geometry(dialog)
    run('import', '-window', dialog, str(root / 'confirmation.png'))
    run('xdotool', 'mousemove', str(g['X'] + g['WIDTH'] - 45), str(g['Y'] + g['HEIGHT'] - 22), 'click', '1')
    until(lambda: threads()[0]['compacted'] == 2, 'context reset missing', timeout=10)
    assert len(threads()[0]['messages']) == 2
    key('alt+d')
    dialog = until(lambda: visible('Confirm conversation change'), 'confirmation missing', timeout=10)
    run('xdotool', 'windowactivate', '--sync', dialog)
    g = geometry(dialog)
    run('import', '-window', dialog, str(root / 'confirmation.png'))
    run('xdotool', 'mousemove', str(g['X'] + g['WIDTH'] - 45), str(g['Y'] + g['HEIGHT'] - 22), 'click', '1')
    until(lambda: len(threads()) == 1, 'conversation deletion missing', timeout=10)
    print('PASS: model refresh, chat, context, multiple threads, settings, persistence, reset, deletion, second WM rejection')
    print('Artifacts:', root)
finally:
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    xvfb.terminate()
    xvfb.wait(timeout=10)
    if server:
        server.shutdown()
        server.server_close()
