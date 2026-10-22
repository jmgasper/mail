#!/usr/bin/env python3
"""Run snooze restart/UI checks and click the real native notification in QEMU."""
import json
import os
from pathlib import Path
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
PORT = os.environ.get('MAIL_VM_PORT', '2258')
KEY = os.environ.get('MAIL_VM_KEY', str(ROOT / '../summit/.vm/airos-x86_64/id_ed25519'))
KNOWN = os.environ.get('MAIL_VM_KNOWN_HOSTS', str(ROOT / '.cache/vm/known_hosts'))
BUILD = Path(os.environ.get('MAIL_BUILD', str(ROOT / 'build-haiku')))
OPTIONS = ['-i', KEY, '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes', '-o', f'UserKnownHostsFile={KNOWN}', '-o', 'ConnectTimeout=10']
SSH = ['ssh', *OPTIONS, '-p', PORT, 'user@127.0.0.1']
SCP = ['scp', '-O', *OPTIONS, '-P', PORT]
GUEST = '/boot/home/mail-check'


def click(points):
    width, height = points['width'], points['height']
    assert 300 <= width <= 8192 and 300 <= height <= 8192
    assert 0 <= points['x'] < width and 0 <= points['y'] < height
    with socket.socket(socket.AF_UNIX) as connection:
        connection.connect(os.environ.get('MAIL_VM_QMP', str(ROOT / '.cache/vm/qmp.sock')))
        with connection.makefile('rwb', buffering=0) as stream:
            json.loads(stream.readline())

            def qmp(command, arguments=None):
                stream.write((json.dumps({'execute': command, **({'arguments': arguments} if arguments else {})}) + '\n').encode())
                while True:
                    response = json.loads(stream.readline())
                    if 'error' in response:
                        raise RuntimeError(response)
                    if 'return' in response:
                        return response['return']

            qmp('qmp_capabilities')
            qmp('input-send-event', {'events': [
                {'type': 'abs', 'data': {'axis': 'x', 'value': round(points['x'] * 32767 / (width - 1))}},
                {'type': 'abs', 'data': {'axis': 'y', 'value': round(points['y'] * 32767 / (height - 1))}},
            ]})
            time.sleep(.1)
            qmp('input-send-event', {'events': [{'type': 'btn', 'data': {'down': True, 'button': 'left'}}]})
            time.sleep(.1)
            qmp('input-send-event', {'events': [{'type': 'btn', 'data': {'down': False, 'button': 'left'}}]})


def main():
    subprocess.run([*SSH, f'mkdir -p {GUEST} && rm -f {GUEST}/snooze-click.json'], check=True)
    subprocess.run([*SCP, str(BUILD / 'mail_native_snooze'), f'user@127.0.0.1:{GUEST}/mail_native_snooze.new'], check=True)
    command = (f'cd {GUEST} && mv mail_native_snooze.new mail_native_snooze && '
               'mail_snooze_dir=$(mktemp -d /tmp/mail-snooze-ui-XXXXXX) && '
               './mail_native_snooze --seed "$mail_snooze_dir/mail.db" && sleep 3 && '
               './mail_native_snooze --run "$mail_snooze_dir/mail.db" > mail_native_snooze.log 2>&1; '
               'mail_test_status=$?; cat mail_native_snooze.log; '
               'if test "$mail_test_status" = 0; then rm -f "$mail_snooze_dir/mail.db" "$mail_snooze_dir/mail.db-wal" "$mail_snooze_dir/mail.db-shm"; rmdir "$mail_snooze_dir"; fi; '
               'exit "$mail_test_status"')
    process = subprocess.Popen([*SSH, command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    deadline, clicked = time.monotonic() + 135, False
    try:
        while process.poll() is None:
            if time.monotonic() > deadline:
                raise TimeoutError('Inspect mail_native_snooze.log in the guest for the last stage.')
            if not clicked:
                coordinates = subprocess.run([*SSH, f'cat {GUEST}/snooze-click.json 2>/dev/null'], capture_output=True, text=True)
                if coordinates.returncode == 0 and coordinates.stdout.strip():
                    try:
                        points = json.loads(coordinates.stdout)
                    except json.JSONDecodeError:
                        continue
                    click(points)
                    clicked = True
            time.sleep(.2)
        output, _ = process.communicate()
        print(output, end='')
        if process.returncode:
            raise SystemExit(process.returncode)
        assert clicked, 'Native test must exercise a real notification click.'
        for name in ('notification', 'reminders', 'custom', 'live'):
            subprocess.run([*SCP, f'user@127.0.0.1:{GUEST}/snooze-{name}.png', str(ROOT / f'.cache/vm/snooze-{name}.png')], check=True)
    finally:
        if process.poll() is None:
            subprocess.run([*SSH, 'hey application/x-vnd.air-Mail-Snooze-Test quit'], timeout=10, check=False)
            try:
                output, _ = process.communicate(timeout=10)
                print(output, end='')
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=10)


if __name__ == '__main__':
    main()
