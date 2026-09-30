/* RACCOON V2 - calibration tests: schema integrity, bounds, roles and the rule parity. */
#include "rc_test.h"

#include "cal_access.h"
#include "rc/cal.h"
#include "rc/cal_validate.h"
#include "rc/engine.h"
#include "rc/rc_crc32.h"
#include "rc/rc_rules.h"
#include "cal_rules.h"

static void schema_is_self_consistent(void)
{
    /* Every descriptor is unique, in range, and points inside the image. */
    RC_CHECK(rc_cal_desc_count > 100u);
    for (uint16_t i = 0u; i < rc_cal_desc_count; ++i) {
        const rc_cal_desc_t *a = &rc_cal_desc[i];
        RC_CHECK_MSG(a->minimum <= a->maximum, "%s: min > max", a->name);
        RC_CHECK_MSG((uint32_t)a->offset + a->size <= RC_CAL_IMAGE_SIZE,
                     "%s: offset %u + size %u exceeds the %u byte image", a->name,
                     (unsigned)a->offset, (unsigned)a->size, (unsigned)RC_CAL_IMAGE_SIZE);
        RC_CHECK_MSG(a->scale > 0.0f, "%s: non-positive scale", a->name);
        RC_CHECK(a->type <= RC_CAL_T_AXIS1D);
        /* A table and an axis both have a cell width, and a width of zero would make
         * their byte size arithmetic silently meaningless rather than wrong-looking. */
        if (a->type == RC_CAL_T_TABLE2D || a->type == RC_CAL_T_AXIS1D) {
            RC_CHECK_MSG(a->cell_bytes == 1u || a->cell_bytes == 2u || a->cell_bytes == 4u,
                         "%s: cell_bytes %u is not 1, 2 or 4", a->name,
                         (unsigned)a->cell_bytes);
            RC_CHECK_MSG(a->size % a->cell_bytes == 0u, "%s: size %u is not a whole number "
                         "of %u byte cells", a->name, (unsigned)a->size,
                         (unsigned)a->cell_bytes);
        } else {
            RC_CHECK_MSG(a->cell_bytes == 0u, "%s: a scalar has no cell width", a->name);
        }
        RC_CHECK(a->owner <= RC_CAL_OWNER_MODE);
        RC_CHECK(a->safety <= RC_CAL_SAFETY_CRITICAL);
        RC_CHECK_MSG(a->default_source <= RC_CAL_SRC_UNVERIFIED, "%s: bad provenance", a->name);
        for (uint16_t j = (uint16_t)(i + 1u); j < rc_cal_desc_count; ++j) {
            RC_CHECK_MSG(a->id != rc_cal_desc[j].id, "duplicate id 0x%04X", (unsigned)a->id);
        }
    }
    /* Ids are unique, and lookup by name and by id agree. */
    RC_CHECK(rc_cal_desc_by_name("bore_mm") != NULL);
    RC_CHECK(rc_cal_desc_by_name("no_such_item") == NULL);
    const rc_cal_desc_t *desc = NULL;
    RC_CHECK(rc_cal_describe(RC_CAL_ID_ENGINE_CYLINDER_COUNT, &desc));
    RC_CHECK(desc != NULL && desc->id == RC_CAL_ID_ENGINE_CYLINDER_COUNT);
    RC_CHECK(!rc_cal_describe(0xFFFEu, &desc));
}

static void shipped_defaults_are_valid_and_armable(void)
{
    /* The image the product ships with must itself pass every rule.  If it does not, the
     * ECU would ship un-armable - which is exactly what the build-time rules exist to
     * stop, and this test is the third gate. */
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_cal_fault_t faults[16];
    uint32_t blocking = 0u;
    const bool ok = rc_cal_validate(&cal, faults, 16u, &blocking);
    RC_CHECK_MSG(ok, "the shipped defaults do not validate: %u violations, first '%s' = %f",
                 (unsigned)blocking, (blocking > 0u) ? faults[0].name : "?", (double)faults[0].value);

    /* And they must build into a real engine. */
    rc_engine_t engine;
    const char *reason = NULL;
    RC_CHECK_MSG(rc_engine_build(&cal, &engine, &reason), "defaults do not build: %s",
                 (reason != NULL) ? reason : "?");
    RC_CHECK_EQ_U(engine.cylinder_count, 2u);
}

static void bounds_are_enforced_by_the_ecu_not_only_the_gui(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    float value = 0.0f;
    const rc_cal_desc_t *desc = rc_cal_desc_by_name("bore_mm");
    RC_REQUIRE(desc != NULL);
    const uint16_t id = desc->id;

    RC_CHECK(rc_cal_get_float(&cal, id, &value));
    RC_CHECK_NEAR(value, 78.0f, 1e-3);

    /* A value inside the declared range is accepted. */
    RC_CHECK(rc_cal_set_float(&cal, id, 80.0f));
    RC_CHECK(rc_cal_get_float(&cal, id, &value));
    RC_CHECK_NEAR(value, 80.0f, 1e-3);

    /* Outside it is refused, not clamped: the mistake must be visible. */
    RC_CHECK(!rc_cal_set_float(&cal, id, 500.0f));
    RC_CHECK(!rc_cal_set_float(&cal, id, -5.0f));
    RC_CHECK(rc_cal_get_float(&cal, id, &value));
    RC_CHECK_NEAR(value, 80.0f, 1e-3);

    /* NaN is refused too, because NaN fails both comparisons. */
    RC_CHECK(!rc_cal_set_float(&cal, id, NAN));
    /* An unknown id is refused. */
    RC_CHECK(!rc_cal_set_float(&cal, 0xFF00u, 1.0f));
    RC_CHECK(!rc_cal_get_float(&cal, 0xFF00u, &value));
}

static void safety_limits_can_only_be_narrowed(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    const rc_cal_desc_t *rpm_desc = rc_cal_desc_by_name("rpm_limit");
    const rc_cal_desc_t *oil_desc = rc_cal_desc_by_name("oil_pressure_min_bar");
    RC_CHECK(rpm_desc != NULL && oil_desc != NULL);

    /* A viewer cannot write anything. */
    RC_CHECK(!rc_cal_owner_allows_write(rpm_desc, RC_CAL_ROLE_VIEWER));
    /* An engineer can, because a hard limit must be tightenable on the bench. */
    RC_CHECK(rc_cal_owner_allows_write(rpm_desc, RC_CAL_ROLE_ENGINEER));

    const float rpm_limit = rc_cal_get_safety_rpm_limit(&cal);
    const float oil_min = rc_cal_get_safety_oil_pressure_min_bar(&cal);
    /* SAFETY_SPEC section 6: the load-bearing property. */
    RC_CHECK_MSG(rc_cal_set_safety_limit(&cal, rpm_desc->id, rpm_limit - 200.0f,
                                         RC_CAL_ROLE_ENGINEER),
                 "an engineer must be able to lower a limit");
    RC_CHECK(!rc_cal_set_safety_limit(&cal, rpm_desc->id, rpm_limit + 200.0f,
                                      RC_CAL_ROLE_ENGINEER));
    /* ...and a tuner can never do either, whatever the role. */
    RC_CHECK(!rc_cal_set_safety_limit(&cal, oil_desc->id, oil_min - 0.5f, RC_CAL_ROLE_TUNER));
    RC_CHECK(!rc_cal_set_safety_limit(&cal, rpm_desc->id, rpm_limit - 200.0f, RC_CAL_ROLE_TUNER));
    /* The value really did change, and only in the safe direction. */
    RC_CHECK(rc_cal_get_safety_rpm_limit(&cal) < rpm_limit);
}

static void tables_round_trip_and_refuse_bad_cells(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    uint8_t rows = 0u;
    uint8_t cols = 0u;
    const void *cells = rc_cal_table(&cal, RC_CAL_ID_IGNITION_IGNITION_BASE_TABLE, &rows, &cols);
    RC_CHECK(cells != NULL);
    RC_CHECK_EQ_U(rows, 16u);
    RC_CHECK_EQ_U(cols, 16u);
    RC_CHECK_EQ_U(rc_cal_table(&cal, RC_CAL_ID_ENGINE_BORE_MM, &rows, &cols), NULL);

    /* Write a cell and read it back through the same path the tuner uses. */
    RC_CHECK(rc_cal_set_table_cell(&cal, RC_CAL_ID_IGNITION_IGNITION_BASE_TABLE, 3, 4, 20.0f));
    const float *as_f32 = (const float *)cells;
    RC_CHECK_NEAR(as_f32[(3 * 16) + 4], 20.0f, 1e-4);
    RC_CHECK_NEAR(rc_cal_get_ignition_ignition_base_table(&cal, 3, 4), 20.0f, 1e-4);
    /* Out of shape, and out of range, are both refused. */
    RC_CHECK(!rc_cal_set_table_cell(&cal, RC_CAL_ID_IGNITION_IGNITION_BASE_TABLE, 16, 0, 10.0f));
    RC_CHECK(!rc_cal_set_table_cell(&cal, RC_CAL_ID_IGNITION_IGNITION_BASE_TABLE, -1, 0, 10.0f));
    RC_CHECK_MSG(!rc_cal_set_table_cell(&cal, RC_CAL_ID_IGNITION_IGNITION_BASE_TABLE, 0, 0, 500.0f),
                 "a cell outside the cell range must be refused");
    /* A scalar cannot be used as a table and vice versa. */
    RC_CHECK(!rc_cal_set_table_cell(&cal, RC_CAL_ID_ENGINE_CYLINDER_COUNT, 0, 0, 2.0f));
    RC_CHECK(!rc_cal_set_float(&cal, RC_CAL_ID_IGNITION_IGNITION_BASE_TABLE, 1.0f));
}

static void a_corrupted_image_is_rejected(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    const uint32_t good = rc_cal_digest(&cal);
    /* One flipped bit in the cylinder count must change the digest: this is the check
     * that catches a scrambled calibration before it is run. */
    /* Corrupt the cylinder count itself.  An item ID is not a byte offset - using one as
     * an index scrambles an arbitrary byte, which proves nothing about whether a corrupted
     * image is caught. */
    const uint8_t good_cylinders = cal.engine_cylinder_count;
    cal.engine_cylinder_count = 9u; /* far outside the 1..4 the engine can build */
    RC_CHECK(rc_cal_digest(&cal) != good);
    RC_CHECK_MSG(cal.engine_cylinder_count != good_cylinders,
                 "the corruption must actually change the image");
    /* ...and the corrupted image must not validate. */
    rc_cal_fault_t faults[8];
    uint32_t blocking = 0u;
    RC_CHECK(!rc_cal_validate(&cal, faults, 8u, &blocking));
    RC_CHECK(blocking > 0u);
    RC_CHECK(rc_cal_severity_name(faults[0].severity) != NULL);
}

static void rule_parity_with_the_build_time_list(void)
{
    /* The runtime rules and the build-time rules must be the same rules, in the same
     * order.  The list is generated from one Python source into cal_rules.h, so all this
     * can prove is that the firmware has the current generated table and that its prose
     * table has an entry for every rule: a stale checkout would fail the count. */
    RC_CHECK_EQ_U(rc_rule_count(), RC_GENERATED_RULE_COUNT);
    RC_CHECK_EQ_U(rc_rule_count(), (uint16_t)RC_RULE_INDEX_COUNT);
    for (uint16_t i = 0u; i < rc_rule_count(); ++i) {
        RC_CHECK_MSG(rc_rule_name(i) != NULL && rc_rule_name(i)[0] != '\0',
                     "rule %u has no name", (unsigned)i);
        RC_CHECK_MSG(rc_rule_text(i) != NULL && rc_rule_text(i)[0] != '\0',
                     "rule %u (%s) has no explanation text", (unsigned)i, rc_rule_name(i));
    }
    RC_CHECK(strcmp(rc_rule_name(rc_rule_count()), "invalid") == 0);
    RC_CHECK(strstr(rc_rule_text(rc_rule_count()), "outside its declared bounds") != NULL);
    /* The two rules that matter most must say something a tuner can act on. */
    RC_CHECK(strstr(rc_rule_text(RC_RULE_INDEX_SEQUENTIAL_REQUIRES_CAM), "fabricate") != NULL);
    RC_CHECK(strstr(rc_rule_text(RC_RULE_INDEX_KILL_SWITCH_BLOCKS_ARM), "kill switch") != NULL);
    /* The axis rule is the newest refusal an operator can meet, and "not increasing" is the
     * phrase they will search for. */
    RC_CHECK(strstr(rc_rule_text(RC_RULE_INDEX_AXIS_BREAKPOINTS_INCREASE), "increase") != NULL);
    RC_CHECK(strstr(rc_rule_text(RC_RULE_INDEX_AXIS_BREAKPOINTS_INCREASE), "unsearchable") != NULL);
}

/* An axis is the coordinates a table is interpolated along. It has to be stored, read
 * back, shaped to its table, and refused when it is not searchable - an axis with a
 * repeated or inverted pair makes the bilinear lookup below it non-deterministic, which is
 * the kind of fault that appears only at one operating point and is very hard to find. */
static void axes_round_trip_and_refuse_bad_ordering(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    uint8_t count = 0u;

    /* The shape of an axis is its table's shape: a 16-cell row needs 16 breakpoints. */
    const void *points = rc_cal_axis(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, &count);
    RC_CHECK(points != NULL);
    RC_CHECK_EQ_U(count, 16u);
    RC_CHECK(rc_cal_axis_is_monotonic(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS));
    /* An axis is not a scalar and not a table, and must refuse both. */
    RC_CHECK_EQ_U(rc_cal_axis(&cal, RC_CAL_ID_ENGINE_BORE_MM, &count), NULL);
    RC_CHECK(!rc_cal_get_float(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, &(float){0.0f}));
    RC_CHECK(!rc_cal_set_float(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, 2000.0f));
    RC_CHECK(!rc_cal_table_cell(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, 0u, &(float){0.0f}));
    RC_CHECK(!rc_cal_set_table_cell(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, 0, 0, 2000.0f));

    /* The shipped axis starts at its declared minimum and ends at its maximum: an axis
     * that does not reach its own top silently clamps every cell above it to the last
     * row, which reads as a tuning mistake in the table rather than in the axis. */
    const rc_cal_desc_t *desc = NULL;
    RC_CHECK(rc_cal_describe(RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, &desc));
    float first = 0.0f;
    float last = 0.0f;
    RC_CHECK(rc_cal_axis_point(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, 0u, &first));
    RC_CHECK(rc_cal_axis_point(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, (uint16_t)(count - 1u), &last));
    RC_CHECK_NEAR(first, desc->minimum, 2.0f);
    RC_CHECK_NEAR(last, desc->maximum, 2.0f);

    /* Write a breakpoint, read it back, and confirm the stored width is right. A u16
     * breakpoint read as a byte would read back a plausible small number instead of the
     * value written, so the round trip through the raw storage is checked too. */
    RC_CHECK(rc_cal_set_axis_point(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, 3, 2750.0f));
    float back = 0.0f;
    RC_CHECK(rc_cal_axis_point(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, 3u, &back));
    RC_CHECK_NEAR(back, 2750.0f, 0.001f);
    const uint16_t *as_u16 = (const uint16_t *)points;
    RC_CHECK_EQ_U(as_u16[3], 2750u);
    RC_CHECK(rc_cal_axis_is_monotonic(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS));

    /* Out of range and out of shape are refused. */
    RC_CHECK(!rc_cal_set_axis_point(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, 0, 50.0f));
    RC_CHECK(!rc_cal_set_axis_point(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, 0, 99000.0f));
    RC_CHECK(!rc_cal_set_axis_point(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, 16, 2000.0f));
    RC_CHECK(!rc_cal_set_axis_point(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, -1, 2000.0f));

    /* A breakpoint may be dragged PAST its neighbour - that is the single most common axis
     * edit there is, and refusing it would make the axis untunable. Ordering is a property
     * of the whole axis and is caught at validation, not at the point of the write. */
    RC_CHECK(rc_cal_set_axis_point(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, 5, 1200.0f));
    RC_CHECK(!rc_cal_axis_is_monotonic(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS));

    /* ...and the ECU refuses to arm it. The refusal is a RULE fault rather than an item
     * fault, which is the difference between telling the operator "fuel.ve_table_rpm_axis
     * is outside its range" - a lie, every breakpoint is in range - and telling them the
     * breakpoints are not increasing. Blaming one item for something two of them did is
     * what sends an operator looking in the wrong place. */
    rc_cal_fault_t faults[8] = {{0}};
    uint32_t blocking = 0u;
    RC_CHECK_EQ_U(rc_cal_validate_items(&cal, faults, 8u), 0u);
    RC_CHECK(rc_cal_validate_rules(&cal, faults, 8u) > 0u);
    RC_CHECK_EQ_U(faults[0].rule_index, RC_RULE_INDEX_AXIS_BREAKPOINTS_INCREASE);
    RC_CHECK_EQ_U(faults[0].id, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS);
    RC_CHECK(faults[0].name != NULL);
    /* The reported value is the index of the first pair that is not increasing, so the
     * operator knows which breakpoint to move rather than being handed the whole axis. */
    RC_CHECK_NEAR(faults[0].value, 5.0f, 0.001f);
    RC_CHECK_MSG(strstr(faults[0].name, "ve_table_rpm_axis") != NULL,
                 "the fault should name the axis, got '%s'", faults[0].name);
    RC_CHECK(!rc_cal_validate(&cal, faults, 8u, &blocking));

    /* A repeated pair is equally unsearchable. */
    rc_cal_t repeated = rc_cal_defaults_filled();
    float third = 0.0f;
    float fourth = 0.0f;
    RC_CHECK(rc_cal_axis_point(&repeated, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, 3u, &third));
    RC_CHECK(rc_cal_axis_point(&repeated, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, 4u, &fourth));
    RC_CHECK(rc_cal_set_axis_point(&repeated, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, 4, third));
    RC_CHECK(!rc_cal_axis_is_monotonic(&repeated, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS));
    RC_CHECK_EQ_U(rc_cal_validate_items(&repeated, faults, 8u), 0u);
    RC_CHECK(rc_cal_validate_rules(&repeated, faults, 8u) > 0u);
    RC_CHECK_NEAR(faults[0].value, 4.0f, 0.001f);

    /* Restoring the order makes it armable again, so the refusal was about the axis and
     * not about the write having corrupted something else. */
    RC_CHECK(rc_cal_set_axis_point(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS, 5, 5000.0f));
    RC_CHECK(rc_cal_axis_is_monotonic(&cal, RC_CAL_ID_FUEL_VE_TABLE_RPM_AXIS));
    RC_CHECK_EQ_U(rc_cal_validate_items(&cal, faults, 8u), 0u);
    RC_CHECK_EQ_U(rc_cal_validate_rules(&cal, faults, 8u), 0u);
    RC_CHECK(rc_cal_validate(&cal, faults, 8u, &blocking));
}

void test_calibration(void)
{
    RC_TEST_CASE("schema/is/self/consistent", schema_is_self_consistent);
    RC_TEST_CASE("shipped/defaults/are/valid/and/armable", shipped_defaults_are_valid_and_armable);
    RC_TEST_CASE("bounds/are/enforced/by/the/ecu/not/only/the/gui", bounds_are_enforced_by_the_ecu_not_only_the_gui);
    RC_TEST_CASE("safety/limits/can/only/be/narrowed", safety_limits_can_only_be_narrowed);
    RC_TEST_CASE("tables/round/trip/and/refuse/bad/cells", tables_round_trip_and_refuse_bad_cells);
    RC_TEST_CASE("axes/round/trip/and/refuse/bad/ordering", axes_round_trip_and_refuse_bad_ordering);
    RC_TEST_CASE("a/corrupted/image/is/rejected", a_corrupted_image_is_rejected);
    RC_TEST_CASE("rule/parity/with/the/build/time/list", rule_parity_with_the_build_time_list);
}
