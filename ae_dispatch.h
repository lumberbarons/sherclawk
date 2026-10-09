/* Lifetime-unique queued Apple-event correlations, shared by native clients. */
#ifndef SHERCLAWK_AE_DISPATCH_H
#define SHERCLAWK_AE_DISPATCH_H
#include <AppleEvents.h>
#include <Processes.h>
typedef void (*AEAnswerConsumer)(const AppleEvent *);
OSErr ae_dispatch_init(void);
void ae_dispatch_close(void);
int ae_dispatch_ready(void);
int ae_dispatch_available(void);
OSErr ae_reserve(const ProcessSerialNumber *, AEAnswerConsumer, short *);
void ae_forget(short);
/* attempted is true only if AESend was called: errors then may follow delivery. */
OSErr ae_send(const ProcessSerialNumber *, AEEventClass, AEEventID,
              const char *, short, int *attempted);
#endif
