#!/usr/bin/env python3
"""Exercise native Mail against real Bridge and a disposable local Proton API."""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
PORT = os.environ.get('MAIL_VM_PORT', '2258')
KEY = os.environ.get('MAIL_VM_KEY', str(ROOT / '../summit/.vm/airos-x86_64/id_ed25519'))
KNOWN = os.environ.get('MAIL_VM_KNOWN_HOSTS', str(ROOT / '.cache/vm/known_hosts'))
BUILD = Path(os.environ.get('MAIL_BUILD', str(ROOT / 'build-haiku')))
BRIDGE = Path(os.environ.get('MAIL_BRIDGE_BUILD', str(ROOT / '.cache/bridge')))
OPTIONS = ['-i', KEY, '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes', '-o', f'UserKnownHostsFile={KNOWN}', '-o', 'ConnectTimeout=10']
SSH = ['ssh', *OPTIONS, '-p', PORT, 'user@127.0.0.1']
SCP = ['scp', '-O', '-C', *OPTIONS, '-P', PORT]
GUEST = '/boot/home/mail-check'


def main():
    subprocess.run([*SSH, f'mkdir -p {GUEST}'], check=True)
    for path, name in [(BUILD / 'mail_bridge_tests', 'mail_bridge_tests'), (BRIDGE / 'bridge-mail.test', 'bridge-mail.test')]:
        subprocess.run([*SCP, str(path), f'user@127.0.0.1:{GUEST}/{name}.new'], check=True)
    command = (f'cd {GUEST} && mv mail_bridge_tests.new mail_bridge_tests && mv bridge-mail.test.new bridge-mail.test && '
               f'env -u FEATURE_TEST_HOST_URL -u BRIDGE_LOG_IMAP_CLIENT -u BRIDGE_LOG_IMAP_SERVER -u BRIDGE_LOG_SMTP '
               f'MAIL_BRIDGE_CLIENT={GUEST}/mail_bridge_tests ./bridge-mail.test -test.run "^TestMailInterop$" -test.v -test.timeout 5m '
               '> bridge-mail.log 2>&1; mail_test_status=$?; tail -70 bridge-mail.log; exit $mail_test_status')
    result = subprocess.run([*SSH, command], check=False, timeout=330)
    subprocess.run([*SCP, f'user@127.0.0.1:{GUEST}/bridge-mail.log', str(ROOT / '.cache/vm/bridge-mail.log')], check=True)
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
