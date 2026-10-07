/* Native misc/dosc wire protocol, independently implemented from documented
 * behavior. No MacRelix code is copied or linked. All sends are queued; there
 * is no automatic replay, cancellation claim, or out-of-memory retry.
 * AEProcessAppleEvent must be called by the owner's cooperative event loop.
 */
#include "toolserver.h"
#include <Processes.h>
#include <AERegistry.h>
#include <Events.h>
#include <stdio.h>
#include <string.h>
#define TEXT_LIMIT TOOLSERVER_TEXT_LIMIT
static ToolServerLog record;
static ProcessSerialNumber server;
static AEEventHandlerUPP answer_upp, open_upp;
static short next_id=100, pending_id;
static int waiting, abandoned, received, uncertain_reported;
static uint32_t sent_at;
static ToolServerReply result;
static int find_server(ProcessSerialNumber *psn)
{
    ProcessInfoRec p; FSSpec spec; Str255 name;
    psn->highLongOfPSN=0; psn->lowLongOfPSN=kNoProcess;
    while(GetNextProcess(psn)==noErr) {
        memset(&p,0,sizeof(p)); p.processInfoLength=sizeof(p);
        p.processName=name; p.processAppSpec=&spec;
        if(!GetProcessInformation(psn,&p) && p.processSignature=='MPSX') {
            record("ToolServer process=%lu:%lu app=%.*s",(unsigned long)psn->highLongOfPSN,
                (unsigned long)psn->lowLongOfPSN,spec.name[0],spec.name+1); return 0;
        }
    }
    return -1;
}
static OSErr find_or_launch(void)
{
    HParamBlockRec v; DTPBRec dt; Str255 volume,name; FSSpec spec;
    LaunchParamBlockRec launch; short index; OSErr e=fnfErr;
    if(!find_server(&server)) return noErr;
    record("ToolServer absent; searching mounted desktop databases");
    for(index=1;index<64;index++) {
        memset(&v,0,sizeof(v)); v.volumeParam.ioNamePtr=volume;
        v.volumeParam.ioVolIndex=index;
        if(PBHGetVInfoSync(&v)) break;
        memset(&dt,0,sizeof(dt)); dt.ioVRefNum=v.volumeParam.ioVRefNum;
        if(PBDTGetPath(&dt)) continue;
        dt.ioNamePtr=name; dt.ioIndex=0; dt.ioFileCreator='MPSX';
        e=PBDTGetAPPLSync(&dt); if(e) continue;
        memset(&spec,0,sizeof(spec)); spec.vRefNum=v.volumeParam.ioVRefNum;
        spec.parID=dt.ioAPPLParID; memcpy(spec.name,name,(size_t)name[0]+1);
        record("launch ToolServer volume=%d parent=%ld app=%.*s",spec.vRefNum,
            spec.parID,name[0],name+1);
        memset(&launch,0,sizeof(launch)); launch.launchBlockID=extendedBlock;
        launch.launchEPBLength=extendedBlockLen;
        launch.launchControlFlags=launchContinue | launchNoFileFlags | launchDontSwitch;
        launch.launchAppSpec=&spec; e=LaunchApplication(&launch);
        if(!e) { server=launch.launchProcessSN; return noErr; }
    }
    return e ? e : fnfErr;
}
static OSErr text_param(const AppleEvent *event,AEKeyword key,char *buf)
{
    DescType type=typeNull; Size size=0,got=0; OSErr e=AESizeOfParam(event,key,&type,&size);
    buf[0]=0;
    if(e==errAEDescNotFound) return noErr;
    if(e || type!=typeChar || size<0 || size>TEXT_LIMIT) {
        record("text parameter %08lx error=%d type=%08lx bytes=%ld limit=%d",
            (unsigned long)key,e,(unsigned long)type,(long)size,TEXT_LIMIT);
        return e ? e : paramErr;
    }
    e=AEGetParamPtr(event,key,typeChar,&type,buf,TEXT_LIMIT,&got);
    if(e || got!=size) return e ? e : paramErr;
    buf[got]=0; return noErr;
}
static pascal OSErr answer(const AppleEvent *event,AppleEvent *reply,long refcon)
{
    DescType type; Size got; short id=0; ProcessSerialNumber sender;
    OSErr e; (void)reply; (void)refcon;
    e=AEGetAttributePtr(event,keyReturnIDAttr,typeSInt16,&type,&id,sizeof(id),&got);
    if(e || got!=sizeof(id) || !waiting || id!=pending_id) {
        record("ignored reply return_id=%d error=%d waiting=%d expected=%d",id,e,waiting,pending_id);
        return noErr;
    }
    e=AEGetAttributePtr(event,keyAddressAttr,typeProcessSerialNumber,&type,&sender,sizeof(sender),&got);
    if(e || got!=sizeof(sender) || sender.highLongOfPSN!=server.highLongOfPSN ||
        sender.lowLongOfPSN!=server.lowLongOfPSN) {
        record("ignored reply sender error=%d bytes=%ld",e,(long)got); return noErr;
    }
    result.malformed=0; result.status=-999;
    e=AEGetParamPtr(event,'stat',typeSInt32,&type,&result.status,sizeof(result.status),&got);
    if(e || got!=sizeof(result.status)) result.malformed=1;
    if(text_param(event,keyDirectObject,result.output) || text_param(event,'diag',result.diagnostic)) result.malformed=1;
    result.abandoned=abandoned;
    record("reply id=%d stat=%ld malformed=%d abandoned=%d elapsed_ticks=%lu",
        id,(long)result.status,result.malformed,abandoned,(unsigned long)(TickCount()-sent_at));
    waiting=0; received=1; return noErr;
}
static pascal OSErr opened(const AppleEvent *event,AppleEvent *reply,long refcon)
{ (void)event; (void)reply; (void)refcon; return noErr; }
static OSErr send_event(AEEventClass cls,AEEventID event_id,const char *script,short id,AESendMode mode)
{
    AEAddressDesc address={typeNull,NULL}; AppleEvent event={typeNull,NULL},reply={typeNull,NULL};
    OSErr e=AECreateDesc(typeProcessSerialNumber,&server,sizeof(server),&address);
    if(!e) e=AECreateAppleEvent(cls,event_id,&address,id,kAnyTransactionID,&event);
    if(!e && script) e=AEPutParamPtr(&event,keyDirectObject,typeChar,script,(Size)strlen(script));
    if(!e) e=AESend(&event,&reply,mode | kAENeverInteract,kAENormalPriority,60,NULL,NULL);
    AEDisposeDesc(&reply); AEDisposeDesc(&event); AEDisposeDesc(&address); return e;
}

OSErr toolserver_init(ToolServerLog log)
{
    OSErr e;
    if(!log || answer_upp || open_upp)return paramErr;
    record=log;
    answer_upp=NewAEEventHandlerUPP(answer); open_upp=NewAEEventHandlerUPP(opened);
    if(!answer_upp || !open_upp) { toolserver_close(); return memFullErr; }
    e=AEInstallEventHandler(kCoreEventClass,kAEAnswer,answer_upp,0,false);
    if(!e)e=AEInstallEventHandler(kCoreEventClass,kAEOpenApplication,open_upp,0,false);
    if(e)toolserver_close();
    return e;
}
void toolserver_close(void)
{
    if(answer_upp) {
        AERemoveEventHandler(kCoreEventClass,kAEAnswer,answer_upp,false);
        DisposeAEEventHandlerUPP(answer_upp); answer_upp=NULL;
    }
    if(open_upp) {
        AERemoveEventHandler(kCoreEventClass,kAEOpenApplication,open_upp,false);
        DisposeAEEventHandlerUPP(open_upp); open_upp=NULL;
    }
    waiting=received=0;
}
int toolserver_busy(void) { return waiting || received; }
OSErr toolserver_send(const char *directory,const char *command,uint32_t now)
{
    char script[4096]; OSErr e;
    if(!answer_upp || toolserver_busy() || next_id==32767 ||
        !directory || !command || strchr(directory,'"') || strchr(directory,'\r') ||
        strchr(directory,'\n'))return paramErr;
    if(snprintf(script,sizeof(script),"Set Exit 0\rDirectory \"%s\"\r%s < Dev:Null\rSet CommandStatus {Status}\rDirectory \"{MPW}\"\rExit {CommandStatus}\r",directory,command)>=(int)sizeof(script))return paramErr;
    e=find_or_launch(); if(e)return e;
    pending_id=++next_id; abandoned=received=uncertain_reported=0;
    memset(&result,0,sizeof(result)); result.status=-999; sent_at=now;
    e=send_event(kAEMiscStandards,kAEDoScript,script,pending_id,kAEQueueReply);
    record("send id=%d error=%d duration_ticks=%lu command=[%s]",pending_id,e,
        (unsigned long)(TickCount()-now),command);
    if(!e)waiting=1;
    return e;
}
int toolserver_poll(uint32_t now,int stop,ToolServerReply *reply)
{
    if((waiting || received) && !abandoned &&
        (stop || (uint32_t)(now-sent_at)>=120UL*60UL)) {
        abandoned=1;
        record("UNKNOWN id=%d reason=%s; no resend",pending_id,stop ? "Stop" : "deadline");
    }
    if(waiting) {
        ProcessInfoRec p; memset(&p,0,sizeof(p)); p.processInfoLength=sizeof(p);
        if(GetProcessInformation(&server,&p)) {
            abandoned=1; waiting=0;
            record("UNKNOWN id=%d ToolServer disappeared; no resend",pending_id);
        }
    }
    if(abandoned && !uncertain_reported) { uncertain_reported=1; return -1; }
    if(received) {
        received=0; result.abandoned=abandoned; *reply=result;
        return 1;
    }
    return 0;
}
