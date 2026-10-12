/* Small argument/result boundaries used by every executor and provider path. */
#include "jsonargs.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
static JsonToken tokens[64];
static void parse(const char *s) { assert(json_parse(s,strlen(s),tokens,64)>0); }
static void arguments(void)
{
    char value[8]; long n=123;
    const char *s="{\"path\":\"caf\\u00e9\",\"settings\":{\"limit\":4}}";
    parse(s);
    assert(!json_args_check_keys(s,tokens,0,"|path||settings|"));
    assert(!json_args_check_keys(s,tokens,json_member(s,tokens,0,"settings"),"|limit|"));
    assert(json_args_string(s,tokens,"path",value,sizeof(value))==5 && !strcmp(value,"caf\xc3\xa9"));
    assert(json_args_string(s,tokens,"absent",value,sizeof(value))<0);
    assert(json_args_check_keys(s,tokens,-1,"|path|")<0);
    const char *bad[]={"{\"path\":1,\"p\\u0061th\":2}","{\"path|text\":1}","{\"unknown\":0}","{\"\":0}","[]"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        parse(bad[i]); assert(json_args_check_keys(bad[i],tokens,0,"|path||text|")<0);
    }
    parse("{}"); assert(!json_args_int("{}",tokens,"n",7,0,9,&n) && n==7);
    const char *numbers[]={"{\"n\":0}","{\"n\":9}","{\"n\":-3}"};
    for(int i=0;i<3;i++) {parse(numbers[i]);assert(!json_args_int(numbers[i],tokens,"n",0,-3,9,&n) && n==(i==0 ? 0 : i==1 ? 9 : -3));}
    const char *invalid[]={"{\"n\":-1}","{\"n\":10}","{\"n\":1.0}","{\"n\":1e0}","{\"n\":\"1\"}","{\"n\":true}","{\"n\":999999999999999999999999999999}"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);i++) {
        parse(invalid[i]);n=123;assert(json_args_int(invalid[i],tokens,"n",0,0,9,&n)<0 && n==123);
    }
    char number[64];snprintf(number,sizeof(number),"{\"n\":%ld}",LONG_MIN);parse(number);
    assert(!json_args_int(number,tokens,"n",0,LONG_MIN,LONG_MAX,&n) && n==LONG_MIN);
    snprintf(number,sizeof(number),"{\"n\":%ld}",LONG_MAX);parse(number);
    assert(!json_args_int(number,tokens,"n",0,LONG_MIN,LONG_MAX,&n) && n==LONG_MAX);
}
static void buffers(void)
{
    char out[16]="", decoded[16];size_t at=0;
    JsonBuf b={out,sizeof(out),&at};
    assert(!json_buf_append(&b,"[") && at==1);
    assert(!json_buf_quote(&b,"a\"\n") && at==12);
    assert(!json_buf_append_n(&b,"]ignored",1) && !strcmp(out,"[\"a\\\"\\u000a\"]"));
    parse(out);assert(json_string(out,tokens,1,decoded,sizeof(decoded))==3 && !strcmp(decoded,"a\"\n"));
    at=15;out[15]=0;assert(json_buf_append(&b,"x")<0 && at==15);
    at=16;assert(json_buf_quote(&b,"x")<0 && at==16);
    at=(size_t)-1;assert(json_buf_quote(&b,"x")<0 && json_buf_append(&b,"x")<0 && at==(size_t)-1);
    at=0;b.capacity=0;assert(json_buf_quote(&b,"")<0 && json_buf_append(&b,"")<0 && !at);
    b.capacity=4;assert(!json_buf_quote(&b,"x") && at==3 && !strcmp(out,"\"x\""));
    at=0;b.capacity=3;assert(json_buf_quote(&b,"x")<0 && !at);
    char error[256];assert(json_args_error(error,sizeof(error),"READ","quote \" and newline\n",-36)==-1);
    parse(error);assert(json_args_string(error,tokens,"message",decoded,sizeof(decoded))<0); /* small output refuses truncation */
    char message[64];assert(json_args_string(error,tokens,"message",message,sizeof(message))>0 && !strcmp(message,"quote \" and newline\n"));
}
int main(void) { arguments();buffers();puts("PASS shared JSON arguments: decoded keys, integer bounds, overflow and buffer cursors");return 0; }
