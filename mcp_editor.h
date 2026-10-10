/* Text-side rules for the MCP Servers editor: classic TextEdit holds MacRoman
 * with CR, the configuration file is UTF-8 with LF. Pure C, no Toolbox. */
#ifndef SHERCLAWK_MCP_EDITOR_H
#define SHERCLAWK_MCP_EDITOR_H
#include <stddef.h>
#include "mcp.h"
/* Editor text is MacRoman, so each byte becomes at most three UTF-8 bytes. */
#define MCP_EDITOR_TEXT_CAP MCP_CONFIG_CAP
#define MCP_EDITOR_UTF8_CAP (MCP_EDITOR_TEXT_CAP * 3 + 1)
/* Shown when no configuration file exists yet; valid and disabled. */
extern const char mcp_editor_template[];
/* Convert editor text to UTF-8/LF in utf8 (>= MCP_EDITOR_UTF8_CAP) and accept
 * it only if mcp_config_parse does. Returns the UTF-8 length, or -1 with a
 * message naming the field, never a value, in error. The parsed credentials
 * are wiped before returning. */
int mcp_editor_validate(const char *text, size_t len, char *utf8, size_t utf8_cap,
                        char *error, size_t error_cap);
/* Convert stored UTF-8/LF to MacRoman/CR for display. Returns the length, or
 * -1 when the file is not representable or does not fit text_cap. */
int mcp_editor_display(const char *utf8, size_t len, char *text, size_t text_cap);
#endif
