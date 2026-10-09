/* Trusted native snapshot producer; one bounded step per cooperative event turn.
 * This API does not authorize artifacts or expose shell text to the model. */
#ifndef SHERCLAWK_JOBS_H
#define SHERCLAWK_JOBS_H
#include <Files.h>
#include <stddef.h>
#include <stdint.h>
#include "agent.h"
#define JOB_PAGE 1024
#define JOB_INPUT_MAX 13
#define JOB_FILE_MAX 65536
#define JOB_TOTAL_MAX 131072
#define JOB_RECORD_MAX 256

typedef enum { JOB_STAGING, JOB_WAITING, JOB_SUCCEEDED, JOB_FAILED,
    JOB_SIGNALED, JOB_REJECTED, JOB_ABANDONED, JOB_UNKNOWN } JobState;
typedef struct { const char *name; const void *bytes; size_t size; } JobInput;
typedef struct {
    JobState state;
    short volume;
    long directory;
    char id[25];
    JobInput inputs[JOB_INPUT_MAX];
    int count, input, verifying, published;
    long offset, stdout_offset, stderr_offset;
    uint32_t start, timeout, next_poll;
    AgentJournal journal;
    void *context;
    OSErr error;
    int exit_code, signal, wait_status;
    size_t stdout_size, stderr_size;
    char stdout_page[JOB_PAGE], stderr_page[JOB_PAGE];
} NativeJob;
/* Queue is an already-resolved trusted folder (not an arbitrary model path).
 * Input names/bytes must stay immutable until staging ends. ID never reused.
 * Journal context must retain the logical queue path: volume/directory IDs
 * are observational across remounts.
 * timeout is 1..INT32_MAX ticks; now is TickCount(). No automatic retries. */
int jobs_begin(NativeJob *, const FSSpec *queue, const char *id,
               const JobInput *, int count, uint32_t now, uint32_t timeout,
               AgentJournal, void *context);
/* <=1 KiB input write/read or <=256-byte record + two <=1 KiB log pages.
 * Poll at most once per 60 ticks. Pages are raw bytes, not C strings; caller
 * consumes each before the next step. Stop abandons unpublished jobs and
 * marks published jobs unknown; it never cancels the executor or writes STOP. */
void jobs_step(NativeJob *, uint32_t now, int stop);
/* Read-only log continuation, including after a terminal result. */
int jobs_logs(NativeJob *);
/* Strict protocol-1 terminal parser; malformed records do not imply success. */
int jobs_parse_result(NativeJob *, const char *, size_t);
#endif
