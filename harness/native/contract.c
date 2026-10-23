#include "contract.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static int line(const char **p, const char *end, char *out, size_t cap)
{
    const char *start = *p;
    size_t n = 0;
    while (*p < end && **p != '\r') {
        unsigned char c = (unsigned char)**p;
        if (c < 32 || c > 126 || ++n >= cap) return 0;
        (*p)++;
    }
    if (*p == end) return 0;
    memcpy(out, start, n); out[n] = 0; (*p)++;
    return 1;
}
static int token(const char *s)
{
    return strlen(s) == 32 && strspn(s, "0123456789abcdef") == 32;
}
static int path(const char *s, int directory)
{
    size_t n = strlen(s), i, component=0;
    for (i=0; i<n; i++) {
        if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) return 0;
        if (s[i] == ':') component=0;
        else if (++component > 31) return 0;
    }
    return n > 2 && n <= 220 && s[0] != ':' && strchr(s, ':') &&
        !strstr(s, "::") && (s[n-1] == ':') == directory;
}
int h_registered(const char *s)
{
    static const char *names[] = {
        "SherclawkWriteCheck", "SherclawkSearchCheck", "SherclawkEditCheck",
        "SherclawkInspectCheck", "SherclawkProjectCheck", "SherclawkScrollCheck",
        "SherclawkHandoffCheck", "SherclawkSelfBuildCheck",
        "SherclawkSelfBuildStopCheck", "HarnessAcceptance"
    };
    unsigned int i;
    for (i = 0; i < sizeof(names)/sizeof(*names); i++) if (!strcmp(s, names[i])) return 1;
    return 0;
}
static int valid(const HRequest *r)
{
    const char *name = strrchr(r->application, ':');
    return (!strcmp(r->slot, "slot1") || !strcmp(r->slot, "slot2")) &&
        token(r->generation) && token(r->boot) && token(r->run) &&
        h_registered(r->diagnostic) && path(r->application, 0) &&
        name && !strcmp(name+1, r->diagnostic) && path(r->workspace, 1) &&
        r->deadline > 0 && r->deadline <= 4294967295UL;
}
static int number(const char *s, unsigned long *n)
{
    char *end;
    if (!*s || strlen(s) > 10 || strspn(s, "0123456789") != strlen(s)) return 0;
    errno=0; *n = strtoul(s, &end, 10);
    return errno != ERANGE && !*end && *n <= 4294967295UL;
}
static int request(const char **p, const char *end, HRequest *r)
{
    char s[32];
    memset(r, 0, sizeof(*r));
    if (!line(p,end,s,sizeof(s)) || strcmp(s,"HARNESS/1") ||
        !line(p,end,r->slot,sizeof(r->slot)) ||
        !line(p,end,r->generation,sizeof(r->generation)) ||
        !line(p,end,r->boot,sizeof(r->boot)) ||
        !line(p,end,r->run,sizeof(r->run)) ||
        !line(p,end,r->diagnostic,sizeof(r->diagnostic)) ||
        !line(p,end,r->application,sizeof(r->application)) ||
        !line(p,end,r->workspace,sizeof(r->workspace)) ||
        !line(p,end,s,sizeof(s)) || !number(s,&r->deadline)) return 0;
    return valid(r);
}
int h_request_parse(const char *data, size_t n, HRequest *r)
{
    const char *p = data, *end = data+n;
    char s[8];
    return n < H_REQUEST_CAP && request(&p,end,r) && line(&p,end,s,sizeof(s)) &&
        !strcmp(s,"END") && p == end;
}
int h_request_format(const HRequest *r, char *out, size_t cap)
{
    int n;
    if (!valid(r)) return -1;
    n = snprintf(out,cap,"HARNESS/1\r%s\r%s\r%s\r%s\r%s\r%s\r%s\r%lu\rEND\r",
        r->slot,r->generation,r->boot,r->run,r->diagnostic,r->application,r->workspace,r->deadline);
    return n >= 0 && (size_t)n < cap ? n : -1;
}
int h_request_equal(const HRequest *a, const HRequest *b)
{
    return !strcmp(a->slot,b->slot) && !strcmp(a->generation,b->generation) &&
        !strcmp(a->boot,b->boot) && !strcmp(a->run,b->run) &&
        !strcmp(a->diagnostic,b->diagnostic) && !strcmp(a->application,b->application) &&
        !strcmp(a->workspace,b->workspace) && a->deadline == b->deadline;
}
int h_report_check(HReport *r, int passed, const char *name)
{
    size_t n = strlen(name), i;
    if (r->total >= H_CHECK_MAX || !n || n >= H_NAME_CAP) return 0;
    for (i=0; i<n; i++) if ((unsigned char)name[i] < 32 || (unsigned char)name[i] > 126) return 0;
    r->checks[r->total].passed = !!passed;
    strcpy(r->checks[r->total++].name,name);
    if (!passed) r->failures++;
    return 1;
}
static int outcome(const HReport *r)
{
    if (!strcmp(r->outcome,"passed")) return r->total > 0 && !r->failures;
    if (!strcmp(r->outcome,"failed")) return r->failures > 0;
    return !strcmp(r->outcome,"harness_error") || !strcmp(r->outcome,"stopped") ||
        !strcmp(r->outcome,"unknown");
}
int h_report_format(const HReport *r, char *out, size_t cap)
{
    int n, k;
    unsigned int i, failures = 0;
    n = h_request_format(&r->request,out,cap);
    if (n < 0 || r->total > H_CHECK_MAX || !outcome(r)) return -1;
    n -= 4; /* replace request END with outcome and assertion records */
    k = snprintf(out+n,cap-(size_t)n,"%s\r",r->outcome);
    if (k < 0 || (size_t)k >= cap-(size_t)n) return -1;
    n += k;
    for (i=0; i<r->total; i++) {
        if (!r->checks[i].passed) failures++;
        k = snprintf(out+n,cap-(size_t)n,"%d %s\r",r->checks[i].passed,r->checks[i].name);
        if (k < 0 || (size_t)k >= cap-(size_t)n) return -1;
        n += k;
    }
    if (failures != r->failures) return -1;
    k = snprintf(out+n,cap-(size_t)n,"TOTAL %u %u\rEND\r",r->total,r->failures);
    return k >= 0 && (size_t)k < cap-(size_t)n ? n+k : -1;
}
int h_report_parse(const char *data, size_t n, const HRequest *expected, HReport *r)
{
    const char *p=data, *end=data+n;
    char s[H_NAME_CAP+16], canonical[64];
    unsigned int total, failures;
    int used;
    memset(r,0,sizeof(*r));
    if (n >= H_REPORT_CAP || !request(&p,end,&r->request) ||
        !h_request_equal(&r->request,expected) || !line(&p,end,r->outcome,sizeof(r->outcome))) return 0;
    while (line(&p,end,s,sizeof(s))) {
        if (!strncmp(s,"TOTAL ",6)) {
            if (sscanf(s,"TOTAL %u %u%n",&total,&failures,&used) != 2 || s[used]) return 0;
            snprintf(canonical,sizeof(canonical),"TOTAL %u %u",total,failures);
            return !strcmp(s,canonical) && total == r->total && failures == r->failures &&
                outcome(r) && line(&p,end,s,sizeof(s)) && !strcmp(s,"END") && p == end;
        }
        if ((s[0] != '0' && s[0] != '1') || s[1] != ' ' || !h_report_check(r,s[0]=='1',s+2)) return 0;
    }
    return 0;
}
