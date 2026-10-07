/* Test-only Font Manager model for family iteration and lookup. */
#ifndef TEST_FONTS_H
#define TEST_FONTS_H
#include "Quickdraw.h"
typedef short FMFontFamily;
typedef long OSStatus;
typedef unsigned long OptionBits;
typedef struct { unsigned long reserved[16]; } FMFontFamilyIterator;
typedef struct FMFilter { unsigned long reserved[2]; } FMFilter;
OSStatus FMCreateFontFamilyIterator(const FMFilter *, void *, OptionBits, FMFontFamilyIterator *);
OSStatus FMGetNextFontFamily(FMFontFamilyIterator *, FMFontFamily *);
OSStatus FMDisposeFontFamilyIterator(FMFontFamilyIterator *);
OSStatus FMGetFontFamilyName(FMFontFamily, Str255);
FMFontFamily FMGetFontFamilyFromName(const unsigned char *);
#endif
