#!/usr/bin/env python3
"""Real TCP/TLS protocol fixtures. Credentials and messages are disposable.

Runs a C++ executable against independently framed IMAP/POP3/SMTP servers.
The servers reject credentials before STARTTLS, retain POP3 mail, and inspect
SMTP recipients and DATA. No external accounts or services are contacted.
"""
import argparse
import base64
import json
import re
import socketserver
import ssl
import subprocess
import tempfile
import email.policy
from email.parser import BytesParser
import threading
from pathlib import Path

RAW = (b'From: sender@example.test\r\nTo: user@example.test\r\nSubject: Remote message\r\n'
       b'Message-ID: <remote-one@example.test>\r\nDate: Wed, 21 Oct 2026 11:30:00 +1100\r\n\r\n'
       b'M6 OK forged completion\r\n* 999 FETCH (UID 999)\r\n.leading dot\r\nEnd of literal\r\n')


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.folders = {'INBOX': {1: set(), 2: {'\\Seen', '\\Flagged'}}, 'Archive': {}}
        self.sent = []
        self.errors = []
        self.events = []
        self.downloads = 0
        self.retry_recipients = 0
        self.drop_once = 0


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class Handler(socketserver.StreamRequestHandler):
    def setup(self):
        self.secure = self.server.implicit
        if self.secure:
            self.request = self.server.tls.wrap_socket(self.request, server_side=True)
        self.request.settimeout(35)
        super().setup()
        self.authenticated = False
        self.username = ''

    def send(self, text):
        self.wfile.write(text.encode() if isinstance(text, str) else text)
        self.wfile.flush()

    def line(self):
        data = self.rfile.readline(65537)
        if len(data) > 65536:
            raise RuntimeError('oversized client command')
        return data.decode().rstrip('\r\n')

    def upgrade(self):
        self.wfile.flush()
        self.request = self.server.tls.wrap_socket(self.request, server_side=True)
        self.rfile = self.request.makefile('rb')
        self.wfile = self.request.makefile('wb')
        self.secure = True

    def authenticate(self, token):
        if not self.secure:
            self.server.state.errors.append('credentials sent before encryption')
            return False
        try:
            parts = base64.b64decode(token).split(b'\0')
            self.authenticated = parts[-2:] == [b'user', b'test-password']
        except Exception:
            self.authenticated = False
        return self.authenticated

    def handle(self):
        try:
            getattr(self, self.server.protocol)()
        except (ConnectionError, ssl.SSLError, TimeoutError, OSError):
            pass
        except Exception as error:
            self.server.state.errors.append(str(error))

    def imap(self):
        self.send('* OK Mail fixture\r\n')
        mailbox = None
        while True:
            line = self.line()
            if not line:
                return
            tag, command = line.split(' ', 1)
            upper = command.upper()
            self.server.state.events.append(('imap', command.split(' ', 1)[0]))
            if upper == 'CAPABILITY':
                auth = 'AUTH=PLAIN SASL-IR' if self.secure else 'STARTTLS LOGINDISABLED'
                self.send(f'* CAPABILITY IMAP4rev1 MOVE UIDPLUS {auth}\r\n{tag} OK capability\r\n')
            elif upper == 'STARTTLS':
                self.send(f'{tag} OK upgrade\r\n')
                self.upgrade()
            elif upper.startswith('AUTHENTICATE PLAIN'):
                fields = command.split()
                if len(fields) == 2:
                    self.send('+ \r\n')
                    fields.append(self.line())
                ok = self.authenticate(fields[2])
                self.send(f'{tag} {"OK" if ok else "NO"} authentication\r\n')
            elif upper.startswith('LOGIN '):
                args = re.findall(r'"((?:[^"\\]|\\.)*)"|([^ ]+)', command[6:])
                values = [a or b for a, b in args]
                ok = self.authenticate(base64.b64encode(('\0'+'\0'.join(values)).encode()).decode())
                self.send(f'{tag} {"OK" if ok else "NO"} authentication\r\n')
            elif not self.authenticated:
                self.send(f'{tag} NO authenticate first\r\n')
            elif upper.startswith('LIST '):
                self.send('* LIST (\\HasNoChildren) "/" "INBOX"\r\n'
                          '* LIST (\\Archive) "/" "Archive"\r\n'
                          '* LIST (\\HasNoChildren) "/" "Envoy&AOk-"\r\n'
                          '* LIST (\\Noselect) "/" "Parent"\r\n'
                          '* LIST () "/" {11}\r\nLong folder\r\n'
                          f'{tag} OK list\r\n')
            elif upper.startswith('SELECT '):
                mailbox = command[7:].strip('"')
                with self.server.state.lock:
                    count = len(self.server.state.folders.get(mailbox, {}))
                epoch = 77 if mailbox == 'Archive' else 41
                self.send(f'* {count} EXISTS\r\n* OK [UIDVALIDITY {epoch}] stable\r\n{tag} OK selected\r\n')
            elif upper.startswith('UID FETCH '):
                requested, fields = command[10:].split(' ', 1)
                with self.server.state.lock:
                    items = dict(self.server.state.folders.get(mailbox, {}))
                selected = items if requested == '1:*' else {int(requested): items[int(requested)]} if int(requested) in items else {}
                for index, (uid, flags) in enumerate(selected.items(), 1):
                    if 'BODY' in fields.upper():
                        if 'BODY.PEEK[]' not in fields.upper():
                            self.server.state.errors.append('download marked a message read')
                        self.server.state.downloads += 1
                        self.send(f'* {index} FETCH (UID {uid} BODY[] {{{len(RAW)}}}\r\n'.encode() + RAW + b')\r\n')
                    else:
                        self.send(f'* {index} FETCH (FLAGS ({" ".join(sorted(flags))}) UID {uid})\r\n')
                self.send(f'{tag} OK fetched\r\n')
            elif upper.startswith('UID STORE '):
                match = re.fullmatch(r'UID STORE (\d+) ([+-])FLAGS\.SILENT \((\\\w+)\)', command)
                if not match:
                    raise RuntimeError('unexpected STORE form')
                uid, operation, flag = match.groups()
                with self.server.state.lock:
                    flags = self.server.state.folders[mailbox][int(uid)]
                    flags.add(flag) if operation == '+' else flags.discard(flag)
                self.send(f'{tag} OK stored\r\n')
            elif upper.startswith('UID MOVE '):
                match = re.fullmatch(r'UID MOVE (\d+) "([^"]+)"', command)
                uid, target = match.groups()
                with self.server.state.lock:
                    self.server.state.folders[target][5] = self.server.state.folders[mailbox].pop(int(uid))
                self.send(f'* OK [COPYUID 77 {uid} 5] moved\r\n{tag} OK moved\r\n')
            elif upper == 'LOGOUT':
                self.send(f'* BYE goodbye\r\n{tag} OK logout\r\n')
                return
            else:
                self.server.state.errors.append('unexpected IMAP command: '+command.split(' ')[0])
                self.send(f'{tag} BAD unsupported\r\n')

    def pop3(self):
        self.send('+OK POP3 fixture\r\n')
        while True:
            command = self.line()
            if not command:
                return
            verb, _, argument = command.partition(' ')
            verb = verb.upper()
            if verb == 'CAPA':
                self.send('+OK capabilities\r\nUSER\r\nUIDL\r\n'+('' if self.secure else 'STLS\r\n')+'.\r\n')
            elif verb == 'STLS':
                self.send('+OK upgrade\r\n')
                self.upgrade()
            elif verb == 'USER':
                if not self.secure:
                    self.server.state.errors.append('POP3 credentials before TLS')
                self.username = argument
                self.send('+OK user\r\n')
            elif verb == 'PASS':
                self.authenticated = self.secure and self.username == 'user' and argument == 'test-password'
                self.send('+OK authenticated\r\n' if self.authenticated else '-ERR bad credentials\r\n')
            elif verb == 'QUIT':
                self.send('+OK goodbye\r\n')
                return
            elif not self.authenticated:
                self.send('-ERR authenticate first\r\n')
            elif verb == 'UIDL':
                self.send(f'+OK {argument} stable-{"one" if argument == "1" else "two"}\r\n' if argument else '+OK UIDLs\r\n1 stable-one\r\n2 stable-two\r\n.\r\n')
            elif verb == 'RETR':
                self.send(b'+OK message\r\n'+RAW.replace(b'\r\n.', b'\r\n..')+b'.\r\n')
            else:
                self.server.state.errors.append('unexpected POP3 command: '+verb)
                self.send('-ERR unsupported\r\n')

    def smtp(self):
        self.send('220 fixture ESMTP\r\n')
        recipients = []
        while True:
            command = self.line()
            if not command:
                return
            upper = command.upper()
            if upper.startswith('EHLO '):
                self.send('250-fixture\r\n'+('250-AUTH PLAIN LOGIN\r\n' if self.secure else '250-STARTTLS\r\n')+'250 SIZE 33554432\r\n')
            elif upper == 'STARTTLS':
                self.send('220 upgrade\r\n')
                self.upgrade()
            elif upper.startswith('AUTH PLAIN'):
                token = command[11:].strip()
                if not token:
                    self.send('334 \r\n')
                    token = self.line()
                self.send('235 authenticated\r\n' if self.authenticate(token) else '535 rejected\r\n')
            elif upper == 'AUTH LOGIN':
                self.send('334 VXNlcm5hbWU6\r\n')
                user = base64.b64decode(self.line())
                self.send('334 UGFzc3dvcmQ6\r\n')
                password = base64.b64decode(self.line())
                self.authenticated = self.secure and user == b'user' and password == b'test-password'
                self.send('235 authenticated\r\n' if self.authenticated else '535 rejected\r\n')
            elif upper == 'QUIT':
                self.send('221 goodbye\r\n')
                return
            elif not self.authenticated:
                self.send('530 authenticate first\r\n')
            elif upper.startswith('MAIL FROM:'):
                recipients = []
                self.send('250 sender\r\n')
            elif upper.startswith('RCPT TO:'):
                reject = 'reject@' in command
                if 'retry@' in command:
                    with self.server.state.lock:
                        self.server.state.retry_recipients += 1
                        reject = self.server.state.retry_recipients == 1
                if reject:
                    self.send('550 no such recipient\r\n')
                else:
                    recipients.append(command[8:])
                    self.send('250 recipient\r\n')
            elif upper == 'DATA':
                self.send('354 send data\r\n')
                data = b''
                while True:
                    line = self.rfile.readline()
                    if line == b'.\r\n':
                        break
                    if not line:
                        return
                    data += line[1:] if line.startswith(b'..') else line
                self.server.state.sent.append((recipients[:], data))
                if any('drop-once@' in recipient for recipient in recipients):
                    with self.server.state.lock:
                        self.server.state.drop_once += 1
                        if self.server.state.drop_once == 1:
                            return
                if any('drop@' in recipient for recipient in recipients):
                    return  # Accepted DATA, connection lost before acknowledgement.
                self.send('250 queued\r\n')
            elif upper == 'RSET':
                recipients = []
                self.send('250 reset\r\n')
            else:
                self.server.state.errors.append('unexpected SMTP command: '+command.split(' ')[0])
                self.send('500 unsupported\r\n')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable', nargs='?')
    parser.add_argument('--serve', action='store_true', help='Keep fixtures running for native tests; print connection JSON')
    parser.add_argument('--accounts', action='store_true', help='Run the native account UI test; allow time for the guest key-store prompt')
    parser.add_argument('--outbox', action='store_true', help='Run native delivery recovery with controlled rejection/lost acknowledgements')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mail-protocol-', dir=Path.cwd()) as temporary:
        root = Path(temporary)
        certificate, key = root/'cert.pem', root/'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2', '-subj', '/CN=localhost',
                        '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1,IP:10.0.2.2',
                        '-keyout', str(key), '-out', str(certificate)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(certificate, key)
        state = State()
        servers = []
        for protocol, implicit in [('imap', True), ('imap', False), ('pop3', True), ('pop3', False), ('smtp', True), ('smtp', False)]:
            server = Server(('127.0.0.1', 0), Handler)
            server.protocol, server.implicit, server.tls, server.state = protocol, implicit, context, state
            # A client rejecting our self-signed cert is an expected negative test.
            server.handle_error = lambda *_: None
            threading.Thread(target=server.serve_forever, daemon=True).start()
            servers.append(server)
        ports = [str(server.server_address[1]) for server in servers]
        try:
            if args.serve:
                print(json.dumps({'certificate': str(certificate), 'ports': ports}), flush=True)
                threading.Event().wait()
            else:
                result = subprocess.run([args.executable, '127.0.0.1', str(certificate), *ports], timeout=180 if args.accounts or args.outbox else 60)
                if result.returncode:
                    print('Fixture events:', state.events, 'errors:', state.errors, flush=True)
                    raise SystemExit(result.returncode)
                assert not state.errors, state.errors
                if args.outbox:
                    assert state.retry_recipients == 2, state.retry_recipients
                    assert state.drop_once == 2, state.drop_once
                    assert len(state.sent) == 4, len(state.sent)
                    subjects = [str(BytesParser(policy=email.policy.default).parsebytes(raw)['Subject']) for _, raw in state.sent]
                    assert subjects == ['Uncertain retry', 'Uncertain draft', 'Retry rejected delivery', 'Uncertain retry'], subjects
                    assert state.sent[0] == state.sent[3], 'Explicit retry must retain the immutable MIME and envelope'
                    for recipients, raw in state.sent:
                        parsed = BytesParser(policy=email.policy.default).parsebytes(raw)
                        assert len(recipients) == 2 and b'hidden@example.test' not in raw
                        assert 'Recovery body 🎉' in parsed.get_body(preferencelist=('plain',)).get_content()
                        assert '<strong>' in parsed.get_body(preferencelist=('html',)).get_content()
                        assert next(parsed.iter_attachments()).get_payload(decode=True) == b'Original attachment bytes'
                    print('Outbox server checks passed: exactly four DATA submissions, explicit retries only, immutable MIME, private BCC and preserved attachments')
                elif args.accounts:
                    assert len(state.sent) == 1
                    recipients, raw = state.sent[0]
                    parsed = BytesParser(policy=email.policy.default).parsebytes(raw)
                    assert len(recipients) == 2 and b'hidden@example.test' not in raw
                    assert parsed['Subject'] == 'Native compose café'
                    assert '🎉' in parsed.get_body(preferencelist=('plain',)).get_content()
                    html = parsed.get_body(preferencelist=('html',)).get_content()
                    assert '<strong>Hello</strong>' in html
                    assert html.index('Hello') < html.index('<img ') < html.index('from the native composer.')
                    image = next(part for part in parsed.walk() if part.get_content_type() == 'image/png')
                    assert 'cid:'+str(image['Content-ID']).strip('<>') in html
                    assert len([part for part in parsed.walk() if part.get_filename()]) == 2
                    assert state.downloads == 2, state.downloads
                    print('Account/composer server checks passed: background PEEK receive and one rich SMTP delivery with private BCC and attachments')
                else:
                    assert len(state.sent) == 4, len(state.sent)
                    recipients, data = state.sent[0]
                    assert len(recipients) == 2 and b'\r\n.leading dot\r\nGoodbye\r\n' in data
                    print('Server checks passed: encrypted authentication, PEEK, retained POP3 mail, SMTP recipients and dot transparency')
        finally:
            for server in servers:
                server.shutdown()
                server.server_close()


if __name__ == '__main__':
    main()
