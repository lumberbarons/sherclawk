#include "ae_dispatch.h"
#include <AERegistry.h>
#include <string.h>
static AEEventHandlerUPP answer_upp;
static unsigned int next_id=100; /* Never reset, even on close/reinitialization. */
static struct { short id; ProcessSerialNumber psn; AEAnswerConsumer consume; } slots[2];
static pascal OSErr answer(const AppleEvent *event,AppleEvent *reply,long refcon)
{
    DescType type; Size got; short id; ProcessSerialNumber sender; int i;
    (void)reply; (void)refcon;
    if(AEGetAttributePtr(event,keyReturnIDAttr,typeSInt16,&type,&id,sizeof(id),&got) ||
       type!=typeSInt16 || got!=sizeof(id))return noErr;
    if(AEGetAttributePtr(event,keyAddressAttr,typeProcessSerialNumber,&type,&sender,sizeof(sender),&got) ||
       type!=typeProcessSerialNumber || got!=sizeof(sender))return noErr;
    for(i=0;i<2;i++)if(slots[i].consume && slots[i].id==id &&
        slots[i].psn.highLongOfPSN==sender.highLongOfPSN &&
        slots[i].psn.lowLongOfPSN==sender.lowLongOfPSN) {
        AEAnswerConsumer consume=slots[i].consume;
        slots[i].consume=NULL; consume(event); break;
    }
    return noErr;
}
int ae_dispatch_ready(void) { return answer_upp!=NULL; }
int ae_dispatch_available(void)
{
    return answer_upp && next_id<32767 && (!slots[0].consume || !slots[1].consume);
}
OSErr ae_dispatch_init(void)
{
    OSErr e;
    if(answer_upp)return noErr;
    answer_upp=NewAEEventHandlerUPP(answer);
    if(!answer_upp)return memFullErr;
    e=AEInstallEventHandler(kCoreEventClass,kAEAnswer,answer_upp,0,false);
    if(e) { DisposeAEEventHandlerUPP(answer_upp);answer_upp=NULL; }
    return e;
}
void ae_dispatch_close(void)
{
    if(answer_upp) {
        AERemoveEventHandler(kCoreEventClass,kAEAnswer,answer_upp,false);
        DisposeAEEventHandlerUPP(answer_upp);answer_upp=NULL;
    }
    memset(slots,0,sizeof(slots));
}
OSErr ae_reserve(const ProcessSerialNumber *psn,AEAnswerConsumer consume,short *id)
{
    int i;
    if(!answer_upp || !consume || next_id>=32767)return paramErr;
    for(i=0;i<2;i++)if(!slots[i].consume) {
        slots[i].id=(short)++next_id;slots[i].psn=*psn;slots[i].consume=consume;
        *id=slots[i].id;return noErr;
    }
    return paramErr;
}
void ae_forget(short id)
{ int i;for(i=0;i<2;i++)if(slots[i].id==id)slots[i].consume=NULL; }
OSErr ae_send(const ProcessSerialNumber *psn,AEEventClass cls,AEEventID event_id,
              const char *text,short id,int *attempted)
{
    AEAddressDesc address={typeNull,NULL}; AppleEvent event={typeNull,NULL},reply={typeNull,NULL};
    OSErr e;
    *attempted=0;
    e=AECreateDesc(typeProcessSerialNumber,psn,sizeof(*psn),&address);
    if(!e)e=AECreateAppleEvent(cls,event_id,&address,id,kAnyTransactionID,&event);
    if(!e && text)e=AEPutParamPtr(&event,keyDirectObject,typeChar,text,(Size)strlen(text));
    if(!e) {
        *attempted=1;
        e=AESend(&event,&reply,kAEQueueReply | kAENeverInteract,kAENormalPriority,60,NULL,NULL);
    }
    AEDisposeDesc(&reply);AEDisposeDesc(&event);AEDisposeDesc(&address);
    return e;
}
