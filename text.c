/* Classic TextEdit uses MacRoman and CR; REST/JSON uses UTF-8 and LF.
 * Keep original UTF-8 for model history, even when the display substitutes ?. */
#include "text.h"
#include "json.h"
#include <string.h>
static const unsigned short macroman[128] = {
    0x00c4, 0x00c5, 0x00c7, 0x00c9, 0x00d1, 0x00d6, 0x00dc, 0x00e1,
    0x00e0, 0x00e2, 0x00e4, 0x00e3, 0x00e5, 0x00e7, 0x00e9, 0x00e8,
    0x00ea, 0x00eb, 0x00ed, 0x00ec, 0x00ee, 0x00ef, 0x00f1, 0x00f3,
    0x00f2, 0x00f4, 0x00f6, 0x00f5, 0x00fa, 0x00f9, 0x00fb, 0x00fc,
    0x2020, 0x00b0, 0x00a2, 0x00a3, 0x00a7, 0x2022, 0x00b6, 0x00df,
    0x00ae, 0x00a9, 0x2122, 0x00b4, 0x00a8, 0x2260, 0x00c6, 0x00d8,
    0x221e, 0x00b1, 0x2264, 0x2265, 0x00a5, 0x00b5, 0x2202, 0x2211,
    0x220f, 0x03c0, 0x222b, 0x00aa, 0x00ba, 0x03a9, 0x00e6, 0x00f8,
    0x00bf, 0x00a1, 0x00ac, 0x221a, 0x0192, 0x2248, 0x2206, 0x00ab,
    0x00bb, 0x2026, 0x00a0, 0x00c0, 0x00c3, 0x00d5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201c, 0x201d, 0x2018, 0x2019, 0x00f7, 0x25ca,
    0x00ff, 0x0178, 0x2044, 0x20ac, 0x2039, 0x203a, 0xfb01, 0xfb02,
    0x2021, 0x00b7, 0x201a, 0x201e, 0x2030, 0x00c2, 0x00ca, 0x00c1,
    0x00cb, 0x00c8, 0x00cd, 0x00ce, 0x00cf, 0x00cc, 0x00d3, 0x00d4,
    0xf8ff, 0x00d2, 0x00da, 0x00db, 0x00d9, 0x0131, 0x02c6, 0x02dc,
    0x00af, 0x02d8, 0x02d9, 0x02da, 0x00b8, 0x02dd, 0x02db, 0x02c7,
};
int text_to_utf8(const char *in, size_t len, char *out, size_t cap)
{
    size_t i, used = 0;
    for (i = 0; i < len; i++) {
        unsigned char b = (unsigned char)in[i];
        unsigned long c = b < 128 ? b : macroman[b - 128];
        int n;
        if (!c) return -1;
        if (c == 13) c = 10;
        n = utf8_emit(c, out + used, cap - used);
        if (n < 0 || (size_t)n >= cap - used) return -1;
        used += (size_t)n;
    }
    out[used] = 0; return (int)used;
}
static int convert_macroman(const char *in, char *out, size_t cap, int strict)
{
    size_t at = 0, used = 0, len = strlen(in);
    while (at < len) {
        unsigned long c;
        unsigned char b = '?';
        int i;
        if (utf8_next(in, len, &at, &c) || used + 1 >= cap) return -1;
        if (c < 128) b = (unsigned char)c;
        else {
            for (i = 0; i < 128; i++) if (c == macroman[i]) { b = (unsigned char)(i + 128); break; }
            if (strict && i == 128) return -1;
        }
        if (c == 10) b = 13;
        /* Normalize CRLF into one classic line separator. */
        if (!(c == 10 && at >= 2 && in[at - 2] == 13)) out[used++] = (char)b;
    }
    out[used] = 0; return (int)used;
}

int text_to_macroman(const char *in, char *out, size_t cap) { return convert_macroman(in, out, cap, 0); }
int text_to_macroman_strict(const char *in, char *out, size_t cap) { return convert_macroman(in, out, cap, 1); }
