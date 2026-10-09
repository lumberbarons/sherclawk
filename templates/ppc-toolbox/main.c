#include <Types.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Controls.h>
#include <ControlDefinitions.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Events.h>
#include <Files.h>
#include <Script.h>
#include <Processes.h>
#include <AppleEvents.h>
#include <AERegistry.h>
#include <stdio.h>
#define P(s) (ConstStr255Param)(s)
QDGlobals qd;
static short fd=-1;
static long bytes;
static int done;
static pascal OSErr event(const AppleEvent *e,AppleEvent *r,long quitting)
{ (void)e;(void)r;if(quitting) done=1;return noErr;}
static void start(void)
{
ProcessSerialNumber psn;ProcessInfoRec info={0};
FSSpec app,file;
OSErr err;
info.processInfoLength=sizeof(info);
info.processAppSpec=&app;
if(GetCurrentProcess(&psn)||GetProcessInformation(&psn,&info)) return;
err=FSMakeFSSpec(app.vRefNum,app.parID,P("\pruntime.log"),&file);
if(err==fnfErr) err=FSpCreate(&file,'ttxt','TEXT',smSystemScript);
if(err||FSpOpenDF(&file,fsWrPerm,&fd)) { fd=-1;return;}
if(SetEOF(fd,0)) { FSClose(fd);fd=-1;}
}
static void line(const char *name,long code)
{
char line[96];
long count,want;
if(fd==-1) return;
want=sprintf(line,"%.64s code=%ld\r",name,code);
if(bytes+want>4096) return;
count=want;
if(FSWrite(fd,&count,line)||count!=want) {
FSClose(fd);fd=-1;return;
}
bytes += count;
}
int main(void)
{
WindowPtr w,hit;
ControlHandle btn,ctl;
TEHandle edit;
EventRecord ev;
Rect r,field,frame;
Point local;
short part;
char key;
int active=1;
AEEventHandlerUPP upp=NULL;
InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();
TEInit();InitCursor();
start();line("startup",0);
upp=NewAEEventHandlerUPP(event);
if(!upp ||
AEInstallEventHandler(kCoreEventClass,kAEOpenApplication,upp,0,0) ||
AEInstallEventHandler(kCoreEventClass,kAEQuitApplication,upp,1,0)) {
line("AE init",-1);goto finish;
}
SetRect(&r,80,60,470,210);
w=NewWindow(0L,&r,P("\pTemplate"),1,
documentProc,(WindowPtr)-1L,1,0L);
if(!w) { line("NewWindow",MemError());goto finish;}
SetPort(w);TextFont(0);TextSize(12);
SetRect(&field,18,42,370,66);
frame=field;InsetRect(&frame,-3,-3);
edit=TENew(&field,&field);
if(!edit) line("TENew",MemError());
SetRect(&r,290,90,370,110);
btn=NewControl(w,&r,P("\pClear"),1,0,0,1,pushButProc,0);
if(!btn) line("NewControl",MemError());
if(!edit||!btn) {
if(edit) TEDispose(edit);
DisposeWindow(w);goto finish;
}
if(fd==-1) SetWTitle(w,P("\pLogging unavailable"));
TEAutoView(1,edit);TEActivate(edit);
while(!done) {
SetPort(w);
if(WaitNextEvent(everyEvent,&ev,6L,0L)) switch(ev.what) {
case kHighLevelEvent:
AEProcessAppleEvent(&ev);break;
case updateEvt:
if((WindowPtr)ev.message!=w) break;
BeginUpdate(w);EraseRect(&w->portRect);
FrameRect(&frame);TEUpdate(&field,edit);DrawControls(w);
EndUpdate(w);break;
case mouseDown:
part=FindWindow(ev.where,&hit);
if(hit!=w) break;
if(part==inGoAway&&TrackGoAway(w,ev.where)) done=1;
else if(part==inDrag) DragWindow(w,ev.where,&qd.screenBits.bounds);
else if(part==inContent) {
local=ev.where;GlobalToLocal(&local);
if(FindControl(local,w,&ctl)&&ctl==btn) {
if(TrackControl(btn,local,0L)) {
TESetText("",0,edit);InvalRect(&field);
line("Clear",0);
}
} else if(PtInRect(local,&field))
TEClick(local,(ev.modifiers & shiftKey)!=0,edit);
}
break;
case activateEvt:
case osEvt:
if(ev.what==activateEvt&&(WindowPtr)ev.message!=w) break;
if(ev.what==osEvt&&(ev.message >> 24)!=suspendResumeMessage) break;
active=ev.what==osEvt ? (ev.message & resumeFlag) : (ev.modifiers & activeFlag);
if(active) TEActivate(edit);else TEDeactivate(edit);
break;
case keyDown:
case autoKey:
if(!active) break;
key=ev.message & charCodeMask;
if(ev.modifiers & cmdKey) {
if(key=='q') done=1;
} else if(key==8||(key >= 0x1c&&key <= 0x1f) ||
(*edit)->teLength-((*edit)->selEnd-(*edit)->selStart)<255)
TEKey(key,edit);
break;
}
if(active) TEIdle(edit);
}
TEDeactivate(edit);TEDispose(edit);DisposeWindow(w);
line("quit",0);
finish:
if(upp) {
AERemoveEventHandler(kCoreEventClass,kAEQuitApplication,upp,0);
AERemoveEventHandler(kCoreEventClass,kAEOpenApplication,upp,0);
DisposeAEEventHandlerUPP(upp);
}
if(fd!=-1) FSClose(fd);
return done ? 0 : 1;
}
