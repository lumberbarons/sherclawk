#ifndef HARNESS_DIAGNOSTIC_H
#define HARNESS_DIAGNOSTIC_H
/* Public diagnostic adapter; no Sherclawk dependency. Init: 0 manual, 1 harness,
 * -1 invalid configuration (caller must exit). Finish publishes then returns. */
int harness_begin(const char *diagnostic);
const char *harness_workspace(void);
void harness_check(int passed, const char *name);
int harness_finish(int failures);
#endif
