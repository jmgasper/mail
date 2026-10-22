#!/usr/bin/env python3
"""Stateful Gmail HTTPS fixture for cache, history, journal and outbox integration."""
import argparse
import base64
import email.policy
import http.server
import json
import socket
import ssl
import subprocess
import tempfile
import threading
import urllib.parse
from email.parser import BytesParser
from pathlib import Path
from google_fixture import Handler as GoogleHandler


def message(name, labels, thread=None, sender='sender@example.test'):
    raw = (f'From: {sender}\r\nTo: user@example.test\r\nSubject: Message {name}\r\n'
           f'Message-ID: <{name}@example.test>\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nMail {name} 🎉\r\n').encode()
    return {'id': name, 'threadId': thread or f't-{name}', 'historyId': '100', 'labelIds': labels,
            'internalDate': '1792542600123', 'raw': raw}


class State:
    def __init__(self):
        self.errors = []
        self.phase = 'full'
        self.history = '100'
        self.labels = [{'id': name, 'name': name, 'type': 'system'} for name in
                       ('INBOX', 'SENT', 'DRAFT', 'TRASH', 'SPAM', 'UNREAD', 'STARRED')]
        self.labels.append({'id': 'Label_1', 'name': 'Project', 'type': 'user'})
        self.messages = {
            'a': message('a', ['INBOX', 'UNREAD', 'Label_1'], 't1'),
            'b': message('b', ['SENT'], 't1'),
            'omitted': message('omitted', ['INBOX']),
            'concurrent': message('concurrent', ['SENT'], 't-concurrent')}
        self.raw_fetches = {}
        self.gets = []
        self.modifications = []
        self.send_attempts = []
        self.label_creates = 0
        self.profile_calls = 0

    def stage(self, phase):
        self.phase = phase
        if phase == 'partial':
            self.history = '200'
            self.messages['a']['labelIds'] = ['INBOX', 'STARRED', 'Label_1']
            del self.messages['b']
            self.messages['c'] = message('c', ['INBOX', 'UNREAD'], sender='news@example.test')
        elif phase in ('local', 'label-missing', 'label-restored'):
            self.history = '300'
        elif phase in ('broken-full', 'repeated-full', 'complete-full'):
            self.history = '500'
        elif phase in ('broken-history', 'complete-history'):
            self.history = '600'
            self.messages['a']['labelIds'] = ['INBOX', 'UNREAD', 'Label_1']
        elif phase == 'sent':
            self.history = '700'


class Handler(GoogleHandler):
    def route(self):
        state = self.server.state
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        length = int(self.headers.get('Content-Length', 0))
        assert 0 <= length <= 48 * 1024 * 1024
        body = self.rfile.read(length)
        if url.path == '/fixture/stage':
            state.stage(json.loads(body)['phase'])
            self.respond(200, {})
            return
        if url.path == '/fixture/stats':
            self.respond(200, {'raw_fetches': state.raw_fetches, 'gets': state.gets,
                              'modifications': state.modifications, 'send_attempts': state.send_attempts,
                              'labels': {name: value['labelIds'] for name, value in state.messages.items()},
                              'label_creates': state.label_creates})
            return
        assert self.headers.get('Authorization') == 'Bearer fixture-access'
        assert url.path.startswith('/gmail/')
        path = url.path[len('/gmail'):]
        if path == '/profile':
            state.profile_calls += 1
            self.respond(200, {'emailAddress': 'wrong@example.test' if state.phase == 'wrong-account' else 'user@example.test',
                              'historyId': state.history})
        elif path == '/drafts' and self.command == 'GET':
            self.respond(200, {'drafts': []})
        elif path == '/labels' and self.command == 'GET':
            self.respond(200, {'labels': state.labels})
        elif path == '/labels':
            request = json.loads(body)
            assert request['name'] == 'Rule'
            state.label_creates += 1
            assert state.label_creates == 1
            state.labels.append({'id': 'Label_rule', 'name': 'Rule', 'type': 'user'})
            # Simulate another client winning the creation race. Re-listing is
            # necessary before the pending tag can be acknowledged.
            self.respond(409, {'error': 'already exists'})
        elif path == '/messages':
            assert query['includeSpamTrash'] == ['true'] and query['maxResults'] == ['500']
            if 'pageToken' not in query:
                self.respond(200, {'messages': [{'id': 'a'}], 'nextPageToken': 'page2'})
            elif state.phase == 'broken-full':
                self.respond(503, {'error': 'temporary list failure'})
            elif state.phase == 'repeated-full':
                self.respond(200, {'messages': [{'id': 'a'}], 'nextPageToken': 'page2'})
            else:
                assert query['pageToken'] == ['page2']
                self.respond(200, {'messages': [{'id': 'b' if state.phase == 'full' else 'c'}]})
        elif path == '/history':
            assert query['maxResults'] == ['500']
            if state.phase in ('broken-full', 'repeated-full', 'complete-full'):
                self.respond(404, {'error': 'expired history'})
            elif state.phase == 'partial':
                assert query['startHistoryId'] == ['100']
                if 'pageToken' not in query:
                    self.respond(200, {'historyId': '200', 'nextPageToken': 'history2', 'history': [
                        {'id': '101', 'labelsRemoved': [{'message': {'id': 'a'}, 'labelIds': ['UNREAD']}],
                         'messagesDeleted': [{'message': {'id': 'b'}}]}]})
                else:
                    assert query['pageToken'] == ['history2']
                    self.respond(200, {'historyId': '200', 'history': [{'id': '102', 'messagesAdded': [
                        {'message': {'id': 'c'}}, {'message': {'id': 'concurrent'}}]}]})
            elif state.phase in ('broken-history', 'complete-history'):
                assert query['startHistoryId'] == ['500']
                if 'pageToken' not in query:
                    self.respond(200, {'historyId': '600', 'nextPageToken': 'history2',
                                      'history': [{'id': '501', 'messages': [{'id': 'a'}]}]})
                elif state.phase == 'broken-history':
                    self.respond(503, {'error': 'temporary history failure'})
                else:
                    self.respond(200, {'historyId': '600', 'history': [{'id': '502', 'messages': [{'id': 'c'}]}]})
            elif state.phase == 'sent':
                self.respond(200, {'historyId': '700', 'history': [{'id': '601', 'messagesAdded': [{'message': {'id': 'sent1'}}]}]})
            else:
                self.respond(200, {'historyId': state.history})
        elif path == '/messages/send':
            request = json.loads(body)
            raw = base64.urlsafe_b64decode(request['raw'] + '=' * (-len(request['raw']) % 4))
            parsed = BytesParser(policy=email.policy.default).parsebytes(raw)
            subject = str(parsed['Subject'])
            state.send_attempts.append(subject)
            assert str(parsed['To']) == 'recipient@example.test'
            assert str(parsed['Bcc']) == 'hidden@example.test'
            if subject == 'Refused':
                self.respond(400, {'error': 'rejected'})
                return
            assert subject in ('Delivered', 'Lost')
            item = message('sent1' if subject == 'Delivered' else 'lost1', ['SENT'], 't1')
            item['raw'] = raw
            state.messages[item['id']] = item
            if subject == 'Lost':
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                self.close_connection = True
            else:
                assert request['threadId'] == 't1'
                self.respond(200, {'id': 'sent1', 'threadId': 't1'})
        elif path.startswith('/messages/'):
            pieces = path.strip('/').split('/')
            name = pieces[1]
            if name not in state.messages:
                self.respond(404, {'error': 'message deleted'})
                return
            item = state.messages[name]
            if len(pieces) == 2:
                state.gets.append(name)
                result = {key: value for key, value in item.items() if key != 'raw'}
                if query['format'] == ['raw']:
                    state.raw_fetches[name] = state.raw_fetches.get(name, 0) + 1
                    result['raw'] = base64.urlsafe_b64encode(item['raw']).decode().rstrip('=')
                else:
                    assert query['format'] == ['minimal']
                self.respond(200, result)
            elif pieces[2] == 'modify':
                request = json.loads(body)
                assert len(request['addLabelIds']) <= 100 and len(request['removeLabelIds']) <= 100
                if state.phase == 'label-missing':
                    self.respond(404, {'error': 'label not found, message still exists'})
                    return
                present = set(item['labelIds'])
                present.update(request['addLabelIds'])
                present.difference_update(request['removeLabelIds'])
                item['labelIds'] = sorted(present)
                state.modifications.append({'id': name, **request})
                self.respond(200, {'id': name, 'threadId': item['threadId'], 'labelIds': item['labelIds']})
            elif pieces[2] in ('trash', 'untrash'):
                present = set(item['labelIds'])
                if pieces[2] == 'trash':
                    present.add('TRASH')
                    present.discard('INBOX')
                else:
                    present.discard('TRASH')
                item['labelIds'] = sorted(present)
                self.respond(200, {'id': name, 'threadId': item['threadId'], 'labelIds': item['labelIds']})
            else:
                raise AssertionError(f'unexpected operation {path}')
        else:
            raise AssertionError(f'unexpected request {path}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable')
    args = parser.parse_args()
    state = State()
    with tempfile.TemporaryDirectory(prefix='mail-gmail-sync-') as temporary:
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
    assert result.returncode == 0, f'Gmail sync tests exited {result.returncode}'
    assert not state.errors, state.errors
    assert state.raw_fetches == {'a': 1, 'b': 1, 'c': 1}, state.raw_fetches
    assert state.send_attempts == ['Delivered', 'Lost', 'Refused'], state.send_attempts
    assert state.label_creates == 1 and len(state.modifications) == 9, state.modifications
    print('Independent Gmail integration fixture: full/history sync, mutations, selective downloads, sent deduplication and no uncertain resend verified')


if __name__ == '__main__':
    main()
