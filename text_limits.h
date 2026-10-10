/* Model budgets remain fixed while the native 64 KiB path is guest-tested. */
#ifndef SHERCLAWK_TEXT_LIMITS_H
#define SHERCLAWK_TEXT_LIMITS_H
#define TOOLS_FILE_CAP 65536L
#define TOOLS_STRING_CAP 4096
#define TOOLS_WORK_CHUNK 1024
#define TOOLS_DESCRIPTOR_CAP 4096
#define TOOLS_SNAPSHOT_CAP 131072L
/* Stringify a cap for model-facing text. The snapshot cap carries an L suffix
 * in code, so its description is hand-written here; the host registry test
 * fails when any description stops naming its cap. */
#define TOOLS_LIMIT_STRINGIFY_(x) #x
#define TOOLS_LIMIT_STRINGIFY(x) TOOLS_LIMIT_STRINGIFY_(x)
#define TOOLS_STRING_CAP_DESCRIPTION TOOLS_LIMIT_STRINGIFY(TOOLS_STRING_CAP)
#define TOOLS_DESCRIPTOR_CAP_DESCRIPTION TOOLS_LIMIT_STRINGIFY(TOOLS_DESCRIPTOR_CAP)
#define TOOLS_SNAPSHOT_CAP_DESCRIPTION "131072"
/* The one refusal message for a descriptor, input or total-snapshot limit. */
#define TOOLS_SNAPSHOT_LIMIT_MESSAGE \
    "Descriptor limit " TOOLS_DESCRIPTOR_CAP_DESCRIPTION " bytes; input limit " \
    TOOLS_FILE_CAP_DESCRIPTION " bytes; total snapshot limit " \
    TOOLS_SNAPSHOT_CAP_DESCRIPTION " bytes."
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
