#!/usr/bin/env python3
"""Stateful draft API and independent MIME checks; no Google account or delivery."""
import argparse
import base64
from datetime import datetime, timedelta, timezone
import email.policy
from email.parser import BytesParser
import http.server
import json
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading
import urllib.parse


class State:
    def __init__(self):
        self.errors, self.sent, self.attempts = [], [], []
        self.leaked, self.refreshes, self.serial = False, 0, 3
        self.drafts = {}
        for number in range(1, 4):
            self.drafts[f'server_{number}'] = {
                'raw': (f'From: author@example.test\r\nTo: recipient@example.test\r\n'
                        f'Bcc: hidden@example.test\r\nSubject: Seed {number}\r\n'
                        f'Message-ID: <seed-{number}@example.test>\r\n\r\nSaved draft\r\n').encode(),
                'message': f'message_{number}', 'thread': 'thread-seed'}

    def record(self, draft_id, raw=False):
        saved = self.drafts[draft_id]
        message = {'id': saved['message'], 'threadId': saved['thread'], 'labelIds': ['DRAFT'],
                   'historyId': str(self.serial), 'internalDate': '1792542600000'}
        if raw:
            message['raw'] = base64.urlsafe_b64encode(saved['raw']).decode().rstrip('=')
        return {'id': draft_id, 'message': message}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass

    def respond(self, status, value, headers=None):
        data = value if isinstance(value, bytes) else json.dumps(value).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Connection', 'close')
        for name, content in (headers or {}).items():
            self.send_header(name, content)
        self.end_headers()
        self.wfile.write(data)
        self.close_connection = True

    def disconnect(self):
        self.close_connection = True
        self.connection.shutdown(socket.SHUT_RDWR)
        self.connection.close()

    def handle(self):
        try:
            super().handle()
        except (ConnectionError, ssl.SSLError):
            pass

    def dispatch(self):
        try:
            self.route()
        except (ConnectionError, ssl.SSLError):
            pass
        except Exception as error:
            self.server.state.errors.append(str(error))
            self.respond(500, {'error': 'fixture assertion failed'})

    do_GET = do_POST = do_PUT = do_DELETE = dispatch

    def route(self):
        state = self.server.state
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        length = int(self.headers.get('Content-Length', '0'))
        assert 0 <= length <= 48*1024*1024
        body = json.loads(self.rfile.read(length)) if length else {}
        if url.path == '/fixture/stats':
            return self.respond(200, {'sent': state.sent, 'leaked': state.leaked})
        if url.path == '/fixture/replace':
            saved = state.drafts[body['id']]
            saved['raw'] = b'From: author@example.test\r\nTo: other-client@example.test\r\nSubject: Changed elsewhere\r\n\r\nOther content'
            saved['message'] = 'message-other-client'
            return self.respond(200, {})
        if url.path == '/leak':
            state.leaked = True
            return self.respond(200, {})
        assert url.path.startswith('/gmail/drafts') or url.path == '/gmail/profile', url.path
        if self.headers.get('Authorization') == 'Bearer expired-access':
            state.refreshes += 1
            return self.respond(401, {'error': 'expired'})
        assert self.headers.get('Authorization') == 'Bearer fixture-access'
        if url.path == '/gmail/profile':
            assert self.command == 'GET'
            return self.respond(200, {'emailAddress': 'author@example.test', 'historyId': '100'})
        if self.command == 'GET' and url.path == '/gmail/drafts':
            assert query['maxResults'] == ['500'] and query['includeSpamTrash'] == ['true'], query
            if query.get('pageToken') == ['oversized']:
                return self.respond(200, {'drafts': [state.record('server_1')]*501})
            if query.get('q'):
                text = query['q'][0]
                assert text.startswith('rfc822msgid:'), text
                ids = [key for key, value in state.drafts.items()
                       if str(BytesParser(policy=email.policy.default).parsebytes(value['raw'])['Message-ID']) == text[12:]]
                return self.respond(200, {'drafts': [state.record(key) for key in ids]})
            if query.get('pageToken'):
                assert query['pageToken'] == ['page+2 /'], query
                return self.respond(200, {'drafts': [state.record('server_3')]})
            return self.respond(200, {'drafts': [state.record('server_1'), state.record('server_2')], 'nextPageToken': 'page+2 /'})
        draft_id = url.path.rsplit('/', 1)[-1]
        if self.command == 'GET':
            assert query.get('format') in (['raw'], ['minimal']), query
            if draft_id == 'wrong_id':
                return self.respond(200, state.record('server_1', True))
            if draft_id == 'bad_message':
                return self.respond(200, {'id': draft_id, 'message': 'invalid'})
            if draft_id == 'bad_raw':
                return self.respond(200, {'id': draft_id, 'message': {'id': 'bad', 'threadId': 'thread', 'raw': '%%%'}})
            if draft_id not in state.drafts:
                return self.respond(404, {})
            return self.respond(200, state.record(draft_id, query['format'] == ['raw']))
        if self.command == 'DELETE':
            if draft_id not in state.drafts:
                return self.respond(404, {})
            removed = state.drafts.pop(draft_id)
            subject = str(BytesParser(policy=email.policy.default).parsebytes(removed['raw'])['Subject'])
            state.attempts.append(('DELETE', subject))
            if subject == 'Delete disconnect':
                return self.disconnect()
            return self.respond(204, b'')
        assert self.headers.get_content_type() == 'application/json'
        assert isinstance(body.get('message'), dict) and isinstance(body['message'].get('raw'), str), body.keys()
        encoded = body['message']['raw']
        raw = base64.urlsafe_b64decode(encoded+'='*((-len(encoded)) % 4))
        assert base64.urlsafe_b64encode(raw).decode().rstrip('=') == encoded
        message = BytesParser(policy=email.policy.default).parsebytes(raw)
        subject = str(message['Subject'] or '')
        state.attempts.append((self.command, subject))
        assert str(message['From']) == 'author@example.test'
        assert not message.defects, message.defects
        assert message['Message-ID'] is not None
        if subject:
            assert [(a.display_name, a.addr_spec) for a in message['To'].addresses] == [('Recipient, Team', 'recipient@example.test')]
            assert [a.addr_spec for a in message['Cc'].addresses] == ['copy@example.test']
            assert [a.addr_spec for a in message['Bcc'].addresses] == ['hidden@example.test']
            files = {part.get_filename(): part.get_payload(decode=True) for part in message.walk() if part.get_filename()}
            assert files == {'Photo 🐦.png': b'\x89PNG\r\n\x1a\n\x00\xff', 'résumé.txt': b'notes\x00end'}, files.keys()
            html = message.get_body(preferencelist=('html',)).get_content()
            assert html.count('cid:photo@example.test') == 2 and '<strong>' in html
            assert 'https://example.test/a?b=1&amp;c=2' in html
            assert str(message['In-Reply-To']) == '<previous@example.test>'
        else:
            assert message['To'] is None and message['Cc'] is None and message['Bcc'] is None
        if subject in ('Create denied', 'Rate limited', 'Redirect'):
            code = {'Create denied': 403, 'Rate limited': 429, 'Redirect': 307}[subject]
            return self.respond(code, {'error': 'fixture-secret'}, {'Location': f'https://127.0.0.1:{self.server.server_port}/leak'})
        if url.path == '/gmail/drafts/send':
            assert self.command == 'POST'
            if body['id'] not in state.drafts:
                return self.respond(404, {})
            if subject == 'Send journal denied':
                return self.respond(403, {'error': 'Denied'})
            del state.drafts[body['id']]
            # This checks the supplied immutable outbox body, not the changed
            # recipients/content previously stored in the draft resource.
            assert subject == 'Final 🎉' or subject.startswith('Send '), subject
            state.sent.append(subject)
            if subject in ('Send disconnect', 'Send journal disconnect'):
                return self.disconnect()
            if subject == 'Send malformed':
                return self.respond(200, {'id': 'missing-thread'})
            if subject == 'Send 503':
                return self.respond(503, {})
            return self.respond(200, {'id': f'sent_{len(state.sent)}', 'threadId': body['message']['threadId']})
        state.serial += 1
        if self.command == 'PUT':
            assert body['id'] == draft_id and draft_id in state.drafts
        else:
            assert self.command == 'POST' and url.path == '/gmail/drafts'
            draft_id = f'created_{state.serial}'
        state.drafts[draft_id] = {'raw': raw, 'message': f'message_{state.serial}',
                                  'thread': body['message'].get('threadId', 'new-thread')}
        if subject in ('Create disconnect', 'Update disconnect'):
            return self.disconnect()
        if subject == 'Create malformed':
            return self.respond(200, {'id': draft_id})
        if subject == 'Update malformed':
            result = state.record(draft_id)
            result['id'] = 'different-id'
            return self.respond(200, result)
        if subject == 'Create 503':
            return self.respond(503, {})
        return self.respond(200, state.record(draft_id))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='gmail-drafts-', dir=root/'.cache') as temporary:
        directory = Path(temporary)
        cert, key, request = directory/'cert.pem', directory/'key.pem', directory/'request.pem'
        (directory/'index').touch()
        (directory/'serial').write_text('01\n')
        config = directory/'ca.conf'
        config.write_text('[ca]\ndefault_ca=fixture\n[fixture]\n'
                          f'database={directory}/index\nnew_certs_dir={directory}\nserial={directory}/serial\n'
                          'default_md=sha256\npolicy=policy\nx509_extensions=extensions\n'
                          '[policy]\ncommonName=supplied\n[extensions]\n'
                          'subjectAltName=IP:127.0.0.1,IP:10.0.2.2\nbasicConstraints=critical,CA:TRUE\n')
        subprocess.run(['openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=Mail draft fixture',
                        '-keyout', str(key), '-out', str(request)], check=True, capture_output=True)
        subprocess.run(['openssl', 'ca', '-selfsign', '-batch', '-notext', '-config', str(config), '-keyfile', str(key),
                        '-in', str(request), '-out', str(cert), '-days', '1', '-startdate',
                        (datetime.now(timezone.utc)-timedelta(minutes=5)).strftime('%Y%m%d%H%M%SZ')], check=True, capture_output=True)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.minimum_version = ssl.TLSVersion.TLSv1_2
        tls.load_cert_chain(cert, key)
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        server.state = State()
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            result = subprocess.run([args.executable, '127.0.0.1', str(cert), str(server.server_port)], timeout=100)
            assert not server.state.errors, server.state.errors
            assert result.returncode == 0, result.returncode
            state = server.state
            assert state.sent == ['Final 🎉', 'Send disconnect', 'Send malformed', 'Send 503',
                                  'Send journal accepted', 'Send journal disconnect'], state.sent
            assert state.refreshes == 1 and not state.leaked
            for kind in ('Create disconnect', 'Create malformed', 'Create 503', 'Update disconnect', 'Update malformed',
                         'Create denied', 'Rate limited', 'Redirect', 'Send disconnect', 'Send malformed', 'Send 503'):
                assert sum(subject == kind for _, subject in state.attempts) == 1, (kind, state.attempts)
            assert sum(method == 'DELETE' and subject == 'Delete disconnect' for method, subject in state.attempts) == 1
            for outcome in ('accepted', 'disconnect', 'denied', 'missing'):
                assert sum(subject == 'Send journal '+outcome for _, subject in state.attempts) == 1
            print('Draft server verified: MIME, Unicode names, BCC, repeated CID images, stable draft IDs, changing message IDs, exact mutation counts, no uncertain replay or credential redirect.')
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == '__main__':
    main()
