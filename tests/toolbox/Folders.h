/* Test-only Folder Manager model: the volume Trash lookup. */
#ifndef TEST_FOLDERS_H
#define TEST_FOLDERS_H
#include <Files.h>
enum { kTrashFolderType=0x74727368 };
OSErr FindFolder(short, unsigned long, Boolean, short *, long *);
#endif
