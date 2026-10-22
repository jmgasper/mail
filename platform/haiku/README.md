# Private-font lifetime fix

Mail's native emoji sequence rendering requires the accompanying
[app_server patch](app-server-font-cache.patch). The unpatched air/OS test
runtime (`hrev60206+1145`) crashed in FreeType's format-13 character lookup
after the emoji test exited and another application requested font coverage.
Do not deploy this Mail build to that runtime without the fix.

`FontCacheEntry` creates FreeType memory faces that borrow `FontStyle`'s font
bytes. The global cache can outlive the client font manager, which previously
freed those bytes. In addition, manager addresses and family/style identifiers
can be reused by a later client. The patch retains the style until after its
FreeType engine is destroyed and includes that retained style's identity in
the cache signature. It does not install Mail's fonts system-wide.

The fix commit is `2940a1cdff`; `e9268e63e0` adds the package capability
`haiku_private_font_cache = 1`. Both were merged as `51b26dfe89` into `master`
and pushed to the owner's experimental fork (`jmgasper/haiku`). The merged
topic branch was deleted. The validation worktree, `.cache/haiku-font-cache`,
is detached at `e9268e63e0`. No change was submitted upstream.
The patch applies to base `763c7eea18165f6f94bc0af9b81be33b5184d26a`.
For another checkout of the same base, review and apply with:

```sh
git apply --check /path/to/mail/platform/haiku/app-server-font-cache.patch
git apply /path/to/mail/platform/haiku/app-server-font-cache.patch
```

Build `app_server` using the fork's matching x86_64 build configuration. The
validation build used `tools/rock5-itx/env.sh` and an isolated copy of the
master build's objects, then `jam -j8 app_server`. No shared build objects or
installed host files were changed. Mail's package requires the capability
above, so package resolution rejects OS packages that do not advertise it.

For the x86_64 air/OS `hrev60207+1733` build, the matching base-system update is
`artifacts/haiku-r1~beta6_hrev60207_1733-2-x86_64.hpkg`. See the
[installation steps](../../docs/FONT_CACHE_UPDATE.md). This retains that build's
kernel and other files while updating app_server and the capability metadata.
The older `hrev60206+1145` package below is the original VM qualification package,
not an update to install over the newer `1733` build.

## Validation, 2026-10-21

- The isolated app_server build completed. Its SHA-256 is
  `6ca524b957548383c62c15293587e2fa6b323af30e3fd5b579d421e6640eca63`.
- A separate owned x86_64 QEMU image initially ran that binary through a user
  launch override. The running path and file hash were checked through
  `launch_roster`; the shared source image remained intact.
- `bash tools/test-native.sh` passed 125 sequence checks, 149 grapheme checks,
  55 typography checks, 120 composer checks, 40 inline-image checks and the
  native smoke/core suites. The sequence checks compare native raster output
  to eight glyphs drawn independently from the original font with FreeType.
  They also load 263 faces in one private family, beyond Haiku's 128-family
  limit, and check formatting, incremental composition and exact Unicode MIME.
- `bash tools/test-native-font-lifetime.sh` passed ten successive application
  pairs with the same app_server team. Each pair ran the sequence suite and
  typography suite in separate processes. Without screenshot requests those
  suites contain 123 and 53 checks respectively.
- A guest reboot retained the launch override and binary hash. Both suites
  passed again after reboot; the owned VM was then shut down.
- Package qualification subsequently upgraded the owned VM's OS package to
  `haiku-r1~beta6_hrev60206_1145-2`, replacing only app_server and metadata in a
  copy of its original package. The user launch override was removed. After
  reboot, the normal `/system/servers/app_server` path and matching binary hash
  were verified. Mail's dependency was rejected before the capability existed
  and resolved normally after the OS package upgrade. The optimized native
  suites and ten restart pairs also passed with this packaged server.

Logs and the reviewed composer capture are linked from
[Mail's status](../../docs/STATUS.md). The original crash report and isolated
build/image details are retained under `.cache` for debugging.

The source image was a hybrid live image: its kernel intentionally discarded
guest writes through a RAM filesystem overlay. The first offline-injected
server also encountered an unrelated `write_overlay` IORequest assertion on
boot. For this test only, the owned raw copy's ISO identification bytes at
offset 32769 were changed from `CD001` to `MAIL0`, leaving the MBR and BFS
partition intact. This makes that copy mount as a writable installed BFS disk;
it is not a change to the shared live image or an OS fix for the overlay.
