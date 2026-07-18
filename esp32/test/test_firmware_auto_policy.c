#include "unity.h"

#include "firmware_auto_policy.h"
#include "firmware_legacy_ready.h"

#include <stdint.h>

#define TEST_GENERATION 42U
#define TEST_MANIFEST_CRC 0x13572468U
#define TEST_SLOT 0U
#define TEST_IDENTITY_GENERATION 9U
#define TEST_MAC "e0:72:a1:f9:48:58"

static fof_auto_offer_binding_t offer_binding_fixture(void)
{
    fof_auto_offer_binding_t binding = {
        .generation = TEST_GENERATION,
        .manifest_crc32 = TEST_MANIFEST_CRC,
        .slot = TEST_SLOT,
        .identity_generation = TEST_IDENTITY_GENERATION,
        .hardware_id = TEST_MAC,
        .captured_ms = 1000,
    };
    return binding;
}

static fof_auto_recovery_view_t recovery_fixture(void)
{
    fof_auto_recovery_view_t recovery = {
        .manual_probe = true,
        .identity_fresh = true,
        .same_hardware_id = true,
        .target_contract_matches = true,
        .rollback_clear = true,
        .recovery_normal = true,
        .command_healthy = true,
        .profile_healthy = true,
        .radio_healthy = true,
        .version_relation = FOF_VERSION_EQUAL,
        .source_version = "0.64.69-badge-defcon34",
    };
    return recovery;
}

void test_auto_identity_freshness_requires_complete_new_recent_snapshot(void)
{
    fof_auto_identity_view_t identity = {
        .complete = true,
        .identity_generation = 11,
        .received_ms = 1000,
    };

    TEST_ASSERT_TRUE(fof_auto_identity_is_fresh(&identity, 10, 4000, 5000));

    identity.complete = false;
    TEST_ASSERT_FALSE(fof_auto_identity_is_fresh(&identity, 10, 4000, 5000));
    identity.complete = true;
    identity.identity_generation = 10;
    TEST_ASSERT_FALSE(fof_auto_identity_is_fresh(&identity, 10, 4000, 5000));
    identity.identity_generation = 11;
    identity.received_ms = 0;
    TEST_ASSERT_FALSE(fof_auto_identity_is_fresh(&identity, 10, 4000, 5000));
    identity.received_ms = 1000;
    TEST_ASSERT_FALSE(fof_auto_identity_is_fresh(&identity, 10, 7001, 5000));
    TEST_ASSERT_FALSE(fof_auto_identity_is_fresh(&identity, 10, 999, 5000));
    TEST_ASSERT_FALSE(fof_auto_identity_is_fresh(NULL, 10, 4000, 5000));
}

void test_auto_offer_binding_matches_every_bound_field_and_ttl(void)
{
    fof_auto_offer_binding_t binding = offer_binding_fixture();

    TEST_ASSERT_TRUE(fof_auto_offer_binding_matches(
        &binding, TEST_GENERATION, TEST_MANIFEST_CRC, TEST_SLOT,
        TEST_IDENTITY_GENERATION, TEST_MAC, 4999, 4000));
    TEST_ASSERT_FALSE(fof_auto_offer_binding_matches(
        &binding, TEST_GENERATION + 1, TEST_MANIFEST_CRC, TEST_SLOT,
        TEST_IDENTITY_GENERATION, TEST_MAC, 4999, 4000));
    TEST_ASSERT_FALSE(fof_auto_offer_binding_matches(
        &binding, TEST_GENERATION, TEST_MANIFEST_CRC ^ 1U, TEST_SLOT,
        TEST_IDENTITY_GENERATION, TEST_MAC, 4999, 4000));
    TEST_ASSERT_FALSE(fof_auto_offer_binding_matches(
        &binding, TEST_GENERATION, TEST_MANIFEST_CRC, 1U,
        TEST_IDENTITY_GENERATION, TEST_MAC, 4999, 4000));
    TEST_ASSERT_FALSE(fof_auto_offer_binding_matches(
        &binding, TEST_GENERATION, TEST_MANIFEST_CRC, TEST_SLOT,
        TEST_IDENTITY_GENERATION + 1U, TEST_MAC, 4999, 4000));
    TEST_ASSERT_FALSE(fof_auto_offer_binding_matches(
        &binding, TEST_GENERATION, TEST_MANIFEST_CRC, TEST_SLOT,
        TEST_IDENTITY_GENERATION, "14:c1:9f:52:ca:b0", 4999, 4000));
    TEST_ASSERT_FALSE(fof_auto_offer_binding_matches(
        &binding, TEST_GENERATION, TEST_MANIFEST_CRC, TEST_SLOT,
        TEST_IDENTITY_GENERATION, TEST_MAC, 5001, 4000));
    TEST_ASSERT_FALSE(fof_auto_offer_binding_matches(
        NULL, TEST_GENERATION, TEST_MANIFEST_CRC, TEST_SLOT,
        TEST_IDENTITY_GENERATION, TEST_MAC, 4999, 4000));
}

void test_auto_queue_requires_exact_offered_state(void)
{
    TEST_ASSERT_TRUE(fof_auto_queue_state_allows(FOF_AUTO_SLOT_OFFERED));
    TEST_ASSERT_FALSE(fof_auto_queue_state_allows(FOF_AUTO_SLOT_AWAITING_CHECK));
    TEST_ASSERT_FALSE(fof_auto_queue_state_allows(FOF_AUTO_SLOT_READY_QUEUED));
    TEST_ASSERT_FALSE(fof_auto_queue_state_allows(FOF_AUTO_SLOT_RELAYING));
    TEST_ASSERT_FALSE(fof_auto_queue_state_allows(FOF_AUTO_SLOT_RECOVERING));
    TEST_ASSERT_FALSE(fof_auto_queue_state_allows(FOF_AUTO_SLOT_CONVERGED));
    TEST_ASSERT_FALSE(fof_auto_queue_state_allows(FOF_AUTO_SLOT_CURRENT));
    TEST_ASSERT_FALSE(fof_auto_queue_state_allows(FOF_AUTO_SLOT_REFUSED));
    TEST_ASSERT_FALSE(fof_auto_queue_state_allows(FOF_AUTO_SLOT_FAILED));
    TEST_ASSERT_FALSE(fof_auto_queue_state_allows(FOF_AUTO_SLOT_NEWER_SKIPPED));
}

void test_auto_wifi_gate_requires_ble_success_or_current(void)
{
    TEST_ASSERT_TRUE(fof_auto_wifi_gate_open(false, FOF_AUTO_SLOT_FAILED));
    TEST_ASSERT_TRUE(fof_auto_wifi_gate_open(true, FOF_AUTO_SLOT_CONVERGED));
    TEST_ASSERT_TRUE(fof_auto_wifi_gate_open(true, FOF_AUTO_SLOT_CURRENT));
    TEST_ASSERT_FALSE(fof_auto_wifi_gate_open(true, FOF_AUTO_SLOT_FAILED));
    TEST_ASSERT_FALSE(fof_auto_wifi_gate_open(true, FOF_AUTO_SLOT_REFUSED));
    TEST_ASSERT_FALSE(fof_auto_wifi_gate_open(
        true, FOF_AUTO_SLOT_NEWER_SKIPPED));
    TEST_ASSERT_FALSE(fof_auto_wifi_gate_open(true, FOF_AUTO_SLOT_RECOVERING));
}

void test_auto_recovery_probe_waits_without_consuming_budget(void)
{
    TEST_ASSERT_EQUAL_INT(
        FOF_AUTO_PROBE_WAIT,
        fof_auto_recovery_probe_decide(34999, 35000, 0, 3));
    TEST_ASSERT_EQUAL_INT(
        FOF_AUTO_PROBE_WAIT,
        fof_auto_recovery_probe_decide(34999, 35000, 3, 3));
    TEST_ASSERT_EQUAL_INT(
        FOF_AUTO_PROBE_SEND,
        fof_auto_recovery_probe_decide(35000, 35000, 0, 3));
    TEST_ASSERT_EQUAL_INT(
        FOF_AUTO_PROBE_SEND,
        fof_auto_recovery_probe_decide(55000, 35000, 2, 3));
    TEST_ASSERT_EQUAL_INT(
        FOF_AUTO_PROBE_WAIT,
        fof_auto_recovery_probe_decide(54999, 55000, 1, 3));
    TEST_ASSERT_EQUAL_INT(
        FOF_AUTO_PROBE_SEND,
        fof_auto_recovery_probe_decide(55000, 55000, 1, 3));
    TEST_ASSERT_EQUAL_INT(
        FOF_AUTO_PROBE_EXHAUSTED,
        fof_auto_recovery_probe_decide(55000, 35000, 3, 3));
    TEST_ASSERT_EQUAL_INT(
        FOF_AUTO_PROBE_EXHAUSTED,
        fof_auto_recovery_probe_decide(55000, 35000, 0, 0));
}

void test_auto_recovery_converges_only_from_full_same_mac_health_proof(void)
{
    fof_auto_recovery_view_t recovery = recovery_fixture();

    TEST_ASSERT_EQUAL_INT(
        FOF_AUTO_RECOVERY_CONVERGED,
        fof_auto_recovery_decide(&recovery));

    recovery.manual_probe = false;
    TEST_ASSERT_EQUAL_INT(FOF_AUTO_RECOVERY_HOLD,
                          fof_auto_recovery_decide(&recovery));
    recovery = recovery_fixture();
    recovery.identity_fresh = false;
    TEST_ASSERT_EQUAL_INT(FOF_AUTO_RECOVERY_HOLD,
                          fof_auto_recovery_decide(&recovery));
    recovery = recovery_fixture();
    recovery.same_hardware_id = false;
    TEST_ASSERT_EQUAL_INT(FOF_AUTO_RECOVERY_HOLD,
                          fof_auto_recovery_decide(&recovery));
    recovery = recovery_fixture();
    recovery.target_contract_matches = false;
    TEST_ASSERT_EQUAL_INT(FOF_AUTO_RECOVERY_HOLD,
                          fof_auto_recovery_decide(&recovery));
    recovery = recovery_fixture();
    recovery.rollback_clear = false;
    TEST_ASSERT_EQUAL_INT(FOF_AUTO_RECOVERY_HOLD,
                          fof_auto_recovery_decide(&recovery));
    recovery = recovery_fixture();
    recovery.recovery_normal = false;
    TEST_ASSERT_EQUAL_INT(FOF_AUTO_RECOVERY_HOLD,
                          fof_auto_recovery_decide(&recovery));
    recovery = recovery_fixture();
    recovery.command_healthy = false;
    TEST_ASSERT_EQUAL_INT(FOF_AUTO_RECOVERY_HOLD,
                          fof_auto_recovery_decide(&recovery));
    recovery = recovery_fixture();
    recovery.profile_healthy = false;
    TEST_ASSERT_EQUAL_INT(FOF_AUTO_RECOVERY_HOLD,
                          fof_auto_recovery_decide(&recovery));
    recovery = recovery_fixture();
    recovery.radio_healthy = false;
    TEST_ASSERT_EQUAL_INT(FOF_AUTO_RECOVERY_HOLD,
                          fof_auto_recovery_decide(&recovery));
}

void test_auto_recovery_reoffers_only_exact_legacy_source(void)
{
    fof_auto_recovery_view_t recovery = recovery_fixture();
    recovery.version_relation = FOF_VERSION_NEWER;
    recovery.source_version = FOF_LEGACY_READY_BOOTSTRAP_VERSION;

    TEST_ASSERT_EQUAL_INT(
        FOF_AUTO_RECOVERY_REOFFER,
        fof_auto_recovery_decide(&recovery));

    recovery.source_version = "0.64.67-badge-live-follow";
    TEST_ASSERT_EQUAL_INT(FOF_AUTO_RECOVERY_REFUSED,
                          fof_auto_recovery_decide(&recovery));
    recovery = recovery_fixture();
    recovery.version_relation = FOF_VERSION_OLDER;
    TEST_ASSERT_EQUAL_INT(FOF_AUTO_RECOVERY_REFUSED,
                          fof_auto_recovery_decide(&recovery));
    recovery.version_relation = FOF_VERSION_UNORDERED;
    TEST_ASSERT_EQUAL_INT(FOF_AUTO_RECOVERY_REFUSED,
                          fof_auto_recovery_decide(&recovery));
    recovery.version_relation = FOF_VERSION_INVALID;
    TEST_ASSERT_EQUAL_INT(FOF_AUTO_RECOVERY_REFUSED,
                          fof_auto_recovery_decide(&recovery));
    TEST_ASSERT_EQUAL_INT(FOF_AUTO_RECOVERY_REFUSED,
                          fof_auto_recovery_decide(NULL));
}
