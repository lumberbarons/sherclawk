/* Exercise Sherclawk's actual UI against the guest TextEdit/Control Manager.
 * Host mocks miss TESelView's disabled-autoscroll behavior and destRect drift.
 * Only synthetic transcript text is used; no requests or credentials are sent. */
#define main SherclawkMain
#include "../main.c"
#undef main

static FILE *check_log;
static int failures;

static void CheckPosition(const char *name, short expected)
{
    long actual = (long)(*gResponseTE)->viewRect.top - (*gResponseTE)->destRect.top;
    short value = GetControlValue(gResponseScroll);
    int ok = actual == expected && value == expected;
    fprintf(check_log, "%s %s: text=%ld control=%d expected=%d\n",
            ok ? "PASS" : "FAIL", name, actual, value, expected);
    if (!ok) failures++;
}

int main(void)
{
    static char text[CHAT_TRANSCRIPT_CAP];
    short maximum, lh;
    int i;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL); InitCursor();
    UIInit();
    check_log = fopen("Retro68:SherclawkScrollCheck.log", "w");
    if (!check_log || !gResponseTE || !gResponseScroll) return 1;

    /* Exercise the main screen's two-pane focus cycle and read-only transcript
     * with the guest TextEdit Manager, rather than a host UI stub. */
    {
        EventRecord event;
        memset(&event, 0, sizeof(event));
        event.what = keyDown; event.message = 0x09;
        HandleEvent(&event);
        if (gFocusedTE != gResponseTE) failures++;
        event.message = 'x'; HandleEvent(&event);
        if ((*gResponseTE)->teLength) failures++;
        event.message = 0x09; HandleEvent(&event);
        if (gFocusedTE != gPromptTE) failures++;
        event.message = 'x'; HandleEvent(&event);
        if ((*gPromptTE)->teLength != 1) failures++;
        TESetText("", 0, gPromptTE);
        fprintf(check_log, "two-pane focus and read-only transcript checked\n");
    }

    for (i = 0; i < 100; i++) strcat(text, "Synthetic transcript line\r");
    strcat(text, "END OF REPLY");
    ResponseSetText(text, strlen(text));
    CheckPosition("loaded at top", 0);
    maximum = GetControlMaximum(gResponseScroll);
    lh = ResponseLineHeight();
    if (maximum <= ResponsePageHeight()) failures++;
    ResponseScrollTo(maximum);
    CheckPosition("reply revealed at bottom", maximum);
    if ((long)(*gResponseTE)->destRect.top + (*gResponseTE)->nLines * lh !=
        (*gResponseTE)->viewRect.bottom) failures++;
    ScrollActionProc(gResponseScroll, kControlUpButtonPart);
    CheckPosition("up arrow", maximum - lh);
    ScrollActionProc(gResponseScroll, kControlDownButtonPart);
    CheckPosition("down arrow", maximum);
    ScrollActionProc(gResponseScroll, kControlPageUpPart);
    CheckPosition("page up", maximum - ResponsePageHeight());
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
    if (GetControlMaximum(gResponseScroll) != 0) failures++;
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
    return failures ? 1 : 0;
}
