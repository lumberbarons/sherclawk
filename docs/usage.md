# Using Sherclawk

Build and transfer the application as described in the
[development guide](development.md#build-and-publish). The app runs in classic
Mac OS 9 and makes direct HTTPS requests to OpenRouter; there is no host model
relay. An OpenRouter key and a reachable configured workspace are needed to
run the agent.

## Workspace and Preferences

The default workspace is `Retro68:`. Configure a native MacRoman path ending
in a colon. Tool paths are relative to it, for example `Spikes:ccapp:hello.c`;
leading colons, parent traversal, slash paths, and invalid names are refused.
Workspace source text is interpreted as MacRoman, converted to UTF-8 for the
model, and displayed through native TextEdit. Binary/resource-fork files and
aliases are refused by text tools; metadata and resources have separate
[inspection tools](tools.md#inspecting-resources-and-identity).
Tool arguments never pass through lossy UI conversion.

**Preferences** (Edit menu, last item) sets the model, API key, workspace,
per-run limits and a *Show tool debug in Conversation* toggle. Values are saved
as clear `key=value` MacRoman text with CR line endings at
`System Folder:Preferences:Sherclawk Preferences` (file type `pref`, creator
`ShCk`); the file is created on the first save, so existing builds are
unchanged until the dialog is used. The key is shown plainly in the dialog and
stored without encryption; protect the preferences file and guest disk images.
Saved values override `config.local.h`,
which stays as the compiled fallback. The workspace applies to new tool work
immediately (tools, new sessions, builds); a session already open keeps
writing to the folder it was opened in. Limits accept 1–128 and apply per run;
outside that range, or with a value missing or malformed in the file, the
compiled default is used. The Model field is also the catalog search box: the
dialog lists the ten most popular models when it opens (once per launch),
typing filters that list locally, **Find** searches the whole public catalog
for the typed text, and clicking a row confirms the model and lists its Effort
choices (the effort is shown only; it is not saved or sent yet). OK saves only
a model that resolved to a catalog row — a changed, unconfirmed ID is checked
with `GET /api/v1/models?q=<id>&limit=10` first, and a typo leaves the dialog
open with the closest candidates. The model already saved is not re-checked,
so the other fields can be changed while the catalog is unreachable. A failed
fetch or Cancel leaves the typed text in place and changes nothing. Return and
Enter press OK; Escape and Command-. press Cancel. Each executed
call shows a compact call line — the tool name and rendered arguments — in the
Conversation. Consecutive calls to the same tool fold into one counted line
(`• read_text x12`) until another message or tool appears; with the debug
toggle on, every call stays on its own line and the result and the call's
journal event names follow it. The session JSONL is byte-identical either way, and the
API key never appears in status text, logs, prompts or results.

## Controls, limits and sessions

The compact mascot header leaves the conversation pane the full window width.
Command-Return sends the message; its shortcut is shown beside the message box.
Tab switches between the message and conversation panes.
New Chat and Save Handoff are on the left; Stop and Send are on the right.
Send has a native default-button outline while it is available.

### Reading status

A recessed two-row information strip below the message box groups status and
the read-only model name above history, context and cost. Choose the model in
**Edit > Preferences**; both sends and handoffs use that saved preference.
Long labels are shortened to fit their columns; click the status row while
idle to read the full status message. The status lamp is green while idle and amber during
a run. History shows used/capacity KiB, a percentage and a small usage meter;
it updates during runs and resets with New Chat or a successful handoff.

The bottom row also reports provider accounting.
`Context: 45.2k tokens (4%)` is the most recent model round's
`usage.prompt_tokens` (cached input included) and its rounded share of that
model's context window; before the first reply it reads `Context: -`, and the
percentage is omitted while the window is unknown. `Cost: $0.012345` is the
running sum, across the session's model rounds, of the provider-computed USD
`usage.cost` (OpenRouter credits are USD 1:1), accumulated in millionths of a
dollar and hidden while it is zero. Accounting is display-grade: values below
one micro-dollar round to zero. New Chat resets both totals; a successful
handoff carries the cost total into the fresh history but leaves the context
line unset until the next reply.

### Sending and stopping

Send or Command-Return starts a run. The model is fixed for that run. The app
services events while waiting, executes calls sequentially, displays tool
results, and requests another model response until it gets a final answer.
Stop or Command-Period prevents new execution and records interrupted results
for pending calls; completed results remain in history. New Chat starts a fresh session.

### Save Handoff

**Save Handoff**, beside New Chat in the bottom controls, is also available
as **File > Save Handoff (Command-H)**. The button and menu item are disabled
during runs and when there is no conversation to summarize. It summarizes a
stopped or completed conversation with a separate, tool-free model request. This still works when
history is full: the summary request never appends to the old history. The model
produces a concise Markdown handoff covering goals and constraints, completed
work and exact paths, observed verification, unresolved or uncertain operations,
and next steps. Summary generation uses the selected model and the normal
120-second HTTPS deadline. Stop cancels observation and retains the old history;
there is no automatic retry or automatic compaction.

The app saves a unique `hXXXXXXXX.md` in `Retro68:Sherclawk Sessions:` as
MacRoman/CR plain `TEXT` (at most 4096 bytes), so later sessions can inspect it
with `list_files` and `read_text`. It closes, flushes, and reads back the exact
bytes before creating a new UTF-8 journal with the summary as its first user
message. Only then does it replace model history. Truncated/tool-call responses,
unsupported encoding, oversized summaries, and persistence failures leave the
old history and journal intact. A partially created Markdown file is retained
and reported; incomplete candidate journals remain in the sessions folder.
Both the Markdown and seeded message name the original journal. The visible
transcript and any unsent prompt remain available; send a message to continue.
At 75% history usage, the status suggests saving a handoff. The summary is lossy:
current sources and uncertain mutations still need inspection before acting.
Full-journal reloading remains unimplemented; to resume after quitting, ask a
new chat to read the saved Markdown path displayed when it was created.

### Run limits

Limits are explicit: by default 32 model rounds and 64 executed calls per run,
each adjustable 1–128 in Preferences; four calls per
response, 8 KiB arguments per call, 384 KiB history, 416 KiB JSON request, 64 KiB
raw HTTP response, 40 KiB of reply text, and 6,000 output tokens per model request (`AGENT_MAX_TOKENS`
in `agent.h`; the handoff request has its own `AGENT_HANDOFF_MAX_TOKENS`). Reasoning
tokens count against that budget. The [limits reference](limits.md) explains how
these bounds relate. Each HTTPS request has a 120-second
deadline. Tool output is below 1,536 bytes; folder listings have cursors and
text reads provide `next_byte` continuation when a line is partial. Reads scan
at most 8 KiB per invocation. Whole-file revisions guard small-file edits;
larger-file scan revisions are observational.
A reply cut off at the output limit never executes anything. If it has tool
calls, or no visible text at all (reasoning used the whole budget), it is
discarded and the model is told, in a user message, that its last reply was cut
off and not run, so it can retry with a smaller step; the run continues and the
retry counts as a model round. A cut-off reply that is only partial text is
shown with a notice and ends the run. A complete reply with a tool call whose
arguments exceed 8 KiB, or with more calls than a response may carry, is handled
the same way: discarded unrun, the model told to split the work (`write_text` and
`edit_text` text is limited to 4096 bytes), and the retry counts as a round.
Every other malformed reply stops the run with the failed check named in the
error and the lifecycle log, for example a missing call id or a duplicate one.
There is no automatic network retry.
Reaching a run limit pauses with the actual model-round and tool counts, the
configured ceilings, history usage
percentage, and a reminder to send Continue. Sending another message resets
the run counters while retaining conversation history; handoff is not required.

### Session files

Before execution, complete assistant responses and tool-start records are
saved to unique UTF-8 JSON-lines files in `Retro68:Sherclawk Sessions:`.
Results and user messages are saved there too. These files contain conversation
and file contents; lifecycle logs omit them and credentials. Rejected model
responses never enter the conversation: a `model_error` journal record keeps
their HTTP status, received body size and reason, and every stopped run appends
its reason to the lifecycle log so failures stay diagnosable after relaunch.
A recording failure stops execution. Journals are preserved across relaunches;
automatic session reloading/recovery is not implemented yet. When display
limits are reached, earlier visible text is replaced with a notice referring to
the saved session; model history is not silently dropped.
