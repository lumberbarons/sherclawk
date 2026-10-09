/* Minimal Toolbox model for compiling templates/ppc-toolbox/{io,png,selfrender}.c
 * on the host. Scene drawing, controls and TextEdit are guest-only. */
#ifndef TEST_TEMPLATE_MAC_H
#define TEST_TEMPLATE_MAC_H
#include <stddef.h>
typedef short OSErr;
typedef unsigned long OSType;
typedef char *Ptr;
typedef const unsigned char *ConstStr255Param;
typedef unsigned char Boolean;
typedef struct { short top, left, bottom, right; } Rect;
typedef struct { short v, h; } Point;
typedef struct GrafPort { int unused; } GrafPort;
typedef GrafPort *GrafPtr, *CGrafPtr, *GWorldPtr;
typedef struct { unsigned short red, green, blue; } RGBColor;
typedef struct { short value; RGBColor rgb; } ColorSpec;
typedef struct { long ctSeed; short ctFlags, ctSize; ColorSpec ctTable[256]; } ColorTable;
typedef ColorTable **CTabHandle;
typedef struct { Ptr baseAddr; short rowBytes; CTabHandle pmTable; } PixMap;
typedef PixMap **PixMapHandle;
typedef struct GDevice { int unused; } **GDHandle;
typedef struct { short vRefNum; long parID; unsigned char name[64]; } FSSpec;
typedef struct { unsigned long highLongOfPSN, lowLongOfPSN; } ProcessSerialNumber;
typedef struct { unsigned long processInfoLength; unsigned char *processName; FSSpec *processAppSpec; } ProcessInfoRec;
typedef struct WindowRecord *WindowPtr;
typedef struct ControlRecord **ControlHandle;
typedef struct TERec **TEHandle;
enum { noErr = 0, memFullErr = -108, ioErr = -36, fnfErr = -43, paramErr = -50,
       fsRdPerm = 1, fsWrPerm = 2, smSystemScript = 0 };
OSErr FSMakeFSSpec(short, long, const unsigned char *, FSSpec *);
OSErr FSpCreate(const FSSpec *, OSType, OSType, short);
OSErr FSpOpenDF(const FSSpec *, short, short *);
OSErr SetEOF(short, long);
OSErr FSWrite(short, long *, const void *);
OSErr FSRead(short, long *, void *);
OSErr FSClose(short);
OSErr FSpDelete(const FSSpec *);
OSErr GetCurrentProcess(ProcessSerialNumber *);
OSErr GetProcessInformation(const ProcessSerialNumber *, ProcessInfoRec *);
Ptr NewPtr(long);
void DisposePtr(Ptr);
void BlockMoveData(const void *, void *, long);
unsigned long TickCount(void);
OSErr NewGWorld(GWorldPtr *, short, const Rect *, CTabHandle, GDHandle, long);
PixMapHandle GetGWorldPixMap(GWorldPtr);
Ptr GetPixBaseAddr(PixMapHandle);
Boolean LockPixels(PixMapHandle);
void UnlockPixels(PixMapHandle);
void DisposeGWorld(GWorldPtr);
void GetGWorld(CGrafPtr *, GDHandle *);
void SetGWorld(CGrafPtr, GDHandle);
#endif
