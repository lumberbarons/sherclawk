# host-tls — Certainly on the host

A debug tool, not part of the app build. It links Certainly's core TLS
stack against a BSD-socket implementation of its `OTTransport` interface
(`host_transport.c`) plus small shims for the Toolbox calls the library
makes (`shim/`, `host_shim.c`), so handshakes and post-handshake
behaviour can be reproduced on macOS without the Mac OS 9 VM.

```
./build.sh
./host-tls google.com /
HOST_TLS_DUMP=1 ./host-tls postman-echo.com /get
```

Diagnostics (state transitions, errors, transport flags) go to stdout
along with the server's response; `HOST_TLS_DUMP=1` hex-dumps every
send/recv to stderr.

Note: the shims are faithful only where it matters for this debugging —
the socket transport emulates OT's `T_ORDREL`/`T_DISCONNECT` semantics
closely, but the entropy and timer stubs just need to be plausible.