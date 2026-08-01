#include "badge_runtime_rtc_policy.h"

#include "badge_runtime_policy.h"

#if defined(FOF_DC34_GAME_CANARY)
#include "badge_con_game.h"
#include "badge_update_maintenance_policy.h"
#endif

#include <string.h>

static uint16_t load_le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] |
           (uint16_t)((uint16_t)bytes[1] << 8U);
}

static uint32_t load_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static void store_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
}

static void store_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static bool supported_layout(size_t storage_size,
                             uint16_t expected_layout_size)
{
    return storage_size == expected_layout_size &&
           (expected_layout_size == BADGE_RUNTIME_RTC_PRODUCTION_SIZE ||
            expected_layout_size == BADGE_RUNTIME_RTC_CANARY_SIZE);
}

static bool layout_valid(const uint8_t *storage,
                         size_t storage_size,
                         uint16_t expected_layout_size)
{
    return storage &&
           supported_layout(storage_size, expected_layout_size) &&
           load_le32(
               &storage[BADGE_RUNTIME_RTC_LAYOUT_MAGIC_OFFSET]) ==
               BADGE_RUNTIME_RTC_LAYOUT_MAGIC &&
           load_le16(
               &storage[BADGE_RUNTIME_RTC_LAYOUT_VERSION_OFFSET]) ==
               BADGE_RUNTIME_RTC_LAYOUT_VERSION &&
           load_le16(
               &storage[BADGE_RUNTIME_RTC_LAYOUT_SIZE_OFFSET]) ==
               expected_layout_size;
}

static void reset_extension(uint8_t *storage,
                            size_t storage_size,
                            uint16_t expected_layout_size)
{
    memset(
        &storage[BADGE_RUNTIME_RTC_EXTENSION_OFFSET],
        0,
        storage_size - BADGE_RUNTIME_RTC_EXTENSION_OFFSET);
    store_le32(
        &storage[BADGE_RUNTIME_RTC_LAYOUT_MAGIC_OFFSET],
        BADGE_RUNTIME_RTC_LAYOUT_MAGIC);
    store_le16(
        &storage[BADGE_RUNTIME_RTC_LAYOUT_VERSION_OFFSET],
        BADGE_RUNTIME_RTC_LAYOUT_VERSION);
    store_le16(
        &storage[BADGE_RUNTIME_RTC_LAYOUT_SIZE_OFFSET],
        expected_layout_size);
}

#if defined(FOF_DC34_GAME_CANARY)
_Static_assert(sizeof(badge_update_maintenance_marker_t) == 0xA0U,
               "old-canary marker migration size changed");
_Static_assert(BADGE_CON_RTC_RECORD_BYTES == 0x14U,
               "old-canary game migration size changed");

static bool bytes_all_zero(const uint8_t *bytes, size_t byte_count)
{
    uint8_t aggregate = 0U;
    for (size_t i = 0U; i < byte_count; ++i) {
        aggregate |= bytes[i];
    }
    return aggregate == 0U;
}

static bool old_canary_valid(const uint8_t *storage,
                             size_t storage_size,
                             uint16_t expected_layout_size)
{
    if (expected_layout_size != BADGE_RUNTIME_RTC_CANARY_SIZE ||
        storage_size < BADGE_RUNTIME_RTC_OLD_CANARY_SIZE) {
        return false;
    }

    uint32_t generation = load_le32(
        &storage[BADGE_RUNTIME_RTC_OLD_CANARY_GENERATION_OFFSET]);
    if (generation == 0U ||
        load_le32(&storage[BADGE_RUNTIME_RTC_OLD_CANARY_MAGIC_OFFSET]) !=
            BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC) {
        return false;
    }

    badge_con_game_state_t decoded_game = {0};
    if (!badge_con_game_decode_rtc(
            &storage[BADGE_RUNTIME_RTC_OLD_CANARY_GAME_OFFSET],
            BADGE_CON_RTC_RECORD_BYTES,
            generation,
            &decoded_game)) {
        return false;
    }

    const uint8_t *marker_bytes =
        &storage[BADGE_RUNTIME_RTC_OLD_CANARY_MARKER_OFFSET];
    if (bytes_all_zero(
            marker_bytes, sizeof(badge_update_maintenance_marker_t))) {
        return true;
    }

    badge_update_maintenance_marker_t marker;
    memcpy(&marker, marker_bytes, sizeof(marker));
    return badge_update_maintenance_marker_valid(&marker) &&
           badge_update_maintenance_boot_decide(
               &marker, true, generation, false) ==
               BADGE_UPDATE_BOOT_ENTER;
}
#endif

badge_runtime_rtc_boot_result_t badge_runtime_rtc_classify(
    const uint8_t *storage,
    size_t storage_size,
    bool software_reset,
    uint16_t expected_layout_size)
{
    badge_runtime_rtc_boot_result_t result = {
        .source = BADGE_RUNTIME_RTC_SOURCE_NONE,
        .expected_software_reset = false,
        .prior_layout_valid = false,
        .consumed_generation = 0U,
    };
    if (!storage ||
        !supported_layout(storage_size, expected_layout_size)) {
        return result;
    }

    result.prior_layout_valid =
        layout_valid(storage, storage_size, expected_layout_size);
    if (!software_reset) {
        return result;
    }

    uint32_t generation_word = load_le32(
        &storage[BADGE_RUNTIME_RTC_GENERATION_OFFSET]);
    uint32_t magic_word = load_le32(
        &storage[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET]);
    bool current =
        magic_word == BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC &&
        generation_word != 0U;

    /*
     * A valid current header makes the stable core authoritative. When the
     * header is absent, a current token and a fully valid shifted old-canary
     * record must not both authenticate the same bytes.
     */
    if (result.prior_layout_valid && current) {
        result.source = BADGE_RUNTIME_RTC_SOURCE_CURRENT;
        result.expected_software_reset = true;
        result.consumed_generation = generation_word;
        return result;
    }

    bool old_canary = false;
#if defined(FOF_DC34_GAME_CANARY)
    if (!result.prior_layout_valid) {
        old_canary = old_canary_valid(
            storage, storage_size, expected_layout_size);
    }
#endif
    if (current && old_canary) {
        result.source = BADGE_RUNTIME_RTC_SOURCE_AMBIGUOUS;
        return result;
    }
    if (current) {
        result.source = BADGE_RUNTIME_RTC_SOURCE_CURRENT;
        result.expected_software_reset = true;
        result.consumed_generation = generation_word;
        return result;
    }
    if (old_canary) {
        result.source = BADGE_RUNTIME_RTC_SOURCE_OLD_CANARY;
        result.expected_software_reset = true;
        result.consumed_generation = load_le32(
            &storage[BADGE_RUNTIME_RTC_OLD_CANARY_GENERATION_OFFSET]);
        return result;
    }

    /*
     * Original v0.78 stored only its magic at +4. It remains admissible over
     * a stale valid new header after a failed upgrade/rollback, provided +8
     * is not simultaneously a live current token.
     */
    if (generation_word == BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC &&
        magic_word != BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC) {
        result.source = BADGE_RUNTIME_RTC_SOURCE_LEGACY_V078;
        result.expected_software_reset = true;
        result.consumed_generation = 1U;
    }
    return result;
}

badge_runtime_rtc_boot_result_t badge_runtime_rtc_transition(
    uint8_t *storage,
    size_t storage_size,
    bool software_reset,
    uint16_t expected_layout_size)
{
    badge_runtime_rtc_boot_result_t result =
        badge_runtime_rtc_classify(
            storage,
            storage_size,
            software_reset,
            expected_layout_size);
    if (!storage ||
        !supported_layout(storage_size, expected_layout_size)) {
        return result;
    }

#if defined(FOF_DC34_GAME_CANARY)
    uint8_t old_game[BADGE_CON_RTC_RECORD_BYTES] = {0};
    badge_update_maintenance_marker_t old_marker = {0};
    uint32_t old_recovery = 0U;
    if (result.source == BADGE_RUNTIME_RTC_SOURCE_OLD_CANARY) {
        /*
         * Snapshot every admitted legacy byte before reset_extension() can
         * overwrite the shifted source ranges.
         */
        memcpy(
            old_game,
            &storage[BADGE_RUNTIME_RTC_OLD_CANARY_GAME_OFFSET],
            sizeof(old_game));
        memcpy(
            &old_marker,
            &storage[BADGE_RUNTIME_RTC_OLD_CANARY_MARKER_OFFSET],
            sizeof(old_marker));
        old_recovery = load_le32(
            &storage[BADGE_RUNTIME_RTC_OLD_CANARY_RECOVERY_OFFSET]);
    }
#endif

    switch (result.source) {
        case BADGE_RUNTIME_RTC_SOURCE_CURRENT:
            store_le32(
                &storage[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET], 0U);
            if (result.consumed_generation ==
                BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC) {
                store_le32(
                    &storage[BADGE_RUNTIME_RTC_GENERATION_OFFSET], 0U);
            }
            if (!result.prior_layout_valid) {
                reset_extension(
                    storage, storage_size, expected_layout_size);
            }
            break;

        case BADGE_RUNTIME_RTC_SOURCE_OLD_CANARY:
#if defined(FOF_DC34_GAME_CANARY)
            reset_extension(storage, storage_size, expected_layout_size);
            store_le32(
                &storage[BADGE_RUNTIME_RTC_RECOVERY_OFFSET],
                old_recovery);
            store_le32(
                &storage[BADGE_RUNTIME_RTC_GENERATION_OFFSET],
                result.consumed_generation ==
                        BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC
                    ? 0U
                    : result.consumed_generation);
            store_le32(
                &storage[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET], 0U);
            memcpy(
                &storage[BADGE_RUNTIME_RTC_CANARY_MARKER_OFFSET],
                &old_marker,
                sizeof(old_marker));
            memcpy(
                &storage[BADGE_RUNTIME_RTC_CANARY_GAME_OFFSET],
                old_game,
                sizeof(old_game));
#else
            store_le32(
                &storage[BADGE_RUNTIME_RTC_GENERATION_OFFSET], 0U);
            store_le32(
                &storage[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET], 0U);
            reset_extension(storage, storage_size, expected_layout_size);
#endif
            break;

        case BADGE_RUNTIME_RTC_SOURCE_LEGACY_V078:
            store_le32(
                &storage[BADGE_RUNTIME_RTC_GENERATION_OFFSET], 0U);
            store_le32(
                &storage[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET], 0U);
            /* Always destroy stale new-layout tails on a downgrade retry. */
            reset_extension(storage, storage_size, expected_layout_size);
            break;

        case BADGE_RUNTIME_RTC_SOURCE_AMBIGUOUS:
            store_le32(
                &storage[BADGE_RUNTIME_RTC_GENERATION_OFFSET], 0U);
            store_le32(
                &storage[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET], 0U);
            reset_extension(storage, storage_size, expected_layout_size);
            break;

        case BADGE_RUNTIME_RTC_SOURCE_NONE:
        default:
            store_le32(
                &storage[BADGE_RUNTIME_RTC_GENERATION_OFFSET], 0U);
            store_le32(
                &storage[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET], 0U);
            if (!result.prior_layout_valid) {
                reset_extension(
                    storage, storage_size, expected_layout_size);
            }
            break;
    }
    return result;
}
