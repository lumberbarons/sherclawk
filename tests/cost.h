/* Deterministic cost counters for host suites. cost_check appends one row to
 * $SHERCLAWK_COST_LOG, which tools/check.sh and check-transport.sh print as the
 * cost table, or prints it when run standalone, and fails when the measured
 * total exceeds its bound.
 * Bounds are the measured totals: a regression fails, an improvement passes
 * and should tighten the bound. */
#ifndef SHERCLAWK_TEST_COST_H
#define SHERCLAWK_TEST_COST_H
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static void cost_check(const char *label, long value, long bound)
{
    const char *path = getenv("SHERCLAWK_COST_LOG");
    if (!path || !*path) printf("COST %-62s %8ld (bound %ld)\n", label, value, bound);
    else {
        FILE *log = fopen(path, "a");
        assert(log);
        fprintf(log, "%-62s %8ld %8ld\n", label, value, bound);
        fclose(log);
    }
    if (value > bound) {
        fprintf(stderr, "cost regression: %s = %ld exceeds %ld\n", label, value, bound);
        abort();
    }
}
#endif
