# 003 — Sherclawk MCP client and an OS 9 OAuth library

**Status:** researched, not built. Nothing here has been compiled or run in the
guest. Findings come from reading Sherclawk's source, cloning the repositories
named below, and web searches (October 2026). Items marked *unverified* come
from search summaries or documentation only.

## The idea

Give Sherclawk an MCP client so the model can call tools exposed by MCP
servers, and build the OAuth 2.1 support that authenticated servers need as a
**separate, reusable OS 9 library** — the same relationship Sherclawk has with
Certainly (TLS). The MCP client calls the library when a server answers `401`.

## What Sherclawk does today

- `agent.c` already speaks tool calling: `agent_tool_schemas()` returns a
  function-schema array, `agent_response` parses `tool_calls`, calls run
  sequentially, and results are bounded and journaled. MCP tools fit behind the
  same interface: `tools/list` contributes schema entries and `tools/call`
  becomes another dispatch target beside `tools_execute`.
- `network.c` is hardwired to `openrouter.ai:443` (`MacTLS_Create`) and steps one
  cooperative HTTPS exchange per Toolbox tick. An MCP endpoint needs host, port
  and path to become per-request parameters, and a second exchange alongside the
  model call.
- Certainly bakes a closed set of root CAs into the build
  (`tools/generate_ca_roots.sh`). An MCP or authorization server whose chain
  ends at a root outside that set fails TLS until the set is extended.
- Limits that will bite: tool results are capped (`AGENT_RESULT_CAP`), the
  request is capped (`CHAT_REQUEST_CAP`), the schema array is a static string,
  and `AGENT_TOOL_MAX` bounds the tool count. Real MCP servers have verbose tool
  descriptions and large results, so descriptions need trimming and results
  need truncation or pagination.
- `PLAN.md` lists MCP under "not in this pass".

## MCP client

**Transport.** Streamable HTTP only. Stdio needs subprocesses; legacy HTTP+SSE
needs a long-lived stream. Streamable HTTP is mostly POST with JSON-RPC and a
JSON reply; a server may answer with `text/event-stream`, so a small SSE frame
parser is needed eventually.

**Work involved:**

- Per-request host/port/path in the network layer.
- The `initialize` handshake, `notifications/initialized`, and the
  `Mcp-Session-Id` header.
- `tools/list` mapped into the schema array, with description trimming.
- `tools/call` dispatch with result truncation.
- Server configuration (URL, optional token), either in `config.local.h` or the
  planned Preferences dialog ([002](idea-002-sherclawk-preferences.md)).
- Trust-anchor additions for each server's root CA.

**Safety.** MCP results are untrusted data, which the existing policy already
states. Remote tools cannot offer the create-only and recovery guarantees the
file tools have, so a mutating remote tool needs an explicit user confirmation
before it runs. The app has no confirmation step today.

**Suggested staging:**

1. Static bearer token (or none), one fixed server, JSON replies only. This
   proves discovery and calls, and many servers accept personal access tokens.
2. SSE reply parsing and session IDs.
3. OAuth, via the library below.

## OAuth 2.1 library

**Separation.** The library knows how to obtain and refresh a token for a
resource URL. The MCP client knows when to ask: on a `401` with
`WWW-Authenticate`, after mapping the server URL to the `resource` parameter,
then retrying the request.

**Generic pieces (library):**

- Authorization code with PKCE (RFC 7636), S256 only.
- Token exchange, refresh, expiry handling, error parsing.
- Authorization-server metadata (RFC 8414) and protected-resource metadata
  (RFC 9728).
- Resource indicators (RFC 8707) and issuer identification (RFC 9207: validate
  `iss`, compare `state`, exact redirect URI).
- Client registration: client ID metadata documents (CIMD), pre-registration,
  and dynamic registration (RFC 7591).
- Optionally the device grant (RFC 8628), which suits OS 9 best because it needs
  no redirect or local listener. The conformance suite does not test it, and
  many MCP servers may not offer it (*unverified*).

**Hard parts on OS 9:**

- **Entropy.** There is no hardware RNG. PKCE verifiers and `state` need real
  randomness. ClassicNet's design notes call its own seeding "mediocre" and
  expose `CN_TlsAddEntropy()` so the app can feed in user-input timing. Copy
  that shape: the library takes an app-supplied entropy source and documents the
  floor.
- **The user step.** Sign-in happens in a browser. Options are the device grant,
  a pasted authorization code, or a loopback listener inside a cooperative app.
- **Token storage.** Tokens would sit in Preferences on a system with no memory
  protection. The existing apps accept this for revocable tokens; document it.
- **Retry versus the journal philosophy.** Refresh-on-401 is a retry; it must not
  silently repeat a non-idempotent tool call.
- **Several hosts.** The authorization server and resource server are often
  different hosts, each needing its own trust anchors.
- **Registration moved.** The MCP documents I found (2026-07-28 revision) say
  dynamic registration is deprecated in favour of CIMD. CIMD makes the
  `client_id` an HTTPS URL for a JSON document, so the app needs a document
  hosted somewhere reachable. Pre-registration or a pasted token are the
  practical alternatives for a hobby app. Re-check the current spec before
  choosing.

**Interfaces.** Keep the library transport-agnostic: a pluggable non-blocking
HTTPS exchange so it runs on Certainly (BearSSL, used by Sherclawk) or
ClassicNet (mbedTLS). The JSON reader and HTTP helpers already in Sherclawk and
HelloHTTPS cover most of the parsing.

## Prior art

Nobody appears to have built an OAuth client for Classic Mac OS.

- **[ClassicNet](https://github.com/yllan/ClassicNet)** — TLS 1.2/1.3, HTTP/1.1,
  HTTP/2 and WebSocket over Open Transport and mbedTLS; Apache-2.0; single
  author; fuzz suite; async `CNTransport` design. I found no OAuth, PKCE, DPoP or
  SHA-256 helpers (it has SHA-1 and Base64). Transport only.
- **[Palaeomastodon](https://github.com/yllan/Palaeomastodon)** — Mastodon
  client built on ClassicNet. The user creates an access token in the instance's
  web UI and pastes it in; the app sends `Authorization: Bearer`. There is no
  authorize/PKCE/refresh flow.
- **[PlatinumSky](https://github.com/yllan/PlatinumSky)** — Bluesky client. Handle
  plus app password to `com.atproto.server.createSession`, then persists the
  refresh JWT and renews with `refreshSession`. That is Bluesky's own session
  scheme, not OAuth, and there is no DPoP.
- **[macstodon](https://github.com/smallsco/macstodon)** — Classic Mac Mastodon
  client written in Python; not a reusable C library.
- Both yllan apps run network jobs on a Thread Manager thread with a 1 MB stack
  because the mbedTLS handshake overflows the default stack and fails with
  misleading entropy errors. Relevant if the library ever sits on mbedTLS;
  Sherclawk's Certainly path steps cooperatively from the main loop instead.

The ecosystem stops at pasted tokens, which is the cheap fallback here too.

## Testing

No application-agnostic conformance suite for OAuth 2.x *clients* turned up. The
OpenID Foundation's [conformance suite](https://gitlab.com/openid/conformance-suite/)
has plans where it acts as the server (OpenID Connect and FAPI relying-party
tests), but it targets OIDC and the strict banking profile (ID tokens, JWT
validation), and I could not confirm a plain OAuth client plan. Worth reading
its plan list before ruling it out.

**MCP conformance suite — the best available.**
[modelcontextprotocol/conformance](https://github.com/modelcontextprotocol/conformance)
starts a mock MCP server and mock authorization server, runs your client as a
command with the server URL appended and `MCP_CONFORMANCE_SCENARIO` set, and
checks the requests it makes:

```bash
npx @modelcontextprotocol/conformance client --command "./your-client" --suite auth
```

Its auth scenarios cover metadata discovery variants and PRM priority order,
CIMD, pre-registration, token endpoint auth methods, scope handling and
step-up, resource mismatch, offline access and refresh, a set of `iss`
scenarios (including rejecting a missing or wrong issuer), authorization-server
migration, client credentials and DPoP. A `draft` suite targets the in-progress
spec, and `examples/clients/typescript/auth-test.ts` is a well-behaved
reference client.

It tests MCP's *profile* of OAuth, not OAuth alone, and it drives a complete MCP
client. A failure could lie in either layer. To use it, build the OAuth
library, a minimal MCP client (`initialize`, one request, call the library on
`401`, retry), and a host CLI wrapping both. The suite runs on the Mac, so it
cannot exercise the guest or the TLS stack; its mock servers use localhost.
The OS 9 side (entropy, the non-blocking state machine, TLS to a real server,
the sign-in step) is a separate check, and per AGENTS.md host-side success does
not prove guest behaviour. The library needs an "open this URL, give me the
callback" hook the test CLI can fill in headlessly.

**Other tests for the library alone:**

- Unit tests from the RFC 7636 Appendix B PKCE example and NIST SHA-256
  known-answer vectors.
- Recorded JSON fixtures for metadata and token responses.
- Negative cases written by hand: wrong `state`, wrong `iss`, downgrade to
  `plain` PKCE, unregistered redirect URI.

### Mock authorization servers

Checked in source (cloned and grepped):

| Server | PKCE | AS metadata (8414) | Protected-resource metadata (9728) | Registration | `iss` (9207) | `WWW-Authenticate` |
|---|---|---|---|---|---|---|
| [navikt/mock-oauth2-server](https://github.com/navikt/mock-oauth2-server) | yes | yes | no | no | no | no |
| [axa-group/oauth2-mock-server](https://github.com/axa-group/oauth2-mock-server) | yes | OIDC discovery only | no | no | no | no |

Both suit plain code + PKCE + refresh. Neither covers the MCP profile.

From documentation only (*unverified* against source):

- **[rust-mcp-stack/oauth2-test-server](https://github.com/rust-mcp-stack/oauth2-test-server)**
  — MIT, Rust/Axum, `cargo run` standalone. The only one that says it is built
  for MCP clients and servers: dynamic registration, PKCE, refresh, client
  credentials, introspection, revocation. Docs list OIDC discovery but not
  `/.well-known/oauth-authorization-server`, protected-resource metadata, RFC
  8707, RFC 9207, the device grant or CIMD. PKCE accepts `plain`, and S256-only
  enforcement is not documented, which matters because OAuth 2.1 forbids
  `plain`.
- **Keycloak** — dynamic registration and experimental CIMD; documents that it
  cannot recognise the `resource` parameter. Heavy to run.
- **Ory Hydra** — dynamic registration, no resource indicators.
- **Zitadel** — no dynamic registration at the time of the source.
- **MCP SDK examples** — python-sdk `examples/servers/simple-auth` (separate AS
  and RS) and [wille/mcp-oauth-server](https://github.com/wille/mcp-oauth-server)
  (built on the TypeScript SDK's authorization-server code). Not read.
- **MockServer** — scriptable, useful for injecting specific failures.

The most MCP-aware mock is inside the conformance repo
(`src/scenarios/client/auth/helpers/createAuthServer.ts`): AS metadata at
`/.well-known/oauth-authorization-server`, authorize, token, register, S256
checks, DPoP and `iss` handling, with per-scenario misbehaviour. It is an
Express app the runner starts per scenario, not a standalone server.

**Recommended mix:** the conformance suite for profile and negative cases,
`oauth2-test-server` or navikt for quick local flows while developing,
hand-written refusal tests for S256-only and other rejections the mocks do not
enforce, and a guest-side check on real hardware paths.

## Rough effort

Estimates from reading the code, not from building anything.

- MCP client with a static token: moderate — roughly a week or two of focused
  work, dominated by generalising the network layer, the schema and result
  budgets, and the confirmation step for mutating tools.
- OAuth library: larger — plausibly a few weeks, mostly the non-blocking state
  machine, entropy handling, the user-facing sign-in step and token storage,
  plus the discovery and registration chain. The cryptography itself is small
  because BearSSL provides SHA-256.

## Open questions

- Which MCP servers are the actual targets? If they accept personal access
  tokens, the OAuth library may not be needed to get value.
- Do those servers support the device grant, or is paste-the-code the only
  workable sign-in?
- Is CIMD feasible (somewhere to host the client document), or is
  pre-registration enough?
- Does the OpenID Foundation suite have a plain OAuth client plan?
- Certainly or ClassicNet underneath, if the library is to be shared?

## Sources

- [ClassicNet](https://github.com/yllan/ClassicNet),
  [Palaeomastodon](https://github.com/yllan/Palaeomastodon),
  [PlatinumSky](https://github.com/yllan/PlatinumSky),
  [macstodon](https://github.com/smallsco/macstodon)
- [Connect your MacOS 9 to OSM, BlueSky and Mastodon](https://tinkerdifferent.com/posts/48303/)
- [modelcontextprotocol/conformance](https://github.com/modelcontextprotocol/conformance)
- [OpenID Foundation conformance suite](https://gitlab.com/openid/conformance-suite/)
- [oauth2-test-server](https://docs.rs/oauth2-test-server),
  [navikt/mock-oauth2-server](https://github.com/navikt/mock-oauth2-server),
  [axa-group/oauth2-mock-server](https://github.com/axa-group/oauth2-mock-server),
  [MockServer](https://mock-server.com/mock_server/mocking_oauth2.html)
- [Keycloak MCP authorization](https://www.keycloak.org/securing-apps/mcp-authz-server),
  [Best providers for MCP authentication (WorkOS)](https://workos.com/blog/best-mcp-server-authentication-providers),
  [Understanding Authorization in MCP](https://modelcontextprotocol.io/docs/2026-07-28/tutorials/security/authorization)
