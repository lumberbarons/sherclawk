/* File Manager persistence for the MCP configuration. A save stages the new
 * bytes next to the target, verifies them by reading them back, and only then
 * swaps them in, so any failure before the swap leaves the previous file as it
 * was. Callers resolve the target FSSpec (Preferences folder lives in main.c). */
#ifndef SHERCLAWK_MCP_STORE_H
#define SHERCLAWK_MCP_STORE_H
#include <Files.h>
#include <stddef.h>

typedef enum {
    MCP_STORE_OK = 0,
    MCP_STORE_ABSENT,     /* load: no file yet */
    MCP_STORE_UNREADABLE, /* load: I/O error; the file was not touched */
    MCP_STORE_TOO_BIG,    /* load: larger than the buffer; not touched */
    MCP_STORE_RECOVER,    /* an interrupted save left only "<name>.old" */
    MCP_STORE_WRITE,      /* save: staging or verification failed; previous file intact */
    MCP_STORE_SWAP,       /* save: could not start the swap; previous file intact */
    MCP_STORE_RESTORED,   /* save: swap failed after it began; previous file restored */
    MCP_STORE_LOST        /* save: swap failed and restore failed; previous file is "<name>.old" */
} McpStoreResult;

/* Largest file the editor handles; matches MCP_CONFIG_CAP in mcp.h. */
#define MCP_STORE_CAP 8192
/* Siblings of the configuration during a save; both carry its credentials. */
#define MCP_STORE_SUFFIX_NEW ".new"
#define MCP_STORE_SUFFIX_OLD ".old"
#define MCP_STORE_CREATOR 0x74747874UL /* 'ttxt' */
#define MCP_STORE_TYPE 0x54455854UL    /* 'TEXT' */

McpStoreResult mcp_store_load(const FSSpec *target, char *bytes, size_t cap, size_t *len);
McpStoreResult mcp_store_save(const FSSpec *target, const char *bytes, size_t len);
#endif
