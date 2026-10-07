/* Preferences parser/formatter regressions: defaults, tolerated syntax,
 * malformed and boundary values. Runs under ASan/UBSan via tools/check.sh. */
#include "preferences.h"
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
        "show_tool_debug=1\r";
    prefs_defaults(&p);
    assert(prefs_parse(text, strlen(text), &p) == 6);
    assert(!strcmp(p.model, "openai/gpt-6-luna"));
    assert(!strcmp(p.api_key, "sk-or-v1-abcdef"));
    assert(!strcmp(p.workspace, "Macintosh HD:Projects:"));
    assert(p.max_rounds == 2 && p.max_tools == 1 && p.show_tool_debug == 1);
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
    p.max_rounds = 7; p.max_tools = 128; p.show_tool_debug = 1;
    n = prefs_format(&p, buf, sizeof(buf));
    assert(n > 0 && buf[n - 1] == '\r');
    prefs_defaults(&base);
    assert(prefs_parse(buf, (size_t)n, &base) == 6);
    assert(!strcmp(base.model, p.model));
    assert(!strcmp(base.api_key, p.api_key));
    assert(!strcmp(base.workspace, p.workspace));
    assert(base.max_rounds == 7 && base.max_tools == 128 && base.show_tool_debug == 1);
    /* A buffer that cannot hold the file fails instead of truncating. */
    memset(buf, 0, sizeof(buf));
    assert(prefs_format(&p, buf, 16) == -1);
    /* Values with line breaks cannot round-trip. */
    strcpy(p.api_key, "bad\nkey");
    assert(prefs_format(&p, buf, sizeof(buf)) == -1);
}

int main(void)
{
    defaults();
    full_file();
    syntax_tolerated();
    malformed_keeps_defaults();
    empty_key_clears();
    boundaries();
    over_long_lines_ignored();
    format_round_trip();
    printf("preferences checks passed\n");
    return 0;
}
