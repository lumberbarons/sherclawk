/* Model-picker state of the Preferences dialog: the fetched catalog page, the
 * local filter, the confirmed row and the fetch transitions. Pure code: every
 * Dialog Manager, Menu Manager and network effect goes through PrefsModelHost,
 * so it runs against the host stubs in tests/toolbox. */
#ifndef SHERCLAWK_PREFS_MODEL_H
#define SHERCLAWK_PREFS_MODEL_H
#include <stddef.h>
#include <stdint.h>
#include "agent/agent.h"
#include "app/chat.h"

enum {
    PREFS_FETCH_NONE = 0,   /* no exchange in flight */
    PREFS_FETCH_POPULAR,    /* the one auto-load per launch */
    PREFS_FETCH_SEARCH,     /* Find: list the matches for the typed text */
    PREFS_FETCH_VALIDATE,   /* OK: the typed id must resolve exactly */
    PREFS_FETCH_SAVED       /* the saved model's own row, when the popular page lacks it */
};

/* prefs_model_start's host send result: the request did not fit, or the
 * exchange could not begin. */
enum { PREFS_SEND_OK = 0, PREFS_SEND_TOO_LARGE = -1, PREFS_SEND_FAILED = -2 };

/* What the picker learned this launch, owned by the caller so it outlives the
 * dialog: the one popular page and the saved model's own row. */
typedef struct {
    AgentModelRow popular[AGENT_MODEL_ROWS_MAX];
    int popular_count;
    int popular_loaded;
    AgentModelRow saved_row;
    int saved_loaded;
} PrefsModelCache;

/* The catalog fetch shares the send-time lookup's 30-second deadline (ticks):
 * how long an exchange may run before it is abandoned, and again how long the
 * abandoned one may keep closing before it is torn down. */
#define PREFS_FETCH_DEADLINE (30UL * 60UL)

/* PrefsModelHost.poll results. */
enum { PREFS_POLL_PENDING = 0, PREFS_POLL_DONE = 1, PREFS_POLL_ERROR = -1 };

/* Effects the picker cannot perform itself. `context` is passed back. */
typedef struct {
    void (*hint)(void *context, const char *text);
    void (*get_model_text)(void *context, char *out, size_t cap);
    void (*set_model_text)(void *context, const char *text);
    void (*draw_rows)(void *context);
    /* Rebuild the effort popup for `model`; NULL info means no catalog row. */
    void (*show_effort)(void *context, const AgentModelInfo *info, const char *model);
    /* Begin one catalog GET of `path`. Returns a PREFS_SEND_* code. */
    int (*send)(void *context, const char *path);
    /* Current time in ticks; read after a send succeeds, so the fetch
     * deadline excludes the send's own setup. */
    uint32_t (*now)(void *context);
    /* Advance the exchange one step. PREFS_POLL_DONE means a 200 response and
     * sets `body` and `len`; PREFS_POLL_ERROR covers transport failures and
     * any other status. */
    int (*poll)(void *context, const char **body, size_t *len);
    /* Close the exchange's connection after a terminal poll or a teardown. */
    void (*close)(void *context);
} PrefsModelHost;

typedef struct {
    PrefsModelHost host;
    void *context;
    PrefsModelCache *cache;
    const char *saved_model;         /* the model in the preferences file */
    AgentModelRow rows[AGENT_MODEL_ROWS_MAX];
    int count;
    int selected;                    /* index into rows, -1 when none */
    int visible[AGENT_MODEL_ROWS_MAX];
    int visible_count;
    int edited;                      /* the model field changed by hand */
    char filter[CHAT_MODEL_CAP];
    char last_text[CHAT_MODEL_CAP];
    char confirmed_id[CHAT_MODEL_CAP];
    AgentModelInfo confirmed_info;   /* metadata behind confirmed_id */
    int fetch;                       /* PREFS_FETCH_* */
    int draining;                    /* timed-out exchange still closing */
    uint32_t fetch_start;
} PrefsModel;

/* Zero `m` and bind its host, cache and the saved model id. */
void prefs_model_init(PrefsModel *m, const PrefsModelHost *host, void *context,
                      PrefsModelCache *cache, const char *saved_model);

/* Dialog opened: reuse the page cached earlier this launch, or start the
 * popular fetch. Reads the model field through the host. */
void prefs_model_begin(PrefsModel *m);

/* Recompute `visible` from `filter`; the empty filter shows every row. */
void prefs_model_filter(PrefsModel *m);

/* A picked row confirms the model and its efforts. */
void prefs_model_confirm(PrefsModel *m, int index);

/* Find: search the typed text, or show the popular page for an empty box. */
void prefs_model_find(PrefsModel *m);

/* Start one catalog exchange; no other may start until it reaches a terminal
 * state. Returns 0, or -1 with a hint explaining why. */
int prefs_model_start(PrefsModel *m, int kind, const char *query);

/* The exchange ended without a usable page. */
void prefs_model_fetch_failed(PrefsModel *m, int timed_out);

/* The exchange ended with a 200 response `body`. Ignored when no exchange is
 * in flight, so a stray completion cannot replace a confirmed list. */
void prefs_model_fetch_complete(PrefsModel *m, const char *body, size_t len);

/* The model field read `text`. Returns 0 when it still equals the last text
 * the picker saw; otherwise records it, makes it the row filter, marks the
 * field hand-edited and returns 1. Programmatic sets keep last_text in sync,
 * so only hand edits report a change. */
int prefs_model_text_changed(PrefsModel *m, const char *text);

/* Whether `text` names the confirmed row. */
int prefs_model_is_confirmed(const PrefsModel *m, const char *text);

/* One network step per pass while an exchange is in flight or draining: the
 * deadline moves an overdue exchange to draining, a timed-out connect is not
 * torn down until the drain deadline passes, and a terminal poll completes or
 * fails the fetch. A no-op when idle. */
void prefs_model_step(PrefsModel *m);

/* The dialog was blocked (window drag, popup tracking) while no step could
 * run: restart the deadline so the wait does not count against it. */
void prefs_model_resume(PrefsModel *m);

/* After a successful save of a confirmed row: keep it as the saved model's
 * row for later opens this launch. */
void prefs_model_remember_saved(PrefsModel *m, const char *model);
#endif
