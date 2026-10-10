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
#include <Appearance.h>
#include <ToolUtils.h>
#include <TextUtils.h>
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
#include "session.h"
#include "json.h"
#include "tools.h"
#include "build_project.h"
#include "selfbuild.h"
#include <AppleEvents.h>
#include "run_application.h"
#include "view_image.h"
#include <QDOffscreen.h>
#include <Resources.h>
#include "text.h"
#include "config.h"
#include "preferences.h"
#include "display.h"
#include "mcp_editor.h"
#include "mcp_store.h"

/* ── Menu IDs ─────────────────────────────────────────────────────── */
enum {
    kAppleMenuID = 128,
    kFileMenuID  = 129,
    kEditMenuID  = 130,
    kEditPrefsItem = 10,  /* Edit menu: "Preferences…" */
    kEditMcpItem   = 11   /* Edit menu: last item ("MCP Servers…") */
};

/* ── Layout ───────────────────────────────────────────────────────── */
enum {
    kWinLeft   = 40,
    kWinTop    = 40,
    kWinWidth  = 600,
    kWinHeight = 436,

    kPad       = 10,
    kFieldH    = 20,
    kButtonW   = 64
};

/* ── UI state ─────────────────────────────────────────────────────── */
static WindowPtr     gWindow = NULL;
static ControlHandle gSendBtn = NULL, gStopBtn = NULL, gNewBtn = NULL, gHandoffBtn = NULL;
static int gHandoffEnabled = -1;
static TEHandle gPromptTE = NULL;
static Rect gPromptRect, gPromptLabelRect, gStopRect, gNewRect, gHandoffRect;
static TEHandle      gResponseTE = NULL;
static ControlHandle gResponseScroll = NULL;
static ControlActionUPP gScrollActionUPP = NULL;
static TEHandle      gFocusedTE = NULL;

static Rect gModelRect, gSendRect;
static Rect gInfoRect, gStatusRect, gHistoryRect, gMeterRect;
static size_t gDisplayedHistory = (size_t)-1;
static Rect gUsageRect, gContextRect, gCostRect, gPromptHintRect;
static char gModelInfoModel[CHAT_MODEL_CAP];
static AgentModelInfo gModelInfo;   /* Row behind gModelInfoModel; cleared per lookup. */
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
#define kPrefsFileName "\p" PREFS_FILENAME
#define kPrefsCreator 'ShCk'
#define kPrefsFileType 'pref'

/* ── Request state ──────────────────────────────────────────────── */
static Chat gChat; /* Only the bounded native display is reused. */
static Agent gAgent;
static char gRunModel[CHAT_MODEL_CAP];
static GWorldPtr gArt = NULL;
static Session gSession;
static ChatNetwork gNet;
static char gPending[CHAT_PROMPT_CAP * 3];
static char gJSON[CHAT_REQUEST_CAP];
static char gToolResult[AGENT_RESULT_CAP];
/* What the run loop is waiting on. Everything but RUN_IDLE counts as busy. */
typedef enum {
    RUN_IDLE,
    RUN_MODEL_REQUEST,   /* model HTTPS exchange in flight */
    RUN_TOOLS,           /* executing the model's tool calls in order */
    RUN_NEXT_REQUEST,    /* tools done; the next model request starts */
    RUN_READ_TEXT,
    RUN_EDIT_TEXT,
    RUN_BUILD,           /* build_project pending */
    RUN_QUIT,           /* quit_application pending */
    RUN_LAUNCH,          /* run_application pending */
    RUN_VIEW_IMAGE,      /* view_image reading a PNG */
    RUN_HANDOFF,         /* handoff summary HTTPS exchange in flight */
    RUN_CONTEXT_LOOKUP   /* model context-window lookup in flight */
} RunState;
static RunState gRun = RUN_IDLE;
static int RunBusy(void) { return gRun != RUN_IDLE; }
static int gOTOpen = 0;
static int gExchangeDrain = 0;
static char gHandoffSummary[8192], gHandoffPath[256];
static uint32_t gStartTicks;
static uint32_t gDrainStartTicks;  /* separate from the run timer, which aborts leave alone */
static RoundTiming gRoundTiming;
static uint32_t gToolStart;
static void FocusSet(TEHandle te);
static void DrawChrome(void);
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

/* Keep long model IDs and messages inside their column. Full status text is
 * available by clicking its row; the model remains editable in Preferences. */
static void DrawFittedLabel(const Rect *r, const char *text, short where)
{
    Str255 p;
    PStr(p, text);
    TruncString(r->right - r->left, p, where);
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
    int enabled = !RunBusy() && !gExchangeDrain && gSession.open && gAgent.messages &&
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
    Boolean isDefault = enabled != 0;
    if (gSendBtn) HiliteControl(gSendBtn, enabled ? 0 : 255);
    if (gSendBtn) SetControlData(gSendBtn, kControlEntireControl,
        kControlPushButtonDefaultTag, sizeof(isDefault), &isDefault);
    if (gNewBtn) HiliteControl(gNewBtn, enabled ? 0 : 255);
    if (gFileMenu) {
        if (enabled) EnableItem(gFileMenu, 1);
        else DisableItem(gFileMenu, 1);
    }
    if (gEditMenu) {
        if (enabled) { EnableItem(gEditMenu, kEditPrefsItem); EnableItem(gEditMenu, kEditMcpItem); }
        else { DisableItem(gEditMenu, kEditPrefsItem); DisableItem(gEditMenu, kEditMcpItem); }
    }
    UpdateHandoffControls();
    if (gStopBtn) HiliteControl(gStopBtn, enabled ? 255 : 0);
    if (gWindow) InvalRect(&gStatusRect);
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
 *
 * The yield on each side of CloseOpenTransport is SHERCLAWK_OT_YIELD_TICKS
 * (config.h). It was 60, which made teardown about 122 ticks of a ~246-tick
 * round; at 10 it is about 22 ticks. Guest soak (issue #25, 73 rounds): Stop
 * mid-response, connection resets, a refused connect and a dropped-packet
 * stall all aborted cleanly and the next request succeeded each time, with no
 * wedge. Faults did not land mid-download on a large response.
 */
static void CloseChatContext(void)
{
    network_close(&gNet);
    YieldTicks(SHERCLAWK_OT_YIELD_TICKS);
    if (gOTOpen) { CloseOpenTransport(); gOTOpen = 0; }
    YieldTicks(SHERCLAWK_OT_YIELD_TICKS);
}

/*
 * Abort teardown. A live exchange is never torn down mid-connect: classic OT
 * can fault when an async connect is outstanding (type-3 failures on Stop a
 * fraction of a second after Send). While the connect or handshake is still
 * pending, mark the exchange for the loop's bounded drain, which only lets it
 * settle: it never writes the request or reads a response, so Stop does not
 * finish sending the prompt. An exchange that is already connected or
 * terminal has nothing outstanding and closes at once. One path for the
 * lookup, send and handoff aborts.
 */
static int ExchangeConnecting(void)
{
    MacTLS_State state;
    if (!gNet.ctx || gNet.result) return 0;
    state = MacTLS_GetState(gNet.ctx);
    return state == kMacTLS_Idle || state == kMacTLS_Connecting ||
           state == kMacTLS_Handshaking;
}

static void BeginExchangeDrain(void)
{
    if (gExchangeDrain) return;
    gExchangeDrain = 1;
    gDrainStartTicks = (uint32_t)TickCount();
}

static void AbandonChatContext(void)
{
    if (ExchangeConnecting()) { BeginExchangeDrain(); return; }
    if (gNet.ctx || gOTOpen) CloseChatContext();
}

/* Teardown after a cleanly completed model round. Only this path may leave OT
 * open for the next round (SHERCLAWK_OT_KEEP_OPEN_AFTER_CLEAN); aborts and
 * quit abandon a live exchange to the drain above. */
static void FinishChatContext(void)
{
#if SHERCLAWK_OT_KEEP_OPEN_AFTER_CLEAN
    network_close(&gNet);
    YieldTicks(SHERCLAWK_OT_YIELD_TICKS);
#else
    CloseChatContext();
#endif
}

static void EnsureOpenTransport(void)
{
    if (!gOTOpen) { InitOpenTransport(); gOTOpen = 1; }
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
/* Completion tokens against the request cap, from the provider's own usage,
 * for every completed exchange (including ones agent_response will reject). */
static void ObserveCompletionTokens(void)
{
    long completion, reasoning;
    if (!gRoundTiming.active || agent_usage_completion(gNet.body, gNet.body_len, &completion, &reasoning)) return;
    timing_set_tokens(&gRoundTiming, completion, reasoning, gRun == RUN_HANDOFF ? AGENT_HANDOFF_MAX_TOKENS : AGENT_MAX_TOKENS);
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

/* ── Preferences dialog (DLOG/DITL 128) ───────────────────────────── */

/* DITL items: OK/Cancel, the editable model row with Find, the results list
 * (a user item drawn by hand), the hint line, the Effort popup rectangle,
 * then the label/edit pairs and the debug checkbox. The
 * numbers match the DITL in hello.r. */
enum {
    kPrefsDialogID      = 128,
    kPrefsAlertID       = 128,
    kPrefsOKItem        = 1,
    kPrefsCancelItem    = 2,
    kPrefsModelItem     = 4,
    kPrefsFindItem      = 5,
    kPrefsResultsItem   = 6,
    kPrefsHintItem      = 7,
    kPrefsEffortItem    = 9,
    kPrefsKeyItem       = 11,
    kPrefsWorkspaceItem = 13,
    kPrefsRoundsItem    = 15,
    kPrefsToolsItem     = 17,
    kPrefsDebugItem     = 18
};

/* The effort menu is built at runtime. The popup is not a control: the DITL's
 * user item is drawn by hand and PopUpMenuSelect tracks the menu, because both
 * NewControl overloadings of the Appearance popup drew nothing or an empty
 * popup on the guest. */
enum {
    kPrefsEffortMenuID = 1001,
    kPrefsFetchNone = 0,   /* no exchange in flight */
    kPrefsFetchPopular,    /* the one auto-load per launch */
    kPrefsFetchSearch,     /* Find: list the matches for the typed text */
    kPrefsFetchValidate,   /* OK: the typed id must resolve exactly */
    kPrefsRowHeight = 15,
    kPrefsRowsMax = AGENT_MODEL_ROWS_MAX
};

/* The catalog fetch shares the send-time lookup's 30-second deadline. */
#define kPrefsFetchDeadline (30UL * 60UL)

/* State carried between events of the modal Preferences loop. `rows` is the
 * last fetched page, `filter` the local substring the user typed, and
 * `confirmed_id` the field text that has resolved to a catalog row. */
typedef struct {
    DialogPtr     dlg;
    Rect          popup;             /* Effort popup user item rectangle */
    MenuHandle    effort;            /* runtime menu behind the popup; NULL on failure */
    int           effort_on;         /* the menu holds real choices */
    int           effort_choice;     /* 1-based chosen item */
    int           effort_shown;      /* popup lists confirmed_info's efforts */
    Rect          results;           /* results userItem rectangle */
    AgentModelRow rows[AGENT_MODEL_ROWS_MAX];
    int           count;
    int           selected;          /* index into rows, -1 when none */
    int           visible[AGENT_MODEL_ROWS_MAX];
    int           visible_count;
    int           edited;            /* the model field changed by hand */
    char          filter[CHAT_MODEL_CAP];
    char          last_text[CHAT_MODEL_CAP];
    char          confirmed_id[CHAT_MODEL_CAP];
    AgentModelInfo confirmed_info;   /* metadata behind confirmed_id */
    int           fetch;             /* kPrefsFetch* */
    int           draining;          /* timed-out exchange still closing */
    uint32_t      fetch_start;
} PrefsDialog;

static PrefsDialog   gPrefsDlg;
static AgentModelRow gPopularRows[AGENT_MODEL_ROWS_MAX];
static int           gPopularCount;
static int           gPopularLoaded;   /* popular page fetched once per launch */

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

/* Redraw the main window when its update event arrives while the dialog is up. */
static void UpdateMainWindow(void)
{
    if (!gWindow) return;
    SetPort(gWindow);
    BeginUpdate(gWindow);
    EraseRect(&gWindow->portRect);
    DrawChrome();
    EndUpdate(gWindow);
}

/* Flash a dialog button for the few ticks a key press takes to "click" it. */
static void PrefsFlashButton(DialogPtr dlg, short item)
{
    ControlHandle button = (ControlHandle)PrefsItem(dlg, item);
    uint32_t start = (uint32_t)TickCount();
    HiliteControl(button, kControlButtonPart);
    while ((uint32_t)TickCount() - start < 8) { }
    HiliteControl(button, 0);
}

/* DialogSelect does not apply the default and cancel items the way
 * ModalDialog's filter did: Return and Enter press OK, Escape and Command-.
 * press Cancel. Returns the item pressed, or 0 for any other event. */
static short PrefsKeyItem(DialogPtr dlg, const EventRecord *event)
{
    char c;
    short item = 0;
    if (event->what != keyDown && event->what != autoKey) return 0;
    c = (char)(event->message & charCodeMask);
    if (!(event->modifiers & cmdKey) && (c == '\r' || c == 3)) item = kPrefsOKItem;
    else if (c == 27 || ((event->modifiers & cmdKey) && c == '.')) item = kPrefsCancelItem;
    if (item && event->what == keyDown) PrefsFlashButton(dlg, item);
    return item;
}

/* Update the hint line in place; the invalidation is what redraws it. */
static void PrefsHint(PrefsDialog *d, const char *text)
{
    short  type;
    Handle handle;
    Rect   rect;
    SetPrefsText(d->dlg, kPrefsHintItem, text);
    GetDialogItem(d->dlg, kPrefsHintItem, &type, &handle, &rect);
    InvalRect(&rect);
}

/* Draw the popup by hand: a rounded box with its shadow, the chosen effort and
 * a down arrow; the disabled form (no choices) is gray. */
static void PrefsEffortDraw(PrefsDialog *d)
{
    Rect box = d->popup, text;
    RGBColor fill = { 0xE000, 0xE000, 0xE000 }, gray = { 0x8000, 0x8000, 0x8000 };
    Str255 label;
    int i;
    box.right -= 1; box.bottom -= 1;
    RGBBackColor(&fill);
    EraseRoundRect(&box, 8, 8);
    ForeColor(blackColor);
    BackColor(whiteColor);
    FrameRoundRect(&box, 8, 8);
    MoveTo(box.left + 4, box.bottom); LineTo(box.right - 4, box.bottom);
    MoveTo(box.right, box.top + 4); LineTo(box.right, box.bottom - 4);
    label[0] = 0;
    if (d->effort && d->effort_choice >= 1 && d->effort_choice <= CountMItems(d->effort))
        GetMenuItemText(d->effort, (short)d->effort_choice, label);
    text.left = box.left + 8; text.right = box.right - 22;
    text.top = box.top; text.bottom = box.bottom;
    if (!d->effort_on) RGBForeColor(&gray);
    TruncString(text.right - text.left, label, truncEnd);
    MoveTo(text.left, box.bottom - 5);
    DrawString(label);
    for (i = 0; i < 4; i++) {
        MoveTo(box.right - 16 + i, box.top + 7 + i);
        Line(7 - 2 * i, 0);
    }
    ForeColor(blackColor);
}

/* Rebuild the effort menu from a confirmed row, preselecting its default
 * effort; a row without choices leaves the popup disabled showing None. */
static void PrefsEffortShow(PrefsDialog *d, const AgentModelInfo *info)
{
    int i, choose = 1;
    d->effort_shown = info != NULL;
    d->effort_on = 0;
    d->effort_choice = 1;
    if (!d->effort) return;
    while (CountMItems(d->effort) > 0) DeleteMenuItem(d->effort, CountMItems(d->effort));
    if (!info || info->effort_count == 0) {
        AppendMenu(d->effort, (ConstStr255Param)"\pNone");
    } else {
        for (i = 0; i < info->effort_count; i++) {
            Str255 label;
            PStr(label, info->supported_efforts[i]);
            AppendMenu(d->effort, label);
            /* AppendMenu reads metacharacters; the efforts are plain words,
             * but set the text outright so a stray one cannot become a flag. */
            SetMenuItemText(d->effort, (short)(i + 1), label);
        }
        for (i = 0; i < info->effort_count; i++)
            if (info->default_effort[0] && !strcmp(info->supported_efforts[i], info->default_effort)) {
                choose = i + 1;
                break;
            }
        d->effort_on = 1;
        d->effort_choice = choose;
    }
    InvalRect(&d->popup);
}

/* Click on the popup: track the menu at the box and keep the chosen item. */
static void PrefsEffortClick(PrefsDialog *d)
{
    Point where;
    long pick;
    if (!d->effort || !d->effort_on) return;
    where.v = d->popup.top; where.h = d->popup.left;
    LocalToGlobal(&where);
    pick = PopUpMenuSelect(d->effort, where.v, where.h, (short)d->effort_choice);
    if ((short)(pick >> 16) == kPrefsEffortMenuID && (short)(pick & 0xFFFF) > 0)
        d->effort_choice = (short)(pick & 0xFFFF);
    PrefsEffortDraw(d);
}

/* Typing filters the fetched page locally; the empty filter shows all rows. */
static void PrefsFilterRows(PrefsDialog *d)
{
    int i;
    d->visible_count = 0;
    for (i = 0; i < d->count; i++)
        if (agent_model_row_match(&d->rows[i], d->filter))
            d->visible[d->visible_count++] = i;
}

static Rect PrefsRowRect(const Rect *box, int index)
{
    Rect r;
    r.left   = box->left + 2;
    r.right  = box->right - 2;
    r.top    = box->top + 2 + (short)(index * kPrefsRowHeight);
    r.bottom = r.top + kPrefsRowHeight;
    return r;
}

/* Hand-drawn result rows on the user item; the picked row is inverted. */
static void PrefsDrawRows(PrefsDialog *d)
{
    Rect box = d->results;
    int i;
    EraseRect(&box);
    FrameRect(&box);
    PrefsFilterRows(d);
    for (i = 0; i < d->visible_count; i++) {
        const AgentModelRow *row = &d->rows[d->visible[i]];
        Rect r = PrefsRowRect(&box, i);
        DrawFittedLabel(&r, row->id, truncMiddle);
        if (d->visible[i] == d->selected) InvertRect(&r);
    }
}

static int PrefsRowHit(PrefsDialog *d, Point local)
{
    int index;
    PrefsFilterRows(d);
    if (local.h < d->results.left + 2 || local.h >= d->results.right - 2) return -1;
    if (local.v < d->results.top + 2) return -1;
    index = (local.v - (d->results.top + 2)) / kPrefsRowHeight;
    if (index < 0 || index >= d->visible_count) return -1;
    return d->visible[index];
}

/* A picked row confirms the model: the exact id lands in the editable field,
 * its effort choices load with the default preselected, and its metadata is
 * kept for the save. */
static void PrefsConfirm(PrefsDialog *d, int index)
{
    d->selected = index;
    d->edited = 0;
    d->confirmed_info = d->rows[index].info;
    strcpy(d->confirmed_id, d->rows[index].id);
    SetPrefsText(d->dlg, kPrefsModelItem, d->rows[index].id);
    strcpy(d->last_text, d->rows[index].id);
    d->filter[0] = 0;
    PrefsEffortShow(d, &d->confirmed_info);
    PrefsHint(d, "Model confirmed. Press OK to save.");
    PrefsDrawRows(d);
}

/* Start one catalog exchange; no other fetch may start until this one reaches
 * a terminal state. */
static int PrefsFetchStart(PrefsDialog *d, int kind, const char *query)
{
    char path[CHAT_MODEL_CAP * 3 + 48];
    int length;
    if (kind == kPrefsFetchPopular) length = agent_popular_query(path, sizeof(path));
    else length = agent_model_query(path, sizeof(path), query);
    if (length < 0) { PrefsHint(d, "That model name is too long for a catalog search."); return -1; }
    length = http_build_get("openrouter.ai", path, gNet.request, sizeof(gNet.request));
    if (length < 0) { PrefsHint(d, "The catalog request is too large."); return -1; }
    EnsureOpenTransport();
    gStartTicks = (uint32_t)TickCount();
    if (network_start(&gNet, gNet.request, (size_t)length) < 0) {
        CloseChatContext();
        PrefsHint(d, "Could not start the catalog fetch.");
        return -1;
    }
    d->fetch = kind;
    d->fetch_start = (uint32_t)TickCount();
    PrefsHint(d, kind == kPrefsFetchPopular ? "Loading the popular models..." :
                 kind == kPrefsFetchValidate ? "Checking that model against the catalog..." :
                 "Searching the catalog...");
    return 0;
}

static void PrefsFetchFailed(PrefsDialog *d, int timed_out)
{
    d->fetch = kPrefsFetchNone;
    d->draining = 0;
    PrefsHint(d, timed_out ? "The catalog fetch timed out. Try Find again."
                           : "Could not load the catalog. Try Find again.");
}

/* A completed exchange: the page replaces the rows. Validate confirms only an
 * exact id and always lists the candidates; the one popular load is cached for
 * later opens, where a saved model still in the page resolves at once. */
static void PrefsFetchComplete(PrefsDialog *d)
{
    char text[CHAT_MODEL_CAP], hint[128];
    int kind = d->fetch, at;
    int count = agent_model_page(gNet.body, gNet.body_len, d->rows, kPrefsRowsMax);
    /* An empty popular page is a failure, not a list to cache for the launch:
     * a malformed or oversized page parses to zero rows. A search or validate
     * page with no rows is the catalog's answer for a typo. */
    if (count == 0 && kind == kPrefsFetchPopular) { PrefsFetchFailed(d, 0); return; }
    d->fetch = kPrefsFetchNone;
    d->count = count;
    d->selected = -1;
    GetPrefsText(d->dlg, kPrefsModelItem, text, sizeof(text));
    strcpy(d->last_text, text);
    /* Text typed while the popular page loaded filters it. Search and validate
     * pages were already matched by the server, so they list unfiltered and the
     * hint's count is what the user sees. */
    d->filter[0] = 0;
    if (kind == kPrefsFetchPopular && d->edited) strcpy(d->filter, text);
    if (kind == kPrefsFetchValidate) {
        at = agent_model_row_find(d->rows, d->count, text);
        if (at >= 0) {
            d->selected = at;
            d->confirmed_info = d->rows[at].info;
            strcpy(d->confirmed_id, d->rows[at].id);
            PrefsEffortShow(d, &d->confirmed_info);
            PrefsHint(d, "Model confirmed. Press OK again to save.");
        } else PrefsHint(d, "No exact match. Pick a candidate or press Find.");
    } else if (kind == kPrefsFetchPopular) {
        memcpy(gPopularRows, d->rows, (size_t)d->count * sizeof(AgentModelRow));
        gPopularCount = d->count;
        gPopularLoaded = 1;
        at = agent_model_row_find(d->rows, d->count, text);
        if (at >= 0) {
            d->selected = at;
            d->confirmed_info = d->rows[at].info;
            strcpy(d->confirmed_id, d->rows[at].id);
            PrefsEffortShow(d, &d->confirmed_info);
        }
        PrefsHint(d, "Popular models loaded. Type to filter or Find.");
    } else {
        snprintf(hint, sizeof(hint), "%d match%s. Click a row to pick it.",
                 d->count, d->count == 1 ? "" : "es");
        PrefsHint(d, hint);
    }
    d->edited = 0;
    PrefsDrawRows(d);
}

/* One network step per pass, with the send-time lookup's deadline and drain
 * rules: a timed-out connect is not torn down mid-flight. */
static void PrefsFetchStep(PrefsDialog *d)
{
    int result;
    uint32_t now = (uint32_t)TickCount();
    if (!d->draining && now - d->fetch_start > kPrefsFetchDeadline) {
        d->draining = 1;
        d->fetch_start = now;
    }
    result = network_step(&gNet);
    if (result == 0) {
        if (d->draining && (uint32_t)TickCount() - d->fetch_start > kPrefsFetchDeadline) {
            CloseChatContext();
            PrefsFetchFailed(d, 1);
        }
        return;
    }
    CloseChatContext();
    if (d->draining) { PrefsFetchFailed(d, 1); return; }
    if (result < 0 || gNet.status != 200) { PrefsFetchFailed(d, 0); return; }
    PrefsFetchComplete(d);
}

/* DragWindow and TrackControl block until the mouse is released and no network
 * step runs meanwhile, so the wait does not count against the deadline. */
static void PrefsFetchResume(PrefsDialog *d)
{
    if (d->fetch || d->draining) d->fetch_start = (uint32_t)TickCount();
}

/* Find: fetch the matches for the typed text, or show the popular page when
 * the box is empty and nothing has been fetched yet. */
static void PrefsFind(PrefsDialog *d)
{
    char text[CHAT_MODEL_CAP];
    if (d->fetch || d->draining) return;
    GetPrefsText(d->dlg, kPrefsModelItem, text, sizeof(text));
    strcpy(d->last_text, text);
    if (!text[0]) {
        d->filter[0] = 0;
        if (d->count) {
            PrefsHint(d, "Type to filter, or enter a model ID and press Find.");
            PrefsDrawRows(d);
        } else PrefsFetchStart(d, kPrefsFetchPopular, NULL);
        return;
    }
    d->filter[0] = 0;
    PrefsFetchStart(d, kPrefsFetchSearch, text);
}

static void ShowPreferences(void)
{
    static Prefs candidate;   /* the live values change only after a verified save */
    PrefsDialog *d = &gPrefsDlg;
    DialogPtr dlg;
    MenuHandle effort = NULL;
    short item, type;
    Handle item_handle;
    int done = 0, saved = 0, i, debug = gPrefs.show_tool_debug, rounds, tools;
    char model[CHAT_MODEL_CAP], key[PREFS_KEY_CAP], workspace[PREFS_WORKSPACE_CAP];
    char rounds_text[8], tools_text[8];

    memset(d, 0, sizeof(*d));
    d->selected = -1;
    dlg = GetNewDialog(kPrefsDialogID, NULL, (WindowPtr)-1L);
    if (!dlg) { SetStatus("Could not open the Preferences dialog."); return; }
    d->dlg = dlg;
    SetPort(dlg);

    /* The Effort popup is a hand-drawn user item (item 9); its menu is built
     * and inserted here so PopUpMenuSelect can track it. */
    GetDialogItem(dlg, kPrefsEffortItem, &type, &item_handle, &d->popup);
    GetDialogItem(dlg, kPrefsResultsItem, &type, &item_handle, &d->results);
    effort = NewMenu(kPrefsEffortMenuID, (ConstStr255Param)"\p");
    if (effort) InsertMenu(effort, hierMenu);
    d->effort = effort;
    PrefsEffortShow(d, NULL);

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

    GetPrefsText(dlg, kPrefsModelItem, model, sizeof(model));
    strcpy(d->last_text, model);
    if (gPopularLoaded) {
        /* Later opens reuse the page fetched earlier this launch, so the
         * popular models appear without another fetch. */
        d->count = gPopularCount;
        memcpy(d->rows, gPopularRows, (size_t)gPopularCount * sizeof(AgentModelRow));
        i = agent_model_row_find(d->rows, d->count, model);
        if (i >= 0) {
            d->selected = i;
            d->confirmed_info = d->rows[i].info;
            strcpy(d->confirmed_id, d->rows[i].id);
            PrefsEffortShow(d, &d->confirmed_info);
        }
        PrefsHint(d, "Type to filter, or press Find to search.");
        PrefsDrawRows(d);
    } else if (PrefsFetchStart(d, kPrefsFetchPopular, NULL) == 0) {
        PrefsDrawRows(d);
    }

    while (!done) {
        EventRecord event;
        int busy = d->fetch || d->draining;
        int hit = 0;
        WaitNextEvent(everyEvent, &event, busy ? 1 : 10, NULL);
        SetPort(dlg);

        /* Every branch falls through to the network step below: a pass that
         * only dragged the window or answered an update must still advance
         * the fetch. */
        do {
            if (event.what == updateEvt) {
                if ((WindowPtr)event.message == dlg) {
                    BeginUpdate(dlg);
                    DrawDialog(dlg);
                    PrefsDrawRows(d);
                    PrefsEffortDraw(d);
                    EndUpdate(dlg);
                } else if ((WindowPtr)event.message == gWindow) {
                    UpdateMainWindow();
                    SetPort(dlg);
                }
                break;
            }
            if (event.what == kHighLevelEvent) { AEProcessAppleEvent(&event); break; }
            if (event.what == mouseDown) {
                WindowPtr which = NULL;
                short part = FindWindow(event.where, &which);
                if (which == dlg && part == inDrag) {
                    /* The dialog is movable; drag its title bar ourselves. */
                    Rect screen = (*GetGrayRgn())->rgnBBox;
                    DragWindow(dlg, event.where, &screen);
                    SetPort(dlg);
                    PrefsFetchResume(d);
                    break;
                }
            }
            item = PrefsKeyItem(dlg, &event);
            if (item) hit = event.what == keyDown;   /* a held key does not repeat the press */
            else hit = DialogSelect(&event, &dlg, &item);
            if (!hit) break;
            if (item == kPrefsCancelItem) {
                /* Hand an in-flight exchange to the main loop's drain path:
                 * classic OT can fault when a connect is torn down mid-flight.
                 * The main loop owns the exchange from here, so this pass must
                 * not step it again. */
                if (d->fetch || d->draining) BeginExchangeDrain();
                done = 1;
            } else if (item == kPrefsFindItem) {
                PrefsFind(d);
            } else if (item == kPrefsEffortItem) {
                PrefsEffortClick(d);
                PrefsFetchResume(d);
            } else if (item == kPrefsResultsItem) {
                Point local = event.where;
                GlobalToLocal(&local);
                i = PrefsRowHit(d, local);
                if (i >= 0) PrefsConfirm(d, i);
            } else if (item == kPrefsDebugItem) {
                /* DialogSelect reports the checkbox hit but does not toggle it. */
                ControlHandle ctl = PrefsCheckbox(dlg);
                debug = !debug;
                SetControlValue(ctl, debug ? 1 : 0);
                Draw1Control(ctl);
            } else if (item == kPrefsOKItem) {
                if (d->fetch || d->draining) {
                    PrefsHint(d, "Still checking; press OK again in a moment.");
                } else {
                    int confirmed;
                    GetPrefsText(dlg, kPrefsModelItem, model, sizeof(model));
                    GetPrefsText(dlg, kPrefsKeyItem, key, sizeof(key));
                    GetPrefsText(dlg, kPrefsWorkspaceItem, workspace, sizeof(workspace));
                    GetPrefsText(dlg, kPrefsRoundsItem, rounds_text, sizeof(rounds_text));
                    GetPrefsText(dlg, kPrefsToolsItem, tools_text, sizeof(tools_text));
                    rounds = prefs_limit_value(rounds_text);
                    tools = prefs_limit_value(tools_text);
                    confirmed = d->confirmed_id[0] && !strcmp(d->confirmed_id, model);
                    if (!prefs_model_ok(model)) {
                        PrefsProblem(dlg, "Enter a model ID: printable characters, no spaces.");
                        SelectDialogItemText(dlg, kPrefsModelItem, 0, 32767);
                    } else if (!prefs_key_ok(key)) {
                        PrefsProblem(dlg, "The API key must be printable characters without spaces.");
                        SelectDialogItemText(dlg, kPrefsKeyItem, 0, 32767);
                    } else if (!prefs_workspace_ok(workspace)) {
                        PrefsProblem(dlg, "The workspace must be a colon path ending in ':', like Retro68:.");
                        SelectDialogItemText(dlg, kPrefsWorkspaceItem, 0, 32767);
                    } else if (rounds < 0) {
                        PrefsProblem(dlg, "Max model rounds must be a number from 1 to 128.");
                        SelectDialogItemText(dlg, kPrefsRoundsItem, 0, 32767);
                    } else if (tools < 0) {
                        PrefsProblem(dlg, "Max tool calls must be a number from 1 to 128.");
                        SelectDialogItemText(dlg, kPrefsToolsItem, 0, 32767);
                    } else if (!confirmed && strcmp(model, gPrefs.model)) {
                        /* A changed id never reaches the preferences file
                         * unresolved: check the catalog and require an exact
                         * row first. The saved id is already trusted, so the
                         * other fields stay editable while the catalog is
                         * unreachable. */
                        PrefsFetchStart(d, kPrefsFetchValidate, model);
                    } else {
                        candidate = gPrefs;
                        strcpy(candidate.model, model);
                        strcpy(candidate.api_key, key);
                        strcpy(candidate.workspace, workspace);
                        candidate.max_rounds = rounds;
                        candidate.max_tools = tools;
                        candidate.show_tool_debug = debug;
                        if (PrefsSave(&candidate)) {
                            PrefsProblem(dlg, "Could not save the preferences file. Nothing was changed.");
                        } else {
                            gPrefs = candidate;
                            tools_set_workspace(gPrefs.workspace);
                            if (confirmed) {
                                /* The confirmed row is this model's context
                                 * source until a send-time lookup replaces it. */
                                gModelInfo = d->confirmed_info;
                                strcpy(gModelInfoModel, gPrefs.model);
                                gModelInfoAttempted = 1;
                            }
                            saved = 1;
                            done = 1;
                        }
                    }
                }
            }
        } while (0);
        if (done) break;
        /* Typing filters the fetched rows locally; programmatic sets keep
         * last_text in sync so only hand edits land here. */
        GetPrefsText(dlg, kPrefsModelItem, model, sizeof(model));
        if (strcmp(model, d->last_text)) {
            strcpy(d->last_text, model);
            strcpy(d->filter, model);
            d->edited = 1;
            /* The popup describes the confirmed row only while the text
             * still names it. */
            if (d->confirmed_id[0] && !strcmp(model, d->confirmed_id)) {
                if (!d->effort_shown) PrefsEffortShow(d, &d->confirmed_info);
            } else if (d->effort_shown) PrefsEffortShow(d, NULL);
            PrefsDrawRows(d);
        }
        if (d->fetch || d->draining) PrefsFetchStep(d);
    }

    CloseDialog(dlg);
    if (effort) { DeleteMenu(kPrefsEffortMenuID); DisposeMenu(effort); }
    if (gWindow) SetPort(gWindow);
    if (saved) {
        InvalRect(&gModelRect);
        SetStatus("Preferences saved; the workspace applies to new work now.");
    }
    FocusSet(gPromptTE);
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

/* The scroll helpers work on any TextEdit record plus its scroll bar so the
 * MCP editor shares them. ResponseScrollTo stays for the pane and
 * tools/scroll-check.c. */
static short PaneLineHeight(TEHandle te)
{
    short lh;
    if (!te) return 12;
    lh = (*te)->lineHeight;
    return lh > 0 ? lh : 12;
}

static short PanePageHeight(TEHandle te)
{
    return ResponsePageHeight(PaneLineHeight(te), (*te)->viewRect.bottom - (*te)->viewRect.top);
}

static short PaneClamp(ControlHandle bar, long v)
{
    return ClampScroll(v, GetControlMaximum(bar));
}

static short PaneMaxScroll(TEHandle te)
{
    if (!te) return 0;
    return ComputeMaxScroll(PaneLineHeight(te), (*te)->nLines,
                            (*te)->viewRect.bottom - (*te)->viewRect.top);
}

/* The control is an indicator, not the source of the text's position.
 * TESetText preserves destRect, and TESelView does nothing unless TEAutoView
 * is enabled. Always move from the actual destination to the requested offset
 * so a reply, thumb drag, or reset cannot leave the two out of sync. */
static void PaneScrollTo(TEHandle te, ControlHandle bar, long pixels)
{
    short after = PaneClamp(bar, pixels);
    long before = (long)(*te)->viewRect.top - (*te)->destRect.top;
    TEScroll(0, (short)(before - after), te);
    SetControlValue(bar, after);
}

static void PaneScrollStep(TEHandle te, ControlHandle bar, short part)
{
    short delta = 0;
    long before;

    switch (part) {
    case kControlUpButtonPart:   delta = -PaneLineHeight(te); break;
    case kControlDownButtonPart: delta =  PaneLineHeight(te); break;
    case kControlPageUpPart:     delta = -PanePageHeight(te); break;
    case kControlPageDownPart:   delta =  PanePageHeight(te); break;
    default: return;
    }
    before = (long)(*te)->viewRect.top - (*te)->destRect.top;
    PaneScrollTo(te, bar, before + delta);
}

static TEHandle gMcpTE = NULL;            /* MCP editor pane; set only while it is open */
static ControlHandle gMcpScroll = NULL;

static void ResponseScrollTo(long pixels) { PaneScrollTo(gResponseTE, gResponseScroll, pixels); }

static pascal void ScrollActionProc(ControlHandle c, short part)
{
    if (gMcpScroll && c == gMcpScroll) PaneScrollStep(gMcpTE, c, part);
    else if (c == gResponseScroll) PaneScrollStep(gResponseTE, c, part);
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
        SetControlMaximum(gResponseScroll, PaneMaxScroll(gResponseTE));
        SetControlValue(gResponseScroll, 0);
    }
    InvalRect(&gResponseRect);
}

/* ── MCP Servers… editor ──────────────────────────────────────────── */

/* Cut, Copy, Paste, Clear and Select All (Edit items 3-6 and 8) on one
 * TextEdit record. Returns 1 when a paste was refused for size. */
static int TEEditCommand(TEHandle te, short item, long maximum)
{
    switch (item) {
    case 3: ZeroScrap(); TECut(te);  TEToScrap(); break;
    case 4: ZeroScrap(); TECopy(te); TEToScrap(); break;
    case 5: {
        long incoming = 0;
        Handle scrap = NewHandle(0);
        long offset = 0;
        long remaining;
        if (scrap) incoming = GetScrap(scrap, 'TEXT', &offset);
        if (scrap) DisposeHandle(scrap);
        remaining = (*te)->teLength - ((*te)->selEnd - (*te)->selStart);
        if (incoming > 0 && incoming <= maximum - remaining) {
            TEFromScrap(); TEPaste(te);
        } else return 1;
        break;
    }
    case 6: TEDelete(te);              break;
    case 8: TESetSelect(0, 32767, te); break;
    default: break;   /* Undo: not implemented */
    }
    return 0;
}

enum {
    kMcpDialogID = 131,
    kMcpSaveItem = 1,
    kMcpCancelItem = 2,
    kMcpFrameItem = 3,
    kMcpHintItem = 4
};
/* Static: they hold the file, which can carry credentials, and the app stack
 * is small. Wiped when the editor closes. */
static char gMcpText[MCP_EDITOR_TEXT_CAP + 1];
static char gMcpUtf8[MCP_EDITOR_UTF8_CAP];
static char gMcpFile[MCP_STORE_CAP + 1];
static char gMcpError[256];

static OSErr McpSpec(FSSpec *spec)
{
    short vRefNum;
    long  dirID;
    Str255 name;
    OSErr err = FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder,
                           &vRefNum, &dirID);
    if (err) return err;
    PStr(name, MCP_CONFIG_FILENAME);
    return FSMakeFSSpec(vRefNum, dirID, name, spec);
}

/* An alert before the editor exists; PrefsProblem is the one for an open dialog. */
static void McpAlert(const char *message)
{
    Str255 p;
    PStr(p, message);
    ParamText(p, NULL, NULL, NULL);
    StopAlert(kPrefsAlertID, NULL);
    if (gWindow) SetPort(gWindow);
}

static void McpHint(DialogPtr dlg, const char *text)
{
    short  type;
    Handle handle;
    Rect   rect;
    SetPrefsText(dlg, kMcpHintItem, text);
    GetDialogItem(dlg, kMcpHintItem, &type, &handle, &rect);
    InvalRect(&rect);
}

/* Keep the scroll bar's range and thumb in step with the text after any edit. */
static void McpSyncScroll(void)
{
    long top = (long)(*gMcpTE)->viewRect.top - (*gMcpTE)->destRect.top;
    SetControlMaximum(gMcpScroll, PaneMaxScroll(gMcpTE));
    SetControlValue(gMcpScroll, PaneClamp(gMcpScroll, top));
}

static const char *McpRecoverText =
    "An interrupted save left only \"" MCP_CONFIG_FILENAME ".old\". Rename it to restore the file.";

static const char *McpSaveFailure(McpStoreResult r)
{
    switch (r) {
    case MCP_STORE_RECOVER:  return McpRecoverText;
    case MCP_STORE_RESTORED: return "Could not publish the new file; the previous file was restored.";
    case MCP_STORE_LOST:
        return "The save failed and the previous file is now named \"" MCP_CONFIG_FILENAME ".old\". Rename it to restore it.";
    default: return "Could not save the MCP file. The previous file was not changed.";
    }
}

static void ShowMcpServers(void)
{
    FSSpec spec;
    DialogPtr dlg;
    short type;
    Handle item_handle;
    Rect frame, view, bar;
    size_t file_len = 0;
    int text_len, done = 0, saved = 0;
    McpStoreResult loaded;
    OSErr err = McpSpec(&spec);

    if (err && err != fnfErr) { SetStatus("Could not find the Preferences folder."); return; }
    loaded = mcp_store_load(&spec, gMcpFile, MCP_STORE_CAP, &file_len);
    if (loaded == MCP_STORE_ABSENT) {
        text_len = mcp_editor_display(mcp_editor_template, strlen(mcp_editor_template),
                                      gMcpText, sizeof(gMcpText));
    } else if (loaded == MCP_STORE_OK) {
        text_len = mcp_editor_display(gMcpFile, file_len, gMcpText, sizeof(gMcpText));
    } else {
        text_len = -1;
    }
    memset(gMcpFile, 0, sizeof(gMcpFile));
    if (text_len < 0) {
        /* Never open an editor that could overwrite what it cannot show. */
        McpAlert(loaded == MCP_STORE_RECOVER ? McpRecoverText :
                 "The MCP file cannot be shown (too large, unreadable or not Mac text). It was not changed.");
        memset(gMcpText, 0, sizeof(gMcpText));
        return;
    }

    dlg = GetNewDialog(kMcpDialogID, NULL, (WindowPtr)-1L);
    if (!dlg) { SetStatus("Could not open the MCP Servers dialog."); memset(gMcpText, 0, sizeof(gMcpText)); return; }
    SetPort(dlg);
    GetDialogItem(dlg, kMcpFrameItem, &type, &item_handle, &frame);
    bar = frame;  bar.left = frame.right - 16;
    view = frame; view.right = bar.left; InsetRect(&view, 3, 2);
    TextFont(kFontIDMonaco); TextSize(9);
    gMcpTE = TENew(&view, &view);
    TextFont(systemFont); TextSize(0);   /* dialog items keep the system font */
    gMcpScroll = NewControl(dlg, &bar, (ConstStr255Param)"\p", true, 0, 0, 0, scrollBarProc, 0);
    if (!gMcpTE || !gMcpScroll) {
        if (gMcpTE) TEDispose(gMcpTE);
        if (gMcpScroll) DisposeControl(gMcpScroll);
        gMcpTE = NULL; gMcpScroll = NULL;
        CloseDialog(dlg);
        memset(gMcpText, 0, sizeof(gMcpText));
        if (gWindow) SetPort(gWindow);
        SetStatus("Could not create the MCP editor.");
        return;
    }
    TEAutoView(true, gMcpTE);
    TESetText(gMcpText, text_len, gMcpTE);
    TESetSelect(0, 0, gMcpTE);
    memset(gMcpText, 0, sizeof(gMcpText));
    TEActivate(gMcpTE);
    McpSyncScroll();
    SetDialogDefaultItem(dlg, kMcpSaveItem);
    SetDialogCancelItem(dlg, kMcpCancelItem);
    if (loaded == MCP_STORE_ABSENT) McpHint(dlg, "No MCP file yet. Edit the template, then press Save.");

    while (!done) {
        EventRecord event;
        short item = 0;
        int hit = 0;
        WaitNextEvent(everyEvent, &event, 10, NULL);
        SetPort(dlg);
        do {
            if (event.what == updateEvt) {
                if ((WindowPtr)event.message == dlg) {
                    BeginUpdate(dlg);
                    TextFont(systemFont); TextSize(0);
                    DrawDialog(dlg);
                    DrawControls(dlg);
                    FrameRect(&frame);
                    TEUpdate(&view, gMcpTE);
                    EndUpdate(dlg);
                } else if ((WindowPtr)event.message == gWindow) {
                    UpdateMainWindow();
                    SetPort(dlg);
                }
                break;
            }
            if (event.what == activateEvt) {
                if ((WindowPtr)event.message == dlg) {
                    if (event.modifiers & activeFlag) TEActivate(gMcpTE);
                    else TEDeactivate(gMcpTE);
                }
                break;
            }
            if (event.what == kHighLevelEvent) { AEProcessAppleEvent(&event); break; }
            if (event.what == mouseDown) {
                WindowPtr which = NULL;
                short part = FindWindow(event.where, &which);
                if (part == inMenuBar) {
                    long choice = MenuSelect(event.where);
                    if (HiWord(choice) == kEditMenuID &&
                        TEEditCommand(gMcpTE, LoWord(choice), MCP_EDITOR_TEXT_CAP))
                        McpHint(dlg, "Paste is empty or exceeds the 8 KiB limit.");
                    HiliteMenu(0);
                    SetPort(dlg);
                    break;
                }
                if (which == dlg && part == inDrag) {
                    Rect screen = (*GetGrayRgn())->rgnBBox;
                    DragWindow(dlg, event.where, &screen);
                    SetPort(dlg);
                    break;
                }
                if (which == dlg && part == inContent) {
                    ControlHandle ctl = NULL;
                    Point local = event.where;
                    short cpart;
                    GlobalToLocal(&local);
                    cpart = FindControl(local, dlg, &ctl);
                    if (cpart && ctl == gMcpScroll) {
                        if (cpart == kControlIndicatorPart) {
                            TrackControl(ctl, local, NULL);
                            PaneScrollTo(gMcpTE, gMcpScroll, GetControlValue(ctl));
                        } else {
                            TrackControl(ctl, local, gScrollActionUPP);
                        }
                        break;
                    }
                    if (!cpart && PtInRect(local, &view)) {
                        TEClick(local, (event.modifiers & shiftKey) != 0, gMcpTE);
                        break;
                    }
                }
            }
            if (event.what == keyDown || event.what == autoKey) {
                char c = (char)(event.message & charCodeMask);
                if (event.modifiers & cmdKey) {
                    if (c == '.') { if (event.what == keyDown) item = kMcpCancelItem; }
                    else {
                        long choice = MenuKey(c);
                        if (HiWord(choice) == kEditMenuID &&
                            TEEditCommand(gMcpTE, LoWord(choice), MCP_EDITOR_TEXT_CAP))
                            McpHint(dlg, "Paste is empty or exceeds the 8 KiB limit.");
                        HiliteMenu(0);
                    }
                } else if (c == 3) {
                    if (event.what == keyDown) item = kMcpSaveItem;   /* Enter; Return is a newline */
                } else if (c == 27) {
                    if (event.what == keyDown) item = kMcpCancelItem;
                } else {
                    long remaining = (*gMcpTE)->teLength - ((*gMcpTE)->selEnd - (*gMcpTE)->selStart);
                    if (c == 8 || ((unsigned char)c >= 0x1c && (unsigned char)c <= 0x1f) ||
                        remaining < MCP_EDITOR_TEXT_CAP) {
                        TEKey(c, gMcpTE); TESelView(gMcpTE);
                    } else McpHint(dlg, "8 KiB limit reached.");
                }
                hit = item != 0;
                break;
            }
            hit = DialogSelect(&event, &dlg, &item);
        } while (0);

        if (hit && item == kMcpCancelItem) {
            done = 1;
        } else if (hit && item == kMcpSaveItem) {
            int n = TEGetTextInto(gMcpTE, gMcpText, sizeof(gMcpText));
            int u = n < 0 ? -1 : mcp_editor_validate(gMcpText, (size_t)n, gMcpUtf8, sizeof(gMcpUtf8),
                                                     gMcpError, sizeof(gMcpError));
            if (n < 0) {
                PrefsProblem(dlg, "Too much text for the MCP file (8 KiB limit).");
            } else if (u < 0) {
                PrefsProblem(dlg, gMcpError);   /* names the field, never a value */
            } else {
                McpStoreResult r = mcp_store_save(&spec, gMcpUtf8, (size_t)u);
                if (r == MCP_STORE_OK) { saved = 1; done = 1; }
                else PrefsProblem(dlg, McpSaveFailure(r));
            }
            memset(gMcpText, 0, sizeof(gMcpText));
            memset(gMcpUtf8, 0, sizeof(gMcpUtf8));
            memset(gMcpError, 0, sizeof(gMcpError));
        }
        if (done) break;
        TEIdle(gMcpTE);
        McpSyncScroll();
    }

    TESetSelect(0, 32767, gMcpTE);
    TEDelete(gMcpTE);                    /* do not leave credentials in the freed record */
    TEDispose(gMcpTE);
    DisposeControl(gMcpScroll);
    gMcpTE = NULL; gMcpScroll = NULL;
    CloseDialog(dlg);
    if (gWindow) SetPort(gWindow);
    if (saved) SetStatus("MCP configuration saved. The agent does not use it yet.");
    FocusSet(gPromptTE);
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
            if (RunBusy()) SetStatus("Finish the current run before changing Preferences.");
            else if (gExchangeDrain) SetStatus("The stopped request is still closing; try again in a moment.");
            else ShowPreferences();
            break;
        }
        if (item == kEditMcpItem) {
            if (RunBusy()) SetStatus("Finish the current run before changing MCP Servers.");
            else if (gExchangeDrain) SetStatus("The stopped request is still closing; try again in a moment.");
            else ShowMcpServers();
            break;
        }
        if (gFocusedTE && (item == 4 || item == 8 ||
            (!RunBusy() && gFocusedTE != gResponseTE))) {
            if (TEEditCommand(gFocusedTE, item, CHAT_PROMPT_CAP - 1))
                SetStatus("Paste is empty or exceeds the field limit.");
        }
        break;

    default:
        break;
    }
    return 0;
}

/* ── Layout, drawing, UI setup ────────────────────────────────────── */

static void SetDisplayRect(Rect *out, DisplayRect r)
{
    SetRect(out, r.left, r.top, r.right, r.bottom);
}

static void UILayout(void)
{
    DisplayLayout layout;
    ComputeLayout(&layout);
    SetDisplayRect(&gResponseLabelRect, layout.ResponseLabelRect);
    SetDisplayRect(&gResponseRect, layout.ResponseRect);
    SetDisplayRect(&gResponseViewRect, layout.ResponseViewRect);
    SetDisplayRect(&gPromptLabelRect, layout.PromptLabelRect);
    SetDisplayRect(&gPromptHintRect, layout.PromptHintRect);
    SetDisplayRect(&gPromptRect, layout.PromptRect);
    SetDisplayRect(&gInfoRect, layout.InfoRect);
    SetDisplayRect(&gStatusRect, layout.StatusRect);
    SetDisplayRect(&gModelRect, layout.ModelRect);
    SetDisplayRect(&gHistoryRect, layout.HistoryRect);
    SetDisplayRect(&gMeterRect, layout.MeterRect);
    SetDisplayRect(&gUsageRect, layout.UsageRect);
    SetDisplayRect(&gContextRect, layout.ContextRect);
    SetDisplayRect(&gCostRect, layout.CostRect);
    SetDisplayRect(&gNewRect, layout.NewRect);
    SetDisplayRect(&gHandoffRect, layout.HandoffRect);
    SetDisplayRect(&gStopRect, layout.StopRect);
    SetDisplayRect(&gSendRect, layout.SendRect);
}

/* QuickDraw chrome keeps the actual Window/Control Manager in charge of the
 * native title bar, buttons and scrollbar. White is the TextEdit background. */
static void PaintColorRect(const Rect *r, unsigned short shade)
{
    RGBColor color = { shade, shade, shade };
    RGBForeColor(&color); PaintRect(r); ForeColor(blackColor);
}

static void DrawRecessedFrame(const Rect *r)
{
    RGBColor shadow = { 0x7777, 0x7777, 0x7777 };
    RGBForeColor(&shadow);
    MoveTo(r->left, r->bottom - 1); LineTo(r->left, r->top);
    LineTo(r->right - 1, r->top);
    ForeColor(whiteColor);
    MoveTo(r->right - 1, r->top + 1); LineTo(r->right - 1, r->bottom - 1);
    LineTo(r->left + 1, r->bottom - 1);
    ForeColor(blackColor);
}

static void DrawTextPane(const Rect *r, TEHandle te)
{
    Rect outer = *r;
    InsetRect(&outer, -1, -1);
    DrawRecessedFrame(&outer);
    EraseRect(r); FrameRect(r);
    if (te) TEUpdate(&(*te)->viewRect, te);
}

static void DrawChrome(void)
{
    Str255 p;
    char line[CHAT_MODEL_CAP + 16], count[24];
    Rect art = { 0, 0, 52, 52 }, destination = { 2, 10, 54, 62 };
    Rect status = gStatusRect;
    Rect lamp = { 363, 18, 371, 26 };
    Rect meter = gMeterRect;
    RGBColor accent = RunBusy() ? (RGBColor){ 0xAAAA, 0x7777, 0x1111 }
                               : (RGBColor){ 0x2222, 0x8888, 0x2222 };
    long dollars, micros;
    unsigned long percent = (unsigned long)(gAgent.used * 100 / AGENT_HISTORY_CAP);

    PaintColorRect(&gWindow->portRect, 0xDDDD);
    TextSize(10); TextFace(normal);
    if (gArt) {
        PixMapHandle pixels = GetGWorldPixMap(gArt);
        if (LockPixels(pixels)) {
            CopyBits((BitMap *)*pixels, &gWindow->portBits, &art, &destination, srcCopy, NULL);
            UnlockPixels(pixels);
        }
    }
    TextSize(18); TextFace(bold);
    PStr(p, "Sherclawk"); MoveTo(72, 28); DrawString(p);
    TextSize(10); TextFace(normal);
    PStr(p, "The consulting crustacean"); MoveTo(72, 43); DrawString(p);

    DrawLabel(&gResponseLabelRect, "Conversation:");
    DrawTextPane(&gResponseRect, gResponseTE);
    DrawLabel(&gPromptLabelRect, "Message:");
    PStr(p, "Command-Return to send");
    MoveTo(gPromptHintRect.right - StringWidth(p), gPromptHintRect.bottom - 4);
    DrawString(p);
    DrawTextPane(&gPromptRect, gPromptTE);

    DrawRecessedFrame(&gInfoRect);
    RGBForeColor(&accent); PaintOval(&lamp);
    ForeColor(blackColor); FrameOval(&lamp);
    status.left += 14;
    DrawFittedLabel(&status, gStatusText, truncEnd);
    snprintf(line, sizeof(line), "Model: %s", gPrefs.model);
    DrawFittedLabel(&gModelRect, line, truncMiddle);
    {
        Rect divider = { 377, 18, 378, 582 };
        PaintColorRect(&divider, 0xAAAA);
        OffsetRect(&divider, 0, 1); PaintColorRect(&divider, 0xFFFF);
        SetRect(&divider, 246, 382, 247, 394); PaintColorRect(&divider, 0xAAAA);
        OffsetRect(&divider, 1, 0); PaintColorRect(&divider, 0xFFFF);
        SetRect(&divider, 458, 382, 459, 394); PaintColorRect(&divider, 0xAAAA);
        OffsetRect(&divider, 1, 0); PaintColorRect(&divider, 0xFFFF);
    }
    TextSize(9);
    snprintf(line, sizeof(line), "History: %lu/%lu KiB (%lu%%)",
        (unsigned long)((gAgent.used + 1023) / 1024),
        (unsigned long)(AGENT_HISTORY_CAP / 1024), percent);
    {
        Rect label = gHistoryRect;
        label.right = gMeterRect.left - 4;
        DrawFittedLabel(&label, line, truncEnd);
    }
    EraseRect(&meter); FrameRect(&meter); InsetRect(&meter, 1, 1);
    if (percent > 100) percent = 100;
    meter.right = meter.left + (short)((meter.right - meter.left) * percent / 100);
    if (percent > 0) {
        RGBColor fill = { 0x5555, 0x6666, 0xAAAA };
        RGBForeColor(&fill); PaintRect(&meter); ForeColor(blackColor);
    }
    gDisplayedHistory = gAgent.used;
    if (gAgent.context_seen) FormatTokens(gAgent.context_tokens, count, sizeof(count));
    else strcpy(count, "-");
    if (gAgent.context_seen && gModelInfo.context_length > 0 && !strcmp(gModelInfoModel, gRunModel)) {
        long long contextPercent = ((long long)gAgent.context_tokens * 100 + gModelInfo.context_length / 2) / gModelInfo.context_length;
        if (contextPercent > 100) contextPercent = 100;
        snprintf(line, sizeof(line), "Context: %s tokens (%ld%%)", count, (long)contextPercent);
    } else if (gAgent.context_seen) snprintf(line, sizeof(line), "Context: %s tokens", count);
    else snprintf(line, sizeof(line), "Context: -");
    DrawFittedLabel(&gContextRect, line, truncEnd);
    if (gAgent.cost_micros > 0) {
        dollars = (long)(gAgent.cost_micros / 1000000LL);
        micros = (long)(gAgent.cost_micros % 1000000LL);
        snprintf(line, sizeof(line), "Cost: $%lu.%06lu", (unsigned long)dollars, (unsigned long)micros);
        DrawFittedLabel(&gCostRect, line, truncEnd);
    }
    gDisplayedCost = gAgent.cost_micros;
    gDisplayedTokens = gAgent.context_seen ? gAgent.context_tokens : -1;
    gDisplayedLimit = gModelInfo.context_length;
    TextSize(10);
    DrawControls(gWindow);
}

static void UIInit(void)
{
    Rect             bounds;
    MenuBarHandle    mb;

    RegisterAppearanceClient();

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

    UILayout();
    {
        Handle resource = Get1Resource('sART', 129);
        Rect bounds = { 0, 0, 52, 52 };
        if (resource && GetHandleSize(resource) == 4 + 52L * 52L * 4 &&
            NewGWorld(&gArt, 32, &bounds, NULL, NULL, 0) == noErr) {
            PixMapHandle pixels = GetGWorldPixMap(gArt);
            if (LockPixels(pixels)) {
                unsigned char *base = (unsigned char *)GetPixBaseAddr(pixels);
                long row = GetPixRowBytes(pixels) & 0x3fff;
                int y;
                HLock(resource);
                for (y = 0; y < 52; y++) memcpy(base + y * row, *resource + 4 + y * 52L * 4, 52 * 4);
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
    if (gSendBtn)       { DisposeControl(gSendBtn);  gSendBtn = NULL; }

    if (gEditMenu)  { DeleteMenu(kEditMenuID);  DisposeMenu(gEditMenu);  gEditMenu  = NULL; }
    if (gFileMenu)  { DeleteMenu(kFileMenuID);  DisposeMenu(gFileMenu);  gFileMenu  = NULL; }
    if (gAppleMenu) { DeleteMenu(kAppleMenuID); DisposeMenu(gAppleMenu); gAppleMenu = NULL; }

    if (gWindow) { DisposeWindow(gWindow); gWindow = NULL; }
    UnregisterAppearanceClient();
}

/* ── Event handling ───────────────────────────────────────────────── */

static void HandleEvent(const EventRecord *event)
{
    switch (event->what) {
    case kHighLevelEvent: AEProcessAppleEvent(event); break;
    case updateEvt:
        if ((WindowPtr)event->message == gWindow) UpdateMainWindow();
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
                if (!RunBusy() &&
                    TrackControl(ctl, local, NULL) == kControlButtonPart) {
                    SendChat();
                }
                break;
            }

            if (cpart && ctl == gStopBtn) {
                if (RunBusy() && TrackControl(ctl, local, NULL) == kControlButtonPart)
                    AbortChat("Stopped. Completed tool results are retained.");
                break;
            }
            if (cpart && ctl == gNewBtn) {
                if (!RunBusy() && TrackControl(ctl, local, NULL) == kControlButtonPart) NewChat();
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

        if (!RunBusy() && PtInRect(local, &gStatusRect)) {
            Str255 message;
            PStr(message, gStatusText);
            ParamText(message, NULL, NULL, NULL);
            NoteAlert(129, NULL);
            SetPort(gWindow);
            break;
        }
        if (!RunBusy() && gPromptTE && PtInRect(local, &(*gPromptTE)->viewRect)) {
            FocusSet(gPromptTE);
            TEClick(local, (event->modifiers & shiftKey) != 0, gPromptTE);
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
            if (RunBusy()) AbortChat("Stopped. Completed tool results are retained.");
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
            if (!RunBusy()) FocusSet(gFocusedTE == gPromptTE ? gResponseTE : gPromptTE);
            break;
        }
        if (!RunBusy() && gFocusedTE == gPromptTE) {
            long limit = CHAT_PROMPT_CAP - 1;
            long remaining = (*gFocusedTE)->teLength -
                ((*gFocusedTE)->selEnd - (*gFocusedTE)->selStart);
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
/* The session journal and handoff files live in session.c. */
/* Returns where the entry starts in the transcript, or -1 when it was not shown. */
static long ShowMessage(const char *label, const char *text)
{
    long at = chat_append_message(&gChat, label, text);
    if (at < 0) return at;
    ResponseSetText(gChat.transcript, strlen(gChat.transcript));
    ResponseScrollTo(GetControlMaximum(gResponseScroll));
    return at;
}

/* ── AGENTS.md ───────────────────────────────────────────────────── */
/* The workspace-root file rides in the system message for the whole chat and is
 * read when the chat's first message is sent. A project's file is read once,
 * after the first tool round that touches the project, and recorded as history.
 * Either way the user is told what was loaded or skipped. */
static char gInstructions[AGENT_INSTRUCTIONS_CAP + 1];
static int ReadInstructions(const char *folder, unsigned long *hash)
{
    char label[200], note[400];
    int status = tools_read_instructions(folder, gInstructions, sizeof(gInstructions), hash);
    if (status == TOOLS_INSTRUCTIONS_ABSENT) return status;
    snprintf(label, sizeof(label), "%s%sAGENTS.md", folder, *folder ? ":" : "");
    if (status < 0) snprintf(note, sizeof(note), "Skipped %s: it must be a plain TEXT file that is not an alias or a folder and does not change while it is read.", label);
    else snprintf(note, sizeof(note), "Loaded %s (%lu bytes%s).", label, (unsigned long)strlen(gInstructions),
                  status == TOOLS_INSTRUCTIONS_TRUNCATED ? ", cut to fit" : "");
    ShowMessage("Instructions", note);
    return status;
}
static void LoadWorkspaceInstructions(void)
{
    char json[160];
    unsigned long hash = 0;
    int status = ReadInstructions("", &hash);
    agent_set_instructions(status > 0 ? gInstructions : NULL);
    if (status <= 0) return;
    snprintf(json, sizeof(json), "{\"path\":\"AGENTS.md\",\"bytes\":%lu,\"truncated\":%s,\"hash\":\"%08lx\"}",
             (unsigned long)strlen(gInstructions), status == TOOLS_INSTRUCTIONS_TRUNCATED ? "true" : "false", hash);
    session_journal(&gSession, "workspace_instructions", json);
}
/* Called once every tool result of the round is in. Returns -1 when a note
 * could not be recorded, which stops the run. */
static int NoteProjectInstructions(void)
{
    int i;
    for (i = 0; i < gAgent.count; i++) {
        char project[AGENT_PROJECT_NAME_CAP];
        unsigned long hash;
        int status;
        if (!tools_call_project(&gAgent.calls[i], project, sizeof(project)) || agent_project_seen(&gAgent, project)) continue;
        status = ReadInstructions(project, &hash);
        if (agent_project_note(&gAgent, project, status > 0 ? gInstructions : NULL)) return -1;
    }
    return 0;
}

/* ── Tool debug view (display only; the session file is unaffected) ─ */
/* Journal event names emitted for the tool call being executed. */
static char gToolEvents[192];
static int ToolEventJournal(void *context, const char *event, const char *json)
{
    ToolEventsAppend(gToolEvents, sizeof(gToolEvents), event);
    return session_journal(context, event, json);
}

/* One tool message: the compact call line always; with the display toggle on,
 * the result indented under it and the call's journal events follow. */
static void ShowToolResult(const AgentCall *call, const char *label, const char *text)
{
    static char block[AGENT_RESULT_CAP + 4096]; /* header, one result and the call's journal events */
    char header[400];
    if (!call) { ShowMessage(label, text); return; }
    RenderToolCall(call, header, sizeof(header));
    if (!gPrefs.show_tool_debug) {
        /* A run of calls to one tool folds into a single counted line. */
        if (chat_tool_collapse(&gChat, call->name)) {
            ResponseSetText(gChat.transcript, strlen(gChat.transcript));
            ResponseScrollTo(GetControlMaximum(gResponseScroll));
        } else {
            long at = ShowMessage(NULL, header);
            if (at >= 0) chat_tool_note(&gChat, (size_t)at, call->name);
        }
        return;
    }
    snprintf(block, sizeof(block), "%s\r  \xC2\xBB %s\r  journal: %s",
        header, text, gToolEvents[0] ? gToolEvents : "(none)");
    ShowMessage(NULL, block);
}
static void NewChat(void)
{
    if (RunBusy()) return;
    session_close(&gSession); agent_reset(&gAgent, session_journal, &gSession); chat_reset(&gChat);
    agent_set_instructions(NULL);
    view_image_reset();
    ResponseSetText("", 0); TESetText("", 0, gPromptTE); InvalRect(&gPromptRect);
    FocusSet(gPromptTE); SetStatus("Ready. What shall we investigate?");
}
/* Long-running tools finish over many event-loop steps. A row here pairs a
 * registry row with its run state, start and step functions and messages;
 * Stop and normal completion both finish it through PendingToolStep. A new
 * one is a registry row plus a RunState value plus a row here. */
typedef struct {
    RunState state;
    const ToolDef *def;   /* registry row that names the tool */
    int (*begin)(const AgentCall *, char *, size_t, AgentJournal, void *, uint32_t);
    int (*step)(char *, size_t, uint32_t, int stop);
    const char *status;  /* shown while it is pending */
    const char *failure; /* stop reason when the step reports failure */
} PendingState;
static const PendingState kPendingStates[] = {
    { RUN_READ_TEXT, &kToolDefs[TOOL_read_text], read_text_begin, read_text_step,
      "Reading and verifying text...", "Text read stopped; no revision supplied." },
    { RUN_EDIT_TEXT, &kToolDefs[TOOL_edit_text], edit_text_begin, edit_text_step,
      "Staging and verifying exact edit...", "Edit stopped. Inspect retained recovery paths before continuing." },
    { RUN_BUILD, &kToolDefs[TOOL_build_project], build_project_begin, build_project_step,
      "Building snapshot with MPW ToolServer...",
      "Build observation stopped. Inspect retained snapshot and logs before another build." },
    { RUN_LAUNCH, &kToolDefs[TOOL_run_application], run_application_begin, run_application_step,
      "Verifying built application before launch...",
      "Launch outcome uncertain. Inspect the run journal; do not retry automatically." },
    { RUN_QUIT, &kToolDefs[TOOL_quit_application], quit_application_begin, quit_application_step,
      "Requesting graceful quit; observing owned process...",
      "Quit outcome uncertain. The request may still take effect; do not retry." },
    { RUN_VIEW_IMAGE, &kToolDefs[TOOL_view_image], view_image_begin, view_image_step,
      "Reading image...",
      "Image read stopped. Nothing was attached." }
};
static const PendingState *PendingStateForDef(const ToolDef *def)
{
    size_t i;
    for (i = 0; i < sizeof(kPendingStates) / sizeof(kPendingStates[0]); i++)
        if (kPendingStates[i].def == def) return &kPendingStates[i];
    return NULL;
}
static const PendingState *PendingStateForRun(RunState state)
{
    size_t i;
    for (i = 0; i < sizeof(kPendingStates) / sizeof(kPendingStates[0]); i++)
        if (kPendingStates[i].state == state) return &kPendingStates[i];
    return NULL;
}
/* Advance the pending tool; `stop` makes it conclude now. Returns 2 while it
 * is still running. Otherwise the run returns to RUN_TOOLS (before recording,
 * so a recording failure that aborts cannot finish the tool twice), the result
 * is recorded and shown, and 0 comes back with the tool's own status in
 * *result, or -1 with `error` if the result could not be recorded. */
static int PendingToolStep(const PendingState *tool, int stop, int *result, char *error, size_t cap)
{
    const AgentCall *call = gAgent.next < gAgent.count ? &gAgent.calls[gAgent.next] : NULL;
    *result = tool->step(gToolResult, sizeof(gToolResult), (uint32_t)TickCount(), stop);
    if (!stop && *result == 2) return 2;
    gRun = RUN_TOOLS;
    if (!stop) LogToolTiming(gAgent.next + 1, tool->def->name);
    if (agent_tool_result(&gAgent, gToolResult, error, cap)) return -1;
    ShowToolResult(call, tool->def->name, gToolResult);
    return 0;
}
static void StartHandoff(void)
{
    int length;
    char attribution[256];
    if (RunBusy()) return;
    if (gExchangeDrain) { SetStatus("The stopped request is still closing; try again in a moment."); return; }
    if (!gSession.open || !gAgent.messages || gAgent.active || gAgent.next < gAgent.count) {
        SetStatus("Finish or stop the current run before saving a handoff."); return;
    }
    strcpy(gRunModel, gPrefs.model);
    if (!gPrefs.api_key[0] || !*gRunModel) {
        SetStatus("A model and API key are needed for a handoff."); return;
    }
    if (*gRunModel && !model_id_valid(gRunModel)) {
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
    gHandoffPath[0] = 0; gRun = RUN_HANDOFF;
    timing_round_begin(&gRoundTiming, 0, (uint32_t)TickCount());
    EnsureOpenTransport(); gStartTicks = (uint32_t)TickCount();
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
    const PendingState *pending = PendingStateForRun(gRun);
    if (gRun == RUN_HANDOFF) {
        AbandonChatContext();
        LogRoundTiming("abort");
        gRun = RUN_IDLE; SetSendEnabled(1); FocusSet(gPromptTE);
        ShowMessage("Handoff stopped; conversation retained", reason);
        if (*gHandoffPath) ShowMessage("Retained handoff file (may be incomplete)", gHandoffPath);
        SetStatus("Handoff stopped; conversation retained. %s", reason);
        LogAbort("Handoff stopped; conversation retained. Reason", reason);
        return;
    }
    if (gRun == RUN_CONTEXT_LOOKUP) {
        AbandonChatContext();
        gRun = RUN_IDLE; SetSendEnabled(1); FocusSet(gPromptTE);
        SetStatus("Model context lookup stopped. Send again shortly.");
        LogAbort("Model context lookup stopped. Reason", reason);
        return;
    }
    if (pending) {
        char error[256];
        int result;
        PendingToolStep(pending, 1, &result, error, sizeof(error));
    }
    AbandonChatContext();
    LogRoundTiming("abort");
    if (agent_stop(&gAgent, reason)) {
        SetStatus("Session recording failed. Start a new session before continuing.");
        session_close(&gSession);
    } else SetStatus("%s", reason);
    gRun = RUN_IDLE; SetSendEnabled(1); FocusSet(gPromptTE);
    ShowMessage("Stopped", reason);
    LogAbort("Agent stopped; completed records retained. Reason", reason);
}
static void PauseRunAtLimit(void)
{
    char reason[256];
    FormatPauseReason(gAgent.rounds, gAgent.tool_count, gPrefs.max_rounds,
                      gPrefs.max_tools, gAgent.used, reason, sizeof(reason));
    AbortChat(reason);
}
/* agent_response rejects a bad body before recording it, so the history and
 * session journal would keep no trace of the failed request. Journal its HTTP
 * status, received body size and reason instead. */
static void JournalModelError(const char *reason)
{
    static char quoted[1600], detail[1700];
    if (!gSession.open || json_quote(reason, quoted, sizeof(quoted)) < 0) return;
    snprintf(detail, sizeof(detail), "{\"http_status\":%d,\"received_bytes\":%lu,\"error\":%s}",
        gNet.status, (unsigned long)gNet.body_len, quoted);
    session_journal(&gSession, "model_error", detail);
}
static int StartModelRequest(void)
{
    int length;
    char attribution[256];
    if (gAgent.rounds >= gPrefs.max_rounds || gAgent.tool_count >= gPrefs.max_tools) {
        PauseRunAtLimit(); return -1;
    }
    length = agent_request_image(&gAgent, gRunModel, view_image_held(), gJSON, sizeof(gJSON));
    if (length < 0) { AbortChat("Request limit reached. Start a new session."); return -1; }
    if (sherclawk_attribution(SHERCLAWK_APP_URL, attribution, sizeof(attribution)) < 0) {
        AbortChat("Attribution URL is too long."); return -1;
    }
    length = http_build_post_with_headers("openrouter.ai", "/api/v1/chat/completions", gPrefs.api_key,
        attribution, gJSON, (size_t)length, gNet.request, sizeof(gNet.request));
    if (length < 0) { AbortChat("Request or API key is too long or invalid."); return -1; }
    timing_round_begin(&gRoundTiming, gAgent.rounds + 1, (uint32_t)TickCount());
    EnsureOpenTransport();
    gStartTicks = (uint32_t)TickCount(); gRun = RUN_MODEL_REQUEST;
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
    gRun = RUN_IDLE;
    if (!gSession.open && (gAgent.messages || session_open(&gSession, tools_workspace()))) {
        SetStatus("Session file unavailable. Check %s or start a new session.", tools_workspace()); return;
    }
    if (!gAgent.messages) LoadWorkspaceInstructions();
    if (agent_begin(&gAgent, gPending, error, sizeof(error))) { SetStatus("%s", error); return; }
    view_image_reset();
    ShowMessage("You", gPending);
    TESetText("", 0, gPromptTE); InvalRect(&gPromptRect);
    SetSendEnabled(0); StartModelRequest();
}
static void StartContextLookup(void)
{
    char path[CHAT_MODEL_CAP * 3 + 32];
    int length;
    length = agent_model_query(path, sizeof(path), gRunModel);
    if (length < 0) { SendBegin(); return; }
    length = http_build_get("openrouter.ai", path, gNet.request, sizeof(gNet.request));
    if (length < 0) { SendBegin(); return; }
    /* Remember the attempt before the network: at most one lookup per model
     * per launch, whatever the outcome. */
    strcpy(gModelInfoModel, gRunModel); memset(&gModelInfo, 0, sizeof(gModelInfo));
    gModelInfo.context_length = -1; gModelInfoAttempted = 1;
    EnsureOpenTransport();
    gStartTicks = (uint32_t)TickCount();
    if (network_start(&gNet, gNet.request, (size_t)length) < 0) {
        CloseChatContext(); SendBegin(); return;
    }
    gRun = RUN_CONTEXT_LOOKUP;
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
    AgentModelInfo info;
    if (completed && gNet.status == 200 && !agent_model_info(gNet.body, gNet.body_len, gRunModel, &info))
        gModelInfo = info;
    if (gNet.ctx || gOTOpen) CloseChatContext();
    {
        if (strcmp(gPrefs.model, gRunModel)) {
            gRun = RUN_IDLE; SetSendEnabled(1); FocusSet(gPromptTE);
            SetStatus("Model changed during the context lookup. Send again to continue.");
            return;
        }
    }
    gRun = RUN_IDLE;
    SendAdvance();
}
static void SendChat(void)
{
    static char prompt[CHAT_PROMPT_CAP]; /* static: the application stack is small */
    if (RunBusy()) return;
    if (gExchangeDrain) { SetStatus("The stopped request is still closing; send again in a moment."); return; }
    if (!gPrefs.api_key[0]) { SetStatus("No API key: choose Preferences from the Edit menu."); return; }
    strcpy(gRunModel, gPrefs.model);
    if (TEGetTextInto(gPromptTE, prompt, sizeof(prompt)) < 0) { SetStatus("Message is too long."); return; }
    if (*gRunModel && !model_id_valid(gRunModel)) {
        SetStatus("Use an OpenRouter model ID without spaces."); return;
    }
    if (!*gRunModel || prompt_is_blank(prompt)) { SetStatus("Enter a model and message first."); return; }
    if (text_to_utf8(prompt, strlen(prompt), gPending, sizeof(gPending)) < 0) { SetStatus("Could not convert message to UTF-8."); return; }
    SendAdvance();
}
static void StepContextLookup(void)
{
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
}
static void StepPendingTool(void)
{
    const PendingState *tool = PendingStateForRun(gRun);
    char error[256];
    int result;
    int status = PendingToolStep(tool, 0, &result, error, sizeof(error));
    if (status == 2) return;
    if (status) { AbortChat(error); return; }
    if (result) AbortChat(tool->failure);
}
static void StepTools(void)
{
    char error[256];
    int result;
    AgentCall *call;
    const ToolDef *def;
    const PendingState *pending;
    if (gAgent.next == gAgent.count) {
        /* Tool messages cannot carry pixels: an image from this round follows
         * the last tool result, as a user message for the next request. */
        if (NoteProjectInstructions()) { AbortChat("Could not record project instructions. Stop and inspect the session."); return; }
        const AgentImage *shot = view_image_take();
        if (shot && agent_attach_image(&gAgent, shot)) { AbortChat("Could not record the attached image. Stop and inspect the session."); return; }
        gRun = RUN_NEXT_REQUEST; return;
    }
    if (gAgent.tool_count >= gPrefs.max_tools) { PauseRunAtLimit(); return; }
    call = &gAgent.calls[gAgent.next];
    gToolStart = (uint32_t)TickCount();
    SetStatus("Running %s (%d/%d)...", call->name, gAgent.next + 1, gAgent.count);
    ToolEventsReset(gToolEvents, sizeof(gToolEvents));
    {
        char id[800], name[400], started[1300];
        if (json_quote(call->id, id, sizeof(id)) < 0 || json_quote(call->name, name, sizeof(name)) < 0) {
            AbortChat("Could not encode tool start."); return;
        }
        snprintf(started, sizeof(started), "{\"call_id\":%s,\"name\":%s}", id, name);
        if (session_journal(&gSession, "tool_started", started)) { AbortChat("Could not record tool start; no tool executed."); return; }
        ToolEventsAppend(gToolEvents, sizeof(gToolEvents), "tool_started");
    }
    view_image_set_vision(gModelInfoAttempted && !strcmp(gModelInfoModel, gRunModel) ? gModelInfo.vision : AGENT_VISION_UNKNOWN);
    def = tools_lookup(call->name);
    pending = def ? PendingStateForDef(def) : NULL;
    if (pending) {
        result = pending->begin(call, gToolResult, sizeof(gToolResult), ToolEventJournal, &gSession, (uint32_t)TickCount());
        if (result == 2) { gRun = pending->state; SetStatus("%s", pending->status); return; }
    } else if (def && def->id == TOOL_read_build_log) {
        build_project_log(call, gToolResult, sizeof(gToolResult)); result = 0;
    } else result = tools_execute_recorded(call, gToolResult, sizeof(gToolResult), ToolEventJournal, &gSession);
    LogToolTiming(gAgent.next + 1, call->name);
    if (agent_tool_result(&gAgent, gToolResult, error, sizeof(error))) { AbortChat(error); return; }
    ShowToolResult(call, call->name, gToolResult);
    if (result) AbortChat("Mutation stopped. Inspect the result and session recovery records; do not retry automatically.");
}
/* The model request and the handoff summary share one HTTPS exchange; only
 * the completed response is handled differently. */
static void StepModelExchange(void)
{
    char error[256];
    int result;
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
    if (gRun == RUN_HANDOFF) {
        ObserveCompletionTokens();
        if (agent_handoff_response(gNet.body, gNet.body_len, gNet.status, gHandoffSummary,
            sizeof(gHandoffSummary), error, sizeof(error))) { JournalModelError(error); AbortChat(error); return; }
        FinishChatContext();
        /* The summary completion is provider-billed like any other round. */
        agent_usage_absorb(&gAgent, gNet.body, gNet.body_len);
        LogRoundTiming("ok");
        if (session_commit_handoff(&gSession, &gAgent, gHandoffSummary, gHandoffPath, sizeof(gHandoffPath))) {
            AbortChat("Could not save/verify handoff and new journal. History was not cleared."); return;
        }
        gRun = RUN_IDLE; SetSendEnabled(1); FocusSet(gPromptTE);
        ShowMessage("Saved handoff", gHandoffPath);
        ShowMessage("Handoff summary", gHandoffSummary);
        SetStatus("Handoff saved; fresh history is ready. Send a message to continue.");
        return;
    }
    ObserveCompletionTokens();
    if (agent_response(&gAgent, gNet.body, gNet.body_len, gNet.status, error, sizeof(error))) { JournalModelError(error); AbortChat(error); return; }
    FinishChatContext();
    LogRoundTiming(gAgent.truncated ? "truncated" : gAgent.discarded ? "discarded" : "ok");
    if (gAgent.discarded) {
        char note[160];
        gRun = RUN_NEXT_REQUEST;
        snprintf(note, sizeof(note), "Reply discarded: %s. Nothing in it ran. Asking the model to retry.", gAgent.discarded);
        ShowMessage("Notice", note);
        SetStatus("Reply discarded; asking the model to retry smaller.");
        snprintf(note, sizeof(note), "Reply discarded (%s); model told to retry.", gAgent.discarded);
        LogLine(note);
        return;
    }
    if (gAgent.truncated) {
        gRun = RUN_NEXT_REQUEST;
        ShowMessage("Notice", "Reply cut off at the output token limit; nothing in it ran. Asking the model to retry.");
        SetStatus("Output token limit reached; asking the model to retry.");
        LogLine("Reply truncated at the output token limit; model told to retry.");
        return;
    }
    if (*gAgent.text) ShowMessage("Sherclawk", gAgent.text);
    if (gAgent.count) {
        gRun = RUN_TOOLS; SetStatus("Model requested %d tool(s).", gAgent.count);
    } else {
        gRun = RUN_IDLE; SetSendEnabled(1); FocusSet(gPromptTE);
        if (gAgent.limited) ShowMessage("Notice", "Reply incomplete: output token limit reached.");
        if (gAgent.used >= AGENT_HISTORY_CAP * 3 / 4)
            SetStatus("History nearly full. Save Handoff (Command-H) to free history.");
        else SetStatus("Done - %d model rounds, %d tools. Session saved.", gAgent.rounds, gAgent.tool_count);
        LogLine("Agent run completed.");
    }
}
static void DriveChatStep(void)
{
    switch (gRun) {
    case RUN_IDLE: break;
    case RUN_CONTEXT_LOOKUP: StepContextLookup(); break;
    case RUN_NEXT_REQUEST: StartModelRequest(); break;
    case RUN_READ_TEXT:
    case RUN_EDIT_TEXT:
    case RUN_BUILD:
    case RUN_QUIT:
    case RUN_LAUNCH:
    case RUN_VIEW_IMAGE: StepPendingTool(); break;
    case RUN_TOOLS: StepTools(); break;
    case RUN_MODEL_REQUEST:
    case RUN_HANDOFF: StepModelExchange(); break;
    }
}
/* An abandoned exchange keeps its context alive until its connect settles
 * (connected, errored or closed); closing it here avoids tearing OT down
 * mid-connect. It only pumps: no request is written and nothing is read. The
 * deadline is the backstop for a connect that never settles; Certainly owns
 * the connect timeout, so reaching it is logged as a forced close. */
static void DrainAbandonedExchange(void)
{
    int expired;
    if (!gExchangeDrain) return;
    expired = (uint32_t)TickCount() - gDrainStartTicks > 30UL * 60UL;
    if (ExchangeConnecting() && !expired) {
        MacTLS_Pump(gNet.ctx);
        return;
    }
    if (expired && ExchangeConnecting())
        LogLine("Abandoned exchange still connecting after 30 s; forcing close.");
    CloseChatContext();
    gExchangeDrain = 0;
}

int main(void)
{
    EventRecord event;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL); InitCursor(); MacTLS_Init();
    PrefsLoad();
    tools_set_workspace(gPrefs.workspace);
    UIInit();
    if (!gWindow || !gPromptTE || !gResponseTE ||
        !gSendBtn || !gStopBtn || !gNewBtn || !gHandoffBtn || !gResponseScroll || !gScrollActionUPP) {
        UIDispose(); MacTLS_Shutdown(); return 1;
    }
    chat_reset(&gChat); agent_reset(&gAgent, session_journal, &gSession);
    SetStatus(gPrefsUnreadable ? "Preferences unreadable; using compiled defaults."
                               : "Ready. What shall we investigate?");
    LogOpen(); LogLine("Sherclawk session started.");
    if(selfbuild_init())SetStatus("ToolServer unavailable; builds cannot start.");
    while (!gQuit) {
        WaitNextEvent(everyEvent, &event, RunBusy() ? 1 : 10, NULL);
        SetPort(gWindow); HandleEvent(&event);
        if (RunBusy()) DriveChatStep();
        DrainAbandonedExchange();
        selfbuild_drain((uint32_t)TickCount());
        UpdateHandoffControls();
        /* History changes during sends, tool results, New Chat and handoff.
         * Keep its indicator current even when the ordinary status is unchanged. */
        if (gDisplayedHistory != gAgent.used) InvalRect(&gHistoryRect);
        if (gDisplayedCost != gAgent.cost_micros ||
            gDisplayedTokens != (gAgent.context_seen ? gAgent.context_tokens : -1) ||
            gDisplayedLimit != gModelInfo.context_length) InvalRect(&gUsageRect);
    }
    LogRoundTiming("abort");
    if (gAgent.active) AbortChat("Application quit.");
    else AbandonChatContext();  /* a stopped lookup/handoff, or a pending drain */
    if (gExchangeDrain) {
        /* The window would go stale while the loop ignores events, so hide it;
         * the drain is bounded by its own 30 s backstop. */
        HideWindow(gWindow);
        while (gExchangeDrain) {
            WaitNextEvent(everyEvent, &event, 1, NULL);
            DrainAbandonedExchange();
        }
    }
    if (gNet.ctx || gOTOpen) CloseChatContext();
    selfbuild_close();
    session_close(&gSession);
    LogLine("Sherclawk session ended."); LogClose(); UIDispose();
    MacTLS_Shutdown(); return 0;
}
