/* Real File Manager executor with controlled asynchronous replies. Exercise
 * start refusals (busy, blocked, unavailable), ownership races, incompatible
 * outputs, snapshot corruption and late replies. */
#define TEST_SELFBUILD 1
#include "test_build_project.c"
#undef main
static const char good[]="{\"protocol\":2,\"toolchain\":\"mpw-ppc-v2\",\"sources\":[\"main.c\"],\"output\":\"sample\"}\r";
static int named(const char *name)
{ int i;for(i=0;i<64;i++)if(files[i].used && !strcmp(files[i].name,name))return i;return -1; }
static uint32_t ticks;
static void setup(void)
{
    selfbuild_close();ts_busy=ts_ready=ts_sends=ts_abandoned=ts_failure=ts_malformed=ts_send_error=ts_unreachable=0;
    fixture(good);ticks=2;assert(!selfbuild_init());
}
static void until_send(void)
{
    int r=2,turns=0;
    while(r==2 && !ts_busy && turns++<200)r=build_project_step(result,sizeof(result),ticks++,0);
    assert(r==2 && ts_busy && ts_sends==1 && named("worker-lock")>=0 && named("claimed")>=0);
}
static int run_to_end(void)
{
    int r=2,turns;
    for(turns=0;r==2 && turns<200;turns++)r=build_project_step(result,sizeof(result),ticks++,0);
    return r;
}
static void drain(void) {ts_ready=1;selfbuild_drain(ticks++);assert(!ts_busy && named("worker-lock")<0 && named("native-drained")>=0);
    assert(strstr(files[named("native-drained")].bytes,"raw_status=0\nabandoned=1\n"));}
int main(void)
{
    int r,i,turns;
    BuildPlan plan;char command[2048],stage[160],recipe[12288];
    assert(!build_project_plan(good,&plan));
    assert(!build_project_command(&plan,0,command,sizeof(command),stage,sizeof(stage)));
    assert(!build_project_recipe(good,recipe,sizeof(recipe)) && strstr(recipe,"MrC") && strstr(command,"MrC"));
    assert(build_project_command(&plan,2,command,sizeof(command),stage,sizeof(stage)));
    setup();until_send();
    r=build_project_step(result,sizeof(result),ticks++,1);
    assert(r==1 && strstr(result,"uncertain") && ts_busy && ts_sends==1 && named("worker-lock")>=0 && named("result")<0);
    /* The undrained command still owns the executor: the next build is refused
     * before anything is reserved, and the late reply is not disturbed. */
    strcpy(call.arguments,"{\"path\":\"project\"}");
    r=build_project_begin(&call,result,sizeof(result),build_journal,NULL,ticks);assert(r==2);
    assert(run_to_end()==1 && strstr(result,"NATIVE_EXECUTOR_BUSY") && strstr(result,"\"message\"") &&
           ts_sends==1 && ts_busy && named("worker-lock")>=0);
    {   int jobs=0,k;for(k=0;k<64;k++)if(files[k].used && !strncmp(files[k].name,"build-",6))jobs++;
        assert(jobs==1); }
    drain();assert(ts_sends==1 && named("claimed")>=0 && named("success.txt")<0);
    setup();
    while(named("ready")<0)assert(build_project_step(result,sizeof(result),ticks++,0)==2);
    i=named("ready");record_file(files[i].parent,"stdout","old evidence");
    assert(build_project_step(result,sizeof(result),ticks++,0)==1 && !ts_sends && named("worker-lock")<0);
    assert(named("ready")>=0 && named("claimed")<0 && named("stdout")>=0);
    setup();
    while(named("ready")<0)assert(build_project_step(result,sizeof(result),ticks++,0)==2);
    /* Another party wins the worker-lock creation race: nothing is claimed and
     * their lock is left exactly as found. */
    dir_race=1;
    assert(build_project_step(result,sizeof(result),ticks++,0)==1 && strstr(result,"NATIVE_QUEUE_BLOCKED") && !ts_sends && named("claimed")<0);
    dir_race=0;
    assert(named("ready")>=0 && named("worker-lock")>=0 && !files[named("worker-lock")].dir);
    /* ToolServer cannot be found or launched: nothing ran, so the claim ends
     * in a definite rejected record with the lock released. */
    setup();ts_unreachable=1;
    assert(run_to_end()==1 && strstr(result,"NATIVE_EXECUTOR_UNAVAILABLE") && strstr(result,"\"status\":\"error\""));
    assert(!ts_sends && !ts_busy && named("worker-lock")<0 && named("claimed")>=0 && named("success.txt")<0);
    i=named("result");assert(i>=0 && strstr(files[i].bytes,"outcome=rejected\n"));
    i=named("stderr");assert(i>=0 && strstr(files[i].bytes,"no command ran"));
    setup();ts_send_error=1;r=2;
    for(turns=0;r==2 && turns<200;turns++)r=build_project_step(result,sizeof(result),ticks++,0);
    assert(r==1 && ts_busy && ts_sends==1 && named("worker-lock")>=0);drain();
    setup();until_send();ts_ready=1;ts_failure=2;r=2;
    for(turns=0;r==2 && turns<100;turns++)r=build_project_step(result,sizeof(result),ticks+=60,0);
    assert(r==0 && strstr(result,"\"exit\":1") && ts_sends==1 && named("success.txt")<0 && named("worker-lock")<0);
    i=named("result");assert(i>=0 && strstr(files[i].bytes,"outcome=failed\nexit=1\n"));
    setup();until_send();ts_ready=1;ts_malformed=1;
    assert(build_project_step(result,sizeof(result),ticks++,0)==1 && strstr(result,"uncertain"));
    assert(named("result")<0 && ts_sends==1 && named("worker-lock")<0);
    /* An existing worker-lock or a STOP marker refuses the build up front. */
    setup();i=named("buildjobs");assert(i>=0);add(files[i].id,"worker-lock",1);
    assert(run_to_end()==1 && strstr(result,"NATIVE_QUEUE_BLOCKED") && strstr(result,"\"status\":\"error\""));
    assert(!ts_sends && named("ready")<0 && named("claimed")<0 && named("input0.c")<0 && named("worker-lock")>=0);
    setup();i=named("buildjobs");add(files[i].id,"STOP",0);
    assert(run_to_end()==1 && strstr(result,"NATIVE_QUEUE_BLOCKED"));
    assert(!ts_sends && named("ready")<0 && named("worker-lock")<0 && named("STOP")>=0);
    /* The same markers appearing after publication leave an inert, unclaimed
     * snapshot and an error rather than an uncertain build. */
    setup();
    while(named("ready")<0)assert(build_project_step(result,sizeof(result),ticks++,0)==2);
    i=named("buildjobs");add(files[i].id,"worker-lock",1);
    assert(run_to_end()==1 && strstr(result,"NATIVE_QUEUE_BLOCKED") && strstr(result,"\"status\":\"error\""));
    assert(!ts_sends && named("ready")>=0 && named("claimed")<0 && named("result")<0 && files[named("worker-lock")].dir);
    setup();
    while(named("ready")<0)assert(build_project_step(result,sizeof(result),ticks++,0)==2);
    i=named("buildjobs");add(files[i].id,"STOP",0);
    assert(run_to_end()==1 && strstr(result,"NATIVE_QUEUE_BLOCKED"));
    assert(!ts_sends && named("ready")>=0 && named("claimed")<0 && named("worker-lock")<0);
    /* ToolServer never initialized: refused before reserving the job. */
    setup();selfbuild_close();
    assert(run_to_end()==1 && strstr(result,"NATIVE_EXECUTOR_UNAVAILABLE") && strstr(result,"ToolServer"));
    assert(!ts_sends && named("ready")<0 && named("input0.c")<0 && named("worker-lock")<0);
    setup();
    while(named("ready")<0)assert(build_project_step(result,sizeof(result),ticks++,0)==2);
    i=named("input0.c");assert(i>=0);files[i].bytes[0]^=1;r=2;
    for(turns=0;r==2 && turns<100;turns++)r=build_project_step(result,sizeof(result),ticks++,0);
    assert(r==1 && !ts_sends && named("result")<0 && named("worker-lock")<0);
    setup();until_send();
    assert(build_project_step(result,sizeof(result),19000,0)==1 && ts_busy && ts_sends==1);
    drain();
    setup();until_send();ts_ready=1;
    while(ts_sends<2)assert(build_project_step(result,sizeof(result),ticks++,0)==2);
    i=named("native");assert(i>=0);i=record_file(files[i].id,"sample","Joy!peffpwpc............................");files[i].resource=100;
    ts_ready=1;r=2;
    for(turns=0;r==2 && turns<150;turns++)r=build_project_step(result,sizeof(result),ticks+=60,0);
    assert(r==0 && strstr(result,"\"status\":\"ok\"") && ts_sends==2 && named("launch.rec")>=0 && named("success.txt")>=0 && named("worker-lock")<0);
    selfbuild_close();
    puts("PASS self-build busy/blocked/unavailable refusals, paged snapshot verification, raw-status mapping, failure, Stop/deadline and late reply draining, persisted authorization");
    return 0;
}
