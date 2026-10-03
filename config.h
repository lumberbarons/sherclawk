/* Optional local credentials: missing configuration still permits a UI build. */
#ifndef SHERCLAWK_CONFIG_H
#define SHERCLAWK_CONFIG_H
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

#endif /* SHERCLAWK_CONFIG_H */
