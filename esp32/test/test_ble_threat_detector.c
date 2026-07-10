#include "unity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ble_threat_detector.h"

#define BLE_EVIDENCE_SERIAL_UUID  (1U << 0)
#define BLE_EVIDENCE_SPARSE       (1U << 1)
#define BLE_EVIDENCE_GENERIC_NAME (1U << 2)
#define BLE_EVIDENCE_PERSISTENT   (1U << 3)
#define BLE_EVIDENCE_CLOSE        (1U << 4)
#define BLE_EVIDENCE_CONNECTABLE  (1U << 5)
#define BLE_EVIDENCE_UNTRUSTED    (1U << 6)

static ble_threat_observation_t prompt_observation(int index,
                                                   int64_t observed_ms,
                                                   ble_prompt_family_t family,
                                                   int8_t rssi)
{
    ble_threat_observation_t observation = {0};
    observation.mac[0] = 0x02;
    observation.mac[5] = (uint8_t)index;
    observation.observed_ms = observed_ms;
    observation.rssi = rssi;
    observation.structural_hash = 0x1234;
    observation.prompt_family = family;
    observation.company_id = 0x0006;
    return observation;
}

static ble_threat_observation_t serial_observation(int64_t observed_ms,
                                                   uint16_t service_uuid,
                                                   const char *name,
                                                   int8_t rssi,
                                                   bool connectable,
                                                   bool trusted)
{
    ble_threat_observation_t observation = {0};
    const uint8_t mac[6] = {0xC0, 0x98, 0xE5, 0x00, 0x00, 0x01};
    memcpy(observation.mac, mac, sizeof(mac));
    observation.observed_ms = observed_ms;
    observation.rssi = rssi;
    observation.connectable = connectable;
    observation.structural_hash = service_uuid;
    observation.service_uuids[0] = service_uuid;
    observation.service_uuid_count = 1;
    observation.local_name = name;
    observation.trusted_identity = trusted;
    return observation;
}

static void set_serial_services(ble_threat_observation_t *observation,
                                uint16_t first,
                                uint16_t second)
{
    observation->service_uuids[0] = first;
    observation->service_uuids[1] = second;
    observation->service_uuid_count = 2;
}

static bool observe_prompt(int index,
                           int64_t observed_ms,
                           ble_prompt_family_t family,
                           int8_t rssi,
                           ble_threat_signal_t *signal)
{
    const ble_threat_observation_t observation =
        prompt_observation(index, observed_ms, family, rssi);
    return ble_threat_detector_observe(&observation, signal);
}

static int emit_prompt_burst(int64_t start_ms,
                             int index_offset,
                             bool varied_rssi,
                             bool mixed_families,
                             ble_threat_signal_t *last_signal)
{
    int signal_count = 0;

    for (int index = 0; index < 12; ++index) {
        for (int packet_in_device = 0; packet_in_device < 2; ++packet_in_device) {
            const int64_t packet = (int64_t)index * 2 + packet_in_device;
            const int8_t rssi = varied_rssi && (index % 2 != 0) ? -64 :
                                varied_rssi ? -40 : -48;
            const ble_prompt_family_t family = mixed_families ?
                (ble_prompt_family_t)(1U << (index % 3)) : BLE_PROMPT_SWIFT_PAIR;

            if (observe_prompt(index + index_offset,
                               start_ms + packet * 300,
                               family,
                               rssi,
                               last_signal)) {
                ++signal_count;
            }
        }
    }

    return signal_count;
}

void test_ble_threat_swift_pair_rotating_flood_alerts_once(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    TEST_ASSERT_EQUAL_INT(1, emit_prompt_burst(0, 0, false, false, &signal));
    TEST_ASSERT_EQUAL(BLE_THREAT_PAIRING_SPAM, signal.kind);
    TEST_ASSERT_EQUAL_UINT8(BLE_PROMPT_SWIFT_PAIR, signal.prompt_family_mask);
    TEST_ASSERT_EQUAL_UINT16(12, signal.unique_macs);
    TEST_ASSERT_EQUAL_UINT16(24, signal.observation_count);
    TEST_ASSERT_EQUAL_INT8(-48, signal.strongest_rssi);
    TEST_ASSERT_EQUAL_UINT8(0, signal.rssi_span);
}

void test_ble_threat_scan_duplicate_is_deduped(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    TEST_ASSERT_FALSE(observe_prompt(0, 0, BLE_PROMPT_SWIFT_PAIR, -48, &signal));
    TEST_ASSERT_FALSE(observe_prompt(0, 100, BLE_PROMPT_SWIFT_PAIR, -48, &signal));
    TEST_ASSERT_FALSE(observe_prompt(0, 300, BLE_PROMPT_SWIFT_PAIR, -48, &signal));

    for (int index = 1; index < 12; ++index) {
        TEST_ASSERT_FALSE(observe_prompt(index,
                                         (int64_t)index * 600,
                                         BLE_PROMPT_SWIFT_PAIR,
                                         -48,
                                         &signal));
        const bool emitted = observe_prompt(index,
                                            (int64_t)index * 600 + 300,
                                            BLE_PROMPT_SWIFT_PAIR,
                                            -48,
                                            &signal);
        if (index < 11) {
            TEST_ASSERT_FALSE(emitted);
        } else {
            TEST_ASSERT_TRUE(emitted);
        }
    }

    TEST_ASSERT_EQUAL_UINT16(24, signal.observation_count);
}

void test_ble_threat_varied_crowd_does_not_alert(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    TEST_ASSERT_EQUAL_INT(0, emit_prompt_burst(0, 0, true, false, &signal));
}

void test_ble_threat_stable_addresses_do_not_alert(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    for (int index = 0; index < 12; ++index) {
        TEST_ASSERT_FALSE(observe_prompt(index,
                                         (int64_t)index * 300,
                                         BLE_PROMPT_SWIFT_PAIR,
                                         -48,
                                         &signal));
    }

    TEST_ASSERT_EQUAL_INT(0, emit_prompt_burst(9000, 0, false, false, &signal));
}

void test_ble_threat_mixed_prompt_families_alert(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    TEST_ASSERT_EQUAL_INT(1, emit_prompt_burst(0, 0, false, true, &signal));
    TEST_ASSERT_EQUAL_UINT8(BLE_PROMPT_APPLE | BLE_PROMPT_FAST_PAIR | BLE_PROMPT_SWIFT_PAIR,
                            signal.prompt_family_mask);
}

void test_ble_threat_cooldown_and_clear(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    TEST_ASSERT_EQUAL_INT(1, emit_prompt_burst(0, 0, false, false, &signal));
    TEST_ASSERT_EQUAL_INT(0, emit_prompt_burst(8000, 20, false, false, &signal));
    TEST_ASSERT_EQUAL_INT(1, emit_prompt_burst(34900, 40, false, false, &signal));
}

void test_ble_threat_persistent_sparse_ffe0_alerts(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    ble_threat_observation_t observation =
        serial_observation(0, 0xFFE0, "BT", -62, true, false);
    TEST_ASSERT_FALSE(ble_threat_detector_observe(&observation, &signal));
    observation.observed_ms = 2500;
    TEST_ASSERT_FALSE(ble_threat_detector_observe(&observation, &signal));
    observation.observed_ms = 5100;
    TEST_ASSERT_TRUE(ble_threat_detector_observe(&observation, &signal));

    TEST_ASSERT_EQUAL(BLE_THREAT_SERIAL_SKIMMER, signal.kind);
    TEST_ASSERT_EQUAL_HEX16(0xFFE0, signal.serial_service_uuid);
    TEST_ASSERT_EQUAL_HEX8(BLE_EVIDENCE_SERIAL_UUID |
                           BLE_EVIDENCE_SPARSE |
                           BLE_EVIDENCE_GENERIC_NAME |
                           BLE_EVIDENCE_PERSISTENT |
                           BLE_EVIDENCE_CLOSE |
                           BLE_EVIDENCE_CONNECTABLE |
                           BLE_EVIDENCE_UNTRUSTED,
                           signal.evidence_mask);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, signal.confidence);
}

void test_ble_threat_duplicate_serial_uuids_count_once_for_sparse_profile(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    const int64_t timestamps[] = {0, 2500, 5100};
    for (size_t index = 0; index < sizeof(timestamps) / sizeof(timestamps[0]); ++index) {
        ble_threat_observation_t observation =
            serial_observation(timestamps[index], 0xFFE0, NULL, -62, false, false);
        set_serial_services(&observation, 0xFFE0, 0xFFE0);
        const bool emitted = ble_threat_detector_observe(&observation, &signal);
        if (index < 2) {
            TEST_ASSERT_FALSE(emitted);
        } else {
            TEST_ASSERT_TRUE(emitted);
        }
    }

    TEST_ASSERT_EQUAL(BLE_THREAT_SERIAL_SKIMMER, signal.kind);
    TEST_ASSERT_BITS_HIGH(BLE_EVIDENCE_SPARSE, signal.evidence_mask);
}

void test_ble_threat_exact_two_supporting_signals_alert(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    const int64_t timestamps[] = {0, 2500, 5100};
    for (size_t index = 0; index < sizeof(timestamps) / sizeof(timestamps[0]); ++index) {
        const ble_threat_observation_t observation =
            serial_observation(timestamps[index], 0xFFE0, NULL, -62, false, false);
        const bool emitted = ble_threat_detector_observe(&observation, &signal);
        if (index < 2) {
            TEST_ASSERT_FALSE(emitted);
        } else {
            TEST_ASSERT_TRUE(emitted);
        }
    }

    TEST_ASSERT_EQUAL_HEX8(BLE_EVIDENCE_SERIAL_UUID |
                           BLE_EVIDENCE_SPARSE |
                           BLE_EVIDENCE_PERSISTENT |
                           BLE_EVIDENCE_CLOSE |
                           BLE_EVIDENCE_UNTRUSTED,
                           signal.evidence_mask);
}

void test_ble_threat_multi_service_profile_does_not_alert(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    const int64_t timestamps[] = {0, 2500, 5100};
    for (size_t index = 0; index < sizeof(timestamps) / sizeof(timestamps[0]); ++index) {
        ble_threat_observation_t observation =
            serial_observation(timestamps[index], 0xFFE0, "BT", -62, true, false);
        set_serial_services(&observation, 0xFFE0, 0xFEAA);
        TEST_ASSERT_FALSE(ble_threat_detector_observe(&observation, &signal));
    }
}

void test_ble_threat_simultaneous_prompt_and_serial_alerts_are_both_observable(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    ble_threat_observation_t serial =
        serial_observation(0, 0xFFE0, "BT", -62, true, false);
    TEST_ASSERT_FALSE(ble_threat_detector_observe(&serial, &signal));
    serial.observed_ms = 2500;
    TEST_ASSERT_FALSE(ble_threat_detector_observe(&serial, &signal));

    for (int packet = 0; packet < 23; ++packet) {
        const ble_threat_observation_t prompt =
            prompt_observation(packet / 2,
                               3000 + (int64_t)packet * 300,
                               BLE_PROMPT_SWIFT_PAIR,
                               -48);
        TEST_ASSERT_FALSE(ble_threat_detector_observe(&prompt, &signal));
    }

    serial.observed_ms = 9900;
    serial.prompt_family = BLE_PROMPT_SWIFT_PAIR;
    serial.structural_hash = 0x1234;
    TEST_ASSERT_TRUE(ble_threat_detector_observe(&serial, &signal));
    TEST_ASSERT_EQUAL(BLE_THREAT_PAIRING_SPAM, signal.kind);

    serial.observed_ms = 10200;
    TEST_ASSERT_TRUE(ble_threat_detector_observe(&serial, &signal));
    TEST_ASSERT_EQUAL(BLE_THREAT_SERIAL_SKIMMER, signal.kind);
}

void test_ble_threat_observed_ms_rollback_resets_prompt_state(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    TEST_ASSERT_EQUAL_INT(1, emit_prompt_burst(100000, 0, false, false, &signal));
    TEST_ASSERT_EQUAL_INT(1, emit_prompt_burst(0, 0, false, false, &signal));
    TEST_ASSERT_EQUAL(BLE_THREAT_PAIRING_SPAM, signal.kind);
}

void test_ble_threat_observed_ms_rollback_resets_serial_state(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    ble_threat_observation_t observation =
        serial_observation(100000, 0xFFE0, "BT", -62, true, false);
    TEST_ASSERT_FALSE(ble_threat_detector_observe(&observation, &signal));
    observation.observed_ms = 102500;
    TEST_ASSERT_FALSE(ble_threat_detector_observe(&observation, &signal));

    const int64_t timestamps[] = {0, 2500, 5100};
    for (size_t index = 0; index < sizeof(timestamps) / sizeof(timestamps[0]); ++index) {
        observation.observed_ms = timestamps[index];
        const bool emitted = ble_threat_detector_observe(&observation, &signal);
        if (index < 2) {
            TEST_ASSERT_FALSE(emitted);
        } else {
            TEST_ASSERT_TRUE(emitted);
        }
    }
    TEST_ASSERT_EQUAL(BLE_THREAT_SERIAL_SKIMMER, signal.kind);
}

void test_ble_threat_null_observation_clears_signal_output(void)
{
    ble_threat_signal_t signal;
    const ble_threat_signal_t cleared = {0};
    memset(&signal, 0xA5, sizeof(signal));

    TEST_ASSERT_FALSE(ble_threat_detector_observe(NULL, &signal));
    TEST_ASSERT_EQUAL_MEMORY(&cleared, &signal, sizeof(signal));
}

void test_ble_threat_ffe0_only_does_not_alert(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    const int64_t timestamps[] = {0, 2500, 5100};
    for (size_t index = 0; index < sizeof(timestamps) / sizeof(timestamps[0]); ++index) {
        const ble_threat_observation_t observation =
            serial_observation(timestamps[index], 0xFFE0, NULL, -90, false, false);
        TEST_ASSERT_FALSE(ble_threat_detector_observe(&observation, &signal));
    }
}

void test_ble_threat_trusted_product_suppresses_serial_candidate(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    const int64_t timestamps[] = {0, 2500, 5100};
    for (size_t index = 0; index < sizeof(timestamps) / sizeof(timestamps[0]); ++index) {
        const ble_threat_observation_t observation =
            serial_observation(timestamps[index], 0xFFE0, "BT", -62, true, true);
        TEST_ASSERT_FALSE(ble_threat_detector_observe(&observation, &signal));
    }
}

void test_ble_threat_pkoc_fff0_is_suppressed(void)
{
    ble_threat_signal_t signal = {0};
    ble_threat_detector_reset();

    const int64_t timestamps[] = {0, 2500, 5100};
    for (size_t index = 0; index < sizeof(timestamps) / sizeof(timestamps[0]); ++index) {
        const ble_threat_observation_t observation =
            serial_observation(timestamps[index], 0xFFF0, "PKOC", -62, true, false);
        TEST_ASSERT_FALSE(ble_threat_detector_observe(&observation, &signal));
    }
}
