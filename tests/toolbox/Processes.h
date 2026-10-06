/* Host-only Process Manager model; actual launch behavior is guest-verified. */
#ifndef TEST_PROCESSES_H
#define TEST_PROCESSES_H
#include "Files.h"
typedef struct { unsigned long highLongOfPSN, lowLongOfPSN; } ProcessSerialNumber;
typedef struct { unsigned long launchBlockID,launchEPBLength,launchFileFlags,launchControlFlags; FSSpec *launchAppSpec; ProcessSerialNumber launchProcessSN; } LaunchParamBlockRec;
typedef struct { unsigned long processInfoLength; unsigned char *processName; FSSpec *processAppSpec; } ProcessInfoRec;
enum { extendedBlock=1,extendedBlockLen=1,launchContinue=1,launchDontSwitch=2,launchNoFileFlags=4 };
OSErr LaunchApplication(LaunchParamBlockRec *);
OSErr GetProcessInformation(const ProcessSerialNumber *,ProcessInfoRec *);
#endif
