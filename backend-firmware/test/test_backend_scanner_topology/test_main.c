#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <unity.h>

#include "backend_scanner_topology.h"
#include "../support/backend_test_main.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static backend_scanner_health_t transport_ready_scanner(uint32_t boot_id)
{
    backend_scanner_health_t health = {
        .connected = true,
        .identity_valid = true,
        .command_healthy = true,
        .boot_id = boot_id,
    };
    return health;
}

static backend_scanner_health_t converged_scanner(
    uint32_t boot_id,
    backend_scan_profile_t profile)
{
    backend_scanner_health_t health = transport_ready_scanner(boot_id);
    health.radio_healthy = true;
    health.role_acked = true;
    health.acknowledged_generation = 4;
    health.commanded_generation = 4;
    health.commanded_profile = profile;
    health.reported_profile = profile;
    health.convergence_started_ms = 1;
    health.convergence_started = true;
    return health;
}

void test_required_radio_health_covers_every_profile_and_radio_combination(void)
{
    typedef struct {
        backend_scan_profile_t profile;
        bool ble_healthy;
        bool wifi_healthy;
        bool expected;
    } row_t;

    static const row_t rows[] = {
        {BACKEND_SCAN_PROFILE_QUIESCENT, false, false, false},
        {BACKEND_SCAN_PROFILE_QUIESCENT, false, true, false},
        {BACKEND_SCAN_PROFILE_QUIESCENT, true, false, false},
        {BACKEND_SCAN_PROFILE_QUIESCENT, true, true, false},
        {BACKEND_SCAN_PROFILE_BLE_PRIMARY, false, false, false},
        {BACKEND_SCAN_PROFILE_BLE_PRIMARY, false, true, false},
        {BACKEND_SCAN_PROFILE_BLE_PRIMARY, true, false, true},
        {BACKEND_SCAN_PROFILE_BLE_PRIMARY, true, true, true},
        {BACKEND_SCAN_PROFILE_WIFI_PRIMARY, false, false, false},
        {BACKEND_SCAN_PROFILE_WIFI_PRIMARY, false, true, true},
        {BACKEND_SCAN_PROFILE_WIFI_PRIMARY, true, false, false},
        {BACKEND_SCAN_PROFILE_WIFI_PRIMARY, true, true, true},
        {BACKEND_SCAN_PROFILE_HYBRID_FAILOVER, false, false, false},
        {BACKEND_SCAN_PROFILE_HYBRID_FAILOVER, false, true, false},
        {BACKEND_SCAN_PROFILE_HYBRID_FAILOVER, true, false, false},
        {BACKEND_SCAN_PROFILE_HYBRID_FAILOVER, true, true, true},
        {(backend_scan_profile_t)99, false, false, false},
        {(backend_scan_profile_t)99, false, true, false},
        {(backend_scan_profile_t)99, true, false, false},
        {(backend_scan_profile_t)99, true, true, false},
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        TEST_ASSERT_EQUAL(
            rows[i].expected,
            backend_scanner_required_radio_healthy(
                rows[i].profile, rows[i].ble_healthy, rows[i].wifi_healthy));
    }
}

void test_two_healthy_scanners_get_fixed_profiles(void)
{
    backend_scanner_health_t health[2] = {
        converged_scanner(10, BACKEND_SCAN_PROFILE_BLE_PRIMARY),
        converged_scanner(20, BACKEND_SCAN_PROFILE_WIFI_PRIMARY),
    };
    backend_scanner_plan_t plan = {0};

    backend_scanner_plan_compute(health, 0, 1000, &plan);

    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_BLE_PRIMARY, plan.desired[0]);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_WIFI_PRIMARY, plan.desired[1]);
    TEST_ASSERT_EQUAL_HEX8(0x03, plan.eligible_mask);
    TEST_ASSERT_EQUAL_HEX8(0x03, plan.converged_mask);
    TEST_ASSERT_FALSE(plan.converging);
    TEST_ASSERT_FALSE(plan.degraded);
    TEST_ASSERT_FALSE(plan.fatal);
    TEST_ASSERT_EQUAL_INT(0, backend_scanner_ble_owner(&plan));
}

void test_each_single_scanner_becomes_the_hybrid_owner(void)
{
    backend_scanner_plan_t plan = {0};
    backend_scanner_health_t slot_zero[2] = {
        converged_scanner(10, BACKEND_SCAN_PROFILE_HYBRID_FAILOVER),
        {0},
    };

    backend_scanner_plan_compute(slot_zero, 0, 1000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x01, plan.eligible_mask);
    TEST_ASSERT_EQUAL_HEX8(0x01, plan.converged_mask);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_HYBRID_FAILOVER, plan.desired[0]);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_QUIESCENT, plan.desired[1]);
    TEST_ASSERT_TRUE(plan.degraded);
    TEST_ASSERT_FALSE(plan.fatal);
    TEST_ASSERT_EQUAL_INT(0, backend_scanner_ble_owner(&plan));

    backend_scanner_health_t slot_one[2] = {
        {0},
        converged_scanner(20, BACKEND_SCAN_PROFILE_HYBRID_FAILOVER),
    };
    backend_scanner_plan_compute(slot_one, 0, 1000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x02, plan.eligible_mask);
    TEST_ASSERT_EQUAL_HEX8(0x02, plan.converged_mask);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_QUIESCENT, plan.desired[0]);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_HYBRID_FAILOVER, plan.desired[1]);
    TEST_ASSERT_TRUE(plan.degraded);
    TEST_ASSERT_EQUAL_INT(1, backend_scanner_ble_owner(&plan));
}

void test_zero_scanners_converges_until_boot_deadline_then_becomes_fatal(void)
{
    backend_scanner_health_t health[2] = {{0}, {0}};
    backend_scanner_plan_t plan = {0};

    backend_scanner_plan_compute(health, 0, 14999, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x00, plan.eligible_mask);
    TEST_ASSERT_EQUAL_HEX8(0x00, plan.converged_mask);
    TEST_ASSERT_TRUE(plan.converging);
    TEST_ASSERT_FALSE(plan.degraded);
    TEST_ASSERT_FALSE(plan.fatal);
    TEST_ASSERT_EQUAL_INT(-1, backend_scanner_ble_owner(&plan));

    backend_scanner_plan_compute(health, 0, 15000, &plan);
    TEST_ASSERT_FALSE(plan.converging);
    TEST_ASSERT_FALSE(plan.degraded);
    TEST_ASSERT_TRUE(plan.fatal);
}

void test_cold_boot_assigns_roles_before_radios_can_report_healthy(void)
{
    backend_scanner_health_t cold[2] = {
        transport_ready_scanner(10),
        transport_ready_scanner(20),
    };
    backend_scanner_plan_t plan = {0};

    backend_scanner_plan_compute(cold, 0, 1000, &plan);

    TEST_ASSERT_EQUAL_HEX8(0x03, plan.eligible_mask);
    TEST_ASSERT_EQUAL_HEX8(0x00, plan.converged_mask);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_BLE_PRIMARY, plan.desired[0]);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_WIFI_PRIMARY, plan.desired[1]);
    TEST_ASSERT_TRUE(plan.converging);
    TEST_ASSERT_FALSE(plan.degraded);
    TEST_ASSERT_FALSE(plan.fatal);
    TEST_ASSERT_EQUAL_INT(-1, backend_scanner_ble_owner(&plan));
}

void test_transport_eligibility_requires_every_transport_gate(void)
{
    backend_scanner_plan_t plan = {0};
    backend_scanner_health_t health[2] = {{0}, {0}};
    backend_scanner_health_t valid = transport_ready_scanner(10);

    health[0] = valid;
    health[0].connected = false;
    backend_scanner_plan_compute(health, 0, 15000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0, plan.eligible_mask);

    health[0] = valid;
    health[0].identity_valid = false;
    backend_scanner_plan_compute(health, 0, 15000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0, plan.eligible_mask);

    health[0] = valid;
    health[0].boot_id = 0;
    backend_scanner_plan_compute(health, 0, 15000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0, plan.eligible_mask);

    health[0] = valid;
    health[0].command_healthy = false;
    backend_scanner_plan_compute(health, 0, 15000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0, plan.eligible_mask);

    health[0] = valid;
    backend_scanner_plan_compute(health, 0, 15000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x01, plan.eligible_mask);
}

void test_convergence_requires_ack_matching_profile_and_required_radio(void)
{
    backend_scanner_health_t health[2] = {
        converged_scanner(10, BACKEND_SCAN_PROFILE_BLE_PRIMARY),
        converged_scanner(20, BACKEND_SCAN_PROFILE_WIFI_PRIMARY),
    };
    backend_scanner_plan_t plan = {0};

    health[0].convergence_started_ms = 1000;
    health[1].convergence_started_ms = 1000;
    health[1].convergence_started = true;

    health[0].role_acked = false;
    backend_scanner_plan_compute(health, 0, 2000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x03, plan.eligible_mask);
    TEST_ASSERT_EQUAL_HEX8(0x02, plan.converged_mask);

    health[0] = converged_scanner(10, BACKEND_SCAN_PROFILE_WIFI_PRIMARY);
    health[0].convergence_started_ms = 1000;
    backend_scanner_plan_compute(health, 0, 2000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x02, plan.converged_mask);

    health[0] = converged_scanner(10, BACKEND_SCAN_PROFILE_BLE_PRIMARY);
    health[0].convergence_started_ms = 1000;
    health[0].radio_healthy = false;
    backend_scanner_plan_compute(health, 0, 2000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x02, plan.converged_mask);
    TEST_ASSERT_TRUE(plan.converging);
    TEST_ASSERT_FALSE(plan.degraded);
    TEST_ASSERT_FALSE(plan.fatal);

    health[0] = converged_scanner(10, BACKEND_SCAN_PROFILE_BLE_PRIMARY);
    health[0].convergence_started_ms = 1000;
    health[0].acknowledged_generation = 3;
    backend_scanner_plan_compute(health, 0, 2000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x02, plan.converged_mask);
    TEST_ASSERT_TRUE(plan.converging);
}

void test_nonconverged_scanner_is_retained_through_14999_ms_and_removed_at_15000(void)
{
    backend_scanner_health_t health[2] = {
        converged_scanner(10, BACKEND_SCAN_PROFILE_BLE_PRIMARY),
        transport_ready_scanner(20),
    };
    backend_scanner_plan_t plan = {0};

    health[0].convergence_started_ms = 1000;
    health[1].convergence_started_ms = 1000;
    health[1].convergence_started = true;

    backend_scanner_plan_compute(health, 0, 15999, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x03, plan.eligible_mask);
    TEST_ASSERT_EQUAL_HEX8(0x01, plan.converged_mask);
    TEST_ASSERT_TRUE(plan.converging);
    TEST_ASSERT_FALSE(plan.degraded);
    TEST_ASSERT_FALSE(plan.fatal);
    TEST_ASSERT_EQUAL_INT(0, backend_scanner_ble_owner(&plan));

    backend_scanner_plan_compute(health, 0, 16000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x01, plan.eligible_mask);
    TEST_ASSERT_EQUAL_HEX8(0x00, plan.converged_mask);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_HYBRID_FAILOVER, plan.desired[0]);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_QUIESCENT, plan.desired[1]);
    TEST_ASSERT_TRUE(plan.converging);
    TEST_ASSERT_TRUE(plan.degraded);
    TEST_ASSERT_FALSE(plan.fatal);
    TEST_ASSERT_EQUAL_INT(-1, backend_scanner_ble_owner(&plan));
}

void test_newly_eligible_scanner_without_start_time_is_never_pruned_before_assignment(void)
{
    backend_scanner_health_t health[2] = {
        transport_ready_scanner(10),
        {0},
    };
    backend_scanner_plan_t plan = {0};

    /* A changed boot clears validity even if the old timestamp remains. */
    health[0].convergence_started_ms = 12345;
    health[0].convergence_started = false;
    backend_scanner_plan_compute(health, 0, 60000, &plan);

    TEST_ASSERT_EQUAL_HEX8(0x01, plan.eligible_mask);
    TEST_ASSERT_EQUAL_HEX8(0x00, plan.converged_mask);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_HYBRID_FAILOVER, plan.desired[0]);
    TEST_ASSERT_TRUE(plan.converging);
    TEST_ASSERT_TRUE(plan.degraded);
    TEST_ASSERT_FALSE(plan.fatal);
}

void test_convergence_started_at_monotonic_zero_expires_at_exact_deadline(void)
{
    backend_scanner_health_t health[2] = {
        transport_ready_scanner(10),
        {0},
    };
    backend_scanner_plan_t plan = {0};
    health[0].convergence_started = true;
    health[0].convergence_started_ms = 0;

    backend_scanner_plan_compute(health, 0, 14999, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x01, plan.eligible_mask);
    TEST_ASSERT_TRUE(plan.converging);
    TEST_ASSERT_FALSE(plan.fatal);

    backend_scanner_plan_compute(health, 0, 15000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x00, plan.eligible_mask);
    TEST_ASSERT_FALSE(plan.converging);
    TEST_ASSERT_TRUE(plan.fatal);
}

void test_fixed_profile_survivor_gets_fresh_hybrid_transition_after_peer_loss(void)
{
    backend_scanner_plan_t plan = {0};
    backend_scanner_health_t slot_zero[2] = {
        converged_scanner(10, BACKEND_SCAN_PROFILE_BLE_PRIMARY),
        {0},
    };
    slot_zero[0].convergence_started_ms = 1;
    slot_zero[0].convergence_started = true;

    backend_scanner_plan_compute(slot_zero, 0, 20000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x01, plan.eligible_mask);
    TEST_ASSERT_EQUAL_HEX8(0x00, plan.converged_mask);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_HYBRID_FAILOVER, plan.desired[0]);
    TEST_ASSERT_TRUE(plan.converging);
    TEST_ASSERT_TRUE(plan.degraded);
    TEST_ASSERT_FALSE(plan.fatal);

    slot_zero[0].commanded_generation = 5U;
    slot_zero[0].commanded_profile = BACKEND_SCAN_PROFILE_HYBRID_FAILOVER;
    slot_zero[0].convergence_started_ms = 20000;
    backend_scanner_plan_compute(slot_zero, 0, 34999, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x01, plan.eligible_mask);
    TEST_ASSERT_TRUE(plan.converging);
    TEST_ASSERT_FALSE(plan.fatal);
    backend_scanner_plan_compute(slot_zero, 0, 35000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x00, plan.eligible_mask);
    TEST_ASSERT_TRUE(plan.fatal);

    backend_scanner_health_t slot_one[2] = {
        {0},
        converged_scanner(20, BACKEND_SCAN_PROFILE_WIFI_PRIMARY),
    };
    slot_one[1].convergence_started_ms = 1;
    slot_one[1].convergence_started = true;
    backend_scanner_plan_compute(slot_one, 0, 20000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x02, plan.eligible_mask);
    TEST_ASSERT_EQUAL_HEX8(0x00, plan.converged_mask);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_HYBRID_FAILOVER, plan.desired[1]);
    TEST_ASSERT_TRUE(plan.converging);
    TEST_ASSERT_TRUE(plan.degraded);
    TEST_ASSERT_FALSE(plan.fatal);
}

void test_hybrid_survivor_gets_fresh_fixed_transition_when_peer_recovers(void)
{
    backend_scanner_plan_t plan = {0};
    backend_scanner_health_t slot_zero_survivor[2] = {
        converged_scanner(10, BACKEND_SCAN_PROFILE_HYBRID_FAILOVER),
        transport_ready_scanner(20),
    };
    slot_zero_survivor[0].convergence_started_ms = 1;
    slot_zero_survivor[0].convergence_started = true;

    backend_scanner_plan_compute(slot_zero_survivor, 0, 20000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x03, plan.eligible_mask);
    TEST_ASSERT_EQUAL_HEX8(0x00, plan.converged_mask);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_BLE_PRIMARY, plan.desired[0]);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_WIFI_PRIMARY, plan.desired[1]);
    TEST_ASSERT_TRUE(plan.converging);
    TEST_ASSERT_FALSE(plan.degraded);
    TEST_ASSERT_FALSE(plan.fatal);

    backend_scanner_health_t slot_one_survivor[2] = {
        transport_ready_scanner(10),
        converged_scanner(20, BACKEND_SCAN_PROFILE_HYBRID_FAILOVER),
    };
    slot_one_survivor[1].convergence_started_ms = 1;
    slot_one_survivor[1].convergence_started = true;
    backend_scanner_plan_compute(slot_one_survivor, 0, 20000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x03, plan.eligible_mask);
    TEST_ASSERT_EQUAL_HEX8(0x00, plan.converged_mask);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_BLE_PRIMARY, plan.desired[0]);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_WIFI_PRIMARY, plan.desired[1]);
    TEST_ASSERT_TRUE(plan.converging);
    TEST_ASSERT_FALSE(plan.degraded);
    TEST_ASSERT_FALSE(plan.fatal);
}

void test_ble_owner_requires_a_converged_ble_capable_profile(void)
{
    backend_scanner_plan_t plan = {
        .desired = {
            BACKEND_SCAN_PROFILE_WIFI_PRIMARY,
            BACKEND_SCAN_PROFILE_BLE_PRIMARY,
        },
        .eligible_mask = 0x03,
        .converged_mask = 0x01,
    };

    TEST_ASSERT_EQUAL_INT(-1, backend_scanner_ble_owner(&plan));
    plan.converged_mask = 0x02;
    TEST_ASSERT_EQUAL_INT(1, backend_scanner_ble_owner(&plan));
    plan.desired[0] = BACKEND_SCAN_PROFILE_HYBRID_FAILOVER;
    plan.desired[1] = BACKEND_SCAN_PROFILE_HYBRID_FAILOVER;
    plan.converged_mask = 0x03;
    TEST_ASSERT_EQUAL_INT(0, backend_scanner_ble_owner(&plan));
    TEST_ASSERT_EQUAL_INT(-1, backend_scanner_ble_owner(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_required_radio_health_covers_every_profile_and_radio_combination);
    BACKEND_RUN_TEST(test_two_healthy_scanners_get_fixed_profiles);
    BACKEND_RUN_TEST(test_each_single_scanner_becomes_the_hybrid_owner);
    BACKEND_RUN_TEST(
        test_zero_scanners_converges_until_boot_deadline_then_becomes_fatal);
    BACKEND_RUN_TEST(
        test_cold_boot_assigns_roles_before_radios_can_report_healthy);
    BACKEND_RUN_TEST(
        test_transport_eligibility_requires_every_transport_gate);
    BACKEND_RUN_TEST(
        test_convergence_requires_ack_matching_profile_and_required_radio);
    BACKEND_RUN_TEST(
        test_nonconverged_scanner_is_retained_through_14999_ms_and_removed_at_15000);
    BACKEND_RUN_TEST(
        test_newly_eligible_scanner_without_start_time_is_never_pruned_before_assignment);
    BACKEND_RUN_TEST(
        test_convergence_started_at_monotonic_zero_expires_at_exact_deadline);
    BACKEND_RUN_TEST(
        test_fixed_profile_survivor_gets_fresh_hybrid_transition_after_peer_loss);
    BACKEND_RUN_TEST(
        test_hybrid_survivor_gets_fresh_fixed_transition_when_peer_recovers);
    BACKEND_RUN_TEST(
        test_ble_owner_requires_a_converged_ble_capable_profile);
    return UNITY_END();
}
