/* Test-only File Manager model for collision and I/O fault injection. */
#ifndef TEST_FILES_H
#define TEST_FILES_H
#include <stddef.h>
#ifndef TEST_MACTYPES_MIN
#define TEST_MACTYPES_MIN
typedef unsigned char Boolean;
typedef void *Handle;
typedef char *Ptr;
typedef long Size;
#endif
typedef unsigned char Str255[256];
typedef short OSErr;
typedef struct { short vRefNum; long parID; Str255 name; } FSSpec;
typedef struct { unsigned long fdType, fdCreator; unsigned short fdFlags; } FInfo;
typedef union {
    struct { unsigned char *ioNamePtr; short ioVRefNum; long ioDirID; short ioFDirIndex;
        unsigned char ioFlAttrib; FInfo ioFlFndrInfo; long ioFlLgLen, ioFlRLgLen;
        unsigned long ioFlCrDat, ioFlMdDat, ioFlBkDat; } hFileInfo;
    struct { unsigned char *ioNamePtr; short ioVRefNum; long ioDirID; short ioFDirIndex;
        unsigned char ioFlAttrib; FInfo unused; long ioDrDirID; } dirInfo;
} CInfoPBRec;
enum { noErr=0, fnfErr=-43, dupFNErr=-48, paramErr=-50, dirNFErr=-120, ioErr=-36, eofErr=-39,
       fsRdPerm=1, fsWrPerm=2, fsRdWrPerm=3, fsFromStart=1, smSystemScript=0 };
OSErr FSMakeFSSpec(short, long, const unsigned char *, FSSpec *);
OSErr PBGetCatInfoSync(CInfoPBRec *);
OSErr FSpCreate(const FSSpec *, unsigned long, unsigned long, short);
OSErr FSpDirCreate(const FSSpec *, short, long *);
OSErr FSpOpenRF(const FSSpec *, short, short *);
OSErr FSpOpenDF(const FSSpec *, short, short *);
OSErr FSRead(short, long *, void *);
OSErr GetEOF(short, long *);
OSErr FSWrite(short, long *, const void *);
OSErr FSClose(short);
OSErr SetFPos(short, short, long);
OSErr FlushVol(const unsigned char *, short);
OSErr FSpDelete(const FSSpec *);
OSErr FSpGetFInfo(const FSSpec *,FInfo *);
OSErr FSpSetFInfo(const FSSpec *,const FInfo *);
OSErr FSpRename(const FSSpec *, const unsigned char *);
#endif
