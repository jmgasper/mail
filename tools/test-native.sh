#!/usr/bin/env bash
# Run in an already-started disposable Haiku VM. Never installs system packages.
# Replaces only files in /boot/home/mail-check, using atomic executable renames.
set -euo pipefail
MAIL_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
MAIL_VM_PORT=${MAIL_VM_PORT:-2258}
MAIL_VM_KEY=${MAIL_VM_KEY:-$MAIL_ROOT/../summit/.vm/airos-x86_64/id_ed25519}
MAIL_VM_KNOWN_HOSTS=${MAIL_VM_KNOWN_HOSTS:-$MAIL_ROOT/.cache/vm/known_hosts}
MAIL_BUILD=${MAIL_BUILD:-$MAIL_ROOT/build-haiku}
MAIL_SSH=(-i "$MAIL_VM_KEY" -o IdentitiesOnly=yes -o BatchMode=yes
    -o "UserKnownHostsFile=$MAIL_VM_KNOWN_HOSTS" -o ConnectTimeout=10)
ssh "${MAIL_SSH[@]}" -p "$MAIL_VM_PORT" user@127.0.0.1 'mkdir -p /boot/home/mail-check'
for program in Mail MailPreview mail_core_tests mail_snooze_tests mail_gmail_store_tests mail_draft_store_tests mail_organization_tests mail_compose_tests mail_preview_tests mail_reader_tests mail_clipboard_tests mail_text_clusters_tests mail_native_smoke mail_native_typography mail_native_composer mail_native_inline_images mail_native_graphemes mail_native_emoji_sequences; do
    scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" "$MAIL_BUILD/$program" \
        "user@127.0.0.1:/boot/home/mail-check/$program.new"
done
ssh "${MAIL_SSH[@]}" -p "$MAIL_VM_PORT" user@127.0.0.1 'mkdir -p /boot/home/mail-check/unicode'
scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" "$MAIL_ROOT"/tests/data/unicode/GraphemeBreakTest-*.txt \
    user@127.0.0.1:/boot/home/mail-check/unicode/
ssh "${MAIL_SSH[@]}" -p "$MAIL_VM_PORT" user@127.0.0.1 '
    cd /boot/home/mail-check || exit 1
    for program in Mail MailPreview mail_core_tests mail_snooze_tests mail_gmail_store_tests mail_draft_store_tests mail_organization_tests mail_compose_tests mail_preview_tests mail_reader_tests mail_clipboard_tests mail_text_clusters_tests mail_native_smoke mail_native_typography mail_native_composer mail_native_inline_images mail_native_graphemes mail_native_emoji_sequences; do mv "$program.new" "$program" || exit 1; done
    ./mail_core_tests && ./mail_snooze_tests && ./mail_gmail_store_tests && ./mail_draft_store_tests && ./mail_organization_tests && ./mail_compose_tests && ./mail_preview_tests && ./mail_reader_tests && ./mail_clipboard_tests && ./mail_text_clusters_tests unicode && MAIL_SEQUENCES_SCREENSHOT=/boot/home/mail-check/emoji-sequences.png ./mail_native_emoji_sequences && MAIL_SMOKE_SCREENSHOT=/boot/home/mail-check/smoke.png ./mail_native_smoke && MAIL_TYPOGRAPHY_SCREENSHOT=/boot/home/mail-check/typography.png ./mail_native_typography && MAIL_COMPOSE_SCREENSHOT=/boot/home/mail-check/compose-images.png ./mail_native_composer && MAIL_INLINE_SCREENSHOT=/boot/home/mail-check/inline-images.png ./mail_native_inline_images && ./mail_native_graphemes
'
mkdir -p "$MAIL_ROOT/.cache/vm"
scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" user@127.0.0.1:/boot/home/mail-check/smoke.png \
    "$MAIL_ROOT/.cache/vm/smoke.png"
scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" user@127.0.0.1:/boot/home/mail-check/typography.png \
    "$MAIL_ROOT/.cache/vm/typography.png"

scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" user@127.0.0.1:/boot/home/mail-check/compose-images.png \
    "$MAIL_ROOT/.cache/vm/compose-images.png"

scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" user@127.0.0.1:/boot/home/mail-check/inline-images.png \
    "$MAIL_ROOT/.cache/vm/inline-images.png"
scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" user@127.0.0.1:/boot/home/mail-check/inline-images.png.reflow.png \
    "$MAIL_ROOT/.cache/vm/inline-reflow.png"
scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" user@127.0.0.1:/boot/home/mail-check/emoji-sequences.png \
    "$MAIL_ROOT/.cache/vm/emoji-sequences.png"
