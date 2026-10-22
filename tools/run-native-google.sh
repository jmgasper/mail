#!/usr/bin/env bash
# Adapter for the disposable HTTPS OAuth/Gmail fixture. No real account used.
set -euo pipefail
MAIL_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
MAIL_VM_PORT=${MAIL_VM_PORT:-2258}
MAIL_VM_KEY=${MAIL_VM_KEY:-$MAIL_ROOT/../summit/.vm/airos-x86_64/id_ed25519}
MAIL_VM_KNOWN_HOSTS=${MAIL_VM_KNOWN_HOSTS:-$MAIL_ROOT/.cache/vm/known_hosts}
MAIL_BUILD=${MAIL_BUILD:-$MAIL_ROOT/build-haiku}
MAIL_NATIVE_TEST=${MAIL_NATIVE_TEST:-mail_google_tests}
case "$MAIL_NATIVE_TEST" in mail_google_tests|mail_gmail_sync_tests|mail_gmail_draft_tests|mail_gmail_draft_sync_tests|mail_native_drafts|mail_native_google|mail_organization_sync_tests|mail_native_organization) ;; *) exit 2;; esac
MAIL_SSH=(-i "$MAIL_VM_KEY" -o IdentitiesOnly=yes -o BatchMode=yes
    -o "UserKnownHostsFile=$MAIL_VM_KNOWN_HOSTS" -o ConnectTimeout=10)
[[ $# == 3 && "$3" =~ ^[0-9]{1,5}$ ]] || exit 2
ssh "${MAIL_SSH[@]}" -p "$MAIL_VM_PORT" user@127.0.0.1 'mkdir -p /boot/home/mail-check'
scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" "$MAIL_BUILD/$MAIL_NATIVE_TEST" \
    "user@127.0.0.1:/boot/home/mail-check/$MAIL_NATIVE_TEST.new"
scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" "$2" \
    user@127.0.0.1:/boot/home/mail-check/google-cert.pem
ssh "${MAIL_SSH[@]}" -p "$MAIL_VM_PORT" user@127.0.0.1 \
    "cd /boot/home/mail-check && mv $MAIL_NATIVE_TEST.new $MAIL_NATIVE_TEST && MAIL_GOOGLE_SCREENSHOT=/boot/home/mail-check/native-google.png ./$MAIL_NATIVE_TEST 10.0.2.2 /boot/home/mail-check/google-cert.pem $3 > $MAIL_NATIVE_TEST.log 2>&1; mail_test_status=\$?; cat $MAIL_NATIVE_TEST.log; exit \$mail_test_status"
if [[ "$MAIL_NATIVE_TEST" == mail_native_google ]]; then
    scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" user@127.0.0.1:/boot/home/mail-check/native-google.png \
        "$MAIL_ROOT/.cache/vm/native-google.png"
fi
if [[ "$MAIL_NATIVE_TEST" == mail_native_drafts ]]; then
    scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" user@127.0.0.1:/boot/home/mail-check/native-drafts.png \
        "$MAIL_ROOT/.cache/vm/native-drafts.png"
    scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" user@127.0.0.1:/boot/home/mail-check/native-draft-recovery.png \
        "$MAIL_ROOT/.cache/vm/native-draft-recovery.png"
fi

if [[ "$MAIL_NATIVE_TEST" == mail_native_organization ]]; then
    for name in rules smart tags; do
        scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" "user@127.0.0.1:/boot/home/mail-check/organization-$name.png" "$MAIL_ROOT/.cache/vm/organization-$name.png"
    done
fi
