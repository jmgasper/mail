# Connect Proton Mail Bridge to Mail

Run Bridge and Mail on the same air/OS machine. Mail connects to Bridge's
local IMAP and SMTP services; Bridge handles the Proton account and encryption.

## Prepare Bridge

1. Open **Applications → Proton Mail Bridge** and sign in with an account that
   has Bridge access. Keep Bridge running while Mail checks or sends mail.
2. Open that account's mail-client settings in Bridge. Note its email address,
   generated username/password, IMAP and SMTP ports, and connection security.
   The normal ports are 1143 for IMAP and 1025 for SMTP, both with STARTTLS.
   Use the values actually shown by your installation if they differ.
3. In Bridge's settings, show the advanced settings, then choose
   **Export TLS certificates → Export** and select a private directory in your
   home folder. Bridge writes `cert.pem` and `key.pem`. Mail needs only
   `cert.pem`; the private key is not a mail-client credential.

The current air/OS Bridge port supports password/authenticator-code login.
Hardware security-key login is still pending in that port.

## Add the account in Mail

Open **File → Accounts → Add** and choose **Proton Mail Bridge**. Enter a name
and the email address shown by Bridge, then configure both servers:

| Setting | Incoming | Outgoing |
| --- | --- | --- |
| Host | `127.0.0.1` | `127.0.0.1` |
| Port | Bridge's IMAP port, normally `1143` | Bridge's SMTP port, normally `1025` |
| Security | Bridge's setting, normally STARTTLS | Bridge's setting, normally STARTTLS |
| Username | Bridge-generated username | Bridge-generated username |
| Password | Bridge-generated password | Bridge-generated password |
| CA certificate | Absolute path to exported `cert.pem` | Same `cert.pem` path |

Use `127.0.0.1`: the current Bridge certificate identifies that IP address.
Using `localhost` or `::1` with a certificate that lacks those names fails
hostname verification. Mail keeps certificate and hostname verification enabled.
It does not accept a remote host for the Bridge provider.

Leave **Sent copies → Automatic** selected. Bridge creates the Sent copy;
Mail reconciles that copy instead of uploading a second one. Save, then close
Accounts to start checking mail. Mail stores the generated passwords in the
system key store, whose first-use prompt may require authorization.

Add each Bridge identity as its own Mail account. New messages use the account
selected in the composer's From menu. Replies use the received message's account.

## Check the connection

Use a small mailbox initially. Check that Inbox and Sent load, then test
read/star, archive, trash and restore on a disposable message. Send to an address
you control, reply from that account, and check that each sending identity has
one Sent copy. Close and reopen Mail and Bridge, then check again without
re-entering passwords. Cached messages remain readable while Bridge is closed.

| Symptom | Action |
| --- | --- |
| Connection refused | Start Bridge and compare both ports with its current settings. |
| Authentication rejected | Use the password generated for that Bridge identity, rather than the Proton account login password. |
| Certificate verification fails | Use `127.0.0.1` and the exported `cert.pem`; check the clock. After a Bridge certificate reset, export and select its new certificate. |
| Sent copy remains pending | Keep Bridge running and check mail again. Confirm Sent in Bridge before changing the copy policy. |
| Cached account cannot change server identity | Add the new identity as a separate account; Mail protects the existing cache from being reassigned. |

The local integration fixture uses the real Bridge engine with a disposable
Proton API server and generated test identities. It does not validate a paid
account, Proton's live login service or the installed Bridge GUI. Those remain
separate live checks. No account credentials belong in scripts or test evidence.
