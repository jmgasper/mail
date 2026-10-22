#!/usr/bin/env bash
# Run after test-native.sh has uploaded the executables to a disposable VM.
# Each pair creates hundreds of private font styles, exits, then starts a new
# application. Cached FreeType faces must not outlive their backing font data.
set -euo pipefail
MAIL_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
MAIL_VM_PORT=${MAIL_VM_PORT:-2258}
MAIL_VM_KEY=${MAIL_VM_KEY:-$MAIL_ROOT/../summit/.vm/airos-x86_64/id_ed25519}
MAIL_VM_KNOWN_HOSTS=${MAIL_VM_KNOWN_HOSTS:-$MAIL_ROOT/.cache/vm/known_hosts}
timeout 240 ssh -i "$MAIL_VM_KEY" -o IdentitiesOnly=yes -o BatchMode=yes \
    -o "UserKnownHostsFile=$MAIL_VM_KNOWN_HOSTS" -o ConnectTimeout=10 \
    -p "$MAIL_VM_PORT" user@127.0.0.1 sh -s <<'GUEST'
set -eu
cd /boot/home/mail-check
server_team() { ps | awk '$1 ~ /\/app_server$/ { print $2 }'; }
initial=$(server_team)
test -n "$initial"
echo "Initial app_server team: $initial"
for cycle in 1 2 3 4 5 6 7 8 9 10; do
    ./mail_native_emoji_sequences
    ./mail_native_typography
    current=$(server_team)
    test "$current" = "$initial"
    echo "Font lifetime cycle $cycle: passed; app_server team $current unchanged"
done
GUEST
