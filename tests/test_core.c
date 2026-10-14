/* HTTP/JSON regressions, encodings, and native transcript tool folding. */
#include "app/chat.h"
#include "agent/json.h"
#include "agent/text.h"
#include "http.h"
#include "config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char out[CHAT_REQUEST_CAP + 1], req[CHAT_REQUEST_CAP], text[CHAT_REQUEST_CAP + 1];
static JsonToken tokens[2048];
static int response(const char *s, int closed, int *status)
{
    size_t n;
    return http_response(s, strlen(s), closed, CHAT_RESPONSE_CAP, status, out, sizeof(out), &n);
}
static void framing(void)
{
    const char *complete = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    const char *chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2;foo=x\r\nhe\r\n3\r\nllo\r\n0\r\nX-Result: yes\r\n\r\n";
    int status;
    size_t i, n;
    for (i = 0; i < strlen(complete); i++)
        assert(http_response(complete, i, 0, CHAT_RESPONSE_CAP, &status, out, sizeof(out), &n) == 0);
    assert(response(complete, 0, &status) == 1 && status == 200 && !strcmp(out, "hello"));
    for (i = 0; i < strlen(chunked); i++)
        assert(http_response(chunked, i, 0, CHAT_RESPONSE_CAP, &status, out, sizeof(out), &n) == 0);
    assert(response(chunked, 0, &status) == 1 && !strcmp(out, "hello"));
    assert(response("HTTP/1.1 401 Unauthorized\r\n\r\n{}", 0, &status) == 0);
    assert(response("HTTP/1.1 401 Unauthorized\r\n\r\n{}", 1, &status) == 1 && status == 401);
    assert(response("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nhello", 1, &status) == -1);
    assert(response("HTTP/1.1 200 OK\r\nContent-Length: 999999999999\r\n\r\n", 0, &status) == -1);
    assert(response("HTTP/1.1 200 OK\r\nContent-Length: 3x\r\n\r\nabc", 1, &status) == -1);
    assert(response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nabc", 1, &status) == -1);
    assert(response("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n", 1, &status) == -1);
    assert(response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc!\r\n", 1, &status) == -1);
    assert(response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n", 1, &status) == -1);
    assert(response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n0\r\n\r\n", 1, &status) == -1);
    assert(response("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 1, &status) == -1);
    assert(http_build_post("openrouter.ai", "/api/v1/chat/completions", "test\r\nInjected: yes", "{}", 2, req, sizeof(req)) == -1);
    assert(http_build_post("openrouter.ai", "/x", "test", "\xc3\xa9", 2, req, sizeof(req)) > 0);
    assert(strstr(req, "Content-Length: 2\r\n") && strstr(req, "Accept-Encoding: identity"));
#ifdef SHERCLAWK_APP
    assert(strstr(req, "User-Agent: Sherclawk/1.0 (Certainly; Mac OS 9)\r\n"));
#else
    assert(strstr(req, "User-Agent: HelloChat/1.0 (Certainly; Mac OS 9)\r\n"));
#endif
    {
        char headers[128], small[8];
        assert(sherclawk_attribution("", headers, sizeof(headers)) == 0 && !headers[0]);
        assert(sherclawk_attribution("https://x\r\nEvil: y", headers, sizeof(headers)) == -1);
        assert(sherclawk_attribution("https://example.test/sherclawk", small, sizeof(small)) == -1);
        assert(sherclawk_attribution("https://example.test/sherclawk", headers, sizeof(headers)) > 0);
        assert(!strcmp(headers, "HTTP-Referer: https://example.test/sherclawk\r\nX-OpenRouter-Title: Sherclawk\r\n"));
        assert(http_build_post_with_headers("openrouter.ai", "/x", "test", headers, "{}", 2, req, sizeof(req)) > 0);
        assert(strstr(req, "Host: openrouter.ai\r\nHTTP-Referer: https://example.test/sherclawk\r\n"
                          "X-OpenRouter-Title: Sherclawk\r\nUser-Agent: "));
        assert(http_build_post_with_headers("openrouter.ai", "/x", "test", NULL, "{}", 2, req, sizeof(req)) > 0);
        assert(!strstr(req, "HTTP-Referer"));
        assert(http_build_post_with_headers("openrouter.ai", "/x", "test", "Bare\nline\r\n", "{}", 2, req, sizeof(req)) == -1);
        assert(http_build_post_with_headers("openrouter.ai", "/x", "test", "Bare\rline\r\n", "{}", 2, req, sizeof(req)) == -1);
        assert(http_build_post_with_headers("openrouter.ai", "/x", "test", headers, "{}", 2, req, 40) == -1);
    }
    assert(http_build_post("host", "/x", "test", "body", 4, req, 20) == -1);
}
static void json(void)
{
    const char *good = "{\"nested\":{\"content\":\"wrong\"},\"content\":\"quote \\\" \\u00e9 \\ud83d\\ude00\\n\"}";
    const char *bad[] = {"{\"a\":1,}", "[1,]", "[01]", "[1.]", "[1e]", "true false", "[NaN]", "\"\\ud800\"", "\"\\udc00\"", "\"\\u0000\"", "\"\\x\"", "\"\xc0\xaf\"", "{\"a\" 1}", "[", "\"\n\""};
    size_t i;
    int member;
    assert(json_parse(good, strlen(good), tokens, 2048) > 0);
    member = json_member(good, tokens, 0, "content");
    assert(json_string(good, tokens, member, out, sizeof(out)) > 0);
    assert(!strcmp(out, "quote \" \xc3\xa9 \xf0\x9f\x98\x80\n"));
    assert(json_quote(out, text, sizeof(text)) > 0);
    assert(json_parse(text, strlen(text), tokens, 2048) == 1);
    assert(json_string(text, tokens, 0, req, sizeof(req)) > 0 && !strcmp(req, out));
    for (i = 0; i < sizeof(bad)/sizeof(*bad); i++) assert(json_parse(bad[i], strlen(bad[i]), tokens, 2048) == -1);
    assert(json_parse(good, strlen(good), tokens, 2) == -1);
    assert(json_quote("abc", text, 5) == -1);
    memset(text, '[', 40); memset(text + 40, ']', 40);
    assert(json_parse(text, 80, tokens, 2048) == -1);
    /* Malformed/truncated inputs at every boundary should be harmless. */
    for (i = 0; i < strlen(good); i++) assert(json_parse(good, i, tokens, 2048) == -1);
}
static void encoding(void)
{
    assert(text_to_utf8("caf\x8e\r", 5, out, sizeof(out)) == 6);
    assert(!strcmp(out, "caf\xc3\xa9\n"));
    assert(text_to_macroman("caf\xc3\xa9\r\n\xf0\x9f\x98\x80", text, sizeof(text)) == 6);
    assert(!strcmp(text, "caf\x8e\r?"));
    assert(text_to_macroman("\xf0\x80\x80\x80", text, sizeof(text)) == -1);
    assert(text_to_utf8("x", 1, out, 1) == -1);
    assert(text_to_macroman("x", text, 1) == -1);
    { int i; for (i = 128; i < 256; i++) {
        char source[2] = {(char)i, 0};
        assert(text_to_utf8(source, 1, out, sizeof(out)) > 0);
        assert(text_to_macroman(out, text, sizeof(text)) == 1 && (unsigned char)text[0] == i);
    }}
}
/* Append one transcript entry the way the app's ShowMessage does. */
static size_t add_entry(Chat *c, const char *text)
{
    size_t at = strlen(c->transcript);
    chat_tool_forget(c);
    strcat(c->transcript, text); strcat(c->transcript, "\r\r");
    return at;
}
static void tool_collapse(void)
{
    Chat c;
    size_t at;
    int i;
    chat_reset(&c);
    assert(!chat_tool_collapse(&c, "read_text"));
    add_entry(&c, "You:\rhi");
    at = add_entry(&c, "\xA5 read_text(path: \"a\")");
    chat_tool_note(&c, at, "read_text");
    assert(chat_tool_collapse(&c, "read_text"));
    assert(!strcmp(c.transcript, "You:\rhi\r\r\xA5 read_text x2\r\r"));
    for (i = 3; i <= 12; i++) assert(chat_tool_collapse(&c, "read_text"));
    assert(!strcmp(c.transcript, "You:\rhi\r\r\xA5 read_text x12\r\r"));
    /* A different tool starts its own line; the run before it stays closed. */
    assert(!chat_tool_collapse(&c, "list_files"));
    at = add_entry(&c, "\xA5 list_files()");
    chat_tool_note(&c, at, "list_files");
    assert(!chat_tool_collapse(&c, "read_text"));
    assert(chat_tool_collapse(&c, "list_files"));
    assert(strstr(c.transcript, "\xA5 list_files x2\r\r") && strstr(c.transcript, "read_text x12"));
    /* Any other entry, even a debug-view tool block, ends the run. */
    add_entry(&c, "Sherclawk:\rdone");
    assert(!chat_tool_collapse(&c, "list_files"));
    at = add_entry(&c, "\xA5 read_text()");
    chat_tool_note(&c, at, "read_text");
    c.transcript[strlen(c.transcript) - 2] = 0;    /* transcript changed behind our back */
    assert(!chat_tool_collapse(&c, "read_text"));
    /* A name too long to remember is never collapsed. */
    at = add_entry(&c, "\xA5 long");
    { char name[80]; memset(name, 'n', 79); name[79] = 0; chat_tool_note(&c, at, name); assert(!chat_tool_collapse(&c, name)); }
    /* A displayed message ends the run, and a full transcript falls back to appending. */
    chat_reset(&c);
    at = add_entry(&c, "\xA5 read_text()");
    chat_tool_note(&c, at, "read_text");
    assert(chat_append_message(&c, "Assistant", "a") >= 0 && !chat_tool_collapse(&c, "read_text"));
    chat_reset(&c);
    memset(c.transcript, 'x', sizeof(c.transcript) - 8); c.transcript[sizeof(c.transcript) - 8] = 0;
    at = add_entry(&c, "\xA5 t()");
    chat_tool_note(&c, at, "t");
    assert(strlen(c.transcript) == sizeof(c.transcript) - 1 && !chat_tool_collapse(&c, "t"));
    assert(!strcmp(c.transcript + at, "\xA5 t()\r\r"));
}
static void numbers(void)
{
    static const struct { const char *text; long value; } ints[] = {
        {"0", 0}, {"7", 7}, {"45234", 45234}, {"2147483647", 2147483647L}
    };
    static const char *int_bad[] = {
        "2147483648", "99999999999999999999", "1.5", "1e2", "-1", "\"12\"", "[1]", "true", "null"
    };
    static const struct { const char *text; long long value; } decimals[] = {
        {"0", 0LL}, {"0.5", 500000LL}, {"12.5", 12500000LL},
        {"0.012345", 12345LL}, {"1.2e-5", 12LL}, {"2e-6", 2LL},
        {"1.5e-6", 2LL}, {"1e+2", 100000000LL}, {"0.0000004", 0LL},
        {"0.0000005", 1LL}, {"999999999.999999", 999999999999999LL}
    };
    static const char *decimal_bad[] = {
        "-0.1", "1000000000", "999999999999.999999", "1e40",
        "12345678901234567890", "\"0.5\"", "true", "[1]"
    };
    char fixture[128];
    long integer;
    long long micros;
    size_t i;
    for (i = 0; i < sizeof(ints)/sizeof(*ints); i++) {
        snprintf(fixture, sizeof(fixture), "{\"n\":%s}", ints[i].text);
        assert(json_parse(fixture, strlen(fixture), tokens, 2048) > 0);
        assert(!json_integer(fixture, tokens, json_member(fixture, tokens, 0, "n"), &integer));
        assert(integer == ints[i].value);
    }
    for (i = 0; i < sizeof(int_bad)/sizeof(*int_bad); i++) {
        snprintf(fixture, sizeof(fixture), "{\"n\":%s}", int_bad[i]);
        assert(json_parse(fixture, strlen(fixture), tokens, 2048) > 0);
        assert(json_integer(fixture, tokens, json_member(fixture, tokens, 0, "n"), &integer) == -1);
    }
    for (i = 0; i < sizeof(decimals)/sizeof(*decimals); i++) {
        snprintf(fixture, sizeof(fixture), "{\"n\":%s}", decimals[i].text);
        assert(json_parse(fixture, strlen(fixture), tokens, 2048) > 0);
        assert(!json_decimal_micros(fixture, tokens, json_member(fixture, tokens, 0, "n"), &micros));
        assert(micros == decimals[i].value);
    }
    for (i = 0; i < sizeof(decimal_bad)/sizeof(*decimal_bad); i++) {
        snprintf(fixture, sizeof(fixture), "{\"n\":%s}", decimal_bad[i]);
        assert(json_parse(fixture, strlen(fixture), tokens, 2048) > 0);
        assert(json_decimal_micros(fixture, tokens, json_member(fixture, tokens, 0, "n"), &micros) == -1);
    }
    assert(json_integer(fixture, tokens, -1, &integer) == -1);
    assert(json_decimal_micros(fixture, tokens, 0, &micros) == -1);
}
int main(void)
{
    framing(); json(); numbers(); encoding(); tool_collapse();
    puts("PASS HTTP framing, JSON, numbers, encoding and tool collapse");
    return 0;
}
