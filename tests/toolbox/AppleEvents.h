#ifndef TEST_APPLE_EVENTS_H
#define TEST_APPLE_EVENTS_H
#include "Files.h"
#include <stdint.h>
#include <stdbool.h>
#define pascal
typedef uint32_t DescType, AEKeyword, AEEventClass, AEEventID;
typedef long AESendMode;
typedef struct { DescType descriptorType; Handle dataHandle; } AEDesc;
typedef AEDesc AppleEvent, AEAddressDesc;
typedef OSErr (*AEEventHandlerUPP)(const AppleEvent *,AppleEvent *,long);
enum { typeNull=0,typeSInt16='shor',typeSInt32='long',typeChar='TEXT',typeProcessSerialNumber='psn ',
       keyReturnIDAttr='rtid',keyAddressAttr='addr',keyDirectObject='----',keyErrorNumber='errn',
       kCoreEventClass='aevt',kAEAnswer='ansr',kAEOpenApplication='oapp',kAEQuitApplication='quit',
       kAEMiscStandards='misc',kAEDoScript='dosc',kAnyTransactionID=0,
       kAEQueueReply=2,kAENeverInteract=16,kAENormalPriority=0,errAEDescNotFound=-1701,memFullErr=-108 };
AEEventHandlerUPP NewAEEventHandlerUPP(AEEventHandlerUPP);
void DisposeAEEventHandlerUPP(AEEventHandlerUPP);
OSErr AEInstallEventHandler(AEEventClass,AEEventID,AEEventHandlerUPP,long,Boolean);
OSErr AERemoveEventHandler(AEEventClass,AEEventID,AEEventHandlerUPP,Boolean);
OSErr AEGetAttributePtr(const AppleEvent *,AEKeyword,DescType,DescType *,void *,Size,Size *);
OSErr AEGetParamPtr(const AppleEvent *,AEKeyword,DescType,DescType *,void *,Size,Size *);
OSErr AESizeOfParam(const AppleEvent *,AEKeyword,DescType *,Size *);
OSErr AECreateDesc(DescType,const void *,Size,AEDesc *);
OSErr AECreateAppleEvent(AEEventClass,AEEventID,const AEAddressDesc *,short,long,AppleEvent *);
OSErr AEPutParamPtr(AppleEvent *,AEKeyword,DescType,const void *,Size);
OSErr AESend(const AppleEvent *,AppleEvent *,AESendMode,long,long,void *,void *);
OSErr AEDisposeDesc(AEDesc *);
#endif
