/* Reuse the File Manager fault model to check descriptor rejection and source
 * revision binding before publication. Native compiler behavior is guest-tested. */
#define main text_tools_main
#include "test_tools.c"
#undef main
#include "build_project.h"
static int build_journals;
static int build_journal(void *ctx,const char *event,const char *json)
{
    JsonToken t[128]; (void)ctx;(void)event;
    assert(json_parse(json,strlen(json),t,128)>0); build_journals++;
    return 0;
}
static int fixture(const char *descriptor_text)
{
    int d,i,w,q; reset();build_journals=0;
    d=add(10,"project",1); i=add(files[d].id,"project.json",0);
    strcpy(files[i].bytes,descriptor_text);files[i].size=(long)strlen(descriptor_text);files[i].info.fdType='TEXT';
    i=add(files[d].id,"main.c",0);strcpy(files[i].bytes,"int main(void) { return 0; }\r");files[i].size=(long)strlen(files[i].bytes);files[i].info.fdType='TEXT';
    w=add(10,"Worker01",1);q=add(files[w].id,"buildjobs",1);(void)q;
    strcpy(call.arguments,"{\"path\":\"project\"}");
    assert(build_project_begin(&call,result,sizeof(result),build_journal,NULL,1)==2);return i;
}
static int record_file(long parent,const char *name,const char *bytes)
{
    int i=add(parent,name,0);strcpy(files[i].bytes,bytes);files[i].size=(long)strlen(bytes);files[i].info.fdType='TEXT';return i;
}
static void terminal_check(const char *good,int artifact_ok)
{
    int i,r=2,dir,ready=-1;char terminal[200],build_id[25];
    fixture(good);
    for(i=0;i<100 && ready<0;i++) {
        assert(build_project_step(result,sizeof(result),(uint32_t)i+2,0)==2);
        for(int k=0;k<64;k++)if(files[k].used && !strcmp(files[k].name,"ready"))ready=k;
    }
    assert(ready>=0);dir=files[ready].parent;
    for(i=0;i<64;i++)if(files[i].used && files[i].id==dir)break;
    strcpy(build_id,files[i].name);
    snprintf(terminal,sizeof(terminal),"protocol=1\nid=%s\noutcome=succeeded\nexit=0\nsignal=0\nwait_status=0\n",build_id);
    record_file(dir,"result",terminal);
    i=add(dir,"build",1);i=add(files[i].id,"native",1);dir=files[i].id;
    record_file(dir,"success.txt","artifact=sample\n");
    i=record_file(dir,"sample","PEF bytes");files[i].info.fdType='APPL';files[i].resource=artifact_ok ? 1 : 0;
    r=build_project_step(result,sizeof(result),500,0);
    assert(artifact_ok ? r==0 && strstr(result,"\"status\":\"ok\"") && strstr(result,"native:sample") : r==1 && strstr(result,"ARTIFACT_INVALID"));
}
int main(void)
{
    const char *good="{\"protocol\":2,\"toolchain\":\"mpw-ppc-v2\",\"sources\":[\"main.c\"],\"output\":\"sample\"}\r";
    char recipe[12288];int i,r,steps;
    assert(!build_project_recipe(good,recipe,sizeof(recipe)));
    assert(strstr(recipe,"MrC") && strstr(recipe,"PPCLink -o sample") && !strstr(recipe,"-- Rez"));
    assert(build_project_recipe("{\"protocol\":1}",recipe,sizeof(recipe)));
    const char *bad[]={"main.c;echo","..:main.c",":main.c","src::main.c","src/main.c","main.C","Main.c"};
    for(i=0;i<7;i++) {
        char s[512];snprintf(s,sizeof(s),"{\"protocol\":2,\"toolchain\":\"mpw-ppc-v2\",\"sources\":[\"%s\"],\"output\":\"sample\"}",bad[i]);assert(build_project_recipe(s,recipe,sizeof(recipe)));
    }
    assert(build_project_recipe("{\"protocol\":2,\"toolchain\":\"mpw-ppc-v2\",\"sources\":[\"main.c\",\"main.c\"],\"output\":\"sample\"}",recipe,sizeof(recipe)));
    assert(build_project_recipe("{\"protocol\":2,\"toolchain\":\"mpw-ppc-v2\",\"sources\":[\"main.c\"],\"output\":\"sample\",\"settings\":{\"flags\":\"-o evil\"}}",recipe,sizeof(recipe)));
    assert(build_project_recipe("{\"protocol\":2,\"toolchain\":\"mpw-ppc-v2\",\"sources\":[\"main.c\"],\"output\":\"sample\",\"settings\":{\"libraries\":[\"OtherLib\"]}}",recipe,sizeof(recipe)));
    assert(build_project_recipe("{\"protocol\":2,\"protocol\":2,\"toolchain\":\"mpw-ppc-v2\",\"sources\":[\"main.c\"],\"output\":\"sample\"}",recipe,sizeof(recipe)));
    i=fixture(good);assert(build_project_step(result,sizeof(result),2,0)==2);assert(build_project_step(result,sizeof(result),3,0)==2);
    assert(build_project_step(result,sizeof(result),4,0)==2);files[i].bytes[0]='x';
    assert(build_project_step(result,sizeof(result),5,0)==0 && strstr(result,"INPUT_CHANGED") && !creates && !build_journals);
    fixture(good);assert(build_project_step(result,sizeof(result),2,1)==1);
    strcpy(call.arguments,"{\"path\":\"project:\"}");assert(build_project_begin(&call,result,sizeof(result),build_journal,NULL,3)==2);
    assert(build_project_step(result,sizeof(result),4,1)==1);
    fixture(good);bad_close=1;assert(build_project_step(result,sizeof(result),2,0)==0 && strstr(result,"UNREADABLE") && !creates);
    fixture(good);assert(build_project_step(result,sizeof(result),2,1)==1 && strstr(result,"ABANDONED") && !creates);
    fixture(good);r=2;
    for(steps=0;r==2 && steps<100;steps++)r=build_project_step(result,sizeof(result),(uint32_t)(steps+2),0);
    assert(r==2);assert(build_project_step(result,sizeof(result),200,1)==1 && strstr(result,"uncertain"));
    assert(build_journals>=5);
    terminal_check(good,0);terminal_check(good,1);
    { char bid[25];int dir=0,log;JsonToken t[64];
      field("build_id",bid,sizeof(bid));
      for(i=0;i<64;i++)if(files[i].used && !strcmp(files[i].name,bid))dir=(int)files[i].id;
      assert(dir);log=record_file(dir,"stdout","");memset(files[log].bytes,'\r',512);files[log].size=512;
      snprintf(call.arguments,sizeof(call.arguments),"{\"build_id\":\"%s\",\"stream\":\"stdout\"}",bid);
      build_project_log(&call,result,sizeof(result));assert(json_parse(result,strlen(result),t,64)>0 && strstr(result,"\"next_byte\":128") && strstr(result,"\"truncated\":true"));
      files[log].info.fdFlags=0x8000;build_project_log(&call,result,sizeof(result));assert(strstr(result,"error"));
    }
    puts("PASS build descriptors, metacharacters, duplicate/unsupported settings, source changes, read/close faults, snapshot publication and Stop");
    return 0;
}
