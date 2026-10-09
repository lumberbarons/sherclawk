/* Self-render: draws the scene into an 8-bit GWorld, saves it as frameN.png
 * and then writes frame.ready. A request is a new frame.req file naming
 * options, e.g. "overlay selftest frames=3". Nothing here touches the window,
 * so it works while the app is backgrounded and fully covered. */
#include "tmpl.h"
#if SELF_RENDER
#include <QDOffscreen.h>
#include <Events.h>
#include <stdio.h>
#include <string.h>
static OSErr bad;
static unsigned long due, polled;
static long seq, left, total;
static int mode;
static Rect box;
static OSErr render(DrawProc draw, long index)
{
	GWorldPtr gw;
	PixMapHandle pm;
	CGrafPtr port;
	GDHandle dev;
	char name[32];
	OSErr e;
	if (NewGWorld(&gw, 8, &box, 0, 0, 0)) return memFullErr;
	pm = GetGWorldPixMap(gw);
	LockPixels(pm);
	GetGWorld(&port, &dev);
	SetGWorld(gw, 0);
	draw((GrafPtr)gw, mode);
	SetGWorld(port, dev);
	sprintf(name, "frame%ld.png", index);
	e = png_file(name, GetPixBaseAddr(pm), (*pm)->rowBytes & 0x3FFF,
	             box.right - box.left, box.bottom - box.top, (*pm)->pmTable);
	UnlockPixels(pm); DisposeGWorld(gw);
	return e;
}
/* Written last: its presence means the newest request finished. */
static void marker(void)
{
	char t[128];
	long n = bad ? sprintf(t, "seq=%ld status=error code=%d\r", seq, bad)
	             : sprintf(t, "seq=%ld status=ok frames=%ld w=%d h=%d file=frame1.png flags=%d\r",
	                       seq, total, box.right - box.left, box.bottom - box.top, mode);
	text_file("frame.ready", t, n);
}
void sr_ask(int frames, int flags)
{
	FSSpec s;
	if (left) return;
	if (!app_file("frame.ready", &s)) FSpDelete(&s);
	total = left = frames; mode = flags; bad = 0; due = 0; seq++;
}
void sr_poll(DrawProc draw, const Rect *scene)
{
	FSSpec s;
	char t[48];
	long n = 47;
	short f;
	const char *p;
	unsigned long now = TickCount();
	if (!left && now - polled >= 30) {	/* look for a request twice a second */
		polled = now;
		if (!app_file("frame.req", &s) && !FSpOpenDF(&s, fsRdPerm, &f)) {
			FSRead(f, &n, t); FSClose(f); FSpDelete(&s); t[n] = 0;
			p = strstr(t, "frames=");
			sr_ask(p && p[7] > '1' && p[7] <= '8' ? p[7] - '0' : 1,
			       (strstr(t, "overlay") ? SR_OVERLAY : 0) |
			       (strstr(t, "selftest") ? SR_SELFTEST : 0));
		}
	}
	if (!left || now < due) return;
	box = *scene;
	n = render(draw, total - left + 1);
	if (n && !bad) bad = (OSErr)n;
	due = now + 6;
	if (--left) return;
	marker();
	log_line(bad ? "frame_error" : "frame", bad ? bad : seq);
}
#endif
