#!/usr/bin/env bash
set -euo pipefail
MAIL_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
MAIL_WORK=${MAIL_WORK:-/mnt/HaikuWork}
MAIL_CXX=${MAIL_CXX:-$MAIL_WORK/x399/build/x86_64/cross-tools-x86_64/bin/x86_64-unknown-haiku-g++}
MAIL_SYSROOT=${MAIL_SYSROOT:-$MAIL_WORK/toolchains/haiku-sysroot-x86_64}
MAIL_BUILD=${MAIL_BUILD:-$MAIL_ROOT/build-haiku}
MAIL_TOOLS=${MAIL_TOOLS:-$MAIL_WORK/build/master-x86_64/objects/linux/x86_64/release/tools}
# CI: extra roots holding a boot/system tree of staged dependencies (separated
# by ';'), extra compiler flags, and extra CMake arguments (word-split).
MAIL_EXTRA_ROOTS=${MAIL_EXTRA_ROOTS:-}
MAIL_CXX_FLAGS=${MAIL_CXX_FLAGS:-}
MAIL_CMAKE_ARGS=${MAIL_CMAKE_ARGS:-}
mkdir -p "$MAIL_BUILD"
cat > "$MAIL_BUILD/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Haiku)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_CXX_COMPILER "$MAIL_CXX")
set(CMAKE_SYSROOT "$MAIL_SYSROOT")
set(CMAKE_FIND_ROOT_PATH "$MAIL_SYSROOT${MAIL_EXTRA_ROOTS:+;$MAIL_EXTRA_ROOTS}")
set(CMAKE_CXX_FLAGS_INIT "$MAIL_CXX_FLAGS")
set(CMAKE_EXE_LINKER_FLAGS_INIT "$MAIL_CXX_FLAGS")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_PREFIX_PATH /boot/system/develop)
EOF
MAIL_WEBKIT_ARGS=()
if [[ -n ${MAIL_WEBKIT_LIBRARY:-} ]]; then MAIL_WEBKIT_ARGS+=("-DMAIL_WEBKIT_LIBRARY=$MAIL_WEBKIT_LIBRARY"); fi
if [[ -n ${MAIL_WEBKIT_INCLUDE_DIR:-} ]]; then MAIL_WEBKIT_ARGS+=("-DMAIL_WEBKIT_INCLUDE_DIR=$MAIL_WEBKIT_INCLUDE_DIR"); fi
cmake -S "$MAIL_ROOT" -B "$MAIL_BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$MAIL_BUILD/toolchain.cmake" -DCMAKE_BUILD_TYPE="${MAIL_BUILD_TYPE:-Debug}" \
    -DMAIL_RC="$MAIL_TOOLS/rc/rc" -DMAIL_XRES="$MAIL_TOOLS/xres" "${MAIL_WEBKIT_ARGS[@]}" $MAIL_CMAKE_ARGS
cmake --build "$MAIL_BUILD" -j"${JOBS:-4}"
