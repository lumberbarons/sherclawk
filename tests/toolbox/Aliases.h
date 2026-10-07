/* Test-only Alias Manager model; records are opaque byte blobs. */
#ifndef TEST_ALIASES_H
#define TEST_ALIASES_H
#include "Files.h"
typedef Handle AliasHandle;
OSErr ResolveAlias(const FSSpec *, AliasHandle, FSSpec *, Boolean *);
#endif
