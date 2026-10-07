/* Test-only Resource Manager model for resource maps and partial reads. */
#ifndef TEST_RESOURCES_H
#define TEST_RESOURCES_H
#include "Files.h"
typedef unsigned long ResType;
typedef short ResID;
enum { resNotFound = -192, mapReadErr = -199 };
short FSpOpenResFile(const FSSpec *, signed char);
short CurResFile(void);
void UseResFile(short);
void CloseResFile(short);
void SetResLoad(Boolean);
short ResError(void);
short Count1Types(void);
void Get1IndType(ResType *, short);
short Count1Resources(ResType);
Handle Get1IndResource(ResType, short);
Handle Get1Resource(ResType, short);
void GetResInfo(Handle, short *, ResType *, Str255);
long GetResourceSizeOnDisk(Handle);
void ReadPartialResource(Handle, long, void *, long);
void ReleaseResource(Handle);
#endif
