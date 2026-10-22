# Install Mail on air/OS hrev60207+1733 (x86_64)

`haiku_private_font_cache >= 1` is a capability provided by the fixed **haiku
base-system package**. There is no separate library with that name to install.
Mail needs the fix because its private emoji fonts exposed an app_server font
cache lifetime bug. The fix and package capability were merged on October 21,
2026, after the October 16 build reported by `uname -a`.

The matching update is
`haiku-r1~beta6_hrev60207_1733-2-x86_64.hpkg`. It is based on the existing
`r1~beta6_hrev60207_1733-1` package. It replaces `servers/app_server` and updates
the package version/capability metadata; the kernel and every other file are
retained. This package is specifically for that x86_64 build. The older
`hrev60206+1145` package used in Mail's original test VM is not the matching
update for this installation.

## Install the update

Download the update from the workspace's `mail/artifacts` directory and copy it
to the air/OS machine alongside the Mail revision 9 package. In Terminal,
from the directory containing the downloaded files:

```sh
pkgman install ./haiku-r1~beta6_hrev60207_1733-2-x86_64.hpkg
```

Reboot air/OS so the running app_server uses the fix. Then install Mail:

```sh
pkgman install ./airos_mail-0.1.0~dev1-9-x86_64.hpkg
```

`uname -a` will still show `hrev60207+1733`: this update preserves the kernel.
To check the installed package capability:

```sh
package list -i /boot/system/packages/haiku-r1~beta6_hrev60207_1733-2-x86_64.hpkg
```

Its provides list includes `haiku_private_font_cache = 1`. Updating the
capability alone does not fix the old server; the package includes both.

## Build and verification records

The source fix is described in [platform notes](../platform/haiku/README.md).
The original build is `3e7f3a491b` (`hrev60207-1733`); the fixed server is from
`e9268e63e0`. Comparing their app_server, headers and supporting library sources
shows only the three-file font-cache fix in these dependencies.

The package can be reproduced with `tools/package-font-cache-update.py` using
the matching original OS package, the qualified app_server, and the isolated
host tools from `.cache/package-env.sh`. The script verifies the input version,
architecture and server checksum, then extracts and compares the resulting
package. Every other file's content/mode and every symlink target is checked.
The `.manifest.json`, `.sha256` and `.info.txt` files beside the artifact record
its inputs, changes and package metadata.

Validation uses a separate copy-on-write VM upgraded to the exact reported
`hrev60207+1733` build. The original package reproduces the missing-capability
error. The update installs as package revision 2, and reboot loads the fixed
server from `/system/servers/app_server`. The full native Mail regression and
ten successive emoji/typography application pairs pass, with app_server team
118 unchanged throughout those pairs.

The VM also checks Mail installation, revision 8 → 9 upgrade, installed-app
launch/edit/preview and removal. The user's running installation is unchanged.
Logs use the `font-cache-1733-` prefix in `docs/evidence`, including
[`before`](evidence/font-cache-1733-before.log),
[`server-path`](evidence/font-cache-1733-server-path.log),
[`native`](evidence/font-cache-1733-native.log),
[`lifetime`](evidence/font-cache-1733-lifetime.log), and
[`mail-install`](evidence/font-cache-1733-mail-install.log).
