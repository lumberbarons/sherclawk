/* Private, volatile authority. Neither chat reset nor journal replay owns it. */
#ifndef SHERCLAWK_APPLICATION_PROCESS_H
#define SHERCLAWK_APPLICATION_PROCESS_H
#include "run_application.h"
#include <Processes.h>
void application_tracking_begin(void);
/* One registry entry or enumerated process per turn; 2 pending, 0 complete. */
int application_tracking_step(void);
void application_tracking_expire(void);
const char *application_tracking_reason(const ProcessSerialNumber *, const ProcessInfoRec *);
void application_tracking_commit(const char *,const ProcessSerialNumber *,const FSSpec *,const ProcessInfoRec *);
const char *application_original_run(const ProcessSerialNumber *);
#endif
