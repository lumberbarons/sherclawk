# Tests Review 001 — sherclawk

**Date:** 2026-10-02
**Scope:** `sherclawk/tests` — test_core.c, test_agent.c, test_network.c, test_tls_io.c, test_tools.c
**Result:** 10 findings (0 P1, 8 P2, 2 P3)

Each finding carries a **Status** field: `open` until its *Done when* criterion is met, then `fixed`.

---

### 1. [P2] The only TLS 1.2 write case cannot tell whether the data path ran
**Status:** fixed
**Location:** tests/test_tls_io.c:320-321 (fakes at :253-262)

Fake `br_ssl_engine_sendapp_buf` returns a scratch buffer and `br_ssl_engine_sendapp_ack`/`br_ssl_engine_flush` discard their arguments; the case asserts only `MacTLS_Write(...) == 3 && send_acks == 1`. If the `memcpy(bbuf, data, len)` in the patched TLS 1.2 branch (staged certainly.c:940) were deleted, if `sendapp_ack` were called with `len - 1`, or if the flush call were removed, the test would still incorrectly pass — this is the only coverage of the fallback write path used after a server negotiates TLS 1.2, and the `recvapp_buf`/`recvapp_ack` fakes at :259-262 are never invoked by any read case.

**Fix:** Have the ack fake record `last_sendapp_ack = len`, seed `engine_buf` with a sentinel and assert its first 3 bytes equal `"abcdef"`, count flushes and assert `== 1`, and add a `MacTLS_Read` case with `tls13_active == false`.
**Done when:** the file asserts the copied bytes, the ack length, and the flush count, and calls `MacTLS_Read` once with TLS 1.2 active.

### 2. [P2] Two of the six registered tools are never executed by any test
**Status:** open
**Location:** tests/test_tools.c:153-261 (only `write_text`/`read_text`/`search_text`/`edit_text` ever set as `call.name`); dispatch at tools.c:774-777

`get_environment` and `list_files` have zero executed coverage: `environment()` (tools.c:100) and `list()` (tools.c:115) are never entered. Replace the `list_files` dispatch with `fail(...,"UNKNOWN_TOOL",...)`, or truncate `list()`'s pagination output, and the suite still incorrectly passes — no assertion anywhere reads their results.

**Fix:** Add dispatches for both: `get_environment` with `{}` asserting `"status":"ok"`, workspace and tool list; `list_files` asserting folder/file kinds, `{"limit":1}` produces `"truncated":true` with a working `next_cursor`, and `FOLDER`/`ARGUMENTS` for bad roots and cursors.
**Done when:** `test_tools.c` sets `call.name` to each of those names at least once, followed by result assertions.

### 3. [P2] Non-2xx rejection is masked by bodies that fail other validation
**Status:** open
**Location:** tests/test_core.c:100, tests/test_agent.c:53

Both 503 cases use the body `"{}"`, which already fails for a missing `choices` array. Delete the `status < 200 || status >= 300` clause from chat.c:51 and agent.c:119 and every assertion still incorrectly passes; neither suite ever delivers a well-formed success payload with an HTTP error status, and test_agent has no 2xx-with-`"error"`-object case at all.

**Fix:** Reuse the valid completion body with status 503 and assert failure plus `strstr(error, "503")`; in test_agent add a status-200 response carrying an `"error"` object and assert its message is preserved.
**Done when:** each file's 503 assertion is applied to a body that parses as a valid completion, and the error text is checked.

### 4. [P2] Request bodies are verified by substring only — model, role order and schema completeness unpinned
**Status:** open
**Location:** tests/test_core.c:88-94, 110; tests/test_agent.c:38-39, 46-47

Hardcoding a different model string in `chat_request`/`agent_request`, swapping the role ternary at chat.c:32-33, emitting `"role":"user"` for tool results at agent.c:171, or corrupting `agent_tool_schemas()` into invalid JSON while keeping the words `write_text`/`edit_text`/`search_text` leaves every assertion passing — and only 3 of the 6 schema names are checked (test_agent never calls `json_parse` on `req`).

**Fix:** `json_parse` each built request, then assert the literal model, the ordered assistant/user message with its content, `"role":"tool","tool_call_id":"c1"`, and all six tool names in the schemas.
**Done when:** every request-building call site is followed by a `json_parse` plus at least one literal model/role/schema assertion.

### 5. [P2] The patched `MacTLS_Close` pending-send guard and `MacTLS_Available` are never executed
**Status:** open
**Location:** tests/test_tls_io.c (no call sites); production staged certainly.c:841, 1011

The app-I/O patch's `tls13_send_offset == tls13_send_len &&` guard (which suppresses close_notify encryption while a record is pending) and `MacTLS_Available` are exported API, but neither is called. Remove the guard or stub `MacTLS_Available` to `return 0;` and the entire suite still passes — including the coalesced-record case where 8192 queued bytes exist.

**Fix:** In the coalesced case assert `MacTLS_Available(&ctx) == 8192` before the first read and `0` after draining. For the close guard, use a heap-allocated context (the current `static` one cannot be passed to a disposing `MacTLS_Close`), leave a pending send (`send_result = 0`), call `MacTLS_Close`, and assert `encryptions` did not increase.
**Done when:** the file calls `MacTLS_Available` with asserted values and `MacTLS_Close` with a pending send, asserting the close_notify was not encrypted.

### 6. [P2] Unfalsifiable conjuncts stand in for status and identity checks
**Status:** open
**Location:** tests/test_tools.c:287, 184

`strstr(result,"error")` is true for every result `write_text` can produce, because `fail()` (tools.c:21) and `mutation_result` (tools.c:480-482) both always embed `"os_error":N`, including in `"ok"` results; changing `"status":"error"` to `"status":"rejected"` still passes all nine bad-input cases. Likewise `files[i].used` at :184 is set by `add()` and only ever cleared by `reset()`, so it is always truthy.

**Fix:** Assert `strstr(result, "\"status\":\"error\"")` and the specific code per case (`ARGUMENTS`, `ENCODING_LIMIT`, `NOT_TEXT`, `PATH`, `EXISTS`), and drop the `files[i].used` conjunct (assert the backup path/leaf identity instead).
**Done when:** those assertions read `\"status\":\"error\"` or explicit codes, and `files[i].used` no longer appears in an assertion.

### 7. [P2] Dead expectation: the fake journal discards `event`
**Status:** open
**Location:** tests/test_agent.c:15 (`(void)event`)

Production passes a meaningful event name on every record (agent.c:85 `"user"`, :161 `"assistant"`, :175 `"tool"`), but the fake ignores it and only the `records` count is asserted. Replace every event literal in `agent.c` with `""` and the suite still passes, even though the neighbouring test_tools.c:115-118 treats event names as assertion-worthy.

**Fix:** Capture `event` into a static buffer in the fake and assert the sequence per scenario (`user` after begin, `assistant` after response, `tool` after result, and `tool` for `agent_stop`'s interrupted results).
**Done when:** the journal fake stores `event` and at least one assertion per scenario reads it.

### 8. [P2] Dead spy `pumps`, and TLS error codes that nothing asserts
**Status:** open
**Location:** tests/test_network.c:9, 17, 38-40, 66

`pumps++` is incremented but never read; and while production formats all three numeric codes into `net.error` (network.c:38-39), the only check is `strstr(net.error, "TLS error")`. Make the three getters return arbitrary values, or remove their arguments from the `snprintf`, and the test still passes — although `main.c` surfaces this string to the guest.

**Fix:** Assert `pumps == 1`/`pumps == 2` at established transitions; return distinguishable sentinels from the getters and assert the formatted `code=`/`bssl=`/`OT=` fields.
**Done when:** `pumps` appears inside an assertion and the error assertion reads all three numeric fields.

### 9. [P3] `read_text`'s `start_byte` slice and the >4096 scan path are unverified
**Status:** open
**Location:** tests/test_tools.c:164-165, 200-202

Line 164 reads with `"start_byte":5` but asserts only revision and `"editable":true`; line 201 reads at 3500 but asserts only that the revision appears. Delete the `memmove(bytes, bytes + base, ...)` at tools.c:243 and both still pass — the revision is computed before the move. The `!whole` branch (scan- revision, `editable:false`, tools.c:244) is never executed at all; the scan guard is only probed with a fabricated `"scan-stale"` string at :207.

**Fix:** Extract the returned `"text"` with `field()` and assert it begins at the requested offset; add a >4096-byte read asserting a `scan-` revision, `"editable":false` and correct line info, then feed that emitted revision to `edit_text` and assert `ARGUMENTS`.
**Done when:** a nonzero-`start_byte` read compares the returned slice, and a >4096-byte read asserts its scan revision.

### 10. [P3] The engine-close case asserts the fake's own flag, not the production state transition
**Status:** open
**Location:** tests/test_tls_io.c:233-236

`engine_state == BR_SSL_CLOSED` is set by the test's own `br_ssl_engine_sendrec_ack` fake, and because the engine closes during that pump the production `if (st == BR_SSL_CLOSED)` mapping (staged certainly.c:735) never runs. Replace the mapping body with `return ctx->state;` and the test still passes — `ctx.state` is never asserted.

**Fix:** Add a second `pump()` and assert `ctx.state == kMacTLS_Closed`; add an error variant with a non-zero engine error asserting `kMacTLS_ErrHandshake`/`kMacTLS_ErrCertificate`.
**Done when:** the case asserts `ctx.state`, not just the fake's `engine_state`.

---

Note: 9 additional findings omitted (0 P2, 9 P3) — re-run after addressing these to surface what remains.
