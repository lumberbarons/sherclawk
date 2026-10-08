/* Session journal and handoff persistence; see session.h. Journals are private
 * conversation records, separate from the credential-free app log. */
#include "session.h"
#include "text.h"
#include <Files.h>
#include <Script.h>
#include <stdio.h>
#include <string.h>

static void pstr(Str255 p, const char *s)
{
    size_t n = strlen(s);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(&p[1], s, n);
}

/* Create a candidate journal without touching the current one. Handoff
 * failures must leave the original history and its recording sink intact. */
static int create_journal(const char *folder, char *out_path, size_t cap, short *out_ref)
{
    extern unsigned long TickCount(void);
    Str255 path;
    FSSpec spec;
    long dir;
    OSErr err;
    int attempt;
    pstr(path, folder);
    err = FSMakeFSSpec(0, 0, path, &spec);
    if (err == fnfErr) err = FSpDirCreate(&spec, smSystemScript, &dir);
    if (err != noErr && err != dupFNErr) return -1;
    for (attempt = 0; attempt < 100; attempt++) {
        if (snprintf(out_path, cap, "%ss%08lx.jsonl", folder,
                 ((unsigned long)TickCount() + (unsigned long)attempt) & 0xffffffffUL) >= (int)cap) return -1;
        pstr(path, out_path);
        err = FSMakeFSSpec(0, 0, path, &spec);
        if (err == noErr) continue;
        if (err != fnfErr || FSpCreate(&spec, 'ShCk', 'TEXT', smSystemScript) != noErr) return -1;
        if (FSpOpenDF(&spec, fsWrPerm, out_ref)) return -1;
        return 0;
    }
    return -1;
}

/* A fully initialised Session for a freshly created journal. */
static int session_create(Session *out, const char *workspace)
{
    memset(out, 0, sizeof(*out));
    if (snprintf(out->folder, sizeof(out->folder), "%sSherclawk Sessions:", workspace) >= (int)sizeof(out->folder)) return -1;
    if (create_journal(out->folder, out->path, sizeof(out->path), &out->ref)) return -1;
    out->open = 1;
    return 0;
}

int session_open(Session *s, const char *workspace)
{
    Session next;
    if (session_create(&next, workspace)) return -1;
    session_close(s);
    *s = next;
    return 0;
}

void session_close(Session *s)
{
    if (s->open) { FSClose(s->ref); FlushVol(NULL, 0); s->open = 0; }
}

static int write_all(short ref, const char *text)
{
    long length = (long)strlen(text), written = length;
    return FSWrite(ref, &written, text) != noErr || written != length ? -1 : 0;
}

int session_journal(void *session, const char *event, const char *json)
{
    Session *s = session;
    char prefix[100];
    if (!s || !s->open) return -1;
    snprintf(prefix, sizeof(prefix), "{\"event\":\"%s\",\"message\":", event);
    if (write_all(s->ref, prefix) || write_all(s->ref, json) || write_all(s->ref, "}\n") ||
        FlushVol(NULL, 0) != noErr) return -1;
    return 0;
}

/* Native Markdown is MacRoman/CR/TEXT so read_text can read it in later
 * sessions. Never overwrite a handoff, and verify bytes after close/flush. */
int session_save_handoff(Session *s, const char *summary, char *path_out, size_t cap)
{
    extern unsigned long TickCount(void);
    static char utf8[10000], bytes[4097], verified[4097];
    Str255 path;
    FSSpec spec;
    short ref;
    long length, written, actual;
    int attempt, n;
    OSErr err, close_err;
    if (cap) path_out[0] = 0;
    n = snprintf(utf8, sizeof(utf8), "# Sherclawk handoff\n\nOriginal journal: %s\n"
        "This is a model-generated summary. Verify current files and observed results.\n\n%s\n",
        s->path, summary);
    if (n < 0 || (size_t)n >= sizeof(utf8) ||
        (n = text_to_macroman_strict(utf8, bytes, sizeof(bytes))) < 0) return -1;
    length = n;
    for (attempt = 0; attempt < 100; attempt++) {
        char candidate[256];
        if (snprintf(candidate, sizeof(candidate), "%sh%08lx.md", s->folder,
            ((unsigned long)TickCount() + (unsigned long)attempt) & 0xffffffffUL) >= (int)sizeof(candidate)) return -1;
        pstr(path, candidate);
        err = FSMakeFSSpec(0, 0, path, &spec);
        if (err == noErr) continue;
        if (err != fnfErr || FSpCreate(&spec, 'ShCk', 'TEXT', smSystemScript)) return -1;
        /* Retain/report even partial files. */
        if (snprintf(path_out, cap, "%s", candidate) >= (int)cap) return -1;
        if (FSpOpenDF(&spec, fsWrPerm, &ref)) return -1;
        written = length; err = FSWrite(ref, &written, bytes);
        close_err = FSClose(ref);
        if (err || written != length || close_err || FlushVol(NULL, 0)) return -1;
        if (FSpOpenDF(&spec, fsRdPerm, &ref)) return -1;
        err = GetEOF(ref, &actual);
        written = length;
        if (!err && actual == length) err = FSRead(ref, &written, verified);
        close_err = FSClose(ref);
        if (err || actual != length || written != length || close_err || memcmp(bytes, verified, (size_t)length)) return -1;
        return 0;
    }
    return -1;
}

int session_commit_handoff(Session *s, Agent *agent, const char *summary,
                           char *path_out, size_t cap)
{
    static Agent candidate; /* too large for a classic Mac stack */
    Session next;
    if (!s->open) return -1;
    if (session_save_handoff(s, summary, path_out, cap)) return -1;
    memset(&next, 0, sizeof(next));
    strcpy(next.folder, s->folder);
    if (create_journal(next.folder, next.path, sizeof(next.path), &next.ref)) return -1;
    next.open = 1;
    agent_reset(&candidate, session_journal, &next);
    if (agent_handoff_seed(&candidate, summary, s->path, path_out)) {
        FSClose(next.ref); return -1;
    }
    /* Cost is a session total, not history: the fresh candidate keeps it and
     * leaves the context line unset until the next reply. */
    candidate.cost_micros = agent->cost_micros;
    candidate.cost_seen = agent->cost_seen;
    /* The old agent is untouched until both durable files exist. */
    session_close(s);
    *s = next;
    candidate.journal_context = s;
    *agent = candidate;
    return 0;
}
