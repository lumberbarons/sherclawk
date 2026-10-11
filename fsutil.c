/* One place owns the staged-publication body ladder: create, write, close,
 * FlushVol, catalog proof, read-back comparison and close-error propagation
 * (issue #41). The copies this replaces differed in which steps they
 * remembered; callers of this ladder share every step or none. The
 * cooperative chunked paths (edit_text and copy_file staging, MCP result
 * retention, the journal) stage across event turns and keep their own. */
#include "fsutil.h"
#include <Script.h>
#include <string.h>

/* Read-back scratch, static because the application stack is small. The
 * largest one-shot body today is write_text's string cap; a longer body is
 * verified in chunks of the same size, so no buffer is ever file-sized. */
static char fs_readback[4096];

/* A catalog lookup by directory ID never writes through ioNamePtr; the const
 * spec view is the published contract. */
static OSErr fs_catalog(const FSSpec *file, CInfoPBRec *pb)
{
    memset(pb, 0, sizeof(*pb));
    pb->hFileInfo.ioNamePtr = (unsigned char *)file->name;
    pb->hFileInfo.ioVRefNum = file->vRefNum;
    pb->hFileInfo.ioDirID = file->parID;
    return PBGetCatInfoSync(pb);
}

/* The catalog must show exactly what was written: a plain, non-alias file of
 * length data-fork bytes, no resource fork, with the identity it was created
 * with. A volume that lost or replaced any of that is not negotiated with. */
static OSErr fs_prove(const FSSpec *file, OSType creator, OSType type,
                      long length, CInfoPBRec *out)
{
    CInfoPBRec pb;
    OSErr err = fs_catalog(file, &pb);
    if (!err && ((pb.hFileInfo.ioFlAttrib & 16) ||
                 (pb.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) ||
                 pb.hFileInfo.ioFlFndrInfo.fdType != type ||
                 pb.hFileInfo.ioFlFndrInfo.fdCreator != creator ||
                 pb.hFileInfo.ioFlLgLen != length ||
                 pb.hFileInfo.ioFlRLgLen)) err = ioErr;
    if (!err && out) *out = pb;
    return err;
}

/* Read the closed data fork back and compare it byte for byte with the bytes
 * that were supposed to be there. A short read or any difference is ioErr;
 * a close error still propagates after a clean comparison. */
static OSErr fs_readback_compare(short ref, const void *bytes, long length)
{
    OSErr err = noErr, closed;
    long at = 0;
    while (!err && at < length) {
        long n = length - at, got;
        if (n > (long)sizeof(fs_readback)) n = (long)sizeof(fs_readback);
        got = n;
        err = FSRead(ref, &got, fs_readback);
        /* A read that ends exactly at EOF may report eofErr with the whole
         * count delivered; the queue paths always accepted that, and a short
         * count is still caught below. */
        if (err == eofErr && got == n) err = noErr;
        if (!err && (got != n || memcmp((const char *)bytes + at, fs_readback, (size_t)n)))
            err = ioErr;
        at += n;
    }
    closed = FSClose(ref);
    return err ? err : closed;
}

OSErr fs_verify_body(const FSSpec *file, OSType creator, OSType type,
                     const void *bytes, long length)
{
    OSErr err = fs_prove(file, creator, type, length, NULL);
    short ref;
    if (err) return err;
    err = FSpOpenDF(file, fsRdPerm, &ref);
    if (err) return err;
    return fs_readback_compare(ref, bytes, length);
}

OSErr fs_write_verified(const FSSpec *dest, OSType creator, OSType type,
                        const void *bytes, long length, CInfoPBRec *out_info)
{
    OSErr err, closed;
    short ref;
    long count;
    CInfoPBRec pb;
    err = FSpCreate(dest, creator, type, smSystemScript);
    if (err) return err;
    err = FSpOpenDF(dest, fsWrPerm, &ref);
    if (err) return err;
    count = length;
    err = FSWrite(ref, &count, bytes);
    closed = FSClose(ref);
    if (!err && count != length) err = ioErr; /* a short write is a failed write */
    if (!err) err = closed;                   /* the close error still propagates */
    if (err) return err;
    err = FlushVol(NULL, dest->vRefNum);
    if (err) return err;
    err = fs_prove(dest, creator, type, length, &pb);
    if (err) return err;
    err = FSpOpenDF(dest, fsRdPerm, &ref);
    if (err) return err;
    err = fs_readback_compare(ref, bytes, length);
    if (!err && out_info) *out_info = pb; /* only a verified body has a record */
    return err;
}
