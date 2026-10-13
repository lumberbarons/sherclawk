/* Test-only heap observation and alias-record handle support. */
#ifndef TEST_MEMORY_H
#define TEST_MEMORY_H
#include "Files.h"
long FreeMem(void);
Ptr GetApplLimit(void);
OSErr PtrToHand(const void *source, Handle *destination, Size count);
void DisposeHandle(Handle value);
#endif
