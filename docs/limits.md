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
| `AGENT_HISTORY_CAP` | 256 KiB | Recorded conversation JSON (`Agent.history`). |
| `CHAT_REQUEST_CAP` | 288 KiB | The whole request JSON (`gJSON`): history plus system prompt and tool schemas. |
| `AGENT_TEXT_CAP` | 16 KiB | User prompt and handoff message buffers. Not reply text. |
| `AGENT_RESULT_CAP` | 1536 | One tool result recorded into history. |
| JSON token scratch | 4096 tokens | Tokens (not bytes) in any parsed response; `tokens[]` in `agent.c`. |

### Display and time

| Limit | Value | Bounds |
|---|---|---|
| `CHAT_TRANSCRIPT_CAP` | 30,001 bytes | The TextEdit transcript. Classic TextEdit stops at 32,000 bytes, so this cannot simply be raised. A reply too long for it is journaled but shown as a "see the session file" placeholder. |
| Request deadline | 120 s | One HTTPS exchange (`StepModelExchange`). |
| Context lookup deadline | 30 s | The model context-window query. |
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
   rounds; the round limit is what bounds repeated truncation.
5. **One response must fit in the remaining history.** `agent_response`
   refuses a reply that would not fit with room for its tool results.
   Reasoning fields are recorded with the message, so a reasoning-heavy round
   can add tens of KB to history.
6. **Round time ≈ `max_tokens` ÷ generation speed.** Measured about 107 tokens
   per second on a fast model (3072 tokens in 28.7 s), so 6000 tokens is about
   56 s against the 120 s deadline. A slower model shrinks that margin.
7. **Every round re-uploads the full history.** History size is therefore also
   per-round upload time, and bigger history only helps up to the model's own
   context window (the Context line in the status area).

## Memory cost per byte of cap

| Constant | Buffers it sizes | Static bytes per byte of cap |
|---|---|---|
| `CHAT_RESPONSE_CAP` | `raw`, `body`, `agent_response` `message` | 3 |
| `AGENT_HISTORY_CAP` | live history, handoff candidate history, `gJSON`, HTTP request | 4 |
| `AGENT_REPLY_CAP` | `Agent.text` in the live and candidate `Agent` | 2 |
| `AGENT_TEXT_CAP` | prompt buffer; handoff content (once) and message (six times) | 8 |
| `AGENT_MAX_TOKENS` | none | 0 |

Linked static size is measured, not estimated, with `powerpc-apple-macos-size`
on `build/Sherclawk.xcoff` in the Retro68 image:

```bash
docker run --rm -v "$PWD/build:/b" ghcr.io/autc04/retro68 \
    powerpc-apple-macos-size /b/Sherclawk.xcoff
```

Text plus data plus bss was 2,454,552 bytes (2.34 MiB) at 3072 tokens with a
16 KiB reply buffer, and 2,512,456 bytes (2.40 MiB) at 6000 tokens with the
40 KiB `AGENT_REPLY_CAP`. This excludes dynamic TLS and UI allocations and the
stack, so keep the minimum partition comfortably above it.

## Changing a limit

- **`max_tokens`:** check invariant 1 and 6, update the README limits
  paragraph, and update the `max_tokens` assertion in `tests/test_agent.c`.
- **`CHAT_RESPONSE_CAP`:** also update the "exceeds 64 KiB" error in
  `network.c` and the README, and re-measure size.
- **History:** raise `CHAT_REQUEST_CAP` with it, update the README memory
  paragraph, re-measure size, and check the `SIZE` resource.
- **Anything:** run `tools/check.sh`, then a Docker `./build.sh`, then launch on
  the guest. Compile-time guards in `agent.c` enforce invariants 1 and 2 and
  the 16 KiB minimum overhead of invariant 3, but only the guest shows the partition is big enough.

## Reading the evidence

Each model round logs one line to `Retro68:Sherclawk.log` with
`out=<completion>/<max_tokens>`, `reasoning=<n>` (when the provider reports it)
and `down=<bytes>`. Divide `down` by `out` for the real bytes per token, and see
`end=truncated` for rounds that hit the limit and were retried. Re-check the
bytes-per-token figures in invariant 1 against fresh logs whenever the model or
provider changes.
