/* RACCOON V2 - simulator entry point.
 *
 * Runs the behavioural scenarios and then a batch of fuzz cases.  Exit code is non-zero if
 * anything crashed, hung, or produced a value that is not a number - which is the whole
 * contract of this program.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "rc_test.h"
#include <string.h>

#include "rc_sim_fuzz.h"
#include "rc_sim_harness.h"
#include "rc_sim_link.h"

/* How long the starter is held before the engine is left to idle on its own. */
#define CRANK_SECONDS 3.0f
#include "rc_sim_scenarios.h"

/* Run as a tuner server until interrupted.
 *
 * This is the mode the GUI and the tests use to drive the real ECU over the real protocol.
 * The port is announced on stdout as "LINK_PORT <n>" and that is how the parent finds it -
 * parsing a line rather than assuming 5000, because a fixed port that is already in use
 * fails in a way that looks like a networking bug.
 */
static int serve_link(int port, unsigned log_hz, const char *unlock_code, int verbose)
{
    static rc_harness_t harness;
    static rc_sim_link_t link;

    /* A 1000 cc four, which is the largest engine this project targets and therefore the
     * one whose defaults are the least likely to be right for whoever is looking at it. */
    rc_harness_init(&harness, 1000.0f, 4u, 24u, 2u, 4u);
    rc_harness_set_armed(&harness, true);
    rc_sim_link_init(&link, &harness, (uint16_t)log_hz, unlock_code);
    if (rc_sim_link_listen(&link, (uint16_t)port) != 0) {
        fprintf(stderr, "could not bind port %d\n", port);
        return 2;
    }
    if (rc_sim_link_control(&link, 0u, &harness) != 0) {
        fprintf(stderr, "could not bind a control port\n");
        return 2;
    }
    /* Both ports announced, on one line each. A parent that only knows how to read one of
     * them still works, and neither has to guess a default that may already be in use. */
    printf("LINK_PORT %u\n", (unsigned)rc_sim_link_port(&link));
    printf("CONTROL_PORT %u\n", (unsigned)rc_sim_link_control_port(&link));
    fflush(stdout);

    /* Crank it, then let it idle with a light throttle.
     *
     * The plant does not turn itself over: an engine that is not running is not a running
     * engine, and every downstream number - closed-loop correction, ignition advance, the
     * whole fuel model - is only meaningful once it is. A link server that came up showing
     * zero rpm would make the dashboard look broken and would hide the behaviour the tuner
     * exists to observe.
     *
     * The sequence runs on virtual time like everything else, so it takes the same three
     * seconds it would on a bench. */
    float elapsed = 0.0f;

    /* Paced against the wall clock.
     *
     * The batch mode of this program deliberately runs as fast as the CPU allows, because a
     * test wants a thousand seconds of engine behaviour in a second. A link server must not:
     * running flat out, its virtual clock advances thousands of times faster than real time,
     * so "20 Hz" live data becomes twenty thousand frames a second. The socket cannot carry
     * that, the reply to a real request queues behind it, and the client hangs on a link that
     * is behaving perfectly. A server that emulates an ECU has to keep the ECU's time. */
    const float dt = 0.002f;   /* 500 Hz: the ECU's own service rate */
    struct timespec next;
    (void)clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        /* The control port owns the rider once it has been used. Before that, the engine
         * cranks itself and then idles, so a tuner that connects to a freshly started server
         * finds a running engine rather than a dead one. */
        if (!link.control_taken) {
            if (elapsed < CRANK_SECONDS) {
                harness.rider.starter_engaged = true;
                harness.rider.clutch_engaged = true;
                harness.rider.gear = 0u;
                harness.rider.throttle_pct = 0.0f;
            } else {
                harness.rider.starter_engaged = false;
                harness.rider.clutch_engaged = true;
                harness.rider.gear = 0u;
                harness.rider.throttle_pct = 12.0f;
            }
        }
        rc_sim_link_control_poll(&link, &harness);
        rc_harness_step(&harness, dt);
        rc_sim_link_tick(&link, dt, &harness);
        elapsed += dt;

        /* Advance the deadline by one step and sleep until it, rather than sleeping a fixed
         * amount per iteration: a fixed sleep makes the rate depend on how long the work
         * took, so a busy moment silently slows simulated time. */
        next.tv_nsec += (long)(dt * 1000000000.0f);
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec += 1;
        }
        /* Re-seed if the iteration overran its slot.
         *
         * TIMER_ABSTIME on its own converts one slow iteration into an unbounded burst: the
         * deadline is already in the past, so the loop runs flat out trying to catch up and
         * never sleeps again. For a link server that is much worse than running slow - the
         * virtual clock races ahead, the configured log rate becomes thousands of frames a
         * second, a client that pauses for a moment fills the socket buffer, and the server
         * declares the link dead and drops it. The client is then told the ECU vanished,
         * which is a symptom with no relationship to the cause. */
        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec
            || (now.tv_sec == next.tv_sec && now.tv_nsec > next.tv_nsec)) {
            next = now;
            next.tv_nsec += (long)(dt * 1000000000.0f);
            if (next.tv_nsec >= 1000000000L) {
                next.tv_nsec -= 1000000000L;
                next.tv_sec += 1;
            }
        }
        (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        if (verbose) {
            fprintf(stderr, "rpm %.0f map %.1f lambda %.3f\n", (double)harness.last_rpm,
                    (double)harness.control.state.load, (double)harness.last_lambda);
        }
    }
    rc_sim_link_close(&link);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned long cases = 200ul;
    int verbose = 0;
    int serve = 0;
    int port = 0;
    unsigned log_hz = 20u;
    const char *unlock_code = "raccoon";
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--cases=", 8) == 0) {
            cases = strtoul(argv[i] + 8, NULL, 10);
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--serve-link") == 0) {
            serve = 1;
        } else if (strncmp(argv[i], "--port=", 7) == 0) {
            port = (int)strtol(argv[i] + 7, NULL, 10);
        } else if (strncmp(argv[i], "--log-hz=", 9) == 0) {
            log_hz = (unsigned)strtoul(argv[i] + 9, NULL, 10);
        } else if (strncmp(argv[i], "--unlock-code=", 14) == 0) {
            unlock_code = argv[i] + 14;
        }
    }
    if (serve) {
        return serve_link(port, log_hz, unlock_code, verbose);
    }

    fprintf(stderr, "RACCOON V2 software-in-the-loop simulator\n");
    fprintf(stderr, "  scenarios...\n");
    sim_all();
    fprintf(stderr, "  scenarios: done (%d checks, %d failures)\n", rc_test_checks,
            rc_test_failures);

    fprintf(stderr, "  fuzzing: %lu cases...\n", cases);
    unsigned long non_finite_cases = 0ul;
    unsigned long stalled_cases = 0ul;
    for (unsigned long i = 0; i < cases; ++i) {
        rc_harness_fuzz_state_t fs;
        fuzz_run_case((uint32_t)(0x9E3779B9ul + (i * 2654435761ul)), 0.05f, &fs);
        if (fs.non_finite) {
            non_finite_cases++;
            fprintf(stderr, "  !! seed %u produced a non-finite command\n", fs.seed);
        }
        if (fs.cycles == 0u) {
            stalled_cases++;
            fprintf(stderr, "  !! seed %u never ran the control loop\n", fs.seed);
        }
        if (verbose) {
            fprintf(stderr, "  seed %u: %u cycles, %u events, peak %.0f rpm%s\n", fs.seed,
                    fs.cycles, fs.events, (double)fs.peak_rpm,
                    fs.non_finite ? "  <-- NON-FINITE" : "");
        }
    }
    fprintf(stderr, "  fuzz: %lu cases, %lu non-finite, %lu stalled\n", cases,
            non_finite_cases, stalled_cases);

    int rc = 0;
    if (rc_test_failures != 0) {
        fprintf(stderr, "FAILED: %d scenario checks\n", rc_test_failures);
        rc = 1;
    }
    if (non_finite_cases != 0ul) {
        fprintf(stderr, "FAILED: %lu fuzz cases produced non-finite values\n",
                non_finite_cases);
        rc = 1;
    }
    if (stalled_cases != 0ul) {
        fprintf(stderr, "FAILED: %lu fuzz cases stalled\n", stalled_cases);
        rc = 1;
    }
    if (rc == 0) {
        fprintf(stderr, "simulator: OK\n");
    }
    return rc;
}
