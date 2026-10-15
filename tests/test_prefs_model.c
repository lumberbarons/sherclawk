/* Preferences model picker: local filtering, the catalog fetch transitions and
 * row confirmation, driven through a fake PrefsModelHost with the real catalog
 * page parser. */
#include "app/prefs_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char model_text[CHAT_MODEL_CAP];
    char hint[160];
    int hints, draws, sends, sets;
    char set_text[CHAT_MODEL_CAP];
    char sent_path[256];
    int send_result;
    uint32_t tick;
    uint32_t send_tick_step;   /* the send itself takes this many ticks */
    int efforts;
    int effort_has_info;
    char effort_model[CHAT_MODEL_CAP];
    long effort_context;
} Fake;

static void fake_hint(void *c, const char *text)
{
    Fake *f = c;
    snprintf(f->hint, sizeof(f->hint), "%s", text);
    f->hints++;
}

static void fake_get(void *c, char *out, size_t cap)
{
    snprintf(out, cap, "%s", ((Fake *)c)->model_text);
}

static void fake_set(void *c, const char *text)
{
    Fake *f = c;
    snprintf(f->set_text, sizeof(f->set_text), "%s", text);
    snprintf(f->model_text, sizeof(f->model_text), "%s", text);
    f->sets++;
}

static void fake_draw(void *c) { ((Fake *)c)->draws++; }

static void fake_effort(void *c, const AgentModelInfo *info, const char *model)
{
    Fake *f = c;
    f->efforts++;
    f->effort_has_info = info != NULL;
    f->effort_context = info ? info->context_length : -2;
    snprintf(f->effort_model, sizeof(f->effort_model), "%s", model);
}

static int fake_send(void *c, const char *path)
{
    Fake *f = c;
    snprintf(f->sent_path, sizeof(f->sent_path), "%s", path);
    f->sends++;
    f->tick += f->send_tick_step;
    return f->send_result;
}

static uint32_t fake_now(void *c) { return ((Fake *)c)->tick; }

static const PrefsModelHost kHost = { fake_hint, fake_get, fake_set, fake_draw, fake_effort, fake_send, fake_now };

static const char kPopular[] =
    "{\"data\":["
    "{\"id\":\"openai/gpt-4o\",\"name\":\"GPT-4o\",\"context_length\":131072,"
    "\"reasoning\":{\"mandatory\":false,\"default_effort\":\"medium\",\"supported_efforts\":[\"high\",\"medium\"]}},"
    "{\"id\":\"openai/gpt-4o-mini\",\"name\":\"GPT-4o mini\",\"context_length\":65536},"
    "{\"id\":\"z-ai/glm\",\"name\":\"GLM\",\"context_length\":8192}]}";

static void setup(PrefsModel *m, Fake *f, PrefsModelCache *cache, const char *saved)
{
    memset(f, 0, sizeof(*f));
    memset(cache, 0, sizeof(*cache));
    snprintf(f->model_text, sizeof(f->model_text), "%s", saved);
    prefs_model_init(m, &kHost, f, cache, saved);
}

static void complete(PrefsModel *m, const char *page)
{
    prefs_model_fetch_complete(m, page, strlen(page));
}

static void test_init_is_empty(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "z-ai/glm");
    assert(m.count == 0 && m.selected == -1 && m.fetch == PREFS_FETCH_NONE && !m.draining);
    assert(!m.confirmed_id[0] && !m.filter[0] && f.hints == 0 && f.sends == 0);
}

static void test_filter_matches_id_and_name_case_insensitively(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "");
    prefs_model_begin(&m);
    complete(&m, kPopular);
    prefs_model_filter(&m);
    assert(m.visible_count == 3 && m.visible[0] == 0 && m.visible[2] == 2);
    strcpy(m.filter, "GPT-4O");
    prefs_model_filter(&m);
    assert(m.visible_count == 2 && m.visible[0] == 0 && m.visible[1] == 1);
    strcpy(m.filter, "mini");
    prefs_model_filter(&m);
    assert(m.visible_count == 1 && m.visible[0] == 1);
    strcpy(m.filter, "nothing");
    prefs_model_filter(&m);
    assert(m.visible_count == 0);
    m.filter[0] = 0;
    prefs_model_filter(&m);
    assert(m.visible_count == 3);
}

static void test_confirm_sets_field_filter_hint_and_effort(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "");
    prefs_model_begin(&m);
    complete(&m, kPopular);
    strcpy(m.filter, "gpt");
    m.edited = 1;
    f.efforts = f.draws = 0;
    prefs_model_confirm(&m, 1);
    assert(m.selected == 1 && !m.edited && !m.filter[0]);
    assert(!strcmp(m.confirmed_id, "openai/gpt-4o-mini") && m.confirmed_info.context_length == 65536);
    assert(!strcmp(f.set_text, "openai/gpt-4o-mini") && !strcmp(m.last_text, "openai/gpt-4o-mini"));
    assert(f.sets == 1);
    assert(f.efforts == 1 && f.effort_has_info && f.effort_context == 65536 &&
           !strcmp(f.effort_model, "openai/gpt-4o-mini"));
    assert(!strcmp(f.hint, "Model confirmed. Press OK to save."));
    assert(f.draws == 1);
}

static void test_begin_without_cache_starts_popular_fetch(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "z-ai/glm");
    f.tick = 77;
    f.send_tick_step = 5;
    prefs_model_begin(&m);
    assert(f.sends == 1 && !strcmp(f.sent_path, "/api/v1/models?limit=10&sort=most-popular"));
    /* The deadline starts once the send has set up, not before it. */
    assert(m.fetch == PREFS_FETCH_POPULAR && m.fetch_start == 82 && f.draws == 1);
    assert(!strcmp(f.hint, "Loading the popular models..."));
    assert(!strcmp(m.last_text, "z-ai/glm"));
}

static void test_begin_send_failure_leaves_no_fetch_and_no_draw(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "");
    f.send_result = PREFS_SEND_FAILED;
    prefs_model_begin(&m);
    assert(m.fetch == PREFS_FETCH_NONE && f.draws == 0);
    assert(!strcmp(f.hint, "Could not start the catalog fetch."));
    f.send_result = PREFS_SEND_TOO_LARGE;
    assert(prefs_model_start(&m, PREFS_FETCH_SEARCH, "x") == -1);
    assert(m.fetch == PREFS_FETCH_NONE && !strcmp(f.hint, "The catalog request is too large."));
}

static void test_start_rejects_an_overlong_query_before_sending(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    static char huge[CHAT_MODEL_CAP * 4];
    setup(&m, &f, &cache, "");
    memset(huge, 'a', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = 0;
    assert(prefs_model_start(&m, PREFS_FETCH_SEARCH, huge) == -1);
    assert(f.sends == 0 && m.fetch == PREFS_FETCH_NONE);
    assert(!strcmp(f.hint, "That model name is too long for a catalog search."));
}

static void test_start_hints_name_each_kind(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "");
    assert(prefs_model_start(&m, PREFS_FETCH_SEARCH, "gpt") == 0);
    assert(m.fetch == PREFS_FETCH_SEARCH && !strcmp(f.hint, "Searching the catalog..."));
    assert(!strcmp(f.sent_path, "/api/v1/models?q=gpt&limit=10"));
    assert(prefs_model_start(&m, PREFS_FETCH_VALIDATE, "gpt") == 0);
    assert(!strcmp(f.hint, "Checking that model against the catalog..."));
    assert(prefs_model_start(&m, PREFS_FETCH_SAVED, "gpt") == 0);
    assert(!strcmp(f.hint, "Loading the saved model..."));
}

static void test_popular_complete_caches_and_confirms_the_saved_model(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "openai/gpt-4o");
    prefs_model_begin(&m);
    f.efforts = f.draws = 0;
    complete(&m, kPopular);
    assert(m.fetch == PREFS_FETCH_NONE && m.count == 3 && m.selected == 0);
    assert(!strcmp(m.confirmed_id, "openai/gpt-4o") && m.confirmed_info.context_length == 131072);
    assert(f.efforts == 1 && f.effort_has_info && f.effort_context == 131072 &&
           !strcmp(f.effort_model, "openai/gpt-4o"));
    assert(cache.popular_loaded && cache.popular_count == 3 && !strcmp(cache.popular[2].id, "z-ai/glm"));
    assert(!strcmp(f.hint, "Popular models loaded. Type to filter or Find."));
    assert(f.draws == 1 && !m.edited && f.sends == 1);
}

static void test_popular_complete_filters_by_text_typed_while_loading(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "");
    prefs_model_begin(&m);
    strcpy(f.model_text, "glm");
    m.edited = 1;
    complete(&m, kPopular);
    assert(!strcmp(m.filter, "glm") && !strcmp(m.last_text, "glm") && !m.edited);
    prefs_model_filter(&m);
    assert(m.visible_count == 1 && m.visible[0] == 2);
}

static void test_empty_popular_page_is_a_failure_and_is_not_cached(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "");
    prefs_model_begin(&m);
    complete(&m, "{\"data\":[]}");
    assert(m.fetch == PREFS_FETCH_NONE && m.count == 0 && !cache.popular_loaded);
    assert(!strcmp(f.hint, "Could not load the catalog. Try Find again."));
    prefs_model_begin(&m);
    complete(&m, "not json");
    assert(!cache.popular_loaded);
}

static void test_search_complete_lists_unfiltered_with_a_match_count(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "");
    strcpy(f.model_text, "gpt");
    prefs_model_find(&m);
    assert(m.fetch == PREFS_FETCH_SEARCH && !strcmp(f.sent_path, "/api/v1/models?q=gpt&limit=10"));
    m.filter[0] = 'x';
    complete(&m, kPopular);
    assert(m.count == 3 && m.selected == -1 && !m.filter[0] && !m.confirmed_id[0]);
    assert(!strcmp(f.hint, "3 matches. Click a row to pick it."));
    assert(!cache.popular_loaded);
    prefs_model_find(&m);
    complete(&m, "{\"data\":[{\"id\":\"only/one\",\"name\":\"One\",\"context_length\":1}]}");
    assert(m.count == 1 && !strcmp(f.hint, "1 match. Click a row to pick it."));
    prefs_model_find(&m);
    complete(&m, "{\"data\":[]}");
    assert(m.count == 0 && !strcmp(f.hint, "0 matches. Click a row to pick it."));
}

static void test_validate_confirms_only_an_exact_id(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "z-ai/glm");
    strcpy(f.model_text, "openai/gpt-4o-mini");
    prefs_model_start(&m, PREFS_FETCH_VALIDATE, f.model_text);
    f.efforts = 0;
    complete(&m, kPopular);
    assert(m.selected == 1 && !strcmp(m.confirmed_id, "openai/gpt-4o-mini") && f.efforts == 1);
    assert(!strcmp(f.hint, "Model confirmed. Press OK again to save."));
    assert(!cache.popular_loaded);

    memset(&m.confirmed_id, 0, sizeof(m.confirmed_id));
    strcpy(f.model_text, "gpt-4o");
    prefs_model_start(&m, PREFS_FETCH_VALIDATE, f.model_text);
    f.efforts = 0;
    complete(&m, kPopular);
    assert(m.selected == -1 && !m.confirmed_id[0] && f.efforts == 0 && m.count == 3);
    assert(!strcmp(f.hint, "No exact match. Pick a candidate or press Find."));
}

static void test_saved_model_missing_from_popular_is_fetched_then_listed_first(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    static const char saved_page[] =
        "{\"data\":[{\"id\":\"old/model\",\"name\":\"Old\",\"context_length\":4096}]}";
    setup(&m, &f, &cache, "old/model");
    prefs_model_begin(&m);
    f.tick = 500;
    complete(&m, kPopular);
    /* The popular page lacks the saved id: its own row is requested. */
    assert(m.fetch == PREFS_FETCH_SAVED && m.fetch_start == 500 && f.sends == 2);
    assert(!strcmp(f.sent_path, "/api/v1/models?q=old%2Fmodel&limit=10") ||
           !strcmp(f.sent_path, "/api/v1/models?q=old/model&limit=10"));
    assert(m.count == 3 && m.selected == -1);
    f.efforts = 0;
    prefs_model_fetch_complete(&m, saved_page, strlen(saved_page));
    assert(m.fetch == PREFS_FETCH_NONE && m.count == 4 && m.selected == 0);
    assert(!strcmp(m.rows[0].id, "old/model") && !strcmp(m.rows[1].id, "openai/gpt-4o"));
    assert(!strcmp(m.confirmed_id, "old/model") && f.efforts == 1);
    assert(cache.saved_loaded && !strcmp(cache.saved_row.id, "old/model"));
    assert(!strcmp(f.hint, "Type to filter, or press Find to search."));
}

static void test_saved_row_makes_room_when_the_page_is_full(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    static char page[4096];
    size_t at = 0;
    int i;
    at += (size_t)snprintf(page + at, sizeof(page) - at, "{\"data\":[");
    for (i = 0; i < AGENT_MODEL_ROWS_MAX; i++)
        at += (size_t)snprintf(page + at, sizeof(page) - at, "%s{\"id\":\"p/%d\",\"context_length\":%d}",
                               i ? "," : "", i, 1000 + i);
    snprintf(page + at, sizeof(page) - at, "]}");
    setup(&m, &f, &cache, "old/model");
    cache.saved_loaded = 1;
    strcpy(cache.saved_row.id, "old/model");
    cache.saved_row.info.context_length = 4096;
    prefs_model_begin(&m);
    complete(&m, page);
    /* The cached saved row joins at once, no second fetch. */
    assert(f.sends == 1 && m.fetch == PREFS_FETCH_NONE);
    assert(m.count == AGENT_MODEL_ROWS_MAX && m.selected == 0);
    assert(!strcmp(m.rows[0].id, "old/model") && !strcmp(m.rows[1].id, "p/0"));
    assert(!strcmp(m.rows[AGENT_MODEL_ROWS_MAX - 1].id, "p/8"));
    assert(m.confirmed_info.context_length == 4096);
}

static void test_saved_fetch_without_a_match_keeps_the_rows(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "old/model");
    prefs_model_begin(&m);
    complete(&m, kPopular);
    assert(m.fetch == PREFS_FETCH_SAVED);
    complete(&m, "{\"data\":[{\"id\":\"someone/else\",\"context_length\":1}]}");
    assert(m.fetch == PREFS_FETCH_NONE && m.count == 3 && m.selected == -1);
    assert(!cache.saved_loaded && !m.confirmed_id[0]);
}

static void test_saved_model_is_skipped_once_the_field_names_another(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "old/model");
    prefs_model_begin(&m);
    strcpy(f.model_text, "typed/other");
    complete(&m, kPopular);
    assert(m.fetch == PREFS_FETCH_NONE && f.sends == 1 && m.count == 3);
}

static void test_begin_with_a_cached_popular_page_reuses_it(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "z-ai/glm");
    prefs_model_begin(&m);
    complete(&m, kPopular);
    memset(&m, 0, sizeof(m));
    f.sends = f.draws = f.efforts = 0;
    prefs_model_init(&m, &kHost, &f, &cache, "z-ai/glm");
    prefs_model_begin(&m);
    assert(f.sends == 0 && m.fetch == PREFS_FETCH_NONE && m.count == 3);
    assert(m.selected == 2 && !strcmp(m.confirmed_id, "z-ai/glm") && f.efforts == 1);
    assert(!strcmp(f.hint, "Type to filter, or press Find to search.") && f.draws == 1);
}

static void test_begin_with_cache_and_saved_model_absent_uses_the_saved_row(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "old/model");
    prefs_model_begin(&m);
    complete(&m, kPopular);
    complete(&m, "{\"data\":[{\"id\":\"old/model\",\"context_length\":4096}]}");
    assert(cache.saved_loaded);
    prefs_model_init(&m, &kHost, &f, &cache, "old/model");
    f.sends = 0;
    prefs_model_begin(&m);
    assert(f.sends == 0 && m.count == 4 && m.selected == 0 && !strcmp(m.rows[0].id, "old/model"));
}

static void test_begin_with_cache_and_no_saved_row_fetches_it(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "old/model");
    cache.popular_loaded = 1;
    cache.popular_count = 1;
    strcpy(cache.popular[0].id, "a/b");
    f.tick = 900;
    prefs_model_begin(&m);
    assert(m.count == 1 && m.fetch == PREFS_FETCH_SAVED && m.fetch_start == 900 && f.sends == 1);
    assert(f.draws == 1);
}

static void test_find_is_ignored_while_an_exchange_is_in_flight(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "");
    strcpy(f.model_text, "gpt");
    m.fetch = PREFS_FETCH_POPULAR;
    prefs_model_find(&m);
    m.fetch = PREFS_FETCH_NONE;
    m.draining = 1;
    prefs_model_find(&m);
    assert(f.sends == 0 && f.hints == 0);
}

static void test_find_with_an_empty_box_shows_rows_or_loads_popular(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "openai/gpt-4o");
    f.model_text[0] = 0;
    prefs_model_find(&m);
    assert(m.fetch == PREFS_FETCH_POPULAR && f.sends == 1);
    complete(&m, kPopular);
    f.sends = f.draws = 0;
    strcpy(m.filter, "glm");
    prefs_model_find(&m);
    assert(f.sends == 0 && !m.filter[0] && f.draws == 1);
    assert(!strcmp(f.hint, "Type to filter, or enter a model ID and press Find."));
}

static void test_fetch_failed_resets_state_and_names_the_cause(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "");
    m.fetch = PREFS_FETCH_SEARCH;
    m.draining = 1;
    prefs_model_fetch_failed(&m, 1);
    assert(m.fetch == PREFS_FETCH_NONE && !m.draining);
    assert(!strcmp(f.hint, "The catalog fetch timed out. Try Find again."));
    prefs_model_fetch_failed(&m, 0);
    assert(!strcmp(f.hint, "Could not load the catalog. Try Find again."));
}

static void test_remember_saved_records_the_confirmed_row_for_later_opens(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "z-ai/glm");
    prefs_model_begin(&m);
    complete(&m, kPopular);
    prefs_model_confirm(&m, 1);
    prefs_model_remember_saved(&m, "openai/gpt-4o-mini");
    assert(cache.saved_loaded && !strcmp(cache.saved_row.id, "openai/gpt-4o-mini"));
    assert(cache.saved_row.info.context_length == 65536);
}

static void test_stray_completion_leaves_the_list_alone(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    setup(&m, &f, &cache, "z-ai/glm");
    prefs_model_begin(&m);
    complete(&m, kPopular);
    assert(m.fetch == PREFS_FETCH_NONE && m.selected == 2);
    f.hints = f.draws = 0;
    complete(&m, "{\"data\":[{\"id\":\"other/x\",\"context_length\":1}]}");
    assert(m.count == 3 && m.selected == 2 && !strcmp(m.rows[0].id, "openai/gpt-4o"));
    assert(!strcmp(m.confirmed_id, "z-ai/glm") && f.hints == 0 && f.draws == 0);
    /* After a failure too. */
    m.fetch = PREFS_FETCH_SEARCH;
    prefs_model_fetch_failed(&m, 0);
    complete(&m, kPopular);
    assert(m.count == 3 && m.selected == 2);
}

static void test_rejected_popular_page_keeps_the_shown_rows(void)
{
    PrefsModel m; Fake f; static PrefsModelCache cache;
    static const char truncated[] =
        "{\"data\":[{\"id\":\"new/a\",\"context_length\":1},{\"id\":\"new/b\",\"context_length\":";
    setup(&m, &f, &cache, "z-ai/glm");
    prefs_model_begin(&m);
    complete(&m, kPopular);
    assert(m.count == 3);
    prefs_model_find(&m);   /* empty box with rows: no fetch */
    strcpy(f.model_text, "gpt");
    prefs_model_find(&m);
    assert(m.fetch == PREFS_FETCH_SEARCH);
    m.fetch = PREFS_FETCH_POPULAR;   /* a popular exchange completing with a bad page */
    complete(&m, truncated);
    assert(m.fetch == PREFS_FETCH_NONE && m.count == 3);
    assert(!strcmp(m.rows[0].id, "openai/gpt-4o") && !strcmp(m.rows[2].id, "z-ai/glm"));
    assert(m.selected == 2 && !strcmp(f.hint, "Could not load the catalog. Try Find again."));
}

int main(void)
{
    test_init_is_empty();
    test_filter_matches_id_and_name_case_insensitively();
    test_confirm_sets_field_filter_hint_and_effort();
    test_begin_without_cache_starts_popular_fetch();
    test_begin_send_failure_leaves_no_fetch_and_no_draw();
    test_start_rejects_an_overlong_query_before_sending();
    test_start_hints_name_each_kind();
    test_popular_complete_caches_and_confirms_the_saved_model();
    test_popular_complete_filters_by_text_typed_while_loading();
    test_empty_popular_page_is_a_failure_and_is_not_cached();
    test_search_complete_lists_unfiltered_with_a_match_count();
    test_validate_confirms_only_an_exact_id();
    test_saved_model_missing_from_popular_is_fetched_then_listed_first();
    test_saved_row_makes_room_when_the_page_is_full();
    test_saved_fetch_without_a_match_keeps_the_rows();
    test_saved_model_is_skipped_once_the_field_names_another();
    test_begin_with_a_cached_popular_page_reuses_it();
    test_begin_with_cache_and_saved_model_absent_uses_the_saved_row();
    test_begin_with_cache_and_no_saved_row_fetches_it();
    test_find_is_ignored_while_an_exchange_is_in_flight();
    test_find_with_an_empty_box_shows_rows_or_loads_popular();
    test_fetch_failed_resets_state_and_names_the_cause();
    test_remember_saved_records_the_confirmed_row_for_later_opens();
    test_stray_completion_leaves_the_list_alone();
    test_rejected_popular_page_keeps_the_shown_rows();
    puts("prefs model checks passed");
    return 0;
}
