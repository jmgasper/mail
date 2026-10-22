#!/bin/sh
# Mail replaces Haiku's Mail: email messages and mailto links open in it.
for type in text/x-email message/rfc822 application/x-vnd.Be.URL.mailto; do
	setmime -set "$type" -preferredAppSig application/x-vnd.air-Mail
done
