#!/usr/bin/env python3
"""Disposable HTTPS OAuth/Gmail fixture; no Google account or external delivery.

Independently verifies PKCE, redirects, refresh grants, API request shapes and
the submitted MIME envelope. Also runs the same binary on Haiku via an adapter.
"""
import argparse
import base64
import email.policy
import hashlib
import http.server
import json
import re
import socket
import ssl
import subprocess
import tempfile
import threading
import urllib.parse
from email.parser import BytesParser
from pathlib import Path

SCOPE = 'https://www.googleapis.com/auth/gmail.modify'
RAW = ('From: Sender <sender@example.test>\r\nTo: user@example.test\r\n'
       'Subject: Gmail fixture\r\nMessage-ID: <gmail-one@example.test>\r\n'
       'Content-Type: text/plain; charset=utf-8\r\n\r\nHello from Gmail 🎉\r\n').encode()


class State:
    def __init__(self):
        self.errors = []
        self.grants = {}
        self.auth_count = 0
        self.refresh_count = 0
        self.revocations = 0
        self.modified = []
        self.trash = []
        self.created = []
        self.sent = []
        self.send_attempts = {}
        self.leaked = False
        self.states = set()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass

    def handle(self):
        try:
            super().handle()
        except (ConnectionError, ssl.SSLError):
            # Certificate rejection and deliberately aborted uploads close the
            # socket before a complete HTTP request can reach dispatch().
            pass

    def respond(self, status, value, headers=None):
        data = json.dumps(value).encode() if not isinstance(value, bytes) else value
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Connection', 'close')
        for name, content in (headers or {}).items():
            self.send_header(name, content)
        self.end_headers()
        self.wfile.write(data)
        self.close_connection = True

    def do_GET(self):
        self.dispatch()

    def do_POST(self):
        self.dispatch()

    def dispatch(self):
        try:
            self.route()
        except (ConnectionError, BrokenPipeError, ssl.SSLError):
            pass
        except Exception as error:
            self.server.state.errors.append(str(error))
            self.respond(500, {'error': 'fixture assertion failed'})

    def route(self):
        state = self.server.state
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query, strict_parsing=True)
        length = int(self.headers.get('Content-Length', 0))
        assert 0 <= length <= 48 * 1024 * 1024
        body = self.rfile.read(length)
        if url.path == '/authorize':
            assert self.command == 'GET' and not self.headers.get('Authorization')
            assert query['response_type'] == ['code']
            assert query['scope'] == [SCOPE]
            assert query['access_type'] == ['offline']
            assert set(query['prompt'][0].split()) == {'consent', 'select_account'}
            assert query['code_challenge_method'] == ['S256']
            assert re.fullmatch('[A-Za-z0-9_-]{43}', query['code_challenge'][0])
            assert re.fullmatch('[A-Za-z0-9_-]{43}', query['state'][0])
            assert query['state'][0] not in state.states, 'OAuth state reused'
            state.states.add(query['state'][0])
            callback = urllib.parse.urlsplit(query['redirect_uri'][0])
            assert callback.scheme == 'http' and callback.hostname == '127.0.0.1'
            assert callback.port and callback.path == '/oauth/callback'
            state.auth_count += 1
            code = f'code-{state.auth_count}'
            state.grants[code] = query
            fields = {'state': query['state'][0], 'code': code}
            if query['client_id'] == ['denied']:
                fields = {'state': query['state'][0], 'error': 'access_denied'}
            redirect = query['redirect_uri'][0] + '?' + urllib.parse.urlencode(fields)
            self.respond(302, {}, {'Location': redirect})
            return
        if url.path in ('/token', '/redirect-token'):
            assert self.command == 'POST' and not self.headers.get('Authorization')
            assert self.headers['Content-Type'] == 'application/x-www-form-urlencoded'
            if url.path == '/redirect-token':
                self.respond(302, {}, {'Location': '/leak'})
                return
            form = urllib.parse.parse_qs(body.decode(), strict_parsing=True)
            assert form['client_secret'] == ['fixture-secret']
            client = form['client_id'][0]
            if form['grant_type'] == ['authorization_code']:
                grant = state.grants.pop(form['code'][0])
                assert grant['client_id'] == form['client_id']
                assert grant['redirect_uri'] == form['redirect_uri']
                verifier = form['code_verifier'][0]
                assert re.fullmatch('[A-Za-z0-9_-]{43,128}', verifier)
                challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip('=')
                assert grant['code_challenge'] == [challenge], 'PKCE verification failed'
                reply = {'access_token': 'valid-access', 'refresh_token': 'valid-refresh',
                         'expires_in': 3600, 'token_type': 'Bearer', 'scope': SCOPE}
                if client == 'no-refresh':
                    del reply['refresh_token']
                if client == 'wrong-scope':
                    reply['scope'] = 'https://www.googleapis.com/auth/gmail.readonly'
                if client == 'empty-scope':
                    reply['scope'] = ''
                if client == 'bad-expiry':
                    reply['expires_in'] = -1
            else:
                assert form['grant_type'] == ['refresh_token']
                state.refresh_count += 1
                if form['refresh_token'] == ['revoked']:
                    self.respond(400, {'error': 'invalid_grant', 'error_description': 'SECRET-MUST-NOT-LEAK'})
                    return
                assert form['refresh_token'] == ['valid-refresh']
                reply = {'access_token': 'refreshed-access', 'expires_in': 3600, 'token_type': 'bearer'}
            self.respond(200, reply)
            return
        if url.path == '/revoke':
            assert self.command == 'POST'
            form = urllib.parse.parse_qs(body.decode())
            assert form['token'][0] in ('valid-refresh', 'revoked')
            state.revocations += 1
            self.respond(400 if form['token'] == ['revoked'] else 200,
                         {'error': 'invalid_token'} if form['token'] == ['revoked'] else {})
            return
        if url.path == '/leak':
            state.leaked = True
            self.respond(200, {})
            return
        if url.path == '/large':
            self.respond(200, b' ' * 4096)
            return
        if url.path == '/deep':
            self.respond(200, b'{"x":' * 40 + b'{}' + b'}' * 40)
            return
        if url.path == '/redirect/profile':
            self.respond(302, {}, {'Location': '/leak'})
            return
        if url.path == '/limited/profile':
            self.respond(429, {'error': {'message': 'SECRET-MUST-NOT-LEAK'}})
            return
        assert url.path.startswith('/gmail/'), url.path
        assert self.headers.get('Authorization', '').startswith('Bearer ')
        if self.headers['Authorization'] not in ('Bearer valid-access', 'Bearer refreshed-access'):
            self.respond(401, {'error': {'message': 'SECRET-MUST-NOT-LEAK'}})
            return
        path = url.path[len('/gmail'):]
        if path == '/profile':
            self.respond(200, {'emailAddress': 'user@example.test', 'historyId': '90071992547409931'})
        elif path == '/labels' and self.command == 'GET':
            self.respond(200, {'labels': [{'id': 'INBOX', 'name': 'INBOX', 'type': 'system'},
                                         {'id': 'Label_42', 'name': 'Project 🐦', 'type': 'user'}]})
        elif path == '/labels':
            request = json.loads(body)
            assert request == {'name': 'New 🐦', 'labelListVisibility': 'labelShow', 'messageListVisibility': 'show'}
            state.created.append(request['name'])
            self.respond(200, {'id': 'Label_43', 'name': request['name'], 'type': 'user'})
        elif path == '/messages':
            assert query['includeSpamTrash'] == ['true'] and query['maxResults'] == ['500']
            if 'pageToken' in query:
                assert query['pageToken'] == ['next+/=&page']
                self.respond(200, {'messages': [{'id': 'm2', 'threadId': 'thread1'}]})
            else:
                self.respond(200, {'messages': [{'id': 'm1', 'threadId': 'thread1'}], 'nextPageToken': 'next+/=&page'})
        elif path == '/history':
            if query['startHistoryId'] == ['1']:
                self.respond(404, {'error': {'message': 'expired cursor'}})
            elif 'pageToken' in query:
                assert query['pageToken'] == ['history-page']
                self.respond(200, {'historyId': '90071992547409933',
                                  'history': [{'id': '90071992547409933', 'messagesDeleted': [{'message': {'id': 'deleted'}}]}]})
            else:
                assert query['startHistoryId'] == ['90071992547409931']
                self.respond(200, {'historyId': '90071992547409933', 'nextPageToken': 'history-page', 'history': [
                    {'id': '90071992547409932', 'messages': [{'id': 'm1'}],
                     'messagesAdded': [{'message': {'id': 'm2'}}],
                     'labelsAdded': [{'message': {'id': 'm1'}, 'labelIds': ['STARRED']}],
                     'labelsRemoved': [{'message': {'id': 'm2'}, 'labelIds': ['UNREAD']}]}]})
        elif path == '/messages/send':
            data = json.loads(body)
            raw = base64.urlsafe_b64decode(data['raw'] + '=' * (-len(data['raw']) % 4))
            message = BytesParser(policy=email.policy.default).parsebytes(raw)
            assert not message.defects
            subject = str(message['Subject'])
            state.send_attempts[subject] = state.send_attempts.get(subject, 0) + 1
            if subject == 'Refused send':
                self.respond(400, {'error': {'message': 'SECRET-MUST-NOT-LEAK'}})
                return
            if subject in ('API fixture 🎉', 'Refreshed send', 'BCC only'):
                assert data['threadId'] == 'thread1'
                if subject == 'BCC only':
                    assert not message['To'].addresses and message['Cc'] is None
                    assert message['To'].groups[0].display_name == 'undisclosed-recipients'
                else:
                    assert str(message['To']) == 'visible@example.test'
                    assert str(message['Cc']) == 'copy@example.test'
                assert str(message['Bcc']) == 'hidden@example.test'
                assert str(message['In-Reply-To']) == '<parent@example.test>'
                assert 'Hello 🎉' in message.get_body(preferencelist=('plain',)).get_content()
                assert '<strong>' in message.get_body(preferencelist=('html',)).get_content()
                attachments = list(message.iter_attachments())
                assert len(attachments) == 1 and attachments[0].get_payload(decode=True) == b'\x00\x01\xfffile'
                state.sent.append(subject)
            if subject == 'Lost response':
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                self.close_connection = True
            elif subject == 'Server error':
                self.respond(503, {'error': 'unknown result'})
            elif subject == 'Malformed receipt':
                self.respond(200, {'id': 'accepted'})
            else:
                self.respond(200, {'id': 'sent1', 'threadId': 'thread1', 'labelIds': ['SENT']})
        elif path in ('/messages/m1/modify', '/messages/m1/trash', '/messages/m1/untrash'):
            assert self.command == 'POST'
            request = json.loads(body)
            if path.endswith('modify'):
                assert request == {'addLabelIds': ['STARRED', 'Label_42'], 'removeLabelIds': ['UNREAD']}
                state.modified.append(request)
            else:
                assert request == {}
                state.trash.append(path.rsplit('/', 1)[-1])
            self.respond(200, {'id': 'm1', 'threadId': 'thread1'})
        elif path.startswith('/messages/'):
            name = path.rsplit('/', 1)[-1]
            if name == 'deleted':
                self.respond(404, {'error': 'deleted'})
                return
            if name == 'malformed':
                self.respond(200, b'{"id":')
                return
            value = {'id': name, 'threadId': 'thread1', 'historyId': '90071992547409932',
                     'labelIds': ['INBOX', 'SENT', 'UNREAD', 'Label_42'], 'internalDate': '1792542600123'}
            if name == 'wrong_id':
                value['id'] = 'm2'
            if query['format'] == ['raw']:
                value['raw'] = base64.urlsafe_b64encode(RAW).decode().rstrip('=')
                if name == 'bad_base64':
                    value['raw'] = 'A===' 
            else:
                assert query['format'] == ['minimal']
            self.respond(200, value)
        else:
            raise AssertionError(f'unexpected request {path}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable')
    args = parser.parse_args()
    state = State()
    with tempfile.TemporaryDirectory(prefix='mail-google-') as temporary:
        root = Path(temporary)
        cert, key = root / 'cert.pem', root / 'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-keyout', str(key), '-out', str(cert), '-subj', '/CN=localhost',
                        '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1,IP:10.0.2.2'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.minimum_version = ssl.TLSVersion.TLSv1_2
        tls.load_cert_chain(cert, key)
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        server.state = state
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            result = subprocess.run([args.executable, '127.0.0.1', str(cert), str(server.server_port)], timeout=100)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
    assert result.returncode == 0, f'Google client tests exited {result.returncode}'
    assert not state.errors, state.errors
    assert not state.leaked, 'redirect followed with credentials'
    assert state.auth_count == 6 and state.refresh_count == 2 and state.revocations == 2, vars(state)
    assert len(state.modified) == 1 and state.trash == ['trash', 'untrash']
    assert state.created == ['New 🐦'] and state.sent == ['API fixture 🎉', 'Refreshed send', 'BCC only']
    assert len(state.send_attempts) == 7 and all(value == 1 for value in state.send_attempts.values()), state.send_attempts
    print('Independent OAuth/Gmail fixture: PKCE, refresh, redirects, labels, BCC/MIME and no ambiguous retries verified')


if __name__ == '__main__':
    main()
