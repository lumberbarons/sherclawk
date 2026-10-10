/* Editor text rules: validation must accept exactly what the MCP parser does,
 * convert classic text, and never echo credentials into the message. */
#include "mcp_editor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char utf8[MCP_EDITOR_UTF8_CAP], error[256];

static int check(const char *text)
{
    error[0] = 0;
    return mcp_editor_validate(text, strlen(text), utf8, sizeof(utf8), error, sizeof(error));
}
static const char *kTavily =
    "{\r  \"mcpServers\": {\r    \"tavily\": {\r"
    "      \"url\": \"https://mcp.tavily.com/mcp/\",\r"
    "      \"headers\": { \"Authorization\": \"Bearer SECRETVALUE12345\" },\r"
    "      \"tools\": [\"tavily_search\"]\r    }\r  }\r}\r";

static void test_accepts(void)
{
    int n = check(kTavily);
    assert(n > 0 && !strchr(utf8, '\r') && strchr(utf8, '\n'));
    assert(!strncmp(utf8, "{\n  \"mcpServers\"", 16));
    assert(check("{\"mcpServers\":{}}") > 0); /* disabled is valid */
    assert(check(mcp_editor_template) > 0);   /* the prefill must save as is */
}
static void test_rejects(void)
{
    const char *bad[] = {
        "", "   ", "{", "{\"mcpServers\":", "[]", "{\"mcpServers\":{},\"x\":1}",
        "{\"mcpServers\":{\"a\":{\"url\":\"http://example.com/mcp\"}}}",
        "{\"mcpServers\":{\"a\":{\"url\":\"https://a.example/m\"},\"b\":{\"url\":\"https://b.example/m\"}}}",
        "{\"mcpServers\":{\"a\":{\"url\":\"https://a.example/m\",\"bogus\":1}}}",
        "{\"mcpServers\":{},\"mcpServers\":{}}",
    };
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(check(bad[i]) == -1);
        assert(!strncmp(error, "Invalid MCP configuration: ", 27));
    }
}
static void test_no_secret_in_error(void)
{
    /* Wrong shape around a secret: the message names a field, not the value. */
    assert(check("{\"mcpServers\":{\"a\":{\"url\":\"http://a.example/m\","
                 "\"headers\":{\"Authorization\":\"Bearer SECRETVALUE12345\"}}}}") == -1);
    assert(!strstr(error, "SECRET") && !strstr(error, "Bearer"));
    assert(check("{\"mcpServers\":{\"a\":{\"url\":\"https://a.example/m\","
                 "\"headers\":{\"Authorization\":\"Bearer SECRETVALUE12345\",\"Host\":\"x\"}}}}") == -1);
    assert(!strstr(error, "SECRET") && !strstr(error, "Bearer"));
}
static void test_limits(void)
{
    static char big[MCP_EDITOR_TEXT_CAP + 2];
    static const char head[] = "{\"mcpServers\":{}}";
    size_t n = sizeof(head) - 1;

    /* Exactly at the cap: padded with spaces it still parses. */
    memset(big, ' ', MCP_EDITOR_TEXT_CAP + 1);
    memcpy(big, head, n);
    assert(mcp_editor_validate(big, MCP_EDITOR_TEXT_CAP, utf8, sizeof(utf8), error, sizeof(error))
           == MCP_EDITOR_TEXT_CAP);
    assert(mcp_editor_validate(big, MCP_EDITOR_TEXT_CAP + 1, utf8, sizeof(utf8), error, sizeof(error)) == -1);
    assert(strstr(error, "8 KiB"));
    /* MacRoman bytes that expand past the cap in UTF-8 are refused too. */
    memset(big, 0xC4, MCP_EDITOR_TEXT_CAP); /* U+00C4 is two UTF-8 bytes */
    assert(mcp_editor_validate(big, MCP_EDITOR_TEXT_CAP, utf8, sizeof(utf8), error, sizeof(error)) == -1);
    assert(strstr(error, "8 KiB"));
}
static void test_encoding(void)
{
    static const char nul[] = "{\"mcpServers\":{}}\0x";
    assert(mcp_editor_validate(nul, sizeof(nul) - 1, utf8, sizeof(utf8), error, sizeof(error)) == -1);
    assert(strstr(error, "encoding"));
    /* A non-ASCII MacRoman header value is rejected by the parser (visible ASCII). */
    assert(check("{\"mcpServers\":{\"a\":{\"url\":\"https://a.example/m\","
                 "\"headers\":{\"X-Name\":\"caf\x8E\"}}}}") == -1);
}
static void test_display(void)
{
    static char text[MCP_EDITOR_TEXT_CAP + 1];
    static const char stored[] = "{\n  \"a\": \"caf\xC3\xA9\"\n}\n";
    int n = mcp_editor_display(stored, sizeof(stored) - 1, text, sizeof(text));
    assert(n == (int)sizeof(stored) - 2 && text[1] == '\r' && (unsigned char)text[13] == 0x8E);
    assert(mcp_editor_display("\xE2\x82\xAC\xF0\x9F\x98\x80", 7, text, sizeof(text)) == -1); /* emoji */
    assert(mcp_editor_display("a\0b", 3, text, sizeof(text)) == -1);
    assert(mcp_editor_display("abcdef", 6, text, 4) == -1);   /* does not fit */
    /* Round trip of the template. */
    n = mcp_editor_display(mcp_editor_template, strlen(mcp_editor_template), text, sizeof(text));
    assert(n > 0 && check(text) > 0);
}
int main(void)
{
    test_accepts(); test_rejects(); test_no_secret_in_error();
    test_limits(); test_encoding(); test_display();
    puts("mcp editor tests passed");
    return 0;
}
