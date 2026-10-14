/* Preferences parser/formatter. No Toolbox calls: the host suite compiles this
 * file directly, and only main.c adds the File Manager load/save around it. */
#include "app/preferences.h"
#include <string.h>

static void copy_value(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n); dst[n] = 0;
}

void prefs_defaults(Prefs *p)
{
    memset(p, 0, sizeof(*p));
    copy_value(p->model, sizeof(p->model), SHERCLAWK_MODEL);
    copy_value(p->api_key, sizeof(p->api_key), SHERCLAWK_API_KEY);
    copy_value(p->workspace, sizeof(p->workspace), SHERCLAWK_WORKSPACE);
    p->max_rounds = AGENT_TURN_MAX;
    p->max_tools = AGENT_TOOL_MAX;
    p->show_tool_debug = 0;
}

int prefs_model_ok(const char *s)
{
    size_t i;
    if (!*s || strlen(s) >= CHAT_MODEL_CAP) return 0;
    for (i = 0; s[i]; i++) if ((unsigned char)s[i] < 33 || (unsigned char)s[i] > 126) return 0;
    return 1;
}

int prefs_key_ok(const char *s)
{
    size_t i;
    if (strlen(s) >= PREFS_KEY_CAP) return 0;
    for (i = 0; s[i]; i++) if ((unsigned char)s[i] < 33 || (unsigned char)s[i] > 126) return 0;
    return 1; /* an explicitly empty key is allowed: it clears a compiled one */
}

int prefs_workspace_ok(const char *s)
{
    size_t at, len = strlen(s), component = 0;
    if (len < 2 || len > PREFS_WORKSPACE_MAX || s[0] == ':' || s[len - 1] != ':') return 0;
    for (at = 0; at < len; at++) {
        unsigned char c = (unsigned char)s[at];
        if (c < 32 || c == 127 || c == '/' || c == '\\') return 0;
        if (c == ':') { if (!component || component > 31) return 0; component = 0; }
        else component++;
    }
    return component == 0;
}

int prefs_limit_value(const char *s)
{
    long n = 0;
    size_t i;
    if (!*s || strlen(s) > 3) return -1;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        n = n * 10 + (s[i] - '0');
    }
    return n >= PREFS_LIMIT_MIN && n <= PREFS_LIMIT_MAX ? (int)n : -1;
}

int prefs_effort_ok(const char *s)
{
    static const char *const names[] = {
        "max", "xhigh", "high", "medium", "low", "minimal", "none"
    };
    size_t i;
    if (!s[0]) return 1; /* empty stores no effort at all */
    for (i = 0; i < sizeof(names) / sizeof(*names); i++)
        if (!strcmp(s, names[i])) return 1;
    return 0;
}

long prefs_context_value(const char *s)
{
    long n = 0;
    size_t i;
    if (strlen(s) > 9) return -1;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        n = n * 10 + (s[i] - '0');
    }
    return n <= PREFS_CONTEXT_MAX ? n : -1;
}

long prefs_context_for(const Prefs *p, const char *model)
{
    if (p->context <= 0 || !model[0] || strcmp(p->context_for, model)) return -1;
    return p->context;
}

static int key_is(const char *key, size_t len, const char *name)
{
    return strlen(name) == len && !memcmp(key, name, len);
}

int prefs_parse(const char *text, size_t len, Prefs *p)
{
    size_t at = 0;
    int accepted = 0;
    while (at < len) {
        size_t start = at, end, key_len, value_len;
        const char *key, *value, *eq;
        char value_buf[PREFS_KEY_CAP];
        while (at < len && text[at] != '\r' && text[at] != '\n') at++;
        end = at;
        while (at < len && (text[at] == '\r' || text[at] == '\n')) at++;
        while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;
        if (start == end || text[start] == '#') continue;
        eq = memchr(text + start, '=', end - start);
        if (!eq) continue; /* no '=' on the line */
        key = text + start;
        key_len = (size_t)(eq - key);
        while (key_len && (key[key_len - 1] == ' ' || key[key_len - 1] == '\t')) key_len--;
        value = eq + 1;
        while (value < text + end && (*value == ' ' || *value == '\t')) value++;
        value_len = (size_t)(text + end - value);
        while (value_len && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t')) value_len--;
        if (value_len >= sizeof(value_buf)) continue; /* over-long: keep default */
        memcpy(value_buf, value, value_len); value_buf[value_len] = 0;

        if (key_is(key, key_len, "model")) {
            if (prefs_model_ok(value_buf)) { strcpy(p->model, value_buf); accepted++; }
        } else if (key_is(key, key_len, "api_key")) {
            if (prefs_key_ok(value_buf)) { strcpy(p->api_key, value_buf); accepted++; }
        } else if (key_is(key, key_len, "workspace")) {
            if (prefs_workspace_ok(value_buf)) { strcpy(p->workspace, value_buf); accepted++; }
        } else if (key_is(key, key_len, "max_rounds") || key_is(key, key_len, "max_tools")) {
            int n = prefs_limit_value(value_buf);
            if (n >= 0) {
                if (key_is(key, key_len, "max_rounds")) p->max_rounds = n;
                else p->max_tools = n;
                accepted++;
            }
        } else if (key_is(key, key_len, "show_tool_debug")) {
            if (!strcmp(value_buf, "0")) { p->show_tool_debug = 0; accepted++; }
            else if (!strcmp(value_buf, "1")) { p->show_tool_debug = 1; accepted++; }
        } else if (key_is(key, key_len, "effort")) {
            if (prefs_effort_ok(value_buf)) { strcpy(p->effort, value_buf); accepted++; }
        } else if (key_is(key, key_len, "context")) {
            long n = prefs_context_value(value_buf);
            if (n >= 0) { p->context = n; accepted++; }
        } else if (key_is(key, key_len, "vision")) {
            if (value_buf[0] >= '0' && value_buf[0] <= '2' && !value_buf[1]) {
                p->vision = value_buf[0] - '0'; accepted++;
            }
        } else if (key_is(key, key_len, "default_effort")) {
            if (prefs_effort_ok(value_buf)) { strcpy(p->default_effort, value_buf); accepted++; }
        } else if (key_is(key, key_len, "context_for")) {
            if (!value_buf[0] || prefs_model_ok(value_buf)) { strcpy(p->context_for, value_buf); accepted++; }
        }
        /* Unknown keys are ignored. */
    }
    return accepted;
}

static int has_break(const char *s, size_t cap)
{
    size_t i;
    if (strlen(s) >= cap) return 1;
    for (i = 0; s[i]; i++) if (s[i] == '\r' || s[i] == '\n') return 1;
    return 0;
}

int prefs_format(const Prefs *p, char *out, size_t cap)
{
    int n;
    if (has_break(p->model, CHAT_MODEL_CAP) || has_break(p->api_key, PREFS_KEY_CAP) ||
        has_break(p->workspace, PREFS_WORKSPACE_CAP) || has_break(p->effort, AGENT_EFFORT_CAP) ||
        has_break(p->context_for, CHAT_MODEL_CAP) || has_break(p->default_effort, AGENT_EFFORT_CAP) ||
        p->context < 0 || p->context > PREFS_CONTEXT_MAX ||
        p->vision < AGENT_VISION_UNKNOWN || p->vision > AGENT_VISION_YES) return -1;
    n = snprintf(out, cap, "model=%s\rapi_key=%s\rworkspace=%s\rmax_rounds=%d\rmax_tools=%d\r"
        "show_tool_debug=%d\reffort=%s\rcontext=%ld\rvision=%d\rdefault_effort=%s\r"
        "context_for=%s\r", p->model, p->api_key, p->workspace, p->max_rounds, p->max_tools,
        p->show_tool_debug, p->effort, p->context, p->vision, p->default_effort,
        p->context_for);
    return n >= 0 && (size_t)n < cap ? n : -1;
}
