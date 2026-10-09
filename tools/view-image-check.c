/* Native view_image acceptance: the real File Manager serves a valid 1x1 PNG,
 * a PNG at the size cap read in bounded steps, an over-cap file, a non-PNG
 * file and the vision gate. Fixtures stay in Retro68:Sherclawk View <ticks>:
 * for inspection. The log is Retro68:SherclawkViewImageCheck.log. */
#include "view_image.h"
#include "json.h"
#include "config.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Files.h>
#include <Script.h>
#include <Memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static FILE *logfile;
static char result[AGENT_RESULT_CAP];
static int failures;
static const unsigned char tiny[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x64, 0x60, 0xf8, 0x5f,
    0x0f, 0x00, 0x02, 0x87, 0x01, 0x80, 0xeb, 0x47, 0xba, 0x92, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
};
static void check(int ok, const char *name)
{
    fprintf(logfile, "%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
    fflush(logfile);
}
static OSErr spec_for(const char *relative, FSSpec *spec)
{
    char full[256];
    Str255 name;
    size_t n;
    if (snprintf(full, sizeof(full), "%s%s", SHERCLAWK_WORKSPACE, relative) >= (int)sizeof(full)) return paramErr;
    n = strlen(full);
    name[0] = (unsigned char)n; memcpy(name + 1, full, n);
    return FSMakeFSSpec(0, 0, name, spec);
}
static OSErr write_file(const char *relative, ResType type, const unsigned char *bytes, long length)
{
    FSSpec spec;
    short ref;
    long written = length;
    OSErr err = spec_for(relative, &spec);
    if (err != fnfErr) return err ? err : dupFNErr;
    err = FSpCreate(&spec, 'ShCk', type, smSystemScript);
    if (!err) err = FSpOpenDF(&spec, fsWrPerm, &ref);
    if (err) return err;
    err = FSWrite(ref, &written, bytes);
    if (FSClose(ref) && !err) err = ioErr;
    if (!err && written != length) err = ioErr;
    return err ? err : FlushVol(NULL, spec.vRefNum);
}
/* Run one view_image call to its result through the bounded steps. */
static int view(const char *path, int vision, int *steps)
{
    AgentCall call;
    int status;
    memset(&call, 0, sizeof(call)); strcpy(call.id, "native-view"); strcpy(call.name, "view_image");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\"}", path);
    view_image_set_vision(vision);
    *steps = 0;
    status = view_image_begin(&call, result, sizeof(result), NULL, NULL, (uint32_t)TickCount());
    while (status == 2 && *steps < 64) { status = view_image_step(result, sizeof(result), (uint32_t)TickCount(), 0); (*steps)++; }
    fprintf(logfile, "view_image %s vision=%d steps=%d %s\n", path, vision, *steps, result); fflush(logfile);
    return status;
}
int main(void)
{
    static unsigned char big[AGENT_IMAGE_CAP + 1];
    char folder[80], path[160], expected[32];
    const AgentImage *image;
    int steps, i;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL); InitCursor();
    logfile = fopen(SHERCLAWK_WORKSPACE "SherclawkViewImageCheck.log", "w");
    if (!logfile) return 1;
    snprintf(folder, sizeof(folder), "Sherclawk View %08lx", (unsigned long)TickCount());
    {
        FSSpec spec;
        long dir;
        check(spec_for(folder, &spec) == fnfErr && !FSpDirCreate(&spec, smSystemScript, &dir), "fixture folder");
    }
    /* A structurally valid PNG of exactly the cap and one byte more: the
     * signature, IHDR and IEND trailer are real; the middle is filler. */
    for (i = 0; i < (int)sizeof(big); i++) big[i] = (unsigned char)(i * 31 + 7);
    memcpy(big, tiny, 33);
    memcpy(big + AGENT_IMAGE_CAP - 12, tiny + sizeof(tiny) - 12, 12);
    snprintf(path, sizeof(path), "%s:tiny.png", folder);
    check(!write_file(path, 'PNGf', tiny, (long)sizeof(tiny)), "tiny png written");
    snprintf(path, sizeof(path), "%s:cap.png", folder);
    check(!write_file(path, 'PNGf', big, AGENT_IMAGE_CAP), "png at the cap written");
    snprintf(path, sizeof(path), "%s:over.png", folder);
    check(!write_file(path, 'PNGf', big, AGENT_IMAGE_CAP + 1), "png over the cap written");
    snprintf(path, sizeof(path), "%s:note.png", folder);
    check(!write_file(path, 'TEXT', (const unsigned char *)"this is not a png, but long enough to pass the length check", 59), "text fixture written");

    snprintf(path, sizeof(path), "%s:tiny.png", folder);
    snprintf(expected, sizeof(expected), "\"bytes\":%lu,", (unsigned long)sizeof(tiny));
    check(!view(path, AGENT_VISION_YES, &steps) && steps == 1 && strstr(result, "\"width\":1") &&
        strstr(result, "\"height\":1") && strstr(result, expected), "tiny png viewed in one step");
    image = view_image_take();
    check(image && image->length == sizeof(tiny) && !memcmp(image->data, tiny, sizeof(tiny)) && !view_image_take(), "tiny png bytes handed over once");
    check(!view(path, AGENT_VISION_NO, &steps) && strstr(result, "VISION_UNSUPPORTED") && !view_image_take(), "model without images refused");
    check(!view(path, AGENT_VISION_UNKNOWN, &steps) && strstr(result, "VISION_UNKNOWN") && !view_image_take(), "unknown model refused");
    snprintf(path, sizeof(path), "%s:cap.png", folder);
    check(!view(path, AGENT_VISION_YES, &steps) && steps == 8 && strstr(result, "\"bytes\":131072"), "png at the cap read in bounded steps");
    image = view_image_take();
    check(image && image->length == AGENT_IMAGE_CAP && !memcmp(image->data, big, AGENT_IMAGE_CAP), "png at the cap read back exactly");
    snprintf(path, sizeof(path), "%s:over.png", folder);
    check(!view(path, AGENT_VISION_YES, &steps) && strstr(result, "TOO_LARGE") && strstr(result, "exceeds 131072 bytes") && !view_image_take(), "png over the cap refused");
    snprintf(path, sizeof(path), "%s:note.png", folder);
    check(!view(path, AGENT_VISION_YES, &steps) && strstr(result, "NOT_PNG"), "non-png refused");
    check(!view("definitely:missing.png", AGENT_VISION_YES, &steps) && strstr(result, "\"code\":\"FILE\""), "missing file refused");
    fprintf(logfile, "%s %d failures\n", failures ? "FAILED" : "PASSED", failures);
    fclose(logfile);
    return failures ? 1 : 0;
}
