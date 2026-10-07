# host-tls — Certainly on the host

Host-side support for the debug and test targets, not part of the app build.
It links Certainly's core TLS stack against a BSD-socket implementation of its
`OTTransport` interface (`host_transport.c`) plus small shims for the Toolbox
calls the library makes (`shim/`, `host_shim.c`), so handshakes and
post-handshake behaviour can be reproduced on macOS without the Mac OS 9 VM.

The sources are consumed by the host build steps: `tools/build-host-probe.sh`
links `host_transport.c` and `host_shim.c` into `build/host-probe`, and the
`shim/` headers are on the include path of `tools/check.sh` and
`tools/check-transport.sh`.

```sh
tools/build-host-probe.sh          # build build/host-probe
build/host-probe                   # fixed agent probe against OpenRouter
HOST_TLS_DUMP=1 build/host-probe   # hex-dump every send/recv to stderr
```

`build/host-probe` runs a fixed model/tool conversation and needs
`SHERCLAWK_API_KEY` in `config.local.h`; it is not a generic host/URL client.
The probe logs to stdout; `HOST_TLS_DUMP=1` hex-dumps every send/recv to
stderr.

Note: the shims are faithful only where it matters for this debugging —
the socket transport emulates OT's `T_ORDREL`/`T_DISCONNECT` semantics
closely, but the entropy and timer stubs just need to be plausible.