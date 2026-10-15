#include "app/prefs_model.h"
#include <stdio.h>
#include <string.h>

void prefs_model_init(PrefsModel *m, const PrefsModelHost *host, void *context,
                      PrefsModelCache *cache, const char *saved_model)
{
    memset(m, 0, sizeof(*m));
    m->host = *host;
    m->context = context;
    m->cache = cache;
    m->saved_model = saved_model;
    m->selected = -1;
}

static void hint(PrefsModel *m, const char *text)
{
    m->host.hint(m->context, text);
}

static void adopt_row(PrefsModel *m, int at)
{
    m->selected = at;
    m->confirmed_info = m->rows[at].info;
    strcpy(m->confirmed_id, m->rows[at].id);
    m->host.show_effort(m->context, &m->confirmed_info, m->confirmed_id);
}

void prefs_model_filter(PrefsModel *m)
{
    int i;
    m->visible_count = 0;
    for (i = 0; i < m->count; i++)
        if (agent_model_row_match(&m->rows[i], m->filter))
            m->visible[m->visible_count++] = i;
}

void prefs_model_confirm(PrefsModel *m, int index)
{
    m->selected = index;
    m->edited = 0;
    m->confirmed_info = m->rows[index].info;
    strcpy(m->confirmed_id, m->rows[index].id);
    m->host.set_model_text(m->context, m->rows[index].id);
    strcpy(m->last_text, m->rows[index].id);
    m->filter[0] = 0;
    m->host.show_effort(m->context, &m->confirmed_info, m->confirmed_id);
    hint(m, "Model confirmed. Press OK to save.");
    m->host.draw_rows(m->context);
}

/* Put the saved model's row first in the list and confirm it, so its efforts
 * are choosable without a search. The page holds at most one row per model, so
 * the last row makes room when the list is full. */
static void saved_show(PrefsModel *m, const AgentModelRow *row)
{
    int keep = m->count < AGENT_MODEL_ROWS_MAX ? m->count : AGENT_MODEL_ROWS_MAX - 1;
    memmove(&m->rows[1], &m->rows[0], (size_t)keep * sizeof(AgentModelRow));
    m->rows[0] = *row;
    m->count = keep + 1;
    m->selected = 0;
    m->confirmed_info = row->info;
    strcpy(m->confirmed_id, row->id);
    m->host.show_effort(m->context, &m->confirmed_info, m->confirmed_id);
}

/* The saved model is not in the list: show its row from this launch's cache,
 * or fetch it. Skipped once the field names another model or a row is
 * already confirmed. Returns 1 when a fetch was started. */
static int show_saved_model(PrefsModel *m, uint32_t now)
{
    char text[CHAT_MODEL_CAP];
    m->host.get_model_text(m->context, text, sizeof(text));
    if (m->confirmed_id[0] || strcmp(text, m->saved_model)) return 0;
    if (m->cache->saved_loaded && !strcmp(m->cache->saved_row.id, m->saved_model)) {
        saved_show(m, &m->cache->saved_row);
        return 0;
    }
    return prefs_model_start(m, PREFS_FETCH_SAVED, m->saved_model, now) == 0;
}

void prefs_model_begin(PrefsModel *m, uint32_t now)
{
    char model[CHAT_MODEL_CAP];
    m->host.get_model_text(m->context, model, sizeof(model));
    strcpy(m->last_text, model);
    if (m->cache->popular_loaded) {
        int at;
        /* Later opens reuse the page fetched earlier this launch, so the
         * popular models appear without another fetch. */
        m->count = m->cache->popular_count;
        memcpy(m->rows, m->cache->popular, (size_t)m->count * sizeof(AgentModelRow));
        at = agent_model_row_find(m->rows, m->count, model);
        if (at >= 0) adopt_row(m, at);
        hint(m, "Type to filter, or press Find to search.");
        if (at < 0) show_saved_model(m, now);
        m->host.draw_rows(m->context);
    } else if (prefs_model_start(m, PREFS_FETCH_POPULAR, NULL, now) == 0) {
        m->host.draw_rows(m->context);
    }
}

void prefs_model_find(PrefsModel *m, uint32_t now)
{
    char text[CHAT_MODEL_CAP];
    if (m->fetch || m->draining) return;
    m->host.get_model_text(m->context, text, sizeof(text));
    strcpy(m->last_text, text);
    m->filter[0] = 0;
    if (!text[0]) {
        if (m->count) {
            hint(m, "Type to filter, or enter a model ID and press Find.");
            m->host.draw_rows(m->context);
        } else prefs_model_start(m, PREFS_FETCH_POPULAR, NULL, now);
        return;
    }
    prefs_model_start(m, PREFS_FETCH_SEARCH, text, now);
}

int prefs_model_start(PrefsModel *m, int kind, const char *query, uint32_t now)
{
    char path[CHAT_MODEL_CAP * 3 + 48];
    int length, sent;
    if (kind == PREFS_FETCH_POPULAR) length = agent_popular_query(path, sizeof(path));
    else length = agent_model_query(path, sizeof(path), query);
    if (length < 0) { hint(m, "That model name is too long for a catalog search."); return -1; }
    sent = m->host.send(m->context, path);
    if (sent == PREFS_SEND_TOO_LARGE) { hint(m, "The catalog request is too large."); return -1; }
    if (sent != PREFS_SEND_OK) { hint(m, "Could not start the catalog fetch."); return -1; }
    m->fetch = kind;
    m->fetch_start = now;
    hint(m, kind == PREFS_FETCH_POPULAR ? "Loading the popular models..." :
            kind == PREFS_FETCH_SAVED ? "Loading the saved model..." :
            kind == PREFS_FETCH_VALIDATE ? "Checking that model against the catalog..." :
            "Searching the catalog...");
    return 0;
}

void prefs_model_fetch_failed(PrefsModel *m, int timed_out)
{
    m->fetch = PREFS_FETCH_NONE;
    m->draining = 0;
    hint(m, timed_out ? "The catalog fetch timed out. Try Find again."
                      : "Could not load the catalog. Try Find again.");
}

/* A completed exchange: the page replaces the rows. Validate confirms only an
 * exact id and always lists the candidates; the one popular load is cached for
 * later opens, where a saved model still in the page resolves at once. */
void prefs_model_fetch_complete(PrefsModel *m, const char *body, size_t len, uint32_t now)
{
    static AgentModelRow page[AGENT_MODEL_ROWS_MAX];
    char text[CHAT_MODEL_CAP], line[128];
    int kind = m->fetch, at, count;
    if (kind == PREFS_FETCH_SAVED) {
        /* The saved model's row joins the list it is not part of; the
         * rows already shown stay as they are. */
        count = agent_model_page(body, len, page, AGENT_MODEL_ROWS_MAX);
        at = agent_model_row_find(page, count, m->saved_model);
        m->fetch = PREFS_FETCH_NONE;
        if (at >= 0) {
            m->cache->saved_row = page[at];
            m->cache->saved_loaded = 1;
            show_saved_model(m, now);
        }
        hint(m, "Type to filter, or press Find to search.");
        m->host.draw_rows(m->context);
        return;
    }
    count = agent_model_page(body, len, m->rows, AGENT_MODEL_ROWS_MAX);
    /* An empty popular page is a failure, not a list to cache for the launch:
     * a malformed or oversized page parses to zero rows. A search or validate
     * page with no rows is the catalog's answer for a typo. */
    if (count == 0 && kind == PREFS_FETCH_POPULAR) { prefs_model_fetch_failed(m, 0); return; }
    m->fetch = PREFS_FETCH_NONE;
    m->count = count;
    m->selected = -1;
    m->host.get_model_text(m->context, text, sizeof(text));
    strcpy(m->last_text, text);
    /* Text typed while the popular page loaded filters it. Search and validate
     * pages were already matched by the server, so they list unfiltered and the
     * hint's count is what the user sees. */
    m->filter[0] = 0;
    if (kind == PREFS_FETCH_POPULAR && m->edited) strcpy(m->filter, text);
    if (kind == PREFS_FETCH_VALIDATE) {
        at = agent_model_row_find(m->rows, m->count, text);
        if (at >= 0) {
            adopt_row(m, at);
            hint(m, "Model confirmed. Press OK again to save.");
        } else hint(m, "No exact match. Pick a candidate or press Find.");
    } else if (kind == PREFS_FETCH_POPULAR) {
        memcpy(m->cache->popular, m->rows, (size_t)m->count * sizeof(AgentModelRow));
        m->cache->popular_count = m->count;
        m->cache->popular_loaded = 1;
        at = agent_model_row_find(m->rows, m->count, text);
        if (at >= 0) adopt_row(m, at);
        hint(m, "Popular models loaded. Type to filter or Find.");
        if (at < 0) show_saved_model(m, now);
    } else {
        snprintf(line, sizeof(line), "%d match%s. Click a row to pick it.",
                 m->count, m->count == 1 ? "" : "es");
        hint(m, line);
    }
    m->edited = 0;
    m->host.draw_rows(m->context);
}

void prefs_model_remember_saved(PrefsModel *m, const char *model)
{
    m->cache->saved_row.info = m->confirmed_info;
    strcpy(m->cache->saved_row.id, model);
    m->cache->saved_loaded = 1;
}
