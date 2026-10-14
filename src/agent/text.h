#ifndef SHERCLAWK_TEXT_H
#define SHERCLAWK_TEXT_H
#include <stddef.h>
int text_to_utf8(const char *in, size_t len, char *out, size_t cap);
int text_to_macroman_strict(const char *in, char *out, size_t cap);
int text_to_macroman(const char *in, char *out, size_t cap);
#endif
