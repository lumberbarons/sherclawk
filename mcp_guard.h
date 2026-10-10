/* Catalog-identity guard for the MCP configuration and the staging and backup
 * files that carry the same credentials. The files are named by the Preferences
 * folder's volume and directory ID plus a leaf name, never by a workspace path,
 * so a workspace that contains Preferences, a rename of the workspace, or a
 * resolved alias target cannot reach them. Every model-facing file tool asks
 * this guard (ADR-0003). */
#ifndef SHERCLAWK_MCP_GUARD_H
#define SHERCLAWK_MCP_GUARD_H
#include <Files.h>

/* Not a File Manager code: tools report it as os_error, and it cannot be
 * mistaken for a missing file that a create tool may fill. */
#define MCP_GUARD_DENIED 30001
/* How a tool reports a refusal whichever step hit it; fixed text, never a path. */
#define MCP_GUARD_CODE "PROTECTED"
#define MCP_GUARD_MESSAGE "Sherclawk keeps this path private; model tools cannot read or change it."

typedef struct {
    short vRefNum;
    long dirID;
} McpGuard;

/* Locate the Preferences folder. Fails closed: when it cannot be located,
 * nothing can be proved unprotected and the result is MCP_GUARD_DENIED. */
OSErr mcp_guard_open(McpGuard *guard);
/* 1 when spec names the configuration, "<name>.new" or "<name>.old" in the
 * Preferences folder, whether or not the file exists. Names compare without
 * case, as HFS does. */
int mcp_guard_protects(const McpGuard *guard, const FSSpec *spec);
/* open + protects: 0 when spec may be used, else MCP_GUARD_DENIED. */
OSErr mcp_guard_check(const FSSpec *spec);
#endif
