# Building Mail packages

Mail's package name is `airos_mail`; the application is **Mail**, and it
replaces Haiku's Mail. Local builds are `0.1.0~dev1-REVISION`; air/OS images
build `0.1.0~gitDATE-1` from the commit pinned in airos-ci's
`apps/mail.lock.json` (`MAIL_VERSION` sets it). The package installs
`apps/Mail` (with Haiku's Mail icon), its preview helper as
`lib/mail/MailPreview`, the Deskbar entry **Applications → Mail**, and a
post-install script that opens email messages and mailto links in it. air/OS
x86_64 images leave Haiku's Mail out; on a system that still has it, this
package's `apps/Mail` and Deskbar entry take its place.

## Build on Haiku

Install the dependencies described in the main README, including modern
Summit WebKit, HarfBuzz and its subset library. Use an air/OS build providing
`haiku_private_font_cache >= 1`; see the [OS fix](../platform/haiku/README.md).

```sh
cmake -S . -B build-haiku-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-haiku-release
bash tools/package-haiku.sh
```

The builder strips debug information from staging copies, restores all app and
font resources, generates MIME attributes and the Deskbar link, and includes
the installation/Gmail guides and licenses. It checks the package's app
signatures, icons and MIME entries before accepting the artifact.

Output is `artifacts/airos_mail-0.1.0~dev1-1-x86_64.hpkg` on x86_64, with a
portable `.sha256` checksum, an attribute inventory, package metadata and build
input records. `MAIL_BUILD` selects another existing native build directory;
`MAIL_ARTIFACTS` changes the output directory. To make the next package revision:

```sh
MAIL_PACKAGE_REVISION=2 bash tools/package-haiku.sh
```

The builder reads the ELF machine type; x86_64 is the qualified architecture.
It recognizes arm64, but an arm64 package and its renderer dependencies have
not been built or tested here.

## Cross-package from this workspace

`MAIL_BUILD=build-haiku-release MAIL_BUILD_TYPE=Release bash tools/cross-build.sh`
builds the optimized application and native checks with the configured SDK.
Set the same WebKit library/header overrides as the debug cross build.

Cross-packaging needs Haiku's host `package`, `mimeset` and `xres` tools, the
matching cross `strip`, and a complete target MIME database. Configure isolated
host tools with `HAIKU_HOST_USE_XATTR = 0` and `HAIKU_HOST_USE_XATTR_REF = 1` and
rebuild them after changing these settings. The reference mode attaches unique
IDs to files and keeps large attributes in that build's attribute directory.
Plain inode-based attribute storage can pick up stale attributes after temporary
files are deleted; direct xattr storage exceeded this host filesystem's limit
when extracting the target MIME database. Neither mode is used for the final
workspace package artifacts.

Set `MAIL_PACKAGE_TOOL`, `MAIL_MIMESET`, `MAIL_XRES`, `MAIL_STRIP` and
`MAIL_MIME_DB` to those tools and data. `MAIL_READELF` can also be overridden.
All tools must load the same build's compatibility libraries. A target MIME
database can be extracted from the target OS package with:

```sh
mkdir -p .cache/package-mime
"$MAIL_PACKAGE_TOOL" extract -C .cache/package-mime /path/to/haiku.hpkg data/mime_db
export MAIL_MIME_DB="$PWD/.cache/package-mime/data/mime_db"
bash tools/package-haiku.sh
```

The validated workspace configuration is `.cache/package-env.sh`, using the
isolated `.cache/haiku-font-build` tools and their compatibility libraries.
No system SDK installation is replaced by this procedure.

## Native package qualification

Build revisions 1 and 2, then run `bash tools/test-package.sh` against an
already-running disposable VM. The same `MAIL_VM_PORT`, `MAIL_VM_KEY` and
`MAIL_VM_KNOWN_HOSTS` options used by the native test runner apply. This test
installs, upgrades and removes Mail, and refuses to start when its installation
path already exists. It leaves its disposable draft database for inspection.

The fixture uses only a local account and sends no mail. It checks icons and
resources after stripping, launches the installed executable via Deskbar,
edits and saves a draft, opens the installed preview helper, waits for actual
image pixels, upgrades, relaunches by application signature and verifies the
saved draft. Finally it checks that package removal leaves the database and
legacy Mail unchanged. The database is closed before launching Mail and reopened
after exit; the fixture does not keep a competing native SQLite writer open.

`MAIL_PACKAGE_BEFORE_REVISION` and `MAIL_PACKAGE_AFTER_REVISION` select the two
package revisions (defaults 1 and 2). When they use different database schemas,
preserve the earlier build's `mail_native_package` binary and select it with
`MAIL_PACKAGE_SEED_TEST`; the default is the current build's fixture. For the
schema-10 to schema-11 upgrade:

```sh
MAIL_PACKAGE_BEFORE_REVISION=4 MAIL_PACKAGE_AFTER_REVISION=5 \
MAIL_PACKAGE_SEED_TEST=.cache/mail-native-package-schema10-v4 bash tools/test-package.sh
```

The new provider recovery checks run with `ctest --test-dir build-host -R mail_moves`.
For the same checks on the native build in the existing VM:

```sh
MAIL_NATIVE_TEST=mail_move_tests python3 tests/move_fixture.py tools/run-native-protocol.sh
```

The native review controls use a separate fixture run, with disposable key-store
credentials and confirmation dialogs:

```sh
MAIL_BUILD=build-haiku-release MAIL_NATIVE_TEST=mail_native_moves \
python3 tests/move_fixture.py --native-ui tools/run-native-protocol.sh
```

The source fix and package capability are in the owner's air/OS `master` at
`51b26dfe890807be59b64907da3a55edcc5b26e2`. The owned test VM uses a qualification
OS package made from its original `hrev60206+1145` package, replacing only
app_server and package metadata. It is not a new complete OS release image.
