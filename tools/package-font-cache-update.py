#!/usr/bin/env python3
"""Repackage the matching x86_64 1733 OS with the qualified font-cache fix.

Run with the isolated Haiku host tools from .cache/package-env.sh. This does
not install anything. Other OS versions need their own compatibility review.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile

OLD = 'r1~beta6_hrev60207_1733-1'
NEW = 'r1~beta6_hrev60207_1733-2'
SERVER_HASH = '6ca524b957548383c62c15293587e2fa6b323af30e3fd5b579d421e6640eca63'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(root):
    result = {}
    for path in root.rglob('*'):
        name = str(path.relative_to(root))
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode):
            result[name] = ['link', os.readlink(path), stat.S_IMODE(mode)]
        elif stat.S_ISREG(mode):
            result[name] = ['file', digest(path), stat.S_IMODE(mode)]
        elif stat.S_ISDIR(mode):
            result[name] = ['directory', stat.S_IMODE(mode)]
        else:
            raise RuntimeError('Unexpected file type: '+name)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    base, server, output = (p.resolve() for p in (args.base, args.server, args.output))
    if output.exists():
        raise RuntimeError('Output already exists; preserve qualified artifacts.')
    tool = os.environ['MAIL_PACKAGE_TOOL']

    def package(*command):
        return subprocess.check_output([tool, *map(str, command)], text=True)

    metadata = package('list', '-i', base)
    for field, value in [('name', 'haiku'), ('architecture', 'x86_64'), ('version', OLD)]:
        if not re.search(r'^\s*'+field+': '+re.escape(value)+r'$', metadata, re.M):
            raise RuntimeError('Input must be the x86_64 '+OLD+' base OS package.')
    if 'haiku_private_font_cache' in metadata or digest(server) != SERVER_HASH:
        raise RuntimeError('Unexpected base capability or unqualified app_server binary.')
    output.parent.mkdir(parents=True, exist_ok=True)
    cache = Path(__file__).resolve().parents[1]/'.cache'
    with tempfile.TemporaryDirectory(prefix='font-cache-1733-', dir=cache) as temporary:
        root = Path(temporary)
        original, stage, verified = (root/name for name in ('original', 'stage', 'verified'))
        for directory in (original, stage, verified):
            directory.mkdir()
        package('extract', '-C', original, base)
        package('extract', '-C', stage, base, '.PackageInfo', 'servers/app_server')
        before = inventory(original)
        info = stage/'.PackageInfo'
        text = info.read_text()
        if text.count(OLD) != 2:
            raise RuntimeError('Unexpected OS version/provides metadata.')
        text = text.replace(OLD, NEW)
        text, count = re.subn(r'(provides\s*\{)', r'\1\n\thaiku_private_font_cache=1', text, count=1)
        if count != 1:
            raise RuntimeError('Missing OS provides section.')
        text = re.sub(r'^packager\s+.*$', 'packager "air/OS: private-font cache fix for hrev60207+1733"', text, flags=re.M)
        info.write_text(text)
        # Keep the original file mode and extracted attributes while replacing
        # its bytes. All other entries stay in the original compressed package.
        (stage/'servers/app_server').write_bytes(server.read_bytes())
        pending = root/output.name
        shutil.copyfile(base, pending)
        package('add', '-f', '-C', stage, '-i', info, pending, 'servers/app_server')
        package('extract', '-C', verified, pending)
        after = inventory(verified)
        changed = sorted(name for name in before.keys() | after.keys() if before.get(name) != after.get(name))
        if changed != ['.PackageInfo', 'servers/app_server']:
            raise RuntimeError('Unexpected package changes: '+repr(changed))
        if digest(verified/'servers/app_server') != SERVER_HASH:
            raise RuntimeError('Packaged server did not round-trip.')
        resulting_metadata = package('list', '-i', pending)
        if 'provides: haiku_private_font_cache = 1' not in resulting_metadata or 'version: '+NEW not in resulting_metadata:
            raise RuntimeError('Missing updated OS version or font-cache capability.')
        shutil.copyfile(pending, output)
        Path(str(output)+'.sha256').write_text(digest(output)+'  '+output.name+'\n')
        Path(str(output)+'.info.txt').write_text(resulting_metadata)
        Path(str(output)+'.manifest.json').write_text(json.dumps({
            'base': str(base), 'base_sha256': digest(base),
            'base_source': '3e7f3a491b', 'fixed_source': 'e9268e63e0',
            'fix_commit': '2940a1cdff', 'capability_commit': 'e9268e63e0',
            'server_sha256': SERVER_HASH, 'output_sha256': digest(output),
            'changed_entries': changed, 'unchanged_entries': len(before)-len(changed),
            'comparison': 'All entry types, modes, symlink targets and file contents compared after extraction.',
        }, indent=2)+'\n')
        print(str(output))
        print('Verified only .PackageInfo and servers/app_server changed; '+str(len(before)-2)+' other entries preserved.')


if __name__ == '__main__':
    main()
