# Third-party notices

Sherclawk's original source is MIT-licensed (see [LICENSE](LICENSE)).
Third-party material retains its own copyright and license as described below.

## Material included in this repository

- **Certainly HTTP helpers** — `vendor/http.c` and `vendor/http.h` originated
  in this project's HelloHTTPS/HelloChat helpers, adapted from Certainly's
  Postman example. The upstream code is Copyright (c) 2026 Matt Baker,
  MIT-licensed; the full notice is in [LICENSES/Certainly.txt](LICENSES/Certainly.txt).
  [Upstream Certainly](https://github.com/minorbug/certainly).
- **Certainly patches** — `patches/` contains project modifications and
  upstream code/context covered by the same Certainly MIT license.
  `build.sh` applies these patches to a staging copy; the user's upstream
  clone remains pristine.
- **Host TLS shim** — `vendor/host-tls/` is project code under the repository's
  MIT license. It implements Certainly's transport interface for host tests.
- **Project artwork** — `art/sherclawk.png` was generated with ChatGPT. The
  project offers it under the repository's MIT license to the extent it holds
  rights in the artwork.

## Separately supplied dependencies

- **Certainly** — Copyright (c) 2026 Matt Baker, MIT License. Apart from the
  adapted helpers and patch context above, its source is not bundled here.
  Users clone it separately; it is statically linked into Sherclawk apps.
  [License notice](LICENSES/Certainly.txt).
- **BearSSL** — Copyright (c) 2016, 2017, 2018 Thomas Pornin, MIT License.
  Its source is supplied through the user's Certainly clone and statically
  linked into apps; it is not bundled in this repository.
  [Upstream BearSSL](https://www.bearssl.org/),
  [license notice](LICENSES/BearSSL.txt).
- **Retro68** — A separately supplied build toolchain with components under
  several licenses, including GPLv3+. The build uses the
  `ghcr.io/autc04/retro68` Docker image; the image and toolchain sources are
  not bundled here. Runtime code included in compiled apps has its own
  licensing, including the GCC Runtime Library Exception for Retro68's
  `libretro` and GCC runtime libraries. The exception permits qualifying
  applications to use their own licenses; using Retro68 does not by itself
  require relicensing Sherclawk as GPL.
  [Upstream Retro68](https://github.com/autc04/Retro68),
  [runtime exception](https://github.com/autc04/Retro68/blob/master/COPYING.RUNTIME).
- **Apple Universal Interfaces and legacy development tools** — Copyright
  Apple Inc. These are proprietary, user-supplied prerequisites, not included
  or downloaded by this repository. Their use and any distribution of linked
  SDK code remain subject to their applicable licenses; Sherclawk's MIT
  license does not grant rights to them. The same applies to users' Mac OS
  install media, ROMs and guest disk images.

## Distributing compiled apps

An app includes Certainly and BearSSL code even though the libraries' source
trees are supplied separately. Include this notice, the project's [LICENSE](LICENSE)
and the full Certainly and BearSSL notices in `LICENSES/` with app distributions.
Preserve the required notices for any additional code included in the particular
build, including C runtime components, and verify the applicable SDK terms before
distributing SDK-derived code. Do not package the SDK, development tools, ROMs,
OS install media or guest disk images as part of a Sherclawk release.
