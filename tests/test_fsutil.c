/* Direct fault model for fs_write_verified and fs_verify_body: every step of
 * the ladder must fail closed, and out_info must only describe a verified
 * body. Callers' own suites cover their journals and renames. */
#include "tools/fsutil.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CAP 9000
struct File { int used; long size, resource; unsigned char attrib; unsigned short flags;
    unsigned long type, creator; unsigned char name[32]; char bytes[CAP]; };
static struct File files[4];
static long position[4];
static int short_write, close_error, flush_error, corrupt_read, eof_with_data, open_count;
static int find(const unsigned char *name)
{
    int i; for(i=0;i<4;i++) if(files[i].used && !memcmp(files[i].name,name,(size_t)name[0]+1)) return i;
    return -1;
}
OSErr PBGetCatInfoSync(CInfoPBRec *p)
{
    int i=find(p->hFileInfo.ioNamePtr); if(i<0) return fnfErr;
    p->hFileInfo.ioFlAttrib=files[i].attrib; p->hFileInfo.ioFlRLgLen=files[i].resource;
    p->hFileInfo.ioFlFndrInfo.fdFlags=files[i].flags; p->hFileInfo.ioFlFndrInfo.fdType=files[i].type;
    p->hFileInfo.ioFlFndrInfo.fdCreator=files[i].creator; p->hFileInfo.ioFlLgLen=files[i].size;
    return 0;
}
OSErr FSpCreate(const FSSpec *s,unsigned long creator,unsigned long type,short script)
{
    int i; (void)script; if(find(s->name)>=0) return dupFNErr;
    for(i=0;i<4;i++) if(!files[i].used) {
        memset(&files[i],0,sizeof(files[i])); files[i].used=1; files[i].type=type;
        files[i].creator=creator; memcpy(files[i].name,s->name,(size_t)s->name[0]+1); return 0;
    }
    abort();
}
OSErr FSpOpenDF(const FSSpec *s,short mode,short *ref)
{
    int i=find(s->name); (void)mode; if(i<0) return fnfErr;
    *ref=(short)i; position[i]=0; open_count++; return 0;
}
OSErr FSWrite(short ref,long *n,const void *bytes)
{
    if(short_write && *n) --*n;
    assert(position[ref]+*n<=CAP); memcpy(files[ref].bytes+position[ref],bytes,(size_t)*n);
    position[ref]+=*n; if(position[ref]>files[ref].size) files[ref].size=position[ref]; return 0;
}
OSErr FSRead(short ref,long *n,void *bytes)
{
    long avail=files[ref].size-position[ref]; OSErr e=0;
    if(*n>avail) *n=avail;
    memcpy(bytes,files[ref].bytes+position[ref],(size_t)*n); position[ref]+=*n;
    if(corrupt_read && *n) ((char *)bytes)[0]^=1;
    if(eof_with_data && position[ref]==files[ref].size) e=eofErr;
    return e;
}
OSErr FSClose(short ref) { (void)ref; open_count--; return close_error ? ioErr : 0; }
OSErr FlushVol(const unsigned char *name,short v) { (void)name;(void)v; return flush_error ? ioErr : 0; }
static FSSpec dest={1,1,{4,'t','e','s','t'}};
static char body[6000];
static void reset(void)
{
    memset(files,0,sizeof(files)); short_write=close_error=flush_error=corrupt_read=eof_with_data=open_count=0;
    memset(body,'x',sizeof(body)); body[5999]='y'; /* spans two read-back chunks */
}
static OSErr write_body(CInfoPBRec *info) { return fs_write_verified(&dest,'ShCk','ShRR',body,sizeof(body),info); }
static void sentinel(CInfoPBRec *info) { memset(info,0x5a,sizeof(*info)); }
static int untouched(const CInfoPBRec *info) { return ((const unsigned char *)info)[0]==0x5a && ((const unsigned char *)info)[sizeof(*info)-1]==0x5a; }
static void identity_and_revision(void)
{
    CInfoPBRec a, b;
    char revision[80];
    memset(&a,0,sizeof(a));a.hFileInfo.ioDirID=11;a.hFileInfo.ioFlMdDat=1234;
    a.hFileInfo.ioFlLgLen=5;a.hFileInfo.ioFlFndrInfo.fdType='TEXT';
    a.hFileInfo.ioFlFndrInfo.fdCreator='ttxt';b=a;
    assert(fs_same_file(&a,&b) && !fs_is_alias(&a));
    b.hFileInfo.ioFlAttrib=0x60;assert(fs_same_file(&a,&b)); /* transient open bits */
    b=a;b.hFileInfo.ioFlAttrib=1;assert(!fs_same_file(&a,&b));
    b=a;b.hFileInfo.ioFlAttrib=16;assert(!fs_same_file(&a,&b));
    b=a;b.hFileInfo.ioDirID++;assert(!fs_same_file(&a,&b));
    b=a;b.hFileInfo.ioFlMdDat++;assert(!fs_same_file(&a,&b));
    b=a;b.hFileInfo.ioFlLgLen++;assert(!fs_same_file(&a,&b));
    b=a;b.hFileInfo.ioFlRLgLen++;assert(!fs_same_file(&a,&b));
    b=a;b.hFileInfo.ioFlFndrInfo.fdCreator='Oops';assert(!fs_same_file(&a,&b));
    b=a;b.hFileInfo.ioFlFndrInfo.fdFlags=0x4000;assert(!fs_same_file(&a,&b));
    b=a;b.hFileInfo.ioFlFndrInfo.fdFlags=0x8000;assert(fs_is_alias(&b) && !fs_same_file(&a,&b));
    b=a;b.hFileInfo.ioFlFndrInfo.fdType='alis';assert(fs_is_alias(&b) && !fs_same_file(&a,&b));
    assert(fs_fnv1a(NULL,0)==0x811c9dc5UL && fs_fnv1a("hello",5)==0x4f9f2cabUL);
    unsigned char high[]={0,0x80,0xff};
    assert(fs_fnv1a(high,sizeof(high))==0xb0eecb36UL);
    assert(fs_fnv1a_more(fs_fnv1a("he",2),"llo",3)==0x4f9f2cabUL);
    fs_revision(&a,"hello",5,revision,sizeof(revision));
    assert(!strcmp(revision,"full-0000000b-000004d2-00000005-4f9f2cab"));
    fs_revision_hash(&a,5,fs_fnv1a_more(fs_fnv1a("he",2),"llo",3),revision,sizeof(revision));
    assert(!strcmp(revision,"full-0000000b-000004d2-00000005-4f9f2cab"));
}

int main(void)
{
    identity_and_revision();
    CInfoPBRec info;
    reset(); sentinel(&info);
    assert(write_body(&info)==noErr && open_count==0 && !untouched(&info));
    assert(info.hFileInfo.ioFlLgLen==(long)sizeof(body) && info.hFileInfo.ioFlFndrInfo.fdType=='ShRR');
    assert(write_body(NULL)==dupFNErr); /* the name is taken: never overwrite */
    assert(fs_verify_body(&dest,'ShCk','ShRR',body,sizeof(body))==noErr);
    { char other[sizeof(body)]; memcpy(other,body,sizeof(body)); other[5999]='z';
      assert(fs_verify_body(&dest,'ShCk','ShRR',other,sizeof(other))==ioErr); }
    assert(fs_verify_body(&dest,'ShCk','ShRR',body,sizeof(body)-1)==ioErr); /* length proof */
    assert(fs_verify_body(&dest,'Oops','ShRR',body,sizeof(body))==ioErr);
    assert(fs_verify_body(&dest,'ShCk','Oops',body,sizeof(body))==ioErr);
    files[0].resource=1; assert(fs_verify_body(&dest,'ShCk','ShRR',body,sizeof(body))==ioErr);
    files[0].resource=0; files[0].flags=0x8000; assert(fs_verify_body(&dest,'ShCk','ShRR',body,sizeof(body))==ioErr);
    files[0].flags=0; files[0].attrib=16; assert(fs_verify_body(&dest,'ShCk','ShRR',body,sizeof(body))==ioErr);
    files[0].attrib=0; assert(fs_verify_body(&dest,'ShCk','ShRR',body,sizeof(body))==noErr && open_count==0);

    reset(); sentinel(&info); short_write=1;
    assert(write_body(&info)==ioErr && untouched(&info) && open_count==0);
    reset(); sentinel(&info); close_error=1;
    assert(write_body(&info)==ioErr && untouched(&info));
    reset(); sentinel(&info); flush_error=1;
    assert(write_body(&info)==ioErr && untouched(&info));
    reset(); sentinel(&info); corrupt_read=1; /* the catalog is right, the bytes are not */
    assert(write_body(&info)==ioErr && untouched(&info) && open_count==0);

    /* A read ending exactly at EOF may carry eofErr with the full count. */
    reset(); eof_with_data=1; assert(write_body(&info)==noErr);
    assert(fs_verify_body(&dest,'ShCk','ShRR',body,sizeof(body))==noErr);
    puts("PASS fs_write_verified: create, write, flush, catalog proof, read-back, close faults and out_info");
    return 0;
}
