#include "mcp_guard.h"
#include "mcp.h"
#include "mcp_store.h"
#include "preferences.h"
#include <Folders.h>
#include <string.h>

OSErr mcp_guard_open(McpGuard *guard)
{
    short vRefNum = 0;
    long dirID = 0;
    OSErr err = FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder, &vRefNum, &dirID);
    guard->vRefNum = 0;
    guard->dirID = 0;
    if (err || !dirID) return MCP_GUARD_DENIED;
    guard->vRefNum = vRefNum;
    guard->dirID = dirID;
    return noErr;
}

static int fold(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}
/* Pascal name equals "<base><suffix>", ASCII case folded. */
static int named(const unsigned char *leaf_name, const char *base, const char *suffix)
{
    size_t base_len = strlen(base), suffix_len = strlen(suffix), i;
    if (leaf_name[0] != base_len + suffix_len) return 0;
    for (i = 0; i < base_len; i++)
        if (fold(leaf_name[1 + i]) != fold((unsigned char)base[i])) return 0;
    for (i = 0; i < suffix_len; i++)
        if (fold(leaf_name[1 + base_len + i]) != fold((unsigned char)suffix[i])) return 0;
    return 1;
}

int mcp_guard_protects(const McpGuard *guard, const FSSpec *spec)
{
    if (spec->vRefNum != guard->vRefNum || spec->parID != guard->dirID) return 0;
    return named(spec->name, PREFS_FILENAME, "") ||
           named(spec->name, MCP_CONFIG_FILENAME, "") ||
           named(spec->name, MCP_CONFIG_FILENAME, MCP_STORE_SUFFIX_NEW) ||
           named(spec->name, MCP_CONFIG_FILENAME, MCP_STORE_SUFFIX_OLD);
}

OSErr mcp_guard_check(const FSSpec *spec)
{
    McpGuard guard;
    if (mcp_guard_open(&guard)) return MCP_GUARD_DENIED;
    return mcp_guard_protects(&guard, spec) ? MCP_GUARD_DENIED : noErr;
}
