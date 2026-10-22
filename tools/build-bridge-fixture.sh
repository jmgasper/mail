#!/usr/bin/env bash
# Add one integration test through Go's overlay; never edit the Bridge checkout.
set -euo pipefail
MAIL_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
MAIL_BRIDGE_SOURCE=${MAIL_BRIDGE_SOURCE:-$MAIL_ROOT/../proton-bridge}
MAIL_BRIDGE_BUILD=${MAIL_BRIDGE_BUILD:-$MAIL_ROOT/.cache/bridge}
mkdir -p "$MAIL_BRIDGE_BUILD"
python3 - "$MAIL_ROOT" "$MAIL_BRIDGE_SOURCE" "$MAIL_BRIDGE_BUILD" <<'PY'
import json,sys
from pathlib import Path
root,source,build=map(lambda p:Path(p).resolve(),sys.argv[1:])
(build/'overlay.json').write_text(json.dumps({'Replace':{str(source/'internal/bridge/mail_interop_test.go'):str(root/'tests/BridgeFixture.go')}}))
PY
cd "$MAIL_BRIDGE_SOURCE"
[[ -f vendor/modules.txt ]] || { echo 'Prepare the Bridge vendor tree using its documented Haiku engine build first.' >&2; exit 1; }
source tools/haiku/env.sh
go test -mod=vendor -p "${JOBS:-4}" -ldflags '-s -w' -overlay "$MAIL_BRIDGE_BUILD/overlay.json" -c \
    -o "$MAIL_BRIDGE_BUILD/bridge-mail.test" ./internal/bridge
git rev-parse HEAD > "$MAIL_BRIDGE_BUILD/source-revision.txt"
git status --porcelain > "$MAIL_BRIDGE_BUILD/source-status.txt"
