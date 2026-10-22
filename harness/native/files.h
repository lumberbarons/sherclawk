#ifndef HARNESS_FILES_H
#define HARNESS_FILES_H
#include "contract.h"
#include <Files.h>
/* Private boot-disk Preferences files, never an AFP-supplied config. */
int h_private_path(const char *, char *, size_t);
int h_read(const char *, char *, size_t); /* -1 I/O, -2 missing */
int h_publish(const char *, const char *, size_t); /* closed create-only rename */
int h_spec(const char *, FSSpec *);
int h_self_matches(const char *);
void h_mailpath(char *, size_t, const HRequest *, const char *);
unsigned long h_now(void);
#endif
