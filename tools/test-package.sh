#!/usr/bin/env bash
# Installs, upgrades and removes Mail in an already-running disposable Haiku VM.
# Requires a patched OS package advertising haiku_private_font_cache >= 1.
set -euo pipefail
MAIL_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
MAIL_VM_PORT=${MAIL_VM_PORT:-2258}
MAIL_VM_KEY=${MAIL_VM_KEY:-$MAIL_ROOT/../summit/.vm/airos-x86_64/id_ed25519}
MAIL_VM_KNOWN_HOSTS=${MAIL_VM_KNOWN_HOSTS:-$MAIL_ROOT/.cache/vm/known_hosts}
MAIL_BUILD=${MAIL_BUILD:-$MAIL_ROOT/build-haiku-release}
MAIL_ARTIFACTS=${MAIL_ARTIFACTS:-$MAIL_ROOT/artifacts}
MAIL_PACKAGE_BEFORE_REVISION=${MAIL_PACKAGE_BEFORE_REVISION:-1}
MAIL_PACKAGE_AFTER_REVISION=${MAIL_PACKAGE_AFTER_REVISION:-2}
MAIL_PACKAGE_SEED_TEST=${MAIL_PACKAGE_SEED_TEST:-$MAIL_BUILD/mail_native_package}
[[ $MAIL_PACKAGE_BEFORE_REVISION =~ ^[1-9][0-9]*$ && $MAIL_PACKAGE_AFTER_REVISION =~ ^[1-9][0-9]*$ ]] || exit 2
MAIL_SSH=(-i "$MAIL_VM_KEY" -o IdentitiesOnly=yes -o BatchMode=yes
    -o "UserKnownHostsFile=$MAIL_VM_KNOWN_HOSTS" -o ConnectTimeout=10)
ssh "${MAIL_SSH[@]}" -p "$MAIL_VM_PORT" user@127.0.0.1 \
    'test ! -e /boot/system/apps/airOS-Mail/Mail && mkdir -p /boot/home/mail-check'
for revision in "$MAIL_PACKAGE_BEFORE_REVISION" "$MAIL_PACKAGE_AFTER_REVISION"; do
    scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" \
        "$MAIL_ARTIFACTS/airos_mail-0.1.0~dev1-$revision-x86_64.hpkg" \
        user@127.0.0.1:/boot/home/mail-check/
done
scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" "$MAIL_BUILD/mail_native_package" \
    user@127.0.0.1:/boot/home/mail-check/mail_native_package.new
scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" "$MAIL_PACKAGE_SEED_TEST" \
    user@127.0.0.1:/boot/home/mail-check/mail_native_package_seed.new
scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" "$MAIL_ROOT/tests/fixtures/preview.png" \
    user@127.0.0.1:/boot/home/mail-check/package-preview.png
timeout 180 ssh "${MAIL_SSH[@]}" -p "$MAIL_VM_PORT" user@127.0.0.1 sh -s -- "$MAIL_PACKAGE_BEFORE_REVISION" "$MAIL_PACKAGE_AFTER_REVISION" <<'GUEST'
set -eu
cd /boot/home/mail-check
mv mail_native_package.new mail_native_package
mv mail_native_package_seed.new mail_native_package_seed
directory=$(mktemp -d /boot/home/mail-package-XXXXXX)
echo "Disposable package database: $directory/mail.db"
legacy=$(sha256sum /boot/system/apps/Mail)
pkgman install -y -R "./airos_mail-0.1.0~dev1-$1-x86_64.hpkg"
MAIL_PACKAGE_SCREENSHOT=/boot/home/mail-check/package-installed.png \
    ./mail_native_package_seed seed "$directory/mail.db" package-preview.png
pkgman install -y -R "./airos_mail-0.1.0~dev1-$2-x86_64.hpkg"
MAIL_PACKAGE_SCREENSHOT=/boot/home/mail-check/package-upgraded.png \
    ./mail_native_package verify "$directory/mail.db" package-preview.png
before=$(sha256sum "$directory/mail.db")
pkgman uninstall -y airos_mail
test ! -e /boot/system/apps/airOS-Mail/Mail
test ! -e /boot/system/apps/airOS-Mail/MailPreview
test ! -e /boot/system/data/deskbar/menu/Applications/airOS/Mail
test "$before" = "$(sha256sum "$directory/mail.db")"
test "$legacy" = "$(sha256sum /boot/system/apps/Mail)"
echo "Package lifecycle passed: install, native launch/edit/preview, upgrade, reopen, removal; draft database and legacy Mail preserved."
GUEST
for image in package-installed package-upgraded; do
    scp -O "${MAIL_SSH[@]}" -P "$MAIL_VM_PORT" \
        "user@127.0.0.1:/boot/home/mail-check/$image.png" "$MAIL_ROOT/.cache/vm/$image.png"
done
