#include <Types.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Controls.h>
#include <ControlDefinitions.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Events.h>
#include <Files.h>
#include <Script.h>
#include <Processes.h>
#include <stdio.h>
#define P(s) (ConstStr255Param)(s)
QDGlobals qd;
static short logFD = -1;
static long bytes;
static void start_log(void)
{
	ProcessSerialNumber psn;
	ProcessInfoRec info = {0};
	FSSpec app, file;
	OSErr err;
	info.processInfoLength = sizeof(info);
	info.processAppSpec = &app;
	if (GetCurrentProcess(&psn) || GetProcessInformation(&psn, &info)) return;
	err = FSMakeFSSpec(app.vRefNum, app.parID, P("\pruntime.log"), &file);
	if (err == fnfErr) err = FSpCreate(&file, 'ttxt', 'TEXT', smSystemScript);
	if (err || FSpOpenDF(&file, fsWrPerm, &logFD)) { logFD = -1; return; }
	if (SetEOF(logFD, 0)) { FSClose(logFD); logFD = -1; }
}
static void log_line(const char *name, long code)
{
	char line[96];
	long count, want;
	if (logFD == -1) return;
	want = sprintf(line, "%.64s code=%ld\r", name, code);
	if (bytes + want > 4096) return;
	count = want;
	if (FSWrite(logFD, &count, line) || count != want) {
		FSClose(logFD); logFD = -1; return;
	}
	bytes += count;
}
int main(void)
{
	WindowPtr w, hit;
	ControlHandle btn, ctl;
	TEHandle edit;
	EventRecord ev;
	Rect r, field, frame;
	Point local;
	short part;
	char key;
	int done = 0, active = 1;
	InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
	TEInit(); InitCursor();
	start_log(); log_line("startup", 0);
	SetRect(&r, 80, 60, 470, 210);
	w = NewWindow(0L, &r, P("\pTemplate"), 1,
			   documentProc, (WindowPtr)-1L, 1, 0L);
	if (!w) { log_line("NewWindow", MemError()); goto finish; }
	SetPort(w); TextFont(0); TextSize(12);
	SetRect(&field, 18, 42, 370, 66);
	frame = field; InsetRect(&frame, -3, -3);
	edit = TENew(&field, &field);
	if (!edit) log_line("TENew", MemError());
	SetRect(&r, 290, 90, 370, 110);
	btn = NewControl(w, &r, P("\pClear"), 1, 0, 0, 1, pushButProc, 0);
	if (!btn) log_line("NewControl", MemError());
	if (!edit || !btn) {
		if (edit) TEDispose(edit);
		DisposeWindow(w); goto finish;
	}
	if (logFD == -1) SetWTitle(w, P("\pLogging unavailable"));
	TEAutoView(1, edit); TEActivate(edit);
	while (!done) {
		SetPort(w);
		if (WaitNextEvent(everyEvent, &ev, 6L, 0L)) switch (ev.what) {
		case updateEvt:
			if ((WindowPtr)ev.message != w) break;
			BeginUpdate(w); EraseRect(&w->portRect);
			FrameRect(&frame); TEUpdate(&field, edit); DrawControls(w);
			EndUpdate(w); break;
		case mouseDown:
			part = FindWindow(ev.where, &hit);
			if (hit != w) break;
			if (part == inGoAway && TrackGoAway(w, ev.where)) done = 1;
			else if (part == inDrag) DragWindow(w, ev.where, &qd.screenBits.bounds);
			else if (part == inContent) {
				local = ev.where; GlobalToLocal(&local);
				if (FindControl(local, w, &ctl) && ctl == btn) {
					if (TrackControl(btn, local, 0L)) {
						TESetText("", 0, edit); InvalRect(&field);
						log_line("Clear", 0);
					}
				} else if (PtInRect(local, &field))
					TEClick(local, (ev.modifiers & shiftKey) != 0, edit);
			}
			break;
		case activateEvt:
		case osEvt:
			if (ev.what == activateEvt && (WindowPtr)ev.message != w) break;
			if (ev.what == osEvt && (ev.message >> 24) != suspendResumeMessage) break;
			active = ev.what == osEvt ? (ev.message & resumeFlag) : (ev.modifiers & activeFlag);
			if (active) TEActivate(edit); else TEDeactivate(edit);
			break;
		case keyDown:
		case autoKey:
			if (!active) break;
			key = ev.message & charCodeMask;
			if (ev.modifiers & cmdKey) {
				if (key == 'q') done = 1;
			} else if (key == 8 || (key >= 0x1c && key <= 0x1f) ||
			           (*edit)->teLength - ((*edit)->selEnd - (*edit)->selStart) < 255)
				TEKey(key, edit);
			break;
		}
		if (active) TEIdle(edit);
	}
	TEDeactivate(edit); TEDispose(edit); DisposeWindow(w);
	log_line("quit", 0);
finish:
	if (logFD != -1) FSClose(logFD);
	return done ? 0 : 1;
}
