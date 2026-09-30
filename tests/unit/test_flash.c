/* RACCOON V2 - flash transaction tests.
 *
 * The property under test is not "the tune writes and reads back" - that is the easy half.
 * It is what happens when the write is INTERRUPTED, which on a motorcycle is a normal
 * event rather than an edge case. Every test here cuts power at a different point in the
 * transaction and then asks the only question that matters: does the ECU still come up on
 * a tune it can run?
 */
#include "rc_test.h"

#include "cal_access.h"
#include "cal_types.h"
#include "rc/cal.h"
#include "rc/cal_validate.h"
#include "rc/rc_flash.h"

#define SLOT0_BASE 0x00000u
#define SLOT1_BASE 0x10000u
#define MARKER_BASE 0x20000u
#define MEDIUM_SIZE 0x30000u

/* A medium that can be told to fail after N bytes of writing, which is how a power cut is
 * modelled: the write simply stops, halfway, with no error reported - exactly what a
 * brownout does to a flash controller. */
typedef struct {
    uint8_t data[MEDIUM_SIZE];
    uint32_t write_budget;   /**< bytes still writable */
    bool limited;            /**< false = unlimited; a budget of 0 means NO bytes */
    uint32_t bytes_written;
    int writes;
    bool torn;               /**< set once a write has been cut short */
} medium_t;

static medium_t medium;

static int medium_read(void *ctx, uint32_t offset, void *dst, uint32_t len)
{
    (void)ctx;
    if ((size_t)offset + len > sizeof(medium.data)) {
        return RC_HAL_ERR_PARAM;
    }
    memcpy(dst, &medium.data[offset], len);
    return RC_HAL_OK;
}

static int medium_write(void *ctx, uint32_t offset, const void *src, uint32_t len)
{
    (void)ctx;
    if ((size_t)offset + len > sizeof(medium.data)) {
        return RC_HAL_ERR_PARAM;
    }
    /* A cut power: accept the part that fits in the budget and report success for it,
     * because a flash controller mid-brownout does exactly that. */
    if (!medium.limited) {
        memcpy(&medium.data[offset], src, len);
        medium.bytes_written += len;
        medium.writes++;
        return RC_HAL_OK;
    }
    uint32_t take = len;
    if (take > medium.write_budget) {
        take = medium.write_budget;
        medium.torn = true;
    }
    memcpy(&medium.data[offset], src, take);
    medium.write_budget -= take;
    medium.bytes_written += take;
    medium.writes++;
    return RC_HAL_OK;
}

static void erase_medium(void)
{
    memset(&medium, 0, sizeof(medium));
}

static rc_hal_t make_hal(void)
{
    rc_hal_t hal;
    memset(&hal, 0, sizeof(hal));
    hal.storage_read = medium_read;
    hal.storage_write = medium_write;
    return hal;
}

static rc_cal_t a_tune(float bore_mm)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    (void)rc_cal_set_engine_bore_mm(&cal, bore_mm);
    (void)rc_cal_set_engine_cylinder_count(&cal, 2u);
    (void)rc_cal_set_strategy_tdc_verified(&cal, true);
    (void)rc_cal_set_trigger_teeth(&cal, 60u);
    (void)rc_cal_set_trigger_missing_teeth(&cal, 2u);
    return cal;
}

/** Boot the ECU the way real boot code does, and report whether it came up. */
static bool boots_to_a_runnable_tune(rc_cal_t *out, uint8_t *slot)
{
    rc_hal_t hal = make_hal();
    rc_flash_t flash;
    if (rc_flash_init(&flash, &hal, NULL, SLOT0_BASE, SLOT1_BASE, MARKER_BASE) != RC_FLASH_OK) {
        return false;
    }
    if (rc_flash_load_active(&flash, out) != RC_FLASH_OK) {
        return false;
    }
    uint32_t blocking = 0u;
    if (rc_cal_validate(out, NULL, 0u, &blocking) == false) {
        return false;
    }
    if (slot != NULL) {
        *slot = flash.active;
    }
    return true;
}

/* ------------------------------------------------------------------ the basics */

/** A clean write comes up on the new tune, in the OTHER slot from the one it was on. */
static void a_clean_write_activates_the_other_slot(void)
{
    erase_medium();
    rc_hal_t hal = make_hal();
    rc_flash_t flash;
    RC_CHECK_EQ_I(rc_flash_init(&flash, &hal, NULL, SLOT0_BASE, SLOT1_BASE, MARKER_BASE),
                  RC_FLASH_OK);

    /* Boot with nothing written: slot 0, empty. */
    const rc_cal_t first = a_tune(66.0f);
    RC_CHECK_EQ_I(rc_flash_commit(&flash, &first), RC_FLASH_OK);
    RC_CHECK_EQ_U(flash.active, 1u); /* slot 0 was active, so 1 was written */

    const rc_cal_t second = a_tune(78.0f);
    RC_CHECK_EQ_I(rc_flash_commit(&flash, &second), RC_FLASH_OK);
    RC_CHECK_EQ_U(flash.active, 0u); /* back to slot 0 */

    rc_cal_t booted;
    uint8_t slot = 0xFFu;
    RC_CHECK_MSG(boots_to_a_runnable_tune(&booted, &slot), "the ECU did not come up");
    RC_CHECK_EQ_U(slot, 0u);
    RC_CHECK_NEAR(rc_cal_get_engine_bore_mm(&booted), 78.0f, 1e-3);
    /* The version went up, not sideways. */
    RC_CHECK(flash.active_version >= 2u);
}

/** STAGE alone must not change what the ECU is running. That is the entire point of
 *  staging into the inactive slot. */
static void staging_does_not_disturb_the_running_tune(void)
{
    erase_medium();
    rc_hal_t hal = make_hal();
    rc_flash_t flash;
    rc_flash_init(&flash, &hal, NULL, SLOT0_BASE, SLOT1_BASE, MARKER_BASE);
    { const rc_cal_t t = a_tune(66.0f); rc_flash_commit(&flash, &t); }

    rc_cal_t before;
    uint8_t slot_before = 0u;
    RC_REQUIRE(boots_to_a_runnable_tune(&before, &slot_before));

    /* Stage something wildly different. */
    const rc_cal_t staged = a_tune(95.0f);
    RC_CHECK_EQ_I(rc_flash_stage(&flash, &staged), RC_FLASH_OK);

    /* Still running the old tune, from the same slot. */
    rc_cal_t after;
    uint8_t slot_after = 0xFFu;
    RC_CHECK_MSG(boots_to_a_runnable_tune(&after, &slot_after), "staging broke the boot");
    RC_CHECK_EQ_U(slot_after, slot_before);
    RC_CHECK_NEAR(rc_cal_get_engine_bore_mm(&after), 66.0f, 1e-3);
}

/* ------------------------------------------------- the reason any of this exists */

/** The headline property. Power is cut at EVERY possible point in the write, and in every
 *  case the ECU must still come up on a tune it can run.
 *
 *  This is the test that justifies two slots, an image-first write order, and a
 *  CRC-protected header. If it ever fails, the design is wrong - not the test. */
static void a_power_cut_anywhere_in_the_write_leaves_a_bootable_ecu(void)
{
    const uint32_t image_bytes = (uint32_t)sizeof(rc_cal_t);
    uint32_t worst = 0u;

    /* Sweep the cut point across the whole transaction: header, image, tail, and the
     * activation flag, with a step fine enough to land inside individual writes. */
    for (uint32_t cut = 0u; cut <= image_bytes + 4096u; cut += 7u) {
        erase_medium();
        rc_hal_t hal = make_hal();
        rc_flash_t flash;
        rc_flash_init(&flash, &hal, NULL, SLOT0_BASE, SLOT1_BASE, MARKER_BASE);

        /* Get to a known-good running tune first, with no fault injection. */
        {
            const rc_cal_t base = a_tune(66.0f);
            RC_REQUIRE(rc_flash_commit(&flash, &base) == RC_FLASH_OK);
        }
        rc_cal_t good;
        uint8_t good_slot = 0u;
        if (!boots_to_a_runnable_tune(&good, &good_slot)) {
            RC_CHECK_MSG(0, "the fixture itself did not boot at cut %u", cut);
            return;
        }
        const float good_bore = rc_cal_get_engine_bore_mm(&good);

        /* Now the interrupted write. */
        medium.writes = 0u;
        medium.torn = false;
        medium.limited = true;
        medium.write_budget = cut;
        const rc_cal_t newtune = a_tune(84.0f);
        (void)rc_flash_commit(&flash, &newtune);
        medium.limited = false; /* power restored */

        rc_cal_t booted;
        uint8_t slot = 0xFFu;
        if (!boots_to_a_runnable_tune(&booted, &slot)) {
            RC_CHECK_MSG(0,
                         "power cut after %u bytes left the ECU unable to boot - "
                         "torn=%d", cut, (int)medium.torn);
            return;
        }
        /* Whatever came up must be a COMPLETE tune: either the old one or the new one,
         * never a mixture. A half-updated calibration that validates is the worst
         * possible outcome, because nothing will complain about it. */
        const float bore = rc_cal_get_engine_bore_mm(&booted);
        const bool is_old = fabs((double)bore - (double)good_bore) < 0.01;
        const bool is_new = fabs((double)bore - 84.0) < 0.01;
        if (!is_old && !is_new) {
            RC_CHECK_MSG(0, "power cut after %u bytes produced a MIXED tune (bore %.3f)",
                         cut, (double)bore);
            return;
        }
        if (cut > worst) {
            worst = cut;
        }
    }
    RC_CHECK_MSG(worst > (uint32_t)sizeof(rc_cal_t),
                 "the sweep only covered %u bytes; it never reached the image", worst);
}

/** A slot whose image has been corrupted after it was written must be refused, not
 *  activated. This is the case that catches a failing flash cell, which is a real and
 *  gradual failure rather than an exotic one. */
static void a_corrupted_slot_is_refused(void)
{
    erase_medium();
    rc_hal_t hal = make_hal();
    rc_flash_t flash;
    rc_flash_init(&flash, &hal, NULL, SLOT0_BASE, SLOT1_BASE, MARKER_BASE);
    { const rc_cal_t t = a_tune(66.0f); rc_flash_commit(&flash, &t); }
    { const rc_cal_t t = a_tune(78.0f); rc_flash_commit(&flash, &t); }

    /* Flip one bit in the slot the ECU is NOT running from. */
    const uint8_t inactive = (uint8_t)(flash.active ^ 1u);
    const uint32_t offset = (inactive == 0u ? SLOT0_BASE : SLOT1_BASE)
        + (uint32_t)sizeof(rc_flash_header_t) + 100u;
    medium.data[offset] ^= 0x40u;

    rc_cal_t readback;
    RC_CHECK_EQ_I(rc_flash_read_slot(&flash, inactive, &readback, NULL), RC_FLASH_ERR_CORRUPT);
    /* And activating it must be refused. */
    const uint8_t keep = flash.active;
    RC_CHECK(rc_flash_activate(&flash) != RC_FLASH_OK);
    RC_CHECK_MSG(flash.active == keep, "activation moved despite a corrupt staged slot");
}

/** A header that is half old and half new - which is what an interrupted rewrite actually
 *  looks like - must not be believed. */
static void a_torn_header_is_not_believed(void)
{
    erase_medium();
    rc_hal_t hal = make_hal();
    rc_flash_t flash;
    rc_flash_init(&flash, &hal, NULL, SLOT0_BASE, SLOT1_BASE, MARKER_BASE);
    { const rc_cal_t t = a_tune(66.0f); rc_flash_commit(&flash, &t); }
    { const rc_cal_t t = a_tune(78.0f); rc_flash_commit(&flash, &t); }

    /* Rewrite the inactive slot's version field, leaving the header CRC stale. */
    const uint8_t inactive = (uint8_t)(flash.active ^ 1u);
    const uint32_t base = (inactive == 0u ? SLOT0_BASE : SLOT1_BASE);
    medium.data[base + 8u] ^= 0x01u; /* inside `version`, covered by header_crc */

    rc_cal_t readback;
    RC_CHECK_EQ_I(rc_flash_read_slot(&flash, inactive, &readback, NULL), RC_FLASH_ERR_CORRUPT);
    RC_CHECK(rc_flash_activate(&flash) != RC_FLASH_OK);
}

/** Rollback must return the ECU to a tune it previously ran, and must refuse to go
 *  nowhere. */
static void rollback_returns_to_the_previous_tune(void)
{
    erase_medium();
    rc_hal_t hal = make_hal();
    rc_flash_t flash;
    rc_flash_init(&flash, &hal, NULL, SLOT0_BASE, SLOT1_BASE, MARKER_BASE);
    { const rc_cal_t t = a_tune(66.0f); rc_flash_commit(&flash, &t); }
    { const rc_cal_t t = a_tune(78.0f); rc_flash_commit(&flash, &t); }

    rc_cal_t current;
    uint8_t slot = 0u;
    RC_REQUIRE(boots_to_a_runnable_tune(&current, &slot));
    RC_CHECK_NEAR(rc_cal_get_engine_bore_mm(&current), 78.0f, 1e-3);

    RC_CHECK_EQ_I(rc_flash_rollback(&flash), RC_FLASH_OK);
    rc_cal_t rolled;
    uint8_t slot2 = 0u;
    RC_CHECK_MSG(boots_to_a_runnable_tune(&rolled, &slot2), "rollback left no bootable tune");
    RC_CHECK_NEAR(rc_cal_get_engine_bore_mm(&rolled), 66.0f, 1e-3);
    RC_CHECK(slot2 != slot);
}

/** With only one slot ever written there is nothing to roll back to, and the ECU must say
 *  so rather than flipping to an empty slot and failing to boot. */
static void rollback_with_nothing_to_roll_back_to_is_refused(void)
{
    erase_medium();
    rc_hal_t hal = make_hal();
    rc_flash_t flash;
    rc_flash_init(&flash, &hal, NULL, SLOT0_BASE, SLOT1_BASE, MARKER_BASE);
    { const rc_cal_t t = a_tune(66.0f); rc_flash_commit(&flash, &t); }
    const uint8_t keep = flash.active;
    RC_CHECK_MSG(rc_flash_rollback(&flash) != RC_FLASH_OK,
                 "rolled back to a slot that was never written");
    rc_cal_t booted;
    uint8_t slot = 0u;
    RC_CHECK(boots_to_a_runnable_tune(&booted, &slot));
    RC_CHECK_EQ_U(slot, keep);
}

/** A calibration that is perfectly storable but must not run is caught at VERIFY, which is
 *  the last point at which it can be caught without it having been on the road. */
static void a_storable_but_unacceptable_tune_is_refused_before_activation(void)
{
    erase_medium();
    rc_hal_t hal = make_hal();
    rc_flash_t flash;
    rc_flash_init(&flash, &hal, NULL, SLOT0_BASE, SLOT1_BASE, MARKER_BASE);
    { const rc_cal_t t = a_tune(66.0f); rc_flash_commit(&flash, &t); }
    const uint8_t keep = flash.active;

    /* Byte-for-byte storable, but the dwell ceiling is above what the hardware can do:
     * the exact class of mistake that destroys coils. */
    rc_cal_t bad = a_tune(78.0f);
    bad.ignition_dwell_max_ms = 12.0f;
    bad.hardware_ignition_max_dwell_ms = 6.0f;

    const int rc = rc_flash_commit(&flash, &bad);
    RC_CHECK_MSG(rc == RC_FLASH_ERR_VALIDATE, "expected a validation refusal, got %d (%s)", rc,
                 rc_flash_result_text(rc));
    RC_CHECK_MSG(flash.active == keep, "an unacceptable tune was activated anyway");
    rc_cal_t booted;
    uint8_t slot = 0u;
    RC_CHECK(boots_to_a_runnable_tune(&booted, &slot));
    RC_CHECK_NEAR(rc_cal_get_engine_bore_mm(&booted), 66.0f, 1e-3);
}

/** A medium that refuses everything must leave the running tune intact and report the
 *  failure - not silently succeed. */
static void a_failing_medium_is_reported_not_ignored(void)
{
    erase_medium();
    rc_hal_t hal = make_hal();
    rc_flash_t flash;
    rc_flash_init(&flash, &hal, NULL, SLOT0_BASE, SLOT1_BASE, MARKER_BASE);
    { const rc_cal_t t = a_tune(66.0f); rc_flash_commit(&flash, &t); }

    /* Break the medium: reads return garbage, writes fail. */
    const uint8_t keep = flash.active;
    rc_hal_t broken = hal;
    broken.storage_write = NULL;
    rc_flash_t flash2 = flash;
    flash2.hal = &broken;
    const rc_cal_t doomed = a_tune(84.0f);
    const int rc = rc_flash_commit(&flash2, &doomed);
    RC_CHECK_MSG(rc == RC_FLASH_ERR_IO, "a dead medium was not reported (got %d)", rc);
    RC_CHECK_EQ_U(flash.active, keep);

    /* And the original ECU is untouched. */
    rc_cal_t booted;
    uint8_t slot = 0u;
    RC_CHECK(boots_to_a_runnable_tune(&booted, &slot));
}

/** Repeated writes must keep the version strictly increasing, because that is what makes
 *  anti-rollback possible. */
static void the_version_increases_monotonically(void)
{
    erase_medium();
    rc_hal_t hal = make_hal();
    rc_flash_t flash;
    rc_flash_init(&flash, &hal, NULL, SLOT0_BASE, SLOT1_BASE, MARKER_BASE);
    uint32_t previous = 0u;
    for (int i = 0; i < 8; ++i) {
        const rc_cal_t cal = a_tune(60.0f + (float)i);
        RC_CHECK_EQ_I(rc_flash_commit(&flash, &cal), RC_FLASH_OK);
        RC_CHECK_MSG(flash.active_version > previous,
                     "version went from %u to %u on write %d", previous,
                     flash.active_version, i);
        previous = flash.active_version;
    }
}

void test_flash(void)
{
    RC_TEST_CASE("a/clean/write/activates/the/other/slot", a_clean_write_activates_the_other_slot);
    RC_TEST_CASE("staging/does/not/disturb/the/running/tune",
                 staging_does_not_disturb_the_running_tune);
    RC_TEST_CASE("a/power/cut/anywhere/in/the/write/leaves/a/bootable/ecu",
                 a_power_cut_anywhere_in_the_write_leaves_a_bootable_ecu);
    RC_TEST_CASE("a/corrupted/slot/is/refused", a_corrupted_slot_is_refused);
    RC_TEST_CASE("a/torn/header/is/not/believed", a_torn_header_is_not_believed);
    RC_TEST_CASE("rollback/returns/to/the/previous/tune", rollback_returns_to_the_previous_tune);
    RC_TEST_CASE("rollback/with/nothing/to/roll/back/to/is/refused",
                 rollback_with_nothing_to_roll_back_to_is_refused);
    RC_TEST_CASE("a/storable/but/unacceptable/tune/is/refused/before/activation",
                 a_storable_but_unacceptable_tune_is_refused_before_activation);
    RC_TEST_CASE("a/failing/medium/is/reported/not/ignored",
                 a_failing_medium_is_reported_not_ignored);
    RC_TEST_CASE("the/version/increases/monotonically", the_version_increases_monotonically);
}
