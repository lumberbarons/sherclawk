/* Copy to config.local.h (gitignored). The test key is embedded in the app.
 * Never distribute a build containing your key. Preferences saved in the app
 * (Edit menu) override these compiled fallbacks; config.local.h is only the
 * starting point for a build without a saved preferences file. */
#ifndef SHERCLAWK_LOCAL_CONFIG_H
#define SHERCLAWK_LOCAL_CONFIG_H
#define SHERCLAWK_API_KEY ""
#define SHERCLAWK_MODEL "openai/gpt-6-luna"
/* Classic MacRoman workspace path, ending in a colon; Preferences can override. */
#define SHERCLAWK_WORKSPACE "Retro68:"
/* Optional public app URL for OpenRouter attribution (empty sends none). */
#define SHERCLAWK_APP_URL ""
#endif
