/* One queued ToolServer command at a time. Stop only abandons observation;
 * the return ID remains reserved until its reply drains or the server dies. */
#ifndef SHERCLAWK_TOOLSERVER_H
#define SHERCLAWK_TOOLSERVER_H
#include <AppleEvents.h>
#include <stdint.h>
#define TOOLSERVER_TEXT_LIMIT 8192
typedef void (*ToolServerLog)(const char *format,...);
typedef struct {
    int malformed, abandoned;
    SInt32 status;
    char output[TOOLSERVER_TEXT_LIMIT+1], diagnostic[TOOLSERVER_TEXT_LIMIT+1];
} ToolServerReply;
OSErr toolserver_init(ToolServerLog log);
void toolserver_close(void);
OSErr toolserver_send(const char *directory,const char *command,uint32_t now);
/* 0 pending, 1 reply, -1 uncertain (timeout/Stop/disappearance). */
int toolserver_poll(uint32_t now,int stop,ToolServerReply *reply);
int toolserver_busy(void);
#endif
