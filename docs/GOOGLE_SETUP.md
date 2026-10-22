# Connect Gmail to Mail

Mail needs a Google **Desktop app** OAuth client before its Google sign-in
button can be used. This checkout does not include a registered client. These
instructions configure a development installation; no real Gmail account has
yet been used to validate this build.

You can create the client on another computer, then copy its downloaded JSON
to air/OS. The actual sign-in must run in the browser on the same machine as
Mail, because the browser returns to Mail's local callback listener. No Python
installation or Google quickstart sample is needed to use Mail.

## Create the client

1. Select or create a project in [Google Cloud Console](https://console.cloud.google.com/).
   Enable **Gmail API** under **APIs & Services → Library**. Google documents
   this project setup in its [Gmail quickstart](https://developers.google.com/workspace/gmail/api/quickstart/python).
2. Open **Google Auth platform → Branding**. Configure the app name (for
   example, “Mail for air/OS”), support email and developer contact details.
   If the platform is not configured yet, choose **Get Started** first.
   Under **Audience**, choose **External** for a personal Gmail account and
   add the account under **Test users** while the project is in **Testing**.
   An organization-only Workspace project can use **Internal** when available.
   See [Google's consent setup](https://developers.google.com/workspace/guides/configure-oauth-consent).
3. Under **Data Access**, add
   `https://www.googleapis.com/auth/gmail.modify`. Mail requests this scope for
   reading, composing, sending and organizing mail. It is a restricted scope;
   distributing a public client requires Google's applicable verification.
   See [Gmail scopes](https://developers.google.com/workspace/gmail/api/auth/scopes).
4. Open **Clients → Create client**, select **Desktop app**, give it a name,
   then create it and download its JSON. The file must contain an `installed`
   object. A Web application client or service-account key will not work.
   See [Google's Desktop OAuth instructions](https://developers.google.com/identity/protocols/oauth2/native-app).

## Install the JSON on air/OS

Copy the downloaded file to the machine running Mail. In Terminal, substituting
its actual download path:

```sh
mkdir -p "$HOME/config/settings/Mail"
chmod 700 "$HOME/config/settings/Mail"
cp "/path/to/downloaded-client.json" "$HOME/config/settings/Mail/google-client.json"
chmod 600 "$HOME/config/settings/Mail/google-client.json"
```

Mail checks this path first, then `Mail.google-client.json` beside the Mail
executable. An invalid file in the settings directory takes precedence over a
valid adjacent file; replace or remove the stale settings copy when changing
clients. For a packaged build, CMake can copy and install that adjacent file:

```sh
cmake -S . -B build -DMAIL_GOOGLE_CLIENT_JSON=/absolute/path/to/downloaded-client.json
cmake --build build
```

Use the existing build directory and toolchain when cross-compiling (normally
`build-haiku`). Clearing the CMake option does not delete an earlier copied
client file; remove that old copy when withdrawing a registration. Keep client
JSON files out of source control. Desktop clients are public clients: this
file identifies the application, while the account's refresh token is the
credential that grants mailbox access. Mail stores that token in Haiku's
system key store. It does not write tokens into the mail database.

## Sign in and check mail

1. Open **File → Accounts → Add**, choose **Google (Gmail)**, and optionally
   enter a display name. Select **Sign in with Google**.
2. Complete account selection and Gmail consent in the preferred system web
   browser on the same air/OS machine. Mail listens temporarily on a random
   `127.0.0.1` port for the callback, using PKCE and state validation. No fixed
   redirect port or manual authorization-code paste is needed.
3. Allow Mail's system key-store access when Haiku prompts. The account's
   email address comes from Gmail and cannot be edited in the account form.
4. Close Accounts. Mail starts a background check and keeps downloaded mail
   available offline. **Check mail** runs another check; **Stop** cancels it.
5. For an initial live check, use a small test mailbox. Confirm that messages
   arrive and that reopening Mail can check mail without another browser
   sign-in. Then deliberately test read/star/archive and label changes on
   disposable messages. Use **Message → Add tag… / Remove tag…** for selected
   conversations, or **Organize → Tags…** to rename or remove a label throughout
   this Gmail account. Press **Check mail**, then verify the changes in Gmail;
   removing a label should keep its messages. To verify sending, compose a message to an address
   you control and press Send; confirm its delivery and a single Sent copy.

Package revision 7 and current source builds synchronize Gmail drafts. Mail
saves edits locally every two seconds and schedules a Gmail save after a short
pause in editing. The composer status shows whether the draft is saved locally,
waiting for Gmail, or saved on Gmail. **File → Draft synchronization…** and the
composer's **Review saved versions…** button show progress and conflicts;
**Check Gmail** checks immediately. Older packages through revision 6 save drafts
only on this computer.

For the first draft check, create a disposable draft with text, formatting and
an attachment. Wait for **Draft saved on Gmail**, then confirm it appears in
Gmail. Close and reopen Mail to check local recovery. With the composer closed,
edit the draft in Gmail and check mail in Mail to import that edit. If a remote
edit conflicts with local work, close the composer and use **Keep both versions**
in the review window. This preserves the local work in a separate draft. A
missing Gmail draft instead offers **Keep local draft** when local edits exist.
Avoid editing the same draft on both devices simultaneously during this first
check: the API check and subsequent update are separate requests.

After an interrupted upload, Mail checks the saved attempt before writing again.
Package revision 8 and current source builds list possible copies in the review
window. Select one to compare its text and recipients, then choose **Use Gmail
copy…**. A copy with only rewritten transport headers can resolve the save
without another upload. Different content keeps both versions; other Gmail
copies remain intact and may appear separately in Drafts.

If no copy is suitable, inspect Gmail first, then choose **Retry save…**. This
requires confirmation because the earlier upload could appear later and leave
a duplicate. Cancel keeps the unresolved save. Close the composer and return
queued mail to Drafts before either action. For an already discarded local
draft, **Stop checking…** leaves any Gmail copies intact. These actions do not
send messages to recipients. No live Gmail draft behavior has been qualified yet.

Cancel in Accounts, or close Accounts during sign-in, to abandon the attempt.
Signing in again must select the saved account; a different identity is
rejected. Adding an already connected Google identity reconnects that account
without creating another mailbox.

## Reconnect and troubleshoot

| Symptom | Action |
| --- | --- |
| Google sign-in is not configured | Install the Desktop client JSON at one of the paths above. |
| The downloaded client is rejected | Download a Desktop app client; do not hand-edit a Web client into one. |
| Google denies access to a test user | Check the project's Audience test-user list and any Workspace administrator policy. |
| Browser does not open | Set a preferred browser on air/OS, then try again. |
| Browser callback cannot connect | Keep Mail open and complete sign-in in the browser on the same machine. Cancel and retry if the five-minute window has elapsed. |
| Mail asks for sign-in again | Open the saved Google account and choose Sign in again. A revoked or expired refresh grant needs new consent. |
| TLS verification fails | Check the machine's clock and installed system CA trust; verification remains enabled. |

For an External project in Testing, Google issues refresh tokens that expire
after seven days when Gmail access is requested. This is expected during
development. See [refresh token expiration](https://developers.google.com/identity/protocols/oauth2#expiration).

**Revoke Google access…** invalidates this project's authorization for that
Google account and removes Mail's saved credential, while keeping cached mail.
Google revocation also invalidates that account's grants for other clients
under the same project, including installations on other devices. See
[Google's revocation behavior](https://developers.google.com/identity/protocols/oauth2/native-app#tokenrevoke).
**Remove** in Accounts instead deletes the local account, cache and credential;
it does not revoke the Google project's grant.

The automated native checks use a simulated browser and disposable HTTPS
provider. Completing Google's actual consent pages in the target browser is
still a separate live validation step.

Console steps and linked Google documentation were checked on 2026-10-22.
