/* RFC 4648 section 10 vectors plus the bounds the request builder relies on. */
#include "base64.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void vector(const char *plain, const char *encoded)
{
    char out[64];
    size_t n = base64_encode((const unsigned char *)plain, strlen(plain), out, sizeof(out));
    assert(n == strlen(encoded));
    assert(!strcmp(out, encoded));
    assert(BASE64_LENGTH(strlen(plain)) == strlen(encoded));
}
int main(void)
{
    unsigned char binary[256];
    char out[512], tight[8];
    size_t i;
    vector("", "");
    vector("f", "Zg==");
    vector("fo", "Zm8=");
    vector("foo", "Zm9v");
    vector("foob", "Zm9vYg==");
    vector("fooba", "Zm9vYmE=");
    vector("foobar", "Zm9vYmFy");
    /* Every byte value, including the high bit and NUL, uses only the alphabet. */
    for (i = 0; i < sizeof(binary); i++) binary[i] = (unsigned char)i;
    assert(base64_encode(binary, sizeof(binary), out, sizeof(out)) == BASE64_LENGTH(sizeof(binary)));
    for (i = 0; out[i]; i++) assert(strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=", out[i]));
    assert(!memcmp(out, "AAECAwQFBgcICQoL", 16));
    assert(!strcmp(out + 336, "/P3+/w=="));
    /* The output must hold the terminator too: 6 bytes encode to 8 characters. */
    assert(base64_encode((const unsigned char *)"foobar", 6, tight, 8) == (size_t)-1);
    assert(base64_encode((const unsigned char *)"foo", 3, tight, sizeof(tight)) == 4);
    assert(base64_encode((const unsigned char *)"foo", 3, tight, 0) == (size_t)-1);
    puts("base64 checks passed");
    return 0;
}
