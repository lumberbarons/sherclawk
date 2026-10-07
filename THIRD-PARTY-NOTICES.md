# Third-party notices

Sherclawk is MIT-licensed (see [LICENSE](LICENSE)). It depends on or borrows
from the following, none of which this repository redistributes:

- **Certainly** — MIT License, © 2026 Matt Baker
  (https://github.com/minorbug/certainly). Fetch-only: the user clones it next
  to this repository; `build.sh` applies the patches in `patches/` to a
  container staging copy, leaving the upstream clone pristine.
- **BearSSL** — MIT License, © 2016 Thomas Pornin
  (https://www.bearssl.org/). Built as a Certainly dependency from the user's
  clone; not vendored here.
- **Retro68** — GPLv3+ (https://github.com/autc04/Retro68). Used as the
  `ghcr.io/autc04/retro68` Docker image, a build prerequisite; the toolchain
  is not redistributed here.
- **Apple Universal Interfaces** — Copyright Apple Inc. Not redistributed;
  fetched one-time by `tools/get-universal-interfaces.sh` for local builds.

Files vendored into this repository from the Sherclawk project itself:
`vendor/http.c`, `vendor/http.h`, and `vendor/host-tls/` (the host TLS test
shim). The patches in `patches/` are original project work that applies to
Certainly at build time.
