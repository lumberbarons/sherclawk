/* view_image reads one workspace PNG into a fixed buffer, a bounded slice per
 * event-loop turn, and hands it to the agent for the next request. The bytes
 * go to the provider unchanged: only the PNG signature, header and trailer are
 * checked here, never decoded. The file is re-checked every step so a file
 * rewritten by a running application cannot be sent half old, half new. */
#include "view_image.h"
#include "json.h"
#include <Files.h>
#include <stdio.h>
#include <string.h>

#define STR_(x) #x
#define STR(x) STR_(x)
#define STEP_BYTES 16384L
#define HEADER_BYTES 33
#define TRAILER_BYTES 12
#define DIMENSION_MAX 8192UL
#define DEADLINE_TICKS (60UL * 60UL)

static unsigned char buffer[AGENT_IMAGE_CAP];
static AgentImage image;
static FSSpec spec;
static CInfoPBRec seen;
static long offset;
static uint32_t started;
static int vision, active, waiting;

void view_image_set_vision(int value) { vision = value; }
void view_image_reset(void) { active = waiting = 0; memset(&image, 0, sizeof(image)); }
const AgentImage *view_image_held(void) { return image.length ? &image : NULL; }
const AgentImage *view_image_take(void)
{
    if (!waiting) return NULL;
    waiting = 0; return &image;
}

static int fail(char *out, size_t cap, const char *code, const char *message, int native)
{
    char quoted[512];
    if (json_quote(message, quoted, sizeof(quoted)) < 0) strcpy(quoted, "\"view_image failed\"");
    snprintf(out, cap, "{\"status\":\"error\",\"code\":\"%s\",\"message\":%s,\"os_error\":%d}", code, quoted, native);
    return 0;
}
/* A failure after the buffer was started: the image is no longer usable. */
static int abandon(char *out, size_t cap, const char *code, const char *message, int native)
{
    active = 0; memset(&image, 0, sizeof(image));
    return fail(out, cap, code, message, native);
}
static OSErr catalog(const FSSpec *s, CInfoPBRec *pb)
{
    FSSpec copy = *s;
    memset(pb, 0, sizeof(*pb)); pb->hFileInfo.ioNamePtr = copy.name;
    pb->hFileInfo.ioVRefNum = copy.vRefNum; pb->hFileInfo.ioDirID = copy.parID;
    return PBGetCatInfoSync(pb);
}
static int same_file(const CInfoPBRec *a, const CInfoPBRec *b)
{
    return a->hFileInfo.ioDirID == b->hFileInfo.ioDirID && a->hFileInfo.ioFlMdDat == b->hFileInfo.ioFlMdDat &&
        a->hFileInfo.ioFlLgLen == b->hFileInfo.ioFlLgLen && a->hFileInfo.ioFlRLgLen == b->hFileInfo.ioFlRLgLen;
}
static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3];
}
static int read_at(long position, long count, void *into)
{
    short ref;
    long got = count;
    OSErr e = FSpOpenDF(&spec, fsRdPerm, &ref), c;
    if (e) return e;
    e = SetFPos(ref, fsFromStart, position);
    if (!e) e = FSRead(ref, &got, into);
    c = FSClose(ref);
    if (e) return e;
    if (c) return c;
    return got == count ? 0 : ioErr;
}
int view_image_begin(const AgentCall *call, char *out, size_t cap, AgentJournal journal, void *context, uint32_t now)
{
    static const unsigned char signature[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    JsonToken t[8];
    char key[16], path[AGENT_IMAGE_PATH_CAP];
    unsigned char head[HEADER_BYTES];
    unsigned long width, height;
    OSErr err;
    (void)journal; (void)context;
    if (active || waiting)
        return fail(out, cap, "ONE_IMAGE_PER_ROUND", "One image can be attached per round. Call view_image again after the model's next reply.", 0);
    view_image_reset();
    if (json_parse(call->arguments, strlen(call->arguments), t, 8) != 3 || t[0].type != JSON_OBJECT ||
        json_string(call->arguments, t, 1, key, sizeof(key)) < 0 || strcmp(key, "path") ||
        json_string(call->arguments, t, 2, path, sizeof(path)) <= 0)
        return fail(out, cap, "ARGUMENTS", "Expected exactly one field: path.", 0);
    if (vision == AGENT_VISION_NO)
        return fail(out, cap, "VISION_UNSUPPORTED", "The selected model does not accept images, so it cannot see this file. Say that you have not looked at it.", 0);
    if (vision != AGENT_VISION_YES)
        return fail(out, cap, "VISION_UNKNOWN", "Image support could not be confirmed for the selected model, so no image was sent. Say that you have not looked at it.", 0);
    err = tools_resolve(path, &spec);
    if (!err) err = catalog(&spec, &seen);
    if (err) return fail(out, cap, "FILE", "Cannot resolve the workspace file.", err);
    if ((seen.hFileInfo.ioFlAttrib & 16) || seen.hFileInfo.ioFlFndrInfo.fdType == 'alis' ||
        (seen.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000))
        return fail(out, cap, "NOT_PNG", "Only a plain PNG file is supported; folders and aliases are refused.", 0);
    if (seen.hFileInfo.ioFlLgLen > AGENT_IMAGE_CAP)
        return fail(out, cap, "TOO_LARGE", "The PNG exceeds " STR(AGENT_IMAGE_CAP) " bytes. Render or export a smaller image.", 0);
    if (seen.hFileInfo.ioFlLgLen < HEADER_BYTES + TRAILER_BYTES)
        return fail(out, cap, "NOT_PNG", "The file is too short to be a PNG.", 0);
    err = read_at(0, HEADER_BYTES, head);
    if (err) return fail(out, cap, "READ", "Cannot read the PNG header.", err);
    width = be32(head + 16); height = be32(head + 20);
    if (memcmp(head, signature, sizeof(signature)) || be32(head + 8) != 13 || memcmp(head + 12, "IHDR", 4))
        return fail(out, cap, "NOT_PNG", "The data fork does not start with a PNG signature and IHDR chunk.", 0);
    if (!width || !height || width > DIMENSION_MAX || height > DIMENSION_MAX)
        return fail(out, cap, "NOT_PNG", "PNG dimensions must be 1 to 8192 pixels each way.", 0);
    memcpy(image.path, path, strlen(path) + 1);
    image.data = buffer; image.length = (size_t)seen.hFileInfo.ioFlLgLen;
    image.width = (long)width; image.height = (long)height;
    offset = 0; started = now; active = 1;
    return 2;
}
int view_image_step(char *out, size_t cap, uint32_t now, int stop)
{
    CInfoPBRec current;
    char quoted[1200];
    long want;
    OSErr err;
    if (!active) return fail(out, cap, "NO_ACTIVE_VIEW", "No image is being read.", 0);
    if (stop) return abandon(out, cap, "STOPPED", "Stopped before the image was read; nothing was attached.", 0);
    if ((uint32_t)(now - started) >= DEADLINE_TICKS) return abandon(out, cap, "TIMEOUT", "Reading the image took too long; nothing was attached.", 0);
    err = catalog(&spec, &current);
    if (err || !same_file(&seen, &current)) return abandon(out, cap, "CHANGED", "The file changed while it was read; call view_image again.", err);
    want = (long)image.length - offset;
    if (want > STEP_BYTES) want = STEP_BYTES;
    err = read_at(offset, want, buffer + offset);
    if (err) return abandon(out, cap, "READ", "Cannot read the PNG.", err);
    offset += want;
    if (offset < (long)image.length) return 2;
    err = catalog(&spec, &current);
    if (err || !same_file(&seen, &current)) return abandon(out, cap, "CHANGED", "The file changed while it was read; call view_image again.", err);
    /* A PNG that is still being written has no IEND chunk yet. */
    if (memcmp(buffer + image.length - TRAILER_BYTES, "\0\0\0\0IEND\xAE\x42\x60\x82", TRAILER_BYTES))
        return abandon(out, cap, "NOT_PNG", "The PNG has no IEND trailer: it is truncated or still being written.", 0);
    if (json_quote(image.path, quoted, sizeof(quoted)) < 0) return abandon(out, cap, "ARGUMENTS", "The path cannot be encoded.", 0);
    active = 0; waiting = 1;
    snprintf(out, cap, "{\"status\":\"ok\",\"path\":%s,\"width\":%ld,\"height\":%ld,\"bytes\":%lu,"
        "\"attached\":\"The pixels follow in the next user message; look at them then.\"}",
        quoted, image.width, image.height, (unsigned long)image.length);
    return 0;
}
