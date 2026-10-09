/* Diagnostic-only driver: service native events between bounded tool steps. */
#ifndef SHERCLAWK_DIAGNOSTIC_TOOLS_H
#define SHERCLAWK_DIAGNOSTIC_TOOLS_H
#include "tools.h"
#include <Events.h>
#include <AppleEvents.h>
static int diagnostic_execute_recorded(const AgentCall *call,char *out,size_t cap,AgentJournal journal,void *ctx)
{
    int r=tools_execute_recorded(call,out,cap,journal,ctx);
    while(r==2) {
        EventRecord event;
        int stop=0;
        if(WaitNextEvent(everyEvent,&event,0,NULL)) {
            if(event.what==kHighLevelEvent)AEProcessAppleEvent(&event);
            if(event.what==keyDown && (event.modifiers & cmdKey) && (event.message & charCodeMask)=='.')stop=1;
        }
        SystemTask();
        r=tools_text_step(out,cap,(uint32_t)TickCount(),stop);
    }
    return r;
}
#define tools_execute_recorded diagnostic_execute_recorded
#define tools_execute(call,out,cap) diagnostic_execute_recorded(call,out,cap,NULL,NULL)
#endif
