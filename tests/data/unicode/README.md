# Grapheme conformance data

Unmodified Unicode Character Database test files, downloaded from Unicode's
official site on 2026-10-21:

- [Unicode 15.1.0](https://www.unicode.org/Public/15.1.0/ucd/auxiliary/GraphemeBreakTest.txt)
- [Unicode 16.0.0](https://www.unicode.org/Public/16.0.0/ucd/auxiliary/GraphemeBreakTest.txt)
- [Unicode 17.0.0](https://www.unicode.org/Public/17.0.0/ucd/auxiliary/GraphemeBreakTest.txt)
- [Unicode data license](https://www.unicode.org/license.txt), copied to `LICENSE.txt`.

`mail_text_clusters_tests` selects the corpus using ICU's `u_getUnicodeVersion`
and verifies every case and the file's declared case count. A missing version
fails explicitly. Character properties change between Unicode versions, so a
newer runtime must not be checked against an older corpus. Add the matching
official test file when upgrading ICU to a new Unicode version.

Current execution evidence covers ICU 74.1 / Unicode 15.1.0 on air/OS (1,187
cases) and ICU 78.2 / Unicode 17.0.0 on the Linux host (766 cases). The Unicode
16.0.0 corpus is included for other supported ICU installations, but has not
been executed against such an installation here. Each run also checks 154
Mail-specific byte-boundary, malformed-input and size-limit assertions.

SHA-256:

```
ed9c5e92fd0911ccbeeb63c97cb19c519ea272ff1112ce843abd991582dd848f  GraphemeBreakTest-15.1.0.txt
ee2b9354d270ac061b29f09662cafea06341d77e704b8cc6bd72aaeeda363cb5  GraphemeBreakTest-16.0.0.txt
e2d134d2c52919bace503ebb6a551c1855fe1a1faec18478c78fff254a1793ec  GraphemeBreakTest-17.0.0.txt
e7a93b009565cfce55919a381437ac4db883e9da2126fa28b91d12732bc53d96  LICENSE.txt
```
