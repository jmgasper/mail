#!/usr/bin/env python3
"""Independent Gmail organization service for host and native interaction tests."""
import base64
import http.server
import json
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import urllib.parse
from pathlib import Path
from google_fixture import Handler as GoogleHandler
from gmail_sync_fixture import message


class State:
    def __init__(self):
        self.errors = []
        self.labels = {name: {'id': name, 'name': name.title(), 'type': 'system'} for name in ('INBOX', 'UNREAD', 'STARRED', 'TRASH', 'SPAM', 'SENT', 'DRAFT')}
        self.labels.update({'Label_1': {'id': 'Label_1', 'name': 'Project', 'type': 'user'}, 'Label_2': {'id': 'Label_2', 'name': 'Empty', 'type': 'user'}})
        self.messages = {'a': message('a', ['INBOX', 'UNREAD', 'Label_1']), 'b': message('b', ['INBOX', 'UNREAD'])}
        self.history = 100
        self.config = {}
        self.requests = []
        self.creates = 0


class Handler(GoogleHandler):
    do_PATCH = GoogleHandler.do_POST
    do_DELETE = GoogleHandler.do_POST

    def route(self):
        state = self.server.state
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        length = int(self.headers.get('Content-Length', 0))
        assert 0 <= length <= 65536
        body = self.rfile.read(length)
        if url.path == '/fixture/state':
            self.respond(200, {'labels': state.labels, 'memberships': {key: value['labelIds'] for key, value in state.messages.items()},
                              'requests': state.requests, 'creates': state.creates, 'messages': list(state.messages)})
            return
        if url.path == '/fixture/config':
            state.config = json.loads(body)
            if state.config.get('inject'):
                state.messages['news'] = message('news', ['INBOX', 'UNREAD'], sender='news@example.test')
                state.history += 1
            if state.config.get('remove'):
                label = state.config['remove']
                state.labels.pop(label, None)
                for item in state.messages.values():
                    item['labelIds'] = [value for value in item['labelIds'] if value != label]
            self.respond(200, {})
            return
        assert self.headers.get('Authorization') == 'Bearer fixture-access'
        assert url.path.startswith('/gmail/')
        path = url.path[6:]
        if path == '/profile':
            self.respond(200, {'emailAddress': 'wrong@example.test' if state.config.get('wrong') else 'user@example.test', 'historyId': str(state.history)})
        elif path == '/drafts' and self.command == 'GET':
            self.respond(200, {'drafts': []})
        elif path == '/labels' and self.command == 'GET':
            self.respond(200, {'labels': list(state.labels.values())})
        elif path == '/labels' and self.command == 'POST':
            request = json.loads(body)
            assert request['name'] not in [label['name'] for label in state.labels.values()], 'duplicate label created during rename'
            state.creates += 1
            label = {'id': f'Label_new{state.creates}', 'name': request['name'], 'type': 'user'}
            state.labels[label['id']] = label
            self.respond(200, label)
        elif path.startswith('/labels/'):
            label_id = path.split('/')[-1]
            assert label_id.startswith('Label_'), 'system label mutation'
            if self.command == 'PATCH':
                request = json.loads(body)
                assert set(request) == {'name'}
                state.requests.append(['rename', label_id, request['name']])
                if state.config.get('fail') == 'rename':
                    self.respond(403, {'error': 'label rename denied'})
                    return
                if label_id not in state.labels:
                    self.respond(404, {})
                    return
                state.labels[label_id]['name'] = request['name']
                if state.config.pop('drop', False):
                    self.connection.shutdown(socket.SHUT_RDWR); self.connection.close(); self.close_connection = True
                    return
                result = dict(state.labels[label_id])
                if state.config.get('bad_receipt'):
                    result['id'] = 'Label_wrong'
                self.respond(200, result)
            elif self.command == 'DELETE':
                assert body == b'', 'DELETE body must be empty'
                state.requests.append(['delete', label_id])
                if state.config.get('fail') == 'delete':
                    self.respond(503, {'error': 'temporary deletion failure'})
                    return
                if label_id not in state.labels:
                    self.respond(404, {})
                    return
                del state.labels[label_id]
                for item in state.messages.values():
                    item['labelIds'] = [value for value in item['labelIds'] if value != label_id]
                self.respond(200, {})
            else:
                raise AssertionError('invalid label verb')
        elif path == '/messages':
            assert self.command == 'GET' and query['includeSpamTrash'] == ['true']
            self.respond(200, {'messages': [{'id': key} for key in state.messages]})
        elif path == '/history':
            self.respond(200, {'historyId': str(state.history), 'history': [{'id': str(state.history), 'messages': [{'id': key} for key in state.messages]}]})
        elif path.startswith('/messages/'):
            pieces = path.split('/')
            key = pieces[2]
            assert key in state.messages, 'unrecognized fixture message'
            item = state.messages[key]
            if len(pieces) == 3:
                result = {k: v for k, v in item.items() if k != 'raw'}
                if query['format'] == ['raw']:
                    result['raw'] = base64.urlsafe_b64encode(item['raw']).decode().rstrip('=')
                self.respond(200, result)
            else:
                assert pieces[3] == 'modify', 'organization must not send, trash or delete messages'
                request = json.loads(body)
                assert all(label in state.labels for label in request['addLabelIds'] + request['removeLabelIds']), 'unknown label used in membership'
                present = set(item['labelIds']); present.update(request['addLabelIds']); present.difference_update(request['removeLabelIds'])
                item['labelIds'] = sorted(present)
                state.requests.append(['membership', key, request])
                self.respond(200, {'id': key, 'threadId': item['threadId']})
        else:
            raise AssertionError('unexpected organization route '+path)


def main():
    state = State()
    with tempfile.TemporaryDirectory(prefix='mail-organization-api-') as temporary:
        root = Path(temporary); cert, key = root/'cert.pem', root/'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-keyout', str(key), '-out', str(cert),
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1,IP:10.0.2.2'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); tls.minimum_version = ssl.TLSVersion.TLSv1_2; tls.load_cert_chain(cert, key)
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.socket = tls.wrap_socket(server.socket, server_side=True); server.state = state
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        try:
            result = subprocess.run([sys.argv[1], '127.0.0.1', str(cert), str(server.server_port)], timeout=180)
            assert result.returncode == 0, (result.returncode, state.errors)
            assert not state.errors, state.errors
            assert {'a', 'b'}.issubset(state.messages), 'label removal must retain messages'
            assert 'Label_1' not in state.labels and 'Label_2' not in state.labels, 'both removals reached server'
            assert any(item[0] == 'rename' for item in state.requests)
            print(f'Independent organization API checks passed: {len(state.requests)} mutations, existing messages retained, no duplicate label creation during rename')
        finally:
            server.shutdown(); server.server_close(); thread.join()


if __name__ == '__main__':
    main()
