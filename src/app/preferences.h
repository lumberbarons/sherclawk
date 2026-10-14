/* Persistent app preferences: a "key=value" MacRoman file in the guest's
 * System Folder:Preferences over compiled defaults. Parsing and validation are
 * pure C so the host suite exercises exactly what the dialog validates. Model
 * and key are printable ASCII; the workspace follows the tool path rules. */
#ifndef SHERCLAWK_PREFERENCES_H
#define SHERCLAWK_PREFERENCES_H
#include <stddef.h>
#include "agent/agent.h"
#include "app/chat.h"
#include "config.h"

/* Leaf name in System Folder:Preferences. The save rewrites it in place and
 * stages no sibling file. The model-facing file tools never reach it
 * (mcp/guard.c). */
#define PREFS_FILENAME "Sherclawk Preferences"
#define PREFS_KEY_CAP 256
#define PREFS_WORKSPACE_CAP 256
#define PREFS_MAX_BYTES 4096
#define PREFS_WORKSPACE_MAX 192 /* room for the session, handoff and build paths */
#define PREFS_LIMIT_MIN 1
#define PREFS_LIMIT_MAX 128
#define PREFS_CONTEXT_MAX 100000000L /* tokens; far above any real window */

typedef struct {
    char model[CHAT_MODEL_CAP];
    char api_key[PREFS_KEY_CAP];
    char workspace[PREFS_WORKSPACE_CAP];
    char effort[AGENT_EFFORT_CAP];
    /* The confirmed catalog row, kept so a launch needs no lookup. All three
     * belong to context_for and are trusted only while it names the model. */
    long context;                    /* context window; 0 = unknown */
    int vision;                      /* AGENT_VISION_* */
    char default_effort[AGENT_EFFORT_CAP];
    char context_for[CHAT_MODEL_CAP];/* model id the row was stored for */
    int max_rounds;
    int max_tools;
    int show_tool_debug;
} Prefs;

/* Compiled defaults: config.local.h over config.h, plus agent.h limits. */
void prefs_defaults(Prefs *p);

/* Overlay recognized values in `text` on `p`; missing, blank, unknown and
 * malformed entries leave the current value. Returns the accepted key count. */
int prefs_parse(const char *text, size_t len, Prefs *p);

/* Render the whole file; -1 when a value cannot round-trip (line break or a
 * value longer than its field). Returns the byte length written, excluding NUL. */
int prefs_format(const Prefs *p, char *out, size_t cap);

int prefs_model_ok(const char *s);     /* non-empty, printable ASCII, < 256 */
int prefs_key_ok(const char *s);       /* printable ASCII, may be empty, < 256 */
int prefs_workspace_ok(const char *s); /* MacRoman colon path ending in ':' */
int prefs_limit_value(const char *s);  /* decimal 1-128, else -1 */
/* Empty, or one exact provider effort name (max, xhigh, high, medium, low,
 * minimal, none). The dialog stores only what the confirmed row supports. */
int prefs_effort_ok(const char *s);
/* Decimal token count 0..PREFS_CONTEXT_MAX (empty and "0" mean unknown), else -1. */
long prefs_context_value(const char *s);
/* The stored context window when `context_for` names exactly `model` and the
 * window is known, else -1: preferences from older builds and hand-edited
 * model ids then fall back to a catalog lookup. */
long prefs_context_for(const Prefs *p, const char *model);
/* Store the confirmed catalog row for `model`. A window outside 1..
 * PREFS_CONTEXT_MAX, a bad vision value or an effort word the file would not
 * accept is stored as unknown or empty so the file always saves. */
void prefs_store_row(Prefs *p, const char *model, const AgentModelInfo *info);
/* Fill `out` from the stored row and return 1 when it is trusted for `model`
 * (same test as prefs_context_for); else return 0 and leave `out` alone. Only
 * the window, vision flag and default effort are stored, the rest stays zero. */
int prefs_row_for(const Prefs *p, const char *model, AgentModelInfo *out);
#endif
