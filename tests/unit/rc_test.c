/* RACCOON V2 - the shared unit-test framework.
 *
 * Kept in its own translation unit rather than in the test binary's main file, so the
 * simulator can assert on its scenarios with exactly the same macros the unit tests use.
 * A second, subtly different check macro is worse than no check macro. */
#include "rc_test.h"

#include <stdarg.h>
#include <stdio.h>

int rc_test_failures;
int rc_test_checks;
const char *rc_test_current = "(none)";

void rc_test_fail(const char *file, int line, const char *fmt, ...)
{
    ++rc_test_failures;
    fprintf(stderr, "FAIL  %s\n      at %s:%d\n      ", rc_test_current, file, line);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

void rc_test_case(const char *name)
{
    rc_test_current = name;
}

