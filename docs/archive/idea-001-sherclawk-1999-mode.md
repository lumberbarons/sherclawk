# 001 — Sherclawk 1999 mode

> Historical design note. Proposals, status statements and source-line references
> describe development at the time of writing and may be superseded. Use the
> [current guides](../../README.md#documentation) for supported behavior and the
> [issue tracker](https://github.com/lumberbarons/sherclawk/issues) for active work.

**Status:** idea, not built. Feasibility consideration only.

## The idea

A mode where Sherclawk is convinced it is 1999, the year the iMac G3 was
released. Purely for fun, but it also exercises how much the agent's view of
the world is controlled by its prompt and tool output.

## What Sherclawk gives the model today

- The system prompt (`policy` in `agent.c`) is a static string. It
  already says "classic Mac OS 9.2.2".
- Nothing injects a date or time. The model has no clock, so it falls back on
  its training prior and assumes it is recent.
- Tool results (`get_environment`, `list_files`, `read_text`, ...) may carry
  file metadata. Any mtimes come from the guest's real clock.

## Approaches, cheapest first

1. **Prompt-only persona.** Append a few lines to the system prompt when the
   mode is on: a specific 1999 date, a freshly released iMac G3, dial-up, a
   young web, and nothing newer known. No new code beyond the flag. Models
   play along reliably, and naming era details (Y2K worry, AOL, Netscape 4,
   Napster arriving mid-year) makes it more fun.
2. **Make `get_environment` consistent with the persona.** Report the 1999
   date there too. Models trust tool output more than the prompt, so this
   reinforces the fiction.
3. **Change the guest clock.** Possible via the Date & Time control panel or
   QEMU `-rtc base=...`, but not recommended:
   - `-rtc base=localtime` in the QEMU launcher (see
     [macos9-qemu.md](../macos9-qemu.md)) is load-bearing.
   - TLS would see every certificate as not yet valid, so the TLS path to the
     model API would fail.
4. **Fake the clock inside Sherclawk only.** Leave the guest clock alone and
   apply a date offset in the tool layer, translating "now" and file mtimes
   for the model while TLS keeps the true time. Only worth doing if file
   listings should look era-appropriate.

## Limits

- The model cannot un-know things. Files mentioning modern technology will
  either break the persona or get hand-waved; leaning into it ("that sounds
  like science fiction") works.
- Journaled writes and backup names use real times. If those stamps reach the
  model they contradict the persona unless approach 4 covers them.
- A modern date leaking through any tool result can break the illusion.

## Recommendation

Do approaches 1 and 2 behind a config flag, which is small, safe and gives
most of the effect. Add approach 4 only if era-appropriate file dates matter.
Skip changing the guest clock.

## Open question

Should the flag be compile-time (`config.local.h`) or a runtime toggle in the
chat UI?
