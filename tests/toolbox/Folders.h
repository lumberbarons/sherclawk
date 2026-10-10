/* Test-only Folder Manager model: the volume Trash and Preferences lookups. */
#ifndef TEST_FOLDERS_H
#define TEST_FOLDERS_H
#include <Files.h>
enum { kTrashFolderType=0x74727368, kPreferencesFolderType=0x70726566,
       kOnSystemDisk=-32768, kDontCreateFolder=0, kCreateFolder=1 };
OSErr FindFolder(short, unsigned long, Boolean, short *, long *);
#endif
