# Single-server MCP implementation

The protocol core and native diagnostic for #76 are implemented.
MCP is not yet exposed in the Sherclawk application. The next gate is a real
OS 9 Tavily diagnostic run, followed by the configuration editor, agent
integration and release acceptance. The implemented diagnostic does not
establish that those application-integration gates have passed.

## Protocol diagnostic

`SherclawkMCPCheck` reads the existing UTF-8 configuration from
`System Folder:Preferences:Sherclawk MCP Servers.json`. It does not create or
modify that file. Install this file privately in the guest, replacing the
placeholder locally:

```json
{
  "mcpServers": {
    "tavily": {
      "url": "https://mcp.tavily.com/mcp/",
      "headers": {
        "Authorization": "Bearer YOUR_TAVILY_API_KEY"
      },
      "tools": ["tavily_search", "tavily_extract"]
    }
  }
}
```

[Tavily documents header authentication](https://docs.tavily.com/documentation/mcp).
Never commit the populated file or put its contents in an issue, transcript or
log. This diagnostic omits response bodies, schemas, headers and session IDs
from its log. It reports only fixed milestones and counts.

```sh
./build.sh SherclawkMCPCheck_APPL
SHARE_HOST=<afp-host> APP=SherclawkMCPCheck tools/deploy-to-share.sh
# Launch SherclawkMCPCheck in OS 9, then read Retro68:SherclawkMCPCheck.log.
ssh "$SHARE_HOST" 'cat /srv/retro68/SherclawkMCPCheck.log'
```

Expected milestones are `PASS configuration`, `PASS TLS initialize` (including
negotiated version, discovery counts), `PASS Tavily search`, and `END PASS`.
Escape requests Stop. Outstanding OT connects drain cooperatively before their
contexts are freed (the existing #34 crash window); the TLS library owns its
connect timeout. No new tool request is launched during this cleanup. Closing a connection does not prove cancellation of
server-side work; cancellation notifications are best effort, with no replay.
Search forwards a fixed diagnostic query, five results, basic depth, and
images/raw content disabled. The production adapter forwards arguments rather
than silently rewriting them.

The client implements the initialization-based versions `2025-11-25`,
`2025-06-18` and `2025-03-26`, following the
[MCP lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)
and [Streamable HTTP transport](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports).
It offers the newest of those versions. It supports JSON and incremental SSE
POST responses, chunked HTTP, multiline data, keepalives, response ID matching,
session headers, cursors, ping replies and unsupported-server-request errors.
It uses separate POSTs for server-request replies while keeping the originating
stream alive. There is no GET stream, resumption, retry, redirect, compression,
stdio, OAuth, resource fetching or server-instruction propagation.

An empty `mcpServers` disables the client. Exactly one server is otherwise
allowed. `url` is required; `headers` and `tools` are optional. An empty `tools`
selects nothing, while omission selects eligible tools up to the fixed limits.
Unknown fields, duplicates (including escaped spellings), non-HTTPS URLs,
user information, fragments, injection and transport-owned headers are
rejected. Header values use visible ASCII; secrets in JSON may use escapes.
Server/tool names use ASCII letters, digits and underscores. URLs use DNS/IPv4
hosts; IPv6 literals are not supported by this adapter. Server names are at
most 31 bytes and mapped tool names at most 63 bytes.

At the exact official Tavily endpoint only `tavily_search` and `tavily_extract`
are eligible. Other endpoints need `readOnlyHint: true` plus selection. Such
annotations are server claims, not proof of safety. Native schemas are not yet
combined with these schemas. Unsupported individual tools are skipped with a
registry notice; ambiguous or over-budget discovery clears the whole registry.

## Remaining implementation and acceptance

- Prove guest TLS, initialization, discovery and a search against Tavily.
- Add the scrolling MacRoman/CR editor and strict UTF-8/LF conversion; preserve
  readable invalid files and refuse unsafe overwrites.
- Add stage/flush/read-back/publication verification, first-create rename and
  replacement exchange, with persistence fault tests and identity protection
  from all model-facing file tools.
- Integrate frozen per-run discovery, schemas and remote calls into the event
  loop, history/journal notices, native-only fallback and Stop.
- Make JSON parsing, duplicate validation and schema/result processing
  resumable within the 8 KiB parsing budget. The diagnostic currently bounds
  I/O per step but parses each completed message synchronously.
- Redact decoded credential values and JSON-escaped reflections across all
  model-visible schemas, results and server messages. The diagnostic's
  body-free logging avoids that exposure; its plain-text redaction helper
  alone is not sufficient for production result retention.
- Add immutable verified result artifacts, session-scoped IDs and
  `read_mcp_result`, with UTF-8/escaped-output pagination and fault tests.
- Demonstrate editor save/cancel/restart/scrolling/paste/errors, model-triggered
  search with source links, extraction and multi-page results, failed-discovery
  fallback, responsive Stop without replay, and absence of credentials.
- Measure the integrated linked footprint and test the guest minimum partition.

## Verification evidence (2026-10-08)

The ASan/UBSan host suites (`tools/check.sh`), patched TLS transport suites,
worker protocol tests, and lint gate (shellcheck, cppcheck, ruff) passed.
The new protocol and cooperative-client fixtures also passed host GCC in the
Retro68 image with `-Wall -Wextra -Werror`. Docker builds passed for both
`Sherclawk_APPL` and `SherclawkMCPCheck_APPL`.

`powerpc-apple-macos-size` measured the diagnostic at 339,968 bytes text,
6,624 data and 1,190,632 bss: 1,537,224 bytes total. The main application's
linked total remains 3,036,744 bytes. These figures exclude TLS/UI heap and
stack allocations; they do not establish guest minimum-partition acceptance.
No guest/live Tavily result has been recorded yet.
