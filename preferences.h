/* Persistent app preferences: a "key=value" MacRoman file in the guest's
 * System Folder:Preferences over compiled defaults. Parsing and validation are
 * pure C so the host suite exercises exactly what the dialog validates. Model
 * and key are printable ASCII; the workspace follows the tool path rules. */
#ifndef SHERCLAWK_PREFERENCES_H
#define SHERCLAWK_PREFERENCES_H
#include <stddef.h>
#include "agent.h"
#include "chat.h"
#include "config.h"

#define PREFS_KEY_CAP 256
#define PREFS_WORKSPACE_CAP 256
#define PREFS_MAX_BYTES 4096
#define PREFS_WORKSPACE_MAX 192 /* room for the session, handoff and build paths */
#define PREFS_LIMIT_MIN 1
#define PREFS_LIMIT_MAX 128

typedef struct {
    char model[CHAT_MODEL_CAP];
    char api_key[PREFS_KEY_CAP];
    char workspace[PREFS_WORKSPACE_CAP];
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
#endif
