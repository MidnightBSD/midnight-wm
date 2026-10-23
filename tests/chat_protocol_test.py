#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Bounded local HTTP fixtures; no external endpoint or user settings."""
import http.server
import json
import subprocess
import sys
import tempfile
import threading
import time

requests = []
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        requests.append((self.path, self.headers.get('Authorization'), None))
        if '/slow/' in self.path:
            time.sleep(1)
        if '/unauthorized/' in self.path:
            self.send_response(401)
            self.end_headers()
            return
        if '/redirect/' in self.path:
            self.send_response(302)
            self.send_header('Location', '/leaked')
            self.end_headers()
            return
        if self.path.endswith('/api/ps'):
            self.respond(json.dumps({'models': [
                {'name': 'fixture-running:latest', 'context_length': 3072},
                {'model': 'unrelated', 'context_length': 512},
                {'name': 'fixture-invalid', 'context_length': '2048'}]}).encode())
            return
        data = json.dumps({'data': [{'id': 'fixture'}, {'id': 'second'},
            {'id': 'fixture-running', 'context_length': 32768},
            {'id': 'fixture-metadata', 'contextWindow': 4096},
            {'id': 'fixture-invalid', 'context_length': -2},
            {'id': 'fixture-small', 'context_window': 2048}]}).encode()
        if '/malformed/' in self.path:
            data = b'{bad'
        if '/wrong-shape/' in self.path:
            data = b'{"data": 4}'
        if '/oversized/' in self.path:
            data = b' ' * (1024 * 1024 + 1)
        self.respond(data)

    def respond(self, data):
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append((self.path, self.headers.get('Authorization'), body))
        assert self.path.endswith('/v1/chat/completions')
        assert body['model'].startswith('fixture') and body['stream'] is False
        if body['model'] == 'fixture-small':
            assert 256 + sum(len(m['content'].encode()) + 32 for m in body['messages']) + body['max_tokens'] <= 2048
        assert 'tools' not in body
        if body['model'] == 'fixture-fail' and body['messages'][0]['content'].startswith('Summarize'):
            self.send_response(500)
            self.end_headers()
            return
        if body['model'] == 'fixture-cancel' and body['messages'][0]['content'].startswith('Summarize'):
            time.sleep(1)
        answer = 'Robin enjoys walking by the sea.' if 'Summarize' in body['messages'][0]['content'] else 'Hello Robin.'
        self.respond(json.dumps({'choices': [{'message': {'role': 'assistant', 'content': answer}}]}).encode())

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    with tempfile.TemporaryDirectory(prefix='midnight-chat-protocol-') as root:
        subprocess.run([sys.argv[1], 'http://127.0.0.1/v1', str(server.server_port), root], check=True, timeout=45)
    assert any(auth is None and body for _, auth, body in requests)
    assert any(auth == 'Bearer fixture-secret' and body for _, auth, body in requests)
    assert not any(path == '/leaked' for path, _, _ in requests)
    assert any(body and 'Summarize' in body['messages'][0]['content'] for _, _, body in requests)
    print('PASS: HTTP requests verified, no auth header in no-auth mode, bearer token in auth mode')
finally:
    server.shutdown()
    server.server_close()
