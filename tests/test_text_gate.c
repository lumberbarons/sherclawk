/* Shipping tools retain the guest-verified cap while acceptance targets
 * exercise 64 KiB. Reuse the same File Manager model without enabling it. */
#define main large_text_acceptance_main
#include "test_tools.c"
#undef main
int main(void)
{
    int f;static char source[4098];
    assert(TOOLS_ACCEPTED_FILE_CAP==4096);
    memset(source,'x',4096);source[4095]='z';source[4096]=0;
    edit_setup(source,"z","y");assert(!run() && strstr(result,"EDITED"));
    reset();f=add(10,"hello.c",0);files[f].size=4097;files[f].info.fdType='TEXT';memset(files[f].bytes,'x',4097);
    strcpy(call.name,"read_text");strcpy(call.arguments,"{\"path\":\"hello.c\"}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"\"editable\":false") && strstr(result,"\"revision_scope\":\"scan\""));
    strcpy(call.name,"edit_text");strcpy(call.arguments,"{\"path\":\"hello.c\",\"expected_revision\":\"full-test\",\"old_text\":\"x\",\"new_text\":\"y\"}");
    assert(!run() && strstr(result,"LIMIT") && !creates);handles_closed();
    strcpy(call.name,"get_environment");strcpy(call.arguments,"{}");tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"edit_max_bytes\":4096") && strstr(result,"\"build_input_max_bytes\":4096"));
    puts("PASS shipping text limit remains gated at 4096 until guest acceptance");return 0;
}
