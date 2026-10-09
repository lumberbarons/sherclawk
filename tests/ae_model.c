/* Apple-event fault model used with the actual production dispatcher. */
#include <AppleEvents.h>
#include <Processes.h>
#include <assert.h>
#include <string.h>
static AEEventHandlerUPP ae_answer_handler;
static int ae_fail_stage,ae_sends,ae_live,ae_disposals;
static short ae_last_id;
static char descriptor_storage;
static ProcessSerialNumber ae_target;
static AEEventID ae_last_event;
static AESendMode ae_last_mode;
static char ae_script[4096];
static int ae_attribute_fault;
static struct { short id;ProcessSerialNumber sender;int malformed,has_error;int32_t error; } ae_incoming;
AEEventHandlerUPP NewAEEventHandlerUPP(AEEventHandlerUPP p) {return ae_fail_stage==1 ? NULL : p;}
void DisposeAEEventHandlerUPP(AEEventHandlerUPP p) {(void)p;}
OSErr AEInstallEventHandler(AEEventClass c,AEEventID i,AEEventHandlerUPP p,long r,Boolean b)
{(void)c;(void)r;(void)b;if(ae_fail_stage==2)return ioErr;if(i==kAEAnswer)ae_answer_handler=p;return 0;}
OSErr AERemoveEventHandler(AEEventClass c,AEEventID i,AEEventHandlerUPP p,Boolean b)
{(void)c;(void)p;(void)b;if(i==kAEAnswer)ae_answer_handler=NULL;return 0;}
OSErr AEGetAttributePtr(const AppleEvent *e,AEKeyword k,DescType t,DescType *type,void *p,Size cap,Size *got)
{
    (void)e;*type=t;
    if(k==keyReturnIDAttr){*got=sizeof(ae_incoming.id);assert(cap>=*got);memcpy(p,&ae_incoming.id,(size_t)*got);}
    else {assert(k==keyAddressAttr);*got=sizeof(ae_incoming.sender);assert(cap>=*got);memcpy(p,&ae_incoming.sender,(size_t)*got);}
    if((k==keyReturnIDAttr && (ae_attribute_fault & 1)) || (k==keyAddressAttr && (ae_attribute_fault & 2)))*type=typeChar;
    if((k==keyReturnIDAttr && (ae_attribute_fault & 4)) || (k==keyAddressAttr && (ae_attribute_fault & 8)))*got=1;
    return 0;
}
OSErr AEGetParamPtr(const AppleEvent *e,AEKeyword k,DescType t,DescType *type,void *p,Size cap,Size *got)
{
    (void)e;*type=t;
    if(k==keyErrorNumber && !ae_incoming.has_error)return errAEDescNotFound;
    if(k=='stat' || k==keyErrorNumber) {assert(cap>=4);*got=ae_incoming.malformed ? 1 : 4;memcpy(p,&ae_incoming.error,4);return 0;}
    return errAEDescNotFound;
}
OSErr AESizeOfParam(const AppleEvent *e,AEKeyword k,DescType *t,Size *s)
{(void)e;(void)k;(void)t;(void)s;return errAEDescNotFound;}
OSErr AECreateDesc(DescType t,const void *p,Size n,AEDesc *d)
{assert(t==typeProcessSerialNumber && n==sizeof(ae_target));if(ae_fail_stage==3)return memFullErr;memcpy(&ae_target,p,sizeof(ae_target));d->descriptorType=t;d->dataHandle=&descriptor_storage;ae_live++;return 0;}
OSErr AECreateAppleEvent(AEEventClass c,AEEventID i,const AEAddressDesc *a,short id,long tr,AppleEvent *e)
{(void)c;(void)a;(void)tr;if(ae_fail_stage==4)return memFullErr;ae_last_event=i;ae_last_id=id;e->descriptorType=typeChar;e->dataHandle=&descriptor_storage;ae_live++;return 0;}
OSErr AEPutParamPtr(AppleEvent *e,AEKeyword k,DescType t,const void *p,Size n)
{(void)e;assert(k==keyDirectObject && t==typeChar && n<(Size)sizeof(ae_script));memcpy(ae_script,p,(size_t)n);ae_script[n]=0;return ae_fail_stage==5 ? memFullErr : 0;}
OSErr AESend(const AppleEvent *e,AppleEvent *r,AESendMode m,long pri,long timeout,void *idle,void *filter)
{(void)e;(void)r;(void)pri;(void)timeout;(void)idle;(void)filter;ae_sends++;ae_last_mode=m;return ae_fail_stage==6 ? ioErr : 0;}
OSErr AEDisposeDesc(AEDesc *d) {ae_disposals++;if(d->dataHandle)ae_live--;d->dataHandle=NULL;return 0;}
void ae_deliver(short id,unsigned long sender,int malformed,int has_error,int32_t error)
{
    AppleEvent e={typeNull,NULL},r={typeNull,NULL};assert(ae_answer_handler);
    ae_incoming.id=id;ae_incoming.sender.highLongOfPSN=0;ae_incoming.sender.lowLongOfPSN=sender;
    ae_incoming.malformed=malformed;ae_incoming.has_error=has_error;ae_incoming.error=error;
    assert(!ae_answer_handler(&e,&r,0));
}
