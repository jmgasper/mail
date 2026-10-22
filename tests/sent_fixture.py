#!/usr/bin/env python3
"""Stateful TLS SMTP/IMAP Sent-copy fixture; never contacts external services."""
import base64
import email.policy
from email.parser import BytesParser
import re
import ssl
import subprocess
import sys
import tempfile
import threading
from pathlib import Path
from protocol_fixture import Handler, Server


class SentHandler(Handler):
    def authenticate(self, token):
        assert self.secure, 'credentials before TLS'
        parts = base64.b64decode(token).split(b'\0')
        self.authenticated = len(parts) == 3 and parts[2] == b'test-password'
        self.username = parts[-2].decode()
        return self.authenticated

    def auth(self, tag, command):
        fields = command.split()
        if len(fields) == 2:
            self.send('+ \r\n' if tag else '334 \r\n')
            fields.append(self.line())
        ok = self.authenticate(fields[2])
        self.send(f'{tag} {"OK" if ok else "NO"} authentication\r\n' if tag else ('235 authenticated\r\n' if ok else '535 denied\r\n'))

    def imap(self):
        self.send('* OK Sent fixture\r\n')
        mailbox = 'INBOX'
        while True:
            line = self.line()
            if not line:
                return
            tag, command = line.split(' ', 1)
            u = command.upper()
            if u == 'CAPABILITY':
                self.send(f'* CAPABILITY IMAP4rev1 UIDPLUS MOVE AUTH=PLAIN SASL-IR\r\n{tag} OK capability\r\n')
                continue
            if u.startswith('AUTHENTICATE PLAIN'):
                self.auth(tag, command)
                continue
            assert self.authenticated, 'command before authentication'
            state = self.server.state
            user = self.username
            boxes = state.boxes.setdefault(user, {'INBOX': {}, 'Sent': {}, 'Custom': {}, 'Archive': {}})
            if u.startswith('LIST '):
                for name in boxes:
                    flag = '\\Sent' if name == 'Sent' and user != 'missing' else ('\\Archive' if name == 'Archive' else '')
                    if user == 'ambiguous' and name == 'Custom':
                        flag = '\\Sent'
                    self.send(f'* LIST ({flag}) "/" "{name}"\r\n')
            elif u.startswith('SELECT '):
                mailbox = command[7:].strip('"')
                assert mailbox in boxes
                self.send(f'* {len(boxes[mailbox])} EXISTS\r\n* OK [UIDVALIDITY 41] stable\r\n')
                if user == 'sticky':
                    self.send('* OK [UIDNOTSTICKY] unstable\r\n')
            elif u.startswith('UID SEARCH HEADER MESSAGE-ID '):
                wanted = command.split(' ', 4)[4].strip('"')
                ids = [str(uid) for uid, (raw, _) in boxes[mailbox].items() if wanted.encode().lower() in raw.lower()]
                if user in ('no-uid', 'lost', 'bad-uid', 'native') and state.searches.get((user, wanted), 0) == 0:
                    # Delay visibility after APPEND so the journal must survive.
                    ids = []
                state.searches[(user, wanted)] = state.searches.get((user, wanted), 0) + 1
                if user == 'native':
                    ids = []
                if user == 'malformed-search':
                    self.send('* SEARCH invalid-uid\r\n')
                else:
                    self.send('* SEARCH'+(' '+' '.join(ids) if ids else '')+'\r\n')
            elif u.startswith('UID FETCH '):
                target = command.split()[2]
                for uid, (raw, flags) in list(boxes[mailbox].items()):
                    if target != '1:*' and target != str(uid):
                        continue
                    if 'BODY.PEEK[HEADER.FIELDS' in u:
                        parsed = BytesParser(policy=email.policy.default).parsebytes(raw)
                        data = ('Message-ID: '+str(parsed['Message-ID'])+'\r\n\r\n').encode()
                        self.send(f'* 1 FETCH (UID {uid} BODY[HEADER.FIELDS (MESSAGE-ID)] {{{len(data)}}}\r\n'.encode()+data+b')\r\n')
                    elif 'BODY.PEEK[]' in u:
                        # Lost receipt copies are hidden from the ordinary snapshot
                        # until the test's next check; SEARCH can still find them.
                        self.send(f'* 1 FETCH (UID {uid} BODY[] {{{len(raw)}}}\r\n'.encode()+raw+b')\r\n')
                    else:
                        if user in ('lost', 'bad-uid', 'no-uid', 'native') and target == '1:*':
                            continue
                        self.send(f'* 1 FETCH (UID {uid} FLAGS ({" ".join(sorted(flags))}))\r\n')
            elif u.startswith('UID STORE '):
                target = int(command.split()[2]); assert target in boxes[mailbox]
                flag = '\\Seen' if '\\SEEN' in u else '\\Flagged'
                flags = boxes[mailbox][target][1]
                (flags.add if '+FLAGS' in u else flags.discard)(flag)
            elif u.startswith('UID MOVE '):
                target = int(command.split()[2]); dest = command.split(' ', 3)[3].strip('"')
                uid = max(boxes[dest], default=0)+1
                boxes[dest][uid] = boxes[mailbox].pop(target)
                self.send(f'{tag} OK [COPYUID 41 {target} {uid}] moved\r\n')
                continue
            elif u.startswith('APPEND '):
                match = re.fullmatch(r'APPEND "([^"]+)" \(\\Seen\) "([0-9]{2}-[A-Za-z]{3}-[0-9]{4} [0-9:]{8} \+0000)" \{([0-9]+)\}', command)
                assert match, 'APPEND framing/date/flags'
                dest, date, size = match.groups()
                state.attempts[user] = state.attempts.get(user, 0)+1
                if user == 'reject-pre':
                    self.send(f'{tag} NO permission denied\r\n')
                    continue
                self.send('+ send literal\r\n')
                raw = self.rfile.read(int(size))
                assert self.rfile.read(2) == b'\r\n' and len(raw) == int(size), 'exact literal framing'
                state.uploads.append((user, raw))
                if user == 'reject-post':
                    self.send(f'{tag} NO quota\r\n')
                    continue
                uid = max(boxes[dest], default=0)+1
                boxes[dest][uid] = (raw, {'\\Seen'})
                parsed = BytesParser(policy=email.policy.default).parsebytes(raw)
                state.searches[(user, str(parsed['Message-ID']))] = 0
                if user in ('lost', 'native'):
                    return
                if user == 'bad-uid':
                    self.send(f'{tag} OK [APPENDUID 41 1:2] malformed\r\n')
                elif user == 'no-uid':
                    self.send(f'{tag} OK saved\r\n')
                else:
                    self.send(f'{tag} OK [APPENDUID 41 {uid}] saved\r\n')
                continue
            elif u == 'LOGOUT':
                self.send(f'* BYE closing\r\n{tag} OK done\r\n')
                return
            else:
                raise AssertionError('unexpected IMAP command: '+command)
            self.send(f'{tag} OK done\r\n')

    def smtp(self):
        self.send('220 Sent fixture\r\n'); recipients = []
        while True:
            command = self.line()
            if not command:
                return
            upper = command.upper()
            if upper.startswith(('EHLO ', 'HELO ')):
                self.send('250-localhost\r\n250 AUTH PLAIN\r\n')
            elif upper.startswith('AUTH PLAIN'):
                self.auth('', command)
            elif upper == 'QUIT':
                self.send('221 goodbye\r\n'); return
            elif not self.authenticated:
                self.send('530 authenticate\r\n')
            elif upper.startswith('MAIL FROM:'):
                recipients = []; self.send('250 sender\r\n')
            elif upper.startswith('RCPT TO:'):
                recipients.append(command[8:]); self.send('250 recipient\r\n')
            elif upper == 'DATA':
                self.send('354 data\r\n'); raw = b''
                while True:
                    line = self.rfile.readline()
                    if not line:
                        return
                    if line == b'.\r\n':
                        break
                    raw += line[1:] if line.startswith(b'..') else line
                state = self.server.state
                state.deliveries.append((self.username, recipients, raw))
                if self.username in ('auto', 'search'):
                    boxes = state.boxes.setdefault(self.username, {'INBOX': {}, 'Sent': {}, 'Custom': {}, 'Archive': {}})
                    if self.username == 'search':
                        parsed = BytesParser(policy=email.policy.default).parsebytes(raw)
                        wanted = str(parsed['Message-ID'])
                        # Substring match must not be adopted: place the actual ID
                        # only in an unrelated field of a different message.
                        raw = raw.replace(wanted.encode(), ('<different@example.test>\r\nX-Original: '+wanted).encode(), 1)
                    boxes['Sent'][1] = (raw, {'\\Seen'})
                self.send('250 delivered\r\n')
            else:
                raise AssertionError('unexpected SMTP command')


class State:
    def __init__(self):
        self.boxes, self.attempts, self.searches = {}, {}, {}
        self.uploads, self.deliveries, self.errors = [], [], []


def main():
    with tempfile.TemporaryDirectory(prefix='mail-sent-', dir=Path.cwd()) as temporary:
        root = Path(temporary); cert, key = root/'cert.pem', root/'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=localhost',
                        '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1,IP:10.0.2.2', '-keyout', str(key), '-out', str(cert)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); tls.minimum_version = ssl.TLSVersion.TLSv1_2; tls.load_cert_chain(cert, key)
        state = State(); servers = []
        for protocol in ('imap', 'smtp'):
            server = Server(('127.0.0.1', 0), SentHandler)
            server.protocol, server.implicit, server.tls, server.state = protocol, True, tls, state
            server.handle_error = lambda *_: None
            threading.Thread(target=server.serve_forever, daemon=True).start(); servers.append(server)
        imap, smtp = (str(s.server_address[1]) for s in servers)
        try:
            result = subprocess.run([sys.argv[1], '127.0.0.1', str(cert), imap, imap, imap, imap, smtp, smtp], timeout=180)
            assert result.returncode == 0, ('client failed', state.errors)
            assert not state.errors, state.errors
            for user, raw in state.uploads:
                delivery = next(data for who, _, data in state.deliveries if who == user)
                assert raw == delivery, 'Sent copy must exactly match delivered MIME'
                assert b'hidden@example.test' not in raw
            users = [who for who, _, _ in state.deliveries]
            assert len(users) == len(set(users)), 'copy recovery must never repeat SMTP delivery'
            if 'native' in users:
                assert users == ['native'] and state.attempts.get('native') == 2, 'native retry must upload exactly one extra copy'
            assert state.attempts.get('auto', 0) == 0, 'Bridge/server saves its own copy'
            assert state.attempts.get('lost', 0) <= 1 and state.attempts.get('no-uid', 0) <= 1
            print(f'Sent server checks passed: {len(state.deliveries)} deliveries, {len(state.uploads)} copy literals, exact MIME, private BCC, no SMTP resends')
        finally:
            for server in servers:
                server.shutdown(); server.server_close()


if __name__ == '__main__':
    main()
