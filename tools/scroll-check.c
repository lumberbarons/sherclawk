/* Exercise Sherclawk's actual UI against the guest TextEdit/Control Manager.
 * Host mocks miss TESelView's disabled-autoscroll behavior and destRect drift.
 * Only synthetic transcript text is used; no requests or credentials are sent. */
#define main SherclawkMain
#include "app/main.c"
#undef main
#include "harness/native/diagnostic.h"

static FILE *check_log;
static int failures;

static void Check(const char *name, int ok)
{
    harness_check(ok, name);
    fprintf(check_log, "%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

static void CheckPosition(const char *name, short expected)
{
    long actual = (long)(*gResponseTE)->viewRect.top - (*gResponseTE)->destRect.top;
    short value = GetControlValue(gResponseScroll);
    int ok = actual == expected && value == expected;
    harness_check(ok, name);
    fprintf(check_log, "%s %s: text=%ld control=%d expected=%d\n",
            ok ? "PASS" : "FAIL", name, actual, value, expected);
    if (!ok) failures++;
}

int main(void)
{
    static char text[CHAT_TRANSCRIPT_CAP];
    char logpath[256];
    short maximum, lh;
    int i;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL); InitCursor();
    if (harness_begin("SherclawkScrollCheck") < 0) return 1;
    if (harness_workspace()) tools_set_workspace(harness_workspace());
    RunInit();
    UIInit();
    snprintf(logpath, sizeof(logpath), "%sSherclawkScrollCheck.log",
             tools_workspace());
    check_log = fopen(logpath, "w");
    if (!check_log || !gResponseTE || !gResponseScroll) return harness_finish(1);

    /* Exercise the main screen's two-pane focus cycle and read-only transcript
     * with the guest TextEdit Manager, rather than a host UI stub. */
    {
        EventRecord event;
        memset(&event, 0, sizeof(event));
        event.what = keyDown; event.message = 0x09;
        HandleEvent(&event);
        Check("focus response pane", gFocusedTE == gResponseTE);
        event.message = 'x'; HandleEvent(&event);
        Check("response transcript is read-only", !(*gResponseTE)->teLength);
        event.message = 0x09; HandleEvent(&event);
        Check("focus prompt pane", gFocusedTE == gPromptTE);
        event.message = 'x'; HandleEvent(&event);
        Check("prompt accepts text", (*gPromptTE)->teLength == 1);
        TESetText("", 0, gPromptTE);
        fprintf(check_log, "two-pane focus and read-only transcript checked\n");
    }

    for (i = 0; i < 100; i++) strcat(text, "Synthetic transcript line\r");
    strcat(text, "END OF REPLY");
    ResponseSetText(text, strlen(text));
    CheckPosition("loaded at top", 0);
    maximum = GetControlMaximum(gResponseScroll);
    lh = PaneLineHeight(gResponseTE);
    Check("transcript exceeds one page", maximum > PanePageHeight(gResponseTE));
    ResponseScrollTo(maximum);
    CheckPosition("reply revealed at bottom", maximum);
    Check("bottom alignment", (long)(*gResponseTE)->destRect.top +
          (*gResponseTE)->nLines * lh == (*gResponseTE)->viewRect.bottom);
    ScrollActionProc(gResponseScroll, kControlUpButtonPart);
    CheckPosition("up arrow", maximum - lh);
    ScrollActionProc(gResponseScroll, kControlDownButtonPart);
    CheckPosition("down arrow", maximum);
    ScrollActionProc(gResponseScroll, kControlPageUpPart);
    CheckPosition("page up", maximum - PanePageHeight(gResponseTE));
    ScrollActionProc(gResponseScroll, kControlPageDownPart);
    CheckPosition("page down", maximum);

    /* TrackControl changes the control before the thumb handler scrolls. */
    SetControlValue(gResponseScroll, maximum / 2);
    ResponseScrollTo(GetControlValue(gResponseScroll));
    CheckPosition("thumb to middle", maximum / 2);
    ResponseScrollTo(0);
    CheckPosition("thumb to top", 0);
    /* Reproduce the old mismatch: bottom thumb with text still at top. */
    SetControlValue(gResponseScroll, maximum);
    ScrollActionProc(gResponseScroll, kControlDownButtonPart);
    CheckPosition("stale control corrected", lh);
    ResponseScrollTo(100000L);
    CheckPosition("upper clamp", maximum);
    ResponseScrollTo(-100000L);
    CheckPosition("lower clamp", 0);

    ResponseScrollTo(maximum);
    ResponseSetText("Short reply", 11);
    CheckPosition("short replacement clears old offset", 0);
    Check("short replacement resets the maximum",
          GetControlMaximum(gResponseScroll) == 0);
    ResponseSetText(text, strlen(text));
    ResponseScrollTo(GetControlMaximum(gResponseScroll));
    CheckPosition("next transcript follows bottom", maximum);
    NewChat();
    CheckPosition("new chat clears old offset", 0);

    /* Wrap a byte-limit transcript, then stress the explicit CR ceiling. */
    memset(text, 'W', sizeof(text) - 1); text[sizeof(text) - 1] = 0;
    ResponseSetText(text, strlen(text));
    ResponseScrollTo(GetControlMaximum(gResponseScroll));
    CheckPosition("30000 bytes with wrapping", GetControlMaximum(gResponseScroll));
    for (i = 0; i < 1500; i++) text[i] = '\r';
    strcpy(text + i, "END");
    ResponseSetText(text, strlen(text));
    maximum = GetControlMaximum(gResponseScroll);
    ResponseScrollTo(maximum);
    CheckPosition("1500 separators", maximum);
    ScrollActionProc(gResponseScroll, kControlDownButtonPart);
    CheckPosition("bottom remains pinned", maximum);
    ResponseScrollTo(0);
    CheckPosition("long transcript returns to top", 0);

    fprintf(check_log, "RESULT failures=%d\n", failures);
    fclose(check_log); FlushVol(NULL, 0);
    UIDispose();
    return harness_finish(failures);
}
