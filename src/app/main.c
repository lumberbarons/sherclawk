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
#include "agent/network.h"
#include "app/timing.h"
#include "app/run_ctl.h"
#include "agent/agent.h"
#include "app/session.h"
#include "agent/json.h"
#include "tools/tools.h"
#include "native/build_project.h"
#include "native/selfbuild.h"
#include <AppleEvents.h>
#include "native/run_application.h"
#include "tools/view_image.h"
#include <QDOffscreen.h>
#include <Resources.h>
#include "agent/text.h"
#include "config.h"
#include "app/preferences.h"
#include "app/prefs_model.h"
#include "app/display.h"
#include "mcp/editor.h"
#include "mcp/store.h"
#include "mcp/run.h"

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
static Chat gChat; /* Bounded display transcript; gAgent owns wire history. */

static Rect gModelRect, gSendRect;
static Rect gInfoRect, gStatusRect, gHistoryRect, gMeterRect;
static size_t gDisplayedHistory = (size_t)-1;
static Rect gUsageRect, gContextRect, gCostRect, gPromptHintRect;
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
static Agent gAgent;
static GWorldPtr gArt = NULL;
static Session gSession;
static ChatNetwork gNet;
static RunCtl gRunCtl;
static int RunBusy(void) { return run_ctl_busy(&gRunCtl); }
static void RunInit(void);
static void CloseChatContext(void) { run_ctl_close_context(&gRunCtl); }
static void AbandonChatContext(void) { run_ctl_abandon_context(&gRunCtl); }
static void EnsureOpenTransport(void) { run_ctl_open_transport(&gRunCtl); }
static void DrainAbandonedExchange(void) { run_ctl_drain_exchange(&gRunCtl); }
static void DrainMcpClient(void) { run_ctl_drain_mcp(&gRunCtl); }
static void DriveChatStep(void) { run_ctl_step(&gRunCtl); }
static void LogRoundTiming(const char *outcome) { run_ctl_log_round(&gRunCtl, outcome); }
static void AbortChat(const char *reason) { run_ctl_abort(&gRunCtl, reason); }
static void SendChat(void) { run_ctl_send(&gRunCtl); }
static void StartHandoff(void) { run_ctl_handoff(&gRunCtl); }
static void FocusSet(TEHandle te);
static void DrawChrome(void);
static void NewChat(void);

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

static void UpdateHandoffControls(void)
{
    int enabled = !RunBusy() && !gRunCtl.exchange_drain && gSession.open && gAgent.messages &&
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

/* A trusted saved row (window, vision flag and default effort, stored when
 * the model was confirmed) stands in for the send-time lookup, so the first
 * send needs no catalog request. Without one the lookup runs as before. */
static void ModelInfoSeedFromPrefs(void)
{
    if (!prefs_row_for(&gPrefs, gPrefs.model, &gRunCtl.model_info)) return;
    strcpy(gRunCtl.model_info_model, gPrefs.model);
    gRunCtl.model_info_attempted = 1;
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
    kPrefsRowHeight = 15
};

/* State carried between events of the modal Preferences loop. The model
 * picker's rows, filter and confirmed row live in `model`
 * (src/app/prefs_model.c); the rest is Toolbox state. */
typedef struct {
    DialogPtr     dlg;
    Rect          popup;             /* Effort popup user item rectangle */
    MenuHandle    effort;            /* runtime menu behind the popup; NULL on failure */
    int           effort_on;         /* the menu holds real choices */
    int           effort_choice;     /* 1-based chosen item */
    int           effort_shown;      /* popup lists confirmed_info's efforts */
    int           effort_saved_label;/* disabled popup shows the saved effort */
    int           effort_item_count; /* menu items backed by effort_items */
    char          effort_items[AGENT_EFFORT_MAX + 1][AGENT_EFFORT_CAP];
                                     /* stored value of each menu item: the
                                      * Default entry resolves to the row's
                                      * default effort, the rest store their
                                      * own name */
    Rect          results;           /* results userItem rectangle */
    PrefsModel    model;
} PrefsDialog;

static PrefsDialog     gPrefsDlg;
static PrefsModelCache gPrefsCache;    /* popular page and saved row, once per launch */

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
 * a down arrow. The disabled form (no choices) dims the label and arrow with a
 * gray pattern and text mode: the dialog is a plain GrafPort, where an RGB gray
 * maps to white and the label would vanish. */
static void PrefsEffortDraw(PrefsDialog *d)
{
    Rect box = d->popup, text;
    Str255 label;
    int i;
    box.right -= 1; box.bottom -= 1;
    EraseRoundRect(&box, 8, 8);
    FrameRoundRect(&box, 8, 8);
    MoveTo(box.left + 4, box.bottom); LineTo(box.right - 4, box.bottom);
    MoveTo(box.right, box.top + 4); LineTo(box.right, box.bottom - 4);
    label[0] = 0;
    if (d->effort && d->effort_choice >= 1 && d->effort_choice <= CountMItems(d->effort))
        GetMenuItemText(d->effort, (short)d->effort_choice, label);
    text.left = box.left + 8; text.right = box.right - 22;
    text.top = box.top; text.bottom = box.bottom;
    if (!d->effort_on) TextMode(grayishTextOr);
    TruncString(text.right - text.left, label, truncEnd);
    MoveTo(text.left, box.bottom - 5);
    DrawString(label);
    TextMode(srcOr);
    if (!d->effort_on) PenPat(&qd.gray);
    for (i = 0; i < 4; i++) {
        MoveTo(box.right - 16 + i, box.top + 7 + i);
        Line(7 - 2 * i, 0);
    }
    PenNormal();
}

/* Append an item with a plain placeholder, then set its text outright:
 * AppendMenu reads metacharacters such as the '(' in "Default (X)", which
 * would create the item disabled, and SetMenuItemText never clears that. */
static void PrefsEffortAppend(PrefsDialog *d, const char *text)
{
    Str255 label;
    PStr(label, text);
    AppendMenu(d->effort, (ConstStr255Param)"\px");
    SetMenuItemText(d->effort, CountMItems(d->effort), label);
}

/* An effort the popup may offer: a provider word the preferences file would
 * also accept, so a pick is never discarded at save time. */
static int PrefsEffortOffered(const AgentModelInfo *info, const char *effort)
{
    return effort[0] && prefs_effort_ok(effort) &&
           !(info->mandatory && !strcmp(effort, "none"));
}

/* Rebuild the effort menu for the text `model`. From a confirmed row: a
 * Default (X) entry first, where X is the row's default effort when it is one
 * of the offered supported efforts, then the supported efforts in order,
 * hiding none when reasoning is mandatory. Each item records the concrete
 * value its choice stores, so Default (X) resolves to X at save time. When
 * `model` is the saved model its saved effort is preselected instead of the
 * default. Without a row the popup is disabled, showing the saved effort for
 * the saved model and None otherwise. */
static void PrefsEffortShow(PrefsDialog *d, const AgentModelInfo *info, const char *model)
{
    int i, items = 0;
    const char *saved = !strcmp(model, gPrefs.model) ? gPrefs.effort : "";
    d->effort_shown = info != NULL;
    d->effort_saved_label = !info && saved[0];
    d->effort_on = 0;
    d->effort_choice = 1;
    d->effort_item_count = 0;
    memset(d->effort_items, 0, sizeof(d->effort_items));
    if (!d->effort) return;
    while (CountMItems(d->effort) > 0) DeleteMenuItem(d->effort, CountMItems(d->effort));
    if (info && info->effort_count > 0) {
        int default_ok = 0;
        for (i = 0; i < info->effort_count; i++)
            if (!strcmp(info->supported_efforts[i], info->default_effort) &&
                PrefsEffortOffered(info, info->default_effort)) default_ok = 1;
        if (default_ok) {
            char text[AGENT_EFFORT_CAP + 16];
            snprintf(text, sizeof(text), "Default (%s)", info->default_effort);
            PrefsEffortAppend(d, text);
            strcpy(d->effort_items[items++], info->default_effort);
        }
        for (i = 0; i < info->effort_count; i++) {
            if (!PrefsEffortOffered(info, info->supported_efforts[i])) continue;
            PrefsEffortAppend(d, info->supported_efforts[i]);
            strcpy(d->effort_items[items], info->supported_efforts[i]);
            items++;  /* the item table holds AGENT_EFFORT_MAX + 1 values */
        }
    }
    if (items > 0) {
        d->effort_on = 1;
        d->effort_item_count = items;
        d->effort_choice = 1;   /* the default effort, or the first supported */
        for (i = 0; saved[0] && i < items; i++)
            if (!strcmp(d->effort_items[i], saved)) { d->effort_choice = i + 1; break; }
    } else {
        PrefsEffortAppend(d, d->effort_saved_label ? saved : "None");
    }
    InvalRect(&d->popup);
}

/* The concrete effort value the chosen popup item stores, or "" when the
 * popup is disabled or the choice is out of range. */
static const char *PrefsEffortValue(const PrefsDialog *d)
{
    if (!d->effort_on || d->effort_choice < 1 || d->effort_choice > d->effort_item_count) return "";
    return d->effort_items[d->effort_choice - 1];
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
    PrefsModel *m = &d->model;
    Rect box = d->results;
    int i;
    EraseRect(&box);
    FrameRect(&box);
    prefs_model_filter(m);
    for (i = 0; i < m->visible_count; i++) {
        const AgentModelRow *row = &m->rows[m->visible[i]];
        Rect r = PrefsRowRect(&box, i);
        DrawFittedLabel(&r, row->id, truncMiddle);
        if (m->visible[i] == m->selected) InvertRect(&r);
    }
}

static int PrefsRowHit(PrefsDialog *d, Point local)
{
    int index;
    prefs_model_filter(&d->model);
    if (local.h < d->results.left + 2 || local.h >= d->results.right - 2) return -1;
    if (local.v < d->results.top + 2) return -1;
    index = (local.v - (d->results.top + 2)) / kPrefsRowHeight;
    if (index < 0 || index >= d->model.visible_count) return -1;
    return d->model.visible[index];
}

/* The picker's Toolbox effects; `context` is the PrefsDialog. */
static void PrefsHostHint(void *context, const char *text)
{
    PrefsHint((PrefsDialog *)context, text);
}

static void PrefsHostGetModel(void *context, char *out, size_t cap)
{
    GetPrefsText(((PrefsDialog *)context)->dlg, kPrefsModelItem, out, cap);
}

static void PrefsHostSetModel(void *context, const char *text)
{
    SetPrefsText(((PrefsDialog *)context)->dlg, kPrefsModelItem, text);
}

static void PrefsHostDrawRows(void *context)
{
    PrefsDrawRows((PrefsDialog *)context);
}

static void PrefsHostShowEffort(void *context, const AgentModelInfo *info, const char *model)
{
    PrefsEffortShow((PrefsDialog *)context, info, model);
}

static int PrefsHostSend(void *context, const char *path)
{
    int length = http_build_get("openrouter.ai", path, gNet.request, sizeof(gNet.request));
    (void)context;
    if (length < 0) return PREFS_SEND_TOO_LARGE;
    EnsureOpenTransport();
    gRunCtl.start_ticks = (uint32_t)TickCount();
    if (network_start(&gNet, gNet.request, (size_t)length) < 0) {
        CloseChatContext();
        return PREFS_SEND_FAILED;
    }
    return PREFS_SEND_OK;
}

static uint32_t PrefsHostNow(void *context)
{
    (void)context;
    return (uint32_t)TickCount();
}

static int PrefsHostPoll(void *context, const char **body, size_t *len)
{
    int result = network_step(&gNet);
    (void)context;
    if (result == 0) return PREFS_POLL_PENDING;
    if (result < 0 || gNet.status != 200) return PREFS_POLL_ERROR;
    *body = gNet.body;
    *len = gNet.body_len;
    return PREFS_POLL_DONE;
}

static void PrefsHostClose(void *context)
{
    (void)context;
    CloseChatContext();
}

static const PrefsModelHost kPrefsHost = {
    PrefsHostHint, PrefsHostGetModel, PrefsHostSetModel,
    PrefsHostDrawRows, PrefsHostShowEffort, PrefsHostSend, PrefsHostNow,
    PrefsHostPoll, PrefsHostClose
};

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
    prefs_model_init(&d->model, &kPrefsHost, d, &gPrefsCache, gPrefs.model);
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
    PrefsEffortShow(d, NULL, gPrefs.model);

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

    prefs_model_begin(&d->model);

    while (!done) {
        EventRecord event;
        int busy = prefs_model_busy(&d->model);
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
                    prefs_model_resume(&d->model);
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
                if (prefs_model_busy(&d->model)) run_ctl_begin_drain(&gRunCtl);
                done = 1;
            } else if (item == kPrefsFindItem) {
                prefs_model_find(&d->model);
            } else if (item == kPrefsEffortItem) {
                PrefsEffortClick(d);
                prefs_model_resume(&d->model);
            } else if (item == kPrefsResultsItem) {
                Point local = event.where;
                GlobalToLocal(&local);
                i = PrefsRowHit(d, local);
                if (i >= 0) prefs_model_confirm(&d->model, i);
            } else if (item == kPrefsDebugItem) {
                /* DialogSelect reports the checkbox hit but does not toggle it. */
                ControlHandle ctl = PrefsCheckbox(dlg);
                debug = !debug;
                SetControlValue(ctl, debug ? 1 : 0);
                Draw1Control(ctl);
            } else if (item == kPrefsOKItem) {
                if (prefs_model_busy(&d->model)) {
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
                    confirmed = prefs_model_is_confirmed(&d->model, model);
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
                        prefs_model_start(&d->model, PREFS_FETCH_VALIDATE, model);
                    } else {
                        const char *effort;
                        candidate = gPrefs;
                        strcpy(candidate.model, model);
                        strcpy(candidate.api_key, key);
                        strcpy(candidate.workspace, workspace);
                        candidate.max_rounds = rounds;
                        candidate.max_tools = tools;
                        candidate.show_tool_debug = debug;
                        /* A confirmed row supplies the effort its popup chose
                         * (empty when the row exposes none); the trusted saved
                         * model keeps its saved effort. A provider word
                         * outside the known set is not stored. */
                        effort = confirmed ? PrefsEffortValue(d) : gPrefs.effort;
                        if (!prefs_effort_ok(effort)) effort = "";
                        strcpy(candidate.effort, effort);
                        /* The confirmed row's window, vision flag and default
                         * effort are stored with the model they belong to; an
                         * unchanged, unconfirmed model keeps what it had. */
                        if (confirmed) {
                            prefs_store_row(&candidate, model, &d->model.confirmed_info);
                        }
                        if (PrefsSave(&candidate)) {
                            PrefsProblem(dlg, "Could not save the preferences file. Nothing was changed.");
                        } else {
                            gPrefs = candidate;
                            tools_set_workspace(gPrefs.workspace);
                            if (confirmed) {
                                /* The confirmed row is this model's context
                                 * source until a send-time lookup replaces it. */
                                gRunCtl.model_info = d->model.confirmed_info;
                                strcpy(gRunCtl.model_info_model, gPrefs.model);
                                gRunCtl.model_info_attempted = 1;
                                prefs_model_remember_saved(&d->model, gPrefs.model);
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
        if (prefs_model_text_changed(&d->model, model)) {
            /* The popup describes the confirmed row only while the text
             * still names it. */
            if (prefs_model_is_confirmed(&d->model, model)) {
                if (!d->effort_shown) PrefsEffortShow(d, &d->model.confirmed_info, d->model.confirmed_id);
            } else if (d->effort_shown ||
                       d->effort_saved_label != (!strcmp(model, gPrefs.model) && gPrefs.effort[0])) {
                PrefsEffortShow(d, NULL, model);
            }
            PrefsDrawRows(d);
        }
        prefs_model_step(&d->model);
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
    if (saved) SetStatus("MCP configuration saved. The next run discovers its tools.");
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
            else if (gRunCtl.exchange_drain) SetStatus("The stopped request is still closing; try again in a moment.");
            else ShowPreferences();
            break;
        }
        if (item == kEditMcpItem) {
            if (RunBusy()) SetStatus("Finish the current run before changing MCP Servers.");
            else if (mcp_run_draining()) SetStatus("The stopped MCP connection is still closing; try again in a moment.");
            else if (gRunCtl.exchange_drain) SetStatus("The stopped request is still closing; try again in a moment.");
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
    char line[CHAT_MODEL_CAP + AGENT_EFFORT_CAP + 16], count[24];
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
    /* The saved effort is the one every request sends. Without one the model's
     * default is in effect, which describes the configured model only while
     * the info row belongs to it. */
    FormatModelLabel(gPrefs.model, gPrefs.effort,
                     !strcmp(gRunCtl.model_info_model, gPrefs.model) ? gRunCtl.model_info.default_effort : "",
                     line, sizeof(line));
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
    if (gAgent.context_seen && gRunCtl.model_info.context_length > 0 && !strcmp(gRunCtl.model_info_model, gRunCtl.model)) {
        long long contextPercent = ((long long)gAgent.context_tokens * 100 + gRunCtl.model_info.context_length / 2) / gRunCtl.model_info.context_length;
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
    gDisplayedLimit = gRunCtl.model_info.context_length;
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
static int ToolEventJournal(void *context, const char *event, const char *json)
{
    (void)context;
    return run_ctl_tool_journal(&gRunCtl, event, json);
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
        header, text, gRunCtl.tool_events[0] ? gRunCtl.tool_events : "(none)");
    ShowMessage(NULL, block);
}
static void NewChat(void)
{
    if (RunBusy()) return;
    session_close(&gSession); agent_reset(&gAgent, session_journal, &gSession); chat_reset(&gChat);
    agent_set_instructions(NULL);
    view_image_reset();
    mcp_run_end();
    ResponseSetText("", 0); TESetText("", 0, gPromptTE); InvalRect(&gPromptRect);
    FocusSet(gPromptTE); SetStatus("Ready. What shall we investigate?");
}

/* ── Remote MCP tools in the run loop ───────────────────────────────
 * The controller (discovery, freeze, calls, retention, Stop, drain) is
 * mcp/run.c, built and tested on the host; this is its Toolbox glue and the
 * controller transitions live in run_ctl.c. */
static McpRunLoad McpHostLoad(char *bytes, size_t cap, size_t *len)
{
    FSSpec spec;
    OSErr err = McpSpec(&spec);
    if (err && err != fnfErr) return MCP_RUN_LOAD_NO_FOLDER;
    switch (mcp_store_load(&spec, bytes, cap, len)) {
    case MCP_STORE_OK: return MCP_RUN_LOAD_OK;
    case MCP_STORE_ABSENT: return MCP_RUN_LOAD_ABSENT;
    case MCP_STORE_RECOVER: return MCP_RUN_LOAD_RECOVER;
    default: return MCP_RUN_LOAD_UNREADABLE;
    }
}
static void McpHostOpenTransport(void) { EnsureOpenTransport(); }
static unsigned long McpHostTicks(void) { return (unsigned long)TickCount(); }
static const McpRunHost kMcpHost = {
    McpHostLoad, tools_workspace, McpHostOpenTransport, McpHostTicks, ToolEventJournal, &gSession
};

/* RunHost: Toolbox and live network effects for the cooperative controller. */
static void RunStatus(void *c, const char *text) { (void)c; SetStatus("%s", text); }
static void RunEnabled(void *c, int on) { (void)c; SetSendEnabled(on); }
static void RunMessage(void *c, const char *label, const char *text) { (void)c; ShowMessage(label, text); }
static void RunToolMessage(void *c, const AgentCall *call, const char *label, const char *text)
{
    (void)c; ShowToolResult(call, label, text);
}
static void RunLog(void *c, const char *text) { (void)c; LogLine(text); }
static uint32_t RunTicks(void *c) { (void)c; return (uint32_t)TickCount(); }
static int RunPrompt(void *c, char *out, size_t cap) { (void)c; return TEGetTextInto(gPromptTE, out, cap); }
static void RunClearPrompt(void *c) { (void)c; TESetText("", 0, gPromptTE); InvalRect(&gPromptRect); }
static void RunFocus(void *c) { (void)c; FocusSet(gPromptTE); }
static void RunModelChanged(void *c) { (void)c; if (gWindow) InvalRect(&gModelRect); }
static void RunWorkspaceInstructions(void *c) { (void)c; LoadWorkspaceInstructions(); }
static int RunProjectInstructions(void *c) { (void)c; return NoteProjectInstructions(); }
static int RunNetworkStart(void *c, ChatNetwork *n, const char *request, size_t len)
{
    (void)c; return network_start(n, request, len);
}
static int RunNetworkStep(void *c, ChatNetwork *n) { (void)c; return network_step(n); }
static MacTLS_State RunNetworkState(void *c, ChatNetwork *n) { (void)c; return MacTLS_GetState(n->ctx); }
static void RunNetworkPump(void *c, ChatNetwork *n) { (void)c; MacTLS_Pump(n->ctx); }
static void RunCloseContext(void *c, ChatNetwork *n) { (void)c; network_close(n); }
static void RunOpenTransport(void *c) { (void)c; InitOpenTransport(); }
static void RunCloseTransport(void *c) { (void)c; CloseOpenTransport(); }
static void RunYieldTransport(void *c) { (void)c; YieldTicks(SHERCLAWK_OT_YIELD_TICKS); }
static const RunHost kRunHost = {
    RunStatus, RunEnabled, RunMessage, RunToolMessage, RunLog, RunTicks,
    RunPrompt, RunClearPrompt, RunFocus, RunModelChanged,
    RunWorkspaceInstructions, RunProjectInstructions,
    RunNetworkStart, RunNetworkStep, RunNetworkState, RunNetworkPump,
    RunCloseContext, RunOpenTransport, RunCloseTransport, RunYieldTransport
};
static void RunInit(void)
{
    run_ctl_init(&gRunCtl, &kRunHost, NULL, &gAgent, &gSession, &gPrefs, &gNet);
}

int main(void)
{
    EventRecord event;
    /* No MaxApplZone here: growing the zone to the application limit up front
     * made DialogSelect fault in the Preferences dialog in OS 9.2.2. The
     * headroom get_environment reports adds the ungrown gap instead. */
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL); InitCursor(); MacTLS_Init();
    RunInit();
    PrefsLoad();
    ModelInfoSeedFromPrefs();
    tools_set_workspace(gPrefs.workspace);
    mcp_run_init(&kMcpHost);
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
        DrainMcpClient();
        selfbuild_drain((uint32_t)TickCount());
        UpdateHandoffControls();
        /* History changes during sends, tool results, New Chat and handoff.
         * Keep its indicator current even when the ordinary status is unchanged. */
        if (gDisplayedHistory != gAgent.used) InvalRect(&gHistoryRect);
        if (gDisplayedCost != gAgent.cost_micros ||
            gDisplayedTokens != (gAgent.context_seen ? gAgent.context_tokens : -1) ||
            gDisplayedLimit != gRunCtl.model_info.context_length) InvalRect(&gUsageRect);
    }
    LogRoundTiming("abort");
    if (gAgent.active) AbortChat("Application quit.");
    else AbandonChatContext();  /* a stopped lookup/handoff, or a pending drain */
    if (gRunCtl.exchange_drain) {
        /* The window would go stale while the loop ignores events, so hide it;
         * the drain is bounded by its own 30 s backstop. */
        HideWindow(gWindow);
        while (gRunCtl.exchange_drain) {
            WaitNextEvent(everyEvent, &event, 1, NULL);
            DrainAbandonedExchange();
        }
    }
    if (mcp_run_drain_pending()) {
        /* Same pattern: the MCP drain is bounded by the client's own connect
         * timeout and its 5 s cancellation deadline. */
        HideWindow(gWindow);
        while (mcp_run_drain_pending()) {
            WaitNextEvent(everyEvent, &event, 1, NULL);
            DrainMcpClient();
        }
    }
    if (gNet.ctx || gRunCtl.ot_open) CloseChatContext();
    selfbuild_close();
    session_close(&gSession);
    LogLine("Sherclawk session ended."); LogClose(); UIDispose();
    MacTLS_Shutdown(); return 0;
}
