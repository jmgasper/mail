#!/usr/bin/env python3
"""Copy Haiku's vector icons (MIT licensed) out of Haiku .rdef sources.

    python3 tools/extract-haiku-icons.py /path/to/haiku

Writes resources/Mail.hvif (the application icon) and resources/icons/<name>.hvif. The generated files are checked in, so a
Haiku source tree is only needed to refresh them.
"""
import re
import sys
from pathlib import Path

# name: (rdef relative to the Haiku tree, resource selector regexp)
ICONS = {
    'inbox': ('src/kits/tracker/TrackerIcons.rdef', r'resource\(R_MailDirIcon\)'),
    'folder': ('src/data/mime_db/application/x-vnd.be-directory', r'resource\(5, "META:ICON"\)'),
    'message': ('src/data/mime_db/text/x-email', r'resource\(5, "META:ICON"\)'),
    'trash': ('src/kits/tracker/TrackerIcons.rdef', r'resource\(R_TrashIcon\)'),
    'query': ('src/kits/tracker/TrackerIcons.rdef', r'resource\(2, "BEOS:L:application/x-vnd.Be-query"\)'),
    'computer': ('src/kits/tracker/TrackerIcons.rdef', r'resource\(R_BootVolumeIcon\)'),
    'file': ('src/kits/tracker/TrackerIcons.rdef', r'resource\(R_FileIcon\)'),
    'send': ('src/apps/mail/pictures.rdef', r'resource\(1, "Send"\)'),
    'reply': ('src/apps/mail/pictures.rdef', r'resource\(8, "Reply"\)'),
    'forward': ('src/apps/mail/pictures.rdef', r'resource\(9, "Forward"\)'),
    'junk': ('src/apps/mail/pictures.rdef', r'resource\(10, "Junk"\)'),
    'new': ('src/apps/mail/pictures.rdef', r'resource\(11, "New Message"\)'),
    'unread': ('src/apps/mail/pictures.rdef', r'resource\(12, "Unread"\)'),
    'read': ('src/apps/mail/pictures.rdef', r'resource\(13, "Read"\)'),
    'delete': ('src/apps/mail/pictures.rdef', r'resource\(4, "Trash"\)'),
    'draft': ('src/apps/mail/Mail.rdef', r'resource\(0, "BEOS:L:text/x-vnd.be-maildraft"\)'),
    'star': ('src/apps/haikudepot/HaikuDepot.rdef', r'resource\(510, "starblue"\)'),
    'star-off': ('src/apps/haikudepot/HaikuDepot.rdef', r'resource\(520, "stargray"\)'),
    'snooze': ('src/apps/clock/Clock.rdef', r'resource vector_icon'),
    'archive': ('src/apps/expander/Expander.rdef', r'resource vector_icon'),
    'account': ('src/preferences/mail/e-mail.rdef', r'resource vector_icon'),
    'check': ('src/servers/mail/DeskbarViewIcons.rdef', r'resource\(kIconNewMail\)'),
    'mailboxes': ('src/servers/mail/DeskbarViewIcons.rdef', r'resource\(kIconNoMail\)'),
}


def extract(text, selector):
    match = re.search(selector + r'[^{]*\{(.*?)\};', text, re.S)
    if not match:
        raise SystemExit(f'missing {selector}')
    data = bytes.fromhex(''.join(re.findall(r'\$"([0-9A-Fa-f]*)"', match.group(1))))
    if not data.startswith(b'ncif'):
        raise SystemExit(f'{selector} is not HVIF')
    return data


# The application icon is Haiku's own Mail icon: this Mail replaces that one.
APP_ICON = ('src/apps/mail/Mail.rdef', r'resource vector_icon')


def main():
    haiku = Path(sys.argv[1] if len(sys.argv) > 1 else '/mnt/HaikuWork/src/haiku')
    root = Path(__file__).resolve().parents[1]
    rdef, selector = APP_ICON
    data = extract((haiku / rdef).read_text(errors='replace'), selector)
    (root / 'resources/Mail.hvif').write_bytes(data)
    print(f'Mail.hvif (application icon): {len(data)} bytes from {rdef}')
    out = root / 'resources/icons'
    out.mkdir(exist_ok=True)
    for name, (rdef, selector) in ICONS.items():
        data = extract((haiku / rdef).read_text(errors='replace'), selector)
        (out / f'{name}.hvif').write_bytes(data)
        print(f'{name}: {len(data)} bytes from {rdef}')


if __name__ == '__main__':
    main()
