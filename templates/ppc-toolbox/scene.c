/* The scene: window updates and self-render both draw it through draw_scene. */
#include "tmpl.h"
#include <Controls.h>
#include <TextEdit.h>
WindowPtr w;
ControlHandle btn;
TEHandle edit;
Rect pr, field;
/* 1 = Clear button, 2 = text field, 0 = elsewhere. Clicks and self-test share it. */
int region_at(Point pt)
{
	ControlHandle c;
	if (FindControl(pt, w, &c) && c == btn) return 1;
	return PtInRect(pt, &field) ? 2 : 0;
}
#if SELF_RENDER
static int at(const Rect *q)
{
	Point c;
	c.h = (q->left + q->right) / 2; c.v = (q->top + q->bottom) / 2;
	return region_at(c);
}
/* Failed-check bits: click hit tests, then edits on a scratch text field. */
static int selftest(void)
{
	int bad = (at(&(*btn)->contrlRect) != 1) | (at(&field) != 2) << 1;
	TEHandle t = TENew(&field, &field);
	if (!t) return bad | 4;
	TEKey('x', t); bad |= ((*t)->teLength != 1) << 2;
	TESetText("", 0, t); bad |= ((*t)->teLength != 0) << 3;
	TEDispose(t);
	log_line("selftest", bad);
	return bad;
}
#endif
/* The one drawing routine: updateEvt calls it for the window, self-render for a GWorld. */
void draw_scene(GrafPtr p, int flags)
{
	GrafPtr save;
	Rect f = field;
	int bad = 0;
	InsetRect(&f, -3, -3);
	GetPort(&save); SetPort(p);
	(*edit)->inPort = p; (*btn)->contrlOwner = (WindowPtr)p;
	TextFont(0); TextSize(12);
#if SELF_RENDER
	if (flags & SR_SELFTEST) bad = selftest();
#endif
	EraseRect(&pr); FrameRect(&f); TEUpdate(&field, edit); Draw1Control(btn);
#if SELF_RENDER
	if (flags & SR_OVERLAY) {
		ForeColor(redColor); PenSize(2, 2);
		FrameRect(&field); FrameRect(&(*btn)->contrlRect);
	}
	if (flags & SR_SELFTEST) {
		MoveTo(8, pr.bottom - 6);
		DrawString(bad ? P("\pselftest FAIL") : P("\pselftest PASS"));
	}
#endif
	(*edit)->inPort = (GrafPtr)w; (*btn)->contrlOwner = w;
	SetPort(save);
}
