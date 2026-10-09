#include "tmpl.h"
#include <Fonts.h>
#include <Windows.h>
#include <Controls.h>
#include <ControlDefinitions.h>
#include <Menus.h>
#include <Events.h>
QDGlobals qd;
int main(void)
{
	WindowPtr hit;
	EventRecord ev;
	Rect r;
	Point local;
	short part;
	char key;
	int done = 0, active = 1, first = 1, ok;
	InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
	TEInit(); InitCursor();
	ok = log_start(); log_line("startup", 0);
	SetRect(&r, 80, 60, 470, 210);
	w = NewWindow(0L, &r, P("\pTemplate"), 1,
			   documentProc, (WindowPtr)-1L, 1, 0L);
	if (!w) { log_line("NewWindow", MemError()); goto finish; }
	if (!ok) SetWTitle(w, P("\pLogging unavailable"));
	SetPort(w); TextFont(0); TextSize(12); pr = w->portRect;
	SetRect(&field, 18, 42, 370, 66);
	edit = TENew(&field, &field);
	if (!edit) log_line("TENew", MemError());
	SetRect(&r, 290, 90, 370, 110);
	btn = NewControl(w, &r, P("\pClear"), 1, 0, 0, 1, pushButProc, 0);
	if (!btn) log_line("NewControl", MemError());
	if (!edit || !btn) {
		if (edit) TEDispose(edit);
		DisposeWindow(w); goto finish;
	}
	TEAutoView(1, edit); TEActivate(edit);
	while (!done) {
		SetPort(w);
		if (WaitNextEvent(everyEvent, &ev, 6L, 0L)) switch (ev.what) {
		case updateEvt:
			if ((WindowPtr)ev.message != w) break;
			BeginUpdate(w); draw_scene((GrafPtr)w, 0); EndUpdate(w);
			if (first) { first = 0; sr_ask(1, 0); }
			break;
		case mouseDown:
			part = FindWindow(ev.where, &hit);
			if (hit != w) break;
			if (part == inGoAway && TrackGoAway(w, ev.where)) done = 1;
			else if (part == inDrag) DragWindow(w, ev.where, &qd.screenBits.bounds);
			else if (part == inContent) {
				local = ev.where; GlobalToLocal(&local);
				part = region_at(local);
				if (part == 1) {
					if (TrackControl(btn, local, 0L)) {
						TESetText("", 0, edit); InvalRect(&field);
						log_line("Clear", 0);
					}
				} else if (part == 2)
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
		sr_poll(draw_scene, &pr);
	}
	TEDeactivate(edit); TEDispose(edit); DisposeWindow(w);
	log_line("quit", 0);
finish:
	log_stop();
	return done ? 0 : 1;
}
