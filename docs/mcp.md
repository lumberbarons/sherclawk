# Single-server MCP implementation

The protocol core and native diagnostic for #76 are implemented, and the
application has a configuration editor (**Edit ▸ MCP Servers…**). The agent
run loop uses the configuration: every run loads it before the first model
request, discovers the server's eligible tools, freezes the admitted schemas
into the request advertisement and dispatches model-selected remote calls
cooperatively (see [Agent integration](#agent-integration)). Completed,
redacted responses are retained as immutable artifacts and paged back with
`read_mcp_result`. The client redacts credentials from everything it would
hand on (see [Credential redaction](#credential-redaction)). The OS 9.2.2 Tavily diagnostic gate passed on October 10,
2026 (see [guest evidence](history/verification.md#mcp-tavily-diagnostic-october-10-2026)).
Integrated guest acceptance remains a separate gate.

## Configuration editor

**Edit ▸ MCP Servers…** opens a modal dialog with the contents of
`System Folder:Preferences:Sherclawk MCP Servers.json` in a scrolling
TextEdit pane (Monaco 9, Cut/Copy/Paste/Clear/Select All through the Edit menu
or Command keys). The item is refused while a run or a stopped lookup is still
closing, like Preferences. Return inserts a newline; Enter saves; Escape and
Command-. cancel and discard.

- **Opening.** With no file the editor shows `{"mcpServers": {}}` (disabled).
  A readable file is shown even when it is invalid, so it can be fixed. A
  file that is over 8 KiB, unreadable, or not representable as MacRoman is not
  opened (an editor that cannot show a file must not be able to overwrite it),
  and neither is a folder holding only `Sherclawk MCP Servers.json.old` from an
  interrupted save; the alert says what to rename.
- **Saving.** The text is converted from MacRoman/CR to UTF-8/LF and must be
  accepted by the same `mcp_config_parse` the client uses. A rejected save
  shows the parser's message, which names a field and never a value, keeps the
  dialog open and writes nothing. Editor text is limited to 8 KiB; MacRoman
  characters that need more bytes in UTF-8 are checked after conversion.
- **Persistence.** The new bytes are staged as `….json.new` in the same
  folder, flushed, read back and compared. They replace the file by renaming
  the old one to `….json.old`, renaming the stage in and reading the result back
  again; any failure after the first rename puts the old file back. A failure
  before the first rename leaves the previous file untouched. If restoring also
  fails, the previous file stays as `….json.old` and the alert says so. After a
  successful replace the `.old` file is deleted, since it holds the same
  credentials. `FSpExchangeFiles` is not used: it is unverified on AFP shares.
- **Credentials.** The file and the editor show header values in clear text,
  as Preferences does for the OpenRouter key. The status line, alerts and logs
  carry only fixed text and the parser's field names, and the editor's buffers
  are wiped when it closes.

The save path is covered by host fault tests (`tests/test_mcp_store.c`) and the
text and validation rules by `tests/test_mcp_editor.c`.

## Configuration protection

The workspace confines the file tools, but a workspace may contain the
Preferences folder.
`mcp_guard.c` therefore protects `Sherclawk Preferences`, which holds the
OpenRouter key, and the configuration with its `….json.new` and `….json.old`
siblings by catalog identity: the Preferences folder's volume and
directory ID from `FindFolder`, plus the leaf name compared without case.
`tools_resolve`, `list_files`, `search_text` and `resolve_alias` consult it, so
every model-facing read, search, listing, alias, resource, image, create, edit,
folder and Trash path is refused or skips the file. The editor and the
diagnostic are not model-facing and use their own paths. A refusal is code
`PROTECTED` with `os_error` 30001 and fixed text with no path or credential; an
unlocatable Preferences folder refuses every guarded call. The contract for
tool callers is in [tools](tools.md#protected-credential-files), following
[ADR-0003](adr/0003-admit-tools-read-only-first.md); the host fixtures are in
`tests/test_tools.c`, and `SherclawkMCPGuardCheck` exercises the real Preferences
folder in the guest (see [evidence](history/verification.md#mcp-configuration-guard-october-10-2026)).

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
negotiated version, discovery counts), `PASS Tavily search` (including a count
of HTTP/HTTPS result URLs), and `END PASS`. Non-empty content alone does not
prove a search: the diagnostic requires result URLs in Tavily's JSON results
or formatted detailed results, without logging their values.
Escape requests Stop. Outstanding OT connects drain cooperatively before their
contexts are freed (the existing #34 crash window); the TLS library owns its
connect timeout. No new tool request is launched during this cleanup. Closing a connection does not prove cancellation of
server-side work; cancellation notifications are best effort, with no replay.
The Stop log records the client phase, TLS state, whether a connect was
outstanding, elapsed drain ticks, remaining contexts and whether the request ID
changed. `tls_state=1` is connecting and `tls_state=2` is handshaking. An
intentional stop ends with `FAIL stopped` and `END FAIL`; its separate
`PASS Stop drained` milestone records completed cleanup. The TLS 1.3 handshake
abort skips BearSSL's uninitialized runner; TLS 1.2 fallback retains its
graceful close.

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

## Credential redaction

A server can reflect what it was sent: a header value in an error, the session
ID in a description, the API key inside a JSON document echoed as tool content.
Everything the client keeps for the model is redacted first, in place and
length-preserving (`*` for each replaced byte), by the resumable `McpRedact`
in `mcp_config.c`.

**What counts as a credential.** Every configured header value, and the part of
it after the first space (so `Bearer abc` also redacts `abc`), every query
value of at least eight characters, and the session ID. Values shorter than
that in a query are left alone because redacting them would mangle unrelated
text; a short *header* value is still redacted wherever it appears, so give
headers distinctive values.

**Which spellings.** Plain text; the credential as a JSON string spells it
(`\"`, `\\`, `\/`, or `\u00XX` in either hex case for any character); and, in
raw JSON, as a JSON document inside a string spells it (the same escaping
applied twice). Other encodings (URL, base64, case changes, text split by the
server) and deeper nesting are not recognised. This is protection against
reflection, not against a server that is trying to smuggle the key out.

**Where it applies.**

| Text | Treatment |
|---|---|
| Tool descriptions | Decoded, redacted as text, then quoted. |
| Input schemas | Copied verbatim, then redacted as raw JSON before they are published: nested descriptions, defaults, enums and property names. |
| Tool names | A name containing a credential is skipped with a registry notice; a name cannot be rewritten because the call uses it verbatim. |
| Call results and errors | `MCP_STAGE_CALL` rewrites the `result` or `error` member of the response in the stream's message buffer, so `mcp_client_response` never holds the original. Server errors, tool content, structured content and `isError` payloads are all inside that member. |
| Server instructions, `serverInfo`, notifications, server requests | Never retained or shown; instructions are not propagated. |
| Retained results | `mcp_result.c` decodes `text` content into a UTF-8 artifact and re-runs `McpRedact` over the decoded text before it is staged, so a wiring mistake cannot retain a credential either; the input response must be the already-redacted `mcp_client_response` text. |

Raw JSON is searched only inside strings and a match is made of whole escape
sequences, so a redacted message has exactly the same tokens at the same
offsets and never gains or loses a quote. Object keys are strings too, so a
credential used as a key is replaced there as well. The classification and the
`isError` flag are read before the rewrite. `mcp_client_response` is empty
unless the call completed, so a Stop or the call deadline during the rewrite
never exposes a partly redacted message, and closing the client clears the
buffer. The envelope (`jsonrpc`, `id`) is outside the rewritten member.

Cost is message length times the credentials times the spellings searched, in
steps of `MCP_WORK_BUDGET` units; see [limits](limits.md#mcp-diagnostic-limits-mcph-mcp_clienth).
The fixtures are `tests/test_mcp_redact.c` (against an independent reference
matcher, with decode-and-search oracles for escaped and nested spellings) and
the reflection case in `tests/test_mcp_client.c`.

## TLS trust anchors

Certainly bakes a fixed set of root CA certificates into the build; they are
generated by `tools/generate_ca_roots.sh` in the Certainly clone and cover a
small set of public issuers. An MCP server whose chain ends at a root outside
that set fails the TLS handshake until the set is extended and Certainly is
rebuilt. When adding a server, check its issuing root first. Authorization
servers add a second host with its own chain.

## Agent integration

Every run loads the saved configuration before its first model request. With
no file, a disabled configuration, an unreadable file or an invalid one, no
MCP traffic happens at all: the run continues with the native tools and the
session journal records a fixed-text `mcp_notice` (never a host, path, header
or credential value). An enabled configuration starts discovery
(`initialize`, the `tools/list` pages) cooperatively from the event loop,
under the client's own 30-second deadline; a failed or over-budget discovery
records a sanitized notice and the run continues natively.

Discovery is frozen per run. The admitted mapped names
(`mcp_<server>_<tool>`) and their redacted schemas are re-validated —
structure, name uniqueness, no native-name collisions, at most eight tools
and 8 KiB of schema text inside the request budget — and then merged after
the native schemas in the `tools` array of every request of that run (see
[limits](limits.md#frozen-remote-schemas-agenth)). A set that fails
validation is refused whole: nothing is advertised and the run continues
natively with a notice. A successful freeze journals one `mcp_discovery`
record with the admitted count and schema bytes; skipped tools keep their
sanitized registry notice.

The model selects a remote tool by its exact mapped name. Calls are
sequential — one exchange at a time, in call order — and the arguments are
forwarded verbatim; the only rewrite is the mapped name back to the server's
original. A completed response is redacted in place by the client, retained
as a verified artifact, and the retention summary (with `result_id`, source
links and error status) is the tool result; the model pages the artifact with
`read_mcp_result` (see [tools](tools.md#retained-remote-mcp-results)). A call
that cannot start (an earlier call failed or was stopped) or that fails
mid-flight sends nothing further and returns an explicit `MCP_UNAVAILABLE`
error result; the run continues, and the frozen advertisement stays for the
rest of the run.

Stop during discovery, a call or result publication stops the client, sends
its best-effort cancellation notification, drains any outstanding OT connect
cooperatively from the event loop — Open Transport teardown waits behind
that drain — and clears the frozen advertisement, so no further remote call
is dispatched. The in-flight call's result says the call was stopped, its
outcome is unknown and it is never replayed; unstarted calls record
interrupted results like native tools. The MCP editor, a new send and a
handoff wait until the drain finishes. A run's end — completion, Stop or New
Chat — closes the client and wipes its copy of the configuration
credentials; the next run re-discovers from the file.

## Remaining implementation and acceptance

- Demonstrate model-triggered search with source links, extraction and
  multi-page results, failed-discovery fallback, responsive Stop without
  replay, and absence of credentials in model-visible output in the OS 9.2.2
  guest.
- Measure the integrated linked footprint against the guest minimum
  partition (the host-linked app measured 7,562,128 bytes on 2026-10-11; see
  [limits](limits.md)).

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
Guest/live Tavily evidence was subsequently recorded on October 10, 2026:
configuration, TLS initialize (`2025-11-25`), discovery (one page, five entries,
two eligible tools), five search result URLs and `END PASS`. Escape during
an outstanding connect and during the TLS 1.3 handshake drained safely after
fixing the handshake close path. This does not establish agent integration,
server-side cancellation or minimum-partition acceptance. See the
[attributable build and guest evidence](history/verification.md#mcp-tavily-diagnostic-october-10-2026).
