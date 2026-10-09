/* Files beside the executable: runtime.log and small text files. */
#include "tmpl.h"
#include <Script.h>
#include <Processes.h>
#include <stdio.h>
#include <string.h>
static short logFD = -1;
static long bytes;
static FSSpec home;
static int have;
/* Resolve a file beside the executable; fnfErr still yields a usable spec. */
int app_file(const char *name, FSSpec *spec)
{
	unsigned char pn[32];
	size_t n = strlen(name);
	if (!have || n > 31) return paramErr;
	pn[0] = (unsigned char)n; memcpy(pn + 1, name, n);
	return FSMakeFSSpec(home.vRefNum, home.parID, pn, spec);
}
OSErr open_out(const char *name, OSType type, short *ref)
{
	FSSpec s;
	OSErr e = app_file(name, &s);
	if (e == fnfErr) e = FSpCreate(&s, 'ttxt', type, smSystemScript);
	if (!e) e = FSpOpenDF(&s, fsWrPerm, ref);
	if (!e && SetEOF(*ref, 0)) { FSClose(*ref); e = ioErr; }
	return e;
}
int log_start(void)
{
	ProcessSerialNumber psn;
	ProcessInfoRec info = {0};
	info.processInfoLength = sizeof(info);
	info.processAppSpec = &home;
	if (GetCurrentProcess(&psn) || GetProcessInformation(&psn, &info)) return 0;
	have = 1;
	if (open_out("runtime.log", 'TEXT', &logFD)) logFD = -1;
	return logFD != -1;
}
void log_line(const char *name, long code)
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
void log_stop(void)
{
	if (logFD != -1) FSClose(logFD);
	logFD = -1;
}
OSErr text_file(const char *name, const char *text, long n)
{
	short ref;
	long count = n;
	OSErr e = open_out(name, 'TEXT', &ref);
	if (e) return e;
	e = FSWrite(ref, &count, text);
	FSClose(ref);
	if (e) {	/* never leave a partial file where a marker is expected */
		FSSpec s;
		if (!app_file(name, &s)) FSpDelete(&s);
	}
	return e;
}
