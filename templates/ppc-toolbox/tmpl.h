/* Shared declarations. Set SELF_RENDER to 0 to ship without frame capture. */
#define SELF_RENDER 1
#include <Types.h>
#include <Quickdraw.h>
#include <Files.h>
#include <Windows.h>
#include <Controls.h>
#include <TextEdit.h>
#define P(s) (ConstStr255Param)(s)
enum { SR_OVERLAY = 1, SR_SELFTEST = 2 };
typedef void (*DrawProc)(GrafPtr port, int flags);
/* scene.c: the window, its controls and the one drawing routine. */
extern WindowPtr w;
extern ControlHandle btn;
extern TEHandle edit;
extern Rect pr, field;
int region_at(Point pt);
void draw_scene(GrafPtr port, int flags);
/* io.c: runtime.log and files written beside the executable. */
int log_start(void);
void log_line(const char *name, long code);
void log_stop(void);
int app_file(const char *name, FSSpec *spec);
OSErr open_out(const char *name, OSType type, short *ref);
OSErr text_file(const char *name, const char *text, long n);
/* png.c: 8-bit indexed pixels to a PNG file. */
OSErr png_file(const char *name, const char *pix, long rowBytes, int w, int h,
               CTabHandle colors);
#if SELF_RENDER
/* selfrender.c: sr_poll runs from the event loop; sr_ask queues a render. */
void sr_ask(int frames, int flags);
void sr_poll(DrawProc draw, const Rect *scene);
#else
#define sr_ask(frames, flags) ((void)0)
#define sr_poll(draw, scene) ((void)0)
#endif
