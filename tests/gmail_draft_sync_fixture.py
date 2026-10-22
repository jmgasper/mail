#!/usr/bin/env python3
"""Disposable Gmail draft synchronization provider; never contacts Google."""
import argparse
import base64
from datetime import datetime, timedelta, timezone
from email.parser import BytesParser
import email.policy
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
        self.drafts, self.errors, self.counts = {}, [], {'create': 0, 'update': 0, 'delete': 0}
        self.serial = 0
        self.behavior, self.omit, self.identity = '', '', 'user@example.test'

    def put(self, key, raw, thread='thread'):
        self.serial += 1
        self.drafts[key] = {'raw': raw, 'id': 'm'+str(self.serial), 'thread': thread}

    def record(self, key, raw=False):
        d = self.drafts[key]
        message = {'id': d['id'], 'threadId': d['thread'], 'labelIds': ['DRAFT'], 'internalDate': '1792542600000'}
        if raw:
            message['raw'] = base64.urlsafe_b64encode(d['raw']).decode().rstrip('=')
        return {'id': key, 'message': message}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def respond(self, code, value=None):
        data = json.dumps(value or {}).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def dispatch(self):
        try:
            self.route()
        except (BrokenPipeError, ConnectionResetError):
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
        if url.path == '/fixture/put':
            state.put(body['id'], base64.urlsafe_b64decode(body['raw']+'='*((-len(body['raw'])) % 4)))
            return self.respond(200)
        if url.path == '/fixture/drop':
            state.drafts.pop(body['id'], None)
            return self.respond(200)
        if url.path == '/fixture/reset':
            state.drafts.clear()
            state.counts = {'create': 0, 'update': 0, 'delete': 0}
            state.behavior, state.omit, state.identity = '', '', 'user@example.test'
            return self.respond(200)
        if url.path == '/fixture/settings':
            for key in ('behavior', 'omit', 'identity'):
                if key in body:
                    setattr(state, key, body[key])
            return self.respond(200)
        if url.path == '/fixture/stats':
            return self.respond(200, {**state.counts, 'drafts': list(state.drafts)})
        assert self.headers.get('Authorization') == 'Bearer fixture-access'
        if url.path == '/gmail/profile':
            return self.respond(200, {'emailAddress': state.identity, 'historyId': '100'})
        if url.path == '/gmail/labels':
            return self.respond(200, {'labels': [{'id': 'DRAFT', 'name': 'Drafts', 'type': 'system'}]})
        if url.path == '/gmail/messages':
            return self.respond(200, {'messages': []})
        if url.path.startswith('/gmail/history'):
            return self.respond(200, {'historyId': '100', 'history': []})
        assert url.path.startswith('/gmail/drafts'), url.path
        key = url.path.rsplit('/', 1)[-1]
        if self.command == 'GET' and key == 'drafts':
            assert query['maxResults'] == ['500'] and query['includeSpamTrash'] == ['true']
            ids = sorted(k for k in state.drafts if k != state.omit)
            if query.get('q'):
                text = query['q'][0]
                assert text.startswith('rfc822msgid:')
                ids = [k for k in ids if str(BytesParser(policy=email.policy.default).parsebytes(state.drafts[k]['raw'])['Message-ID']) == text[12:]]
            offset = int(query.get('pageToken', ['0'])[0])
            result = {'drafts': [state.record(k) for k in ids[offset:offset+2]]}
            if offset+2 < len(ids):
                result['nextPageToken'] = str(offset+2)
            return self.respond(200, result)
        if self.command == 'GET':
            return self.respond(200, state.record(key, query.get('format') == ['raw'])) if key in state.drafts else self.respond(404)
        if self.command == 'DELETE':
            state.counts['delete'] += 1
            if key not in state.drafts:
                return self.respond(404)
            del state.drafts[key]
            return self.respond(204)
        assert self.command in ('POST', 'PUT')
        kind = 'create' if self.command == 'POST' else 'update'
        state.counts[kind] += 1
        behavior, state.behavior = state.behavior, ''
        if behavior == 'denied':
            return self.respond(403)
        raw = base64.urlsafe_b64decode(body['message']['raw']+'='*((-len(body['message']['raw'])) % 4))
        mime = BytesParser(policy=email.policy.default).parsebytes(raw)
        assert str(mime['From']) == 'user@example.test' and mime['Message-ID'] is not None
        assert not mime.defects
        if self.command == 'PUT':
            assert body['id'] == key
            if key not in state.drafts:
                return self.respond(404)
        else:
            key = 'created'+str(state.serial+1)
        if behavior == 'rewrite-disconnect':
            raw = b'X-Server-Trace: transformed\r\n'+raw
        state.put(key, raw, body['message'].get('threadId', 'thread'))
        if behavior in ('disconnect', 'rewrite-disconnect'):
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            return
        return self.respond(200, state.record(key))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mail-draft-sync-') as temporary:
        root = Path(temporary)
        cert, key, csr = root/'cert.pem', root/'key.pem', root/'request.pem'
        (root/'index').touch()
        (root/'serial').write_text('01\n')
        config = root/'ca.conf'
        config.write_text('[ca]\ndefault_ca=fixture\n[fixture]\n'
                          f'database={root}/index\nnew_certs_dir={root}\nserial={root}/serial\n'
                          'default_md=sha256\npolicy=policy\nx509_extensions=extensions\n'
                          '[policy]\ncommonName=supplied\n[extensions]\n'
                          'subjectAltName=IP:127.0.0.1,IP:10.0.2.2\nbasicConstraints=critical,CA:TRUE\n')
        subprocess.run(['openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=Draft sync fixture',
                        '-keyout', str(key), '-out', str(csr)], check=True, capture_output=True)
        subprocess.run(['openssl', 'ca', '-selfsign', '-batch', '-notext', '-config', str(config), '-keyfile', str(key),
                        '-in', str(csr), '-out', str(cert), '-days', '1', '-startdate',
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
            print('Draft synchronization provider verified: TLS, pagination, independent MIME parsing, remote edits and interrupted writes.')
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == '__main__':
    main()
