/* File Manager model for the MCP run fixture: one shared fake behind the
 * retained-result artifacts, in its own translation unit because the
 * Toolbox headers and the host TLS shim both define `noErr`. */
#include <Files.h>
#include <Script.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FOLDER "Retro68:Sherclawk MCP Results:"

static struct File {
    int used, dir;
    char name[320];
    char bytes[70000];
    long size;
    unsigned long type;
} files[24];
static long position[24];
static int open_refs[24];
static unsigned long ticks = 0x2a;

unsigned long TickCount(void) { return ++ticks; }

/* Shared with the driver unit: published artifacts under the results folder. */
void mcp_run_reset(void)
{
    memset(files, 0, sizeof(files));
    memset(position, 0, sizeof(position));
    memset(open_refs, 0, sizeof(open_refs));
}
int mcp_run_file_count(void)
{
    int i, n = 0;
    for (i = 0; i < 24; i++)
        if (files[i].used && !files[i].dir && !strncmp(files[i].name, FOLDER, strlen(FOLDER))) n++;
    return n;
}
int mcp_run_file_count_suffix(const char *suffix)
{
    int i, n = 0, s = (int)strlen(suffix);
    for (i = 0; i < 24; i++)
        if (files[i].used && !files[i].dir && !strncmp(files[i].name, FOLDER, strlen(FOLDER)) &&
            (int)strlen(files[i].name) >= s && !strcmp(files[i].name + strlen(files[i].name) - s, suffix)) n++;
    return n;
}
long mcp_run_file_bytes(int index)
{
    int i, n = 0;
    for (i = 0; i < 24; i++)
        if (files[i].used && !files[i].dir && !strncmp(files[i].name, FOLDER, strlen(FOLDER)) && n++ == index)
            return files[i].size;
    return -1;
}

static int find_name(const char *name)
{
    int i;
    for (i = 0; i < 24; i++) if (files[i].used && !strcmp(files[i].name, name)) return i;
    return -1;
}
static int find(const unsigned char *p)
{
    char name[320];
    memcpy(name, p + 1, p[0]); name[p[0]] = 0;
    return find_name(name);
}
static int add(const char *name, int dir, unsigned long type)
{
    int i;
    for (i = 0; i < 24; i++) if (!files[i].used) {
        memset(&files[i], 0, sizeof(files[i]));
        files[i].used = 1; files[i].dir = dir; files[i].type = type;
        strcpy(files[i].name, name);
        return i;
    }
    abort();
}
OSErr FSMakeFSSpec(short vol, long parent, const unsigned char *name, FSSpec *spec)
{
    (void)vol; (void)parent;
    memset(spec, 0, sizeof(*spec));
    memcpy(spec->name, name, (size_t)name[0] + 1);
    return find(name) < 0 ? fnfErr : noErr;
}
OSErr FSpDirCreate(const FSSpec *s, short code, long *id)
{
    char name[320]; (void)code;
    memcpy(name, s->name + 1, s->name[0]); name[s->name[0]] = 0;
    add(name, 1, 0); *id = 3;
    return noErr;
}
OSErr FSpCreate(const FSSpec *s, unsigned long creator, unsigned long type, short code)
{
    char name[320]; (void)creator; (void)code;
    if (find(s->name) >= 0) return dupFNErr;
    memcpy(name, s->name + 1, s->name[0]); name[s->name[0]] = 0;
    add(name, 0, type);
    return noErr;
}
OSErr FSpOpenDF(const FSSpec *s, short mode, short *ref)
{
    int i = find(s->name); (void)mode;
    if (i < 0) return fnfErr;
    if (files[i].dir) return paramErr;
    *ref = (short)i; position[i] = 0; open_refs[i]++;
    return noErr;
}
OSErr FSWrite(short ref, long *n, const void *bytes)
{
    assert(position[ref] + *n <= (long)sizeof(files[ref].bytes));
    memcpy(files[ref].bytes + position[ref], bytes, (size_t)*n);
    position[ref] += *n;
    if (position[ref] > files[ref].size) files[ref].size = position[ref];
    return noErr;
}
OSErr FSRead(short ref, long *n, void *bytes)
{
    if (*n > files[ref].size - position[ref]) *n = files[ref].size - position[ref];
    memcpy(bytes, files[ref].bytes + position[ref], (size_t)*n);
    position[ref] += *n;
    return noErr;
}
OSErr GetEOF(short ref, long *eof) { *eof = files[ref].size; return noErr; }
OSErr SetFPos(short ref, short pos, long off) { (void)pos; position[ref] = off; return noErr; }
OSErr FSClose(short ref) { assert(open_refs[ref] > 0); open_refs[ref]--; return noErr; }
OSErr FlushVol(const unsigned char *name, short vol) { (void)name; (void)vol; return noErr; }
OSErr FSpDelete(const FSSpec *s)
{
    int i = find(s->name);
    if (i < 0) return fnfErr;
    if (open_refs[i] > 0) return fBsyErr;
    files[i].used = 0;
    return noErr;
}
OSErr FSpRename(const FSSpec *s, const unsigned char *to)
{
    char name[320], *colon;
    int i = find(s->name);
    if (i < 0) return fnfErr;
    memcpy(name, s->name + 1, s->name[0]); name[s->name[0]] = 0;
    colon = strrchr(name, ':');
    if (!colon) return paramErr;
    colon[1] = 0;
    strncat(name, (const char *)to + 1, (size_t)to[0]);
    if (find_name(name) >= 0) return dupFNErr;
    memset(files[i].name, 0, sizeof(files[i].name));
    strcpy(files[i].name, name);
    return noErr;
}
OSErr PBGetCatInfoSync(CInfoPBRec *pb)
{
    int i = find(pb->hFileInfo.ioNamePtr);
    if (i < 0) return fnfErr;
    pb->hFileInfo.ioFlAttrib = (unsigned char)(files[i].dir ? 16 : 0);
    pb->hFileInfo.ioFlFndrInfo.fdFlags = 0;
    pb->hFileInfo.ioFlFndrInfo.fdType = files[i].type;
    pb->hFileInfo.ioFlLgLen = files[i].size;
    pb->hFileInfo.ioFlRLgLen = 0;
    return noErr;
}
