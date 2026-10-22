# Mail for air/OS

Mail is a native email application for multiple IMAP, POP3 and Gmail accounts.
Proton Mail accounts use the separately installed Proton Mail Bridge. This is
a development build; live-provider qualification is still in progress.

## Install and open

Open the `.hpkg` in HaikuDepot, or use Terminal:

```sh
pkgman install /path/to/airos_mail-0.1.0~dev1-9-x86_64.hpkg
```

The package requires an air/OS build providing `haiku_private_font_cache >= 1`,
which fixes private-font lifetime in app_server. This is a capability of the
fixed `haiku` base-system package, not a separate library. If it is missing on
x86_64 `hrev60207+1733`, install the matching
`haiku-r1~beta6_hrev60207_1733-2-x86_64.hpkg` update supplied with the Mail build
artifacts, then reboot and retry Mail. Other OS builds need a matching update
that includes both the fix and capability. Mail also requires the modern Summit
WebKit engine and the libraries listed in the package metadata.

Open **Applications → Mail** from Deskbar. Mail installs as `apps/Mail` in
place of Haiku's Mail (air/OS x86_64 images no longer include Haiku's), with
its preview helper in `lib/mail`. After installation, email messages and
mailto links open in it.

Use **File → Accounts → Add** to configure an account. For Gmail, follow
[Google setup](GOOGLE_SETUP.md); a registered Desktop OAuth client is required.
For Proton, follow [Bridge setup](BRIDGE_SETUP.md), start Bridge first and use
its local IMAP/SMTP credentials and certificate trust. These differ from your
Proton account password.

Mail stores cached messages, drafts and organization in
`~/config/settings/Mail/mail.db`. Passwords and OAuth refresh tokens are stored
in Haiku's key store. The first account connection can trigger its access prompt.

## Update or remove

Install a newer package with the same `pkgman install` command. Close Mail and
its preview windows first. Package upgrades preserve settings, cached mail,
drafts and key-store credentials. Back up the settings directory with Mail
closed before testing development builds.

Package revision 5 introduced folder-move review and schema 11; revision 6 uses
the same schema. Packages older than revision 5 cannot open that database.
Keep the closed-app backup if you need to return to an earlier development build.

Revision 7 introduced schema 13 for Gmail draft synchronization. Revisions 8–9
and current source builds use schema 14 to retain possible copies of interrupted
saves for review. Packages through revision 7 cannot reopen a profile migrated
by this build. Keep a closed-app backup if you need to return to an earlier version.
Revision 9 keeps the same schema and includes both provider setup guides.

```sh
pkgman uninstall airos_mail
```

Removal uninstalls the application and Deskbar entry. It retains the settings
directory and key-store credentials; removing a package does not revoke Google
access or remove an account from its provider. To revoke Google access, use
Mail's Accounts window before uninstalling.

## Current limits

Supported emoji use monochrome outlines, with the font's designed regular and
bold weights. System window titles may still show fallback boxes. Snooze reminders run while Mail is
open. Gmail drafts synchronize in the background; other providers' drafts remain
local. Live-account validation remains pending. SMTP/Gmail sends interrupted without a confirmed response stay
in Outbox for review instead of being retried automatically.

**File → Draft synchronization…** reviews Gmail saves. Local and remote conflicts
can keep both versions after the composer closes. An interrupted save is checked
before another write. Choose **Use Gmail copy…** to link a listed copy, or
**Retry save…** to explicitly create another after inspecting Gmail. Retrying
can leave a duplicate; both confirmations default to Cancel. Simultaneous edits from
another device can race with Mail's separate check and update requests.

IMAP folder moves use MOVE when available, or COPY and targeted removal on
UIDPLUS servers. Mail saves the destination identity before removing the source
and resumes after interruptions. If a reply is lost, it checks new destination
messages for an exact content match. Multiple matches, changed mailbox identifiers
or an unconfirmed copy leave the original cached and the move pending. The check
reports the reason; Mail does not create another uncertain copy automatically.
The two affected folders wait for resolution while other folders continue to sync.

Open **File → Folder moves…**, or the **Folder moves** button, to inspect pending
moves and their latest errors. **Check server** continues saved progress; for an
unconfirmed copy it looks for the previous attempt without copying again.
**Retry copy…** asks for confirmation because another copy can create a duplicate.
Inspect the destination folder first. A recorded destination cannot be copied
again through this control.

**Keep local copy…** stops a started move and keeps the message, tags and current
local edits. It removes no server messages. Server copies may appear separately
on the next check, and later edits to the retained local copy stay on this computer.
A move that has not started instead offers **Cancel move**, which restores its
server folder in Mail. These actions do not send mail to recipients.

The package includes the MIT licenses for Mail and its JSON library, and the
SIL Open Font License for Noto Emoji, under `documentation/packages/airos_mail`.
