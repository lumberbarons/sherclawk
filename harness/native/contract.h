#ifndef HARNESS_CONTRACT_H
#define HARNESS_CONTRACT_H
#include <stddef.h>
/* Public v1 wire contract. ASCII, CR records; absolute HFS paths <= 220 bytes. */
#define H_REQUEST_CAP 1024
#define H_REPORT_CAP 32768
#define H_CHECK_MAX 256
#define H_NAME_CAP 121
typedef struct {
    char slot[6], generation[33], boot[33], run[33], diagnostic[32];
    char application[221], workspace[221];
    unsigned long deadline;
} HRequest;
typedef struct {
    HRequest request;
    char outcome[16];
    unsigned int total, failures;
    struct { int passed; char name[H_NAME_CAP]; } checks[H_CHECK_MAX];
} HReport;
int h_request_parse(const char *, size_t, HRequest *);
int h_request_format(const HRequest *, char *, size_t);
int h_request_equal(const HRequest *, const HRequest *);
int h_report_parse(const char *, size_t, const HRequest *, HReport *);
int h_report_format(const HReport *, char *, size_t);
int h_report_check(HReport *, int, const char *);
/* The registered diagnostics; harness/contract.py DIAGNOSTICS must match. */
extern const char *const h_diagnostics[];
extern const unsigned int h_diagnostic_count;
int h_registered(const char *);
#endif
