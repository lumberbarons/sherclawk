/* Exercise handoff persistence and history switching with the actual OS 9
 * File Manager. Synthetic summaries only; no network or credentials used. */
#define main SherclawkMain
#include "../main.c"
#undef main

static Agent original;
static FILE *check_log;
static int failures;
static void Check(const char *name, int ok)
{
    fprintf(check_log, "%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}
static int CommitHandoff(void)
{
    return session_commit_handoff(&gSession, &gAgent, gHandoffSummary, gHandoffPath, sizeof(gHandoffPath));
}
int main(void)
{
    char error[256], old_path[256];
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL); InitCursor();
    UIInit();
    check_log = fopen("Retro68:SherclawkHandoffCheck.log", "w");
    if (!check_log) return 1;
    fprintf(check_log, "FreeMem after UI=%ld, MaxBlock=%ld\n", (long)FreeMem(), (long)MaxBlock());
    agent_reset(&gAgent, session_journal, &gSession);
    Check("create original journal", !session_open(&gSession, tools_workspace()));
    Check("record original user", !agent_begin(&gAgent, "Build a synthetic prototype", error, sizeof(error)));
    Check("stop before handoff", !agent_stop(&gAgent, "diagnostic"));
    original = gAgent; strcpy(old_path, gSession.path);

    strcpy(gHandoffSummary, "Unsupported emoji: \xf0\x9f\xa6\x80");
    Check("unsupported encoding refused", CommitHandoff() == -1);
    Check("encoding failure retains history and journal", !memcmp(&original, &gAgent, sizeof(gAgent)) && !strcmp(old_path, gSession.path) && gSession.open);
    memset(gHandoffSummary, 'x', 5000); gHandoffSummary[5000] = 0;
    Check("oversize Markdown refused", CommitHandoff() == -1);
    Check("size failure retains history", !memcmp(&original, &gAgent, sizeof(gAgent)));

    strcpy(gHandoffSummary, "## Goal\nBuild a synthetic prototype.\n\n## Completed\nNo files changed.\n\n## Verification\nNot built.\n\n## Next steps\nInspect sources before editing.\n");
    Check("save verify and seed new journal", !CommitHandoff());
    Check("new history is inactive and seeded", gAgent.messages == 1 && !gAgent.active && strstr(gAgent.history, "synthetic prototype") && strstr(gAgent.history, old_path) && strstr(gAgent.history, gHandoffPath));
    Check("journal switched", strcmp(old_path, gSession.path) && gSession.open);
    fprintf(check_log, "original=%s\nhandoff=%s\nnew=%s\n", old_path, gHandoffPath, gSession.path);
    {
        char first[256];
        strcpy(first, gHandoffPath);
        Check("second handoff saves a distinct file", !CommitHandoff() && strcmp(first, gHandoffPath));
    }
    Check("continue from seed", !agent_begin(&gAgent, "Continue", error, sizeof(error)) && agent_request(&gAgent, "model", gJSON, sizeof(gJSON)) > 0 && strstr(gJSON, "Inspect sources"));
    agent_stop(&gAgent, "diagnostic ended");
    session_close(&gSession);
    fprintf(check_log, "RESULT failures=%d\n", failures);
    fclose(check_log); FlushVol(NULL, 0);
    ShowMessage("Handoff diagnostic", failures ? "FAILED. See SherclawkHandoffCheck.log" : "PASS. Markdown saved, verified, and seeded into a new journal. Encoding and size failures retained original history.");
    SetStatus("Handoff diagnostic: %d failure(s).", failures);
    while (!gQuit) {
        EventRecord event;
        WaitNextEvent(everyEvent, &event, 10, NULL);
        SetPort(gWindow); HandleEvent(&event);
    }
    UIDispose();
    return failures ? 1 : 0;
}
