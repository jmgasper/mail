#!/usr/bin/env python3
"""Run the real preview renderer and companion in the owned Mail test VM."""
import http.server
import os
from pathlib import Path
import subprocess
import threading
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
PORT = os.environ.get('MAIL_VM_PORT', '2258')
KEY = os.environ.get('MAIL_VM_KEY', str(ROOT / '../summit/.vm/airos-x86_64/id_ed25519'))
KNOWN = os.environ.get('MAIL_VM_KNOWN_HOSTS', str(ROOT / '.cache/vm/known_hosts'))
BUILD = Path(os.environ.get('MAIL_BUILD', str(ROOT / 'build-haiku')))
OPTIONS = ['-i', KEY, '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes', '-o', f'UserKnownHostsFile={KNOWN}', '-o', 'ConnectTimeout=10']
SSH = ['ssh', *OPTIONS, '-p', PORT, 'user@127.0.0.1']
SCP = ['scp', '-O', *OPTIONS, '-P', PORT]
GUEST = '/boot/home/mail-check'
NATIVE = os.environ.get('MAIL_NATIVE_TEST', 'mail_native_preview')
if NATIVE not in ('mail_native_preview', 'mail_native_reader'):
    raise SystemExit('MAIL_NATIVE_TEST must be mail_native_preview or mail_native_reader')
CORE = 'mail_reader_tests' if NATIVE == 'mail_native_reader' else 'mail_preview_tests'
SCREEN = 'received-reader.png' if NATIVE == 'mail_native_reader' else 'message-preview.png'
SCREEN_ENV = 'MAIL_READER_SCREENSHOT' if NATIVE == 'mail_native_reader' else 'MAIL_PREVIEW_SCREENSHOT'
requests = []


class Probe(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        requests.append(self.path if self.command == 'GET' else self.command + ' ' + self.path)
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'network probe')

    def log_message(self, *_):
        pass

    do_POST = do_GET
    do_HEAD = do_GET
    do_OPTIONS = do_GET


def main():
    server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Probe)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with urllib.request.urlopen(f'http://127.0.0.1:{server.server_port}/control', timeout=5) as response:
            assert response.read() == b'network probe'
        assert requests == ['/control']
        requests.clear()
        subprocess.run([*SSH, f'mkdir -p {GUEST}'], check=True)
        for name in ['MailPreview', NATIVE, CORE]:
            subprocess.run([*SCP, str(BUILD / name), f'user@127.0.0.1:{GUEST}/{name}.new'], check=True)
        subprocess.run([*SCP, str(ROOT / 'tests/fixtures/preview.png'), f'user@127.0.0.1:{GUEST}/preview.png'], check=True)
        command = (f'cd {GUEST} && '
                   f'for program in MailPreview {NATIVE} {CORE}; do mv "$program.new" "$program" || exit 1; done; '
                   f'./{CORE} && '
                   f'{SCREEN_ENV}={GUEST}/{SCREEN} ./{NATIVE} preview.png '
                   f'http://10.0.2.2:{server.server_port} > {NATIVE}.log 2>&1; '
                   f'mail_test_status=$?; cat {NATIVE}.log; exit $mail_test_status')
        completed = subprocess.run([*SSH, command], timeout=150, check=False)
        assert not requests, f'Preview made network requests: {requests}'
        if completed.returncode:
            raise SystemExit(completed.returncode)
        print('Independent preview HTTP fixture: zero network requests', flush=True)
        subprocess.run([*SCP, f'user@127.0.0.1:{GUEST}/{SCREEN}', str(ROOT / '.cache/vm' / SCREEN)], check=True)
        subprocess.run([*SCP, f'user@127.0.0.1:{GUEST}/{NATIVE}.log', str(ROOT / '.cache/vm' / (NATIVE + '.log'))], check=True)
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)


if __name__ == '__main__':
    main()
