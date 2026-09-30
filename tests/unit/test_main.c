/* RACCOON V2 - unit test entry point. */
#include "rc_test.h"

#include "cal_types.h"

int main(void)
{
    fprintf(stderr, "RACCOON V2 firmware test suite\n");
    fprintf(stderr, "  schema image: %u bytes, %u descriptors\n",
            (unsigned)RC_CAL_IMAGE_SIZE, (unsigned)rc_cal_desc_count);

    test_math();
    test_engine();
    test_trigger();
    test_scheduler();
    test_sensors();
    test_fuel();
    test_ignition();
    test_torque();
    test_safety();
    test_idle();
    test_performance();
    test_calibration();
    test_fault_injection();
    test_flash();
    test_link();
    test_link_service();
    test_crypto();
    test_parity();

    fprintf(stderr, "\n%d checks, %d failures\n", rc_test_checks, rc_test_failures);
    return (rc_test_failures == 0) ? 0 : 1;
}
