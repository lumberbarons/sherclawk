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
#include <OpenTransport.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "certainly.h"
#include "http.h"
#include "network.h"
#include "agent.h"
#include "json.h"
#include "tools.h"
#include <QDOffscreen.h>
#include <Resources.h>
#include "text.h"
#include "config.h"

/* ── Menu IDs ─────────────────────────────────────────────────────── */
enum {
    kAppleMenuID = 128,
    kFileMenuID  = 129,
    kEditMenuID  = 130
};

/* ── Layout ───────────────────────────────────────────────────────── */
enum {
    kWinLeft   = 40,
    kWinTop    = 40,
    kWinWidth  = 600,
    kWinHeight = 420,

    kPad       = 10,
    kFieldH    = 20,
    kButtonW   = 64,
    kScrollW   = 15
};

/* ── UI state ─────────────────────────────────────────────────────── */
static WindowPtr     gWindow = NULL;
static ControlHandle gSendBtn = NULL, gStopBtn = NULL, gNewBtn = NULL;
static TEHandle gPromptTE = NULL;
static Rect gPromptRect, gPromptLabelRect, gStopRect, gNewRect;
static TEHandle      gModelTE = NULL;
static TEHandle      gResponseTE = NULL;
static ControlHandle gResponseScroll = NULL;
static ControlActionUPP gScrollActionUPP = NULL;
static TEHandle      gFocusedTE = NULL;

static Rect gModelLabelRect, gModelRect, gSendRect;
static Rect gStatusLabelRect, gStatusRect;
static Rect gResponseLabelRect, gResponseRect, gResponseViewRect;

static MenuHandle gAppleMenu = NULL;
static MenuHandle gFileMenu  = NULL;
static MenuHandle gEditMenu  = NULL;

static char gStatusText[256] = "Idle.";
static int  gQuit = 0;

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
static uint32_t gStartTicks;
static void FocusSet(TEHandle te);
static void SendChat(void);
static void AbortChat(const char *reason);
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

static void SetSendEnabled(int enabled)
{
    if (gSendBtn) HiliteControl(gSendBtn, enabled ? 0 : 255);
    if (gNewBtn) HiliteControl(gNewBtn, enabled ? 0 : 255);
    if (gFileMenu) {
        if (enabled) EnableItem(gFileMenu, 1);
        else DisableItem(gFileMenu, 1);
    }
    if (gStopBtn) HiliteControl(gStopBtn, enabled ? 255 : 0);
    if (!enabled && gFocusedTE != gResponseTE) FocusSet(gResponseTE);
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
        if (item == 3) return 1;   /* Quit */
        break;

    case kAppleMenuID:
        if (item == 1) {
            SetStatus("Sherclawk - native OS 9 tools, powered by Certainly.");
        }
        /* Items 2+ are desk accessories added by AppendResMenu. */
        break;

    case kEditMenuID:
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

static void ComputeLayout(void)
{
    SetRect(&gModelLabelRect, 10, 10, 48, 30);
    SetRect(&gModelRect, 54, 10, 590, 30);
    SetRect(&gStatusLabelRect, 10, 38, 56, 54);
    SetRect(&gStatusRect, 60, 38, 590, 54);
    SetRect(&gResponseLabelRect, 174, 62, 340, 78);
    SetRect(&gResponseRect, 174, 82, 590, 270);
    gResponseViewRect = gResponseRect; gResponseViewRect.right -= kScrollW;
    SetRect(&gPromptLabelRect, 10, 278, 200, 294);
    SetRect(&gPromptRect, 10, 298, 590, 380);
    SetRect(&gSendRect, 10, 390, 74, 410);
    SetRect(&gStopRect, 84, 390, 148, 410);
    SetRect(&gNewRect, 158, 390, 246, 410);
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
        Str255 title;
        PStr(title, "Sherclawk"); MoveTo(10, 74); TextFace(bold); DrawString(title); TextFace(normal);
        if (gArt) {
            PixMapHandle pixels = GetGWorldPixMap(gArt);
            if (LockPixels(pixels)) {
                CopyBits((BitMap *)*pixels, &gWindow->portBits, &art, &art, srcCopy, NULL);
                UnlockPixels(pixels);
            }
        }
        MoveTo(10, 253); PStr(title, "The consulting crustacean"); DrawString(title);
        MoveTo(10, 266); PStr(title, "Text tools: v1"); DrawString(title);
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
            const char *url = SHERCLAWK_MODEL;
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
        } else if (!gSending && gModelTE && PtInRect(local, &(*gModelTE)->viewRect)) {
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
        if (!gSending && gFocusedTE && gFocusedTE != gResponseTE) {
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
static int SessionStart(void)
{
    Str255 path;
    FSSpec spec;
    long dir;
    OSErr err;
    int attempt;
    char folder[256];
    SessionClose();
    snprintf(folder, sizeof(folder), "%sSherclawk Sessions:", SHERCLAWK_WORKSPACE);
    PStr(path, folder);
    err = FSMakeFSSpec(0, 0, path, &spec);
    if (err == fnfErr) err = FSpDirCreate(&spec, smSystemScript, &dir);
    if (err != noErr && err != dupFNErr) return -1;
    for (attempt = 0; attempt < 100; attempt++) {
        snprintf(gSessionPath, sizeof(gSessionPath), "%ss%08lx.jsonl", folder,
                 ((unsigned long)TickCount() + (unsigned long)attempt) & 0xffffffffUL);
        PStr(path, gSessionPath);
        err = FSMakeFSSpec(0, 0, path, &spec);
        if (err == noErr) continue;
        if (err != fnfErr || FSpCreate(&spec, 'ShCk', 'TEXT', smSystemScript) != noErr) return -1;
        if (FSpOpenDF(&spec, fsWrPerm, &gSessionRef)) return -1;
        gSessionOpen = 1; return 0;
    }
    return -1;
}
static int SessionWrite(const char *text)
{
    long length = (long)strlen(text), written = length;
    return !gSessionOpen || FSWrite(gSessionRef, &written, text) != noErr || written != length ? -1 : 0;
}
static int Journal(void *context, const char *event, const char *json)
{
    char prefix[100];
    (void)context;
    snprintf(prefix, sizeof(prefix), "{\"event\":\"%s\",\"message\":", event);
    if (SessionWrite(prefix) || SessionWrite(json) || SessionWrite("}\n") || FlushVol(NULL, 0) != noErr) return -1;
    return 0;
}
static void ShowMessage(const char *label, const char *text)
{
    static char display[CHAT_TRANSCRIPT_CAP];
    size_t at = strlen(gChat.transcript), len, i, lines = 0;
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
    if (at + len + strlen(label) + 8 >= sizeof(gChat.transcript) || lines > 1400) {
        strcpy(gChat.transcript, "[Earlier conversation is saved in the session file.]\r\r");
        at = strlen(gChat.transcript);
    }
    if (strlen(label) + len + 5 >= sizeof(gChat.transcript) - at) return;
    strcpy(gChat.transcript + at, label); strcat(gChat.transcript, ":\r");
    strcat(gChat.transcript, display); strcat(gChat.transcript, "\r\r");
    ResponseSetText(gChat.transcript, strlen(gChat.transcript));
    ResponseScrollTo(GetControlMaximum(gResponseScroll));
}
static void NewChat(void)
{
    if (gSending) return;
    SessionClose(); agent_reset(&gAgent, Journal, NULL); chat_reset(&gChat);
    ResponseSetText("", 0); TESetText("", 0, gPromptTE); InvalRect(&gPromptRect);
    FocusSet(gPromptTE); SetStatus("New session. Text tools in %s", SHERCLAWK_WORKSPACE);
}
static void AbortChat(const char *reason)
{
    if (gNet.ctx || gOTOpen) CloseChatContext();
    if (agent_stop(&gAgent, reason)) {
        SetStatus("Session recording failed. Start a new session before continuing.");
        SessionClose();
    } else SetStatus("%s", reason);
    gSending = 0; SetSendEnabled(1); FocusSet(gPromptTE);
    ShowMessage("Stopped", reason); LogLine("Agent stopped; completed records retained.");
}
static int StartModelRequest(void)
{
    int length;
    if (gAgent.rounds >= AGENT_TURN_MAX || gAgent.tool_count >= AGENT_TOOL_MAX) {
        AbortChat("Run limit reached. Completed results retained."); return -1;
    }
    length = agent_request(&gAgent, gRunModel, gJSON, sizeof(gJSON));
    if (length < 0) { AbortChat("Request limit reached. Start a new session."); return -1; }
    length = http_build_post("openrouter.ai", "/api/v1/chat/completions", SHERCLAWK_API_KEY,
        gJSON, (size_t)length, gNet.request, sizeof(gNet.request));
    if (length < 0) { AbortChat("Request or API key is too long or invalid."); return -1; }
    InitOpenTransport(); gOTOpen = 1;
    gStartTicks = (uint32_t)TickCount(); gSending = 1;
    if (network_start(&gNet, gNet.request, (size_t)length) < 0) { AbortChat(gNet.error); return -1; }
    SetStatus("Connecting to OpenRouter (round %d)...", gAgent.rounds + 1);
    LogLine("Agent model request started."); return 0;
}
static void SendChat(void)
{
    char prompt[CHAT_PROMPT_CAP], error[256];
    size_t i;
    if (gSending) return;
    if (!SHERCLAWK_API_KEY[0]) { SetStatus("No API key: set config.local.h and rebuild."); return; }
    if (TEGetTextInto(gModelTE, gRunModel, sizeof(gRunModel)) < 0 ||
        TEGetTextInto(gPromptTE, prompt, sizeof(prompt)) < 0) { SetStatus("Model or message is too long."); return; }
    for (i = 0; gRunModel[i]; i++) if ((unsigned char)gRunModel[i] <= 32 || (unsigned char)gRunModel[i] >= 127) {
        SetStatus("Use an OpenRouter model ID without spaces."); return;
    }
    for (i = 0; prompt[i] && (prompt[i] == ' ' || prompt[i] == '\r' || prompt[i] == '\t'); i++) {}
    if (!*gRunModel || !prompt[i]) { SetStatus("Enter a model and message first."); return; }
    if (text_to_utf8(prompt, strlen(prompt), gPending, sizeof(gPending)) < 0) { SetStatus("Could not convert message to UTF-8."); return; }
    if (!gSessionOpen && (gAgent.messages || SessionStart())) {
        SetStatus("Session file unavailable. Check %s or start a new session.", SHERCLAWK_WORKSPACE); return;
    }
    if (agent_begin(&gAgent, gPending, error, sizeof(error))) { SetStatus("%s", error); return; }
    ShowMessage("You", gPending);
    TESetText("", 0, gPromptTE); InvalRect(&gPromptRect);
    SetSendEnabled(0); StartModelRequest();
}
static void DriveChatStep(void)
{
    char error[256];
    int result;
    if (!gSending) return;
    if (gSending == 3) { StartModelRequest(); return; }
    if (gSending == 2) {
        AgentCall *call;
        if (gAgent.next == gAgent.count) { gSending = 3; return; }
        if (gAgent.tool_count >= AGENT_TOOL_MAX) { AbortChat("Tool limit reached."); return; }
        call = &gAgent.calls[gAgent.next];
        SetStatus("Running %s (%d/%d)...", call->name, gAgent.next + 1, gAgent.count);
        {
            char id[800], name[400], started[1300];
            if (json_quote(call->id, id, sizeof(id)) < 0 || json_quote(call->name, name, sizeof(name)) < 0) {
                AbortChat("Could not encode tool start."); return;
            }
            snprintf(started, sizeof(started), "{\"call_id\":%s,\"name\":%s}", id, name);
            if (Journal(NULL, "tool_started", started)) { AbortChat("Could not record tool start; no tool executed."); return; }
        }
        result = tools_execute_recorded(call, gToolResult, sizeof(gToolResult), Journal, NULL);
        if (agent_tool_result(&gAgent, gToolResult, error, sizeof(error))) { AbortChat(error); return; }
        ShowMessage(call->name, gToolResult);
        if (result) AbortChat("Mutation stopped. Inspect the result and session recovery records; do not retry automatically.");
        return;
    }
    if ((uint32_t)TickCount() - gStartTicks > 120UL * 60UL) { AbortChat("Request timed out. Completed results retained."); return; }
    result = network_step(&gNet);
    if (result < 0) { AbortChat(gNet.error); return; }
    if (!result) {
        MacTLS_State state = MacTLS_GetState(gNet.ctx);
        if (state == kMacTLS_Handshaking) SetStatus("TLS handshake...");
        else if (state == kMacTLS_Connected) SetStatus("Waiting for model (%s, %lu bytes)...", TLSVersionLabel(gNet.version), (unsigned long)gNet.received);
        return;
    }
    if (agent_response(&gAgent, gNet.body, gNet.body_len, gNet.status, error, sizeof(error))) { AbortChat(error); return; }
    CloseChatContext();
    if (*gAgent.text) ShowMessage("Sherclawk", gAgent.text);
    if (gAgent.count) {
        gSending = 2; SetStatus("Model requested %d tool(s).", gAgent.count);
    } else {
        gSending = 0; SetSendEnabled(1); FocusSet(gPromptTE);
        if (gAgent.limited) ShowMessage("Notice", "Reply incomplete: output token limit reached.");
        SetStatus("Done - %d model rounds, %d tools. Session saved.", gAgent.rounds, gAgent.tool_count);
        LogLine("Agent run completed.");
    }
}

int main(void)
{
    EventRecord event;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL); InitCursor(); MacTLS_Init();
    UIInit();
    if (!gWindow || !gModelTE || !gPromptTE || !gResponseTE ||
        !gSendBtn || !gStopBtn || !gNewBtn || !gResponseScroll || !gScrollActionUPP) {
        UIDispose(); MacTLS_Shutdown(); return 1;
    }
    chat_reset(&gChat); agent_reset(&gAgent, Journal, NULL);
    SetStatus("Text tools in %s. Inspect files or create new text.", SHERCLAWK_WORKSPACE);
    LogOpen(); LogLine("Sherclawk session started.");
    while (!gQuit) {
        WaitNextEvent(everyEvent, &event, gSending ? 1 : 10, NULL);
        SetPort(gWindow); HandleEvent(&event);
        if (gSending) DriveChatStep();
    }
    if (gNet.ctx || gOTOpen) CloseChatContext();
    if (gAgent.active) agent_stop(&gAgent, "Application quit.");
    SessionClose();
    LogLine("Sherclawk session ended."); LogClose(); UIDispose();
    MacTLS_Shutdown(); return 0;
}
