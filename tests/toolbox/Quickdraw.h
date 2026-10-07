/* Test-only QuickDraw port model for bounded text measurement. */
#ifndef TEST_QUICKDRAW_H
#define TEST_QUICKDRAW_H
#include "Files.h"
struct GrafPort { short txFont; unsigned char txFace; short txSize; };
typedef struct GrafPort GrafPort;
typedef GrafPort *GrafPtr;
enum { normal=0, bold=1, italic=2, underline=4, outline=8, shadow=0x10, condense=0x20, extend=0x40 };
typedef unsigned char Style;
typedef short StyleParameter;
typedef Style StyleField;
void GetPort(GrafPtr *);
void TextFont(short);
void TextSize(short);
void TextFace(StyleParameter);
#endif
