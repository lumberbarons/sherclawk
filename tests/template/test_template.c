/* Host checks for the starter's file output and self-render: runtime.log, the
 * PNG writer (decoded by an independent parser), the frame.req / frame.ready
 * protocol, multi-frame spacing and error reporting. The GWorld, File Manager
 * and Process Manager are models; real behavior is verified in the OS 9 guest. */
#include "tmpl.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILES 16
/* Evaluate outside assert() so the call still runs if NDEBUG is ever defined. */
#define MUST(e) do { int must_ = (e); assert(must_); } while (0)
typedef struct { char name[32]; int used; unsigned char *data; long len, cap; } File;
static File files[FILES];
static struct { int file; long pos; int open, write; } refs[8];
static int fail_write_after = -1, writes_ok;
static unsigned long ticks;
static int gworld_fail;

static File *find(const char *name)
{
	int i;
	for (i = 0; i < FILES; i++) if (files[i].used && !strcmp(files[i].name, name)) return &files[i];
	return NULL;
}
static void pname(const unsigned char *p, char *out)
{
	memcpy(out, p + 1, p[0]); out[p[0]] = 0;
}
OSErr FSMakeFSSpec(short v, long d, const unsigned char *name, FSSpec *s)
{
	char n[32];
	s->vRefNum = v; s->parID = d; memcpy(s->name, name, (size_t)name[0] + 1);
	pname(name, n);
	return find(n) ? noErr : fnfErr;
}
OSErr FSpCreate(const FSSpec *s, OSType creator, OSType type, short script)
{
	int i; char n[32];
	(void)creator; (void)type; (void)script;
	pname(s->name, n);
	for (i = 0; i < FILES; i++) if (!files[i].used) {
		memset(&files[i], 0, sizeof(files[i]));
		files[i].used = 1; strcpy(files[i].name, n); return noErr;
	}
	return ioErr;
}
OSErr FSpOpenDF(const FSSpec *s, short perm, short *ref)
{
	int i; char n[32]; File *f;
	pname(s->name, n);
	if (!(f = find(n))) return fnfErr;
	for (i = 0; i < 8; i++) if (!refs[i].open) {
		refs[i].open = 1; refs[i].file = (int)(f - files); refs[i].pos = 0;
		refs[i].write = perm == fsWrPerm; *ref = (short)(i + 1); return noErr;
	}
	return ioErr;
}
OSErr SetEOF(short ref, long len)
{
	assert(len == 0); files[refs[ref - 1].file].len = 0; return noErr;
}
OSErr FSWrite(short ref, long *n, const void *p)
{
	File *f = &files[refs[ref - 1].file];
	assert(refs[ref - 1].open && refs[ref - 1].write);
	if (fail_write_after >= 0 && strcmp(f->name, "runtime.log") && writes_ok++ >= fail_write_after) { *n = 0; return ioErr; }
	if (f->len + *n > f->cap) {
		f->cap = (f->len + *n) * 2; f->data = realloc(f->data, (size_t)f->cap); assert(f->data);
	}
	memcpy(f->data + f->len, p, (size_t)*n); f->len += *n; return noErr;
}
OSErr FSRead(short ref, long *n, void *p)
{
	File *f = &files[refs[ref - 1].file];
	long left = f->len - refs[ref - 1].pos;
	if (*n > left) *n = left;
	memcpy(p, f->data + refs[ref - 1].pos, (size_t)*n); refs[ref - 1].pos += *n;
	return noErr;
}
OSErr FSClose(short ref) { assert(refs[ref - 1].open); refs[ref - 1].open = 0; return noErr; }
OSErr FSpDelete(const FSSpec *s)
{
	char n[32]; File *f;
	pname(s->name, n);
	if (!(f = find(n))) return fnfErr;
	free(f->data); f->used = 0; return noErr;
}
OSErr GetCurrentProcess(ProcessSerialNumber *p) { (void)p; return noErr; }
OSErr GetProcessInformation(const ProcessSerialNumber *p, ProcessInfoRec *info)
{
	(void)p;
	info->processAppSpec->vRefNum = 1; info->processAppSpec->parID = 2;
	memcpy(info->processAppSpec->name, "\003app", 4);
	return noErr;
}
Ptr NewPtr(long n) { return malloc((size_t)n); }
void DisposePtr(Ptr p) { free(p); }
void BlockMoveData(const void *s, void *d, long n) { memmove(d, s, (size_t)n); }
unsigned long TickCount(void) { return ticks; }

typedef struct { Rect r; PixMap pm; PixMap *pmp; ColorTable ct; ColorTable *ctp; } World;
static World *current;
OSErr NewGWorld(GWorldPtr *out, short depth, const Rect *r, CTabHandle c, GDHandle d, long f)
{
	World *w; int h = r->bottom - r->top, i;
	assert(depth == 8 && !c && !d && !f);
	if (gworld_fail) return memFullErr;
	w = calloc(1, sizeof(*w)); assert(w); w->r = *r;
	w->pm.rowBytes = (short)(0x8000 | ((r->right - r->left) + 3));	/* flag bits and padding */
	w->pm.baseAddr = calloc((size_t)(w->pm.rowBytes & 0x3FFF) * (size_t)h, 1); assert(w->pm.baseAddr);
	w->ct.ctSize = 255;
	for (i = 0; i < 256; i++) {
		w->ct.ctTable[i].rgb.red = (unsigned short)(i << 8 | 1);
		w->ct.ctTable[i].rgb.green = (unsigned short)((255 - i) << 8 | 2);
		w->ct.ctTable[i].rgb.blue = (unsigned short)((i * 3 & 255) << 8 | 3);
	}
	w->ctp = &w->ct; w->pm.pmTable = &w->ctp; w->pmp = &w->pm;
	*out = (GWorldPtr)w; return noErr;
}
PixMapHandle GetGWorldPixMap(GWorldPtr g) { return &((World *)g)->pmp; }
Ptr GetPixBaseAddr(PixMapHandle pm) { return (*pm)->baseAddr; }
Boolean LockPixels(PixMapHandle pm) { (void)pm; return 1; }
void UnlockPixels(PixMapHandle pm) { (void)pm; }
void DisposeGWorld(GWorldPtr g) { World *w = (World *)g; free(w->pm.baseAddr); free(w); }
void GetGWorld(CGrafPtr *p, GDHandle *d) { *p = NULL; *d = NULL; }
void SetGWorld(CGrafPtr p, GDHandle d) { (void)d; current = (World *)p; }

static int draws, last_flags;
static int pixel(int x, int y, int frame) { return (x * 7 + y * 13 + frame * 31) & 255; }
static void draw(GrafPtr port, int flags)
{
	World *w = (World *)port;
	int x, y, stride = w->pm.rowBytes & 0x3FFF;
	assert(w == current);
	draws++; last_flags = flags;
	for (y = 0; y < w->r.bottom; y++)
		for (x = 0; x < w->r.right; x++) w->pm.baseAddr[y * stride + x] = (char)pixel(x, y, draws);
}

/* ---- independent PNG decoder ---- */
static unsigned long crc32_of(const unsigned char *p, long n)
{
	unsigned long c = 0xFFFFFFFFUL; long i; int k;
	for (i = 0; i < n; i++)
		for (c ^= p[i], k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
	return ~c & 0xFFFFFFFFUL;
}
static unsigned long be32(const unsigned char *p)
{
	return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}
/* Checks every structural rule and that pixels equal pixel(x, y, frame). */
static void check_png(const char *name, int w, int h, int frame)
{
	File *f = find(name);
	const unsigned char *p, *end;
	unsigned char *raw; long rawlen = 0; int saw_ihdr = 0, saw_plte = 0, saw_idat = 0, saw_iend = 0, i, x, y;
	assert(f && f->len > 8 && !memcmp(f->data, "\211PNG\r\n\032\n", 8));
	p = f->data + 8; end = f->data + f->len;
	raw = malloc((size_t)f->len); assert(raw);
	while (p < end) {
		unsigned long len = be32(p);
		const unsigned char *type = p + 4, *body = p + 8;
		assert(!saw_iend && body + len + 4 <= end);
		assert(be32(body + len) == crc32_of(type, (long)len + 4));
		if (!memcmp(type, "IHDR", 4)) {
			assert(!saw_ihdr && len == 13 && be32(body) == (unsigned long)w && be32(body + 4) == (unsigned long)h);
			assert(!memcmp(body + 8, "\010\003\000\000\000", 5)); saw_ihdr = 1;
		} else if (!memcmp(type, "PLTE", 4)) {
			assert(saw_ihdr && !saw_idat && len == 768);
			for (i = 0; i < 256; i++) {
				assert(body[i * 3] == i && body[i * 3 + 1] == 255 - i && body[i * 3 + 2] == (i * 3 & 255));
			}
			saw_plte = 1;
		} else if (!memcmp(type, "IDAT", 4)) {
			assert(saw_plte && !saw_idat); saw_idat = 1;
			assert(len >= 6 && body[0] == 0x78 && (body[0] * 256 + body[1]) % 31 == 0);
			{	/* stored blocks, then Adler-32 */
				const unsigned char *q = body + 2, *stop = body + len - 4; int last = 0;
				unsigned long a = 1, b = 0; long k;
				while (q < stop) {
					unsigned n; assert(!last && (q[0] & 6) == 0); last = q[0] & 1;
					n = (unsigned)(q[1] | q[2] << 8); assert((unsigned)(q[3] | q[4] << 8) == (~n & 0xFFFF));
					assert(q + 5 + n <= stop); memcpy(raw + rawlen, q + 5, n); rawlen += n; q += 5 + n;
				}
				assert(last && q == stop);
				for (k = 0; k < rawlen; k++) { a = (a + raw[k]) % 65521UL; b = (b + a) % 65521UL; }
				assert(be32(stop) == ((b << 16) | a));
			}
		} else if (!memcmp(type, "IEND", 4)) {
			assert(saw_idat && len == 0); saw_iend = 1;
		} else assert(!"unexpected chunk");
		p = body + len + 4;
	}
	assert(saw_iend && p == end && rawlen == (long)h * (w + 1));
	for (y = 0; y < h; y++) {
		assert(raw[y * (w + 1)] == 0);	/* filter: none */
		for (x = 0; x < w; x++) assert(raw[y * (w + 1) + 1 + x] == pixel(x, y, frame));
	}
	free(raw);
}

static void clean(void)
{
	static const char *const names[] = {"frame1.png", "frame2.png", "frame3.png", "frame8.png",
	                                    "frame.ready", "frame.req"};
	int i; File *f;
	for (i = 0; i < 6; i++) if ((f = find(names[i]))) { free(f->data); f->used = 0; }
	fail_write_after = -1; writes_ok = 0; gworld_fail = 0; draws = 0; last_flags = -1;
}
static void put_file(const char *name, const char *text)
{
	FSSpec s; unsigned char pn[32]; long n = (long)strlen(text); short ref;
	pn[0] = (unsigned char)strlen(name); memcpy(pn + 1, name, pn[0]);
	MUST(FSMakeFSSpec(1, 2, pn, &s) == fnfErr);
	MUST(!FSpCreate(&s, 0, 0, 0));
	MUST(!FSpOpenDF(&s, fsWrPerm, &ref));
	MUST(!FSWrite(ref, &n, text));
	MUST(!FSClose(ref));
}
static int text_is(const char *name, const char *want)
{
	File *f = find(name);
	return f && f->len == (long)strlen(want) && !memcmp(f->data, want, (size_t)f->len);
}
static int has(const char *name, const char *needle)
{
	File *f = find(name);
	char *copy;
	int found;
	if (!f) return 0;
	copy = malloc((size_t)f->len + 1); assert(copy); memcpy(copy, f->data, (size_t)f->len); copy[f->len] = 0;
	found = strstr(copy, needle) != NULL; free(copy); return found;
}
static void settle(const Rect *r)
{
	int guard;
	for (guard = 0; guard < 80; guard++) { ticks += 6; sr_poll(draw, r); }
}
/* One accepted request: marker text must match exactly. */
static void expect_ok(long seq, int frames, const Rect *r, int flags)
{
	char want[160];
	sprintf(want, "seq=%ld status=ok frames=%d w=%d h=%d file=frame1.png flags=%d\r",
	        seq, frames, r->right - r->left, r->bottom - r->top, flags);
	assert(text_is("frame.ready", want));
}

int main(void)
{
	Rect box = {0, 0, 150, 390};
	Rect sizes[] = {{0, 0, 1, 1}, {0, 0, 3, 255}, {0, 0, 3, 256}, {0, 0, 2, 600}};
	const char *requests[] = {"frames=9", "frames=1", "frames=8", "nonsense"};
	const int frames[] = {1, 1, 8, 1};
	FSSpec spec;
	char line[128];
	long seq = 0;
	int i, base;

	assert(app_file("runtime.log", &spec) == paramErr);	/* before log_start */
	assert(log_start() == 1 && find("runtime.log"));
	assert(app_file("a-name-longer-than-thirty-one-bytes.txt", &spec) == paramErr);
	log_line("startup", 0);
	assert(text_is("runtime.log", "startup code=0\r"));

	/* sr_ask: invalidates a stale marker; one frame renders at once. */
	put_file("frame.ready", "stale");
	ticks = 100; sr_ask(1, 0); seq++;
	assert(!find("frame.ready"));
	sr_poll(draw, &box);
	assert(draws == 1 && last_flags == 0);
	check_png("frame1.png", 390, 150, 1);
	expect_ok(seq, 1, &box, 0);
	assert(has("runtime.log", "frame code=1\r"));
	if (getenv("TEMPLATE_PNG_OUT")) {	/* optional: inspect the image with a real decoder */
		File *png = find("frame1.png");
		FILE *out = fopen(getenv("TEMPLATE_PNG_OUT"), "wb");
		size_t wrote;
		assert(out);
		wrote = fwrite(png->data, 1, (size_t)png->len, out);
		assert(wrote == (size_t)png->len);
		fclose(out);
	}
	sr_poll(draw, &box);	/* nothing queued, no request: nothing happens */
	assert(draws == 1);

	/* A request file is polled twice a second, consumed, and spaced by frame. */
	put_file("frame.req", "overlay selftest frames=3");
	ticks += 10; sr_poll(draw, &box);
	assert(find("frame.req") && draws == 1);	/* inside the 30-tick poll interval */
	ticks += 30; base = draws; seq++;
	sr_poll(draw, &box);
	assert(!find("frame.req") && draws == base + 1 && !find("frame.ready"));
	ticks += 5; sr_poll(draw, &box);
	assert(draws == base + 1);	/* spacing: next frame is six ticks later */
	ticks += 1; sr_poll(draw, &box);
	assert(draws == base + 2 && !find("frame.ready"));
	sr_ask(1, SR_OVERLAY);	/* ignored while a request is running */
	settle(&box);
	assert(draws == base + 3 && last_flags == (SR_OVERLAY | SR_SELFTEST));
	for (i = 1; i <= 3; i++) {
		char name[32]; sprintf(name, "frame%d.png", i);
		check_png(name, 390, 150, base + i);
	}
	expect_ok(seq, 3, &box, SR_OVERLAY | SR_SELFTEST);

	/* Request parsing: frame counts are clamped to 1..8, unknown text is ignored. */
	for (i = 0; i < 4; i++) {
		clean(); put_file("frame.req", requests[i]);
		ticks += 40; seq++;
		settle(&box);
		assert(draws == frames[i] && !find("frame.req"));
		sprintf(line, "seq=%ld status=ok frames=%d ", seq, frames[i]);
		assert(has("frame.ready", line));
		if (frames[i] == 8) check_png("frame8.png", 390, 150, 8);
	}

	/* Image sizes around the 8-bit length boundaries of a stored block. */
	for (i = 0; i < 4; i++) {
		clean(); sr_ask(1, 0); seq++;
		sr_poll(draw, &sizes[i]);
		check_png("frame1.png", sizes[i].right, sizes[i].bottom, 1);
		expect_ok(seq, 1, &sizes[i], 0);
	}

	/* Failures are reported through the marker and the log, never silently. */
	clean(); gworld_fail = 1; sr_ask(2, 0); seq++;
	settle(&box);
	sprintf(line, "seq=%ld status=error code=-108\r", seq);
	assert(text_is("frame.ready", line) && !find("frame1.png") && draws == 0);
	assert(has("runtime.log", "frame_error code=-108\r"));
	clean(); fail_write_after = 10; sr_ask(1, 0); seq++;
	sr_poll(draw, &box);
	assert(!find("frame.ready"));	/* no marker without a complete image */
	assert(has("runtime.log", "frame_error code=-36\r"));
	clean(); sr_ask(1, 0); seq++; sr_poll(draw, &box);	/* recovers on the next request */
	check_png("frame1.png", 390, 150, 1); expect_ok(seq, 1, &box, 0);

	/* The log stops at 4096 bytes and ignores later entries. */
	for (i = 0; i < 400; i++) log_line("padding-padding-padding", i);
	assert(find("runtime.log")->len <= 4096 && find("runtime.log")->len > 4000);
	log_line("late-entry", 1);
	assert(!has("runtime.log", "late-entry"));
	log_stop();
	log_line("after-stop", 1);
	assert(!has("runtime.log", "after-stop"));
	puts("PASS template: runtime log, PNG structure/CRC/Adler/pixels, request protocol, frame spacing and failure reporting");
	return 0;
}
