/* Model budgets remain fixed while the native 64 KiB path is guest-tested. */
#ifndef SHERCLAWK_TEXT_LIMITS_H
#define SHERCLAWK_TEXT_LIMITS_H
#define TOOLS_FILE_CAP 65536L
#define TOOLS_STRING_CAP 4096
#define TOOLS_WORK_CHUNK 1024
#define TOOLS_DESCRIPTOR_CAP 4096
#define TOOLS_SNAPSHOT_CAP 131072L
/* Enable in the shipping app only after recording OS 9.2.2 acceptance. */
#define SHERCLAWK_LARGE_TEXT_VERIFIED 0
#if SHERCLAWK_LARGE_TEXT_VERIFIED || defined(SHERCLAWK_HOST) || defined(SHERCLAWK_LARGE_TEXT_CHECK)
#define TOOLS_ACCEPTED_FILE_CAP TOOLS_FILE_CAP
#define TOOLS_FILE_CAP_DESCRIPTION "65536"
#else
#define TOOLS_ACCEPTED_FILE_CAP 4096L
#define TOOLS_FILE_CAP_DESCRIPTION "4096"
#endif
#endif
