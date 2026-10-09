# Limits and how they interact

Sherclawk has no heap for conversation data: every buffer is static and sized
by a constant. Changing one constant usually breaks an assumption another one
relies on, so read this before touching any of them. Values below are the ones
in the headers today; the headers win if this drifts.

## Where each limit lives

### Model request and response (`agent.h`, `chat.h`)

| Constant | Value | Bounds |
|---|---|---|
| `AGENT_MAX_TOKENS` | 6000 | `max_tokens` on every agent request. Reasoning tokens count against it. |
| `AGENT_HANDOFF_MAX_TOKENS` | 1536 | `max_tokens` on the handoff summary request. |
| `CHAT_RESPONSE_CAP` | 64 KiB | One raw HTTP response (`ChatNetwork.raw`), its de-chunked body, and the `message` scratch in `agent_response`. |
| `AGENT_REPLY_CAP` | 40 KiB | Visible text of one reply (`Agent.text`). |
| `AGENT_ARGUMENT_CAP` | 8 KiB | Arguments of one tool call. |
| `AGENT_CALL_MAX` | 4 | Tool calls accepted from one response. |
| `AGENT_HISTORY_CAP` | 384 KiB | Recorded conversation JSON (`Agent.history`). |
| `CHAT_REQUEST_CAP` | 416 KiB | The whole request JSON (`gJSON`): history plus system prompt and tool schemas. |
| `AGENT_TEXT_CAP` | 16 KiB | User prompt and handoff message buffers. Not reply text. |
| `AGENT_RESULT_CAP` | 1536 | One tool result recorded into history. |
| `AGENT_IMAGE_CAP` | 128 KiB | Largest PNG `view_image` reads (`view_image.c` holds one in a static buffer). Its base64 form, a third larger, rides in one request. |
| `AGENT_IMAGE_NOTE_CAP` | 2048 | History reserved for the note that accompanies an image, whenever a response calls `view_image`. |
| JSON token scratch | 4096 tokens | Tokens (not bytes) in any parsed response; `tokens[]` in `agent.c`. A catalog page over the cap parses to zero rows. |

### Display and time

| Limit | Value | Bounds |
|---|---|---|
| `CHAT_TRANSCRIPT_CAP` | 30,001 bytes | The TextEdit transcript. Classic TextEdit stops at 32,000 bytes, so this cannot simply be raised. A reply too long for it is journaled but shown as a "see the session file" placeholder. |
| Request deadline | 120 s | One HTTPS exchange (`StepModelExchange`). |
| Model catalog deadline | 30 s | The model context-window query and each Preferences catalog fetch (popular page, Find, OK validation). Dragging the dialog restarts it. |
| Catalog page rows | 10 | `AGENT_MODEL_ROWS_MAX`; the popular, search and validation queries all send `limit=10`. |
| Rounds / tool calls per run | 32 / 64 default, 1–128 in Preferences | A run pauses at either; sending a message resets them. |

### Memory partition (`hello.r`)

`SIZE` asks for 8 MiB preferred and 4 MiB minimum. The static data below counts
against it, along with dynamic TLS and UI allocations.

## How they relate

1. **Response bytes ≥ `AGENT_MAX_TOKENS` × bytes per token.** A completion can
   never exceed `CHAT_RESPONSE_CAP` or the round fails. Measured on the guest:
   about 5.8 bytes per token for visible text (3072 tokens → 17,885 bytes) and
   about 9.9 for a reasoning-heavy reply (3072 tokens → 30,347 bytes). So the
   budget is `max_tokens` × 10 ≤ `CHAT_RESPONSE_CAP`; 6000 → about 59 KB fits
   64 KiB, 8192 → about 81 KB does not.
2. **`AGENT_REPLY_CAP` ≥ the longest visible reply, < `CHAT_RESPONSE_CAP`.**
   Visible text is part of the response, so it is bounded by invariant 1; the
   reply cap sizes the copy in `Agent.text`. A complete reply longer than it is
   rejected as malformed. The old 16 KiB value was already at its edge at 3072
   tokens.
3. **`CHAT_REQUEST_CAP` = `AGENT_HISTORY_CAP` + overhead.** The overhead is the
   system prompt and tool schemas (about 13 KB today, with room to 32 KiB).
   Raising history means raising the request cap and the HTTP request buffer in
   `network.h`, which is `CHAT_REQUEST_CAP` plus header room.
4. **A cut-off reply is a model round.** At the token limit the reply is
   discarded and the model is told to retry, which consumes one of the run's
   rounds; the round limit is what bounds repeated truncation. A complete reply
   with a call whose arguments exceed `AGENT_ARGUMENT_CAP`, or with more than
   `AGENT_CALL_MAX` calls, is discarded the same way (`end=discarded`); the
   per-call argument cap sits above the 4096-byte text limit of `write_text` and
   `edit_text` to leave room for JSON escaping, so a model that overshoots the text
   limit by a little gets a tool error, and one that overshoots the argument cap
   gets the retry notice.
5. **One response must fit in the remaining history.** `agent_response`
   refuses a reply that would not fit with room for its tool results.
   Reasoning fields are recorded with the message, so a reasoning-heavy round
   can add tens of KB to history.
6. **Round time ≈ `max_tokens` ÷ generation speed.** Measured about 107 tokens
   per second on a fast model (3072 tokens in 28.7 s), so 6000 tokens is about
   56 s against the 120 s deadline. A slower model shrinks that margin.
7. **A catalog page must fit `CHAT_RESPONSE_CAP` and the JSON token scratch.**
   The Preferences chooser reads ten rows per fetch through the same
   network buffers and parser as a model reply. Raising the row count means
   re-checking both limits. An empty popular page is treated as a failed
   fetch and is not cached; an empty search or validation page is the
   catalog's "no match". The 30-second deadline is shared with the
   context lookup, and a timed-out fetch drains before it closes, in the dialog
   or (after Cancel) in the main loop.
8. **An image rides in one request on top of the history.** The encoded image
   (`AGENT_IMAGE_CAP` bytes become 4/3 as many characters) is spliced into the
   request that follows `view_image` only, so the request must hold history,
   system prompt and schemas, and the image at once: with a 128 KiB PNG that
   leaves about 230 KB of history, not the full 384 KiB. `agent_attach_image`
   checks this when it records the note and, if the image would not fit, says
   so in the note instead of sending it. Later rounds and handoff requests
   carry the note alone, so images never occupy history. Raising
   `AGENT_IMAGE_CAP` shrinks that room one for one and needs the request buffer
   re-checked.
9. **Every round re-uploads the full history.** History size is therefore also
   per-round upload time, and bigger history only helps up to the model's own
   context window (the Context line in the status area).

## Memory cost per byte of cap

| Constant | Buffers it sizes | Static bytes per byte of cap |
|---|---|---|
| `CHAT_RESPONSE_CAP` | `raw`, `body`, `agent_response` `message` | 3 |
| `AGENT_HISTORY_CAP` | live history, handoff candidate history, `gJSON`, HTTP request | 4 |
| `AGENT_REPLY_CAP` | `Agent.text` in the live and candidate `Agent` | 2 |
| `AGENT_TEXT_CAP` | prompt buffer; handoff content (once) and message (six times) | 8 |
| `AGENT_IMAGE_CAP` | the `view_image` PNG buffer | 1 |
| `AGENT_MAX_TOKENS` | none | 0 |

Linked static size is measured, not estimated, with `powerpc-apple-macos-size`
on `build/Sherclawk.xcoff` in the Retro68 image:

```bash
docker run --rm -v "$PWD/build:/b" ghcr.io/autc04/retro68 \
    powerpc-apple-macos-size /b/Sherclawk.xcoff
```

Text plus data plus bss was 2,454,552 bytes (2.34 MiB) at 3072 tokens with a
16 KiB reply buffer and 256 KiB history, 2,512,456 bytes (2.40 MiB) at 6000
tokens with the 40 KiB `AGENT_REPLY_CAP`, and 3,036,744 bytes (2.90 MiB) with
384 KiB history, and 3,220,816 bytes (3.07 MiB) once the 128 KiB `view_image`
buffer and later changes were linked. This excludes dynamic TLS and UI
allocations and the stack, so keep the minimum partition comfortably above it.

With the same layout, increasing history from 384 KiB to 512 KiB would raise
that baseline to roughly 3.4 MiB before dynamic allocations; 1 MiB history
would need roughly 5.4 MiB, and 2 MiB would exceed the current 8 MiB preferred
partition. Re-measure and raise `SIZE` before such increases. History capacity
also increases per-round upload and input-token costs and is not a guarantee
of provider context capacity. The TextEdit transcript remains independently
bounded.

## Changing a limit

- **`max_tokens`:** check invariants 1 and 6, update this reference and the
  [usage limits](usage.md#run-limits), and update the `max_tokens` assertion
  in `tests/test_agent.c`.
- **`CHAT_RESPONSE_CAP`:** also update the "exceeds 64 KiB" error in
  `network.c` and the [usage limits](usage.md#run-limits), and re-measure size.
- **History:** raise `CHAT_REQUEST_CAP` with it, update the memory measurements
  here and the [usage limits](usage.md#run-limits), re-measure size, and check
  the `SIZE` resource.
- **`AGENT_IMAGE_CAP`:** check invariant 8 against `CHAT_REQUEST_CAP`, update the
  `view_image` text in `docs/tools.md` and the schema description in `agent.c`
  (it quotes the value), and re-measure size.
- **Anything:** run `tools/check.sh`, then a Docker `./build.sh`, then launch on
  the guest. Compile-time guards in `agent.c` enforce invariants 1 and 2 and
  the 16 KiB minimum overhead of invariant 3, but only the guest shows the partition is big enough.

## Reading the evidence

Each model round logs one line to `Retro68:Sherclawk.log` with
`out=<completion>/<max_tokens>`, `reasoning=<n>` (when the provider reports it)
and `down=<bytes>`. Divide `down` by `out` for the real bytes per token, and see
`end=truncated` for rounds that hit the limit and were retried and
`end=discarded` for replies dropped for an oversized or excess tool call. Re-check the
bytes-per-token figures in invariant 1 against fresh logs whenever the model or
provider changes.

## MCP diagnostic limits (`mcp.h`, `mcp_client.h`)

These buffers currently belong to `SherclawkMCPCheck`, not the main app.

| Limit | Value | Bounds |
|---|---|---|
| Configuration | 8 KiB UTF-8 | One zero/one-server JSON document; 2048 parser tokens. |
| HTTP headers | 8 KiB | Each request/response header section; no compression/redirects. |
| JSON-RPC message | 64 KiB | One JSON body or SSE data event; 8192 parser tokens. |
| Exchange traffic | 256 KiB | All received framing, keepalives, notifications and data. |
| Discovery | 8 pages / 64 entries | Entire discovery; duplicates or budget exhaustion invalidate it. |
| Exposed tools | 8 / 16 KiB schemas | Frozen registry; original and mapped names bounded to 63 bytes. |
| Selection | 8 names | Explicit configuration selection; empty selects none. |
| JSON object keys | 8 KiB / 4096 per object | Duplicates are found by sorting decoded keys, not by pairwise comparison. |
| Server replies | 4 × 2 KiB | Pending ping/error replies; overflow fails rather than dropping. |
| Session / cursor | 1023 bytes | Session visible ASCII; cursor encoded as JSON. |
| Discovery deadline | 30 s total | Initialize, acknowledgment and all pages. |
| Call deadline | 120 s | One sequential call, no retry. |
| Cancellation grace | 5 s | Best-effort notification; never claims server work stopped. |
| Network work | 4 × 2 KiB reads / step | Only one exchange is read per step. |

Stop/deadline cleanup drains an outstanding OT connect before freeing its
context, to avoid the known #34 early-abort crash. This cleanup may outlast the
request deadline; Certainly owns its connect timeout. It launches no new call.

Scratch larger than a few hundred bytes (tool descriptions, key comparison,
configuration parsing) is static, because the application stack is small and
unmeasured; static scratch is counted in the bss figure below and wiped after
configuration parsing.

The streaming decoder retains a bounded current line/message rather than the
whole HTTP exchange, and finishes as soon as the matching response arrives.
The protocol diagnostic still parses a completed JSON message synchronously;
resumable parsing and result processing within 8 KiB per step are required
before application integration. The separate acknowledgment connection adds
TLS allocations, so its footprint must also be measured on the guest.
The future retained-result cap is 128 KiB, while the existing 1536-byte tool
result and 8 KiB argument caps will remain unchanged. Result artifacts and
pagination are not implemented yet.

On 2026-10-08 the Docker-linked MCP diagnostic measured 339,968 bytes text,
6,624 data and 1,190,632 bss (1,537,224 total). The main app remains 3,036,744
bytes because it does not yet link MCP. These measurements exclude dynamic
TLS/UI allocations and stack; they do not prove the integrated 4 MiB minimum
partition is sufficient.
