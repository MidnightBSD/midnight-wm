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
parser.add_argument('--tools', action='store_true', help='Exercise enabled system-information tool and Tools settings')
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
    def audit(self, body=None):
        if getattr(self.server, 'secondary', False):
            assert self.headers.get('Authorization') == 'Bearer second-secret'
            if body:
                assert body['model'] == 'second-model' and 'tools' not in body
        else:
            assert self.headers.get('Authorization') is None
        server_requests.append((getattr(self.server, 'secondary', False), body))
    def do_GET(self):
        if self.path.startswith('/search'):
            search_requests.append(self.path)
            self.respond({'results': []})
            return
        self.audit()
        if getattr(self.server, 'secondary', False):
            self.respond({'data': [{'id': 'second-model'}]})
            return
        self.respond({'data': [{'id': args.model}, {'id': 'second-model'}]})
    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path.endswith('/tokenize'):
            self.audit()
            self.send_response(404)
            self.end_headers()
            return
        self.audit(data)
        if data['messages'][-1]['content'] == 'slow request':
            time.sleep(3)
        if args.tools and data.get('tools') and data['messages'][-1]['role'] == 'user':
            self.respond({'choices': [{'message': {'content': None, 'tool_calls': [
                {'id': 'system_test', 'type': 'function', 'function': {'name': 'system_information', 'arguments': '{}'}}]}}]})
        elif args.tools and data.get('tools') and data['messages'][-1]['tool_call_id'] == 'system_test' and \
                'name' in next(m['content'] for m in reversed(data['messages']) if m['role'] == 'user').lower():
            # A web search after local results must ask the user before running.
            self.respond({'choices': [{'message': {'content': None, 'tool_calls': [
                {'id': 'web_test', 'type': 'function', 'function': {'name': 'web_search', 'arguments': '{"query":"Robin desktop"}'}}]}}]})
        else:
            self.respond({'choices': [{'message': {'content': 'Hello Robin. This is a chat reply.'}}]})

server = None
second_server = None
server_requests = []
search_requests = []
if not args.live:
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    args.live = f'http://127.0.0.1:{server.server_port}/v1'
    second_server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    second_server.secondary = True
    threading.Thread(target=second_server.serve_forever, daemon=True).start()
config = root / 'config/midnightbsd-chat'
config.mkdir(mode=0o700)
settings = config / 'settings.json'
settings.write_text(json.dumps(dict(url=args.live, port=0, model=args.model, token='', authenticate=False, context=8192, reply=128,
                                    system_information=args.tools, web_search=args.tools, search_provider='searxng',
                                    search_url=args.live.rsplit('/', 1)[0] + '/search')))
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
    run('xdotool', 'mousemove', str(g['X'] + g['WIDTH'] - 260), str(g['Y'] + 60), 'click', '1')
    run('import', '-window', 'root', str(root / 'models.png'))
    key('Escape')
    focus_input(window)
    type_text('My name is Robin. Greet me briefly.')
    key('ctrl+Return')
    if args.tools:
        dialog = until(lambda: visible('Allow web search\\?'), 'web search confirmation missing', timeout=30)
        run('xdotool', 'windowactivate', '--sync', dialog)
        run('import', '-window', dialog, str(root / 'web-search-confirmation.png'))
        key('Return')  # Decline is the default response.
    until(lambda: threads() and len(threads()[0]['messages']) == 2, 'first response missing')
    first_id = threads()[0]['id']
    if args.tools:
        tools_used = threads()[0]['messages'][1]['tools']
        assert tools_used[0]['name'] == 'system_information'
        assert tools_used[1]['name'] == 'web_search' and 'declined by the user' in tools_used[1]['result']
        assert not search_requests, 'declined web search reached the provider'
    focus_input(window)
    type_text('What is my name? Answer briefly.')
    key('ctrl+Return')
    if args.tools:
        dialog = until(lambda: visible('Allow web search\\?'), 'second web search confirmation missing', timeout=30)
        run('xdotool', 'windowactivate', '--sync', dialog)
        key('alt+l')  # Allow all searches for this reply.
    until(lambda: len(threads()[0]['messages']) == 4, 'context reply missing')
    assert 'Robin' in threads()[0]['messages'][-1]['content']
    if args.tools:
        allowed = threads()[0]['messages'][3]['tools'][1]
        assert allowed['name'] == 'web_search' and 'declined' not in allowed['result']
        assert len(search_requests) == 1 and 'q=Robin%20desktop' in search_requests[0], search_requests
    key('alt+c')
    until(lambda: threads()[0]['compacted'] == 2, 'manual compaction missing')
    assert len(threads()[0]['messages']) == 4 and threads()[0]['summary']
    key('alt+n')
    until(lambda: len(threads()) == 2, 'new conversation missing')
    type_text('Hello. Reply in one sentence.')
    key('ctrl+Return')
    until(lambda: len(threads()[0]['messages']) == 2, 'second conversation reply missing')
    # Switch to the older conversation through the sidebar.
    g = geometry(window)
    run('xdotool', 'mousemove', str(g['X'] + 90), str(g['Y'] + 185), 'click', '1')
    run('import', '-window', window, str(root / 'chat.png'))
    key('alt+e')
    dialog = until(lambda: visible('Chat Settings'), 'settings missing')
    key('alt+d'); key('ctrl+a'); type_text('1200')
    run('import', '-window', dialog, str(root / 'settings.png'))
    if args.tools:
        key('ctrl+Page_Down')
        run('import', '-window', dialog, str(root / 'tools.png'))
    key('alt+s')
    until(lambda: not visible('Chat Settings'), 'settings did not save')
    saved_profiles = json.loads(settings.read_text())
    saved = next(item['settings'] for item in saved_profiles['servers'] if item['id'] == saved_profiles['selected_server'])
    assert saved['url'] == args.live
    assert saved['request_timeout'] == 1200
    assert saved['tool_rounds'] == 12 and saved['tool_calls'] == 24
    if args.tools:
        assert saved['system_information'] is True
    if server:
        # Add a distinct endpoint with its own credentials. Keep the selected
        # conversation, then return to the original endpoint and model.
        before = threads()
        before_counts = {t['id']: len(t['messages']) for t in before}
        key('alt+a')
        dialog = until(lambda: visible('Add server'), 'Add server dialog missing', timeout=10)
        key('alt+n'); key('ctrl+a'); type_text('Second server')
        key('alt+u'); key('ctrl+a'); type_text('http://127.0.0.1/v1')
        key('alt+p'); key('ctrl+a'); type_text(str(second_server.server_port))
        key('alt+b'); key('alt+t'); type_text('second-secret')
        key('alt+s')
        until(lambda: not visible('Add server'), 'Adding server failed', timeout=10)
        saved_profiles = json.loads(settings.read_text())
        assert len(saved_profiles['servers']) == 2
        assert threads() == before, 'Adding a server changed conversations'
        key('alt+m'); key('ctrl+a'); type_text('second-model')
        key('alt+r'); time.sleep(.5)
        focus_input(window); type_text('Reply from second server'); key('ctrl+Return')
        until(lambda: any(len(t['messages']) == before_counts[t['id']] + 2 for t in threads()),
              'Second server reply missing', timeout=10)
        assert any(secondary and body for secondary, body in server_requests)
        key('alt+e')
        until(lambda: visible('Chat Settings'), 'Server settings did not open', timeout=10)
        key('alt+n'); key('ctrl+a'); type_text('Second renamed')
        key('alt+s')
        until(lambda: not visible('Chat Settings'), 'Renamed server did not save', timeout=10)
        assert json.loads(settings.read_text())['servers'][1]['name'] == 'Second renamed'
        key('alt+v'); key('Home'); key('Return')
        until(lambda: json.loads(settings.read_text())['selected_server'] == saved_profiles['servers'][0]['id'],
              'Original server was not selected', timeout=10)
        before_counts = {t['id']: len(t['messages']) for t in threads()}
        focus_input(window); type_text('Reply from original server'); key('ctrl+Return')
        until(lambda: any(len(t['messages']) == before_counts[t['id']] + 2 for t in threads()),
              'Original server model/credentials were not restored', timeout=10)
        assert server_requests[-1][0] is False
        run('import', '-window', window, str(root / 'servers.png'))
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
    if server:
        saved_profiles = json.loads(settings.read_text())
        assert len(saved_profiles['servers']) == 2
        requests_before = len(server_requests)
        key('alt+r')
        until(lambda: len(server_requests) > requests_before, 'Server selection failed after restart', timeout=10)
        time.sleep(.5)
        assert server_requests[-1][0] is False, 'Restart restored the wrong server'
        key('alt+v'); key('End'); key('Return')
        before = threads()
        g = geometry(window)
        run('xdotool', 'mousemove', str(g['X'] + g['WIDTH'] - 80), str(g['Y'] + 25), 'click', '1')
        dialog = until(lambda: visible('Remove server'), 'Server removal confirmation missing', timeout=10)
        run('xdotool', 'windowactivate', '--sync', dialog)
        g = geometry(dialog)
        run('xdotool', 'mousemove', str(g['X'] + g['WIDTH'] - 45), str(g['Y'] + g['HEIGHT'] - 22), 'click', '1')
        until(lambda: len(json.loads(settings.read_text())['servers']) == 1, 'Server removal did not save', timeout=10)
        assert threads() == before, 'Removing server changed conversations'
        assert 'second-secret' not in settings.read_text(), 'Removed server credentials remain saved'
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
    print('PASS: model refresh, chat, context, multiple threads, settings, server switching, persistence, reset, deletion, second WM rejection')
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
    if second_server:
        second_server.shutdown()
        second_server.server_close()
    if server:
        server.shutdown()
        server.server_close()
