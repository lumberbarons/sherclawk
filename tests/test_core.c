/* Protocol regressions: malformed input, split framing, encodings, and rollback. */
#include "chat.h"
#include "json.h"
#include "text.h"
#include "http.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static Chat c, snapshot;
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
static void history(void)
{
    const char *ok = "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"marmalade\"}}]}";
    const char *err = "{\"error\":{\"code\":401,\"message\":\"Invalid key\"}}";
    int limited, i;
    char error[256];
    chat_reset(&c);
    assert(chat_request(&c, "openai/gpt-6-luna", "Remember marmalade", req, sizeof(req)) > 0);
    assert(json_parse(req, strlen(req), tokens, 2048) > 0);
    assert(chat_reply(ok, strlen(ok), 200, out, sizeof(out), &limited, error, sizeof(error)) == 0);
    assert(!limited && !strcmp(out, "marmalade"));
    assert(chat_commit(&c, "Remember marmalade", out, 0) == 0);
    assert(c.messages == 2 && chat_request(&c, "other/model", "What word?", req, sizeof(req)) > 0);
    assert(strstr(req, "Remember marmalade") && strstr(req, "assistant") && strstr(req, "What word?"));
    snapshot = c;
    assert(chat_reply(err, strlen(err), 401, out, sizeof(out), &limited, error, sizeof(error)) == -1);
    assert(strstr(error, "401") && strstr(error, "Invalid key") && !memcmp(&c, &snapshot, sizeof(c)));
    assert(chat_reply(err, strlen(err), 200, out, sizeof(out), &limited, error, sizeof(error)) == -1);
    assert(chat_reply("{\"choices\":[]}", 14, 200, out, sizeof(out), &limited, error, sizeof(error)) == -1);
    assert(chat_reply("{}", 2, 503, out, sizeof(out), &limited, error, sizeof(error)) == -1);
    assert(chat_reply(ok, strlen(ok), 200, out, 3, &limited, error, sizeof(error)) == -1);
    strcpy(text, ok); { char *p = strstr(text, "stop"); memcpy(p, "xxxx", 4); }
    assert(chat_reply(text, strlen(text), 200, out, sizeof(out), &limited, error, sizeof(error)) == -1);
    strcpy(text, "{\"choices\":[{\"finish_reason\":\"length\",\"message\":{\"content\":\"partial\"}}]}");
    assert(chat_reply(text, strlen(text), 200, out, sizeof(out), &limited, error, sizeof(error)) == 0 && limited);
    strcpy(text, "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":null}}]}");
    assert(chat_reply(text, strlen(text), 200, out, sizeof(out), &limited, error, sizeof(error)) == -1);
    for (i = 1; i < 10; i++) assert(chat_commit(&c, "user", "assistant", 0) == 0);
    snapshot = c;
    assert(chat_request(&c, "model", "next", req, sizeof(req)) == -1);
    assert(chat_commit(&c, "next", "reply", 0) == -1 && !memcmp(&c, &snapshot, sizeof(c)));
    chat_reset(&c); assert(!c.messages && !c.transcript[0]);
    memset(text, 'x', CHAT_TRANSCRIPT_CAP); text[CHAT_TRANSCRIPT_CAP] = 0;
    snapshot = c;
    assert(chat_commit(&c, "prompt", text, 0) == -1 && !memcmp(&c, &snapshot, sizeof(c)));
    memset(text, '\n', 1600); text[1600] = 0;
    assert(chat_commit(&c, "prompt", text, 0) == -1 && !memcmp(&c, &snapshot, sizeof(c)));
    assert(chat_commit(&c, "prompt", "partial", 1) == 0 && strstr(c.transcript, "token limit"));
    memset(out, '\n', CHAT_REQUEST_CAP); out[CHAT_REQUEST_CAP] = 0;
    assert(chat_request(&c, "model", out, req, sizeof(req)) == -1);
}
int main(void)
{
    framing(); json(); encoding(); history();
    puts("PASS HTTP framing, JSON, encoding, history and rollback");
    return 0;
}
