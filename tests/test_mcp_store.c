/* MCP configuration persistence against a File Manager model with fault
 * injection. Every failure before the swap must leave the previous file as it
 * was, a failure after it must restore it, and nothing may be left staged. */
#include "mcp_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAME "Sherclawk MCP Servers.json"
static struct File { int used; char name[64]; char bytes[16384]; long size; } files[8];
static long position[8];
static int creates, writes, reads, renames, flushes;
static int fail_create_at, fail_write_at, short_write, corrupt_read_at, flush_error;
static int fail_rename_at, fail_rename_at2, fail_delete, fail_read_at;

static int find_name(const char *name)
{
    int i;
    for (i = 0; i < 8; i++) if (files[i].used && !strcmp(files[i].name, name)) return i;
    return -1;
}
static int find(const unsigned char *p)
{
    char name[256];
    memcpy(name, p + 1, p[0]); name[p[0]] = 0;
    return find_name(name);
}
static int add(const unsigned char *p)
{
    int i;
    for (i = 0; i < 8; i++) if (!files[i].used) {
        memset(&files[i], 0, sizeof(files[i])); files[i].used = 1;
        memcpy(files[i].name, p + 1, p[0]); return i;
    }
    abort();
}
OSErr FSMakeFSSpec(short vol, long parent, const unsigned char *name, FSSpec *spec)
{
    memset(spec, 0, sizeof(*spec)); spec->vRefNum = vol; spec->parID = parent;
    memcpy(spec->name, name, (size_t)name[0] + 1);
    return find(name) < 0 ? fnfErr : noErr;
}
OSErr FSpCreate(const FSSpec *s, unsigned long creator, unsigned long type, short code)
{
    (void)creator; (void)type; (void)code;
    if (++creates == fail_create_at) return ioErr;
    if (find(s->name) >= 0) return dupFNErr;
    add(s->name); return noErr;
}
OSErr FSpOpenDF(const FSSpec *s, short mode, short *ref)
{
    int i = find(s->name); (void)mode;
    if (i < 0) return fnfErr;
    *ref = (short)i; position[i] = 0; return noErr;
}
OSErr FSWrite(short ref, long *n, const void *bytes)
{
    if (++writes == fail_write_at) return ioErr;
    if (short_write && *n) --*n;
    assert(position[ref] + *n <= (long)sizeof(files[ref].bytes));
    memcpy(files[ref].bytes + position[ref], bytes, (size_t)*n);
    position[ref] += *n; if (position[ref] > files[ref].size) files[ref].size = position[ref];
    return noErr;
}
OSErr FSRead(short ref, long *n, void *bytes)
{
    ++reads;
    if (reads == fail_read_at) return ioErr;
    if (*n > files[ref].size - position[ref]) *n = files[ref].size - position[ref];
    memcpy(bytes, files[ref].bytes + position[ref], (size_t)*n); position[ref] += *n;
    if (reads == corrupt_read_at && *n) ((char *)bytes)[0] ^= 1;
    return noErr;
}
OSErr GetEOF(short ref, long *eof) { *eof = files[ref].size; return noErr; }
OSErr FSClose(short ref) { (void)ref; return noErr; }
OSErr FlushVol(const unsigned char *name, short vol) { (void)name; (void)vol; flushes++; return flush_error ? ioErr : noErr; }
OSErr FSpDelete(const FSSpec *s)
{
    int i = find(s->name);
    if (fail_delete) return ioErr;
    if (i < 0) return fnfErr;
    files[i].used = 0; return noErr;
}
OSErr FSpRename(const FSSpec *s, const unsigned char *to)
{
    int i = find(s->name);
    ++renames;
    if (renames == fail_rename_at || renames == fail_rename_at2) return ioErr;
    if (i < 0) return fnfErr;
    if (find(to) >= 0) return dupFNErr;
    memset(files[i].name, 0, sizeof(files[i].name)); memcpy(files[i].name, to + 1, to[0]);
    return noErr;
}

static FSSpec target;
static void reset(void)
{
    memset(files, 0, sizeof(files));
    creates = writes = reads = renames = flushes = 0;
    fail_create_at = fail_write_at = short_write = corrupt_read_at = flush_error = 0;
    fail_rename_at = fail_rename_at2 = fail_delete = fail_read_at = 0;
    {
        unsigned char p[64];
        p[0] = (unsigned char)strlen(NAME); memcpy(p + 1, NAME, p[0]);
        FSMakeFSSpec(1, 2, p, &target);
    }
}
static void put(const char *name, const char *text)
{
    unsigned char p[64];
    int i;
    p[0] = (unsigned char)strlen(name); memcpy(p + 1, name, p[0]);
    i = add(p); strcpy(files[i].bytes, text); files[i].size = (long)strlen(text);
}
static const char *contents(const char *name)
{
    int i = find_name(name);
    if (i < 0) return NULL;
    files[i].bytes[files[i].size] = 0; return files[i].bytes;
}
static int count(void)
{
    int i, n = 0;
    for (i = 0; i < 8; i++) n += files[i].used;
    return n;
}
static int save(const char *text) { return (int)mcp_store_save(&target, text, strlen(text)); }
/* Only the original file remains: nothing staged, nothing moved aside. */
static void assert_only_original(const char *text)
{
    assert(count() == 1 && !strcmp(contents(NAME), text));
}

static void test_load(void)
{
    char buf[MCP_STORE_CAP];
    size_t len = 99;
    reset();
    assert(mcp_store_load(&target, buf, sizeof(buf), &len) == MCP_STORE_ABSENT && len == 0);
    put(NAME, "{\"a\":1}");
    assert(mcp_store_load(&target, buf, sizeof(buf), &len) == MCP_STORE_OK);
    assert(len == 7 && !memcmp(buf, "{\"a\":1}", 7));
    assert(mcp_store_load(&target, buf, 4, &len) == MCP_STORE_TOO_BIG && len == 0);
    fail_read_at = reads + 1;
    assert(mcp_store_load(&target, buf, sizeof(buf), &len) == MCP_STORE_UNREADABLE);
    assert_only_original("{\"a\":1}");
}
static void test_first_create(void)
{
    char buf[MCP_STORE_CAP];
    size_t len;
    reset();
    assert(save("new") == MCP_STORE_OK);
    assert_only_original("new");
    assert(flushes >= 1);
    assert(mcp_store_load(&target, buf, sizeof(buf), &len) == MCP_STORE_OK && len == 3);
}
static void test_replace(void)
{
    reset(); put(NAME, "old");
    assert(save("new") == MCP_STORE_OK);
    assert_only_original("new");
    /* Stale leftovers from an earlier interrupted save are cleared. */
    reset(); put(NAME, "old"); put(NAME ".old", "ancient"); put(NAME ".new", "junk");
    assert(save("new") == MCP_STORE_OK);
    assert_only_original("new");
}
static void test_stage_failures(void)
{
    int stage;
    for (stage = 0; stage < 6; stage++) {
        reset(); put(NAME, "old");
        switch (stage) {
        case 0: fail_create_at = 1; break;
        case 1: fail_write_at = 1; break;
        case 2: short_write = 1; break;
        case 3: flush_error = 1; break;
        case 4: corrupt_read_at = 1; break;
        case 5: fail_read_at = 1; break;
        }
        assert(save("new") == MCP_STORE_WRITE);
        assert_only_original("old");
        reset();
        switch (stage) { /* the same faults with no previous file */
        case 0: fail_create_at = 1; break;
        case 1: fail_write_at = 1; break;
        case 2: short_write = 1; break;
        case 3: flush_error = 1; break;
        case 4: corrupt_read_at = 1; break;
        case 5: fail_read_at = 1; break;
        }
        assert(save("new") == MCP_STORE_WRITE && count() == 0);
    }
}
static void test_swap_failures(void)
{
    /* Target could not be moved aside: previous file intact, stage removed. */
    reset(); put(NAME, "old"); fail_rename_at = 1;
    assert(save("new") == MCP_STORE_SWAP);
    assert_only_original("old");
    /* Stage could not be moved in: previous file restored. */
    reset(); put(NAME, "old"); fail_rename_at = 2;
    assert(save("new") == MCP_STORE_RESTORED);
    assert_only_original("old");
    /* Published bytes fail verification: previous file restored. */
    reset(); put(NAME, "old"); corrupt_read_at = 2;
    assert(save("new") == MCP_STORE_RESTORED);
    assert_only_original("old");
    /* Restore also fails: the previous file survives as .old. */
    reset(); put(NAME, "old"); fail_rename_at = 2; fail_rename_at2 = 3;
    assert(save("new") == MCP_STORE_LOST);
    assert(!strcmp(contents(NAME ".old"), "old") && !contents(NAME) && !contents(NAME ".new"));
    /* First create: rename failure and verification failure leave nothing. */
    reset(); fail_rename_at = 1;
    assert(save("new") == MCP_STORE_SWAP && count() == 0);
    reset(); corrupt_read_at = 2;
    assert(save("new") == MCP_STORE_WRITE && count() == 0);
}
static void test_recovery_and_leftovers(void)
{
    char buf[MCP_STORE_CAP];
    size_t len;
    reset(); put(NAME ".old", "only copy");
    assert(mcp_store_load(&target, buf, sizeof(buf), &len) == MCP_STORE_RECOVER);
    assert(save("new") == MCP_STORE_RECOVER);
    assert(count() == 1 && !strcmp(contents(NAME ".old"), "only copy"));
    /* A stale .old that cannot be deleted blocks the save before any change. */
    reset(); put(NAME, "old"); put(NAME ".old", "ancient"); fail_delete = 1;
    assert(save("new") == MCP_STORE_WRITE);
    assert(!strcmp(contents(NAME), "old") && !strcmp(contents(NAME ".old"), "ancient"));
}
static void test_limits(void)
{
    static char big[MCP_STORE_CAP + 1];
    unsigned char longname[40];
    reset(); put(NAME, "old");
    memset(big, 'x', sizeof(big) - 1);
    assert(mcp_store_save(&target, big, MCP_STORE_CAP + 1) == MCP_STORE_WRITE);
    assert_only_original("old");
    assert(mcp_store_save(&target, big, MCP_STORE_CAP) == MCP_STORE_OK);
    /* No room for the ".new" suffix inside the 31-byte HFS name limit. */
    memset(longname, 'n', sizeof(longname)); longname[0] = 30;
    FSMakeFSSpec(1, 2, longname, &target);
    assert(mcp_store_save(&target, "x", 1) == MCP_STORE_WRITE);
}
int main(void)
{
    test_load(); test_first_create(); test_replace(); test_stage_failures();
    test_swap_failures(); test_recovery_and_leftovers(); test_limits();
    puts("mcp store tests passed");
    return 0;
}
