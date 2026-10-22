#!/usr/bin/env python3
"""Exercise Mail attachment UI and a physical drag in the disposable QEMU guest.

Uses only the existing mail VM; never boots, restarts, or modifies other VMs.
The native test supplies measured widget coordinates and verifies the exported
file on Tracker's desktop, then removes that disposable output.
"""
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


def drag(points):
    width, height = points['width'], points['height']
    assert 300 <= width <= 8192 and 300 <= height <= 8192
    assert all(0 <= points[name] < bound for name, bound in [('x', width), ('y', height), ('target_x', width), ('target_y', height)])
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

            def move(x, y):
                qmp('input-send-event', {'events': [
                    {'type': 'abs', 'data': {'axis': 'x', 'value': round(x * 32767 / (width - 1))}},
                    {'type': 'abs', 'data': {'axis': 'y', 'value': round(y * 32767 / (height - 1))}},
                ]})

            def button(down):
                qmp('input-send-event', {'events': [{'type': 'btn', 'data': {'down': down, 'button': 'left'}}]})

            qmp('qmp_capabilities')
            move(points['x'], points['y'])
            time.sleep(.15)
            button(True)
            try:
                time.sleep(.15)
                for step in range(1, 17):
                    fraction = step / 16
                    move(points['x'] + (points['target_x'] - points['x']) * fraction,
                         points['y'] + (points['target_y'] - points['y']) * fraction)
                    time.sleep(.06)
                time.sleep(.2)
            finally:
                button(False)


def main():
    subprocess.run([*SSH, f'mkdir -p {GUEST} && rm -f {GUEST}/attachment-drag.json'], check=True)
    subprocess.run([*SCP, str(BUILD / 'mail_native_attachments'), f'user@127.0.0.1:{GUEST}/mail_native_attachments.new'], check=True)
    subprocess.run([*SCP, str(ROOT / 'tests/fixtures/preview.png'), f'user@127.0.0.1:{GUEST}/preview.png'], check=True)
    command = (f'cd {GUEST} && mv mail_native_attachments.new mail_native_attachments && '
               f'MAIL_ATTACHMENT_DRAG={GUEST}/attachment-drag.json MAIL_ATTACHMENT_SCREENSHOT={GUEST}/attachments.png '
               './mail_native_attachments preview.png > mail_native_attachments.log 2>&1; '
               'mail_test_status=$?; cat mail_native_attachments.log; exit $mail_test_status')
    process = subprocess.Popen([*SSH, command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    deadline, dragged = time.monotonic() + 135, False
    try:
        while process.poll() is None:
            if time.monotonic() > deadline:
                raise TimeoutError('Native attachment test did not finish; inspect mail_native_attachments.log in the guest.')
            if not dragged:
                coordinates = subprocess.run([*SSH, f'cat {GUEST}/attachment-drag.json 2>/dev/null'], capture_output=True, text=True)
                if coordinates.returncode == 0 and coordinates.stdout.strip():
                    try:
                        points = json.loads(coordinates.stdout)
                    except json.JSONDecodeError:
                        continue  # The test may still be writing this small file.
                    drag(points)
                    dragged = True
            time.sleep(.3)
        output, _ = process.communicate()
        print(output, end='')
        if process.returncode:
            raise SystemExit(process.returncode)
        assert dragged, 'Native test must include a real pointer drag to Tracker.'
        subprocess.run([*SCP, f'user@127.0.0.1:{GUEST}/attachments.png', str(ROOT / '.cache/vm/attachments.png')], check=True)
    finally:
        if process.poll() is None:
            subprocess.run([*SSH, 'hey application/x-vnd.air-Mail-Attachments-Test quit'], timeout=10, check=False)
            try:
                output, _ = process.communicate(timeout=10)
                print(output, end='')
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=10)


if __name__ == '__main__':
    main()
