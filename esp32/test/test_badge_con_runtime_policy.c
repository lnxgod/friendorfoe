#include "unity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "badge_con_game.h"
#include "nvs_config.h"

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define RTC_NOINIT_ATTR
#define portENTER_CRITICAL(lock) badge_con_test_enter_critical(lock)
#define portEXIT_CRITICAL(lock) badge_con_test_exit_critical(lock)

static bool s_test_nvs_write_ok;
static nvs_config_blob_read_status_t s_test_nvs_read_status;
static uint8_t s_test_nvs_record[BADGE_CON_NVS_RECORD_BYTES];
static size_t s_test_nvs_record_size;
static unsigned s_test_nvs_write_count;
static bool s_test_expected_reset;
static uint32_t s_test_expected_generation;
static int64_t s_test_now_us;
static uint32_t s_test_random_word;
static unsigned s_test_critical_depth;
static bool s_test_nvs_called_in_critical;
static bool (*s_test_expected_reboot_hook)(uint32_t);
static uint8_t s_test_game_rtc_record[BADGE_CON_RTC_RECORD_BYTES];
static unsigned s_test_game_rtc_read_count;
static unsigned s_test_game_rtc_write_count;
static unsigned s_test_game_rtc_clear_count;

static void badge_con_test_enter_critical(portMUX_TYPE *lock)
{
    (void)lock;
    s_test_critical_depth++;
}

static void badge_con_test_exit_critical(portMUX_TYPE *lock)
{
    (void)lock;
    TEST_ASSERT_GREATER_THAN_UINT(0U, s_test_critical_depth);
    s_test_critical_depth--;
}

#define FOF_DC34_GAME_CANARY 1
#include "../uplink/main/core/badge_con_runtime.c"

nvs_config_blob_read_status_t nvs_config_read_blob(
    const char *key, void *data, size_t capacity, size_t *out_size)
{
    TEST_ASSERT_EQUAL_STRING("game_state_v1", key);
    TEST_ASSERT_EQUAL_UINT(0U, s_test_critical_depth);
    if (out_size) {
        *out_size = 0U;
    }
    if (s_test_nvs_read_status != NVS_CONFIG_BLOB_PRESENT) {
        return s_test_nvs_read_status;
    }
    TEST_ASSERT_LESS_OR_EQUAL_UINT(capacity, s_test_nvs_record_size);
    memcpy(data, s_test_nvs_record, s_test_nvs_record_size);
    if (out_size) {
        *out_size = s_test_nvs_record_size;
    }
    return NVS_CONFIG_BLOB_PRESENT;
}

bool nvs_config_set_blob(const char *key, const void *data, size_t size)
{
    TEST_ASSERT_EQUAL_STRING("game_state_v1", key);
    s_test_nvs_called_in_critical =
        s_test_nvs_called_in_critical || s_test_critical_depth != 0U;
    s_test_nvs_write_count++;
    if (!s_test_nvs_write_ok) {
        return false;
    }
    TEST_ASSERT_EQUAL_UINT(BADGE_CON_NVS_RECORD_BYTES, size);
    memcpy(s_test_nvs_record, data, size);
    s_test_nvs_record_size = size;
    s_test_nvs_read_status = NVS_CONFIG_BLOB_PRESENT;
    return true;
}

bool badge_runtime_last_reset_expected(void)
{
    return s_test_expected_reset;
}

uint32_t badge_runtime_last_expected_reboot_generation(void)
{
    return s_test_expected_generation;
}

void badge_runtime_set_expected_reboot_hook(
    badge_runtime_expected_reboot_hook_t hook)
{
    s_test_expected_reboot_hook = hook;
}

bool badge_runtime_game_rtc_read(void *out, size_t record_size)
{
    s_test_game_rtc_read_count++;
    if (!out || record_size != sizeof(s_test_game_rtc_record)) {
        return false;
    }
    memcpy(out, s_test_game_rtc_record, record_size);
    return true;
}

bool badge_runtime_game_rtc_write(const void *record, size_t record_size)
{
    s_test_game_rtc_write_count++;
    if (!record || record_size != sizeof(s_test_game_rtc_record)) {
        return false;
    }
    memcpy(s_test_game_rtc_record, record, record_size);
    return true;
}

void badge_runtime_game_rtc_clear(void)
{
    s_test_game_rtc_clear_count++;
    memset(s_test_game_rtc_record, 0, sizeof(s_test_game_rtc_record));
}

void esp_fill_random(void *buffer, size_t length)
{
    uint8_t *bytes = buffer;
    for (size_t i = 0U; i < length; i++) {
        bytes[i] = (uint8_t)(s_test_random_word >> ((i % 4U) * 8U));
    }
    s_test_random_word += 0x01010101U;
}

int64_t esp_timer_get_time(void)
{
    return s_test_now_us;
}

static void runtime_test_reset(void)
{
    s_test_nvs_write_ok = true;
    s_test_nvs_read_status = NVS_CONFIG_BLOB_MISSING;
    memset(s_test_nvs_record, 0, sizeof(s_test_nvs_record));
    s_test_nvs_record_size = 0U;
    s_test_nvs_write_count = 0U;
    s_test_expected_reset = false;
    s_test_expected_generation = 0U;
    s_test_now_us = 1000000;
    s_test_random_word = 0x12345678U;
    s_test_critical_depth = 0U;
    s_test_nvs_called_in_critical = false;
    s_test_expected_reboot_hook = NULL;
    memset(s_test_game_rtc_record, 0, sizeof(s_test_game_rtc_record));
    s_test_game_rtc_read_count = 0U;
    s_test_game_rtc_write_count = 0U;
    s_test_game_rtc_clear_count = 0U;
    badge_con_runtime_test_reset();
}

static badge_con_snapshot_t runtime_snapshot(void)
{
    badge_con_snapshot_t snapshot = {0};
    TEST_ASSERT_TRUE(badge_con_runtime_snapshot(&snapshot));
    return snapshot;
}

static uint32_t test_crc32(const uint8_t *bytes, size_t byte_count)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0U; i < byte_count; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; bit++) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

static void put_test_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    out[2] = (uint8_t)(value >> 16U);
    out[3] = (uint8_t)(value >> 24U);
}

void test_badge_con_runtime_defaults_and_randomizes_ephemeral_identity(void)
{
    runtime_test_reset();
    badge_con_runtime_init();

    badge_con_snapshot_t snapshot = runtime_snapshot();
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, snapshot.seed);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, snapshot.role);
    TEST_ASSERT_FALSE(snapshot.active);
    TEST_ASSERT_EQUAL_UINT8(0U, snapshot.shield);
    TEST_ASSERT_EQUAL_UINT(0U, s_test_nvs_write_count);
    TEST_ASSERT_NOT_NULL(s_test_expected_reboot_hook);

    uint32_t peer = 0U;
    uint8_t session = 0U;
    TEST_ASSERT_TRUE(badge_con_runtime_identity(&peer, &session));
    TEST_ASSERT_NOT_EQUAL(0U, peer);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(0x00ffffffU, peer);
    TEST_ASSERT_NOT_EQUAL(0U, session);
    TEST_ASSERT_GREATER_THAN_UINT(0U, s_test_game_rtc_write_count);
}

void test_badge_con_runtime_factory_seed_is_atomic_and_resets_game(void)
{
    runtime_test_reset();
    badge_con_runtime_init();
    TEST_ASSERT_TRUE(badge_con_runtime_set_factory_seed(
        BADGE_CON_ROLE_INFECTED));
    TEST_ASSERT_TRUE(badge_con_runtime_activate_after_easter());

    badge_con_packet_t immune = {
        .peer = 1U,
        .session = 1U,
        .sequence = 1U,
        .role = BADGE_CON_ROLE_IMMUNE,
    };
    for (unsigned i = 0U; i < 7U; i++) {
        (void)badge_con_runtime_apply_qualified_peer(&immune);
    }

    TEST_ASSERT_TRUE(badge_con_runtime_set_factory_seed(
        BADGE_CON_ROLE_IMMUNE));
    badge_con_snapshot_t after = runtime_snapshot();
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_IMMUNE, after.seed);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_IMMUNE, after.role);
    TEST_ASSERT_FALSE(after.active);
    TEST_ASSERT_EQUAL_UINT8(0U, after.shield);
    TEST_ASSERT_FALSE(s_test_nvs_called_in_critical);
}

void test_badge_con_runtime_seed_only_and_read_error_restore_safely(void)
{
    runtime_test_reset();
    const badge_con_game_state_t valid = {
        .seed = BADGE_CON_ROLE_INFECTED,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 75U,
        .scar_level = 4U,
        .last_decay_ms = 99U,
    };
    TEST_ASSERT_TRUE(badge_con_game_encode_nvs(
        &valid, s_test_nvs_record));
    s_test_nvs_record_size = sizeof(s_test_nvs_record);
    s_test_nvs_read_status = NVS_CONFIG_BLOB_PRESENT;
    badge_con_runtime_init();
    badge_con_snapshot_t seed_only = runtime_snapshot();
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, seed_only.seed);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, seed_only.role);
    TEST_ASSERT_FALSE(seed_only.active);
    TEST_ASSERT_EQUAL_UINT8(0U, seed_only.shield);

    s_test_nvs_read_status = NVS_CONFIG_BLOB_READ_ERROR;
    badge_con_runtime_init();
    badge_con_snapshot_t error = runtime_snapshot();
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, error.seed);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, error.role);
    TEST_ASSERT_FALSE(error.active);
    TEST_ASSERT_EQUAL_UINT8(0U, error.shield);
}

void test_badge_con_runtime_failed_seed_write_preserves_visible_state(void)
{
    runtime_test_reset();
    badge_con_runtime_init();
    TEST_ASSERT_TRUE(badge_con_runtime_set_factory_seed(
        BADGE_CON_ROLE_INFECTED));
    badge_con_snapshot_t before = runtime_snapshot();
    unsigned writes_before = s_test_nvs_write_count;

    s_test_nvs_write_ok = false;
    TEST_ASSERT_FALSE(badge_con_runtime_set_factory_seed(
        BADGE_CON_ROLE_IMMUNE));
    badge_con_snapshot_t after = runtime_snapshot();
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));
    TEST_ASSERT_EQUAL_UINT(writes_before + 1U, s_test_nvs_write_count);
    TEST_ASSERT_FALSE(s_test_nvs_called_in_critical);
}

void test_badge_con_runtime_expected_reboot_resets_live_state(void)
{
    runtime_test_reset();
    badge_con_runtime_init();
    TEST_ASSERT_TRUE(badge_con_runtime_set_factory_seed(
        BADGE_CON_ROLE_IMMUNE));
    TEST_ASSERT_TRUE(badge_con_runtime_activate_after_easter());

    badge_con_packet_t infected = {
        .peer = 2U,
        .session = 3U,
        .sequence = 4U,
        .role = BADGE_CON_ROLE_INFECTED,
        .rssi = -45,
    };
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_runtime_apply_qualified_peer(&infected));
    TEST_ASSERT_EQUAL_UINT8(70U, runtime_snapshot().shield);

    TEST_ASSERT_NOT_NULL(s_test_expected_reboot_hook);
    TEST_ASSERT_TRUE(s_test_expected_reboot_hook(19U));
    s_test_expected_reset = true;
    s_test_expected_generation = 19U;
    s_test_now_us = 100000;
    s_test_game_rtc_read_count = 0U;
    badge_con_runtime_init();
    badge_con_snapshot_t restored = runtime_snapshot();
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_IMMUNE, restored.seed);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_IMMUNE, restored.role);
    TEST_ASSERT_FALSE(restored.active);
    TEST_ASSERT_EQUAL_UINT8(0U, restored.shield);
    TEST_ASSERT_EQUAL_UINT8(0U, restored.scar_level);
    TEST_ASSERT_FALSE(restored.dead);
    TEST_ASSERT_FALSE(restored.super);
    TEST_ASSERT_EQUAL_UINT(0U, s_test_game_rtc_read_count);
}

void test_badge_con_runtime_expected_hook_writes_seed_only_dependency(void)
{
    runtime_test_reset();
    badge_con_runtime_init();
    TEST_ASSERT_TRUE(badge_con_runtime_set_factory_seed(
        BADGE_CON_ROLE_INFECTED));
    TEST_ASSERT_TRUE(badge_con_runtime_activate_after_easter());

    TEST_ASSERT_NOT_NULL(s_test_expected_reboot_hook);
    TEST_ASSERT_TRUE(s_test_expected_reboot_hook(91U));

    badge_con_game_state_t exact;
    TEST_ASSERT_TRUE(badge_con_game_decode_rtc(
        s_test_game_rtc_record, sizeof(s_test_game_rtc_record),
        91U, &exact));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, exact.seed);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, exact.role);
    TEST_ASSERT_FALSE(exact.active);
    TEST_ASSERT_EQUAL_UINT8(0U, exact.shield);
    TEST_ASSERT_EQUAL_UINT8(0U, exact.scar_level);
    TEST_ASSERT_FALSE(exact.dead);
}

void test_badge_con_runtime_live_mutations_ignore_nvs_failure(void)
{
    runtime_test_reset();
    badge_con_runtime_init();
    s_test_nvs_write_ok = false;
    TEST_ASSERT_TRUE(badge_con_runtime_activate_after_easter());
    badge_con_snapshot_t active = runtime_snapshot();
    TEST_ASSERT_TRUE(active.active);
    badge_con_packet_t infected = {
        .peer = 5U,
        .session = 6U,
        .sequence = 7U,
        .role = BADGE_CON_ROLE_INFECTED,
        .rssi = -60,
    };
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_runtime_apply_qualified_peer(&infected));
    TEST_ASSERT_EQUAL_UINT8(20U, runtime_snapshot().shield);
    TEST_ASSERT_EQUAL_UINT(0U, s_test_nvs_write_count);

    TEST_ASSERT_FALSE(badge_con_runtime_set_factory_seed(
        BADGE_CON_ROLE_IMMUNE));
    badge_con_snapshot_t unchanged = runtime_snapshot();
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, unchanged.seed);
    TEST_ASSERT_TRUE(unchanged.active);
}

void test_badge_con_runtime_every_reboot_path_uses_seed_only(void)
{
    const bool expected_resets[] = {false, false, true, true, true};
    const uint32_t generations[] = {0U, 7U, 7U, 8U, 7U};
    for (size_t i = 0U;
         i < sizeof(generations) / sizeof(generations[0]);
         ++i) {
        runtime_test_reset();
        uint8_t legacy[BADGE_CON_NVS_RECORD_BYTES] = {
            0xC3U, 0x01U, (uint8_t)BADGE_CON_ROLE_INFECTED,
            (uint8_t)BADGE_CON_ROLE_NORMAL, 0x01U, 0x5AU, 0x00U, 0x00U,
            0U, 0U, 0U, 0U,
        };
        put_test_le32(&legacy[8], test_crc32(legacy, 8U));
        memcpy(s_test_nvs_record, legacy, sizeof(legacy));
        s_test_nvs_record_size = sizeof(legacy);
        s_test_nvs_read_status = NVS_CONFIG_BLOB_PRESENT;

        const badge_con_game_state_t rtc_live = {
            .seed = BADGE_CON_ROLE_INFECTED,
            .role = BADGE_CON_ROLE_NORMAL,
            .active = true,
            .shield = 27U,
            .last_decay_ms = 500U,
        };
        TEST_ASSERT_TRUE(badge_con_game_encode_rtc(
            &rtc_live, 7U, s_test_game_rtc_record));
        if (i == 4U) {
            s_test_game_rtc_record[16] ^= 0x01U;
        }
        s_test_expected_reset = expected_resets[i];
        s_test_expected_generation = generations[i];

        badge_con_runtime_init();
        badge_con_snapshot_t reset = runtime_snapshot();
        TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, reset.seed);
        TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, reset.role);
        TEST_ASSERT_FALSE(reset.active);
        TEST_ASSERT_EQUAL_UINT8(0U, reset.shield);
        TEST_ASSERT_EQUAL_UINT8(0U, reset.scar_level);
        TEST_ASSERT_FALSE(reset.dead);
        TEST_ASSERT_FALSE(reset.super);
        TEST_ASSERT_EQUAL_UINT(0U, s_test_game_rtc_read_count);
    }
}

void test_badge_con_runtime_only_factory_seed_writes_nvs(void)
{
    runtime_test_reset();
    badge_con_runtime_init();
    TEST_ASSERT_TRUE(badge_con_runtime_set_factory_seed(
        BADGE_CON_ROLE_NORMAL));
    TEST_ASSERT_EQUAL_UINT(1U, s_test_nvs_write_count);
    TEST_ASSERT_TRUE(badge_con_runtime_activate_after_easter());
    TEST_ASSERT_EQUAL_UINT(1U, s_test_nvs_write_count);

    badge_con_packet_t infected = {
        .peer = 9U,
        .session = 8U,
        .sequence = 7U,
        .role = BADGE_CON_ROLE_INFECTED,
        .rssi = -60,
    };
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_runtime_apply_qualified_peer(&infected));
    TEST_ASSERT_EQUAL_UINT(1U, s_test_nvs_write_count);

    s_test_now_us += 61000000;
    (void)runtime_snapshot();
    TEST_ASSERT_EQUAL_UINT(1U, s_test_nvs_write_count);

    badge_con_game_state_t final_cure = {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 90U,
        .scar_level = 5U,
        .dead = false,
        .last_decay_ms = (uint32_t)(s_test_now_us / 1000),
    };
    portENTER_CRITICAL(&s_lock);
    s_state = final_cure;
    portEXIT_CRITICAL(&s_lock);
    badge_con_packet_t immune = {
        .peer = 4U,
        .session = 3U,
        .sequence = 2U,
        .role = BADGE_CON_ROLE_IMMUNE,
        .rssi = -45,
    };
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_DIED,
        badge_con_runtime_apply_qualified_peer(&immune));
    TEST_ASSERT_TRUE(runtime_snapshot().dead);
    TEST_ASSERT_EQUAL_UINT(1U, s_test_nvs_write_count);
    TEST_ASSERT_FALSE(s_test_nvs_called_in_critical);
}

void test_badge_con_runtime_uses_packet_rssi_and_authenticated_super(void)
{
    static const struct {
        int8_t rssi;
        badge_con_effect_t effect;
        uint8_t shield;
        badge_con_role_t role;
    } cases[] = {
        {-45, BADGE_CON_EFFECT_INFECTED, BADGE_CON_INFECTED_RESCUE_POINTS,
         BADGE_CON_ROLE_INFECTED},
        {-46, BADGE_CON_EFFECT_SHIELD_DRAINED, 10U, BADGE_CON_ROLE_NORMAL},
        {-52, BADGE_CON_EFFECT_SHIELD_DRAINED, 10U, BADGE_CON_ROLE_NORMAL},
        {-53, BADGE_CON_EFFECT_SHIELD_DRAINED, 20U, BADGE_CON_ROLE_NORMAL},
        {-60, BADGE_CON_EFFECT_SHIELD_DRAINED, 20U, BADGE_CON_ROLE_NORMAL},
        {-61, BADGE_CON_EFFECT_NONE, 30U, BADGE_CON_ROLE_NORMAL},
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        runtime_test_reset();
        badge_con_runtime_init();
        TEST_ASSERT_TRUE(badge_con_runtime_activate_after_easter());
        badge_con_packet_t packet = {
            .peer = 1U,
            .session = 2U,
            .sequence = 3U,
            .role = BADGE_CON_ROLE_INFECTED,
            .super = false,
            .rssi = cases[i].rssi,
        };
        TEST_ASSERT_EQUAL(
            cases[i].effect,
            badge_con_runtime_apply_qualified_peer(&packet));
        badge_con_snapshot_t after = runtime_snapshot();
        TEST_ASSERT_EQUAL_UINT8(cases[i].shield, after.shield);
        TEST_ASSERT_EQUAL(cases[i].role, after.role);
    }

    runtime_test_reset();
    badge_con_runtime_init();
    TEST_ASSERT_TRUE(badge_con_runtime_activate_after_easter());
    badge_con_packet_t super = {
        .peer = 4U,
        .session = 5U,
        .sequence = 6U,
        .role = BADGE_CON_ROLE_INFECTED,
        .super = true,
        .rssi = -60,
    };
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_runtime_apply_qualified_peer(&super));
    TEST_ASSERT_EQUAL_UINT8(10U, runtime_snapshot().shield);

    super.role = BADGE_CON_ROLE_IMMUNE;
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_NONE,
        badge_con_runtime_apply_qualified_peer(&super));
    TEST_ASSERT_EQUAL_UINT8(10U, runtime_snapshot().shield);
}

void test_badge_con_runtime_self_ack_requires_current_ephemeral_identity(void)
{
    runtime_test_reset();
    badge_con_runtime_init();
    uint32_t peer = 0U;
    uint8_t session = 0U;
    TEST_ASSERT_TRUE(badge_con_runtime_identity(&peer, &session));
    TEST_ASSERT_FALSE(badge_con_runtime_self_ack_matches(peer, session));

    badge_con_runtime_note_self_ack(peer + 1U, session);
    TEST_ASSERT_FALSE(badge_con_runtime_self_ack_matches(peer, session));
    badge_con_runtime_note_self_ack(peer, session);
    TEST_ASSERT_TRUE(badge_con_runtime_self_ack_matches(peer, session));
}

void test_badge_con_runtime_exposes_sequence_and_clears_self_ack(void)
{
    runtime_test_reset();
    uint8_t sequence = 0xFFU;
    TEST_ASSERT_FALSE(badge_con_runtime_sequence_start(&sequence));
    TEST_ASSERT_EQUAL_HEX8(0U, sequence);

    badge_con_runtime_init();
    uint32_t peer = 0U;
    uint8_t session = 0U;
    TEST_ASSERT_TRUE(badge_con_runtime_identity(&peer, &session));
    TEST_ASSERT_TRUE(badge_con_runtime_sequence_start(&sequence));

    badge_con_runtime_note_self_ack(peer, session);
    TEST_ASSERT_TRUE(badge_con_runtime_self_ack_matches(peer, session));
    badge_con_runtime_clear_self_ack();
    TEST_ASSERT_FALSE(badge_con_runtime_self_ack_matches(peer, session));
    TEST_ASSERT_TRUE(badge_con_runtime_sequence_start(&sequence));
}

void test_badge_con_runtime_preinit_outputs_are_safe_and_hook_is_rtc_only(void)
{
    runtime_test_reset();
    badge_con_snapshot_t snapshot = {
        .seed = BADGE_CON_ROLE_IMMUNE,
        .role = BADGE_CON_ROLE_IMMUNE,
        .active = true,
        .shield = 100U,
    };
    TEST_ASSERT_FALSE(badge_con_runtime_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, snapshot.seed);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, snapshot.role);
    TEST_ASSERT_FALSE(snapshot.active);
    TEST_ASSERT_EQUAL_UINT8(0U, snapshot.shield);

    uint32_t peer = 123U;
    uint8_t session = 45U;
    TEST_ASSERT_FALSE(badge_con_runtime_identity(&peer, &session));
    TEST_ASSERT_EQUAL_UINT32(0U, peer);
    TEST_ASSERT_EQUAL_UINT8(0U, session);

    badge_con_runtime_init();
    unsigned writes_before = s_test_nvs_write_count;
    TEST_ASSERT_NOT_NULL(s_test_expected_reboot_hook);
    TEST_ASSERT_TRUE(s_test_expected_reboot_hook(77U));
    TEST_ASSERT_EQUAL_UINT(writes_before, s_test_nvs_write_count);
    badge_con_game_state_t exact;
    TEST_ASSERT_TRUE(badge_con_game_decode_rtc(
        s_test_game_rtc_record, sizeof(s_test_game_rtc_record),
        77U, &exact));
}

void test_badge_con_runtime_snapshot_reads_committed_state_while_write_is_busy(void)
{
    runtime_test_reset();
    badge_con_runtime_init();
    TEST_ASSERT_TRUE(badge_con_runtime_set_factory_seed(
        BADGE_CON_ROLE_INFECTED));

    badge_con_game_state_t pending;
    TEST_ASSERT_TRUE(begin_mutation(&pending));
    badge_con_game_apply_factory_seed(
        &pending, BADGE_CON_ROLE_IMMUNE, 22U);
    badge_con_snapshot_t visible = {0};
    TEST_ASSERT_TRUE(badge_con_runtime_snapshot(&visible));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, visible.seed);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, visible.role);
    cancel_mutation();
}
