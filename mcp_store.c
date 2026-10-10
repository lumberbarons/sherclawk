#include "mcp_store.h"
#include <Script.h>
#include <string.h>

#define SUFFIX_NEW MCP_STORE_SUFFIX_NEW
#define SUFFIX_OLD MCP_STORE_SUFFIX_OLD

/* Spec for "<target name><suffix>" in the target's folder. FSMakeFSSpec fills
 * the spec even when the file is absent (fnfErr), so callers test the result. */
static OSErr sibling(const FSSpec *target, const char *suffix, FSSpec *out)
{
    unsigned char name[256];
    size_t n = strlen(suffix);
    if (target->name[0] + n > 31) return paramErr; /* HFS file name limit */
    memcpy(name + 1, target->name + 1, target->name[0]);
    memcpy(name + 1 + target->name[0], suffix, n);
    name[0] = (unsigned char)(target->name[0] + n);
    return FSMakeFSSpec(target->vRefNum, target->parID, name, out);
}
static OSErr refresh(const FSSpec *spec, FSSpec *out)
{
    return FSMakeFSSpec(spec->vRefNum, spec->parID, spec->name, out);
}

static OSErr read_all(const FSSpec *spec, char *bytes, size_t cap, long *length)
{
    short ref = 0;
    long count;
    OSErr err = FSpOpenDF(spec, fsRdPerm, &ref), close_err;
    if (err) return err;
    err = GetEOF(ref, length);
    if (!err && *length < 0) err = ioErr;
    if (!err && (size_t)*length > cap) err = paramErr;
    if (!err) {
        count = *length;
        err = FSRead(ref, &count, bytes);
        if (!err && count != *length) err = ioErr;
    }
    close_err = FSClose(ref);
    return err ? err : close_err;
}

McpStoreResult mcp_store_load(const FSSpec *target, char *bytes, size_t cap, size_t *len)
{
    FSSpec spec, old;
    long length = 0;
    OSErr err = refresh(target, &spec);

    *len = 0;
    if (err == fnfErr) {
        /* An interrupted save may have moved the only copy aside. */
        if (sibling(target, SUFFIX_OLD, &old) == noErr) return MCP_STORE_RECOVER;
        return MCP_STORE_ABSENT;
    }
    if (err) return MCP_STORE_UNREADABLE;
    err = read_all(&spec, bytes, cap, &length);
    if (err == paramErr) return MCP_STORE_TOO_BIG;
    if (err) return MCP_STORE_UNREADABLE;
    *len = (size_t)length;
    return MCP_STORE_OK;
}

/* Write the whole stage file, flush it, and compare it with a fresh read. */
static OSErr write_stage(const FSSpec *stage, const char *bytes, size_t len)
{
    static char verified[MCP_STORE_CAP];
    short ref = 0;
    long count = (long)len, actual = 0;
    OSErr err = FSpCreate(stage, MCP_STORE_CREATOR, MCP_STORE_TYPE, smSystemScript), close_err;

    if (!err) err = FSpOpenDF(stage, fsWrPerm, &ref);
    if (!err) {
        err = FSWrite(ref, &count, bytes);
        if (!err && count != (long)len) err = ioErr;
        close_err = FSClose(ref);
        if (!err) err = close_err;
    }
    if (!err) err = FlushVol(NULL, stage->vRefNum);
    if (!err) err = read_all(stage, verified, sizeof(verified), &actual);
    if (!err && (actual != (long)len || memcmp(verified, bytes, len))) err = ioErr;
    memset(verified, 0, sizeof(verified));
    return err;
}
static int matches(const FSSpec *spec, const char *bytes, size_t len)
{
    static char verified[MCP_STORE_CAP];
    long actual = 0;
    int ok = !read_all(spec, verified, sizeof(verified), &actual) &&
             actual == (long)len && !memcmp(verified, bytes, len);
    memset(verified, 0, sizeof(verified));
    return ok;
}

McpStoreResult mcp_store_save(const FSSpec *target, const char *bytes, size_t len)
{
    FSSpec spec, stage, old, now;
    OSErr err;
    int have_target;

    if (len > MCP_STORE_CAP) return MCP_STORE_WRITE;
    err = refresh(target, &spec);
    if (err && err != fnfErr) return MCP_STORE_WRITE;
    have_target = err == noErr;
    if (sibling(target, SUFFIX_NEW, &stage) == paramErr ||
        sibling(target, SUFFIX_OLD, &old) == paramErr) return MCP_STORE_WRITE;
    if (refresh(&old, &now) == noErr) {
        /* With no target, "<name>.old" is the only copy; never destroy it. */
        if (!have_target) return MCP_STORE_RECOVER;
        if (FSpDelete(&old)) return MCP_STORE_WRITE; /* stale after a finished swap */
    }
    if (refresh(&stage, &now) == noErr && FSpDelete(&stage)) return MCP_STORE_WRITE;

    if (write_stage(&stage, bytes, len)) {
        if (refresh(&stage, &now) == noErr) FSpDelete(&stage);
        return MCP_STORE_WRITE;
    }
    if (!have_target) {
        if (FSpRename(&stage, spec.name)) { FSpDelete(&stage); return MCP_STORE_SWAP; }
        if (refresh(&spec, &now) || !matches(&now, bytes, len)) {
            if (refresh(&spec, &now) == noErr) FSpDelete(&now);
            return MCP_STORE_WRITE;
        }
        return MCP_STORE_OK;
    }

    /* Replace: move the old file aside, move the stage in, verify, and put the
     * old file back if anything after the first rename goes wrong. */
    if (FSpRename(&spec, old.name)) { FSpDelete(&stage); return MCP_STORE_SWAP; }
    if (FSpRename(&stage, spec.name) || refresh(&spec, &now) || !matches(&now, bytes, len)) {
        int restored;
        if (refresh(&spec, &now) == noErr) FSpDelete(&now); /* unverified content */
        restored = !refresh(&old, &now) && !FSpRename(&now, spec.name);
        if (refresh(&stage, &now) == noErr) FSpDelete(&stage);
        return restored ? MCP_STORE_RESTORED : MCP_STORE_LOST;
    }
    if (refresh(&old, &now) == noErr) FSpDelete(&now); /* a leftover is cleared next save */
    return MCP_STORE_OK;
}
