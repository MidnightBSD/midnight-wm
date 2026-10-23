#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
import http.server
import json
import subprocess
import sys
import tempfile
import threading
import urllib.parse

seen = []
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass
    def respond(self, body):
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    def do_GET(self):
        if self.path != '/search' and not self.path.startswith('/search?'):
            self.respond({'data': [], 'models': []})
            return
        query = urllib.parse.urlparse(self.path)
        seen.append(('search', self.headers.get('Authorization'), self.headers.get('X-Subscription-Token'), urllib.parse.parse_qs(query.query)['q'][0]))
        assert query.path == '/search'
        assert urllib.parse.parse_qs(query.query)['q'][0] in ('test & query', 'fixture query', 'large')
        content = 'A' * 600 if urllib.parse.parse_qs(query.query)['q'][0] == 'large' else 'A search result.'
        self.respond({'results': [{'title': 'Example', 'url': 'https://example.org/' + 'b' * 900, 'content': content}] * 5})
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        messages = request['messages']
        seen.append(('chat', request))
        assert 'model-secret' not in json.dumps(request)
        if messages[0]['content'].startswith('Summarize'):
            assert 'tools' not in request
            assert 'Conversation fragment:' in messages[-1]['content']
            self.respond({'choices': [{'message': {'content': 'Earlier tools reported desktop system data and example web results.'}}]})
            return
        tools = [m for m in messages if m['role'] == 'tool']
        for i, msg in enumerate(messages):
            if msg['role'] == 'tool':
                assert messages[i-1]['tool_calls'][0]['id'] == msg['tool_call_id']
        model = request['model']
        if model in ('fixture-loop', 'fixture-batch', 'fixture-stubborn', 'fixture-stubborn-text', 'fixture-custom', 'fixture-custom-calls') and 'tools' not in request:
            assert len(tools) == (24 if model == 'fixture-batch' else 2 if model == 'fixture-custom' else 5 if model == 'fixture-custom-calls' else 12)
            assert request['tool_choice'] == 'none'
            assert 'budget for this reply is exhausted' in messages[0]['content']
            message = {'role': 'assistant', 'content': 'Answer from collected results'}
            if model.startswith('fixture-stubborn'):
                message['tool_calls'] = [{'id': 'ignored', 'type': 'function', 'function': {'name': 'system_information', 'arguments': '{}'}}]
                message['content'] = 'Partial answer' if model.endswith('-text') else None
            self.respond({'choices': [{'message': message}]})
            return
        if model == 'fixture-repeat':
            if len(tools) == 3:
                assert all('Repeated search skipped' in row['content'] for row in tools[1:])
                self.respond({'choices': [{'message': {'content': 'Used existing search results'}}]})
                return
            args = '{ "query" : "fixture query" }' if tools else '{"query":"fixture query"}'
            self.respond({'choices': [{'message': {'content': None, 'tool_calls': [
                {'id': 'repeat_' + str(len(tools)), 'type': 'function', 'function': {'name': 'web_search', 'arguments': args}}]}}]})
            return
        if model == 'fixture-large':
            name, args = 'web_search', '{"query":"large"}'
        elif model == 'fixture-loop':
            name, args = 'system_information', '{}'
        elif model == 'fixture-unknown':
            name, args = 'run_shell', '{"command":"touch /tmp/chat-must-not-execute"}'
        else:
            name, args = 'system_information', '{}'
        if tools and model not in ('fixture-loop', 'fixture-batch', 'fixture-stubborn', 'fixture-stubborn-text', 'fixture-custom', 'fixture-custom-calls'):
            if model == 'fixture-tools':
                assert any('Operating system:' in m['content'] for m in tools)
                assert any('https://example.org/' in m['content'] for m in tools)
            self.respond({'choices': [{'message': {'role': 'assistant', 'content': 'Verified tools'}}]})
            return
        calls = [{'id': 'call_' + str(len(tools)), 'type': 'function', 'function': {'name': name, 'arguments': args}}]
        if model in ('fixture-batch', 'fixture-custom-calls'):
            calls = [{'id': 'call_' + str(len(tools) + i), 'type': 'function', 'function': {'name': name, 'arguments': args}} for i in range(3)]
        if model == 'fixture-tools':
            calls.append({'id': 'web_0', 'type': 'function', 'function': {'name': 'web_search', 'arguments': '{"query":"fixture query"}'}})
        self.respond({'choices': [{'message': {'role': 'assistant', 'content': None, 'tool_calls': calls}}]})

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix='chat-tools-') as root:
        subprocess.run([sys.argv[1], 'http://127.0.0.1', str(server.server_port), root], check=True, timeout=45)
    summaries = [row[1] for row in seen if row[0] == 'chat' and row[1]['messages'][0]['content'].startswith('Summarize')]
    assert summaries and any('Tool system_information' in row['messages'][-1]['content'] for row in summaries)
    repeat_start = next(i for i, row in enumerate(seen) if row[0] == 'chat' and row[1]['model'] == 'fixture-repeat')
    assert sum(row[0] == 'search' for row in seen[repeat_start:]) == 1
    assert all(row[1] is None and row[2] is None for row in seen if row[0] == 'search')
    schemas = next(row[1]['tools'] for row in seen if row[0] == 'chat' and 'tools' in row[1])
    assert {tool['function']['name'] for tool in schemas} == {'file_search', 'system_information', 'mport_search', 'mport_count', 'web_search'}
    print('PASS: tool call/result protocol and model credentials isolated from search provider')
finally:
    server.shutdown()
    server.server_close()
