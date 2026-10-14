/* RFC 4648 base64 for bounded, in-memory payloads such as image data URLs. */
#ifndef SHERCLAWK_BASE64_H
#define SHERCLAWK_BASE64_H
#include <stddef.h>
/* Encoded length, without a terminator, of n input bytes. */
#define BASE64_LENGTH(n) (4 * (((n) + 2) / 3))
/* Encode n bytes into out, padded and NUL-terminated. Returns the encoded
 * length, or (size_t)-1 when out cannot hold it plus the terminator. */
static size_t base64_encode(const unsigned char *in, size_t n, char *out, size_t cap)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, at = 0;
    if (cap == 0 || BASE64_LENGTH(n) >= cap) return (size_t)-1;
    for (; i + 3 <= n; i += 3) {
        unsigned long v = ((unsigned long)in[i] << 16) | ((unsigned long)in[i + 1] << 8) | in[i + 2];
        out[at++] = alphabet[(v >> 18) & 63]; out[at++] = alphabet[(v >> 12) & 63];
        out[at++] = alphabet[(v >> 6) & 63]; out[at++] = alphabet[v & 63];
    }
    if (n - i) {
        unsigned long v = (unsigned long)in[i] << 16;
        if (n - i == 2) v |= (unsigned long)in[i + 1] << 8;
        out[at++] = alphabet[(v >> 18) & 63]; out[at++] = alphabet[(v >> 12) & 63];
        out[at++] = n - i == 2 ? alphabet[(v >> 6) & 63] : '=';
        out[at++] = '=';
    }
    out[at] = 0;
    return at;
}
#endif
