/* Preferences parser/formatter regressions: defaults, tolerated syntax,
 * malformed and boundary values. Runs under ASan/UBSan via tools/check.sh. */
#include "app/preferences.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static Prefs p, base;
static char buf[PREFS_MAX_BYTES];

static void defaults(void)
{
    prefs_defaults(&p);
    assert(!strcmp(p.model, SHERCLAWK_MODEL));
    assert(!strcmp(p.api_key, SHERCLAWK_API_KEY));
    assert(!strcmp(p.workspace, SHERCLAWK_WORKSPACE));
    assert(!p.effort[0]);
    assert(p.context == 0 && !p.context_for[0]);
    assert(p.vision == AGENT_VISION_UNKNOWN && !p.default_effort[0]);
    assert(p.max_rounds == AGENT_TURN_MAX && p.max_tools == AGENT_TOOL_MAX);
    assert(p.show_tool_debug == 0);
}

static void full_file(void)
{
    const char *text = "model=openai/gpt-6-luna\r"
        "api_key=sk-or-v1-abcdef\r"
        "workspace=Macintosh HD:Projects:\r"
        "max_rounds=2\r"
        "max_tools=1\r"
        "show_tool_debug=1\r"
        "effort=high\r"
        "context=262144\r"
        "context_for=openai/gpt-6-luna\r"
        "vision=2\r"
        "default_effort=medium\r";
    prefs_defaults(&p);
    assert(prefs_parse(text, strlen(text), &p) == 11);
    assert(!strcmp(p.model, "openai/gpt-6-luna"));
    assert(!strcmp(p.api_key, "sk-or-v1-abcdef"));
    assert(!strcmp(p.workspace, "Macintosh HD:Projects:"));
    assert(p.max_rounds == 2 && p.max_tools == 1 && p.show_tool_debug == 1);
    assert(!strcmp(p.effort, "high"));
    assert(p.context == 262144 && !strcmp(p.context_for, "openai/gpt-6-luna"));
    assert(p.vision == AGENT_VISION_YES && !strcmp(p.default_effort, "medium"));
}

static void syntax_tolerated(void)
{
    /* LF, CRLF, blank lines, comments, spaces around '=' and duplicate lines
     * (last wins) are all accepted; unknown keys are ignored. */
    const char *text = "\r\n# comment\r"
        "  model = first/model\r"
        "\nmodel=second/model\n"
        "unknown=key\r"
        "max_rounds =\t16\t\r"
        "show_tool_debug=1\r";
    prefs_defaults(&p);
    assert(prefs_parse(text, strlen(text), &p) == 4);
    assert(!strcmp(p.model, "second/model"));
    assert(p.max_rounds == 16 && p.show_tool_debug == 1);
}

static void malformed_keeps_defaults(void)
{
    const char *text = "model=has space\r"
        "model=\r"
        "api_key=bad key\r"
        "workspace=Retro68\r"
        "workspace=:leading:\r"
        "workspace=Retro68::Sub:\r"
        "workspace=Retro68/Sub:\r"
        "max_rounds=0\r"
        "max_rounds=129\r"
        "max_rounds=-1\r"
        "max_rounds=2x\r"
        "max_rounds=\r"
        "max_tools=999\r"
        "show_tool_debug=2\r"
        "show_tool_debug=true\r"
        "show_tool_debug=\r";
    prefs_defaults(&p);
    base = p;
    assert(prefs_parse(text, strlen(text), &p) == 0);
    assert(!strcmp(p.model, base.model));
    assert(!strcmp(p.api_key, base.api_key));
    assert(!strcmp(p.workspace, base.workspace));
    assert(p.max_rounds == base.max_rounds && p.max_tools == base.max_tools);
    assert(p.show_tool_debug == 0);
}

static void empty_key_clears(void)
{
    /* A present-but-empty api_key is an explicit clear, not a fallback. */
    const char *text = "api_key=\r";
    prefs_defaults(&p);
    strcpy(p.api_key, "compiled-key");
    assert(prefs_parse(text, strlen(text), &p) == 1);
    assert(p.api_key[0] == 0);
}

static void effort_values(void)
{
    /* The stored effort is empty or one exact provider effort name. */
    static const char *const names[] = {
        "max", "xhigh", "high", "medium", "low", "minimal", "none"
    };
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(*names); i++)
        assert(prefs_effort_ok(names[i]));
    assert(prefs_effort_ok("")); /* empty is the stored no-effort value */
    assert(!prefs_effort_ok("extreme") && !prefs_effort_ok("HIGH"));
    assert(!prefs_effort_ok("high ") && !prefs_effort_ok(" high"));
    assert(!prefs_effort_ok("reasoning"));
    {
        char long_effort[AGENT_EFFORT_CAP + 1];
        memset(long_effort, 'x', AGENT_EFFORT_CAP); long_effort[AGENT_EFFORT_CAP] = 0;
        assert(!prefs_effort_ok(long_effort)); /* no room for the terminator */
    }
    /* A known name is taken over the default; unknown, malformed and blank
     * values are refused and leave the previous one. */
    prefs_defaults(&p);
    assert(prefs_parse("effort=medium\r", 14, &p) == 1 && !strcmp(p.effort, "medium"));
    assert(prefs_parse("effort=extreme\r", 15, &p) == 0 && !strcmp(p.effort, "medium"));
    assert(prefs_parse("effort=HIGH\r", 12, &p) == 0 && !strcmp(p.effort, "medium"));
    assert(prefs_parse("effort=high low\r", 16, &p) == 0 && !strcmp(p.effort, "medium"));
    assert(prefs_parse("effort=\r", 8, &p) == 1 && !p.effort[0]); /* explicit clear */
}

static int parse_str(const char *text)
{
    return prefs_parse(text, strlen(text), &p);
}

static void context_values(void)
{
    assert(prefs_context_value("1") == 1 && prefs_context_value("1048576") == 1048576);
    assert(prefs_context_value("0") == 0 && prefs_context_value("") == 0); /* none */
    assert(prefs_context_value("-1") < 0 && prefs_context_value("12x") < 0);
    assert(prefs_context_value(" 5") < 0 && prefs_context_value("1.5") < 0);
    assert(prefs_context_value("100000001") < 0); /* over the ceiling */
    assert(prefs_context_value("99999999999999999999") < 0);
    /* Malformed context or context_for leaves the previous value. */
    prefs_defaults(&p);
    assert(parse_str("context=2048\rcontext_for=a/b\r") == 2);
    assert(p.context == 2048 && !strcmp(p.context_for, "a/b"));
    assert(parse_str("context=-5\rcontext=12x\rcontext_for=has space\r") == 0);
    assert(p.context == 2048 && !strcmp(p.context_for, "a/b"));
    /* Empty values are explicit clears. */
    assert(parse_str("context=\rcontext_for=\r") == 2);
    assert(p.context == 0 && !p.context_for[0]);
}

static void context_trust(void)
{
    /* The stored context is trusted only for the model it was stored for. */
    prefs_defaults(&p);
    strcpy(p.model, "openai/gpt-6-luna");
    assert(prefs_context_for(&p, p.model) == -1); /* nothing stored */
    p.context = 200000; strcpy(p.context_for, "openai/gpt-6-luna");
    assert(prefs_context_for(&p, "openai/gpt-6-luna") == 200000);
    assert(prefs_context_for(&p, "openai/gpt-6-luna:free") == -1); /* other model */
    assert(prefs_context_for(&p, "OPENAI/gpt-6-luna") == -1);
    p.context = 0;
    assert(prefs_context_for(&p, "openai/gpt-6-luna") == -1);     /* known unknown */
    p.context = 5; p.context_for[0] = 0;
    assert(prefs_context_for(&p, "") == -1);                       /* never matches blank */
    /* A hand-edited model with the old context_for falls back to a lookup. */
    {
        const char *text = "model=other/model\rcontext=1000\rcontext_for=openai/gpt-6-luna\r";
        prefs_defaults(&p);
        assert(prefs_parse(text, strlen(text), &p) == 3);
        assert(prefs_context_for(&p, p.model) == -1);
    }
}

static void catalog_row_values(void)
{
    /* vision is one of the three AGENT_VISION_* digits; default_effort follows
     * the effort rules. Anything else leaves the previous value. */
    prefs_defaults(&p);
    assert(parse_str("vision=1\rdefault_effort=low\r") == 2);
    assert(p.vision == AGENT_VISION_NO && !strcmp(p.default_effort, "low"));
    assert(parse_str("vision=3\rvision=yes\rvision=\rvision=12\r") == 0);
    assert(p.vision == AGENT_VISION_NO);
    assert(parse_str("default_effort=extreme\rdefault_effort=HIGH\r") == 0);
    assert(!strcmp(p.default_effort, "low"));
    assert(parse_str("vision=0\rdefault_effort=\r") == 2); /* explicit unknown / none */
    assert(p.vision == AGENT_VISION_UNKNOWN && !p.default_effort[0]);
    /* A value that cannot round-trip is refused by the formatter. */
    p.vision = 3;
    assert(prefs_format(&p, buf, sizeof(buf)) == -1);
    p.vision = AGENT_VISION_YES; strcpy(p.default_effort, "bad\neffort");
    assert(prefs_format(&p, buf, sizeof(buf)) == -1);
}

static void older_build_file(void)
{
    /* Files written before effort, context and context_for existed. */
    const char *text = "model=openai/gpt-6-luna\rapi_key=sk-x\rworkspace=Retro68:\r"
        "max_rounds=8\rmax_tools=9\rshow_tool_debug=1\r";
    prefs_defaults(&p);
    assert(prefs_parse(text, strlen(text), &p) == 6);
    assert(!p.effort[0] && p.context == 0 && !p.context_for[0]);
    assert(p.vision == AGENT_VISION_UNKNOWN && !p.default_effort[0]);
    assert(prefs_context_for(&p, p.model) == -1);
    assert(p.max_rounds == 8 && p.show_tool_debug == 1);
}

static void boundaries(void)
{
    char long_model[CHAT_MODEL_CAP + 1], long_key[PREFS_KEY_CAP + 1];
    char long_workspace[PREFS_WORKSPACE_MAX + 2];
    memset(long_model, 'm', CHAT_MODEL_CAP - 1); long_model[CHAT_MODEL_CAP - 1] = 0;
    memset(long_key, 'k', PREFS_KEY_CAP - 1); long_key[PREFS_KEY_CAP - 1] = 0;
    assert(prefs_model_ok(long_model)); /* 255 chars fits */
    assert(!prefs_model_ok(""));        /* empty model invalid */
    long_model[CHAT_MODEL_CAP - 1] = 'x'; long_model[CHAT_MODEL_CAP] = 0;
    assert(!prefs_model_ok(long_model)); /* 256 chars refused */
    assert(prefs_key_ok(long_key));
    assert(prefs_key_ok(""));
    long_key[PREFS_KEY_CAP - 1] = 'x'; long_key[PREFS_KEY_CAP] = 0;
    assert(!prefs_key_ok(long_key));
    memset(long_workspace, 'w', PREFS_WORKSPACE_MAX + 1);
    long_workspace[PREFS_WORKSPACE_MAX + 1] = 0;
    assert(!prefs_workspace_ok(long_workspace)); /* longer than the cap */
    assert(prefs_workspace_ok("Retro68:"));
    assert(prefs_workspace_ok("Macintosh HD:Projects:"));
    assert(!prefs_workspace_ok(""));
    assert(!prefs_workspace_ok(":"));
    assert(!prefs_workspace_ok("Retro68")); /* must end in a colon */
    {
        char component[40];
        memset(component, 'c', 32); component[32] = ':'; component[33] = 0;
        assert(!prefs_workspace_ok(component)); /* 32-byte HFS component */
        component[31] = ':'; component[32] = 0;
        assert(prefs_workspace_ok(component));  /* 31-byte component fits */
    }
    assert(prefs_limit_value("1") == 1 && prefs_limit_value("128") == 128);
    assert(prefs_limit_value("0") < 0 && prefs_limit_value("129") < 0);
    assert(prefs_limit_value("") < 0 && prefs_limit_value(" 32") < 0);
    assert(prefs_limit_value("1234") < 0);
}

static void over_long_lines_ignored(void)
{
    char line[PREFS_KEY_CAP + 16];
    char big[PREFS_KEY_CAP + 1];
    memset(big, 'x', PREFS_KEY_CAP); big[PREFS_KEY_CAP] = 0;
    prefs_defaults(&p);
    base = p;
    snprintf(line, sizeof(line), "model=%s\r", big); /* 1 + 256-byte value */
    assert(prefs_parse(line, strlen(line), &p) == 0);
    assert(!strcmp(p.model, base.model));
    snprintf(line, sizeof(line), "api_key=%s\r", big);
    assert(prefs_parse(line, strlen(line), &p) == 0);
    assert(!strcmp(p.api_key, base.api_key));
}

static void format_round_trip(void)
{
    int n;
    prefs_defaults(&p);
    strcpy(p.model, "openai/gpt-6-luna");
    strcpy(p.api_key, "sk-or-v1-quoted=value"); /* '=' must survive a round trip */
    strcpy(p.workspace, "Macintosh HD:Projects:");
    strcpy(p.effort, "minimal");
    p.context = 1000000; strcpy(p.context_for, "openai/gpt-6-luna");
    p.vision = AGENT_VISION_YES; strcpy(p.default_effort, "high");
    p.max_rounds = 7; p.max_tools = 128; p.show_tool_debug = 1;
    n = prefs_format(&p, buf, sizeof(buf));
    assert(n > 0 && buf[n - 1] == '\r');
    prefs_defaults(&base);
    assert(prefs_parse(buf, (size_t)n, &base) == 11);
    assert(!strcmp(base.model, p.model));
    assert(!strcmp(base.api_key, p.api_key));
    assert(!strcmp(base.workspace, p.workspace));
    assert(!strcmp(base.effort, "minimal"));
    assert(base.context == 1000000 && !strcmp(base.context_for, p.context_for));
    assert(prefs_context_for(&base, base.model) == 1000000);
    assert(base.vision == AGENT_VISION_YES && !strcmp(base.default_effort, "high"));
    assert(base.max_rounds == 7 && base.max_tools == 128 && base.show_tool_debug == 1);
    /* A buffer that cannot hold the file fails instead of truncating. */
    memset(buf, 0, sizeof(buf));
    assert(prefs_format(&p, buf, 16) == -1);
    /* Values with line breaks cannot round-trip. */
    strcpy(p.api_key, "bad\nkey");
    assert(prefs_format(&p, buf, sizeof(buf)) == -1);
    strcpy(p.api_key, "sk-or-v1-quoted=value");
    strcpy(p.effort, "bad\neffort");
    assert(prefs_format(&p, buf, sizeof(buf)) == -1);
    strcpy(p.effort, "minimal");
    strcpy(p.context_for, "bad\nmodel");
    assert(prefs_format(&p, buf, sizeof(buf)) == -1);
}

int main(void)
{
    defaults();
    full_file();
    syntax_tolerated();
    malformed_keeps_defaults();
    empty_key_clears();
    effort_values();
    context_values();
    context_trust();
    catalog_row_values();
    older_build_file();
    boundaries();
    over_long_lines_ignored();
    format_round_trip();
    printf("preferences checks passed\n");
    return 0;
}
