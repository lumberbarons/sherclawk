/* Host callers explicitly advance pending text operations. */
#ifndef SHERCLAWK_TEST_STEPPED_TOOLS_H
#define SHERCLAWK_TEST_STEPPED_TOOLS_H
extern unsigned long TickCount(void);
static int test_execute_recorded(const AgentCall *call,char *out,size_t cap,AgentJournal journal,void *ctx)
{
    int r=tools_execute_recorded(call,out,cap,journal,ctx);
    unsigned int steps=0;
    while(r==2) {
        steps++;assert(steps<10000);
        r=tools_text_step(out,cap,(uint32_t)TickCount(),0);
    }
    return r;
}
#define tools_execute_recorded test_execute_recorded
#define tools_execute(call,out,cap) test_execute_recorded(call,out,cap,NULL,NULL)
#endif
