/* Optional local credentials: missing configuration still permits a UI build. */
#ifndef SHERCLAWK_CONFIG_H
#define SHERCLAWK_CONFIG_H
#include <stdio.h>
#ifdef SHERCLAWK_HAS_LOCAL_CONFIG
#include "config.local.h"
#endif
#ifndef SHERCLAWK_API_KEY
#define SHERCLAWK_API_KEY ""
#endif
#ifndef SHERCLAWK_MODEL
#define SHERCLAWK_MODEL "openai/gpt-6-luna"
#endif
#ifndef SHERCLAWK_WORKSPACE
#define SHERCLAWK_WORKSPACE "Retro68:"
#endif
/* Optional public app URL for OpenRouter attribution; empty sends none. */
#ifndef SHERCLAWK_APP_URL
#define SHERCLAWK_APP_URL ""
#endif

/* Open Transport teardown experiment (issue #25). SHERCLAWK_OT_YIELD_TICKS is
 * the yield on each side of CloseOpenTransport (60 ticks per second; 0 skips
 * it). With SHERCLAWK_OT_KEEP_OPEN_AFTER_CLEAN set to 1, a cleanly completed
 * model round closes only the TLS context and leaves Open Transport open for
 * the next round; any abort or error still cycles it. 10 ticks was soaked on
 * the guest (see issue #25); the former value was 60. */
#ifndef SHERCLAWK_OT_YIELD_TICKS
#define SHERCLAWK_OT_YIELD_TICKS 10
#endif
#ifndef SHERCLAWK_OT_KEEP_OPEN_AFTER_CLEAN
#define SHERCLAWK_OT_KEEP_OPEN_AFTER_CLEAN 0
#endif

/* Compose OpenRouter app-attribution headers for a configured URL. An empty
 * URL writes an empty block (0); -1 for CR/LF or when the URL does not fit. */
static inline int sherclawk_attribution(const char *url, char *out, size_t cap)
{
    int n;
    const char *p;
    if (!url || !url[0]) { out[0] = 0; return 0; }
    for (p = url; *p; p++) if (*p == '\r' || *p == '\n') return -1;
    n = snprintf(out, cap, "HTTP-Referer: %s\r\nX-OpenRouter-Title: Sherclawk\r\n", url);
    return n > 0 && (size_t)n < cap ? n : -1;
}

/* Fixed originating queue shared by build publication and retained authority. */
#define SHERCLAWK_BUILD_QUEUE "Worker01:buildjobs"

#endif /* SHERCLAWK_CONFIG_H */
