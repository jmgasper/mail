#!/usr/bin/env python3
import email.policy
from email.parser import BytesParser
from pathlib import Path
import subprocess
import sys
import tempfile
from html.parser import HTMLParser


class ImageOrder(HTMLParser):
    def __init__(self):
        super().__init__()
        self.parts = []
        self.images = []

    def handle_starttag(self, tag, attrs):
        if tag == 'img':
            attrs = dict(attrs)
            self.images.append(attrs)
            self.parts.append(('image', attrs['src']))

    def handle_data(self, data):
        self.parts.append(('text', data))

with tempfile.TemporaryDirectory(prefix='mail-compose-check-') as root:
    path = Path(root)/'outgoing.eml'
    subprocess.run([sys.argv[1], str(path)], check=True)
    raw = path.read_bytes()
    assert b'\n' not in raw.replace(b'\r\n', b''), 'bare LF'
    assert max(map(len, raw.split(b'\r\n'))) <= 998
    message = BytesParser(policy=email.policy.default).parsebytes(raw)
    assert not any(part.defects for part in message.walk())
    assert 'Bcc' not in message and b'hidden@example.test' not in raw
    assert str(message['Subject']).endswith('Café 🎉')
    assert len(message['To'].addresses) == 2
    assert message['To'].addresses[0].display_name == 'Doe, Jane'
    assert message.get_body(preferencelist=('plain',)).get_content().startswith('Hello café 🎉')
    html = message.get_body(preferencelist=('html',)).get_content()
    assert '<strong>' in html and '&lt;safe&gt;' in html and 'cid:inline@test' in html
    files = [p for p in message.walk() if p.get_filename()]
    assert len(files) == 2
    binary = next(p for p in files if p.get_content_type() == 'application/octet-stream')
    assert binary.get_filename() == 'report café 🎉 and a very long filename.bin'
    assert binary.get_payload(decode=True) == b'a\0b'
    assert any(p.get_content_type() == 'multipart/related' for p in message.walk())
    print('Independent Python MIME checks passed: addressing, Unicode, BCC privacy, rich HTML, related images, binary files, wire framing')
    raw = Path(str(path)+'.positioned.eml').read_bytes()
    assert b'\n' not in raw.replace(b'\r\n', b'')
    message = BytesParser(policy=email.policy.default).parsebytes(raw)
    assert not any(part.defects for part in message.walk())
    html = message.get_body(preferencelist=('html',)).get_content()
    order = ImageOrder()
    order.feed(html)
    assert order.parts == [('text', 'Before café 🎉 '), ('image', 'cid:inline@test'),
                           ('text', ' middle '), ('image', 'cid:second@test'),
                           ('image', 'cid:inline@test'), ('text', ' after <safe>'),
                           ('image', 'cid:legacy@test')], order.parts
    assert order.images[1]['alt'] == 'diagram & "café".png'
    assert message.get_body(preferencelist=('plain',)).get_content() == (
        'Before café 🎉 [Image: picture.png] middle [Image: diagram & "café".png]'
        '[Image: picture.png] after <safe>')
    images = [part for part in message.walk() if part.get_content_maintype() == 'image']
    assert len(images) == 3  # Two positions share one part; one legacy image.
    second = next(part for part in images if part['Content-ID'] == '<second@test>')
    assert second.get_payload(decode=True) == b'\0second\xff'
    assert second.get_filename() == 'diagram & "café".png'
    assert all(part.get_content_disposition() == 'inline' for part in images)
    print('Independent positioned-image checks passed: exact HTML order, reused CID, Unicode alt/filename, plain fallback, binary payload')
