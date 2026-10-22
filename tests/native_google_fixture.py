#!/usr/bin/env python3
"""Google browser/key-store/native-window fixture, isolated from real accounts."""
import argparse
import http.server
import ssl
import subprocess
import tempfile
import threading
import urllib.parse
from pathlib import Path
from google_fixture import Handler as OAuthHandler, State as OAuthState
from gmail_sync_fixture import Handler as GmailHandler, State as GmailState


class State(GmailState, OAuthState):
    def __init__(self):
        OAuthState.__init__(self)
        GmailState.__init__(self)
        self.seen_tokens = set()
        self.rejections = 0


class Handler(GmailHandler):
    def route(self):
        path = urllib.parse.urlsplit(self.path).path
        state = self.server.state
        if path in ('/authorize', '/token', '/revoke'):
            return OAuthHandler.route(self)
        if path == '/fixture/auth-stats':
            self.respond(200, {'authorizations': state.auth_count, 'refreshes': state.refresh_count,
                              'revocations': state.revocations, 'rejections': state.rejections})
            return
        if path.startswith('/gmail/'):
            token = self.headers.get('Authorization')
            state.seen_tokens.add(token)
            if token not in ('Bearer valid-access', 'Bearer refreshed-access'):
                state.rejections += 1
                self.respond(401, {'error': 'expired access token'})
                return
            self.headers.replace_header('Authorization', 'Bearer fixture-access')
        return super().route()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable')
    args = parser.parse_args()
    state = State()
    with tempfile.TemporaryDirectory(prefix='mail-native-google-') as temporary:
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
            result = subprocess.run([args.executable, '127.0.0.1', str(cert), str(server.server_port)], timeout=180)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
    assert result.returncode == 0, f'Native Google UI test exited {result.returncode}'
    assert not state.errors, state.errors
    assert (state.auth_count, state.refresh_count, state.revocations) == (5, 2, 1), (state.auth_count, state.refresh_count, state.revocations)
    assert state.rejections >= 1
    assert state.send_attempts == ['Delivered']
    assert state.raw_fetches == {'a': 1, 'b': 1}, state.raw_fetches
    print('Native Google fixture: browser PKCE, coordinated key-store refresh, identity checks, background receive, reply/send, deduplication and revocation verified')


if __name__ == '__main__':
    main()
