/* Test-only QuickDraw text metrics model. */
#ifndef TEST_QUICKDRAWTEXT_H
#define TEST_QUICKDRAWTEXT_H
#include "Quickdraw.h"
typedef struct FontInfo { short ascent, descent, widMax, leading; } FontInfo;
short TextWidth(const void *, short, short);
void GetFontInfo(FontInfo *);
#endif
