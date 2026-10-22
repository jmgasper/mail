#!/usr/bin/env bash
# Package an existing native build. Host tools may be overridden for cross builds.
set -euo pipefail
MAIL_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
MAIL_BUILD=${MAIL_BUILD:-$MAIL_ROOT/build-haiku-release}
MAIL_ARTIFACTS=${MAIL_ARTIFACTS:-$MAIL_ROOT/artifacts}
MAIL_PACKAGE_REVISION=${MAIL_PACKAGE_REVISION:-1}
# CI passes its own version (for example 0.1.0~git20261023.1200-1).
MAIL_VERSION=${MAIL_VERSION:-0.1.0~dev1-$MAIL_PACKAGE_REVISION}
MAIL_PACKAGE_TOOL=${MAIL_PACKAGE_TOOL:-package}
MAIL_MIMESET=${MAIL_MIMESET:-mimeset}
MAIL_STRIP=${MAIL_STRIP:-strip}
MAIL_XRES=${MAIL_XRES:-xres}
MAIL_READELF=${MAIL_READELF:-readelf}
MAIL_MIME_DB=${MAIL_MIME_DB:-/boot/system/data/mime_db}
[[ $MAIL_PACKAGE_REVISION =~ ^[1-9][0-9]*$ ]] || { echo 'Package revision must be a positive integer.' >&2; exit 1; }
MAIL_BUILD=$(cd -- "$MAIL_BUILD" && pwd)
cmake --build "$MAIL_BUILD" --target Mail MailPreview -j"${JOBS:-4}"
MAIL_MACHINE=$("$MAIL_READELF" -h "$MAIL_BUILD/Mail" | sed -n 's/^ *Machine: *//p')
case $MAIL_MACHINE in
    'Advanced Micro Devices X86-64') MAIL_ARCH=x86_64 ;;
    AArch64) MAIL_ARCH=arm64 ;;
    *) echo "Unsupported Mail ELF machine: $MAIL_MACHINE" >&2; exit 1 ;;
esac
mkdir -p "$MAIL_ROOT/.cache" "$MAIL_ARTIFACTS"
MAIL_ARTIFACTS=$(cd -- "$MAIL_ARTIFACTS" && pwd)
MAIL_STAGE=$(mktemp -d "$MAIL_ROOT/.cache/package-stage-XXXXXX")
trap 'rm -rf -- "$MAIL_STAGE"' EXIT
cmake --install "$MAIL_BUILD" --prefix "$MAIL_STAGE"
# strip removes appended resources; restore both app metadata and embedded fonts.
# Mail replaces Haiku's Mail: apps/Mail, with its preview helper in lib/mail.
for app in apps/Mail lib/mail/MailPreview; do
    "$MAIL_STRIP" --strip-debug "$MAIL_STAGE/$app"
    "$MAIL_XRES" -o "$MAIL_STAGE/$app" \
        "$MAIL_BUILD/$(basename "$app").fonts.rsrc" "$MAIL_BUILD/$(basename "$app").rsrc"
done
sed -e "s/@VERSION@/$MAIL_VERSION/g" -e "s/@SHORT_VERSION@/${MAIL_VERSION%-*}/g" -e "s/@ARCH@/$MAIL_ARCH/g" \
    "$MAIL_ROOT/resources/Mail.PackageInfo.in" > "$MAIL_STAGE/.PackageInfo"
mkdir -p "$MAIL_STAGE/data/deskbar/menu/Applications" "$MAIL_STAGE/data/licenses" "$MAIL_STAGE/boot/post-install"
ln -s ../../../../apps/Mail "$MAIL_STAGE/data/deskbar/menu/Applications/Mail"
cp "$MAIL_ROOT/third_party/noto-emoji/OFL.txt" "$MAIL_STAGE/data/licenses/Mail Noto Emoji OFL 1.1"
install -m 755 "$MAIL_ROOT/resources/mail-post-install.sh" "$MAIL_STAGE/boot/post-install/mail-default-email.sh"
(
    cd "$MAIL_STAGE"
    "$MAIL_MIMESET" --all -f --mimedb data/mime_db --mimedb "$MAIL_MIME_DB" \
        apps/Mail lib/mail/MailPreview
)
MAIL_OUTPUT="$MAIL_ARTIFACTS/airos_mail-$MAIL_VERSION-$MAIL_ARCH.hpkg"
MAIL_PENDING="$MAIL_OUTPUT.new.hpkg"
"$MAIL_PACKAGE_TOOL" create -C "$MAIL_STAGE" "$MAIL_PENDING"
"$MAIL_PACKAGE_TOOL" list -a "$MAIL_PENDING" > "$MAIL_OUTPUT.contents.txt.new"
# Missing MIME sniff rules or stale inode-based host attributes must never
# produce a package that appears successful but has unusable/foreign metadata.
for attribute in BEOS:APP_SIG BEOS:ICON META:TYPE; do
    if ! awk -v attribute="$attribute" 'index($0,"<" attribute " "){n++} END{exit n==2?0:1}' "$MAIL_OUTPUT.contents.txt.new"; then
        echo "Invalid package attributes ($attribute). Use a complete MIME database and isolated Haiku host tools with xattr references." >&2
        rm -f -- "$MAIL_PENDING" "$MAIL_OUTPUT.contents.txt.new"
        exit 1
    fi
done
mv -- "$MAIL_PENDING" "$MAIL_OUTPUT"
mv -- "$MAIL_OUTPUT.contents.txt.new" "$MAIL_OUTPUT.contents.txt"
"$MAIL_PACKAGE_TOOL" list -i "$MAIL_OUTPUT" > "$MAIL_OUTPUT.info.txt"
(cd "$MAIL_ARTIFACTS" && sha256sum "$(basename -- "$MAIL_OUTPUT")") > "$MAIL_OUTPUT.sha256"
sha256sum "$MAIL_BUILD/Mail" "$MAIL_BUILD/MailPreview" \
    "$MAIL_ROOT/CMakeLists.txt" "$MAIL_ROOT/resources/Mail.PackageInfo.in" \
    > "$MAIL_OUTPUT.build-inputs.sha256"
sed -n '/^CMAKE_BUILD_TYPE:/p; /^CMAKE_CXX_FLAGS[^:]*:/p; /^CMAKE_CXX_COMPILER:/p' \
    "$MAIL_BUILD/CMakeCache.txt" > "$MAIL_OUTPUT.build-info.txt"
printf '%s\n' "$MAIL_OUTPUT"
