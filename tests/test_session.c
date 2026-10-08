/* Session journal and handoff persistence against a File Manager model. Faults
 * check that a failed handoff leaves the agent, the open journal and the file
 * handles exactly as they were, and that an existing file is never reused. */
#include "session.h"
#include <Files.h>
#include <Script.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKSPACE "Retro68:"
#define FOLDER "Retro68:Sherclawk Sessions:"
static struct File { int used, dir; char name[256]; char bytes[16384]; long size; } files[32];
static long position[32];
static int open_count, creates, fail_create_at, short_write, corrupt_read, flush_error, fail_write_at, writes;
static unsigned long ticks = 0x2a;

unsigned long TickCount(void) { return ticks; }
static int find_name(const char *name)
{
    int i;
    for (i = 0; i < 32; i++) if (files[i].used && !strcmp(files[i].name, name)) return i;
    return -1;
}
static int find(const unsigned char *p)
{
    char name[256];
    memcpy(name, p + 1, p[0]); name[p[0]] = 0;
    return find_name(name);
}
static int add(const char *name, int dir)
{
    int i;
    for (i = 0; i < 32; i++) if (!files[i].used) {
        memset(&files[i], 0, sizeof(files[i])); files[i].used = 1; files[i].dir = dir;
        strcpy(files[i].name, name); return i;
    }
    abort();
}
OSErr FSMakeFSSpec(short vol, long parent, const unsigned char *name, FSSpec *spec)
{
    (void)vol; (void)parent;
    memset(spec, 0, sizeof(*spec)); memcpy(spec->name, name, (size_t)name[0] + 1);
    return find(name) < 0 ? fnfErr : noErr;
}
OSErr FSpDirCreate(const FSSpec *s, short code, long *id)
{
    char name[256]; (void)code;
    memcpy(name, s->name + 1, s->name[0]); name[s->name[0]] = 0;
    add(name, 1); *id = 1; return noErr;
}
OSErr FSpCreate(const FSSpec *s, unsigned long creator, unsigned long type, short code)
{
    char name[256]; (void)creator; (void)type; (void)code;
    creates++;
    if (creates == fail_create_at) return ioErr;
    if (find(s->name) >= 0) return dupFNErr;
    memcpy(name, s->name + 1, s->name[0]); name[s->name[0]] = 0;
    add(name, 0); return noErr;
}
OSErr FSpOpenDF(const FSSpec *s, short mode, short *ref)
{
    int i = find(s->name); (void)mode;
    if (i < 0) return fnfErr;
    *ref = (short)i; position[i] = 0; open_count++; return noErr;
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
    if (*n > files[ref].size - position[ref]) *n = files[ref].size - position[ref];
    memcpy(bytes, files[ref].bytes + position[ref], (size_t)*n); position[ref] += *n;
    if (corrupt_read && *n) ((char *)bytes)[0] ^= 1;
    return noErr;
}
OSErr GetEOF(short ref, long *eof) { *eof = files[ref].size; return noErr; }
OSErr FSClose(short ref) { (void)ref; open_count--; return noErr; }
OSErr FlushVol(const unsigned char *name, short vol) { (void)name; (void)vol; return flush_error ? ioErr : noErr; }

static Agent agent, before;
static Session session;
static char handoff[256];
static const char *kSummary = "## Goal\nBuild a prototype.\n\n## Next steps\nInspect sources.\n";

static void reset(void)
{
    memset(files, 0, sizeof(files));
    open_count = creates = fail_create_at = short_write = corrupt_read = flush_error = fail_write_at = writes = 0;
    memset(&session, 0, sizeof(session)); handoff[0] = 0; ticks = 0x2a;
}
static int file_count(void)
{
    int i, n = 0;
    for (i = 0; i < 32; i++) n += files[i].used && !files[i].dir;
    return n;
}
static const char *contents(const char *path)
{
    int i = find_name(path);
    assert(i >= 0); files[i].bytes[files[i].size] = 0; return files[i].bytes;
}
static void open_with_agent(void)
{
    char error[256];
    reset();
    assert(!session_open(&session, WORKSPACE));
    agent_reset(&agent, session_journal, &session);
    assert(!agent_begin(&agent, "Build a prototype", error, sizeof(error)));
    assert(!agent_stop(&agent, "diagnostic"));
    agent.cost_micros = 1234; agent.cost_seen = 1;
    memcpy(&before, &agent, sizeof(agent));
}
/* The failed commit left agent, journal and handles as they were. */
static void assert_untouched(const char *path, int handles)
{
    assert(!memcmp(&before, &agent, sizeof(agent)));
    assert(session.open && !strcmp(session.path, path) && open_count == handles);
    assert(!session_journal(&session, "still_open", "{}"));
}

static void test_open_and_journal(void)
{
    char first[256];
    reset();
    assert(session_journal(&session, "x", "{}") == -1); /* never opened */
    assert(session_journal(NULL, "x", "{}") == -1);
    assert(!session_open(&session, WORKSPACE));
    assert(session.open && open_count == 1 && find_name(FOLDER) >= 0);
    assert(!strncmp(session.path, FOLDER "s", sizeof(FOLDER)));
    assert(!session_journal(&session, "user", "{\"a\":1}"));
    assert(!strcmp(contents(session.path), "{\"event\":\"user\",\"message\":{\"a\":1}}\n"));
    strcpy(first, session.path);
    /* A second open moves to a distinct file and releases the first. */
    assert(!session_open(&session, WORKSPACE));
    assert(strcmp(first, session.path) && open_count == 1 && file_count() == 2);
    session_close(&session);
    assert(!session.open && open_count == 0 && session_journal(&session, "x", "{}") == -1);
    session_close(&session); assert(open_count == 0);
    reset(); flush_error = 1; assert(!session_open(&session, WORKSPACE));
    assert(session_journal(&session, "x", "{}") == -1); /* flush failure is a failed record */
    reset(); assert(!session_open(&session, WORKSPACE));
    fail_write_at = writes + 2; assert(session_journal(&session, "x", "{}") == -1);
}

static void test_open_failure_keeps_current(void)
{
    char path[256];
    reset(); assert(!session_open(&session, WORKSPACE));
    strcpy(path, session.path); fail_create_at = creates + 1;
    assert(session_open(&session, WORKSPACE) == -1);
    assert(session.open && !strcmp(session.path, path) && open_count == 1);
    assert(!session_journal(&session, "x", "{}"));
}

static void test_save_handoff(void)
{
    const char *text;
    char big[6000];
    open_with_agent();
    assert(!session_save_handoff(&session, kSummary, handoff, sizeof(handoff)));
    assert(!strncmp(handoff, FOLDER "h", sizeof(FOLDER)) && open_count == 1);
    text = contents(handoff);
    assert(strstr(text, "# Sherclawk handoff") && strstr(text, session.path) && strstr(text, "Inspect sources."));
    assert(!strchr(text, '\n') && strchr(text, '\r')); /* MacRoman TEXT uses CR */
    /* Unsupported characters and oversize summaries are refused before any file exists. */
    open_with_agent(); handoff[0] = 'x';
    assert(session_save_handoff(&session, "Unsupported emoji: \xf0\x9f\xa6\x80", handoff, sizeof(handoff)) == -1);
    assert(!handoff[0] && file_count() == 1);
    memset(big, 'x', sizeof(big) - 1); big[sizeof(big) - 1] = 0;
    assert(session_save_handoff(&session, big, handoff, sizeof(handoff)) == -1 && file_count() == 1);
    /* A partial file is reported so it can be shown, and the failure is a failure. */
    short_write = 1;
    assert(session_save_handoff(&session, kSummary, handoff, sizeof(handoff)) == -1);
    assert(!strncmp(handoff, FOLDER "h", sizeof(FOLDER)) && find_name(handoff) >= 0 && open_count == 1);
    short_write = 0; corrupt_read = 1; ticks++;
    assert(session_save_handoff(&session, kSummary, handoff, sizeof(handoff)) == -1 && open_count == 1);
    corrupt_read = 0; flush_error = 1; ticks++;
    assert(session_save_handoff(&session, kSummary, handoff, sizeof(handoff)) == -1);
    flush_error = 0;
    /* Never overwrite: with the clock stuck, the next save skips existing names. */
    {
        char first[256];
        ticks = 0x500;
        assert(!session_save_handoff(&session, kSummary, first, sizeof(first)));
        assert(!session_save_handoff(&session, kSummary, handoff, sizeof(handoff)));
        assert(strcmp(first, handoff));
    }
    /* A path buffer too small for the name is a failure, not a truncated path. */
    ticks = 0x900;
    assert(session_save_handoff(&session, kSummary, handoff, 8) == -1);
}

static void test_commit_handoff(void)
{
    char old_path[256], first[256], error[256];
    open_with_agent(); strcpy(old_path, session.path);
    assert(!session_commit_handoff(&session, &agent, kSummary, handoff, sizeof(handoff)));
    assert(strcmp(old_path, session.path) && session.open && open_count == 1);
    assert(agent.messages == 1 && !agent.active && agent.journal == session_journal && agent.journal_context == &session);
    assert(strstr(agent.history, "Inspect sources.") && strstr(agent.history, old_path) && strstr(agent.history, handoff));
    assert(agent.cost_micros == 1234 && agent.cost_seen && !agent.context_seen);
    assert(strstr(contents(session.path), "\"event\":\"handoff_seed\""));
    /* The old journal is closed: it keeps its records and gets no more. */
    {
        size_t old_size = (size_t)files[find_name(old_path)].size;
        assert(!agent_begin(&agent, "Continue", error, sizeof(error)));
        assert((size_t)files[find_name(old_path)].size == old_size);
        assert(strstr(contents(session.path), "Continue"));
        assert(!agent_stop(&agent, "diagnostic ended"));
    }
    /* Even with the clock stuck, a second handoff saves a distinct file. */
    strcpy(first, handoff);
    assert(!session_commit_handoff(&session, &agent, kSummary, handoff, sizeof(handoff)));
    assert(strcmp(first, handoff) && open_count == 1);
}

static void test_commit_failures_leave_agent_untouched(void)
{
    char old_path[256], big[6000];
    open_with_agent(); strcpy(old_path, session.path);
    assert(session_commit_handoff(&session, &agent, "Unsupported emoji: \xf0\x9f\xa6\x80", handoff, sizeof(handoff)) == -1);
    assert_untouched(old_path, 1);
    memset(big, 'x', sizeof(big) - 1); big[sizeof(big) - 1] = 0;
    assert(session_commit_handoff(&session, &agent, big, handoff, sizeof(handoff)) == -1);
    assert_untouched(old_path, 1);
    /* Handoff file written, successor journal cannot be created. */
    fail_create_at = creates + 2;
    assert(session_commit_handoff(&session, &agent, kSummary, handoff, sizeof(handoff)) == -1);
    assert(find_name(handoff) >= 0); assert_untouched(old_path, 1);
    /* Seeding fails (empty summary); the half-built journal is released. */
    assert(session_commit_handoff(&session, &agent, "", handoff, sizeof(handoff)) == -1);
    assert_untouched(old_path, 1);
    /* Handoff cannot be verified. */
    corrupt_read = 1;
    assert(session_commit_handoff(&session, &agent, kSummary, handoff, sizeof(handoff)) == -1);
    corrupt_read = 0; assert_untouched(old_path, 1);
    /* The journal cannot record the seed. */
    fail_write_at = writes + 3; /* handoff body, then the seed record's prefix */
    assert(session_commit_handoff(&session, &agent, kSummary, handoff, sizeof(handoff)) == -1);
    fail_write_at = 0; assert_untouched(old_path, 1);
    /* Without an open journal there is nothing to hand off from. */
    session_close(&session);
    assert(session_commit_handoff(&session, &agent, kSummary, handoff, sizeof(handoff)) == -1);
    assert(!memcmp(&before, &agent, sizeof(agent)) && !session.open);
}

int main(void)
{
    test_open_and_journal();
    test_open_failure_keeps_current();
    test_save_handoff();
    test_commit_handoff();
    test_commit_failures_leave_agent_untouched();
    puts("session tests passed");
    return 0;
}
