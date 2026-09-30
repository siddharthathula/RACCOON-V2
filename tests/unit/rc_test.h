/* RACCOON V2 - minimal test framework.
 *
 * Deliberately tiny: a test binary that links nothing but libc and the shipping firmware,
 * so "the tests pass" means the product code is correct rather than that a test framework
 * is configured correctly.  Every check reports file, line and both values, because a
 * failure at 2 AM has to be diagnosable from the log alone.
 */
#ifndef RC_TEST_H
#define RC_TEST_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int rc_test_failures;
extern int rc_test_checks;
extern const char *rc_test_current;

void rc_test_fail(const char *file, int line, const char *fmt, ...);
void rc_test_case(const char *name);

/** Like RC_CHECK, but abandons the case: use before dereferencing a pointer that a failed
 *  check has just proven may be NULL, because RC_CHECK records and carries on. */
#define RC_REQUIRE_MSG(cond, ...)                                                \
    do {                                                                      \
        if (!(cond)) {                                                       \
            RC_CHECK_MSG(0, __VA_ARGS__);                                   \
            return;                                                          \
        }                                                                     \
    } while (0)

#define RC_REQUIRE(cond)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                       \
            RC_CHECK(cond);                                                  \
            return;                                                          \
        }                                                                     \
    } while (0)

#define RC_CHECK_EQ_I(got, want)                                                  \
    RC_CHECK_MSG((long)(got) == (long)(want), "got %ld, expected %ld", (long)(got),  \
                 (long)(want))

#define RC_CHECK(cond)                                                        \
    do {                                                                      \
        ++rc_test_checks;                                                     \
        if (!(cond)) {                                                        \
            rc_test_fail(__FILE__, __LINE__, "expected: %s", #cond);          \
        }                                                                     \
    } while (0)

#define RC_CHECK_MSG(cond, ...)                                               \
    do {                                                                      \
        ++rc_test_checks;                                                     \
        if (!(cond)) {                                                        \
            rc_test_fail(__FILE__, __LINE__, __VA_ARGS__);                    \
        }                                                                     \
    } while (0)

#define RC_CHECK_EQ_U(actual, expected)                                       \
    do {                                                                      \
        ++rc_test_checks;                                                     \
        const unsigned long long a_ = (unsigned long long)(actual);          \
        const unsigned long long e_ = (unsigned long long)(expected);        \
        if (a_ != e_) {                                                       \
            rc_test_fail(__FILE__, __LINE__, "%s: got %llu, expected %llu",   \
                         #actual, a_, e_);                                    \
        }                                                                     \
    } while (0)

#define RC_CHECK_NEAR(actual, expected, tol)                                  \
    do {                                                                      \
        ++rc_test_checks;                                                     \
        const double a_ = (double)(actual);                                   \
        const double e_ = (double)(expected);                                 \
        const double t_ = (double)(tol);                                      \
        if (!(fabs(a_ - e_) <= t_)) {                                         \
            rc_test_fail(__FILE__, __LINE__, "%s: got %.9g, expected %.9g "   \
                         "+/- %.3g (delta %.3g)", #actual, a_, e_, t_,         \
                         fabs(a_ - e_));                                      \
        }                                                                     \
    } while (0)

#define RC_TEST_CASE(name, fn)                                               \
    do {                                                                      \
        rc_test_case(name);                                                   \
        fn();                                                                 \
    } while (0)

/* Each test file exposes one entry point. */
void test_math(void);
void test_engine(void);
void test_trigger(void);
void test_scheduler(void);
void test_sensors(void);
void test_fuel(void);
void test_ignition(void);
void test_torque(void);
void test_safety(void);
void test_idle(void);
void test_performance(void);
void test_calibration(void);
void test_fault_injection(void);
void test_flash(void);
void test_link(void);
void test_link_service(void);
void test_crypto(void);
void test_parity(void);

#endif /* RC_TEST_H */
