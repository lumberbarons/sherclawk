/* Durable session store: the UTF-8 JSON-lines journal and the Markdown
 * handoff that seeds a successor journal. Pure File Manager code, so it runs
 * against the host stubs in tests/toolbox. */
#ifndef SHERCLAWK_SESSION_H
#define SHERCLAWK_SESSION_H
#include <stddef.h>
#include "agent.h"

typedef struct {
    short ref;
    int open;
    char folder[256]; /* "<workspace>Sherclawk Sessions:" the journal lives in */
    char path[256];   /* open journal, colon path */
} Session;

/* Create a new journal in the sessions folder of `workspace` (MacRoman,
 * ending in ':'), closing any journal `s` already holds only after the new
 * one exists. Returns 0, or -1 leaving `s` untouched. */
int session_open(Session *s, const char *workspace);
void session_close(Session *s);

/* AgentJournal callback; `session` is always the owning Session *. Writes one
 * `{"event":..,"message":..}` line and flushes it. -1 if the session is
 * closed or any write fails. */
int session_journal(void *session, const char *event, const char *json);

/* Write `summary` as a never-overwritten `hXXXXXXXX.md` beside the journal
 * (MacRoman/CR TEXT) and read it back byte for byte. `path_out` receives the
 * file's path as soon as it exists, so a partial file is reported on failure
 * too; it is emptied first. Returns 0 or -1. */
int session_save_handoff(Session *s, const char *summary, char *path_out, size_t cap);

/* Save and verify the handoff, create a successor journal, seed a fresh
 * history from `summary`, and only then swap: `s` moves to the new journal
 * and `*agent` becomes the seeded history (session cost carried over, journal
 * context `s`). On any failure `*agent` and `*s` are untouched. */
int session_commit_handoff(Session *s, Agent *agent, const char *summary,
                           char *path_out, size_t cap);
#endif
