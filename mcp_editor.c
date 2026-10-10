#include "mcp_editor.h"
#include "text.h"
#include <stdio.h>
#include <string.h>

const char mcp_editor_template[] = "{\n  \"mcpServers\": {}\n}\n";

static void fail(char *error, size_t cap, const char *field)
{
    if (cap) snprintf(error, cap, "Invalid MCP configuration: %s.", field);
}

int mcp_editor_validate(const char *text, size_t len, char *utf8, size_t utf8_cap,
                        char *error, size_t error_cap)
{
    static McpConfig config; /* ~19 KiB of credentials; wiped below */
    int used, rc;

    if (len > MCP_EDITOR_TEXT_CAP) { fail(error, error_cap, "8 KiB limit"); return -1; }
    used = text_to_utf8(text, len, utf8, utf8_cap);
    if (used < 0) { fail(error, error_cap, "text encoding"); return -1; }
    if ((size_t)used > MCP_CONFIG_CAP) { fail(error, error_cap, "8 KiB limit"); return -1; }
    rc = mcp_config_parse(utf8, (size_t)used, &config, error, error_cap);
    memset(&config, 0, sizeof(config));
    return rc ? -1 : used;
}

int mcp_editor_display(const char *utf8, size_t len, char *text, size_t text_cap)
{
    static char terminated[MCP_EDITOR_UTF8_CAP];
    int used;

    if (len > MCP_EDITOR_UTF8_CAP - 1 || memchr(utf8, 0, len)) return -1;
    memcpy(terminated, utf8, len); terminated[len] = 0;
    used = text_to_macroman_strict(terminated, text, text_cap);
    memset(terminated, 0, len);
    return used;
}
