#!/usr/bin/env python3
"""Stateful, disposable IMAP draft server; never contacts a mail provider."""
from datetime import datetime, timedelta, timezone
from email.parser import BytesParser
import email.policy
from pathlib import Path
import re
import ssl
import subprocess
import sys
import tempfile
import threading
from protocol_fixture import Server
from sent_fixture import SentHandler


def message(subject):
    return ('From: user@example.test\r\nTo: recipient@example.test\r\nSubject: '+subject+
            '\r\nMessage-ID: <original@example.test>\r\n\r\nOriginal body').encode()


class State:
    def __init__(self):
        self.boxes, self.next_uid, self.attempts, self.deletes = {}, {}, {}, {}
        self.uploads, self.expunges, self.errors = [], [], []

    def mailbox(self, user):
        if user not in self.boxes:
            self.boxes[user] = {'INBOX': {}, 'Drafts': {
                3: (message('Original draft'), {'\\Draft'}),
                4: (message('Another deleted draft'), {'\\Draft', '\\Deleted'}),
                5: (message('An ordinary message'), {'\\Seen'})}}
            self.next_uid[user] = 50
        return self.boxes[user]


class Handler(SentHandler):
    def imap(self):
        self.send('* OK Draft fixture\r\n')
        mailbox = 'INBOX'
        while True:
            line = self.line()
            if not line:
                return
            tag, command = line.split(' ', 1)
            u = command.upper()
            if u == 'CAPABILITY':
                user = self.username
                capabilities = 'IMAP4rev2' if user == 'rev2' else 'IMAP4rev1'+('' if user == 'legacy' else ' UIDPLUS')
                self.send(f'* CAPABILITY {capabilities} AUTH=PLAIN SASL-IR'+('' if self.secure else ' STARTTLS')+f'\r\n{tag} OK capability\r\n')
                continue
            if u == 'STARTTLS':
                self.send(f'{tag} OK begin TLS\r\n'); self.upgrade(); continue
            if u.startswith('AUTHENTICATE PLAIN'):
                self.auth(tag, command); continue
            assert self.authenticated and self.secure, 'unencrypted/unauthorized command'
            state, user = self.server.state, self.username
            boxes = state.mailbox(user)
            if u.startswith('LIST '):
                self.send('* LIST () "/" "INBOX"\r\n* LIST (\\Drafts) "/" "Drafts"\r\n')
            elif u.startswith('SELECT '):
                mailbox = command[7:].strip('"'); assert mailbox in boxes
                self.send(f'* {len(boxes[mailbox])} EXISTS\r\n* OK [UIDVALIDITY 41] stable\r\n')
                if user != 'missing-next':
                    next_uid = 0 if user == 'bad-next' else state.next_uid[user]
                    self.send(f'* OK [UIDNEXT {next_uid}] next\r\n')
            elif u.startswith('UID FETCH '):
                target = command.split()[2]
                for uid, (raw, flags) in list(boxes[mailbox].items()):
                    if target != '1:*' and target != str(uid):
                        continue
                    body = 'BODY.PEEK[]' in u
                    fields = f'UID {uid}'
                    if 'FLAGS' in u and not (body and user == 'missing-flags'):
                        fields += f' FLAGS ({" ".join(sorted(flags))})'
                    if body and user != 'missing-body':
                        self.send(f'* 1 FETCH ({fields} BODY[] {{{len(raw)}}}\r\n'.encode()+raw+b')\r\n')
                    else:
                        self.send(f'* 1 FETCH ({fields})\r\n')
            elif u.startswith('APPEND '):
                match = re.fullmatch(r'APPEND "Drafts" \(\\Seen \\Draft\) "[0-9]{2}-[A-Za-z]{3}-[0-9]{4} [0-9:]{8} \+0000" \{([0-9]+)\}', command)
                assert match, 'draft APPEND flags/date/framing'
                state.attempts[user] = state.attempts.get(user, 0)+1
                if user == 'reject-pre':
                    self.send(f'{tag} NO denied\r\n'); continue
                self.send('+ send literal\r\n')
                size = int(match[1]); raw = self.rfile.read(size)
                assert len(raw) == size and self.rfile.read(2) == b'\r\n', 'complete literal'
                parsed = BytesParser(policy=email.policy.default).parsebytes(raw)
                assert str(parsed['Subject']) == 'Saved draft 🎉' and str(parsed['Bcc']) == 'hidden@example.test'
                assert parsed.get_body(preferencelist=('plain',)).get_content().strip() == 'Body 🎉'
                assert '<strong>' in parsed.get_body(preferencelist=('html',)).get_content()
                assert next(parsed.iter_attachments()).get_payload(decode=True) == b'a\0z'
                state.uploads.append((user, raw))
                if user == 'reject-post':
                    self.send(f'{tag} NO quota\r\n'); continue
                uid = state.next_uid[user]; state.next_uid[user] += 1
                boxes['Drafts'][uid] = (raw, {'\\Seen', '\\Draft'})
                if user == 'lost':
                    return
                receipt = '' if user in ('legacy', 'no-uid') else f' [APPENDUID 41 {"1:2" if user == "bad-uid" else uid}]'
                self.send(f'{tag} OK{receipt} saved\r\n'); continue
            elif u.startswith('UID STORE '):
                match = re.fullmatch(r'UID STORE ([0-9]+) \+FLAGS.SILENT \(\\Deleted\)', command)
                assert match and int(match[1]) == 3, 'only the owned draft can be marked Deleted'
                state.deletes[user] = state.deletes.get(user, 0)+1
                if user != 'ignore-delete':
                    boxes[mailbox][3][1].add('\\Deleted')
                if user == 'lost-delete':
                    return
            elif u.startswith('UID EXPUNGE '):
                assert user != 'legacy' and command == 'UID EXPUNGE 3', 'never use broad or unsupported expunge'
                state.expunges.append(user)
                if user == 'expunge-denied':
                    self.send(f'{tag} NO denied\r\n'); continue
                if 3 in boxes[mailbox] and '\\Deleted' in boxes[mailbox][3][1]:
                    del boxes[mailbox][3]
                    self.send('* 1 EXPUNGE\r\n')
            elif u == 'LOGOUT':
                self.send(f'* BYE closing\r\n{tag} OK done\r\n'); return
            else:
                raise AssertionError('unexpected IMAP command: '+command)
            self.send(f'{tag} OK done\r\n')


def main():
    with tempfile.TemporaryDirectory(prefix='mail-imap-drafts-') as temporary:
        root = Path(temporary); cert, key, csr = root/'cert.pem', root/'key.pem', root/'request.pem'
        (root/'index').touch(); (root/'serial').write_text('01\n')
        config = root/'ca.conf'
        config.write_text('[ca]\ndefault_ca=fixture\n[fixture]\n'
                          f'database={root}/index\nnew_certs_dir={root}\nserial={root}/serial\n'
                          'default_md=sha256\npolicy=policy\nx509_extensions=extensions\n'
                          '[policy]\ncommonName=supplied\n[extensions]\n'
                          'subjectAltName=IP:127.0.0.1,IP:10.0.2.2\nbasicConstraints=critical,CA:TRUE\n')
        subprocess.run(['openssl','req','-new','-newkey','rsa:2048','-nodes','-subj','/CN=IMAP draft fixture','-keyout',str(key),'-out',str(csr)],check=True,capture_output=True)
        subprocess.run(['openssl','ca','-selfsign','-batch','-notext','-config',str(config),'-keyfile',str(key),'-in',str(csr),'-out',str(cert),'-days','1',
                        '-startdate',(datetime.now(timezone.utc)-timedelta(minutes=5)).strftime('%Y%m%d%H%M%SZ')],check=True,capture_output=True)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); tls.minimum_version = ssl.TLSVersion.TLSv1_2; tls.load_cert_chain(cert,key)
        state, servers = State(), []
        for implicit in (True,False):
            server = Server(('127.0.0.1',0),Handler)
            server.protocol, server.implicit, server.tls, server.state = 'imap',implicit,tls,state
            server.handle_error = lambda *_: None
            threading.Thread(target=server.serve_forever,daemon=True).start(); servers.append(server)
        ports = [str(s.server_address[1]) for s in servers]
        try:
            result = subprocess.run([sys.argv[1],'127.0.0.1',str(cert),*ports,ports[0],ports[0],ports[0],ports[0]],timeout=100)
            assert not state.errors, state.errors
            assert result.returncode == 0, result.returncode
            assert all(count == 1 for count in state.attempts.values()), 'no blind upload repeats'
            assert all(4 in boxes['Drafts'] and 5 in boxes['Drafts'] for boxes in state.boxes.values()), 'unrelated messages preserved'
            assert all(count == 1 for count in state.deletes.values()), 'cleanup reconnect verifies earlier deletion'
            print('IMAP draft fixture verified: TLS/STARTTLS, independent MIME parsing, exact upload counts, targeted cleanup and unrelated deleted messages preserved.')
        finally:
            for server in servers:
                server.shutdown(); server.server_close()


if __name__ == '__main__':
    main()
