/* Sherclawk — cooperative Mac OS 9 OpenRouter chat. TextEdit is MacRoman;
 * JSON/history remain UTF-8. Network and conversation code also run in the
 * diagnostic rig, so live tests exercise the app's actual protocol code. */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <Controls.h>
#include <ControlDefinitions.h>
#include <ToolUtils.h>
#include <Scrap.h>
#include <Files.h>
#include <Folders.h>
#include <OpenTransport.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "certainly.h"
#include "http.h"
#include "network.h"
#include "timing.h"
#include "agent.h"
#include "json.h"
#include "tools.h"
#include "build_project.h"
#include "selfbuild.h"
#include <AppleEvents.h>
#include "run_application.h"
#include <QDOffscreen.h>
#include <Resources.h>
#include "text.h"
#include "config.h"
#include "preferences.h"

/* ── Menu IDs ─────────────────────────────────────────────────────── */
enum {
    kAppleMenuID = 128,
    kFileMenuID  = 129,
    kEditMenuID  = 130,
    kEditPrefsItem = 10   /* Edit menu: last item ("Preferences…") */
};

/* ── Layout ───────────────────────────────────────────────────────── */
enum {
    kWinLeft   = 40,
    kWinTop    = 40,
    kWinWidth  = 600,
    kWinHeight = 436,

    kPad       = 10,
    kFieldH    = 20,
    kButtonW   = 64,
    kScrollW   = 15
};

/* ── UI state ─────────────────────────────────────────────────────── */
static WindowPtr     gWindow = NULL;
static ControlHandle gSendBtn = NULL, gStopBtn = NULL, gNewBtn = NULL, gHandoffBtn = NULL;
static int gHandoffEnabled = -1;
static TEHandle gPromptTE = NULL;
static Rect gPromptRect, gPromptLabelRect, gStopRect, gNewRect, gHandoffRect;
static TEHandle      gModelTE = NULL;
static TEHandle      gResponseTE = NULL;
static ControlHandle gResponseScroll = NULL;
static ControlActionUPP gScrollActionUPP = NULL;
static TEHandle      gFocusedTE = NULL;

static Rect gModelLabelRect, gModelRect, gSendRect;
static Rect gStatusLabelRect, gStatusRect;
static Rect gHistoryRect;
static size_t gDisplayedHistory = (size_t)-1;
static Rect gUsageRect;
static char gModelInfoModel[CHAT_MODEL_CAP];
static long gModelInfoLimit = -1;
static int  gModelInfoAttempted = 0;
static long long gDisplayedCost = -1;
static long gDisplayedTokens = -1;
static long gDisplayedLimit = -1;
static Rect gResponseLabelRect, gResponseRect, gResponseViewRect;

static MenuHandle gAppleMenu = NULL;
static MenuHandle gFileMenu  = NULL;
static MenuHandle gEditMenu  = NULL;

static char gStatusText[256] = "Idle.";
static int  gQuit = 0;

/* ── Preferences ──────────────────────────────────────────────────── */
static Prefs gPrefs;
static int   gPrefsUnreadable = 0;   /* existing file could not be read */
#define kPrefsFileName "\pSherclawk Preferences"
#define kPrefsCreator 'ShCk'
#define kPrefsFileType 'pref'

/* ── Request state ──────────────────────────────────────────────── */
static Chat gChat; /* Only the bounded native display is reused. */
static Agent gAgent;
static char gRunModel[CHAT_MODEL_CAP];
static GWorldPtr gArt = NULL;
static short gSessionRef = 0;
static int gSessionOpen = 0;
static char gSessionPath[256];
static int Journal(void *context, const char *event, const char *json);
static ChatNetwork gNet;
static char gPending[CHAT_PROMPT_CAP * 3];
static char gJSON[CHAT_REQUEST_CAP];
static char gToolResult[AGENT_RESULT_CAP];
static int gSending = 0, gOTOpen = 0;
static int gLookupDrain = 0;
static int gHandoff = 0;
static Agent gHandoffCandidate;
static char gHandoffSummary[8192], gHandoffPath[256];
static uint32_t gStartTicks;
static RoundTiming gRoundTiming;
static uint32_t gToolStart;
static void FocusSet(TEHandle te);
static void SendChat(void);
static void AbortChat(const char *reason);
static void NewChat(void);
static void StartHandoff(void);

/* ── Small helpers ────────────────────────────────────────────────── */

static void PStr(Str255 p, const char *s)
{
    size_t n = strlen(s);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(&p[1], s, n);
}

static void DrawLabel(const Rect *r, const char *text)
{
    Str255 p;
    PStr(p, text);
    MoveTo(r->left, r->bottom - 4);
    DrawString(p);
}

/* Replace the status line text; redraw only when it actually changed. */
static void SetStatus(const char *fmt, ...)
{
    char    buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (strcmp(buf, gStatusText) == 0) return;
    strcpy(gStatusText, buf);
    if (gWindow) InvalRect(&gStatusRect);
}

static const char *TLSVersionLabel(MacTLS_Version v)
{
    switch (v) {
    case kMacTLS_Version13: return "TLS 1.3";
    case kMacTLS_Version12: return "TLS 1.2";
    default:                return "TLS ?";
    }
}

static void UpdateHandoffControls(void)
{
    int enabled = !gSending && !gLookupDrain && gSessionOpen && gAgent.messages &&
        !gAgent.active && gAgent.next >= gAgent.count;
    if (enabled == gHandoffEnabled) return;
    gHandoffEnabled = enabled;
    if (gHandoffBtn) HiliteControl(gHandoffBtn, enabled ? 0 : 255);
    if (gFileMenu) {
        if (enabled) EnableItem(gFileMenu, 2);
        else DisableItem(gFileMenu, 2);
    }
}

static void SetSendEnabled(int enabled)
{
    if (gSendBtn) HiliteControl(gSendBtn, enabled ? 0 : 255);
    if (gNewBtn) HiliteControl(gNewBtn, enabled ? 0 : 255);
    if (gFileMenu) {
        if (enabled) EnableItem(gFileMenu, 1);
        else DisableItem(gFileMenu, 1);
    }
    if (gEditMenu) {
        if (enabled) EnableItem(gEditMenu, kEditPrefsItem);
        else DisableItem(gEditMenu, kEditPrefsItem);
    }
    UpdateHandoffControls();
    if (gStopBtn) HiliteControl(gStopBtn, enabled ? 255 : 0);
    if (!enabled && gFocusedTE != gResponseTE) FocusSet(gResponseTE);
}

/* ── Preferences file (System Folder:Preferences) ─────────────────── */

static OSErr PrefsSpec(FSSpec *spec)
{
    short vRefNum;
    long  dirID;
    OSErr err = FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder,
                           &vRefNum, &dirID);
    if (err) return err;
    return FSMakeFSSpec(vRefNum, dirID, (ConstStr255Param)kPrefsFileName, spec);
}

/* Load once at startup. A missing file is normal (defaults); an unreadable or
 * oversized one keeps the compiled defaults and reports through the status
 * line after the window exists. */
static void PrefsLoad(void)
{
    static char bytes[PREFS_MAX_BYTES];
    FSSpec spec;
    short ref = 0;
    long length = 0, count;
    OSErr err, close_err;

    prefs_defaults(&gPrefs);
    gPrefsUnreadable = 0;
    err = PrefsSpec(&spec);
    if (err == fnfErr) return;               /* first run: compiled defaults */
    if (err) { gPrefsUnreadable = 1; return; }
    err = FSpOpenDF(&spec, fsRdPerm, &ref);
    if (err == fnfErr) return;               /* file disappeared after the probe */
    if (err) { gPrefsUnreadable = 1; return; }
    err = GetEOF(ref, &length);
    if (err || length < 0 || length > (long)sizeof(bytes)) err = ioErr;
    if (!err) {
        count = length;
        err = FSRead(ref, &count, bytes);
        if (!err && count != length) err = ioErr;
    }
    close_err = FSClose(ref);
    if (err || close_err) { gPrefsUnreadable = 1; return; }
    prefs_parse(bytes, (size_t)length, &gPrefs);
}

/* Write, flush and read back the whole file; a save that cannot be verified
 * leaves the previous file and the previous in-memory values in place. */
static int PrefsSave(const Prefs *p)
{
    static char bytes[PREFS_MAX_BYTES];
    static char verified[PREFS_MAX_BYTES];
    FSSpec spec;
    short ref = 0;
    long length, written, actual;
    OSErr err, close_err;

    length = prefs_format(p, bytes, sizeof(bytes));
    if (length < 0) return -1;
    err = PrefsSpec(&spec);
    if (err == fnfErr) err = noErr;          /* create below; spec is usable */
    if (!err) {
        err = FSpCreate(&spec, kPrefsCreator, kPrefsFileType, smSystemScript);
        if (err == dupFNErr) err = noErr;
    }
    if (!err) err = FSpOpenDF(&spec, fsWrPerm, &ref);
    if (!err) err = SetEOF(ref, 0);
    if (!err) {
        written = length;
        err = FSWrite(ref, &written, bytes);
        if (!err && written != length) err = ioErr;
    }
    if (ref) {
        close_err = FSClose(ref); ref = 0;
        if (!err) err = close_err;
    }
    if (!err) err = FlushVol(NULL, spec.vRefNum);
    if (!err) err = FSpOpenDF(&spec, fsRdPerm, &ref);
    if (!err) err = GetEOF(ref, &actual);
    if (!err && actual != length) err = ioErr;
    if (!err) {
        written = length;
        err = FSRead(ref, &written, verified);
        if (!err && written != length) err = ioErr;
    }
    if (!err && memcmp(bytes, verified, (size_t)length)) err = ioErr;
    if (ref) {
        close_err = FSClose(ref); ref = 0;
        if (!err) err = close_err;
    }
    return err ? -1 : 0;
}

/* ── Preferences dialog (DLOG/DITL 128) ───────────────────────────── */

/* DITL item numbers: the OK/Cancel buttons, then label/edit pairs in order,
 * the debug checkbox and the location hint. */
enum {
    kPrefsDialogID     = 128,
    kPrefsAlertID      = 128,
    kPrefsOKItem       = 1,
    kPrefsCancelItem   = 2,
    kPrefsModelItem    = 4,
    kPrefsKeyItem      = 6,
    kPrefsWorkspaceItem = 8,
    kPrefsRoundsItem   = 10,
    kPrefsToolsItem    = 12,
    kPrefsDebugItem    = 13
};

static Handle PrefsItem(DialogPtr dlg, short item)
{
    short  type;
    Handle handle;
    Rect   rect;
    GetDialogItem(dlg, item, &type, &handle, &rect);
    return handle;
}

static void SetPrefsText(DialogPtr dlg, short item, const char *text)
{
    Str255 p;
    PStr(p, text);
    SetDialogItemText(PrefsItem(dlg, item), p);
}

static void GetPrefsText(DialogPtr dlg, short item, char *out, size_t cap)
{
    Str255 p;
    size_t n;
    GetDialogItemText(PrefsItem(dlg, item), p);
    n = p[0];
    if (n >= cap) n = cap - 1;
    memcpy(out, p + 1, n);
    out[n] = 0;
}

static ControlHandle PrefsCheckbox(DialogPtr dlg)
{
    return (ControlHandle)PrefsItem(dlg, kPrefsDebugItem);
}

/* The dialog stays open on a rejected value; the alert explains why. */
static void PrefsProblem(DialogPtr dlg, const char *message)
{
    Str255 p;
    PStr(p, message);
    ParamText(p, NULL, NULL, NULL);
    StopAlert(kPrefsAlertID, NULL);
    SetPort(dlg);
}

static void ShowPreferences(void)
{
    static Prefs candidate;   /* the live values change only after a verified save */
    DialogPtr dlg;
    short item;
    int done = 0, debug = gPrefs.show_tool_debug, rounds, tools;
    char model[CHAT_MODEL_CAP], key[PREFS_KEY_CAP], workspace[PREFS_WORKSPACE_CAP];
    char rounds_text[8], tools_text[8];

    dlg = GetNewDialog(kPrefsDialogID, NULL, (WindowPtr)-1L);
    if (!dlg) { SetStatus("Could not open the Preferences dialog."); return; }
    SetPort(dlg);
    SetPrefsText(dlg, kPrefsModelItem, gPrefs.model);
    SetPrefsText(dlg, kPrefsKeyItem, gPrefs.api_key);
    SetPrefsText(dlg, kPrefsWorkspaceItem, gPrefs.workspace);
    snprintf(rounds_text, sizeof(rounds_text), "%d", gPrefs.max_rounds);
    snprintf(tools_text, sizeof(tools_text), "%d", gPrefs.max_tools);
    SetPrefsText(dlg, kPrefsRoundsItem, rounds_text);
    SetPrefsText(dlg, kPrefsToolsItem, tools_text);
    SetControlValue(PrefsCheckbox(dlg), debug);
    SelectDialogItemText(dlg, kPrefsModelItem, 0, 32767);
    SetDialogDefaultItem(dlg, kPrefsOKItem);
    SetDialogCancelItem(dlg, kPrefsCancelItem);

    while (!done) {
        ModalDialog(NULL, &item);
        if (item == kPrefsDebugItem) {
            /* ModalDialog reports the checkbox hit but does not toggle it. */
            ControlHandle ctl = PrefsCheckbox(dlg);
            debug = !debug;
            SetControlValue(ctl, debug ? 1 : 0);
            Draw1Control(ctl);
            continue;
        }
        if (item == kPrefsCancelItem) break;
        if (item != kPrefsOKItem) continue;
        GetPrefsText(dlg, kPrefsModelItem, model, sizeof(model));
        GetPrefsText(dlg, kPrefsKeyItem, key, sizeof(key));
        GetPrefsText(dlg, kPrefsWorkspaceItem, workspace, sizeof(workspace));
        GetPrefsText(dlg, kPrefsRoundsItem, rounds_text, sizeof(rounds_text));
        GetPrefsText(dlg, kPrefsToolsItem, tools_text, sizeof(tools_text));
        if (!prefs_model_ok(model)) {
            PrefsProblem(dlg, "Enter a model ID: printable characters, no spaces.");
            SelectDialogItemText(dlg, kPrefsModelItem, 0, 32767); continue;
        }
        if (!prefs_key_ok(key)) {
            PrefsProblem(dlg, "The API key must be printable characters without spaces.");
            SelectDialogItemText(dlg, kPrefsKeyItem, 0, 32767); continue;
        }
        if (!prefs_workspace_ok(workspace)) {
            PrefsProblem(dlg, "The workspace must be a colon path ending in ':', like Retro68:.");
            SelectDialogItemText(dlg, kPrefsWorkspaceItem, 0, 32767); continue;
        }
        rounds = prefs_limit_value(rounds_text);
        if (rounds < 0) {
            PrefsProblem(dlg, "Max model rounds must be a number from 1 to 128.");
            SelectDialogItemText(dlg, kPrefsRoundsItem, 0, 32767); continue;
        }
        tools = prefs_limit_value(tools_text);
        if (tools < 0) {
            PrefsProblem(dlg, "Max tool calls must be a number from 1 to 128.");
            SelectDialogItemText(dlg, kPrefsToolsItem, 0, 32767); continue;
        }
        candidate = gPrefs;
        strcpy(candidate.model, model);
        strcpy(candidate.api_key, key);
        strcpy(candidate.workspace, workspace);
        candidate.max_rounds = rounds;
        candidate.max_tools = tools;
        candidate.show_tool_debug = debug;
        if (PrefsSave(&candidate)) {
            PrefsProblem(dlg, "Could not save the preferences file. Nothing was changed.");
            continue;
        }
        gPrefs = candidate;
        tools_set_workspace(gPrefs.workspace);
        done = 1;
    }
    CloseDialog(dlg);
    if (gWindow) SetPort(gWindow);
    if (done) {
        TESetText(gPrefs.model, (long)strlen(gPrefs.model), gModelTE);
        InvalRect(&gModelRect);
        SetStatus("Preferences saved; the workspace applies to new work now.");
    }
    FocusSet(gPromptTE);
}

/* ── Open Transport lifecycle and the share log ───────────────────── */

/* Yield to the OS for `ticks` while still pumping events (same pattern
 * as Certainly's simple_get example). */
static void YieldTicks(uint32_t ticks)
{
    uint32_t start = (uint32_t)TickCount();
    EventRecord ev;
    while ((uint32_t)TickCount() - start < ticks) {
        WaitNextEvent(everyEvent, &ev, 1, NULL);
    }
}

/*
 * Tear down the TLS context and give Open Transport a clean slate.
 *
 * Mac OS 9's OT can wedge when a connection fails mid-flight; after
 * that, every later attempt fails too. Certainly's own simple_get
 * example works around this by fully cycling InitOpenTransport /
 * CloseOpenTransport around every test, so we do the same around every
 * request (the Postman example, which inits OT once, shows the wedge).
 */
static void CloseChatContext(void)
{
    network_close(&gNet);
    YieldTicks(60);
    if (gOTOpen) { CloseOpenTransport(); gOTOpen = 0; }
    YieldTicks(60);
}

/*
 * Optional log on the deployment share: when the "Retro68" volume is
 * mounted, fetch results are appended to Retro68:Sherclawk.log so a
 * failed run can be inspected from the host without screenshots.
 * Missing volume = silent no-op.
 */
static short gLogRefNum = 0;
static int   gLogOpen   = 0;

static void LogOpen(void)
{
    HParamBlockRec pb;
    Str255         name;
    OSErr          err;
    const char    *path = "Retro68:Sherclawk.log";
    size_t         len  = strlen(path);

    if (len > 255) len = 255;
    name[0] = (unsigned char)len;
    memcpy(&name[1], path, len);

    memset(&pb, 0, sizeof(pb));
    pb.fileParam.ioNamePtr = name;
    PBHDeleteSync(&pb);                /* start each session fresh */

    memset(&pb, 0, sizeof(pb));
    pb.fileParam.ioNamePtr = name;
    err = PBHCreateSync(&pb);
    if (err != noErr && err != dupFNErr) return;

    memset(&pb, 0, sizeof(pb));
    pb.ioParam.ioNamePtr = name;
    pb.ioParam.ioPermssn = fsWrPerm;
    err = PBHOpenSync(&pb);
    if (err != noErr) return;

    gLogRefNum = pb.ioParam.ioRefNum;
    gLogOpen   = 1;
}

static void LogLine(const char *text)
{
    long              len;
    static const char crlf[2] = {'\r', '\n'};

    if (!gLogOpen) return;
    len = (long)strlen(text);
    FSWrite(gLogRefNum, &len, text);
    len = 2;
    FSWrite(gLogRefNum, &len, crlf);
    FlushVol(NULL, 0);   /* flush every line so the host can tail it */
}

/* One credential-free line per network round (phase offsets in ticks since the
 * request began) and per tool call, so tuning can see where a round goes. */
static void LogRoundTiming(const char *outcome)
{
    char line[256];
    if (!gRoundTiming.active) return;
    timing_mark(&gRoundTiming, TIMING_CLOSE, (uint32_t)TickCount());
    timing_set_bytes(&gRoundTiming, (unsigned long)gNet.sent, (unsigned long)gNet.received);
    if (timing_round_format(&gRoundTiming, outcome, line, sizeof(line)) > 0) LogLine(line);
    gRoundTiming.active = 0;
}
static void LogToolTiming(int index, const char *name)
{
    char line[96];
    if (timing_tool_format(index, name, gToolStart, (uint32_t)TickCount(), line, sizeof(line)) > 0) LogLine(line);
}
static void ObserveRound(void)
{
    uint32_t now = (uint32_t)TickCount();
    MacTLS_State state;
    if (!gNet.ctx || !gRoundTiming.active) return;
    state = MacTLS_GetState(gNet.ctx);
    if (state == kMacTLS_Handshaking || state == kMacTLS_Connected) timing_mark(&gRoundTiming, TIMING_CONNECT, now);
    if (state == kMacTLS_Connected) timing_mark(&gRoundTiming, TIMING_HANDSHAKE, now);
    if (gNet.request_len && gNet.sent == gNet.request_len) timing_mark(&gRoundTiming, TIMING_SENT, now);
    if (gNet.received) timing_mark(&gRoundTiming, TIMING_FIRST_BYTE, now);
    if (gNet.result) timing_mark(&gRoundTiming, TIMING_DONE, now);
}

static void LogClose(void)
{
    if (!gLogOpen) return;
    FSClose(gLogRefNum);
    FlushVol(NULL, 0);
    gLogOpen = 0;
}

/* ── Focus and text access ────────────────────────────────────────── */

static void FocusSet(TEHandle te)
{
    if (gFocusedTE == te) return;
    if (gFocusedTE) TEDeactivate(gFocusedTE);
    gFocusedTE = te;
    if (gFocusedTE) TEActivate(gFocusedTE);
}

static int TEGetTextInto(TEHandle te, char *buf, size_t cap)
{
    CharsHandle h;
    long        n;

    if (!te) return -1;
    h = TEGetText(te);
    if (!h) return -1;
    n = (*te)->teLength;
    if ((size_t)n + 1 > cap) return -1;
    HLock((Handle)h);
    memcpy(buf, *h, (size_t)n);
    HUnlock((Handle)h);
    buf[n] = '\0';
    return (int)n;
}

/* ── Response pane scrollbar ──────────────────────────────────────── */

static short ResponseLineHeight(void)
{
    if (!gResponseTE) return 12;
    short lh = (*gResponseTE)->lineHeight;
    return lh > 0 ? lh : 12;
}

static short ResponsePageHeight(void)
{
    short lh = ResponseLineHeight();
    Rect  v  = (*gResponseTE)->viewRect;
    short visible_lines = (v.bottom - v.top) / lh;
    return (visible_lines > 1) ? (short)((visible_lines - 1) * lh) : lh;
}

static short ClampScroll(long v)
{
    short max = GetControlMaximum(gResponseScroll);
    if (v < 0)   v = 0;
    if (v > max) v = max;
    return (short)v;
}

static short ComputeMaxScroll(void)
{
    if (!gResponseTE) return 0;
    short lh     = ResponseLineHeight();
    short nLines = (*gResponseTE)->nLines;
    Rect  view   = (*gResponseTE)->viewRect;
    long pixels = (long)nLines * lh - (view.bottom - view.top);
    if (pixels < 0) return 0;
    return (short)(pixels > 32767L ? 32767L : pixels);
}

/* The control is an indicator, not the source of the text's position.
 * TESetText preserves destRect, and TESelView does nothing unless TEAutoView
 * is enabled. Always move from the actual destination to the requested offset
 * so a reply, thumb drag, or reset cannot leave the two out of sync. */
static void ResponseScrollTo(long pixels)
{
    short after = ClampScroll(pixels);
    long before = (long)(*gResponseTE)->viewRect.top - (*gResponseTE)->destRect.top;
    TEScroll(0, (short)(before - after), gResponseTE);
    SetControlValue(gResponseScroll, after);
}

static pascal void ScrollActionProc(ControlHandle c, short part)
{
    short delta = 0;

    if (c != gResponseScroll) return;
    switch (part) {
    case kControlUpButtonPart:   delta = -ResponseLineHeight(); break;
    case kControlDownButtonPart: delta =  ResponseLineHeight(); break;
    case kControlPageUpPart:     delta = -ResponsePageHeight(); break;
    case kControlPageDownPart:   delta =  ResponsePageHeight(); break;
    default: return;
    }
    long before = (long)(*gResponseTE)->viewRect.top - (*gResponseTE)->destRect.top;
    ResponseScrollTo(before + delta);
}

static void ResponseSetText(const char *text, size_t len)
{
    long l = (long)len;

    if (!gResponseTE) return;
    if (l > 32767L) l = 32767L;   /* TERec hard cap */
    /* Reset the drawing origin before reflowing a replacement transcript. */
    (*gResponseTE)->destRect = (*gResponseTE)->viewRect;
    TESetText(text, l, gResponseTE);
    if (gResponseScroll) {
        SetControlMaximum(gResponseScroll, ComputeMaxScroll());
        SetControlValue(gResponseScroll, 0);
    }
    InvalRect(&gResponseRect);
}

/* ── Menu handling ────────────────────────────────────────────────── */

/* Returns 1 when the app should quit. */
static int HandleMenu(long menuChoice)
{
    short menuID = HiWord(menuChoice);
    short item   = LoWord(menuChoice);

    switch (menuID) {
    case kFileMenuID:
        if (item == 1) NewChat();
        if (item == 2) StartHandoff();
        if (item == 4) return 1;   /* Quit */
        break;

    case kAppleMenuID:
        if (item == 1) {
            SetStatus("Sherclawk - native OS 9 tools, powered by Certainly.");
        }
        /* Items 2+ are desk accessories added by AppendResMenu. */
        break;

    case kEditMenuID:
        if (item == kEditPrefsItem) {
            if (gSending) SetStatus("Finish the current run before changing Preferences.");
            else ShowPreferences();
            break;
        }
        if (gFocusedTE && (item == 4 || item == 8 ||
            (!gSending && gFocusedTE != gResponseTE))) {
            switch (item) {
            case 3: ZeroScrap(); TECut(gFocusedTE);   TEToScrap(); break;
            case 4: ZeroScrap(); TECopy(gFocusedTE);  TEToScrap(); break;
            case 5: {
                long incoming = 0;
                Handle scrap = NewHandle(0);
                long offset = 0;
                if (scrap) incoming = GetScrap(scrap, 'TEXT', &offset);
                if (scrap) DisposeHandle(scrap);
                long maximum = gFocusedTE == gModelTE ? CHAT_MODEL_CAP - 1 : CHAT_PROMPT_CAP - 1;
                long remaining = (*gFocusedTE)->teLength -
                    ((*gFocusedTE)->selEnd - (*gFocusedTE)->selStart);
                if (incoming > 0 && incoming <= maximum - remaining) {
                    TEFromScrap(); TEPaste(gFocusedTE);
                } else SetStatus("Paste is empty or exceeds the field limit.");
                break;
            }
            case 6: TEDelete(gFocusedTE);                          break;
            case 8: TESetSelect(0, 32767, gFocusedTE); break;
            default: break;   /* Undo: not implemented */
            }
        }
        break;

    default:
        break;
    }
    return 0;
}

/* ── Layout, drawing, UI setup ────────────────────────────────────── */

/* Compact OpenCode-style token counts: exact below 1000, else one decimal k/M. */
static void FormatTokens(long tokens, char *out, size_t cap)
{
    if (tokens < 1000) snprintf(out, cap, "%ld", tokens);
    else if (tokens < 1000000) {
        long tenths = (tokens + 50) / 100;
        snprintf(out, cap, "%ld.%ldk", tenths / 10, tenths % 10);
    } else {
        long tenths = (tokens + 50000) / 100000;
        snprintf(out, cap, "%ld.%ldM", tenths / 10, tenths % 10);
    }
}

static void ComputeLayout(void)
{
    SetRect(&gModelLabelRect, 10, 10, 48, 30);
    SetRect(&gModelRect, 54, 10, 590, 30);
    SetRect(&gStatusLabelRect, 10, 38, 56, 54);
    SetRect(&gStatusRect, 60, 38, 590, 54);
    SetRect(&gHistoryRect, 10, 254, 166, 270);
    SetRect(&gUsageRect, 10, 272, 590, 288);
    SetRect(&gResponseLabelRect, 174, 62, 340, 78);
    SetRect(&gResponseRect, 174, 82, 590, 270);
    gResponseViewRect = gResponseRect; gResponseViewRect.right -= kScrollW;
    SetRect(&gPromptLabelRect, 10, 294, 200, 310);
    SetRect(&gPromptRect, 10, 314, 590, 396);
    SetRect(&gSendRect, 10, 406, 74, 426);
    SetRect(&gStopRect, 84, 406, 148, 426);
    SetRect(&gNewRect, 158, 406, 246, 426);
    SetRect(&gHandoffRect, 478, 406, 590, 426);
}

static void DrawChrome(void)
{
    Str255 p;

    FrameRect(&gModelRect);
    DrawLabel(&gModelLabelRect, "Model:");
    if (gModelTE) TEUpdate(&(*gModelTE)->viewRect, gModelTE);

    DrawLabel(&gStatusLabelRect, "Status:");
    EraseRect(&gStatusRect);
    PStr(p, gStatusText);
    MoveTo(gStatusRect.left, gStatusRect.bottom - 4);
    DrawString(p);

    {
        Rect art = { 82, 10, 238, 166 };
        Rect destination = { 96, 10, 252, 166 };
        Str255 title;
        char history[80];
        PStr(title, "Sherclawk"); MoveTo(10, 74); TextFace(bold); DrawString(title); TextFace(normal);
        MoveTo(10, 87); PStr(title, "The consulting crustacean"); DrawString(title);
        if (gArt) {
            PixMapHandle pixels = GetGWorldPixMap(gArt);
            if (LockPixels(pixels)) {
                CopyBits((BitMap *)*pixels, &gWindow->portBits, &art, &destination, srcCopy, NULL);
                UnlockPixels(pixels);
            }
        }
        EraseRect(&gHistoryRect);
        snprintf(history, sizeof(history), "History: %lu/%lu KiB (%lu%%)",
            (unsigned long)((gAgent.used + 1023) / 1024),
            (unsigned long)(AGENT_HISTORY_CAP / 1024),
            (unsigned long)(gAgent.used * 100 / AGENT_HISTORY_CAP));
        DrawLabel(&gHistoryRect, history);
        gDisplayedHistory = gAgent.used;
        {
            char line[96], count[24];
            long dollars, micros;
            EraseRect(&gUsageRect);
            if (gAgent.context_seen) FormatTokens(gAgent.context_tokens, count, sizeof(count));
            else strcpy(count, "-");
            if (gAgent.context_seen && gModelInfoLimit > 0 && !strcmp(gModelInfoModel, gRunModel)) {
                long long percent = ((long long)gAgent.context_tokens * 100 + gModelInfoLimit / 2) / gModelInfoLimit;
                if (percent > 100) percent = 100;
                snprintf(line, sizeof(line), "Context: %s tokens (%ld%%)", count, (long)percent);
            } else if (gAgent.context_seen) {
                snprintf(line, sizeof(line), "Context: %s tokens", count);
            } else {
                snprintf(line, sizeof(line), "Context: -");
            }
            DrawLabel(&gUsageRect, line);
            if (gAgent.cost_micros > 0) {
                dollars = (long)(gAgent.cost_micros / 1000000LL);
                micros = (long)(gAgent.cost_micros % 1000000LL);
                snprintf(line, sizeof(line), "Cost: $%lu.%06lu", (unsigned long)dollars, (unsigned long)micros);
                PStr(p, line);
                MoveTo(gUsageRect.right - StringWidth(p), gUsageRect.bottom - 4);
                DrawString(p);
            }
            gDisplayedCost = gAgent.cost_micros;
            gDisplayedTokens = gAgent.context_seen ? gAgent.context_tokens : -1;
            gDisplayedLimit = gModelInfoLimit;
        }
    }
    DrawLabel(&gResponseLabelRect, "Conversation:");
    FrameRect(&gResponseRect);
    if (gResponseTE) TEUpdate(&(*gResponseTE)->viewRect, gResponseTE);

    DrawLabel(&gPromptLabelRect, "Message (Command-Return to send):");
    FrameRect(&gPromptRect);
    if (gPromptTE) TEUpdate(&(*gPromptTE)->viewRect, gPromptTE);
    DrawControls(gWindow);
}

static void UIInit(void)
{
    Rect             bounds;
    MenuBarHandle    mb;

    mb = GetNewMBar(128);
    if (mb) SetMenuBar(mb);
    gAppleMenu = GetMenu(kAppleMenuID);
    gFileMenu  = GetMenu(kFileMenuID);
    gEditMenu  = GetMenu(kEditMenuID);
    if (gAppleMenu) AppendResMenu(gAppleMenu, 'DRVR');   /* desk accessories */
    DrawMenuBar();

    SetRect(&bounds, kWinLeft, kWinTop,
            kWinLeft + kWinWidth, kWinTop + kWinHeight);
    gWindow = NewCWindow(NULL, &bounds, (ConstStr255Param)"\pSherclawk",
                         true, documentProc, (WindowPtr)-1L, true, 0);
    if (gWindow == NULL) return;
    SetPort(gWindow);
    TextFont(kFontIDGeneva);
    TextSize(10);

    ComputeLayout();
    {
        Handle resource = Get1Resource('sART', 128);
        Rect bounds = { 82, 10, 238, 166 };
        if (resource && GetHandleSize(resource) == 4 + 156L * 156L * 4 &&
            NewGWorld(&gArt, 32, &bounds, NULL, NULL, 0) == noErr) {
            PixMapHandle pixels = GetGWorldPixMap(gArt);
            if (LockPixels(pixels)) {
                unsigned char *base = (unsigned char *)GetPixBaseAddr(pixels);
                long row = GetPixRowBytes(pixels) & 0x3fff;
                int y;
                HLock(resource);
                for (y = 0; y < 156; y++) memcpy(base + y * row, *resource + 4 + y * 156L * 4, 156 * 4);
                HUnlock(resource); UnlockPixels(pixels);
            } else { DisposeGWorld(gArt); gArt = NULL; }
        }
        if (resource) ReleaseResource(resource);
    }

    gSendBtn = NewControl(gWindow, &gSendRect, (ConstStr255Param)"\pSend",
                           true, 0, 0, 1, pushButProc, 0);

    gStopBtn = NewControl(gWindow, &gStopRect, (ConstStr255Param)"\pStop",
                           true, 0, 0, 1, pushButProc, 0);
    gNewBtn = NewControl(gWindow, &gNewRect, (ConstStr255Param)"\pNew Chat",
                           true, 0, 0, 1, pushButProc, 0);
    gHandoffBtn = NewControl(gWindow, &gHandoffRect, (ConstStr255Param)"\pSave Handoff",
                           true, 0, 0, 1, pushButProc, 0);
    {
        Rect view = gPromptRect;
        InsetRect(&view, 3, 2);
        gPromptTE = TENew(&view, &view);
        if (gPromptTE) TEAutoView(true, gPromptTE);
    }
    /* Model field — single-line, horizontal autoscroll. */
    {
        Rect view = gModelRect;
        InsetRect(&view, 3, 2);
        Rect dest = view;
        dest.right += 4000;   /* room for long model IDs */
        gModelTE = TENew(&dest, &view);
        if (gModelTE) {
            const char *url = gPrefs.model;
            TEAutoView(true, gModelTE);
            TESetText(url, (long)strlen(url), gModelTE);
            TESetSelect(0, 32767, gModelTE);   /* selected: type to replace */
        }
    }

    /* Response pane — multiline TE plus a scrollbar. */
    {
        Rect view = gResponseViewRect;
        InsetRect(&view, 3, 2);
        Rect dest = view;
        gResponseTE = TENew(&dest, &view);
    }
    {
        Rect sb;
        sb.top    = gResponseRect.top;
        sb.bottom = gResponseRect.bottom;
        sb.right  = gResponseRect.right + 1;
        sb.left   = sb.right - 16;
        gResponseScroll = NewControl(gWindow, &sb, (ConstStr255Param)"\p",
                                     true, 0, 0, 0, scrollBarProc, 0);
    }
    gScrollActionUPP = NewControlActionUPP(ScrollActionProc);

    FocusSet(gPromptTE);
    SetSendEnabled(1);
}

static void UIDispose(void)
{
    if (gArt) { DisposeGWorld(gArt); gArt = NULL; }
    if (gPromptTE) TEDispose(gPromptTE);
    if (gStopBtn) DisposeControl(gStopBtn);
    if (gNewBtn) DisposeControl(gNewBtn);
    if (gHandoffBtn) { DisposeControl(gHandoffBtn); gHandoffBtn = NULL; }
    if (gResponseTE)     { TEDispose(gResponseTE);     gResponseTE = NULL; }
    if (gResponseScroll) { DisposeControl(gResponseScroll); gResponseScroll = NULL; }
    if (gScrollActionUPP){ DisposeControlActionUPP(gScrollActionUPP); gScrollActionUPP = NULL; }
    if (gModelTE)          { TEDispose(gModelTE);          gModelTE = NULL; }
    if (gSendBtn)       { DisposeControl(gSendBtn);  gSendBtn = NULL; }

    if (gEditMenu)  { DeleteMenu(kEditMenuID);  DisposeMenu(gEditMenu);  gEditMenu  = NULL; }
    if (gFileMenu)  { DeleteMenu(kFileMenuID);  DisposeMenu(gFileMenu);  gFileMenu  = NULL; }
    if (gAppleMenu) { DeleteMenu(kAppleMenuID); DisposeMenu(gAppleMenu); gAppleMenu = NULL; }

    if (gWindow) { DisposeWindow(gWindow); gWindow = NULL; }
}

/* ── Event handling ───────────────────────────────────────────────── */

static void HandleEvent(const EventRecord *event)
{
    switch (event->what) {
    case kHighLevelEvent: AEProcessAppleEvent(event); break;
    case updateEvt:
        if ((WindowPtr)event->message == gWindow) {
            SetPort(gWindow);
            BeginUpdate(gWindow);
            EraseRect(&gWindow->portRect);
            DrawChrome();
            EndUpdate(gWindow);
        }
        break;

    case mouseDown: {
        WindowPtr which;
        short     part = FindWindow(event->where, &which);

        if (part == inMenuBar) {
            long choice = MenuSelect(event->where);
            if (HandleMenu(choice)) gQuit = 1;
            HiliteMenu(0);
            break;
        }
        if (which != gWindow) break;

        if (part == inDrag) {
            Rect screen = (*GetGrayRgn())->rgnBBox;
            DragWindow(gWindow, event->where, &screen);
            break;
        }
        if (part == inGoAway) {
            if (TrackGoAway(gWindow, event->where)) gQuit = 1;
            break;
        }
        if (part != inContent) break;

        SetPort(gWindow);
        Point local = event->where;
        GlobalToLocal(&local);

        {
            ControlHandle ctl = NULL;
            short         cpart = FindControl(local, gWindow, &ctl);

            /* Send button (ignored while a request is in flight). */
            if (cpart && ctl == gSendBtn) {
                if (!gSending &&
                    TrackControl(ctl, local, NULL) == kControlButtonPart) {
                    SendChat();
                }
                break;
            }

            if (cpart && ctl == gStopBtn) {
                if (gSending && TrackControl(ctl, local, NULL) == kControlButtonPart)
                    AbortChat("Stopped. Completed tool results are retained.");
                break;
            }
            if (cpart && ctl == gNewBtn) {
                if (!gSending && TrackControl(ctl, local, NULL) == kControlButtonPart) NewChat();
                break;
            }
            if (cpart && ctl == gHandoffBtn) {
                if (gHandoffEnabled && TrackControl(ctl, local, NULL) == kControlButtonPart)
                    StartHandoff();
                break;
            }
            /* Response pane scrollbar. */
            if (cpart && ctl == gResponseScroll) {
                if (cpart == kControlIndicatorPart) {
                    TrackControl(ctl, local, NULL);
                    ResponseScrollTo(GetControlValue(ctl));
                } else {
                    TrackControl(ctl, local, gScrollActionUPP);
                }
                break;
            }
        }

        if (!gSending && gPromptTE && PtInRect(local, &(*gPromptTE)->viewRect)) {
            FocusSet(gPromptTE);
            TEClick(local, (event->modifiers & shiftKey) != 0, gPromptTE);
        } else if ((!gSending || gSending == 6) && gModelTE && PtInRect(local, &(*gModelTE)->viewRect)) {
            FocusSet(gModelTE);
            TEClick(local, (event->modifiers & shiftKey) != 0, gModelTE);
        } else if (gResponseTE && PtInRect(local, &(*gResponseTE)->viewRect)) {
            FocusSet(gResponseTE);
            TEClick(local, (event->modifiers & shiftKey) != 0, gResponseTE);
        }
        break;
    }

    case keyDown:
    case autoKey: {
        char c = (char)(event->message & charCodeMask);

        if ((event->modifiers & cmdKey) && c == '.') {
            if (gSending) AbortChat("Stopped. Completed tool results are retained.");
            break;
        }
        if ((event->modifiers & cmdKey) && (c == '\r' || c == 0x03)) {
            SendChat(); break;
        }
        if (event->modifiers & cmdKey) {
            long choice = MenuKey(c);
            if (HiWord(choice) != 0) {
                if (HandleMenu(choice)) gQuit = 1;
                HiliteMenu(0);
                break;
            }
            break; /* Never type command shortcuts into editable text. */
        }

        if (c == 0x09) {
            if (!gSending) FocusSet(gFocusedTE == gModelTE ? gPromptTE :
                gFocusedTE == gPromptTE ? gResponseTE : gModelTE);
            break;
        }
        if ((!gSending || (gSending == 6 && gFocusedTE == gModelTE)) &&
            gFocusedTE && gFocusedTE != gResponseTE) {
            long limit = gFocusedTE == gModelTE ? CHAT_MODEL_CAP - 1 : CHAT_PROMPT_CAP - 1;
            long remaining = (*gFocusedTE)->teLength -
                ((*gFocusedTE)->selEnd - (*gFocusedTE)->selStart);
            if (gFocusedTE == gModelTE && (c == '\r' || c == 0x03)) break;
            if (c == 8 || ((unsigned char)c >= 0x1c && (unsigned char)c <= 0x1f) || remaining < limit) {
                TEKey(c, gFocusedTE); TESelView(gFocusedTE);
            } else SetStatus("Field limit reached.");
        }
        break;
    }

    case activateEvt: {
        int active = (event->modifiers & activeFlag) != 0;
        if (gFocusedTE) {
            if (active) TEActivate(gFocusedTE);
            else        TEDeactivate(gFocusedTE);
        }
        break;
    }

    default:
        break;
    }

    if (gFocusedTE) TEIdle(gFocusedTE);
}

/* ── Chat flow ───────────────────────────────────────────────────── */
/* Session journals are UTF-8 JSON lines on the selected workspace. They are
 * private conversation records, separate from the credential-free app log. */
static void SessionClose(void)
{
    if (gSessionOpen) { FSClose(gSessionRef); FlushVol(NULL, 0); gSessionOpen = 0; }
}
/* Create a candidate journal without closing the current one. Handoff
 * failures must leave the original history and its recording sink intact. */
static int SessionCreate(char *out_path, size_t cap, short *out_ref)
{
    Str255 path;
    FSSpec spec;
    long dir;
    OSErr err;
    int attempt;
    char folder[256];
    snprintf(folder, sizeof(folder), "%sSherclawk Sessions:", tools_workspace());
    PStr(path, folder);
    err = FSMakeFSSpec(0, 0, path, &spec);
    if (err == fnfErr) err = FSpDirCreate(&spec, smSystemScript, &dir);
    if (err != noErr && err != dupFNErr) return -1;
    for (attempt = 0; attempt < 100; attempt++) {
        if (snprintf(out_path, cap, "%ss%08lx.jsonl", folder,
                 ((unsigned long)TickCount() + (unsigned long)attempt) & 0xffffffffUL) >= (int)cap) return -1;
        PStr(path, out_path);
        err = FSMakeFSSpec(0, 0, path, &spec);
        if (err == noErr) continue;
        if (err != fnfErr || FSpCreate(&spec, 'ShCk', 'TEXT', smSystemScript) != noErr) return -1;
        if (FSpOpenDF(&spec, fsWrPerm, out_ref)) return -1;
        return 0;
    }
    return -1;
}
static int SessionStart(void)
{
    short ref;
    char path[256];
    if (SessionCreate(path, sizeof(path), &ref)) return -1;
    SessionClose();
    gSessionRef = ref; gSessionOpen = 1; strcpy(gSessionPath, path);
    return 0;
}
static int SessionWrite(short ref, const char *text)
{
    long length = (long)strlen(text), written = length;
    return FSWrite(ref, &written, text) != noErr || written != length ? -1 : 0;
}
static int Journal(void *context, const char *event, const char *json)
{
    char prefix[100];
    short ref = context ? *(short *)context : gSessionRef;
    if (!context && !gSessionOpen) return -1;
    snprintf(prefix, sizeof(prefix), "{\"event\":\"%s\",\"message\":", event);
    if (SessionWrite(ref, prefix) || SessionWrite(ref, json) || SessionWrite(ref, "}\n") || FlushVol(NULL, 0) != noErr) return -1;
    return 0;
}
static void ShowMessage(const char *label, const char *text)
{
    static char display[CHAT_TRANSCRIPT_CAP];
    size_t at = strlen(gChat.transcript), len, i, lines = 0;
    size_t prefix = (label && *label) ? strlen(label) + 2 : 0; /* "label:\r" */
    if (text_to_macroman(text, display, sizeof(display)) < 0) strcpy(display, "[Text exceeds display capacity; see session file.]");
    len = strlen(display);
    for (i = 0; i < at; i++) if (gChat.transcript[i] == 13) lines++;
    {
        size_t own_lines = 0;
        for (i = 0; i < len; i++) if (display[i] == 13) own_lines++;
        if (own_lines > 1000 || len > sizeof(gChat.transcript) - 256) {
            strcpy(display, "[Text exceeds display limits; see the UTF-8 session file.]");
            len = strlen(display); own_lines = 0;
        }
        lines += own_lines;
    }
    if (at + len + prefix + 8 >= sizeof(gChat.transcript) || lines > 1400) {
        strcpy(gChat.transcript, "[Earlier conversation is saved in the session file.]\r\r");
        at = strlen(gChat.transcript);
    }
    if (prefix + len + 5 >= sizeof(gChat.transcript) - at) return;
    if (prefix) { strcpy(gChat.transcript + at, label); strcat(gChat.transcript, ":\r"); }
    strcat(gChat.transcript, display); strcat(gChat.transcript, "\r\r");
    ResponseSetText(gChat.transcript, strlen(gChat.transcript));
    ResponseScrollTo(GetControlMaximum(gResponseScroll));
}

/* ── Tool debug view (display only; the session file is unaffected) ─ */
/* Journal event names emitted for the tool call being executed. */
static char gToolEvents[192];
static void ToolEventsReset(void) { gToolEvents[0] = 0; }
static void ToolEventsAppend(const char *event)
{
    size_t at = strlen(gToolEvents), n = strlen(event);
    if (!at) {
        if (n < sizeof(gToolEvents)) memcpy(gToolEvents, event, n + 1);
        return;
    }
    if (at + n + 3 > sizeof(gToolEvents)) return; /* keep what already fit */
    gToolEvents[at] = ','; gToolEvents[at + 1] = ' ';
    memcpy(gToolEvents + at + 2, event, n + 1);
}
static int ToolEventJournal(void *context, const char *event, const char *json)
{
    ToolEventsAppend(event);
    return Journal(context, event, json);
}

static void AppendText(char *out, size_t cap, const char *s)
{
    size_t at = strlen(out), n = strlen(s);
    if (at + n >= cap) return;
    memcpy(out + at, s, n + 1);
}

/* "• name(arg: value, ...)" from the recorded arguments, the same JSON the
 * tool layer parses. Long strings and nesting are abbreviated, and the whole
 * header is display-bounded. */
static void RenderToolCall(const AgentCall *call, char *out, size_t cap)
{
    static JsonToken tokens[256];
    static char value[132];
    size_t i;
    int parsed, first = 1;

    snprintf(out, cap, "\xE2\x80\xA2 %s(", call->name);
    parsed = call->arguments[0] ? json_parse(call->arguments, strlen(call->arguments), tokens, 256) : -1;
    if (parsed < 1 || tokens[0].type != JSON_OBJECT) {
        size_t at = strlen(out), n = strlen(call->arguments);
        if (at < cap - 1 && n > cap - at - 2) n = cap - at - 2;
        if (at < cap - 1) { memcpy(out + at, call->arguments, n); out[at + n] = 0; }
    } else {
        for (i = 1; i < (size_t)tokens[0].next; i = (size_t)tokens[i + 1].next) {
            int v = (int)i + 1;
            char key[64];
            if (!first) AppendText(out, cap, ", ");
            first = 0;
            if (json_string(call->arguments, tokens, (int)i, key, sizeof(key)) < 0) strcpy(key, "?");
            AppendText(out, cap, key);
            AppendText(out, cap, ": ");
            switch (tokens[v].type) {
            case JSON_STRING:
                if (json_string(call->arguments, tokens, v, value, sizeof(value)) < 0) strcpy(value, "<long>");
                AppendText(out, cap, "\""); AppendText(out, cap, value); AppendText(out, cap, "\"");
                break;
            case JSON_PRIMITIVE: {
                int n = tokens[v].end - tokens[v].start;
                if (n > 40) n = 40;
                memcpy(value, call->arguments + tokens[v].start, (size_t)n); value[n] = 0;
                AppendText(out, cap, value);
                break;
            }
            default:
                AppendText(out, cap, tokens[v].type == JSON_ARRAY ? "[...]" : "{...}");
                break;
            }
            if (strlen(out) > cap - 12) { AppendText(out, cap, ", ..."); break; }
        }
    }
    AppendText(out, cap, ")");
}

/* One tool message: with the toggle off, the plain result as before; with it
 * on, the call header, the result indented and the call's journal events. */
static void ShowToolResult(const AgentCall *call, const char *label, const char *text)
{
    static char block[4096];
    char header[400];
    if (!gPrefs.show_tool_debug || !call) { ShowMessage(label, text); return; }
    RenderToolCall(call, header, sizeof(header));
    snprintf(block, sizeof(block), "%s\r  \xC2\xBB %s\r  journal: %s",
        header, text, gToolEvents[0] ? gToolEvents : "(none)");
    ShowMessage(NULL, block);
}
static void NewChat(void)
{
    if (gSending) return;
    SessionClose(); agent_reset(&gAgent, Journal, NULL); chat_reset(&gChat);
    ResponseSetText("", 0); TESetText("", 0, gPromptTE); InvalRect(&gPromptRect);
    FocusSet(gPromptTE); SetStatus("Ready. What shall we investigate?");
}
/* Native Markdown is MacRoman/CR/TEXT so read_text can read it in later
 * sessions. Never overwrite a handoff, and verify bytes after close/flush. */
static int SaveHandoff(void)
{
    static char utf8[10000], bytes[4097], verified[4097];
    char folder[256];
    Str255 path;
    FSSpec spec;
    short ref;
    long length, written, actual;
    int attempt, n;
    OSErr err, close_err;
    n = snprintf(utf8, sizeof(utf8), "# Sherclawk handoff\n\nOriginal journal: %s\n"
        "This is a model-generated summary. Verify current files and observed results.\n\n%s\n",
        gSessionPath, gHandoffSummary);
    if (n < 0 || (size_t)n >= sizeof(utf8) ||
        (n = text_to_macroman_strict(utf8, bytes, sizeof(bytes))) < 0) return -1;
    length = n;
    snprintf(folder, sizeof(folder), "%sSherclawk Sessions:", tools_workspace());
    for (attempt = 0; attempt < 100; attempt++) {
        char candidate[256];
        if (snprintf(candidate, sizeof(candidate), "%sh%08lx.md", folder,
            ((unsigned long)TickCount() + (unsigned long)attempt) & 0xffffffffUL) >= (int)sizeof(candidate)) return -1;
        PStr(path, candidate);
        err = FSMakeFSSpec(0, 0, path, &spec);
        if (err == noErr) continue;
        if (err != fnfErr || FSpCreate(&spec, 'ShCk', 'TEXT', smSystemScript)) return -1;
        strcpy(gHandoffPath, candidate); /* Retain/report even partial files. */
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
static int CommitHandoff(void)
{
    char path[256];
    short ref;
    if (SaveHandoff()) return -1;
    if (SessionCreate(path, sizeof(path), &ref)) return -1;
    agent_reset(&gHandoffCandidate, Journal, &ref);
    if (agent_handoff_seed(&gHandoffCandidate, gHandoffSummary, gSessionPath, gHandoffPath)) {
        FSClose(ref); return -1;
    }
    /* Cost is a session total, not history: the fresh candidate keeps it and
     * leaves the context line unset until the next reply. */
    gHandoffCandidate.cost_micros = gAgent.cost_micros;
    gHandoffCandidate.cost_seen = gAgent.cost_seen;
    /* The old agent is untouched until both durable files exist. */
    SessionClose();
    gSessionRef = ref; gSessionOpen = 1; strcpy(gSessionPath, path);
    gHandoffCandidate.journal_context = NULL;
    gAgent = gHandoffCandidate;
    return 0;
}
static void StartHandoff(void)
{
    int length;
    size_t i;
    char attribution[256];
    if (gSending) return;
    if (gLookupDrain) { SetStatus("The stopped lookup is still closing; try again in a moment."); return; }
    if (!gSessionOpen || !gAgent.messages || gAgent.active || gAgent.next < gAgent.count) {
        SetStatus("Finish or stop the current run before saving a handoff."); return;
    }
    if (!gPrefs.api_key[0] || TEGetTextInto(gModelTE, gRunModel, sizeof(gRunModel)) < 0 || !*gRunModel) {
        SetStatus("A model and API key are needed for a handoff."); return;
    }
    for (i = 0; gRunModel[i]; i++) if ((unsigned char)gRunModel[i] <= 32 || (unsigned char)gRunModel[i] >= 127) {
        SetStatus("Use an OpenRouter model ID without spaces."); return;
    }
    length = agent_handoff_request(&gAgent, gRunModel, gJSON, sizeof(gJSON));
    if (length < 0) { SetStatus("Could not prepare handoff; conversation retained."); return; }
    if (sherclawk_attribution(SHERCLAWK_APP_URL, attribution, sizeof(attribution)) < 0) {
        SetStatus("Attribution URL is too long; conversation retained."); return;
    }
    length = http_build_post_with_headers("openrouter.ai", "/api/v1/chat/completions", gPrefs.api_key,
        attribution, gJSON, (size_t)length, gNet.request, sizeof(gNet.request));
    if (length < 0) { SetStatus("Could not prepare handoff request; conversation retained."); return; }
    gHandoffPath[0] = 0; gHandoff = 1;
    timing_round_begin(&gRoundTiming, 0, (uint32_t)TickCount());
    InitOpenTransport(); gOTOpen = 1; gStartTicks = (uint32_t)TickCount(); gSending = 1;
    timing_mark(&gRoundTiming, TIMING_INIT, gStartTicks);
    SetSendEnabled(0);
    if (network_start(&gNet, gNet.request, (size_t)length) < 0) { AbortChat(gNet.error); return; }
    SetStatus("Summarizing for handoff; current conversation retained...");
}
/* Abort reasons otherwise survive only in the on-screen transcript; keep a
 * stable marker plus the actual reason in the share lifecycle log. */
static void LogAbort(const char *prefix, const char *reason)
{
    char line[320];
    snprintf(line, sizeof(line), "%s: %s", prefix, reason);
    LogLine(line);
}
static void AbortChat(const char *reason)
{
    if (gHandoff) {
        if (gNet.ctx || gOTOpen) CloseChatContext();
        LogRoundTiming("abort");
        gHandoff = 0; gSending = 0; SetSendEnabled(1); FocusSet(gPromptTE);
        ShowMessage("Handoff stopped; conversation retained", reason);
        if (*gHandoffPath) ShowMessage("Retained handoff file (may be incomplete)", gHandoffPath);
        SetStatus("Handoff stopped; conversation retained. %s", reason);
        LogAbort("Handoff stopped; conversation retained. Reason", reason);
        return;
    }
    if (gSending == 6) {
        /* Do not tear the TLS context down mid-connect: classic OT can fault
         * when an async connect is outstanding. Keep the exchange draining and
         * close it from the loop once network_step reaches a terminal state. */
        gLookupDrain = 1; gStartTicks = (uint32_t)TickCount();
        gSending = 0; SetSendEnabled(1); FocusSet(gPromptTE);
        SetStatus("Model context lookup stopped. Send again shortly.");
        LogAbort("Model context lookup stopped. Reason", reason);
        return;
    }
    if (gSending == 4 || gSending == 5) {
        char error[256];
        const AgentCall *call = gAgent.next < gAgent.count ? &gAgent.calls[gAgent.next] : NULL;
        if(gSending==5) run_application_step(gToolResult, sizeof(gToolResult), (uint32_t)TickCount(), 1);
        else build_project_step(gToolResult, sizeof(gToolResult), (uint32_t)TickCount(), 1);
        agent_tool_result(&gAgent, gToolResult, error, sizeof(error));
        ShowToolResult(call, gSending==5 ? "run_application" : "build_project", gToolResult);
    }
    if (gNet.ctx || gOTOpen) CloseChatContext();
    LogRoundTiming("abort");
    if (agent_stop(&gAgent, reason)) {
        SetStatus("Session recording failed. Start a new session before continuing.");
        SessionClose();
    } else SetStatus("%s", reason);
    gSending = 0; SetSendEnabled(1); FocusSet(gPromptTE);
    ShowMessage("Stopped", reason);
    LogAbort("Agent stopped; completed records retained. Reason", reason);
}
static void PauseRunAtLimit(void)
{
    char reason[256];
    snprintf(reason, sizeof(reason),
        "Run paused after %d model rounds and %d tools (configured limits: %d rounds, %d tools). "
        "History is %lu%% full. Send Continue to resume.",
        gAgent.rounds, gAgent.tool_count, gPrefs.max_rounds, gPrefs.max_tools,
        (unsigned long)(gAgent.used * 100 / AGENT_HISTORY_CAP));
    AbortChat(reason);
}
/* agent_response rejects a bad body before recording it, so the history and
 * session journal would keep no trace of the failed request. Journal its HTTP
 * status, received body size and reason instead. */
static void JournalModelError(const char *reason)
{
    static char quoted[1600], detail[1700];
    if (!gSessionOpen || json_quote(reason, quoted, sizeof(quoted)) < 0) return;
    snprintf(detail, sizeof(detail), "{\"http_status\":%d,\"received_bytes\":%lu,\"error\":%s}",
        gNet.status, (unsigned long)gNet.body_len, quoted);
    Journal(NULL, "model_error", detail);
}
static int StartModelRequest(void)
{
    int length;
    char attribution[256];
    if (gAgent.rounds >= gPrefs.max_rounds || gAgent.tool_count >= gPrefs.max_tools) {
        PauseRunAtLimit(); return -1;
    }
    length = agent_request(&gAgent, gRunModel, gJSON, sizeof(gJSON));
    if (length < 0) { AbortChat("Request limit reached. Start a new session."); return -1; }
    if (sherclawk_attribution(SHERCLAWK_APP_URL, attribution, sizeof(attribution)) < 0) {
        AbortChat("Attribution URL is too long."); return -1;
    }
    length = http_build_post_with_headers("openrouter.ai", "/api/v1/chat/completions", gPrefs.api_key,
        attribution, gJSON, (size_t)length, gNet.request, sizeof(gNet.request));
    if (length < 0) { AbortChat("Request or API key is too long or invalid."); return -1; }
    timing_round_begin(&gRoundTiming, gAgent.rounds + 1, (uint32_t)TickCount());
    InitOpenTransport(); gOTOpen = 1;
    gStartTicks = (uint32_t)TickCount(); gSending = 1;
    timing_mark(&gRoundTiming, TIMING_INIT, gStartTicks);
    if (network_start(&gNet, gNet.request, (size_t)length) < 0) { AbortChat(gNet.error); return -1; }
    SetStatus("Connecting to OpenRouter (round %d)...", gAgent.rounds + 1);
    LogLine("Agent model request started."); return 0;
}
/* The session and agent state are touched only after the optional
 * context-window lookup, so Stop during it keeps the typed prompt and leaves
 * no session record. */
static void SendBegin(void)
{
    char error[256];
    gSending = 0;
    if (!gSessionOpen && (gAgent.messages || SessionStart())) {
        SetStatus("Session file unavailable. Check %s or start a new session.", tools_workspace()); return;
    }
    if (agent_begin(&gAgent, gPending, error, sizeof(error))) { SetStatus("%s", error); return; }
    ShowMessage("You", gPending);
    TESetText("", 0, gPromptTE); InvalRect(&gPromptRect);
    SetSendEnabled(0); StartModelRequest();
}
static int ModelLookupAllowed(const char *model)
{
    size_t i;
    if (!*model) return 0;
    for (i = 0; model[i]; i++) {
        unsigned char c = (unsigned char)model[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' ||
              c == '/' || c == '-')) return 0;
    }
    return 1;
}
static void StartContextLookup(void)
{
    char path[CHAT_MODEL_CAP + 32];
    int length;
    if (snprintf(path, sizeof(path), "/api/v1/models/%s/endpoints", gRunModel) >= (int)sizeof(path)) { SendBegin(); return; }
    length = http_build_get("openrouter.ai", path, gNet.request, sizeof(gNet.request));
    if (length < 0) { SendBegin(); return; }
    /* Remember the attempt before the network: at most one lookup per model
     * per launch, whatever the outcome. */
    strcpy(gModelInfoModel, gRunModel); gModelInfoLimit = -1; gModelInfoAttempted = 1;
    InitOpenTransport(); gOTOpen = 1;
    gStartTicks = (uint32_t)TickCount();
    if (network_start(&gNet, gNet.request, (size_t)length) < 0) {
        CloseChatContext(); SendBegin(); return;
    }
    gSending = 6;
    SetSendEnabled(0);
    SetStatus("Checking context window for %s...", gRunModel);
}
static void SendAdvance(void)
{
    if (!(gModelInfoAttempted && !strcmp(gModelInfoModel, gRunModel)) && ModelLookupAllowed(gRunModel)) {
        StartContextLookup(); return;
    }
    SendBegin();
}
static void FinishContextLookup(int completed)
{
    long limit = -1;
    if (completed && gNet.status == 200) limit = agent_context_limit(gNet.body, gNet.body_len);
    if (limit > 0) gModelInfoLimit = limit;
    if (gNet.ctx || gOTOpen) CloseChatContext();
    {
        char current[CHAT_MODEL_CAP];
        if (TEGetTextInto(gModelTE, current, sizeof(current)) < 0 || strcmp(current, gRunModel)) {
            gSending = 0; SetSendEnabled(1); FocusSet(gPromptTE);
            SetStatus("Model changed during the context lookup. Send again to continue.");
            return;
        }
    }
    gSending = 0;
    SendAdvance();
}
static void SendChat(void)
{
    char prompt[CHAT_PROMPT_CAP];
    size_t i;
    if (gSending) return;
    if (gLookupDrain) { SetStatus("The stopped lookup is still closing; send again in a moment."); return; }
    if (!gPrefs.api_key[0]) { SetStatus("No API key: choose Preferences from the Edit menu."); return; }
    if (TEGetTextInto(gModelTE, gRunModel, sizeof(gRunModel)) < 0 ||
        TEGetTextInto(gPromptTE, prompt, sizeof(prompt)) < 0) { SetStatus("Model or message is too long."); return; }
    for (i = 0; gRunModel[i]; i++) if ((unsigned char)gRunModel[i] <= 32 || (unsigned char)gRunModel[i] >= 127) {
        SetStatus("Use an OpenRouter model ID without spaces."); return;
    }
    for (i = 0; prompt[i] && (prompt[i] == ' ' || prompt[i] == '\r' || prompt[i] == '\t'); i++) {}
    if (!*gRunModel || !prompt[i]) { SetStatus("Enter a model and message first."); return; }
    if (text_to_utf8(prompt, strlen(prompt), gPending, sizeof(gPending)) < 0) { SetStatus("Could not convert message to UTF-8."); return; }
    SendAdvance();
}
static void DriveChatStep(void)
{
    char error[256];
    int result;
    if (!gSending) return;
    if (gSending == 6) {
        int lookup;
        if ((uint32_t)TickCount() - gStartTicks > 30UL * 60UL) { FinishContextLookup(0); return; }
        lookup = network_step(&gNet);
        if (lookup < 0) { FinishContextLookup(0); return; }
        if (lookup == 0) {
            MacTLS_State state = MacTLS_GetState(gNet.ctx);
            if (state == kMacTLS_Handshaking) SetStatus("TLS handshake...");
            else if (state == kMacTLS_Connected) SetStatus("Checking context window for %s...", gRunModel);
            return;
        }
        FinishContextLookup(1);
        return;
    }
    if (gSending == 3) { StartModelRequest(); return; }
    if (gSending == 4 || gSending == 5) {
        int running=gSending==5;
        const AgentCall *call = gAgent.next < gAgent.count ? &gAgent.calls[gAgent.next] : NULL;
        result = running ? run_application_step(gToolResult, sizeof(gToolResult), (uint32_t)TickCount(), 0) : build_project_step(gToolResult, sizeof(gToolResult), (uint32_t)TickCount(), 0);
        if (result == 2) return;
        gSending = 2;
        LogToolTiming(gAgent.next + 1, gAgent.calls[gAgent.next].name);
        if (agent_tool_result(&gAgent, gToolResult, error, sizeof(error))) { AbortChat(error); return; }
        ShowToolResult(call, running ? "run_application" : "build_project", gToolResult);
        if (result) AbortChat(running ? "Launch outcome uncertain. Inspect the run journal; do not retry automatically." : "Build observation stopped. Inspect retained snapshot and logs before another build.");
        return;
    }
    if (gSending == 2) {
        AgentCall *call;
        if (gAgent.next == gAgent.count) { gSending = 3; return; }
        if (gAgent.tool_count >= gPrefs.max_tools) { PauseRunAtLimit(); return; }
        call = &gAgent.calls[gAgent.next];
        gToolStart = (uint32_t)TickCount();
        SetStatus("Running %s (%d/%d)...", call->name, gAgent.next + 1, gAgent.count);
        ToolEventsReset();
        {
            char id[800], name[400], started[1300];
            if (json_quote(call->id, id, sizeof(id)) < 0 || json_quote(call->name, name, sizeof(name)) < 0) {
                AbortChat("Could not encode tool start."); return;
            }
            snprintf(started, sizeof(started), "{\"call_id\":%s,\"name\":%s}", id, name);
            if (Journal(NULL, "tool_started", started)) { AbortChat("Could not record tool start; no tool executed."); return; }
            ToolEventsAppend("tool_started");
        }
        if (!strcmp(call->name, "build_project")) {
            result = build_project_begin(call, gToolResult, sizeof(gToolResult), ToolEventJournal, NULL, (uint32_t)TickCount());
            if (result == 2) { gSending = 4; SetStatus("Building snapshot; waiting for MacRelix worker..."); return; }
        } else if (!strcmp(call->name, "run_application")) {
            result = run_application_begin(call, gToolResult, sizeof(gToolResult), ToolEventJournal, NULL, (uint32_t)TickCount());
            if (result == 2) { gSending = 5; SetStatus("Verifying built application before launch..."); return; }
        } else if (!strcmp(call->name, "read_build_log")) {
            build_project_log(call, gToolResult, sizeof(gToolResult)); result = 0;
        } else result = tools_execute_recorded(call, gToolResult, sizeof(gToolResult), ToolEventJournal, NULL);
        LogToolTiming(gAgent.next + 1, call->name);
        if (agent_tool_result(&gAgent, gToolResult, error, sizeof(error))) { AbortChat(error); return; }
        ShowToolResult(call, call->name, gToolResult);
        if (result) AbortChat("Mutation stopped. Inspect the result and session recovery records; do not retry automatically.");
        return;
    }
    if ((uint32_t)TickCount() - gStartTicks > 120UL * 60UL) { AbortChat("Request timed out. Completed results retained."); return; }
    result = network_step(&gNet);
    ObserveRound();
    if (result < 0) { AbortChat(gNet.error); return; }
    if (!result) {
        MacTLS_State state = MacTLS_GetState(gNet.ctx);
        if (state == kMacTLS_Handshaking) SetStatus("TLS handshake...");
        else if (state == kMacTLS_Connected) SetStatus("Waiting for model (%s, %lu bytes)...", TLSVersionLabel(gNet.version), (unsigned long)gNet.received);
        return;
    }
    if (gHandoff) {
        if (agent_handoff_response(gNet.body, gNet.body_len, gNet.status, gHandoffSummary,
            sizeof(gHandoffSummary), error, sizeof(error))) { JournalModelError(error); AbortChat(error); return; }
        CloseChatContext();
        /* The summary completion is provider-billed like any other round. */
        agent_usage_absorb(&gAgent, gNet.body, gNet.body_len);
        LogRoundTiming("ok");
        if (CommitHandoff()) { AbortChat("Could not save/verify handoff and new journal. History was not cleared."); return; }
        gHandoff = 0; gSending = 0; SetSendEnabled(1); FocusSet(gPromptTE);
        ShowMessage("Saved handoff", gHandoffPath);
        ShowMessage("Handoff summary", gHandoffSummary);
        SetStatus("Handoff saved; fresh history is ready. Send a message to continue.");
        return;
    }
    if (agent_response(&gAgent, gNet.body, gNet.body_len, gNet.status, error, sizeof(error))) { JournalModelError(error); AbortChat(error); return; }
    CloseChatContext();
    LogRoundTiming("ok");
    if (*gAgent.text) ShowMessage("Sherclawk", gAgent.text);
    if (gAgent.count) {
        gSending = 2; SetStatus("Model requested %d tool(s).", gAgent.count);
    } else {
        gSending = 0; SetSendEnabled(1); FocusSet(gPromptTE);
        if (gAgent.limited) ShowMessage("Notice", "Reply incomplete: output token limit reached.");
        if (gAgent.used >= AGENT_HISTORY_CAP * 3 / 4)
            SetStatus("History nearly full. Save Handoff (Command-H) to free history.");
        else SetStatus("Done - %d model rounds, %d tools. Session saved.", gAgent.rounds, gAgent.tool_count);
        LogLine("Agent run completed.");
    }
}
/* A stopped context lookup keeps its exchange alive until network_step reaches
 * a terminal state; closing it here avoids tearing OT down mid-connect. */
static void DrainAbandonedLookup(void)
{
    int result;
    if (!gLookupDrain) return;
    if ((uint32_t)TickCount() - gStartTicks > 30UL * 60UL) {
        CloseChatContext(); gLookupDrain = 0; return;
    }
    result = network_step(&gNet);
    if (result != 0) { CloseChatContext(); gLookupDrain = 0; }
}

int main(void)
{
    EventRecord event;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL); InitCursor(); MacTLS_Init();
    PrefsLoad();
    tools_set_workspace(gPrefs.workspace);
    UIInit();
    if (!gWindow || !gModelTE || !gPromptTE || !gResponseTE ||
        !gSendBtn || !gStopBtn || !gNewBtn || !gHandoffBtn || !gResponseScroll || !gScrollActionUPP) {
        UIDispose(); MacTLS_Shutdown(); return 1;
    }
    chat_reset(&gChat); agent_reset(&gAgent, Journal, NULL);
    SetStatus(gPrefsUnreadable ? "Preferences unreadable; using compiled defaults."
                               : "Ready. What shall we investigate?");
    LogOpen(); LogLine("Sherclawk session started.");
    if(selfbuild_init())SetStatus("Native executor unavailable; builds require the external worker.");
    while (!gQuit) {
        WaitNextEvent(everyEvent, &event, gSending ? 1 : 10, NULL);
        SetPort(gWindow); HandleEvent(&event);
        if (gSending) DriveChatStep();
        DrainAbandonedLookup();
        selfbuild_drain((uint32_t)TickCount());
        UpdateHandoffControls();
        /* History changes during sends, tool results, New Chat and handoff.
         * Keep its indicator current even when the ordinary status is unchanged. */
        if (gDisplayedHistory != gAgent.used) InvalRect(&gHistoryRect);
        if (gDisplayedCost != gAgent.cost_micros ||
            gDisplayedTokens != (gAgent.context_seen ? gAgent.context_tokens : -1) ||
            gDisplayedLimit != gModelInfoLimit) InvalRect(&gUsageRect);
    }
    if (gNet.ctx || gOTOpen) CloseChatContext();
    LogRoundTiming("abort");
    if (gAgent.active) AbortChat("Application quit.");
    selfbuild_close();
    SessionClose();
    LogLine("Sherclawk session ended."); LogClose(); UIDispose();
    MacTLS_Shutdown(); return 0;
}
