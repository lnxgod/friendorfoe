/**
 * Friend or Foe — Scanner Firmware Store + Reliable UART Relay
 *
 * Store-then-forward OTA: upload scanner firmware to flash via HTTP,
 * then relay to scanner UART with CRC32 + ACK/NACK retransmission.
 *
 * Uses esp_ota_begin/write/abort for flash storage — the OTA API
 * handles erase scheduling internally, preventing TCP drops that
 * killed the raw partition approach on heap-constrained ESP32.
 */

#include "fw_store.h"
#include "config.h"
#include "nvs_config.h"
#include "uart_protocol.h"
#include "uart_rx.h"
#include "http_upload.h"
#include "version.h"
#include "firmware_version_order.h"
#include "firmware_legacy_ready.h"
#include "firmware_relay_policy.h"
#include "firmware_auto_policy.h"
#include "firmware_coordinator_migration.h"
#include "detection_policy.h"
#ifdef FOF_BADGE_VARIANT
#include "badge_power_runtime.h"
#endif

#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "psram_alloc.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <sys/socket.h>

static const char *TAG = "fw_store";

/* ── State ───────────────────────────────────────────────────────────────── */

static bool s_operation_active = false;
static bool s_operation_uart_lease = false;
static portMUX_TYPE s_operation_lock = portMUX_INITIALIZER_UNLOCKED;

static bool operation_try_begin(void)
{
    bool acquired = false;
    portENTER_CRITICAL(&s_operation_lock);
    if (!s_operation_active) {
        s_operation_active = true;
        acquired = true;
    }
    portEXIT_CRITICAL(&s_operation_lock);
    if (acquired &&
        !uart_rx_scanner_tx_lease_acquire(pdMS_TO_TICKS(2000))) {
        portENTER_CRITICAL(&s_operation_lock);
        s_operation_active = false;
        portEXIT_CRITICAL(&s_operation_lock);
        ESP_LOGE(TAG, "Firmware operation refused: scanner UART lease unavailable");
        return false;
    }
    if (acquired) {
        portENTER_CRITICAL(&s_operation_lock);
        s_operation_uart_lease = true;
        portEXIT_CRITICAL(&s_operation_lock);
    }
    return acquired;
}

static void operation_end(void)
{
    bool release_uart_lease = false;
    portENTER_CRITICAL(&s_operation_lock);
    s_operation_active = false;
    release_uart_lease = s_operation_uart_lease;
    s_operation_uart_lease = false;
    portEXIT_CRITICAL(&s_operation_lock);
    if (release_uart_lease) {
        uart_rx_scanner_tx_lease_release();
    }
}

static bool operation_is_active(void)
{
    portENTER_CRITICAL(&s_operation_lock);
    bool active = s_operation_active;
    portEXIT_CRITICAL(&s_operation_lock);
    return active;
}

typedef struct {
    bool ok;
    uint32_t size;
    uint32_t bytes;
    char stage[24];
    char error[64];
    int chunks;
    int nacks;
    int retries;
    int64_t elapsed_s;
    int64_t finished_ms;
    uint32_t cmd_rx_before;
    uint32_t cmd_rx_after;
    uint32_t fw_check_before;
    uint32_t fw_check_after;
    int64_t cmd_age_after_s;
    char scanner_version[32];
    char scanner_fw_state[16];
} fw_last_relay_state_t;

static fw_last_relay_state_t s_last_relay[2] = {0};

/* ── OTA upload staging buffer (P4) ──────────────────────────────────────
 * Lazily allocated on first upload. 64 KB on PSRAM collapses the
 * recv/esp_ota_write loop from ~280 iterations per 1.1 MB scanner firmware
 * down to ~18, visibly cutting upload latency. Falls back to a 4 KB static
 * on boards without PSRAM — matches the heap-stability baseline. */
#define FW_STAGE_BUF_PSRAM   (64 * 1024)
#define FW_STAGE_BUF_FALLBACK 4096

/* Busy scanners can miss a single control line while their TX queue is full.
 * Keep re-sending the quiet request before relay, but still require proof. */
#define FW_RELAY_STOP_STORM_MS       8000
#define FW_RELAY_STOP_STORM_STEP_MS  250
#ifdef FOF_BADGE_VARIANT
#define FW_RELAY_NACK_POLL_MS        OTA_RELAY_BADGE_NACK_DRAIN_MS
#define FW_RELAY_CHUNK_DATA          OTA_CHUNK_BADGE_MAX_DATA
#else
#define FW_RELAY_NACK_POLL_MS        120
#define FW_RELAY_CHUNK_DATA          256
#endif
#define FW_RELAY_NACK_SETTLE_MS       20

static uint8_t  s_fw_stage_fallback[FW_STAGE_BUF_FALLBACK];
static uint8_t *s_fw_stage_buf = NULL;     /* resolved on first use */
static size_t   s_fw_stage_cap = 0;

typedef struct {
    bool active;
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    uint32_t size;
    uint32_t received;
    uint32_t crc32;
    uint32_t expected_crc32;
    uint8_t target_slot_mask;
    char name[32];
    char version[32];
    char expected_sha256[FOF_FIRMWARE_SHA256_HEX_SIZE];
} serial_upload_state_t;

static serial_upload_state_t s_serial_upload;

static bool auto_coordinator_begin_generation(uint32_t generation,
                                              uint8_t target_slot_mask,
                                              uint32_t manifest_crc32);
static bool auto_coordinator_force_fail_closed(uint32_t generation,
                                               uint32_t manifest_crc32);
static bool auto_coordinator_reprompt_requested(void);
static bool auto_coordinator_start_worker(void);
static void
auto_coordinator_release_excluded_slots(void);
static bool auto_hardware_id_is_canonical(const char *hardware_id);
static bool auto_identity_matches_manifest(
    const scanner_identity_snapshot_t *identity,
    const fw_store_info_t *info,
    const char *scanner_board,
    const char *scanner_version);

static void fw_stage_buf_ensure(void)
{
    if (s_fw_stage_buf) return;
    uint8_t *p = (uint8_t *)psram_alloc_strict(FW_STAGE_BUF_PSRAM);
    if (p) {
        s_fw_stage_buf = p;
        s_fw_stage_cap = FW_STAGE_BUF_PSRAM;
        ESP_LOGW(TAG, "OTA upload stage buffer: %u KB in PSRAM",
                 (unsigned)(FW_STAGE_BUF_PSRAM / 1024));
    } else {
        s_fw_stage_buf = s_fw_stage_fallback;
        s_fw_stage_cap = FW_STAGE_BUF_FALLBACK;
        ESP_LOGI(TAG, "OTA upload stage buffer: %u KB internal (no PSRAM)",
                 (unsigned)(FW_STAGE_BUF_FALLBACK / 1024));
    }
}

bool fw_store_is_relay_active(void) { return operation_is_active(); }

/* ── NVS metadata keys ───────────────────────────────────────────────────── */

#define NVS_FW_SIZE     "fw_size"
#define NVS_FW_CKSUM    "fw_cksum"
#define NVS_FW_CRC32    "fw_crc32"
#define NVS_FW_VER      "fw_ver"
#define NVS_FW_NAME     "fw_name"
#define NVS_FW_PART     "fw_part"
#define NVS_FW_VALID    "fw_valid"
#define NVS_FW_GEN      "fw_gen"
#define NVS_FW_MCRC     "fw_mcrc"
#define NVS_FW_SHA256   "fw_sha256"
#define NVS_FW_PROJECT  "fw_project"
#define NVS_FW_HW       "fw_hw"
#define NVS_FW_SLOT_MASK "fw_slotmask"
#define NVS_FW_COORDINATOR "fw_coord"

#define FW_AUTO_TARGET_ALL ((uint8_t)FW_AUTO_UPDATE_SLOT_ALL)

#define FW_MANIFEST_COMMITTED_MAGIC 0xF0F34A11u

typedef struct {
    const char *target;
    const char *project;
    const char *hardware;
} fw_target_contract_t;

static const fw_target_contract_t s_target_contracts[] = {
    {"scanner-s3-combo", "fof_scanner", "esp32-s3-devkitc-1"},
    {"scanner-s3-combo-seed", "fof_scanner_seed", "esp32-s3-devkitc-1"},
    {"scanner-s3-combo-fof_badge", "fof_badge_scanner", "seeed_xiao_esp32s3"},
};

static const fw_target_contract_t *find_target_contract(const char *target)
{
    if (!target || !target[0]) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_target_contracts) / sizeof(s_target_contracts[0]); ++i) {
        if (strcmp(target, s_target_contracts[i].target) == 0) {
            return &s_target_contracts[i];
        }
    }
    return NULL;
}

static const char *version_without_prefix(const char *version)
{
    if (!version) {
        return "";
    }
    return (version[0] == 'v' || version[0] == 'V') ? version + 1 : version;
}

static uint32_t manifest_crc_feed_string(uint32_t crc, const char *value)
{
    const char *safe = value ? value : "";
    return esp_rom_crc32_le(crc, (const uint8_t *)safe, strlen(safe) + 1);
}

static uint32_t fw_manifest_crc(const fw_store_info_t *info)
{
    uint32_t crc = 0;
    crc = esp_rom_crc32_le(crc, (const uint8_t *)&info->generation,
                           sizeof(info->generation));
    crc = esp_rom_crc32_le(crc, (const uint8_t *)&info->size,
                           sizeof(info->size));
    crc = esp_rom_crc32_le(crc, (const uint8_t *)&info->checksum,
                           sizeof(info->checksum));
    crc = esp_rom_crc32_le(crc, (const uint8_t *)&info->target_slot_mask,
                           sizeof(info->target_slot_mask));
    crc = manifest_crc_feed_string(crc, info->version);
    crc = manifest_crc_feed_string(crc, info->name);
    crc = manifest_crc_feed_string(crc, info->project);
    crc = manifest_crc_feed_string(crc, info->hardware);
    crc = manifest_crc_feed_string(crc, info->sha256);
    crc = manifest_crc_feed_string(crc, info->partition);
    return crc;
}

static bool invalidate_fw_metadata(void)
{
    /* Validity is cleared and committed before any flash erase/write. Even if
     * a later NVS write or power cycle interrupts staging, readers fail closed. */
    bool valid_cleared = nvs_config_set_u32(NVS_FW_VALID, 0);
    bool size_cleared = nvs_config_set_u32(NVS_FW_SIZE, 0);
    return valid_cleared && size_cleared;
}

/** Read only a fully committed, internally consistent manifest. */
static bool read_fw_metadata(fw_store_info_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    uint32_t valid = 0;
    uint32_t stored_crc = 0;
    uint32_t stored_slot_mask = 0;
    if (!nvs_config_get_u32(NVS_FW_VALID, &valid) ||
        valid != FW_MANIFEST_COMMITTED_MAGIC ||
        !nvs_config_get_u32(NVS_FW_GEN, &out->generation) ||
        !nvs_config_get_u32(NVS_FW_SIZE, &out->size) || out->size == 0 ||
        !nvs_config_get_u32(NVS_FW_CRC32, &out->checksum) ||
        !nvs_config_get_u32(NVS_FW_SLOT_MASK, &stored_slot_mask) ||
        !nvs_config_get_u32(NVS_FW_MCRC, &stored_crc) ||
        !nvs_config_get_string(NVS_FW_VER, out->version, sizeof(out->version)) ||
        !nvs_config_get_string(NVS_FW_NAME, out->name, sizeof(out->name)) ||
        !nvs_config_get_string(NVS_FW_PROJECT, out->project, sizeof(out->project)) ||
        !nvs_config_get_string(NVS_FW_HW, out->hardware, sizeof(out->hardware)) ||
        !nvs_config_get_string(NVS_FW_SHA256, out->sha256, sizeof(out->sha256)) ||
        !nvs_config_get_string(NVS_FW_PART, out->partition, sizeof(out->partition))) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    if (stored_slot_mask == 0 ||
        (stored_slot_mask & (uint32_t)~FW_AUTO_TARGET_ALL) != 0) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->target_slot_mask = (uint8_t)stored_slot_mask;

    const fw_target_contract_t *contract = find_target_contract(out->name);
    if (!contract || strcmp(out->project, contract->project) != 0 ||
        strcmp(out->hardware, contract->hardware) != 0 ||
        !fof_firmware_sha256_hex_is_valid(out->sha256) ||
        fof_firmware_version_compare(out->version, out->version) != FOF_VERSION_EQUAL ||
        fw_manifest_crc(out) != stored_crc) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    out->manifest_crc32 = stored_crc;
    out->stored = true;
    return true;
}

/* Forward decl — defined below near the http handlers. */
static const esp_partition_t *get_store_partition(void);
static void resume_all_tasks(void);

typedef struct {
    const uint8_t *pattern;
    size_t length;
    size_t matched;
    bool found;
} fw_pattern_matcher_t;

static void pattern_matcher_feed(fw_pattern_matcher_t *matcher,
                                 const uint8_t *data,
                                 size_t length)
{
    if (!matcher || matcher->found || !data) {
        return;
    }
    for (size_t i = 0; i < length && !matcher->found; ++i) {
        uint8_t byte = data[i];
        if (byte == matcher->pattern[matcher->matched]) {
            matcher->matched++;
            matcher->found = matcher->matched == matcher->length;
        } else {
            matcher->matched = byte == matcher->pattern[0] ? 1 : 0;
        }
    }
}

static void canonicalize_sha256(const char *input,
                                char output[FOF_FIRMWARE_SHA256_HEX_SIZE])
{
    for (size_t i = 0; i < FOF_FIRMWARE_SHA256_HEX_LENGTH; ++i) {
        char ch = input[i];
        output[i] = (ch >= 'A' && ch <= 'F') ? (char)(ch - 'A' + 'a') : ch;
    }
    output[FOF_FIRMWARE_SHA256_HEX_LENGTH] = '\0';
}

static bool validate_staged_image(const esp_partition_t *partition,
                                  uint32_t size,
                                  uint32_t crc32,
                                  const char *expected_target,
                                  const char *expected_version,
                                  const char *expected_sha256,
                                  fw_store_info_t *out,
                                  char *error,
                                  size_t error_len)
{
    const fw_target_contract_t *contract = find_target_contract(expected_target);
#ifdef FOF_BADGE_VARIANT
    if (!contract || strcmp(contract->target, "scanner-s3-combo-fof_badge") != 0) {
#else
    if (!contract) {
#endif
        if (error && error_len) snprintf(error, error_len, "target_refused");
        return false;
    }
    if (!partition || size < 1024 || size > partition->size) {
        if (error && error_len) snprintf(error, error_len, "invalid_image_size");
        return false;
    }
    if (expected_sha256 && !fof_firmware_sha256_hex_is_valid(expected_sha256)) {
        if (error && error_len) snprintf(error, error_len, "invalid_sha256");
        return false;
    }

    uint8_t prefix[160] = {0};
    if (esp_partition_read(partition, 0, prefix, sizeof(prefix)) != ESP_OK) {
        if (error && error_len) snprintf(error, error_len, "descriptor_read_failed");
        return false;
    }
    fof_firmware_image_identity_t identity = {0};
    if (!fof_firmware_image_parse_identity(prefix, sizeof(prefix), &identity)) {
        if (error && error_len) snprintf(error, error_len, "invalid_app_descriptor");
        return false;
    }
    if (strcmp(identity.project, contract->project) != 0) {
        if (error && error_len) snprintf(error, error_len, "project_mismatch");
        return false;
    }
    if (fof_firmware_version_compare(identity.version, identity.version) !=
        FOF_VERSION_EQUAL) {
        if (error && error_len) snprintf(error, error_len, "invalid_embedded_version");
        return false;
    }
    if (expected_version && expected_version[0] &&
        strcmp(version_without_prefix(expected_version),
               version_without_prefix(identity.version)) != 0) {
        if (error && error_len) snprintf(error, error_len, "version_mismatch");
        return false;
    }

    const size_t target_pattern_len = strlen(contract->target) + 1;
    const size_t hardware_pattern_len = strlen(contract->hardware) + 1;
    fw_pattern_matcher_t target_matcher = {
        .pattern = (const uint8_t *)contract->target,
        .length = target_pattern_len,
    };
    fw_pattern_matcher_t hardware_matcher = {
        .pattern = (const uint8_t *)contract->hardware,
        .length = hardware_pattern_len,
    };

    fw_stage_buf_ensure();
    mbedtls_sha256_context sha_ctx;
    uint8_t digest[FOF_FIRMWARE_SHA256_SIZE];
    mbedtls_sha256_init(&sha_ctx);
    if (mbedtls_sha256_starts(&sha_ctx, 0) != 0) {
        mbedtls_sha256_free(&sha_ctx);
        if (error && error_len) snprintf(error, error_len, "sha256_start_failed");
        return false;
    }

    uint32_t offset = 0;
    bool read_ok = true;
    while (offset < size) {
        size_t chunk = size - offset;
        if (chunk > s_fw_stage_cap) chunk = s_fw_stage_cap;
        if (esp_partition_read(partition, offset, s_fw_stage_buf, chunk) != ESP_OK ||
            mbedtls_sha256_update(&sha_ctx, s_fw_stage_buf, chunk) != 0) {
            read_ok = false;
            break;
        }
        pattern_matcher_feed(&target_matcher, s_fw_stage_buf, chunk);
        pattern_matcher_feed(&hardware_matcher, s_fw_stage_buf, chunk);
        offset += (uint32_t)chunk;
    }
    bool sha_ok = read_ok && mbedtls_sha256_finish(&sha_ctx, digest) == 0;
    mbedtls_sha256_free(&sha_ctx);
    if (!sha_ok) {
        if (error && error_len) snprintf(error, error_len, "flash_verify_failed");
        return false;
    }
    if (!target_matcher.found || !hardware_matcher.found) {
        if (error && error_len) {
            snprintf(error, error_len, "%s_marker_missing",
                     !target_matcher.found ? "target" : "hardware");
        }
        return false;
    }

    char computed_sha256[FOF_FIRMWARE_SHA256_HEX_SIZE];
    fof_firmware_sha256_to_hex(digest, computed_sha256);
    if (expected_sha256) {
        char canonical_expected[FOF_FIRMWARE_SHA256_HEX_SIZE];
        canonicalize_sha256(expected_sha256, canonical_expected);
        unsigned difference = 0;
        for (size_t i = 0; i < FOF_FIRMWARE_SHA256_HEX_LENGTH; ++i) {
            difference |= (unsigned)(computed_sha256[i] ^ canonical_expected[i]);
        }
        if (difference != 0) {
            if (error && error_len) snprintf(error, error_len, "sha256_mismatch");
            return false;
        }
    }

    if (out) {
        memset(out, 0, sizeof(*out));
        out->size = size;
        out->checksum = crc32;
        strncpy(out->version, identity.version, sizeof(out->version) - 1);
        strncpy(out->name, contract->target, sizeof(out->name) - 1);
        strncpy(out->project, identity.project, sizeof(out->project) - 1);
        strncpy(out->hardware, contract->hardware, sizeof(out->hardware) - 1);
        strncpy(out->sha256, computed_sha256, sizeof(out->sha256) - 1);
        strncpy(out->partition, partition->label, sizeof(out->partition) - 1);
    }
    return true;
}

static bool poison_staged_image(const esp_partition_t *partition)
{
    if (!partition) {
        return false;
    }
    uint8_t header[16] = {0};
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint8_t magic = ESP_IMAGE_HEADER_MAGIC;
        if (esp_partition_read(partition, 0, &magic, sizeof(magic)) == ESP_OK &&
            magic != ESP_IMAGE_HEADER_MAGIC) {
            return true;
        }
        if (esp_partition_write(partition, 0, header, sizeof(header)) != ESP_OK) {
            continue;
        }
        magic = ESP_IMAGE_HEADER_MAGIC;
        if (esp_partition_read(partition, 0, &magic, sizeof(magic)) == ESP_OK &&
            magic != ESP_IMAGE_HEADER_MAGIC) {
            ESP_LOGE(TAG, "Poisoned staged firmware descriptor after commit failure");
            return true;
        }
    }
    ESP_LOGE(TAG, "CRITICAL: unable to poison staged firmware descriptor");
    return false;
}

static bool persist_validated_metadata(fw_store_info_t *info)
{
    if (!info || !find_target_contract(info->name) ||
        !fof_firmware_sha256_hex_is_valid(info->sha256) ||
        info->target_slot_mask == 0 ||
        (info->target_slot_mask & (uint8_t)~FW_AUTO_TARGET_ALL) != 0) {
        return false;
    }

    uint32_t prior_generation = 0;
    bool prior_generation_known =
        nvs_config_get_u32(NVS_FW_GEN, &prior_generation);
    if (prior_generation_known && prior_generation != UINT32_MAX) {
        info->generation = prior_generation + 1;
    } else {
        do {
            info->generation = esp_random();
        } while (info->generation == 0 ||
                 (prior_generation_known &&
                  info->generation == prior_generation));
    }
    uint32_t manifest_crc = fw_manifest_crc(info);
    info->manifest_crc32 = manifest_crc;

    if (!invalidate_fw_metadata()) {
        return false;
    }
    bool fields_ok =
        nvs_config_set_u32(NVS_FW_GEN, info->generation) &&
        nvs_config_set_u32(NVS_FW_SIZE, info->size) &&
        nvs_config_set_u32(NVS_FW_CKSUM, info->checksum) &&
        nvs_config_set_u32(NVS_FW_CRC32, info->checksum) &&
        nvs_config_set_u32(NVS_FW_SLOT_MASK, info->target_slot_mask) &&
        nvs_config_set_string(NVS_FW_VER, info->version) &&
        nvs_config_set_string(NVS_FW_NAME, info->name) &&
        nvs_config_set_string(NVS_FW_PROJECT, info->project) &&
        nvs_config_set_string(NVS_FW_HW, info->hardware) &&
        nvs_config_set_string(NVS_FW_SHA256, info->sha256) &&
        nvs_config_set_string(NVS_FW_PART, info->partition) &&
        nvs_config_set_u32(NVS_FW_MCRC, manifest_crc);
    if (!fields_ok) {
        return false;
    }
    if (!nvs_config_set_u32(NVS_FW_VALID, FW_MANIFEST_COMMITTED_MAGIC)) {
        return false;
    }
    info->stored = true;
    return true;
}

const esp_partition_t *fw_store_get_target_partition(void)
{
    return get_store_partition();
}

bool fw_store_persist_metadata(const char *name, const char *version,
                               const esp_partition_t *partition,
                               uint32_t size, uint32_t crc32)
{
    fw_store_info_t info = {0};
    char error[48] = {0};
    if (!validate_staged_image(partition, size, crc32, name, version, NULL,
                               &info, error, sizeof(error))) {
        ESP_LOGE(TAG, "Refusing staged firmware metadata: %s", error);
        invalidate_fw_metadata();
        return false;
    }
    info.target_slot_mask = FW_AUTO_UPDATE_SLOT_ALL;
    if (!persist_validated_metadata(&info)) {
        ESP_LOGE(TAG, "Failed to commit staged firmware manifest");
        (void)poison_staged_image(partition);
        (void)invalidate_fw_metadata();
        (void)auto_coordinator_force_fail_closed(
            info.generation, info.manifest_crc32);
        auto_coordinator_release_excluded_slots();
        return false;
    }
    if (!auto_coordinator_begin_generation(info.generation,
                                           info.target_slot_mask,
                                           info.manifest_crc32)) {
        ESP_LOGE(TAG, "Failed to commit automatic-update coordinator");
        (void)poison_staged_image(partition);
        (void)invalidate_fw_metadata();
        (void)auto_coordinator_force_fail_closed(
            info.generation, info.manifest_crc32);
        auto_coordinator_release_excluded_slots();
        return false;
    }
    ESP_LOGW(TAG,
             "Committed firmware manifest gen=%lu target=%s project=%s "
             "hardware=%s version=%s sha256=%s",
             (unsigned long)info.generation,
             info.name, info.project, info.hardware, info.version, info.sha256);
    return true;
}

bool fw_store_serial_upload_active(void)
{
    return s_serial_upload.active;
}

uint32_t fw_store_serial_upload_remaining(void)
{
    if (!s_serial_upload.active || s_serial_upload.received >= s_serial_upload.size) {
        return 0;
    }
    return s_serial_upload.size - s_serial_upload.received;
}

void fw_store_serial_upload_abort(const char *reason)
{
    if (!s_serial_upload.active) {
        return;
    }
    ESP_LOGW(TAG, "USB firmware staging aborted at %lu/%lu: %s",
             (unsigned long)s_serial_upload.received,
             (unsigned long)s_serial_upload.size,
             reason ? reason : "?");
    if (s_serial_upload.handle) {
        esp_ota_abort(s_serial_upload.handle);
    }
    memset(&s_serial_upload, 0, sizeof(s_serial_upload));
    resume_all_tasks();
    operation_end();
}

bool fw_store_serial_upload_begin(const char *name,
                                  const char *version,
                                  uint32_t size,
                                  uint32_t expected_crc32,
                                  const char *expected_sha256,
                                  uint8_t target_slot_mask,
                                  char *out_json,
                                  size_t out_json_len)
{
    if (s_serial_upload.active) {
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"operation_active\"}");
        }
        return false;
    }

    const fw_target_contract_t *contract = find_target_contract(name);
    const esp_partition_t *p = get_store_partition();
    bool target_allowed = contract != NULL;
#ifdef FOF_BADGE_VARIANT
    target_allowed = target_allowed &&
        strcmp(name, "scanner-s3-combo-fof_badge") == 0;
#endif
    bool version_valid = version && version[0] &&
        fof_firmware_version_compare(version, version) == FOF_VERSION_EQUAL;
    if (!p || size < 1024 || size > p->size || expected_crc32 == 0 ||
        target_slot_mask == 0 ||
        (target_slot_mask & (uint8_t)~FW_AUTO_TARGET_ALL) != 0 ||
        !target_allowed || !version_valid ||
        !fof_firmware_sha256_hex_is_valid(expected_sha256)) {
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"invalid_manifest\",\"max\":%lu}",
                     p ? (unsigned long)p->size : 0UL);
        }
        return false;
    }

    if (!operation_try_begin()) {
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"operation_active\"}");
        }
        return false;
    }

    memset(&s_serial_upload, 0, sizeof(s_serial_upload));
    http_upload_pause();
    uart_rx_pause_scanner(0);
#if CONFIG_DUAL_SCANNER
    uart_rx_pause_scanner(1);
#endif
    vTaskDelay(pdMS_TO_TICKS(200));

    if (!invalidate_fw_metadata()) {
        resume_all_tasks();
        operation_end();
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"manifest_invalidate_failed\"}");
        }
        return false;
    }

    esp_err_t err = esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES,
                                  &s_serial_upload.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB staging esp_ota_begin failed: %s on '%s'",
                 esp_err_to_name(err), p->label);
        resume_all_tasks();
        operation_end();
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"esp_ota_begin:%s\"}",
                     esp_err_to_name(err));
        }
        return false;
    }

    s_serial_upload.active = true;
    s_serial_upload.partition = p;
    s_serial_upload.size = size;
    s_serial_upload.expected_crc32 = expected_crc32;
    s_serial_upload.target_slot_mask = target_slot_mask;
    strncpy(s_serial_upload.name,
            (name && name[0]) ? name : "scanner-s3-combo-fof_badge",
            sizeof(s_serial_upload.name) - 1);
    strncpy(s_serial_upload.version,
            version,
            sizeof(s_serial_upload.version) - 1);
    canonicalize_sha256(expected_sha256, s_serial_upload.expected_sha256);

    ESP_LOGW(TAG, "USB staging scanner firmware: %lu bytes to '%s' crc=%08lX",
             (unsigned long)size, p->label, (unsigned long)expected_crc32);
    if (out_json && out_json_len) {
        snprintf(out_json, out_json_len,
                 "{\"ok\":true,\"partition\":\"%s\",\"size\":%lu,"
                 "\"crc32\":%lu,\"sha256\":\"%s\","
                 "\"target\":\"%s\",\"name\":\"%s\","
                 "\"app_project\":\"%s\",\"project\":\"%s\","
                 "\"hardware_type\":\"%s\",\"hardware\":\"%s\","
                 "\"version\":\"%s\",\"slot_mask\":%u}",
                 p->label, (unsigned long)size,
                 (unsigned long)expected_crc32,
                 s_serial_upload.expected_sha256,
                 s_serial_upload.name,
                 s_serial_upload.name,
                 contract->project,
                 contract->project,
                 contract->hardware,
                 contract->hardware,
                 s_serial_upload.version,
                 (unsigned)s_serial_upload.target_slot_mask);
    }
    return true;
}

bool fw_store_serial_upload_write(const uint8_t *data,
                                  size_t len,
                                  char *out_json,
                                  size_t out_json_len)
{
    if (!s_serial_upload.active || !data || len == 0) {
        return true;
    }
    uint32_t remaining = s_serial_upload.size - s_serial_upload.received;
    if (len > remaining) {
        fw_store_serial_upload_abort("too_many_bytes");
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"too_many_bytes\"}");
        }
        return false;
    }
    esp_err_t err = esp_ota_write(s_serial_upload.handle, data, len);
    if (err != ESP_OK) {
        fw_store_serial_upload_abort("write_failed");
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"write:%s\"}",
                     esp_err_to_name(err));
        }
        return false;
    }
    s_serial_upload.crc32 = esp_rom_crc32_le(s_serial_upload.crc32, data, len);
    s_serial_upload.received += (uint32_t)len;
    return true;
}

bool fw_store_serial_upload_end(char *out_json, size_t out_json_len)
{
    if (!s_serial_upload.active) {
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"not_active\"}");
        }
        return false;
    }
    if (s_serial_upload.received != s_serial_upload.size) {
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"incomplete\",\"received\":%lu,"
                     "\"size\":%lu}",
                     (unsigned long)s_serial_upload.received,
                     (unsigned long)s_serial_upload.size);
        }
        fw_store_serial_upload_abort("incomplete");
        return false;
    }
    if (s_serial_upload.crc32 != s_serial_upload.expected_crc32) {
        uint32_t got = s_serial_upload.crc32;
        uint32_t expected = s_serial_upload.expected_crc32;
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"crc_mismatch\","
                     "\"expected\":%lu,\"got\":%lu}",
                     (unsigned long)expected, (unsigned long)got);
        }
        fw_store_serial_upload_abort("crc_mismatch");
        return false;
    }

    esp_ota_abort(s_serial_upload.handle);
    s_serial_upload.handle = 0;

    fw_store_info_t info = {0};
    char validation_error[48] = {0};
    if (!validate_staged_image(s_serial_upload.partition,
                               s_serial_upload.size,
                               s_serial_upload.crc32,
                               s_serial_upload.name,
                               s_serial_upload.version,
                               s_serial_upload.expected_sha256,
                               &info,
                               validation_error,
                               sizeof(validation_error))) {
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"%s\"}",
                     validation_error[0] ? validation_error : "image_validation_failed");
        }
        fw_store_serial_upload_abort(validation_error[0]
                                         ? validation_error
                                         : "image_validation_failed");
        return false;
    }
    info.target_slot_mask = s_serial_upload.target_slot_mask;
    if (!persist_validated_metadata(&info)) {
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"manifest_commit_failed\"}");
        }
        (void)poison_staged_image(s_serial_upload.partition);
        (void)invalidate_fw_metadata();
        (void)auto_coordinator_force_fail_closed(
            info.generation, info.manifest_crc32);
        fw_store_serial_upload_abort("manifest_commit_failed");
        auto_coordinator_release_excluded_slots();
        return false;
    }
    if (!auto_coordinator_begin_generation(
            info.generation, info.target_slot_mask,
            info.manifest_crc32)) {
        bool image_poisoned =
            poison_staged_image(s_serial_upload.partition);
        bool manifest_invalidated = invalidate_fw_metadata();
        bool coordinator_failed_closed =
            auto_coordinator_force_fail_closed(
                info.generation, info.manifest_crc32);
        ESP_LOGE(TAG,
                 "Coordinator commit failed for generation %lu; "
                 "image_poisoned=%d manifest_invalidated=%d fail_closed=%d",
                 (unsigned long)info.generation,
                 image_poisoned ? 1 : 0,
                 manifest_invalidated ? 1 : 0,
                 coordinator_failed_closed ? 1 : 0);
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"coordinator_commit_failed\","
                     "\"generation\":%lu,\"slot_mask\":%u}",
                     (unsigned long)info.generation,
                     (unsigned)info.target_slot_mask);
        }
        fw_store_serial_upload_abort("coordinator_commit_failed");
        auto_coordinator_release_excluded_slots();
        return false;
    }
    if (out_json && out_json_len) {
        snprintf(out_json, out_json_len,
                 "{\"ok\":true,\"partition\":\"%s\",\"size\":%lu,"
                 "\"crc32\":%lu,\"sha256\":\"%s\","
                 "\"target\":\"%s\",\"name\":\"%s\","
                 "\"app_project\":\"%s\",\"project\":\"%s\","
                 "\"hardware_type\":\"%s\",\"hardware\":\"%s\","
                 "\"version\":\"%s\",\"generation\":%lu,"
                 "\"slot_mask\":%u}",
                 info.partition,
                 (unsigned long)info.size,
                 (unsigned long)info.checksum,
                 info.sha256,
                 info.name,
                 info.name,
                 info.project,
                 info.project,
                 info.hardware,
                 info.hardware,
                 info.version,
                 (unsigned long)info.generation,
                 (unsigned)info.target_slot_mask);
    }
    ESP_LOGW(TAG,
             "USB firmware staged: gen=%lu %lu bytes CRC=%08lX "
             "target=%s project=%s hardware=%s version=%s SHA256=%s",
             (unsigned long)info.generation,
             (unsigned long)info.size,
             (unsigned long)info.checksum,
             info.name, info.project, info.hardware, info.version, info.sha256);
    memset(&s_serial_upload, 0, sizeof(s_serial_upload));
    resume_all_tasks();
    operation_end();
    auto_coordinator_release_excluded_slots();
    (void)auto_coordinator_reprompt_requested();
    (void)auto_coordinator_start_worker();
    return true;
}

bool fw_store_get_info(fw_store_info_t *out)
{
    return read_fw_metadata(out);
}

/** Clear stored firmware metadata from NVS. */
static void clear_fw_metadata(void)
{
    invalidate_fw_metadata();
}

/** Find the partition where firmware is stored, by label from NVS. */
static const esp_partition_t *find_fw_partition(void)
{
    fw_store_info_t info = {0};
    if (!read_fw_metadata(&info) || !info.partition[0]) return NULL;

    /* Find by label — works for both OTA and data partitions */
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, info.partition);
    const esp_partition_t *p = it ? esp_partition_get(it) : NULL;
    if (it) esp_partition_iterator_release(it);
    return p;
}

/** Get the best partition for storing scanner firmware. Prefers inactive OTA. */
static const esp_partition_t *get_store_partition(void)
{
#if defined(FOF_BADGE_VARIANT)
    const esp_partition_t *dedicated = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "fw_scanner_s3");
    if (dedicated) {
        ESP_LOGI(TAG, "Store target: fw_scanner_s3 '%s' (%luKB)",
                 dedicated->label, (unsigned long)(dedicated->size / 1024));
        return dedicated;
    }
#endif

    /* Prefer inactive OTA partition — esp_ota_begin works natively with it */
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    if (p) {
        ESP_LOGI(TAG, "Store target: inactive OTA '%s' (%luKB)",
                 p->label, (unsigned long)(p->size / 1024));
        return p;
    }

    /* Fall back to dedicated fw_store partition */
    p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, NULL);
    if (p) {
        ESP_LOGI(TAG, "Store target: fw_store '%s' (%luKB)",
                 p->label, (unsigned long)(p->size / 1024));
    } else {
        ESP_LOGE(TAG, "No storage partition available");
    }
    return p;
}

/** Resume all paused tasks after upload/relay completes or fails. */
static void resume_all_tasks(void)
{
    uart_rx_resume_scanner(0);
#if CONFIG_DUAL_SCANNER
    uart_rx_resume_scanner(1);
#endif
    http_upload_resume();
}

/* ── POST /api/fw/upload — store firmware using OTA API ──────────────────── */

static esp_err_t fw_upload_handler(httpd_req_t *req)
{
    const esp_partition_t *p = get_store_partition();
    if (!p) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No store partition");
        return ESP_FAIL;
    }

    int total = req->content_len;
    if (total < 1024 || total > (int)p->size) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Invalid size: %d (max %lu)",
                 total, (unsigned long)p->size);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_FAIL;
    }

    if (!operation_try_begin()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Operation in progress");
        return ESP_FAIL;
    }

    /* Pause HTTP upload + UART RX tasks to free heap and CPU.
     * The scanners flood with huge BLE JSON that consumes resources
     * needed for the 1MB firmware upload. */
    http_upload_pause();
    uart_rx_pause_scanner(0);  /* BLE scanner */
#if CONFIG_DUAL_SCANNER
    uart_rx_pause_scanner(1);  /* WiFi scanner */
#endif
    vTaskDelay(pdMS_TO_TICKS(500));

    if (!invalidate_fw_metadata()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Manifest invalidation failed");
        resume_all_tasks();
        operation_end();
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Storing scanner firmware: %d bytes to '%s' heap=%lu (uploads+UART paused)",
             total, p->label, (unsigned long)esp_get_free_heap_size());

    /* Increase socket timeout for large uploads */
    {
        int fd = httpd_req_to_sockfd(req);
        struct timeval tv = { .tv_sec = 120, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    /* Use OTA API for flash writes — handles erase scheduling internally.
     * This is the key fix: raw esp_partition_erase_range blocked for 50-200ms
     * per 64KB block, causing TCP drops at ~47%. The OTA API defers erases
     * to small 4KB sectors and never blocks long enough to stall TCP. */
    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s on partition '%s'",
                 esp_err_to_name(err), p ? p->label : "?");
        char msg[96];
        snprintf(msg, sizeof(msg), "esp_ota_begin: %s on '%s'",
                 esp_err_to_name(err), p ? p->label : "?");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
        resume_all_tasks();
        operation_end();
        return ESP_FAIL;
    }

    fw_stage_buf_ensure();
    uint8_t *buf = s_fw_stage_buf;
    const size_t buf_cap = s_fw_stage_cap;
    int received = 0;
    uint32_t checksum = 0;
    int consecutive_timeouts = 0;

    while (received < total) {
        int to_read = total - received;
        if (to_read > (int)buf_cap) to_read = (int)buf_cap;

        int len = httpd_req_recv(req, (char *)buf, to_read);
        if (len <= 0) {
            if (len == HTTPD_SOCK_ERR_TIMEOUT) {
                consecutive_timeouts++;
                if (consecutive_timeouts > 3) {
                    ESP_LOGE(TAG, "Upload timeout at %d/%d", received, total);
                    esp_ota_abort(ota_handle);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Timeout");
                    resume_all_tasks();
                    operation_end();
                    return ESP_FAIL;
                }
                continue;
            }
            ESP_LOGE(TAG, "HTTP recv error at %d/%d", received, total);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            resume_all_tasks();
            operation_end();
            return ESP_FAIL;
        }

        consecutive_timeouts = 0;

        err = esp_ota_write(ota_handle, buf, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed at %d: %s", received, esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            resume_all_tasks();
            operation_end();
            return ESP_FAIL;
        }

        checksum = esp_rom_crc32_le(checksum, buf, len);
        received += len;

        if (received % (100 * 1024) < (int)buf_cap) {
            ESP_LOGI(TAG, "Upload: %d/%d (%.0f%%) heap=%lu",
                     received, total, (float)received / total * 100,
                     (unsigned long)esp_get_free_heap_size());
        }
    }

    /* Abort OTA handle — data persists on flash, but partition is NOT
     * marked as bootable. This is safe: we never call esp_ota_end()
     * or esp_ota_set_boot_partition(). */
    esp_ota_abort(ota_handle);

    /* Store metadata in NVS (survives power cycle, partition swaps) */
    char version[32] = {0};
    char fw_name[32] = {0};
    char query[96] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "version", version, sizeof(version));
    httpd_query_key_value(query, "name", fw_name, sizeof(fw_name));

    if (!fw_store_persist_metadata(fw_name, version, p,
                                   (uint32_t)total, checksum)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Firmware identity or integrity validation failed");
        resume_all_tasks();
        operation_end();
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Firmware stored: %d bytes, CRC32=%08lX, partition=%s, name=%s version=%s",
             total, (unsigned long)checksum, p->label,
             fw_name[0] ? fw_name : "?",
             version[0] ? version : "?");

    resume_all_tasks();
    operation_end();

    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"size\":%d,\"checksum\":%lu,\"partition\":\"%s\","
             "\"name\":\"%s\",\"version\":\"%s\"}",
             total, (unsigned long)checksum, p->label,
             fw_name[0] ? fw_name : "",
             version[0] ? version : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ── Line-based UART read helpers (v0.59 staged handshakes) ──────────────── */

/* Read bytes from UART until '\n', a printable-ASCII line, or timeout.
 * Returns >=0 = line length, -1 = timeout. Skips non-printable bytes so
 * leftover binary OTA-frame fragments don't corrupt the line buffer. */
/* Read bytes from UART until '\n' or timeout.
 *
 * Scanner detection JSON lines can exceed our line buffer (full enrichment
 * blobs run 200–600 chars: local name + raw mfr hex + auth tag + JA3 + etc.).
 * When the buffer fills mid-line, we MUST keep reading to consume the rest
 * of the line through the terminating '\n' — otherwise we'd just exit, then
 * the next call starts mid-stream + sees the trailing bytes as a new "line"
 * and we'd never resync to real JSON line boundaries.
 *
 * Behavior:
 *   - Collect printable ASCII into line[] up to size-1 chars.
 *   - If a line overflows, go into "drain" mode until the next '\n', then
 *     continue waiting for a NEW line from the start.
 *   - Return the first complete line that fits. Return -1 on deadline hit.
 */
static int relay_read_line(uart_port_t uart_num, char *line, size_t size, int timeout_ms)
{
    int pos = 0;
    bool draining = false;
    int lines_overflowed = 0;
    int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t deadline_ms = start_ms + timeout_ms;
    while ((esp_timer_get_time() / 1000) < deadline_ms) {
        uint8_t b;
        int n = uart_read_bytes(uart_num, &b, 1, pdMS_TO_TICKS(10));
        if (n <= 0) continue;
        if (b == '\n') {
            if (draining) {
                /* Reached end of the too-long line; start fresh on the next. */
                draining = false;
                pos = 0;
                continue;
            }
            line[pos] = '\0';
            if (pos == 0) continue;   /* empty line — keep reading */
            return pos;
        }
        if (draining) continue;
        if (b >= 0x20 && b <= 0x7E) {
            if (pos < (int)size - 1) {
                line[pos++] = (char)b;
            } else {
                /* Buffer full before '\n' — discard this line, keep waiting. */
                draining = true;
                lines_overflowed++;
            }
        } else {
            pos = 0;  /* binary garbage — reset */
        }
    }
    if (lines_overflowed > 0) {
        ESP_LOGW(TAG, "relay_read_line: timeout with %d oversized lines dropped",
                 lines_overflowed);
    }
    line[pos] = '\0';
    return -1;
}

static void relay_send_wire_abort_sentinel(uart_port_t uart_num)
{
    uint8_t sentinel[OTA_ABORT_SENTINEL_COUNT + 1];
    memset(sentinel, OTA_ABORT_SENTINEL_BYTE, OTA_ABORT_SENTINEL_COUNT);
    sentinel[OTA_ABORT_SENTINEL_COUNT] = '\n';
    int written = uart_write_bytes(
        uart_num, (const char *)sentinel, sizeof(sentinel));
    if (written != (int)sizeof(sentinel)) {
        ESP_LOGE(TAG, "OTA wire-abort sentinel short write: %d/%u",
                 written, (unsigned)sizeof(sentinel));
    }
    uart_wait_tx_done(uart_num, pdMS_TO_TICKS(500));
}

static bool relay_line_session_matches(const char *line,
                                       const char *session_id)
{
    if (!session_id || !session_id[0]) {
        return true;
    }
    if (!line) return false;
    cJSON *root = cJSON_Parse(line);
    if (!root) return false;
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(
        root, JSON_KEY_OTA_SESSION_ID);
    bool matched = cJSON_IsString(session) &&
                   strcmp(session->valuestring, session_id) == 0;
    cJSON_Delete(root);
    return matched;
}

typedef struct {
    cJSON *root;
    fof_firmware_receipt_view_t view;
} relay_parsed_receipt_t;

static const char *relay_json_string(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool relay_json_u32(const cJSON *root,
                           const char *key,
                           uint32_t *value_out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 ||
        item->valuedouble > (double)UINT32_MAX) {
        return false;
    }
    uint32_t value = (uint32_t)item->valuedouble;
    if ((double)value != item->valuedouble) {
        return false;
    }
    if (value_out) {
        *value_out = value;
    }
    return true;
}

static bool relay_parse_receipt(const char *line,
                                relay_parsed_receipt_t *parsed)
{
    if (!line || !parsed) {
        return false;
    }
    memset(parsed, 0, sizeof(*parsed));
    parsed->root = cJSON_Parse(line);
    if (!parsed->root) {
        return false;
    }

    fof_firmware_receipt_view_t *view = &parsed->view;
    view->type = relay_json_string(parsed->root, JSON_KEY_TYPE);
    view->session_id = relay_json_string(
        parsed->root, JSON_KEY_OTA_SESSION_ID);
    view->target_version = relay_json_string(
        parsed->root, JSON_KEY_FW_TARGET_VERSION);
    view->firmware_name = relay_json_string(
        parsed->root, JSON_KEY_FW_NAME);
    view->project = relay_json_string(parsed->root, JSON_KEY_FW_PROJECT);
    view->hardware = relay_json_string(parsed->root, JSON_KEY_FW_HARDWARE);
    view->sha256 = relay_json_string(parsed->root, JSON_KEY_FW_SHA256);
    view->has_generation = relay_json_u32(
        parsed->root, JSON_KEY_FW_GENERATION, &view->generation);
    view->has_size = relay_json_u32(
        parsed->root, JSON_KEY_FW_SIZE, &view->size);
    view->has_crc32 = relay_json_u32(
        parsed->root, JSON_KEY_FW_CRC32, &view->crc32);
    const cJSON *allow_same = cJSON_GetObjectItemCaseSensitive(
        parsed->root, JSON_KEY_FW_ALLOW_SAME);
    view->has_allow_same_version = cJSON_IsBool(allow_same);
    view->allow_same_version = cJSON_IsTrue(allow_same);
    view->has_received = relay_json_u32(
        parsed->root, "received", &view->received);
    view->has_total = relay_json_u32(
        parsed->root, "total", &view->total);
    view->has_percent = relay_json_u32(
        parsed->root, "percent", &view->percent);
    return true;
}

static void relay_parsed_receipt_free(relay_parsed_receipt_t *parsed)
{
    if (parsed && parsed->root) {
        cJSON_Delete(parsed->root);
        parsed->root = NULL;
    }
}

static bool relay_line_matches_legacy_receipt(
    const char *line,
    const char *expected_type,
    const char *session_id,
    bool require_received,
    uint32_t expected_received)
{
    relay_parsed_receipt_t parsed = {0};
    if (!expected_type || !relay_parse_receipt(line, &parsed)) {
        return false;
    }
    bool matched = false;
    if (!require_received && strcmp(expected_type, MSG_TYPE_OTA_ACK) == 0) {
        matched = fof_firmware_legacy_ack_matches(
            &parsed.view, session_id);
    } else if (require_received &&
               strcmp(expected_type, MSG_TYPE_OTA_DONE) == 0) {
        matched = fof_firmware_legacy_done_matches(
            &parsed.view, session_id, expected_received);
    }
    relay_parsed_receipt_free(&parsed);
    return matched;
}

static bool relay_line_matches_manifest_ack(const char *line,
                                            const fw_store_info_t *info,
                                            const char *session_id,
                                            bool allow_same_version,
                                            const char *expected_type,
                                            uint32_t expected_received)
{
    relay_parsed_receipt_t parsed = {0};
    if (!info || !relay_parse_receipt(line, &parsed)) {
        return false;
    }
    fof_firmware_strict_receipt_expectation_t expected = {
        .type = expected_type,
        .session_id = session_id,
        .target_version = info->version,
        .firmware_name = info->name,
        .project = info->project,
        .hardware = info->hardware,
        .sha256 = info->sha256,
        .generation = info->generation,
        .size = info->size,
        .crc32 = info->checksum,
        .allow_same_version = allow_same_version,
        .received = expected_received,
    };
    bool matched = fof_firmware_strict_receipt_matches(
        &parsed.view, &expected);
    relay_parsed_receipt_free(&parsed);
    return matched;
}

static bool relay_line_matches_stop_ack(const char *line)
{
    relay_parsed_receipt_t parsed = {0};
    if (!relay_parse_receipt(line, &parsed)) {
        return false;
    }
    bool matched = fof_firmware_stop_ack_matches(&parsed.view);
    relay_parsed_receipt_free(&parsed);
    return matched;
}

static bool relay_line_matches_complete_progress(
    const char *line,
    const char *session_id,
    const fw_store_info_t *info)
{
    relay_parsed_receipt_t parsed = {0};
    if (!info || !relay_parse_receipt(line, &parsed)) {
        return false;
    }
    bool matched = fof_firmware_legacy_progress_matches(
        &parsed.view, session_id, info->size);
    relay_parsed_receipt_free(&parsed);
    return matched;
}

/* Wait for a JSON line whose payload contains `needle` (usually the "type"
 * value). Returns 0 on success, -1 on timeout, -2 if "ota_error" seen (fills
 * reason_out with the "reason":"X" string if present). Other line types are
 * silently consumed and ignored. */
static int relay_wait_for(uart_port_t uart_num, const char *needle,
                         int timeout_ms, char *reason_out, size_t reason_size)
{
    char line[512];
    int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t deadline_ms = start_ms + timeout_ms;
    int lines_seen = 0;
    while (true) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        int remaining_ms = (int)(deadline_ms - now_ms);
        if (remaining_ms <= 0) {
            ESP_LOGW(TAG, "relay_wait_for(%s) TIMEOUT after %lldms — %d lines seen",
                     needle, (long long)(now_ms - start_ms), lines_seen);
            return -1;
        }
        int n = relay_read_line(uart_num, line, sizeof(line), remaining_ms);
        if (n < 0) {
            int64_t after_ms = esp_timer_get_time() / 1000;
            ESP_LOGW(TAG, "relay_wait_for(%s) read_line returned -1 after %lldms (remaining was %d) — %d lines",
                     needle, (long long)(after_ms - start_ms), remaining_ms, lines_seen);
            return -1;
        }
        lines_seen++;
        ESP_LOGI(TAG, "relay_wait_for(%s) line[%d]: %.80s", needle, lines_seen, line);
        if (strstr(line, needle)) return 0;
        if (strstr(line, "ota_error")) {
            if (reason_out && reason_size) {
                const char *r = strstr(line, "\"reason\":\"");
                if (r) {
                    r += strlen("\"reason\":\"");
                    const char *e = strchr(r, '"');
                    size_t rlen = e ? (size_t)(e - r) : strlen(r);
                    if (rlen >= reason_size) rlen = reason_size - 1;
                    memcpy(reason_out, r, rlen);
                    reason_out[rlen] = 0;
                } else {
                    reason_out[0] = 0;
                }
            }
            return -2;
        }
        /* Other line (e.g. a stray detection JSON from pre-stop backlog,
         * or an ota_progress) — ignore and keep reading. */
    }
}

static int relay_wait_for_with_resend(int scanner_id,
                                      uart_port_t uart_num,
                                      const char *cmd,
                                      const char *needle,
                                      const char *session_id,
                                      const fw_store_info_t *expected_manifest,
                                      bool expected_allow_same_version,
                                      uint32_t expected_received,
                                      int timeout_ms,
                                      int resend_ms,
                                      char *reason_out,
                                      size_t reason_size)
{
    char line[512];
    int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t deadline_ms = start_ms + timeout_ms;
    int64_t next_send_ms = start_ms;
    int lines_seen = 0;

    ESP_LOGW(TAG, "relay_wait_for_with_resend(%s): timeout=%dms resend=%dms",
             needle, timeout_ms, resend_ms);

    while (true) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms >= deadline_ms) {
            ESP_LOGW(TAG, "relay_wait_for_with_resend(%s) TIMEOUT after %lldms — %d lines seen",
                     needle, (long long)(now_ms - start_ms), lines_seen);
            return -1;
        }

        if (now_ms >= next_send_ms) {
            uart_rx_send_command_to_scanner(scanner_id, cmd);
            next_send_ms = now_ms + resend_ms;
        }

        int remaining_ms = (int)(deadline_ms - now_ms);
        int until_resend_ms = (int)(next_send_ms - now_ms);
        int read_timeout_ms = remaining_ms;
        if (until_resend_ms > 0 && until_resend_ms < read_timeout_ms) {
            read_timeout_ms = until_resend_ms;
        }
        if (read_timeout_ms > 50) {
            read_timeout_ms = 50;
        }
        if (read_timeout_ms <= 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        int n = relay_read_line(uart_num, line, sizeof(line), read_timeout_ms);
        if (n < 0) {
            continue;
        }
        lines_seen++;
        ESP_LOGI(TAG, "relay_wait_for_with_resend(%s) line[%d]: %.80s",
                 needle, lines_seen, line);
        if (strstr(line, needle)) {
            if (session_id && session_id[0] &&
                !relay_line_session_matches(line, session_id)) {
                ESP_LOGW(TAG,
                         "relay_wait_for_with_resend(%s) ignored stale session line: %.80s",
                         needle, line);
                continue;
            }
            bool receipt_matches = false;
            if (expected_manifest != NULL) {
                receipt_matches = relay_line_matches_manifest_ack(
                    line, expected_manifest, session_id,
                    expected_allow_same_version, needle,
                    expected_received);
            } else if (session_id && session_id[0]) {
                receipt_matches = relay_line_matches_legacy_receipt(
                    line, needle, session_id, false, 0);
            } else {
                /* stop_ack has no session or manifest contract. */
                receipt_matches = relay_line_matches_stop_ack(line);
            }
            if (!receipt_matches) {
                ESP_LOGW(TAG,
                         "relay_wait_for_with_resend(%s) ignored manifest-mismatched ack",
                         needle);
                continue;
            }
            return 0;
        }
        if (strstr(line, "ota_error")) {
            if (session_id && session_id[0] &&
                !relay_line_session_matches(line, session_id)) {
                ESP_LOGW(TAG,
                         "relay_wait_for_with_resend(%s) ignored stale ota_error",
                         needle);
                continue;
            }
            if (reason_out && reason_size) {
                const char *r = strstr(line, "\"reason\":\"");
                if (r) {
                    r += strlen("\"reason\":\"");
                    const char *e = strchr(r, '"');
                    size_t rlen = e ? (size_t)(e - r) : strlen(r);
                    if (rlen >= reason_size) rlen = reason_size - 1;
                    memcpy(reason_out, r, rlen);
                    reason_out[rlen] = 0;
                } else {
                    reason_out[0] = 0;
                }
            }
            return -2;
        }
    }
}

/* Extract an integer "seq" field from a NACK line. Returns -1 if not parseable. */
static int relay_extract_seq(const char *line)
{
    if (!line) return -1;
    cJSON *root = cJSON_Parse(line);
    if (!root) return -1;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, JSON_KEY_TYPE);
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    int result = -1;
    if (cJSON_IsString(type) &&
        strcmp(type->valuestring, MSG_TYPE_OTA_NACK) == 0 &&
        cJSON_IsNumber(seq) && seq->valuedouble >= 0.0 &&
        seq->valuedouble <= (double)UINT16_MAX) {
        uint16_t value = (uint16_t)seq->valuedouble;
        if ((double)value == seq->valuedouble) {
            result = (int)value;
        }
    }
    cJSON_Delete(root);
    return result;
}

static int64_t relay_timeout_ms_for_size(uint32_t size_bytes)
{
#ifdef FOF_BADGE_VARIANT
    return (int64_t)OTA_RELAY_TIMEOUT_FOR_SIZE_MS(size_bytes);
#else
    (void)size_bytes;
    return 15 * 60 * 1000;
#endif
}

#ifdef FOF_BADGE_VARIANT
static void relay_emit_progress(int scanner_id,
                                const char *stage,
                                uint32_t bytes,
                                uint32_t size,
                                int chunks,
                                int nacks,
                                int retries,
                                int64_t start_ms,
                                const char *error)
{
    int percent = size ? (int)(((uint64_t)bytes * 100ULL) / size) : 0;
    int64_t elapsed_s = ((esp_timer_get_time() / 1000) - start_ms) / 1000;
    printf("FOF_FW_RELAY_PROGRESS:{\"uart\":\"%s\",\"stage\":\"%s\","
           "\"bytes\":%lu,\"size\":%lu,\"percent\":%d,"
           "\"chunks\":%d,\"nacks\":%d,\"retries\":%d,"
           "\"elapsed_s\":%lld,\"error\":\"%s\"}\n",
           scanner_id == 1 ? "wifi" : "ble",
           stage ? stage : "relay",
           (unsigned long)bytes,
           (unsigned long)size,
           percent,
           chunks,
           nacks,
           retries,
           (long long)elapsed_s,
           error ? error : "");
    fflush(stdout);
}
#else
static void relay_emit_progress(int scanner_id,
                                const char *stage,
                                uint32_t bytes,
                                uint32_t size,
                                int chunks,
                                int nacks,
                                int retries,
                                int64_t start_ms,
                                const char *error)
{
    (void)scanner_id;
    (void)stage;
    (void)bytes;
    (void)size;
    (void)chunks;
    (void)nacks;
    (void)retries;
    (void)start_ms;
    (void)error;
}
#endif

/* A scanner can emit more than one NACK for the same expected sequence while
 * the uplink is still draining UART. Coalesce that short burst before the
 * retransmit so queued duplicates cannot cause a second, stale rewind after
 * the replacement chunk has already made forward progress. */
static int relay_coalesce_nack_burst(uart_port_t uart_num,
                                     const char *session_id,
                                     int first_seq)
{
    int last_seq = first_seq;
    int64_t quiet_deadline_ms =
        (esp_timer_get_time() / 1000) + FW_RELAY_NACK_SETTLE_MS;
    char line[512];

    while ((esp_timer_get_time() / 1000) < quiet_deadline_ms) {
        int remaining_ms = (int)(quiet_deadline_ms -
            (esp_timer_get_time() / 1000));
        if (remaining_ms <= 0) break;
        int n = relay_read_line(uart_num, line, sizeof(line), remaining_ms);
        if (n < 0) break;
        if (!strstr(line, "ota_nack") ||
            !relay_line_session_matches(line, session_id)) {
            continue;
        }
        int seq = relay_extract_seq(line);
        if (seq >= 0) {
            last_seq = seq;
            quiet_deadline_ms =
                (esp_timer_get_time() / 1000) + FW_RELAY_NACK_SETTLE_MS;
        }
    }
    return last_seq;
}

/* Non-blocking peek for a NACK line. Returns seq number on NACK hit, -1 if
 * nothing relevant read within timeout_ms. Silently consumes other lines. */
static int relay_poll_nack(uart_port_t uart_num,
                           const char *session_id,
                           int timeout_ms)
{
    char line[512];
    size_t buffered = 0;
    if (uart_get_buffered_data_len(uart_num, &buffered) != ESP_OK ||
        buffered == 0) {
        return -1;
    }
    int64_t deadline_ms = (esp_timer_get_time() / 1000) + timeout_ms;
    while ((esp_timer_get_time() / 1000) < deadline_ms) {
        int remaining = (int)(deadline_ms - (esp_timer_get_time() / 1000));
        if (remaining <= 0) break;
        int n = relay_read_line(uart_num, line, sizeof(line), remaining);
        if (n < 0) break;
        if (strstr(line, "ota_nack")) {
            if (!relay_line_session_matches(line, session_id)) {
                ESP_LOGW(TAG, "Ignoring stale OTA NACK: %.80s", line);
                continue;
            }
            int seq = relay_extract_seq(line);
            if (seq >= 0) {
                return relay_coalesce_nack_burst(
                    uart_num, session_id, seq);
            }
        }
        /* consume other lines and keep polling */
    }
    return -1;
}

/* The final chunk is special: do not let the lightweight NACK poll consume
 * the scanner's ota_staged receipt. Wait for either an exact staged manifest,
 * a session-bound NACK that requires rewind, or an explicit scanner error. */
static int relay_wait_for_staged_or_nack(
    uart_port_t uart_num,
    const char *session_id,
    const fw_store_info_t *info,
    bool legacy_mode,
    bool allow_same_version,
    int timeout_ms,
    int *nack_seq_out,
    char *reason_out,
    size_t reason_size)
{
    if (nack_seq_out) *nack_seq_out = -1;
    if (reason_out && reason_size) reason_out[0] = '\0';
    char line[512];
    int64_t deadline_ms = (esp_timer_get_time() / 1000) + timeout_ms;
    int last_nack_seq = -1;
    int64_t nack_settle_deadline_ms = 0;
    bool final_frame_accepted = false;
    while ((esp_timer_get_time() / 1000) < deadline_ms) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (!final_frame_accepted && last_nack_seq >= 0 &&
            now_ms >= nack_settle_deadline_ms) {
            if (nack_seq_out) *nack_seq_out = last_nack_seq;
            return 1;
        }
        int remaining_ms = (int)(deadline_ms - now_ms);
        if (!final_frame_accepted && last_nack_seq >= 0) {
            int settle_remaining = (int)(nack_settle_deadline_ms - now_ms);
            if (settle_remaining < remaining_ms) {
                remaining_ms = settle_remaining;
            }
        }
        if (remaining_ms <= 0) break;
        int n = relay_read_line(uart_num, line, sizeof(line), remaining_ms);
        if (n < 0) {
            if (!final_frame_accepted && last_nack_seq >= 0) {
                if (nack_seq_out) *nack_seq_out = last_nack_seq;
                return 1;
            }
            break;
        }
        if (strstr(line, "ota_progress") &&
            relay_line_matches_complete_progress(line, session_id, info)) {
            /* Full-image ota_progress supersedes queued NACKs: the scanner
             * accepted the final frame and is now validating the image. */
            final_frame_accepted = true;
            last_nack_seq = -1;
            nack_settle_deadline_ms = 0;
            ESP_LOGW(TAG,
                     "Final frame accepted; waiting for exact ota_staged receipt");
            if (legacy_mode) {
                /* The exact .68 bridge auto-finalizes from the last binary
                 * chunk and has no ota_staged manifest receipt. */
                return 0;
            }
            continue;
        }
        if (strstr(line, "ota_staged")) {
            if (!relay_line_session_matches(line, session_id) ||
                !relay_line_matches_manifest_ack(
                    line, info, session_id, allow_same_version,
                    MSG_TYPE_OTA_STAGED, info->size)) {
                ESP_LOGW(TAG,
                         "Ignored stale or manifest-mismatched ota_staged");
                continue;
            }
            /* An exact ota_staged supersedes queued NACKs from an earlier
             * chunk: the scanner has verified the complete immutable image. */
            return 0;
        }
        if (strstr(line, "ota_nack")) {
            if (!relay_line_session_matches(line, session_id)) {
                ESP_LOGW(TAG, "Ignored stale OTA NACK while awaiting staged receipt");
                continue;
            }
            if (final_frame_accepted) {
                ESP_LOGW(TAG,
                         "Ignored queued OTA NACK after full-image progress");
                continue;
            }
            int seq = relay_extract_seq(line);
            if (seq >= 0) {
                last_nack_seq = seq;
                nack_settle_deadline_ms =
                    (esp_timer_get_time() / 1000) + FW_RELAY_NACK_SETTLE_MS;
            }
            continue;
        }
        if (strstr(line, "ota_error")) {
            if (session_id && session_id[0] &&
                !relay_line_session_matches(line, session_id)) {
                ESP_LOGW(TAG, "Ignored stale OTA error while awaiting staged receipt");
                continue;
            }
            if (reason_out && reason_size) {
                const char *reason = strstr(line, "\"reason\":\"");
                if (reason) {
                    reason += strlen("\"reason\":\"");
                    const char *end = strchr(reason, '"');
                    size_t len = end ? (size_t)(end - reason) : strlen(reason);
                    if (len >= reason_size) len = reason_size - 1;
                    memcpy(reason_out, reason, len);
                    reason_out[len] = '\0';
                }
            }
            return -2;
        }
    }
    if (!final_frame_accepted && last_nack_seq >= 0) {
        if (nack_seq_out) *nack_seq_out = last_nack_seq;
        return 1;
    }
    return -1;
}

/* ── Firmware offer + relay core ───────────────────────────────────────── */

typedef struct {
    bool ok;
    bool legacy;
    uint32_t size;
    uint32_t bytes;
    int chunks;
    int nacks;
    int retries;
    int64_t elapsed_s;
    char stage[16];
    char error[64];
    uint32_t cmd_rx_before;
    uint32_t cmd_rx_after;
    uint32_t fw_check_before;
    uint32_t fw_check_after;
    int64_t cmd_age_after_s;
    char scanner_version[32];
    char scanner_fw_state[16];
} fw_relay_result_t;

typedef struct {
    bool received;
    uint32_t cmd_rx;
    uint32_t fw_check;
    int64_t cmd_age_s;
    char version[32];
    char board[40];
    char fw_state[16];
} fw_command_health_t;

static void remember_relay_result(int scanner_id, const fw_relay_result_t *result)
{
    if (!result || scanner_id < 0 || scanner_id >= 2) {
        return;
    }
    s_last_relay[scanner_id].ok = result->ok;
    s_last_relay[scanner_id].size = result->size;
    s_last_relay[scanner_id].bytes = result->bytes;
    strncpy(s_last_relay[scanner_id].stage, result->stage,
            sizeof(s_last_relay[scanner_id].stage) - 1);
    s_last_relay[scanner_id].stage[sizeof(s_last_relay[scanner_id].stage) - 1] = '\0';
    strncpy(s_last_relay[scanner_id].error, result->error,
            sizeof(s_last_relay[scanner_id].error) - 1);
    s_last_relay[scanner_id].error[sizeof(s_last_relay[scanner_id].error) - 1] = '\0';
    s_last_relay[scanner_id].chunks = result->chunks;
    s_last_relay[scanner_id].nacks = result->nacks;
    s_last_relay[scanner_id].retries = result->retries;
    s_last_relay[scanner_id].elapsed_s = result->elapsed_s;
    s_last_relay[scanner_id].finished_ms = esp_timer_get_time() / 1000;
    s_last_relay[scanner_id].cmd_rx_before = result->cmd_rx_before;
    s_last_relay[scanner_id].cmd_rx_after = result->cmd_rx_after;
    s_last_relay[scanner_id].fw_check_before = result->fw_check_before;
    s_last_relay[scanner_id].fw_check_after = result->fw_check_after;
    s_last_relay[scanner_id].cmd_age_after_s = result->cmd_age_after_s;
    strncpy(s_last_relay[scanner_id].scanner_version, result->scanner_version,
            sizeof(s_last_relay[scanner_id].scanner_version) - 1);
    s_last_relay[scanner_id].scanner_version[sizeof(s_last_relay[scanner_id].scanner_version) - 1] = '\0';
    strncpy(s_last_relay[scanner_id].scanner_fw_state, result->scanner_fw_state,
            sizeof(s_last_relay[scanner_id].scanner_fw_state) - 1);
    s_last_relay[scanner_id].scanner_fw_state[sizeof(s_last_relay[scanner_id].scanner_fw_state) - 1] = '\0';
}

static void capture_command_health(int scanner_id, fw_command_health_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->cmd_age_s = -1;
    const scanner_info_t *info = (scanner_id == 0)
        ? uart_rx_get_ble_scanner_info()
        : uart_rx_get_wifi_scanner_info();
    if (!info) {
        return;
    }
    out->received = info->received;
    out->cmd_rx = info->cmd_rx_count;
    out->fw_check = info->fw_check_count;
    out->cmd_age_s = info->cmd_last_age_s;
    strncpy(out->version, info->version, sizeof(out->version) - 1);
    strncpy(out->board, info->board, sizeof(out->board) - 1);
    strncpy(out->fw_state, info->fw_update_state, sizeof(out->fw_state) - 1);
}

static bool command_health_moved(const fw_command_health_t *before,
                                 const fw_command_health_t *after)
{
    if (!after || !after->received) {
        return false;
    }
    if (before && after->cmd_rx > before->cmd_rx) {
        return true;
    }
    if (before && after->fw_check > before->fw_check) {
        return true;
    }
    return after->cmd_age_s >= 0 && after->cmd_age_s <= 3;
}

static bool probe_scanner_command_ingress(int scanner_id,
                                          fw_command_health_t *before,
                                          fw_command_health_t *after)
{
    capture_command_health(scanner_id, before);
    const scanner_info_t *info = (scanner_id == 0)
        ? uart_rx_get_ble_scanner_info()
        : uart_rx_get_wifi_scanner_info();
    const char *profile = (info && info->scan_profile[0])
        ? info->scan_profile
        : "hybrid_failover";
    char probe_cmd[96];
    snprintf(probe_cmd, sizeof(probe_cmd),
             "{\"type\":\"scan_profile\",\"%s\":\"%s\"}",
             JSON_KEY_SCAN_PROFILE, profile);
    (void)uart_rx_send_command_to_scanner_checked(scanner_id, probe_cmd);

    int64_t deadline_ms = (esp_timer_get_time() / 1000) + 2500;
    do {
        vTaskDelay(pdMS_TO_TICKS(250));
        capture_command_health(scanner_id, after);
        if (command_health_moved(before, after)) {
            return true;
        }
    } while ((esp_timer_get_time() / 1000) < deadline_ms);

    capture_command_health(scanner_id, after);
    return command_health_moved(before, after);
}

static bool scanner_post_update_converged(const scanner_info_t *scanner,
                                          const fw_store_info_t *staged,
                                          const char *expected_hardware_id,
                                          uint32_t identity_generation_before,
                                          bool quiet_expected,
                                          const char *expected_profile)
{
    if (!scanner || !staged || !scanner->received ||
        scanner->identity_generation <= identity_generation_before ||
        !expected_hardware_id || !expected_hardware_id[0] ||
        strcmp(scanner->hardware_id, expected_hardware_id) != 0 ||
        strcmp(scanner->version, staged->version) != 0 ||
        strcmp(scanner->firmware_name, staged->name) != 0 ||
        strcmp(scanner->board, staged->name) != 0 ||
        strcmp(scanner->app_project, staged->project) != 0 ||
        strcmp(scanner->hardware_type, staged->hardware) != 0 ||
        scanner->rollback_pending ||
        strcmp(scanner->recovery_mode, "normal") != 0 ||
        scanner->cmd_rx_count == 0 ||
        scanner->cmd_last_age_s < 0 || scanner->cmd_last_age_s > 45 ||
        !expected_profile ||
        strcmp(scanner->scan_profile, expected_profile) != 0) {
        return false;
    }

#ifdef FOF_BADGE_VARIANT
    if (quiet_expected) {
        return scanner->quiet_mode && scanner->quiet_uart_commands &&
               !scanner->quiet_tx_enabled && scanner->wifi_paused &&
               !scanner->ble_scanning;
    }
    if (scanner->quiet_mode || !scanner->quiet_tx_enabled) {
        return false;
    }
    if (strcmp(expected_profile, "ble_primary") == 0) {
        return scanner->ble_scanning && scanner->ble_host_active &&
               scanner->ble_host_synced && scanner->wifi_paused;
    }
    if (strcmp(expected_profile, "wifi_primary") == 0) {
        return !scanner->ble_scanning && !scanner->wifi_paused;
    }
#else
    (void)quiet_expected;
#endif
    return scanner->ble_scanning &&
           scanner->ble_host_active && scanner->ble_host_synced &&
           !scanner->wifi_paused;
}

static bool wait_for_scanner_post_update_health(
    int scanner_id,
    const fw_store_info_t *staged,
    const char *expected_hardware_id,
    uint32_t identity_generation_before,
    char *error,
    size_t error_len)
{
    int64_t deadline_ms = (esp_timer_get_time() / 1000) + 180000;
    int64_t next_probe_ms = 0;
    bool saw_new_identity = false;
    while ((esp_timer_get_time() / 1000) < deadline_ms) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        const scanner_info_t *live = scanner_id == 0
            ? uart_rx_get_ble_scanner_info()
            : uart_rx_get_wifi_scanner_info();
        scanner_info_t snapshot = {0};
        if (live) snapshot = *live;
        bool connected = scanner_id == 0
            ? uart_rx_is_ble_scanner_connected()
            : uart_rx_is_wifi_scanner_connected();
        bool peer_connected = scanner_id == 0
            ? uart_rx_is_wifi_scanner_connected()
            : uart_rx_is_ble_scanner_connected();
#ifdef FOF_BADGE_VARIANT
        badge_power_state_t power_state = {0};
        badge_power_runtime_snapshot(&power_state);
        bool quiet_expected = power_state.quiet;
        uint32_t power_generation = power_state.generation;
        const char *expected_profile = peer_connected
            ? fof_policy_scan_profile_for_slot((uint8_t)scanner_id, false)
            : "hybrid_failover";
#else
        bool quiet_expected = false;
        uint32_t power_generation = 0;
        const char *expected_profile = "hybrid_failover";
#endif
        saw_new_identity = saw_new_identity ||
            snapshot.identity_generation > identity_generation_before;

        if (connected && scanner_post_update_converged(
                &snapshot, staged, expected_hardware_id,
                identity_generation_before, quiet_expected,
                expected_profile)) {
            ESP_LOGW(TAG,
                     "Scanner[%d] post-update convergence proved: MAC=%s "
                     "target=%s project=%s hardware=%s version=%s "
                     "rollback_pending=0 recovery=normal quiet=%d cmd_rx=%lu",
                     scanner_id,
                     snapshot.hardware_id,
                     snapshot.firmware_name,
                     snapshot.app_project,
                     snapshot.hardware_type,
                     snapshot.version,
                     snapshot.quiet_mode ? 1 : 0,
                     (unsigned long)snapshot.cmd_rx_count);
            return true;
        }

        if (saw_new_identity && now_ms >= next_probe_ms) {
#ifdef FOF_BADGE_VARIANT
            char power_cmd[112];
            snprintf(power_cmd, sizeof(power_cmd),
                     "{\"type\":\"%s\",\"enabled\":%s,"
                     "\"generation\":%lu}",
                     MSG_TYPE_SCANNER_QUIET,
                     quiet_expected ? "true" : "false",
                     (unsigned long)power_generation);
            uart_rx_send_command_to_scanner(scanner_id, power_cmd);
#endif
            char profile_cmd[96];
            snprintf(profile_cmd, sizeof(profile_cmd),
                     "{\"type\":\"scan_profile\",\"%s\":\"%s\"}",
                     JSON_KEY_SCAN_PROFILE, expected_profile);
            uart_rx_send_command_to_scanner(scanner_id, profile_cmd);
#ifdef FOF_BADGE_VARIANT
            if (!quiet_expected)
#endif
            {
                uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"start\"}");
            }
            next_probe_ms = now_ms + 5000;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    const scanner_info_t *last = scanner_id == 0
        ? uart_rx_get_ble_scanner_info()
        : uart_rx_get_wifi_scanner_info();
    if (error && error_len) {
        snprintf(error, error_len, "%s",
                 !saw_new_identity ? "post_reboot_identity_timeout" :
                 (last && last->rollback_pending) ? "rollback_health_timeout" :
                 "post_reboot_health_timeout");
    }
    return false;
}

#ifdef FOF_BADGE_VARIANT
static bool badge_candidate_seen(const int *pins, int count, int pin)
{
    for (int i = 0; i < count; i++) {
        if (pins[i] == pin) {
            return true;
        }
    }
    return false;
}

static int badge_default_tx_pin_for_scanner(int scanner_id)
{
    return scanner_id == 1 ? CONFIG_WIFI_SCANNER_TX_PIN : CONFIG_BLE_SCANNER_TX_PIN;
}

static int badge_peer_tx_pin_for_scanner(int scanner_id)
{
    return scanner_id == 1 ? CONFIG_BLE_SCANNER_TX_PIN : CONFIG_WIFI_SCANNER_TX_PIN;
}

static bool badge_try_heal_command_tx_pin(int scanner_id,
                                          fw_command_health_t *before,
                                          fw_command_health_t *after,
                                          int *healed_pin)
{
    int candidates[8];
    int count = 0;
    const int raw[] = {
        badge_default_tx_pin_for_scanner(scanner_id),
        badge_peer_tx_pin_for_scanner(scanner_id),
        scanner_id == 1 ? 15 : 17,
        scanner_id == 1 ? 17 : 15,
        scanner_id == 1 ? 16 : 18,
        scanner_id == 1 ? 18 : 16,
    };
    const int rx_pin = scanner_id == 1 ? CONFIG_WIFI_SCANNER_RX_PIN : CONFIG_BLE_SCANNER_RX_PIN;
    const int peer_rx_pin = scanner_id == 1 ? CONFIG_BLE_SCANNER_RX_PIN : CONFIG_WIFI_SCANNER_RX_PIN;

    for (int i = 0; i < (int)(sizeof(raw) / sizeof(raw[0])); i++) {
        if (raw[i] >= 0 &&
            raw[i] != rx_pin &&
            raw[i] != peer_rx_pin &&
            !badge_candidate_seen(candidates, count, raw[i])) {
            candidates[count++] = raw[i];
        }
    }

    ESP_LOGW(TAG, "Badge scanner[%d] command ingress unhealthy; probing %d TX pins",
             scanner_id, count);
    for (int i = 0; i < count; i++) {
        int pin = candidates[i];
        fw_command_health_t pin_before = {0};
        fw_command_health_t pin_after = {0};
        if (!uart_rx_set_scanner_tx_pin_for_badge_probe(scanner_id, pin)) {
            continue;
        }
        if (probe_scanner_command_ingress(scanner_id, &pin_before, &pin_after)) {
            if (before) *before = pin_before;
            if (after) *after = pin_after;
            if (healed_pin) *healed_pin = pin;
            ESP_LOGW(TAG,
                     "Badge scanner[%d] command TX pin healed: GPIO%d "
                     "(cmd_rx %lu->%lu fw_check %lu->%lu)",
                     scanner_id,
                     pin,
                     (unsigned long)pin_before.cmd_rx,
                     (unsigned long)pin_after.cmd_rx,
                     (unsigned long)pin_before.fw_check,
                     (unsigned long)pin_after.fw_check);
            return true;
        }
    }

    (void)uart_rx_set_scanner_tx_pin_for_badge_probe(
        scanner_id,
        badge_default_tx_pin_for_scanner(scanner_id)
    );
    ESP_LOGE(TAG, "Badge scanner[%d] command TX pin probe failed; restored GPIO%d",
             scanner_id, badge_default_tx_pin_for_scanner(scanner_id));
    return false;
}
#endif

static bool staged_firmware_matches_scanner(const fw_store_info_t *info,
                                            const char *scanner_board)
{
    if (!info || !info->stored) return false;
    if (!info->name[0] || !scanner_board || !scanner_board[0]) {
        return false;
    }
    return strcmp(info->name, scanner_board) == 0;
}

static bool staged_firmware_is_newer_for_scanner(const fw_store_info_t *info,
                                                 const char *scanner_board,
                                                 const char *scanner_version)
{
    if (!staged_firmware_matches_scanner(info, scanner_board)) return false;
    if (!info->version[0] || !scanner_version || !scanner_version[0]) return false;
    return fof_firmware_version_is_strictly_newer(info->version,
                                                  scanner_version);
}

static fof_firmware_version_relation_t staged_firmware_version_relation(
    const fw_store_info_t *info,
    const char *scanner_board,
    const char *scanner_version)
{
    if (!staged_firmware_matches_scanner(info, scanner_board) ||
        !info->version[0] || !scanner_version || !scanner_version[0]) {
        return FOF_VERSION_INVALID;
    }
    return fof_firmware_version_compare(info->version, scanner_version);
}

static const char *firmware_relation_refusal_reason(
    fof_firmware_version_relation_t relation)
{
    switch (relation) {
        case FOF_VERSION_EQUAL:
            return "same_version_refused";
        case FOF_VERSION_OLDER:
            return "downgrade_refused";
        case FOF_VERSION_UNORDERED:
            return "unordered_version_refused";
        case FOF_VERSION_INVALID:
        default:
            return "invalid_version_refused";
    }
}

static void send_fw_offer(int scanner_id, bool update, const fw_store_info_t *info,
                          const char *reason)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "{\"type\":\"%s\",\"update\":%s,\"target_ver\":\"%s\","
             "\"fw_name\":\"%s\",\"app_project\":\"%s\","
             "\"hardware_type\":\"%s\",\"sha256\":\"%s\","
             "\"generation\":%lu,\"size\":%lu,\"crc\":%lu,"
             "\"reason\":\"%s\"}",
             MSG_TYPE_FW_OFFER,
             update ? "true" : "false",
             (info && info->version[0]) ? info->version : "",
             (info && info->name[0]) ? info->name : "",
             (info && info->project[0]) ? info->project : "",
             (info && info->hardware[0]) ? info->hardware : "",
             (info && info->sha256[0]) ? info->sha256 : "",
             (unsigned long)((info) ? info->generation : 0),
             (unsigned long)((info) ? info->size : 0),
             (unsigned long)((info) ? info->checksum : 0),
             reason ? reason : "");
    uart_rx_send_command_to_scanner(scanner_id, cmd);
}

static bool fw_relay_stored_to_scanner(int scanner_id,
                                       uint32_t expected_generation,
                                       const char *expected_hardware_id,
                                       bool scanner_already_quiet,
                                       bool legacy_mode,
                                       bool force_probe_skip,
                                       bool allow_same_version,
                                       fw_relay_result_t *result)
{
    fw_relay_result_t local = {0};
    if (!result) result = &local;
    memset(result, 0, sizeof(*result));
    snprintf(result->stage, sizeof(result->stage), "init");

    fw_store_info_t info = {0};
    if (!fw_store_get_info(&info)) {
        snprintf(result->error, sizeof(result->error), "no_firmware_stored");
        remember_relay_result(scanner_id, result);
        return false;
    }
    if (expected_generation != 0 &&
        info.generation != expected_generation) {
        snprintf(result->error, sizeof(result->error), "generation_changed");
        remember_relay_result(scanner_id, result);
        return false;
    }
    if (expected_hardware_id && expected_hardware_id[0] &&
        !auto_hardware_id_is_canonical(expected_hardware_id)) {
        snprintf(result->error, sizeof(result->error),
                 "bound_hardware_id_invalid");
        remember_relay_result(scanner_id, result);
        return false;
    }
    result->legacy = legacy_mode;
    result->size = info.size;

    const esp_partition_t *p = find_fw_partition();
    if (!p) {
        snprintf(result->error, sizeof(result->error), "partition_not_found");
        remember_relay_result(scanner_id, result);
        return false;
    }

    if (!operation_try_begin()) {
        snprintf(result->error, sizeof(result->error), "operation_active");
        remember_relay_result(scanner_id, result);
        return false;
    }

    fw_store_info_t verified_info = {0};
    char staged_validation_error[48] = {0};
    if (!validate_staged_image(p, info.size, info.checksum,
                               info.name, info.version, info.sha256,
                               &verified_info,
                               staged_validation_error,
                               sizeof(staged_validation_error))) {
        snprintf(result->stage, sizeof(result->stage), "manifest");
        snprintf(result->error, sizeof(result->error), "staged_integrity_failed");
        ESP_LOGE(TAG, "Relay refused: staged image revalidation failed: %s",
                 staged_validation_error[0]
                     ? staged_validation_error
                     : "unknown");
        clear_fw_metadata();
        operation_end();
        remember_relay_result(scanner_id, result);
        return false;
    }

    uart_port_t uart_num = scanner_id == 1 ? CONFIG_WIFI_SCANNER_UART : CONFIG_BLE_SCANNER_UART;
    const char *uart_target = scanner_id == 1 ? "wifi" : "ble";

    ESP_LOGW(TAG, "Relay v2: %lu bytes from '%s' to UART%d (uart=%s quiet=%s legacy=%s) heap=%lu",
             (unsigned long)info.size, p->label, uart_num, uart_target,
             scanner_already_quiet ? "true" : "false",
             legacy_mode ? "true" : "false",
             (unsigned long)esp_get_free_heap_size());

    fw_command_health_t cmd_before = {0};
    fw_command_health_t cmd_after = {0};
    bool cmd_ingress_ok = false;
    if (force_probe_skip) {
        capture_command_health(scanner_id, &cmd_before);
        cmd_after = cmd_before;
        cmd_ingress_ok = true;
        ESP_LOGW(TAG, "Relay force mode: skipping command-health probe for scanner[%d]",
                 scanner_id);
    } else {
        cmd_ingress_ok = probe_scanner_command_ingress(
            scanner_id, &cmd_before, &cmd_after);
#ifdef FOF_BADGE_VARIANT
        if (!cmd_ingress_ok) {
            int healed_pin = -1;
            cmd_ingress_ok = badge_try_heal_command_tx_pin(
                scanner_id, &cmd_before, &cmd_after, &healed_pin);
            if (cmd_ingress_ok) {
                ESP_LOGW(TAG, "Relay using badge scanner[%d] healed TX GPIO%d",
                         scanner_id, healed_pin);
            }
        }
#endif
    }
    strncpy(result->scanner_version, cmd_after.version[0] ? cmd_after.version : cmd_before.version,
            sizeof(result->scanner_version) - 1);
    strncpy(result->scanner_fw_state, cmd_after.fw_state[0] ? cmd_after.fw_state : cmd_before.fw_state,
            sizeof(result->scanner_fw_state) - 1);
    result->cmd_rx_before = cmd_before.cmd_rx;
    result->cmd_rx_after = cmd_after.cmd_rx;
    result->fw_check_before = cmd_before.fw_check;
    result->fw_check_after = cmd_after.fw_check;
    result->cmd_age_after_s = cmd_after.cmd_age_s;

    if (!cmd_ingress_ok) {
        snprintf(result->stage, sizeof(result->stage), "probe");
        snprintf(result->error, sizeof(result->error), "command_ingress_unhealthy");
        ESP_LOGE(TAG,
                 "Relay refused: scanner[%d] command ingress unhealthy "
                 "(cmd_rx %lu->%lu fw_check %lu->%lu cmd_age=%lld ver=%s fw=%s)",
                 scanner_id,
                 (unsigned long)cmd_before.cmd_rx,
                 (unsigned long)cmd_after.cmd_rx,
                 (unsigned long)cmd_before.fw_check,
                 (unsigned long)cmd_after.fw_check,
                 (long long)cmd_after.cmd_age_s,
                 result->scanner_version[0] ? result->scanner_version : "?",
                 result->scanner_fw_state[0] ? result->scanner_fw_state : "?");
        operation_end();
        remember_relay_result(scanner_id, result);
        return false;
    }

    scanner_identity_snapshot_t pre_update_identity = {0};
    bool have_pre_update_identity = uart_rx_get_scanner_identity_snapshot(
        scanner_id, &pre_update_identity);
    uint32_t pre_update_identity_generation = have_pre_update_identity
        ? pre_update_identity.identity_generation : 0;
    bool automatic_bound_relay = expected_generation != 0 &&
        expected_hardware_id && expected_hardware_id[0];
    if (legacy_mode) {
        fof_legacy_relay_authorization_view_t authorization = {
            .automatic_bound = automatic_bound_relay,
            .bound_hardware_id = expected_hardware_id,
            .identity = {
                .received = have_pre_update_identity &&
                    pre_update_identity.complete,
                .version = pre_update_identity.version,
                .board = pre_update_identity.board,
                .firmware_name = pre_update_identity.firmware_name,
                .project = pre_update_identity.app_project,
                .hardware = pre_update_identity.hardware_type,
                .hardware_id = pre_update_identity.hardware_id,
            },
            .manifest = {
                .target = info.name,
                .version = info.version,
                .project = info.project,
                .hardware = info.hardware,
                .sha256 = info.sha256,
                .size = info.size,
                .crc32 = info.checksum,
            },
        };
        if (!fof_firmware_legacy_relay_authorized(&authorization)) {
            snprintf(result->stage, sizeof(result->stage), "identity");
            snprintf(result->error, sizeof(result->error),
                     "legacy_not_authorized");
            ESP_LOGE(TAG,
                     "Legacy relay refused: scanner[%d] is not the exact "
                     "automatic-bound .68 badge identity",
                     scanner_id);
            operation_end();
            remember_relay_result(scanner_id, result);
            return false;
        }
    }
    if (automatic_bound_relay &&
        (!have_pre_update_identity || !pre_update_identity.complete ||
         strcmp(pre_update_identity.hardware_id,
                expected_hardware_id) != 0 ||
         (legacy_mode &&
          strcmp(pre_update_identity.version,
                 FOF_LEGACY_READY_BOOTSTRAP_VERSION) != 0) ||
         !auto_identity_matches_manifest(
             &pre_update_identity, &info, pre_update_identity.board,
             pre_update_identity.version) ||
         !staged_firmware_is_newer_for_scanner(
             &info, pre_update_identity.board,
             pre_update_identity.version))) {
        snprintf(result->stage, sizeof(result->stage), "identity");
        snprintf(result->error, sizeof(result->error),
                 "bound_identity_changed");
        ESP_LOGE(TAG,
                 "Relay refused: scanner[%d] live identity no longer matches "
                 "bound MAC %s and staged contract",
                 scanner_id, expected_hardware_id);
        operation_end();
        remember_relay_result(scanner_id, result);
        return false;
    }
    char pre_update_hardware_id[18] = {0};
    if (expected_hardware_id && expected_hardware_id[0]) {
        snprintf(pre_update_hardware_id, sizeof(pre_update_hardware_id),
                 "%s", expected_hardware_id);
    } else if (have_pre_update_identity) {
        snprintf(pre_update_hardware_id, sizeof(pre_update_hardware_id),
                 "%s", pre_update_identity.hardware_id);
    }
    if (!pre_update_hardware_id[0] ||
        strcmp(pre_update_hardware_id, "unknown") == 0) {
        snprintf(result->stage, sizeof(result->stage), "identity");
        snprintf(result->error, sizeof(result->error), "hardware_id_unknown");
        ESP_LOGE(TAG, "Relay refused: scanner[%d] immutable hardware id unknown",
                 scanner_id);
        operation_end();
        remember_relay_result(scanner_id, result);
        return false;
    }

    const char *scanner_board = cmd_after.board[0] ? cmd_after.board : cmd_before.board;
    const char *scanner_version = cmd_after.version[0] ? cmd_after.version : cmd_before.version;
    if (!scanner_board || !scanner_board[0]) {
        snprintf(result->stage, sizeof(result->stage), "version");
        snprintf(result->error, sizeof(result->error), "scanner_board_unknown");
        ESP_LOGE(TAG, "Relay refused: scanner[%d] board identity is unknown",
                 scanner_id);
        operation_end();
        remember_relay_result(scanner_id, result);
        return false;
    }
    if (!staged_firmware_matches_scanner(&info, scanner_board)) {
        snprintf(result->stage, sizeof(result->stage), "version");
        snprintf(result->error, sizeof(result->error), "board_mismatch");
        ESP_LOGE(TAG, "Relay refused: staged board=%s scanner board=%s",
                 info.name[0] ? info.name : "?",
                 scanner_board);
        operation_end();
        remember_relay_result(scanner_id, result);
        return false;
    }
    if (!scanner_version || !scanner_version[0]) {
        snprintf(result->stage, sizeof(result->stage), "version");
        snprintf(result->error, sizeof(result->error), "scanner_version_unknown");
        ESP_LOGE(TAG, "Relay refused: scanner[%d] version is unknown",
                 scanner_id);
        operation_end();
        remember_relay_result(scanner_id, result);
        return false;
    }

    fof_firmware_version_relation_t version_relation =
        staged_firmware_version_relation(&info, scanner_board, scanner_version);
    bool version_allowed = version_relation == FOF_VERSION_NEWER ||
        (version_relation == FOF_VERSION_EQUAL && allow_same_version);
    if (!version_allowed) {
        const char *reason = firmware_relation_refusal_reason(version_relation);
        snprintf(result->stage, sizeof(result->stage), "version");
        snprintf(result->error, sizeof(result->error), "%s", reason);
        ESP_LOGW(TAG,
                 "Relay refused: staged=%s scanner=%s board=%s relation=%d "
                 "allow_same=%d reason=%s",
                 info.version,
                 scanner_version,
                 scanner_board,
                 (int)version_relation,
                 allow_same_version ? 1 : 0,
                 reason);
        operation_end();
        remember_relay_result(scanner_id, result);
        return false;
    }

    http_upload_pause();
    vTaskDelay(pdMS_TO_TICKS(500));
    uart_rx_pause_scanner(scanner_id);

    bool relay_ok = true;
    char error_msg[64] = {0};
    char stage[16] = "init";
    int64_t start_ms = esp_timer_get_time() / 1000;
    uint16_t seq = 0;
    int nack_count = 0;
    int total_retries = 0;
    bool transfer_committed = false;
    bool staged_receipt = false;
    char relay_session_id[16] = {0};
    int64_t overall_timeout_ms = relay_timeout_ms_for_size(info.size);

    relay_emit_progress(scanner_id, "stop", 0, info.size, 0, 0, 0,
                        start_ms, "");

    snprintf(stage, sizeof(stage), "stop");
    {
        if (!scanner_already_quiet) {
            int r = relay_wait_for_with_resend(
                scanner_id,
                uart_num,
                "{\"type\":\"stop\"}",
                "stop_ack",
                NULL,
                NULL,
                false,
                0,
                FW_RELAY_STOP_STORM_MS,
                FW_RELAY_STOP_STORM_STEP_MS,
                NULL,
                0
            );
            if (r == 0) {
                ESP_LOGI(TAG, "Stage stop: stop_ack received");
            } else {
                if (force_probe_skip) {
                    ESP_LOGW(TAG,
                             "Stage stop: no stop_ack in recovery force mode; "
                             "flushing scanner UART and trying ota_begin");
                } else {
                    snprintf(error_msg, sizeof(error_msg), "command_ingress_unhealthy");
                    relay_ok = false;
                    goto relay_done;
                }
            }
        } else {
            ESP_LOGW(TAG, "Stage stop: scanner pre-quiet via fw_ready");
        }
        uart_flush_input(uart_num);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    snprintf(stage, sizeof(stage), "begin");
    relay_emit_progress(scanner_id, "begin", 0, info.size, 0, 0, 0,
                        start_ms, "");
    {
        uint32_t fw_crc32 = info.checksum;
        char session_id[16];
        snprintf(session_id, sizeof(session_id), "r%08lx",
                 (unsigned long)esp_random());
        strncpy(relay_session_id, session_id, sizeof(relay_session_id) - 1);
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "{\"type\":\"ota_begin\",\"size\":%lu,\"crc\":%lu,"
                 "\"sha256\":\"%s\",\"target_ver\":\"%s\","
                 "\"fw_name\":\"%s\",\"app_project\":\"%s\","
                 "\"hardware_type\":\"%s\",\"generation\":%lu,"
                 "\"allow_same_version\":%s,"
                 "\"session_id\":\"%s\"}",
                 (unsigned long)info.size, (unsigned long)fw_crc32,
                 info.sha256, info.version, info.name, info.project,
                 info.hardware, (unsigned long)info.generation,
                 allow_same_version ? "true" : "false", session_id);
        uart_rx_send_command_to_scanner(scanner_id, cmd);
        ESP_LOGI(TAG,
                 "Stage begin: sent ota_begin (%lu bytes, session=%s "
                 "CRC=%08lX SHA256=%s target=%s version=%s gen=%lu)",
                 (unsigned long)info.size, session_id,
                 (unsigned long)fw_crc32, info.sha256, info.name,
                 info.version, (unsigned long)info.generation);

        char reason[48] = {0};
        int r = relay_wait_for_with_resend(
            scanner_id,
            uart_num,
            cmd,
            "ota_ack",
            session_id,
            legacy_mode ? NULL : &info,
            allow_same_version,
            0,
            15000,
            500,
            reason,
            sizeof(reason)
        );
        if (r != 0) {
            snprintf(error_msg, sizeof(error_msg),
                     r == -2 ? "scanner_error:%s" : "ota_ack_timeout",
                     r == -2 ? reason : "");
            relay_ok = false;
            goto relay_done;
        }
        ESP_LOGI(TAG, "Stage begin: ota_ack received, entering chunk stream");
    }

    snprintf(stage, sizeof(stage), "chunks");
    relay_emit_progress(scanner_id, "chunks", 0, info.size, 0, nack_count,
                        total_retries, start_ms, "");
    {
        static uint8_t read_buf[OTA_CHUNK_MAX_DATA];
        static uint8_t frame[OTA_CHUNK_HEADER_SIZE + OTA_CHUNK_MAX_DATA + OTA_CHUNK_CRC_SIZE];
        uint32_t offset = 0;
        uint32_t remaining = info.size;
        uint16_t last_nack_seq = UINT16_MAX;
        uint8_t consecutive_nack_retries = 0;
        const uint32_t relay_chunk_data =
            FW_RELAY_CHUNK_DATA < OTA_CHUNK_MAX_DATA ? FW_RELAY_CHUNK_DATA : OTA_CHUNK_MAX_DATA;
        uint32_t next_progress_bytes = OTA_RELAY_PROGRESS_INTERVAL_BYTES;
        int64_t next_progress_ms = (esp_timer_get_time() / 1000) + 5000;

        while (remaining > 0) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if ((now_ms - start_ms) > overall_timeout_ms) {
                snprintf(error_msg, sizeof(error_msg), "overall_timeout");
                relay_ok = false;
                goto relay_done;
            }

            uint32_t chunk_len = remaining > relay_chunk_data ? relay_chunk_data : remaining;
            esp_err_t err = esp_partition_read(p, offset, read_buf, chunk_len);
            if (err != ESP_OK) {
                snprintf(error_msg, sizeof(error_msg), "flash_read_%lu", (unsigned long)offset);
                relay_ok = false;
                goto relay_done;
            }

            uint32_t crc = esp_rom_crc32_le(0, read_buf, chunk_len);
            frame[0] = OTA_CHUNK_MAGIC;
            frame[1] = (uint8_t)(seq >> 8);
            frame[2] = (uint8_t)(seq & 0xFF);
            frame[3] = (uint8_t)(chunk_len >> 8);
            frame[4] = (uint8_t)(chunk_len & 0xFF);
            memcpy(frame + OTA_CHUNK_HEADER_SIZE, read_buf, chunk_len);
            int crc_off = OTA_CHUNK_HEADER_SIZE + chunk_len;
            frame[crc_off + 0] = (uint8_t)(crc >> 24);
            frame[crc_off + 1] = (uint8_t)(crc >> 16);
            frame[crc_off + 2] = (uint8_t)(crc >> 8);
            frame[crc_off + 3] = (uint8_t)(crc);
            int frame_len = OTA_CHUNK_HEADER_SIZE + chunk_len + OTA_CHUNK_CRC_SIZE;

            int frame_written = uart_write_bytes(
                uart_num, (const char *)frame, frame_len);
            if (frame_written != frame_len) {
                snprintf(error_msg, sizeof(error_msg),
                         "uart_short_write_%d_%d", frame_written, frame_len);
                relay_ok = false;
                goto relay_done;
            }
            esp_err_t tx_done = uart_wait_tx_done(
                uart_num, pdMS_TO_TICKS(1000));
            if (tx_done != ESP_OK) {
                snprintf(error_msg, sizeof(error_msg),
                         "uart_tx_wait_%s", esp_err_to_name(tx_done));
                relay_ok = false;
                goto relay_done;
            }

            bool final_chunk = chunk_len == remaining;
            int nack_seq = -1;
            if (final_chunk) {
                char staged_reason[48] = {0};
                int staged_result = relay_wait_for_staged_or_nack(
                    uart_num,
                    relay_session_id,
                    &info,
                    legacy_mode,
                    allow_same_version,
                    15000,
                    &nack_seq,
                    staged_reason,
                    sizeof(staged_reason));
                if (staged_result == 0) {
                    staged_receipt = true;
                    ESP_LOGW(TAG,
                             "Final chunk verified: exact ota_staged receipt received");
                } else if (staged_result < 0) {
                    if (staged_result == -2) {
                        snprintf(error_msg, sizeof(error_msg),
                                 "scanner_error:%s",
                                 staged_reason[0] ? staged_reason : "unknown");
                    } else {
                        snprintf(error_msg, sizeof(error_msg),
                                 "ota_staged_timeout");
                    }
                    relay_ok = false;
                    goto relay_done;
                }
            } else {
                nack_seq = relay_poll_nack(uart_num, relay_session_id,
                                           FW_RELAY_NACK_POLL_MS);
            }
            if (nack_seq >= 0) {
                if (nack_seq > (int)seq) {
                    ESP_LOGW(TAG, "NACK seq=%d > current seq=%d, ignoring", nack_seq, seq);
                } else {
                    uint32_t nack_offset = (uint32_t)nack_seq * relay_chunk_data;
                    uint32_t rewind_bytes = offset > nack_offset ? (offset - nack_offset) : 0;
                    if ((uint16_t)nack_seq == last_nack_seq) {
                        consecutive_nack_retries++;
                    } else {
                        last_nack_seq = (uint16_t)nack_seq;
                        consecutive_nack_retries = 1;
                    }
                    if (consecutive_nack_retries > 3) {
                        snprintf(error_msg, sizeof(error_msg),
                                 "chunk_%d_crc_retries_exhausted", nack_seq);
                        relay_ok = false;
                        goto relay_done;
                    }
                    nack_count++;
                    total_retries++;
                    offset -= rewind_bytes;
                    remaining = info.size - offset;
                    seq = (uint16_t)nack_seq;
                    result->bytes = offset;
                    ESP_LOGW(TAG, "NACK seq=%d -> rewind %lu bytes (retry %u)",
                             nack_seq, (unsigned long)rewind_bytes,
                             consecutive_nack_retries);
                    continue;
                }
            }

            offset += chunk_len;
            remaining -= chunk_len;
            seq++;
            result->bytes = offset;

            if ((info.size - remaining) % (100 * 1024) < relay_chunk_data) {
                ESP_LOGI(TAG, "Relay: %lu/%lu (%.0f%%) seq=%d nacks=%d heap=%lu",
                         (unsigned long)(info.size - remaining), (unsigned long)info.size,
                         (float)(info.size - remaining) / info.size * 100,
                         seq, nack_count,
                         (unsigned long)esp_get_free_heap_size());
            }
            now_ms = esp_timer_get_time() / 1000;
            if (offset >= next_progress_bytes || now_ms >= next_progress_ms ||
                remaining == 0) {
                relay_emit_progress(scanner_id, "chunks", offset, info.size,
                                    seq, nack_count, total_retries,
                                    start_ms, "");
                while (next_progress_bytes <= offset) {
                    next_progress_bytes += OTA_RELAY_PROGRESS_INTERVAL_BYTES;
                }
                next_progress_ms = now_ms + 5000;
            }
        }
    }

    if (!staged_receipt) {
        snprintf(error_msg, sizeof(error_msg), "ota_staged_missing");
        relay_ok = false;
        goto relay_done;
    }

    snprintf(stage, sizeof(stage), "end");
    relay_emit_progress(scanner_id, "end", info.size, info.size, seq,
                        nack_count, total_retries, start_ms, "");
    {
        char end_cmd[512];
        snprintf(end_cmd, sizeof(end_cmd),
                 "{\"type\":\"ota_end\",\"session_id\":\"%s\","
                 "\"size\":%lu,\"crc\":%lu,\"sha256\":\"%s\","
                 "\"target_ver\":\"%s\",\"fw_name\":\"%s\","
                 "\"app_project\":\"%s\",\"hardware_type\":\"%s\","
                 "\"generation\":%lu,\"allow_same_version\":%s}",
                 relay_session_id,
                 (unsigned long)info.size,
                 (unsigned long)info.checksum,
                 info.sha256,
                 info.version,
                 info.name,
                 info.project,
                 info.hardware,
                 (unsigned long)info.generation,
                 allow_same_version ? "true" : "false");
        uart_rx_send_command_to_scanner(scanner_id, end_cmd);

        char line[512];
        int64_t deadline = (esp_timer_get_time() / 1000) + 60000;
        bool saw_exact_done = false;
        bool health_proved_without_done = false;
        int result_code = -1;
        char reason[48] = {0};
        while ((esp_timer_get_time() / 1000) < deadline) {
            int remaining_ms = (int)(deadline - (esp_timer_get_time() / 1000));
            if (remaining_ms <= 0) break;
            int n = relay_read_line(uart_num, line, sizeof(line), remaining_ms);
            if (n < 0) break;
            if (strstr(line, "ota_done")) {
                bool exact_done = false;
                if (legacy_mode) {
                    exact_done = relay_line_matches_legacy_receipt(
                        line, MSG_TYPE_OTA_DONE, relay_session_id, true,
                        info.size);
                } else {
                    exact_done =
                        relay_line_session_matches(line, relay_session_id) &&
                        relay_line_matches_manifest_ack(
                            line, &info, relay_session_id,
                            allow_same_version,
                            MSG_TYPE_OTA_DONE, info.size);
                }
                if (!exact_done) {
                    ESP_LOGW(TAG, "Ignored stale or manifest-mismatched ota_done");
                    continue;
                }
                result_code = 0;
                saw_exact_done = true;
                transfer_committed = true;
                break;
            }
            if (strstr(line, "ota_error")) {
                if (!relay_line_session_matches(line, relay_session_id)) {
                    ESP_LOGW(TAG, "Ignored stale ota_error during finalize");
                    continue;
                }
                const char *r = strstr(line, "\"reason\":\"");
                if (r) {
                    r += strlen("\"reason\":\"");
                    const char *e = strchr(r, '"');
                    size_t rlen = e ? (size_t)(e - r) : strlen(r);
                    if (rlen >= sizeof(reason)) rlen = sizeof(reason) - 1;
                    memcpy(reason, r, rlen); reason[rlen] = 0;
                }
                result_code = -2; break;
            }
        }

        if (result_code == -2) {
            snprintf(error_msg, sizeof(error_msg), "scanner_error:%s",
                     reason[0] ? reason : "unknown");
            relay_ok = false;
            goto relay_done;
        } else if (!saw_exact_done) {
            if (allow_same_version) {
                /* In recovery rewrite mode the pre- and post-update version
                 * are intentionally identical, so identity/health cannot
                 * prove that the requested bytes were actually committed. */
                snprintf(error_msg, sizeof(error_msg),
                         "exact_ota_done_required_for_same_version");
                relay_ok = false;
                goto relay_done;
            }
            /* The scanner sets its boot partition before emitting ota_done.
             * A power cut or final UART-byte loss in that narrow window can
             * leave a healthy new image with no receipt. Never guess from
             * silence: accept only the stronger proof of a rebooted, exact
             * identity plus rollback/radio/command convergence. */
            uart_rx_resume_scanner(scanner_id);
            snprintf(stage, sizeof(stage), "health");
            relay_emit_progress(scanner_id, "health", info.size, info.size,
                                seq, nack_count, total_retries, start_ms,
                                "ota_done_missing");
            if (!wait_for_scanner_post_update_health(
                    scanner_id, &info, pre_update_hardware_id,
                    pre_update_identity_generation,
                    error_msg, sizeof(error_msg))) {
                if (!error_msg[0]) {
                    snprintf(error_msg, sizeof(error_msg), "finalize_timeout");
                }
                relay_ok = false;
                goto relay_done;
            }
            health_proved_without_done = true;
            transfer_committed = true;
            error_msg[0] = '\0';
            ESP_LOGW(TAG,
                     "ota_done lost but full post-reboot convergence proved; "
                     "accepting scanner[%d] update",
                     scanner_id);
        }
        if (!health_proved_without_done) {
            ESP_LOGW(TAG,
                     "Stage end: exact session/manifest ota_done received; "
                     "waiting for reboot and rollback health proof");
            uart_rx_resume_scanner(scanner_id);
            snprintf(stage, sizeof(stage), "health");
            relay_emit_progress(scanner_id, "health", info.size, info.size, seq,
                                nack_count, total_retries, start_ms, "");
            if (!wait_for_scanner_post_update_health(
                    scanner_id, &info, pre_update_hardware_id,
                    pre_update_identity_generation,
                    error_msg, sizeof(error_msg))) {
                relay_ok = false;
                goto relay_done;
            }
        }
    }

relay_done:;
    if (!relay_ok) {
        relay_emit_progress(scanner_id, stage, result->bytes, info.size, seq,
                            nack_count, total_retries, start_ms, error_msg);
        if (!transfer_committed) {
            char abort_cmd[96];
            if (relay_session_id[0]) {
                snprintf(abort_cmd, sizeof(abort_cmd),
                         "{\"type\":\"ota_abort\",\"session_id\":\"%s\"}",
                         relay_session_id);
            } else {
                snprintf(abort_cmd, sizeof(abort_cmd), "{\"type\":\"ota_abort\"}");
            }
            uart_rx_send_command_to_scanner(scanner_id, abort_cmd);
            vTaskDelay(pdMS_TO_TICKS(50));
            /* JSON is parseable in AWAITING_FINALIZE. During binary STAGING
             * it is intentionally swallowed, so follow it with the reserved
             * inter-frame recovery sentinel before the worker retries. */
            relay_send_wire_abort_sentinel(uart_num);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
    uart_rx_resume_scanner(scanner_id);

    int64_t elapsed_s = ((esp_timer_get_time() / 1000) - start_ms) / 1000;
    if (!relay_ok) {
        uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"start\"}");
    }

    if (relay_ok) {
        relay_emit_progress(scanner_id, "done", info.size, info.size, seq,
                            nack_count, total_retries, start_ms, "");
        ESP_LOGW(TAG, "Relay complete: %lu bytes, %d chunks, %d nacks in %llds",
                 (unsigned long)info.size, seq, nack_count, (long long)elapsed_s);
    } else {
        ESP_LOGE(TAG, "Relay FAILED @ %s: %s (%d chunks in %llds)",
                 stage, error_msg, seq, (long long)elapsed_s);
    }

    http_upload_resume();
    operation_end();

    result->ok = relay_ok;
    result->legacy = legacy_mode;
    result->size = info.size;
    if (relay_ok) {
        result->bytes = info.size;
    }
    result->chunks = seq;
    result->nacks = nack_count;
    result->retries = total_retries;
    result->elapsed_s = elapsed_s;
    strncpy(result->stage, stage, sizeof(result->stage) - 1);
    result->stage[sizeof(result->stage) - 1] = '\0';
    strncpy(result->error, error_msg, sizeof(result->error) - 1);
    result->error[sizeof(result->error) - 1] = '\0';
    remember_relay_result(scanner_id, result);
    return relay_ok;
}

/* ── POST /api/fw/relay — staged handshake + fire-and-forget with NACK ──── */

static esp_err_t fw_relay_handler(httpd_req_t *req)
{
    char query[96] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char uart_target[8] = "ble";
    httpd_query_key_value(query, "uart", uart_target, sizeof(uart_target));
    char mode[16] = {0};
    char legacy_param[8] = {0};
    char force_param[8] = {0};
    char allow_same_param[8] = {0};
    httpd_query_key_value(query, "mode", mode, sizeof(mode));
    httpd_query_key_value(query, "legacy", legacy_param, sizeof(legacy_param));
    httpd_query_key_value(query, "force", force_param, sizeof(force_param));
    httpd_query_key_value(query, "allow_same_version",
                          allow_same_param, sizeof(allow_same_param));
    bool legacy_requested =
        strcmp(mode, "legacy") == 0 ||
        strcmp(legacy_param, "1") == 0 ||
        strcmp(legacy_param, "true") == 0;
    if (legacy_requested) {
        httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "legacy relay is automatic-only");
        return ESP_OK;
    }
    bool force_probe_skip =
        strcmp(force_param, "1") == 0 ||
        strcmp(force_param, "true") == 0;
    bool allow_same_version =
        strcmp(allow_same_param, "1") == 0 ||
        strcmp(allow_same_param, "true") == 0;
    int scanner_id = (strcmp(uart_target, "wifi") == 0) ? 1 : 0;

    fw_relay_result_t result = {0};
    fw_relay_stored_to_scanner(scanner_id, 0, NULL, false, false,
                               force_probe_skip,
                               allow_same_version, &result);

    char resp_buf[900];
    snprintf(resp_buf, sizeof(resp_buf),
             "{\"ok\":%s,\"legacy\":%s,\"size\":%lu,\"bytes\":%lu,"
             "\"chunks\":%d,\"nacks\":%d,"
             "\"retries\":%d,\"elapsed_s\":%lld,\"stage\":\"%s\","
             "\"error\":\"%s\",\"cmd_rx_before\":%lu,\"cmd_rx_after\":%lu,"
             "\"fw_check_before\":%lu,\"fw_check_after\":%lu,"
             "\"cmd_age_after_s\":%lld,\"scanner_ver\":\"%s\","
             "\"scanner_fw_state\":\"%s\"}",
             result.ok ? "true" : "false",
             result.legacy ? "true" : "false",
             (unsigned long)result.size,
             (unsigned long)result.bytes,
             result.chunks, result.nacks, result.retries,
             (long long)result.elapsed_s, result.stage, result.error,
             (unsigned long)result.cmd_rx_before,
             (unsigned long)result.cmd_rx_after,
             (unsigned long)result.fw_check_before,
             (unsigned long)result.fw_check_after,
             (long long)result.cmd_age_after_s,
             result.scanner_version,
             result.scanner_fw_state);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_buf);
    return ESP_OK;
}

bool fw_store_relay_staged_to_scanner(int scanner_id,
                                      char *out_json,
                                      size_t out_json_len)
{
    return fw_store_relay_staged_to_scanner_ex(scanner_id, false, false,
                                               out_json, out_json_len);
}

bool fw_store_relay_staged_to_scanner_ex(int scanner_id,
                                         bool force_probe_skip,
                                         bool allow_same_version,
                                         char *out_json,
                                         size_t out_json_len)
{
    if (scanner_id < 0 || scanner_id > 1) {
        if (out_json && out_json_len > 0) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"invalid_scanner\"}");
        }
        return false;
    }

    fw_relay_result_t result = {0};
    bool ok = fw_relay_stored_to_scanner(scanner_id, 0, NULL, false, false,
                                         force_probe_skip,
                                         allow_same_version,
                                         &result);
    if (out_json && out_json_len > 0) {
        snprintf(out_json, out_json_len,
                 "{\"ok\":%s,\"uart\":\"%s\",\"size\":%lu,\"bytes\":%lu,"
                 "\"chunks\":%d,"
                 "\"nacks\":%d,\"retries\":%d,\"elapsed_s\":%lld,"
                 "\"stage\":\"%s\",\"error\":\"%s\","
                 "\"cmd_rx_before\":%lu,\"cmd_rx_after\":%lu,"
                 "\"fw_check_before\":%lu,\"fw_check_after\":%lu,"
                 "\"cmd_age_after_s\":%lld,\"scanner_ver\":\"%s\","
                 "\"scanner_fw_state\":\"%s\"}",
                 ok ? "true" : "false",
                 scanner_id == 1 ? "wifi" : "ble",
                 (unsigned long)result.size,
                 (unsigned long)result.bytes,
                 result.chunks,
                 result.nacks,
                 result.retries,
                 (long long)result.elapsed_s,
                 result.stage,
                 result.error,
                 (unsigned long)result.cmd_rx_before,
                 (unsigned long)result.cmd_rx_after,
                 (unsigned long)result.fw_check_before,
                 (unsigned long)result.fw_check_after,
                 (long long)result.cmd_age_after_s,
                 result.scanner_version,
                 result.scanner_fw_state);
    }
    return ok;
}

#define FW_AUTO_COORDINATOR_MAGIC 0xF0F34C01u
#define FW_AUTO_COORDINATOR_SCHEMA 3u
#define FW_AUTO_RELAY_MAX_ATTEMPTS 3
#define FW_AUTO_READY_MAX_PROBES 3
#define FW_AUTO_READY_PROBE_DELAY_MS 10000
#define FW_AUTO_COORDINATOR_SAVE_RETRIES 3
#define FW_AUTO_OFFER_BINDING_TTL_MS 60000
#define FW_AUTO_IDENTITY_MAX_AGE_MS 60000
#define FW_AUTO_SECOND_IDENTITY_WAIT_MS 12000
#define FW_AUTO_IDENTITY_POLL_MS 100
#define FW_AUTO_RECOVERY_COOLDOWN_MS 35000
#define FW_AUTO_RECOVERY_PROBE_DELAY_MS 20000

typedef enum {
    FW_AUTO_SLOT_EXCLUDED = 0,
    FW_AUTO_SLOT_AWAITING_CHECK = 1,
    FW_AUTO_SLOT_OFFERED = 2,
    FW_AUTO_SLOT_READY_QUEUED = 3,
    FW_AUTO_SLOT_RELAYING = 4,
    FW_AUTO_SLOT_CONVERGED = 5,
    FW_AUTO_SLOT_CURRENT = 6,
    FW_AUTO_SLOT_REFUSED = 7,
    FW_AUTO_SLOT_FAILED = 8,
    FW_AUTO_SLOT_NEWER_SKIPPED = 9,
    FW_AUTO_SLOT_RECOVERING = 10,
} fw_auto_slot_state_t;

_Static_assert((int)FW_AUTO_SLOT_EXCLUDED ==
                   (int)FOF_FW_COORD_SLOT_EXCLUDED,
               "coordinator excluded state drifted");
_Static_assert((int)FW_AUTO_SLOT_AWAITING_CHECK ==
                   (int)FOF_FW_COORD_SLOT_AWAITING_CHECK,
               "coordinator awaiting state drifted");
_Static_assert((int)FW_AUTO_SLOT_OFFERED ==
                   (int)FOF_FW_COORD_SLOT_OFFERED,
               "coordinator offered state drifted");
_Static_assert((int)FW_AUTO_SLOT_READY_QUEUED ==
                   (int)FOF_FW_COORD_SLOT_READY_QUEUED,
               "coordinator ready state drifted");
_Static_assert((int)FW_AUTO_SLOT_RELAYING ==
                   (int)FOF_FW_COORD_SLOT_RELAYING,
               "coordinator relaying state drifted");
_Static_assert((int)FW_AUTO_SLOT_CONVERGED ==
                   (int)FOF_FW_COORD_SLOT_CONVERGED,
               "coordinator converged state drifted");
_Static_assert((int)FW_AUTO_SLOT_CURRENT ==
                   (int)FOF_FW_COORD_SLOT_CURRENT,
               "coordinator current state drifted");
_Static_assert((int)FW_AUTO_SLOT_REFUSED ==
                   (int)FOF_FW_COORD_SLOT_REFUSED,
               "coordinator refused state drifted");
_Static_assert((int)FW_AUTO_SLOT_FAILED ==
                   (int)FOF_FW_COORD_SLOT_FAILED,
               "coordinator failed state drifted");
_Static_assert((int)FW_AUTO_SLOT_NEWER_SKIPPED ==
                   (int)FOF_FW_COORD_SLOT_NEWER_SKIPPED,
               "coordinator newer state drifted");
_Static_assert((int)FW_AUTO_SLOT_RECOVERING ==
                   (int)FOF_FW_COORD_SLOT_RECOVERING,
               "coordinator recovering state drifted");

typedef fof_fw_coord_v3_t fw_auto_coordinator_blob_t;

_Static_assert(sizeof(fw_auto_coordinator_blob_t) <=
                   NVS_CONFIG_MAX_BLOB_SIZE,
               "automatic update coordinator exceeds NVS blob capacity");

typedef struct {
    uint32_t generation;
    uint32_t manifest_crc32;
    uint8_t slot;
    uint32_t identity_generation;
    char hardware_id[18];
    int64_t captured_ms;
} fw_auto_offer_binding_t;

static StaticSemaphore_t s_auto_coordinator_mutex_storage;
static SemaphoreHandle_t s_auto_coordinator_mutex;
static fw_auto_coordinator_blob_t s_auto_coordinator;
static bool s_auto_coordinator_loaded = false;
static bool s_auto_relay_worker_running = false;
/* These values are deliberately volatile. A reboot requires a new
 * identity/check/offer sequence before a scanner can be queued again. They
 * are read and written only while s_auto_coordinator_mutex is held. */
static uint32_t s_auto_identity_generation_floor[
    FW_AUTO_UPDATE_SCANNER_COUNT] = {0};
static fw_auto_offer_binding_t s_auto_offer_bindings[
    FW_AUTO_UPDATE_SCANNER_COUNT] = {0};
static fw_auto_offer_binding_t s_auto_ready_bindings[
    FW_AUTO_UPDATE_SCANNER_COUNT] = {0};
static bool s_auto_legacy_ready[FW_AUTO_UPDATE_SCANNER_COUNT] = {0};
static int64_t s_auto_recovery_not_before_ms[
    FW_AUTO_UPDATE_SCANNER_COUNT] = {0};
static int64_t s_auto_recovery_next_probe_ms[
    FW_AUTO_UPDATE_SCANNER_COUNT] = {0};

bool fw_store_init_auto_update_coordinator(void)
{
    if (s_auto_coordinator_mutex) {
        return true;
    }
    s_auto_coordinator_mutex =
        xSemaphoreCreateMutexStatic(&s_auto_coordinator_mutex_storage);
    if (!s_auto_coordinator_mutex) {
        ESP_LOGE(TAG, "Failed to initialize scanner-update coordinator mutex");
        return false;
    }
    return true;
}

static bool auto_coordinator_lock(void)
{
    SemaphoreHandle_t mutex = s_auto_coordinator_mutex;
    return mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(3000)) == pdTRUE;
}

static void auto_coordinator_unlock(void)
{
    xSemaphoreGive(s_auto_coordinator_mutex);
}

static void auto_capture_identity_floors(
    uint32_t floors[FW_AUTO_UPDATE_SCANNER_COUNT])
{
    if (!floors) {
        return;
    }
    memset(floors, 0,
           sizeof(uint32_t) * FW_AUTO_UPDATE_SCANNER_COUNT);
    for (int scanner_id = 0; scanner_id < FW_AUTO_UPDATE_SCANNER_COUNT;
         ++scanner_id) {
        scanner_identity_snapshot_t snapshot = {0};
        if (uart_rx_get_scanner_identity_snapshot(scanner_id, &snapshot)) {
            floors[scanner_id] = snapshot.identity_generation;
        }
    }
}

static void auto_set_identity_floors_locked(
    const uint32_t floors[FW_AUTO_UPDATE_SCANNER_COUNT])
{
    for (int scanner_id = 0; scanner_id < FW_AUTO_UPDATE_SCANNER_COUNT;
         ++scanner_id) {
        s_auto_identity_generation_floor[scanner_id] = floors
            ? floors[scanner_id] : 0;
        memset(&s_auto_offer_bindings[scanner_id], 0,
               sizeof(s_auto_offer_bindings[scanner_id]));
        memset(&s_auto_ready_bindings[scanner_id], 0,
               sizeof(s_auto_ready_bindings[scanner_id]));
        s_auto_legacy_ready[scanner_id] = false;
        s_auto_recovery_not_before_ms[scanner_id] = 0;
        s_auto_recovery_next_probe_ms[scanner_id] = 0;
    }
}

static bool auto_hardware_id_is_canonical(const char *hardware_id)
{
    if (!hardware_id || strlen(hardware_id) != 17U) {
        return false;
    }
    for (size_t i = 0; i < 17U; ++i) {
        if ((i + 1U) % 3U == 0U) {
            if (hardware_id[i] != ':') {
                return false;
            }
            continue;
        }
        char ch = hardware_id[i];
        if (!((ch >= '0' && ch <= '9') ||
              (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool auto_identity_matches_manifest(
    const scanner_identity_snapshot_t *identity,
    const fw_store_info_t *info,
    const char *scanner_board,
    const char *scanner_version)
{
    return identity && identity->complete && info && info->stored &&
        scanner_board && scanner_version &&
        auto_hardware_id_is_canonical(identity->hardware_id) &&
        strcmp(scanner_board, info->name) == 0 &&
        strcmp(scanner_version, identity->version) == 0 &&
        strcmp(identity->board, info->name) == 0 &&
        strcmp(identity->firmware_name, info->name) == 0 &&
        strcmp(identity->app_project, info->project) == 0 &&
        strcmp(identity->hardware_type, info->hardware) == 0;
}

static fof_auto_identity_view_t auto_identity_view(
    const scanner_identity_snapshot_t *identity)
{
    fof_auto_identity_view_t view = {0};
    if (identity) {
        view.complete = identity->complete;
        view.identity_generation = identity->identity_generation;
        view.received_ms = identity->received_ms;
    }
    return view;
}

static fof_auto_offer_binding_t auto_offer_binding_view(
    const fw_auto_offer_binding_t *binding)
{
    fof_auto_offer_binding_t view = {0};
    if (binding) {
        view.generation = binding->generation;
        view.manifest_crc32 = binding->manifest_crc32;
        view.slot = binding->slot;
        view.identity_generation = binding->identity_generation;
        view.hardware_id = binding->hardware_id;
        view.captured_ms = binding->captured_ms;
    }
    return view;
}

static bool auto_legacy_ready_authorized(
    const fw_store_info_t *info,
    const scanner_identity_snapshot_t *identity,
    const char *scanner_board,
    const char *scanner_version,
    const char *target_version,
    uint32_t target_size,
    uint32_t target_crc32)
{
    if (!info || !identity) {
        return false;
    }
    const fof_legacy_ready_view_t ready = {
        .strict_fields_absent = true,
        .board = scanner_board,
        .current_version = scanner_version,
        .target_version = target_version,
        .size = target_size,
        .crc32 = target_crc32,
    };
    const fof_legacy_identity_view_t identity_view = {
        .received = identity->complete,
        .version = identity->version,
        .board = identity->board,
        .firmware_name = identity->firmware_name,
        .project = identity->app_project,
        .hardware = identity->hardware_type,
        .hardware_id = identity->hardware_id,
    };
    const fof_legacy_manifest_view_t manifest = {
        .target = info->name,
        .version = info->version,
        .project = info->project,
        .hardware = info->hardware,
        .sha256 = info->sha256,
        .size = info->size,
        .crc32 = info->checksum,
    };
    return fof_firmware_legacy_ready_authorized(
        &ready, &identity_view, &manifest);
}

static uint32_t auto_coordinator_crc(const fw_auto_coordinator_blob_t *blob)
{
    return esp_rom_crc32_le(
        0, (const uint8_t *)blob,
        offsetof(fw_auto_coordinator_blob_t, crc32));
}

static bool auto_coordinator_slot_is_terminal(uint8_t state)
{
    return state == FW_AUTO_SLOT_EXCLUDED ||
           state == FW_AUTO_SLOT_CONVERGED ||
           state == FW_AUTO_SLOT_CURRENT ||
           state == FW_AUTO_SLOT_NEWER_SKIPPED ||
           state == FW_AUTO_SLOT_REFUSED ||
           state == FW_AUTO_SLOT_FAILED;
}

static const char *auto_coordinator_state_name(uint8_t state)
{
    switch ((fw_auto_slot_state_t)state) {
        case FW_AUTO_SLOT_EXCLUDED: return "excluded";
        case FW_AUTO_SLOT_AWAITING_CHECK: return "awaiting_check";
        case FW_AUTO_SLOT_OFFERED: return "offered";
        case FW_AUTO_SLOT_READY_QUEUED: return "ready_queued";
        case FW_AUTO_SLOT_RELAYING: return "relaying";
        case FW_AUTO_SLOT_CONVERGED: return "converged";
        case FW_AUTO_SLOT_CURRENT: return "current";
        case FW_AUTO_SLOT_NEWER_SKIPPED: return "newer_skipped";
        case FW_AUTO_SLOT_REFUSED: return "refused";
        case FW_AUTO_SLOT_FAILED: return "failed";
        case FW_AUTO_SLOT_RECOVERING: return "recovering";
        default: return "invalid";
    }
}

static bool auto_coordinator_blob_valid(
    const fw_auto_coordinator_blob_t *blob)
{
    if (!blob || blob->magic != FW_AUTO_COORDINATOR_MAGIC ||
        blob->schema != FW_AUTO_COORDINATOR_SCHEMA ||
        blob->record_size != sizeof(*blob) || blob->generation == 0 ||
        blob->fail_closed > 1 ||
        (blob->target_slot_mask & (uint8_t)~FW_AUTO_TARGET_ALL) != 0 ||
        (blob->pending_mask & (uint8_t)~blob->target_slot_mask) != 0 ||
        (blob->fail_closed && blob->target_slot_mask != 0) ||
        (!blob->fail_closed && blob->target_slot_mask == 0) ||
        blob->crc32 != auto_coordinator_crc(blob)) {
        return false;
    }
    for (int scanner_id = 0; scanner_id < FW_AUTO_UPDATE_SCANNER_COUNT;
         ++scanner_id) {
        uint8_t bit = (uint8_t)(1u << scanner_id);
        uint8_t state = blob->slot_state[scanner_id];
        bool requested = (blob->target_slot_mask & bit) != 0;
        bool bound_present = blob->bound_hardware_id[scanner_id][0] != '\0';
        bool bound_required = state == FW_AUTO_SLOT_READY_QUEUED ||
            state == FW_AUTO_SLOT_RELAYING ||
            state == FW_AUTO_SLOT_RECOVERING;
        if (state > FW_AUTO_SLOT_RECOVERING ||
            blob->relay_attempts[scanner_id] > FW_AUTO_RELAY_MAX_ATTEMPTS ||
            blob->readiness_probe_attempts[scanner_id] >
                FW_AUTO_READY_MAX_PROBES ||
            (bound_present && !auto_hardware_id_is_canonical(
                blob->bound_hardware_id[scanner_id])) ||
            bound_required != bound_present) {
            return false;
        }
        if (blob->fail_closed) {
            if (state != FW_AUTO_SLOT_FAILED) {
                return false;
            }
            continue;
        }
        if (!requested &&
            (state != FW_AUTO_SLOT_EXCLUDED || bound_present)) {
            return false;
        }
        if (requested && state == FW_AUTO_SLOT_EXCLUDED) {
            return false;
        }
        if (((blob->pending_mask & bit) != 0) !=
            (state == FW_AUTO_SLOT_READY_QUEUED)) {
            return false;
        }
    }
    return true;
}

static bool auto_coordinator_save_locked(void)
{
    if (!s_auto_coordinator_loaded) {
        return false;
    }
    s_auto_coordinator.magic = FW_AUTO_COORDINATOR_MAGIC;
    s_auto_coordinator.schema = FW_AUTO_COORDINATOR_SCHEMA;
    s_auto_coordinator.record_size = sizeof(s_auto_coordinator);
    s_auto_coordinator.crc32 = auto_coordinator_crc(&s_auto_coordinator);
    if (!auto_coordinator_blob_valid(&s_auto_coordinator)) {
        ESP_LOGE(TAG,
                 "Refusing to persist invalid schema 3 coordinator state");
        return false;
    }
    return nvs_config_set_blob(NVS_FW_COORDINATOR, &s_auto_coordinator,
                               sizeof(s_auto_coordinator));
}

static void auto_coordinator_set_fail_closed_locked(
    uint32_t generation, uint32_t manifest_crc32)
{
    memset(&s_auto_coordinator, 0, sizeof(s_auto_coordinator));
    s_auto_coordinator_loaded = true;
    s_auto_coordinator.generation = generation;
    s_auto_coordinator.manifest_crc32 = manifest_crc32;
    s_auto_coordinator.fail_closed = 1;
    for (int scanner_id = 0; scanner_id < FW_AUTO_UPDATE_SCANNER_COUNT;
         ++scanner_id) {
        s_auto_coordinator.relay_attempts[scanner_id] =
            FW_AUTO_RELAY_MAX_ATTEMPTS;
        s_auto_coordinator.readiness_probe_attempts[scanner_id] =
            FW_AUTO_READY_MAX_PROBES;
        s_auto_coordinator.slot_state[scanner_id] = FW_AUTO_SLOT_FAILED;
    }
}

static void auto_coordinator_fail_closed_after_save_failure_locked(
    const char *stage)
{
    uint32_t generation = s_auto_coordinator_loaded
        ? s_auto_coordinator.generation : 0;
    uint32_t manifest_crc32 = s_auto_coordinator_loaded
        ? s_auto_coordinator.manifest_crc32 : 0;
    if (generation != 0) {
        auto_coordinator_set_fail_closed_locked(generation, manifest_crc32);
        bool persisted = false;
        for (int attempt = 0;
             attempt < FW_AUTO_COORDINATOR_SAVE_RETRIES && !persisted;
             ++attempt) {
            persisted = auto_coordinator_save_locked();
        }
        ESP_LOGE(TAG,
                 "Coordinator save failed at %s; fail-closed generation=%lu "
                 "persisted=%d",
                 stage ? stage : "unknown", (unsigned long)generation,
                 persisted ? 1 : 0);
    } else {
        ESP_LOGE(TAG,
                 "Coordinator save failed at %s without a loaded generation",
                 stage ? stage : "unknown");
    }
    s_auto_relay_worker_running = false;
}

static bool auto_coordinator_force_fail_closed(uint32_t generation,
                                               uint32_t manifest_crc32)
{
    if (generation == 0 || !auto_coordinator_lock()) {
        return false;
    }
    auto_coordinator_set_fail_closed_locked(generation, manifest_crc32);
    bool ok = auto_coordinator_save_locked();
    auto_coordinator_unlock();
    return ok;
}

static bool auto_coordinator_begin_generation(uint32_t generation,
                                              uint8_t target_slot_mask,
                                              uint32_t manifest_crc32)
{
    uint32_t identity_floors[FW_AUTO_UPDATE_SCANNER_COUNT] = {0};
    if (generation == 0 || target_slot_mask == 0 ||
        (target_slot_mask & (uint8_t)~FW_AUTO_TARGET_ALL) != 0) {
        return false;
    }
    /* Snapshot identity generations before taking the coordinator mutex so
     * publication and coordinator lock ordering can never invert. */
    auto_capture_identity_floors(identity_floors);
    if (!auto_coordinator_lock()) {
        return false;
    }
    memset(&s_auto_coordinator, 0, sizeof(s_auto_coordinator));
    auto_set_identity_floors_locked(identity_floors);
    s_auto_coordinator_loaded = true;
    s_auto_coordinator.generation = generation;
    s_auto_coordinator.manifest_crc32 = manifest_crc32;
    s_auto_coordinator.target_slot_mask = target_slot_mask;
    for (int scanner_id = 0; scanner_id < FW_AUTO_UPDATE_SCANNER_COUNT;
         ++scanner_id) {
        uint8_t bit = (uint8_t)(1u << scanner_id);
        s_auto_coordinator.slot_state[scanner_id] =
            (target_slot_mask & bit) ? FW_AUTO_SLOT_AWAITING_CHECK
                                     : FW_AUTO_SLOT_EXCLUDED;
    }
    bool ok = auto_coordinator_save_locked();
    if (!ok) {
        auto_coordinator_set_fail_closed_locked(generation, manifest_crc32);
        (void)auto_coordinator_save_locked();
    }
    auto_coordinator_unlock();
    return ok;
}

static bool auto_coordinator_initialize_fail_closed(
    uint32_t generation, uint32_t manifest_crc32)
{
    if (generation == 0 || !auto_coordinator_lock()) {
        return false;
    }
    auto_coordinator_set_fail_closed_locked(generation, manifest_crc32);
    bool ok = auto_coordinator_save_locked();
    auto_coordinator_unlock();
    return ok;
}

static bool auto_coordinator_slot_requested(int scanner_id,
                                            uint32_t generation)
{
    if (scanner_id < 0 || scanner_id >= FW_AUTO_UPDATE_SCANNER_COUNT ||
        !auto_coordinator_lock()) {
        return false;
    }
    bool requested = s_auto_coordinator_loaded &&
        s_auto_coordinator.generation == generation &&
        (s_auto_coordinator.target_slot_mask & (uint8_t)(1u << scanner_id)) != 0;
    auto_coordinator_unlock();
    return requested;
}

void fw_store_get_auto_update_status(fw_auto_update_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!auto_coordinator_lock()) return;
    out->worker_running = s_auto_relay_worker_running;
    if (s_auto_coordinator_loaded) {
        out->generation = s_auto_coordinator.generation;
        out->target_slot_mask = s_auto_coordinator.target_slot_mask;
        out->pending_mask = s_auto_coordinator.pending_mask;
        for (int scanner_id = 0; scanner_id < FW_AUTO_UPDATE_SCANNER_COUNT;
             ++scanner_id) {
            out->attempts[scanner_id] =
                s_auto_coordinator.relay_attempts[scanner_id];
            out->readiness_probe_attempts[scanner_id] =
                s_auto_coordinator.readiness_probe_attempts[scanner_id];
            snprintf(out->state[scanner_id], sizeof(out->state[scanner_id]),
                     "%s", auto_coordinator_state_name(
                         s_auto_coordinator.slot_state[scanner_id]));
        }
    }
    auto_coordinator_unlock();
}

static bool auto_relay_error_is_retryable(const char *error)
{
    if (!error || !error[0]) return true;
    return strstr(error, "operation_active") ||
           strstr(error, "command_ingress_unhealthy") ||
           strstr(error, "timeout") ||
           strstr(error, "scanner_error") ||
           strstr(error, "crc_retries_exhausted") ||
           strstr(error, "flash_read");
}

static bool auto_coordinator_slot_gate_open_locked(int scanner_id)
{
    bool ble_requested =
        (s_auto_coordinator.target_slot_mask & FW_AUTO_UPDATE_SLOT_BLE) != 0;
    return scanner_id != 1 || fof_auto_wifi_gate_open(
        ble_requested,
        (fof_auto_slot_state_t)s_auto_coordinator.slot_state[0]);
}

static bool auto_coordinator_slot_gate_open(int scanner_id,
                                            uint32_t generation)
{
    if (scanner_id < 0 || scanner_id >= FW_AUTO_UPDATE_SCANNER_COUNT ||
        !auto_coordinator_lock()) {
        return false;
    }
    uint8_t bit = (uint8_t)(1u << scanner_id);
    bool open = s_auto_coordinator_loaded &&
        s_auto_coordinator.generation == generation &&
        (s_auto_coordinator.target_slot_mask & bit) != 0 &&
        auto_coordinator_slot_gate_open_locked(scanner_id);
    auto_coordinator_unlock();
    return open;
}

static bool auto_offer_binding_same(
    const fw_auto_offer_binding_t *left,
    const fw_auto_offer_binding_t *right)
{
    return left && right && left->generation == right->generation &&
        left->manifest_crc32 == right->manifest_crc32 &&
        left->slot == right->slot &&
        left->identity_generation == right->identity_generation &&
        left->captured_ms == right->captured_ms &&
        strcmp(left->hardware_id, right->hardware_id) == 0;
}

static void auto_clear_ready_bindings_locked(int scanner_id)
{
    memset(&s_auto_offer_bindings[scanner_id], 0,
           sizeof(s_auto_offer_bindings[scanner_id]));
    memset(&s_auto_ready_bindings[scanner_id], 0,
           sizeof(s_auto_ready_bindings[scanner_id]));
    s_auto_legacy_ready[scanner_id] = false;
}

static void auto_reset_ready_queue_after_revalidation_failure(
    int scanner_id,
    uint32_t generation,
    const fw_auto_offer_binding_t *expected_binding,
    bool expected_legacy_mode)
{
    bool resume_scanner = false;
    if (!auto_coordinator_lock()) {
        return;
    }
    uint8_t bit = (uint8_t)(1u << scanner_id);
    if (s_auto_coordinator_loaded &&
        s_auto_coordinator.generation == generation &&
        s_auto_coordinator.slot_state[scanner_id] ==
            FW_AUTO_SLOT_READY_QUEUED &&
        (s_auto_coordinator.pending_mask & bit) &&
        s_auto_legacy_ready[scanner_id] == expected_legacy_mode &&
        auto_offer_binding_same(&s_auto_ready_bindings[scanner_id],
                                expected_binding)) {
        fw_auto_coordinator_blob_t before = s_auto_coordinator;
        s_auto_coordinator.pending_mask &= (uint8_t)~bit;
        s_auto_coordinator.slot_state[scanner_id] =
            FW_AUTO_SLOT_AWAITING_CHECK;
        s_auto_coordinator.bound_hardware_id[scanner_id][0] = '\0';
        if (!auto_coordinator_save_locked()) {
            (void)before;
            auto_coordinator_fail_closed_after_save_failure_locked(
                "ready_second_identity");
        } else {
            auto_clear_ready_bindings_locked(scanner_id);
            resume_scanner = true;
        }
    }
    auto_coordinator_unlock();

    if (resume_scanner) {
        uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"start\"}");
    }
}

static void auto_begin_recovery_after_restore(uint8_t recovery_mask)
{
    for (int scanner_id = 0; scanner_id < FW_AUTO_UPDATE_SCANNER_COUNT;
         ++scanner_id) {
        uint8_t bit = (uint8_t)(1u << scanner_id);
        if (!(recovery_mask & bit)) {
            continue;
        }

        bool active = false;
        if (auto_coordinator_lock()) {
            active = s_auto_coordinator_loaded &&
                s_auto_coordinator.slot_state[scanner_id] ==
                    FW_AUTO_SLOT_RECOVERING;
            auto_coordinator_unlock();
        }
        if (!active) {
            continue;
        }

        uart_rx_send_command_to_scanner(
            scanner_id, "{\"type\":\"ota_abort\"}");
        vTaskDelay(pdMS_TO_TICKS(50));
        uart_port_t uart_num = scanner_id == 1
            ? CONFIG_WIFI_SCANNER_UART : CONFIG_BLE_SCANNER_UART;
        relay_send_wire_abort_sentinel(uart_num);

        int64_t now_ms = esp_timer_get_time() / 1000;
        if (auto_coordinator_lock()) {
            if (s_auto_coordinator_loaded &&
                s_auto_coordinator.slot_state[scanner_id] ==
                    FW_AUTO_SLOT_RECOVERING) {
                s_auto_recovery_not_before_ms[scanner_id] =
                    now_ms + FW_AUTO_RECOVERY_COOLDOWN_MS;
                s_auto_recovery_next_probe_ms[scanner_id] =
                    s_auto_recovery_not_before_ms[scanner_id];
            }
            auto_coordinator_unlock();
        }
        ESP_LOGW(TAG,
                 "Scanner[%d] interrupted relay aborted; recovery probes "
                 "held for %d ms",
                 scanner_id, FW_AUTO_RECOVERY_COOLDOWN_MS);
    }
}

static bool auto_wait_for_ready_second_identity(
    int scanner_id,
    const fw_store_info_t *info,
    const fw_auto_offer_binding_t *binding,
    bool legacy_mode,
    scanner_identity_snapshot_t *out)
{
    if (!info || !binding || !out ||
        binding->generation != info->generation ||
        binding->manifest_crc32 != info->manifest_crc32 ||
        binding->slot != (uint8_t)scanner_id ||
        !auto_hardware_id_is_canonical(binding->hardware_id)) {
        return false;
    }

    int64_t deadline_ms = (esp_timer_get_time() / 1000) +
        FW_AUTO_SECOND_IDENTITY_WAIT_MS;
    do {
        scanner_identity_snapshot_t identity = {0};
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (uart_rx_get_scanner_identity_snapshot(scanner_id, &identity)) {
            fof_auto_identity_view_t identity_view =
                auto_identity_view(&identity);
            if (fof_auto_identity_is_fresh(
                    &identity_view, binding->identity_generation,
                    now_ms, FW_AUTO_IDENTITY_MAX_AGE_MS) &&
                strcmp(identity.hardware_id, binding->hardware_id) == 0 &&
                (!legacy_mode ||
                 strcmp(identity.version,
                        FOF_LEGACY_READY_BOOTSTRAP_VERSION) == 0) &&
                staged_firmware_is_newer_for_scanner(
                    info, identity.board, identity.version) &&
                auto_identity_matches_manifest(
                    &identity, info, identity.board, identity.version)) {
                *out = identity;
                return true;
            }
        }
        if (now_ms >= deadline_ms) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(FW_AUTO_IDENTITY_POLL_MS));
    } while (true);
    return false;
}

static void auto_advance_ready_binding_locked(
    int scanner_id,
    uint32_t generation,
    uint32_t manifest_crc32,
    const scanner_identity_snapshot_t *identity)
{
    fw_auto_offer_binding_t *binding =
        &s_auto_ready_bindings[scanner_id];
    memset(binding, 0, sizeof(*binding));
    binding->generation = generation;
    binding->manifest_crc32 = manifest_crc32;
    binding->slot = (uint8_t)scanner_id;
    binding->identity_generation = identity->identity_generation;
    snprintf(binding->hardware_id, sizeof(binding->hardware_id), "%s",
             identity->hardware_id);
    binding->captured_ms = esp_timer_get_time() / 1000;
}

static void fw_auto_relay_task(void *arg)
{
    (void)arg;
    /* The stage/boot path sends probe #1 immediately.  Waiting a full probe
     * interval here gives scanners time to boot and answer before probe #2;
     * three persisted probes therefore span roughly 30 seconds. */
    vTaskDelay(pdMS_TO_TICKS(FW_AUTO_READY_PROBE_DELAY_MS));
    while (true) {
        int scanner_id = -1;
        int probe_scanner_id = -1;
        int recovery_probe_scanner_id = -1;
        bool recovery_waiting = false;
        bool recovery_terminal = false;
        uint32_t recovery_wait_ms = 250;
        uint32_t relay_generation = 0;
        uint8_t attempt_number = 0;
        bool legacy_mode = false;
        fw_auto_offer_binding_t ready_binding = {0};
        char relay_bound_hardware_id[18] = {0};

        /* A store/upload owner has not attempted a scanner relay.  Wait for
         * it to finish before reserving a durable relay attempt. */
        if (operation_is_active()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!auto_coordinator_lock()) {
            /* The task still owns worker_running.  Retry instead of exiting
             * with a phantom running flag that would suppress all restarts. */
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (!s_auto_coordinator_loaded) {
            s_auto_relay_worker_running = false;
            auto_coordinator_unlock();
            break;
        }

        int64_t recovery_now_ms = esp_timer_get_time() / 1000;
        for (int slot = 0; slot < FW_AUTO_UPDATE_SCANNER_COUNT; ++slot) {
            uint8_t bit = (uint8_t)(1u << slot);
            if (!(s_auto_coordinator.target_slot_mask & bit) ||
                s_auto_coordinator.slot_state[slot] !=
                    FOF_AUTO_SLOT_RECOVERING ||
                !auto_coordinator_slot_gate_open_locked(slot)) {
                continue;
            }
            int64_t eligible_ms = s_auto_recovery_not_before_ms[slot];
            if (s_auto_recovery_next_probe_ms[slot] > eligible_ms) {
                eligible_ms = s_auto_recovery_next_probe_ms[slot];
            }
            fof_auto_probe_decision_t decision =
                fof_auto_recovery_probe_decide(
                    recovery_now_ms, eligible_ms,
                    s_auto_coordinator.readiness_probe_attempts[slot],
                    FW_AUTO_READY_MAX_PROBES);
            if (decision == FOF_AUTO_PROBE_WAIT) {
                recovery_waiting = true;
                int64_t remaining_ms = eligible_ms - recovery_now_ms;
                if (remaining_ms > 0 && remaining_ms < 1000) {
                    recovery_wait_ms = (uint32_t)remaining_ms;
                } else if (remaining_ms >= 1000) {
                    recovery_wait_ms = 1000;
                }
                break;
            }

            fw_auto_coordinator_blob_t recovery_before = s_auto_coordinator;
            if (decision == FOF_AUTO_PROBE_EXHAUSTED) {
                s_auto_coordinator.slot_state[slot] = FW_AUTO_SLOT_FAILED;
                s_auto_coordinator.pending_mask &= (uint8_t)~bit;
                s_auto_coordinator.bound_hardware_id[slot][0] = '\0';
                if (!auto_coordinator_save_locked()) {
                    (void)recovery_before;
                    auto_coordinator_fail_closed_after_save_failure_locked(
                        "recovery_probe_exhausted");
                } else {
                    auto_clear_ready_bindings_locked(slot);
                    s_auto_recovery_not_before_ms[slot] = 0;
                    s_auto_recovery_next_probe_ms[slot] = 0;
                }
                recovery_terminal = true;
                break;
            }

            s_auto_coordinator.readiness_probe_attempts[slot]++;
            if (!auto_coordinator_save_locked()) {
                (void)recovery_before;
                auto_coordinator_fail_closed_after_save_failure_locked(
                    "recovery_probe_reserved");
            } else {
                s_auto_recovery_next_probe_ms[slot] =
                    recovery_now_ms + FW_AUTO_RECOVERY_PROBE_DELAY_MS;
                recovery_probe_scanner_id = slot;
            }
            break;
        }

        for (int slot = 0;
             !recovery_waiting && !recovery_terminal &&
             recovery_probe_scanner_id < 0 &&
             slot < FW_AUTO_UPDATE_SCANNER_COUNT;
             ++slot) {
            uint8_t bit = (uint8_t)(1u << slot);
            if (!(s_auto_coordinator.pending_mask & bit) ||
                !auto_coordinator_slot_gate_open_locked(slot)) {
                continue;
            }
            if (s_auto_coordinator.relay_attempts[slot] >=
                FW_AUTO_RELAY_MAX_ATTEMPTS) {
                fw_auto_coordinator_blob_t exhausted_before =
                    s_auto_coordinator;
                s_auto_coordinator.pending_mask &= (uint8_t)~bit;
                s_auto_coordinator.slot_state[slot] = FW_AUTO_SLOT_FAILED;
                s_auto_coordinator.bound_hardware_id[slot][0] = '\0';
                if (!auto_coordinator_save_locked()) {
                    (void)exhausted_before;
                    auto_coordinator_fail_closed_after_save_failure_locked(
                        "relay_attempt_exhausted");
                    s_auto_relay_worker_running = false;
                    break;
                }
                auto_clear_ready_bindings_locked(slot);
                continue;
            }

            /* Every automatic relay must prove a newer complete identity
             * while stopped. Select without consuming an attempt; the wait
             * and snapshot copies happen after this mutex drops. */
            scanner_id = slot;
            relay_generation = s_auto_coordinator.generation;
            legacy_mode = s_auto_legacy_ready[slot];
            ready_binding = s_auto_ready_bindings[slot];
            snprintf(relay_bound_hardware_id,
                     sizeof(relay_bound_hardware_id), "%s",
                     s_auto_coordinator.bound_hardware_id[slot]);
            break;
        }

        if (scanner_id < 0 && s_auto_relay_worker_running) {
            if (!recovery_waiting && !recovery_terminal &&
                recovery_probe_scanner_id < 0) {
                for (int slot = 0;
                     slot < FW_AUTO_UPDATE_SCANNER_COUNT; ++slot) {
                    uint8_t bit = (uint8_t)(1u << slot);
                    uint8_t state = s_auto_coordinator.slot_state[slot];
                    if (!(s_auto_coordinator.target_slot_mask & bit) ||
                        !auto_coordinator_slot_gate_open_locked(slot) ||
                        (state != FW_AUTO_SLOT_AWAITING_CHECK &&
                         state != FW_AUTO_SLOT_OFFERED)) {
                        continue;
                    }
                    if (s_auto_coordinator.readiness_probe_attempts[slot] >=
                        FW_AUTO_READY_MAX_PROBES) {
                        fw_auto_coordinator_blob_t exhausted_before =
                            s_auto_coordinator;
                        s_auto_coordinator.slot_state[slot] =
                            FW_AUTO_SLOT_FAILED;
                        s_auto_coordinator.pending_mask &= (uint8_t)~bit;
                        if (!auto_coordinator_save_locked()) {
                            (void)exhausted_before;
                            auto_coordinator_fail_closed_after_save_failure_locked(
                                "readiness_probe_exhausted");
                            s_auto_relay_worker_running = false;
                        } else {
                            probe_scanner_id = -2;
                        }
                        break;
                    }
                    fw_auto_coordinator_blob_t before = s_auto_coordinator;
                    s_auto_coordinator.readiness_probe_attempts[slot]++;
                    if (!auto_coordinator_save_locked()) {
                        (void)before;
                        auto_coordinator_fail_closed_after_save_failure_locked(
                            "readiness_probe_reserved");
                        s_auto_relay_worker_running = false;
                    } else {
                        probe_scanner_id = slot;
                    }
                    break;
                }
            }
        }

        if (scanner_id < 0 && probe_scanner_id == -1 &&
            recovery_probe_scanner_id == -1 && !recovery_waiting &&
            !recovery_terminal &&
            s_auto_relay_worker_running) {
            s_auto_relay_worker_running = false;
        }
        bool worker_continues = s_auto_relay_worker_running;
        auto_coordinator_unlock();

        if (recovery_probe_scanner_id >= 0) {
            uart_rx_send_command_to_scanner(
                recovery_probe_scanner_id,
                "{\"type\":\"fw_check_now\"}");
            vTaskDelay(pdMS_TO_TICKS(FW_AUTO_RECOVERY_PROBE_DELAY_MS));
            continue;
        }
        if (recovery_terminal) {
            (void)auto_coordinator_reprompt_requested();
            continue;
        }
        if (recovery_waiting) {
            vTaskDelay(pdMS_TO_TICKS(recovery_wait_ms));
            continue;
        }

        if (scanner_id >= 0) {
            fw_store_info_t relay_info = {0};
            scanner_identity_snapshot_t second_identity = {0};
            bool identity_confirmed =
                fw_store_get_info(&relay_info) &&
                auto_wait_for_ready_second_identity(
                    scanner_id, &relay_info, &ready_binding,
                    legacy_mode, &second_identity);
            if (!identity_confirmed) {
                ESP_LOGW(TAG,
                         "Auto relay[%d] refused before attempt: newer "
                         "same-MAC identity not proved (legacy=%d)",
                         scanner_id, legacy_mode ? 1 : 0);
                auto_reset_ready_queue_after_revalidation_failure(
                    scanner_id, relay_generation, &ready_binding,
                    legacy_mode);
                continue;
            }

            /* Do not let a store operation that started during the identity
             * wait force a durable attempt reservation. */
            if (operation_is_active()) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            /* Snapshot the latest published identity before taking the
             * coordinator mutex, then revalidate both under coordinator
             * ownership without inverting the two mutexes. */
            scanner_identity_snapshot_t current_identity = {0};
            if (!uart_rx_get_scanner_identity_snapshot(
                    scanner_id, &current_identity)) {
                auto_reset_ready_queue_after_revalidation_failure(
                    scanner_id, relay_generation, &ready_binding,
                    legacy_mode);
                continue;
            }

            while (!auto_coordinator_lock()) {
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            uint8_t bit = (uint8_t)(1u << scanner_id);
            int64_t now_ms = esp_timer_get_time() / 1000;
            fof_auto_identity_view_t current_identity_view =
                auto_identity_view(&current_identity);
            fof_auto_offer_binding_t current_ready_view =
                auto_offer_binding_view(
                    &s_auto_ready_bindings[scanner_id]);
            bool reservation_valid = s_auto_coordinator_loaded &&
                relay_info.generation == ready_binding.generation &&
                relay_info.manifest_crc32 == ready_binding.manifest_crc32 &&
                s_auto_coordinator.generation == relay_generation &&
                s_auto_coordinator.manifest_crc32 ==
                    ready_binding.manifest_crc32 &&
                s_auto_coordinator.slot_state[scanner_id] ==
                    FW_AUTO_SLOT_READY_QUEUED &&
                (s_auto_coordinator.pending_mask & bit) &&
                auto_coordinator_slot_gate_open_locked(scanner_id) &&
                s_auto_coordinator.relay_attempts[scanner_id] <
                    FW_AUTO_RELAY_MAX_ATTEMPTS &&
                s_auto_legacy_ready[scanner_id] == legacy_mode &&
                auto_offer_binding_same(
                    &s_auto_ready_bindings[scanner_id],
                    &ready_binding) &&
                fof_auto_offer_binding_matches(
                    &current_ready_view,
                    ready_binding.generation,
                    ready_binding.manifest_crc32,
                    ready_binding.slot,
                    ready_binding.identity_generation,
                    ready_binding.hardware_id,
                    now_ms, FW_AUTO_OFFER_BINDING_TTL_MS) &&
                fof_auto_identity_is_fresh(
                    &current_identity_view,
                    ready_binding.identity_generation,
                    now_ms, FW_AUTO_IDENTITY_MAX_AGE_MS) &&
                current_identity.identity_generation >=
                    second_identity.identity_generation &&
                strcmp(current_identity.hardware_id,
                       s_auto_coordinator.bound_hardware_id[scanner_id]) ==
                    0 &&
                strcmp(current_identity.hardware_id,
                       relay_bound_hardware_id) == 0 &&
                strcmp(current_identity.hardware_id,
                       ready_binding.hardware_id) == 0 &&
                (!legacy_mode ||
                 strcmp(current_identity.version,
                        FOF_LEGACY_READY_BOOTSTRAP_VERSION) == 0) &&
                staged_firmware_is_newer_for_scanner(
                    &relay_info, current_identity.board,
                    current_identity.version) &&
                auto_identity_matches_manifest(
                    &current_identity, &relay_info,
                    current_identity.board, current_identity.version);
            if (!reservation_valid) {
                auto_coordinator_unlock();
                auto_reset_ready_queue_after_revalidation_failure(
                    scanner_id, relay_generation, &ready_binding,
                    legacy_mode);
                continue;
            }

            fw_auto_coordinator_blob_t before = s_auto_coordinator;
            s_auto_coordinator.pending_mask &= (uint8_t)~bit;
            s_auto_coordinator.relay_attempts[scanner_id]++;
            attempt_number =
                s_auto_coordinator.relay_attempts[scanner_id];
            s_auto_coordinator.slot_state[scanner_id] =
                FW_AUTO_SLOT_RELAYING;
            if (!auto_coordinator_save_locked()) {
                (void)before;
                auto_coordinator_fail_closed_after_save_failure_locked(
                    "relay_attempt_reserved");
                scanner_id = -1;
                s_auto_relay_worker_running = false;
            }
            auto_coordinator_unlock();
            if (scanner_id < 0) {
                continue;
            }

            fw_relay_result_t result = {0};
            fw_relay_stored_to_scanner(scanner_id, relay_generation,
                                        relay_bound_hardware_id, true,
                                        legacy_mode, false, false,
                                        &result);
            ESP_LOGW(TAG,
                     "Auto scanner relay[%d] generation=%lu attempt=%u "
                     "ok=%d stage=%s error=%s chunks=%d",
                     scanner_id, (unsigned long)relay_generation,
                     (unsigned)attempt_number, result.ok ? 1 : 0,
                     result.stage, result.error, result.chunks);

            bool terminal = false;
            bool retry = false;
            bool identity_reset = false;
            bool resume_scanner = false;
            while (!auto_coordinator_lock()) {
                /* This task still owns the durable RELAYING reservation. Do
                 * not discard its only transfer/health result on transient
                 * mutex contention; mirror the loop-top retry behavior. */
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            if (s_auto_coordinator_loaded &&
                s_auto_coordinator.generation == relay_generation &&
                s_auto_coordinator.slot_state[scanner_id] ==
                    FW_AUTO_SLOT_RELAYING) {
                fw_auto_coordinator_blob_t before = s_auto_coordinator;
                if (strcmp(result.error, "operation_active") == 0) {
                    /* operation_try_begin() lost a race after the busy
                     * check, so no scanner relay started.  Restore the
                     * reserved budget durably before requeueing. */
                    if (s_auto_coordinator.relay_attempts[scanner_id] > 0) {
                        s_auto_coordinator.relay_attempts[scanner_id]--;
                    }
                    s_auto_coordinator.slot_state[scanner_id] =
                        FW_AUTO_SLOT_READY_QUEUED;
                    s_auto_coordinator.pending_mask |=
                        (uint8_t)(1u << scanner_id);
                    retry = true;
                } else if (strcmp(result.error, "bound_identity_changed") ==
                           0) {
                    /* No OTA bytes were sent. Return the reserved budget and
                     * force a fresh check/identity sequence. */
                    if (s_auto_coordinator.relay_attempts[scanner_id] > 0) {
                        s_auto_coordinator.relay_attempts[scanner_id]--;
                    }
                    s_auto_coordinator.slot_state[scanner_id] =
                        FW_AUTO_SLOT_AWAITING_CHECK;
                    s_auto_coordinator.pending_mask &=
                        (uint8_t)~(1u << scanner_id);
                    s_auto_coordinator.bound_hardware_id[scanner_id][0] = '\0';
                    identity_reset = true;
                } else if (result.ok) {
                    s_auto_coordinator.slot_state[scanner_id] =
                        FW_AUTO_SLOT_CONVERGED;
                    terminal = true;
                } else if (auto_relay_error_is_retryable(result.error) &&
                           s_auto_coordinator.relay_attempts[scanner_id] <
                               FW_AUTO_RELAY_MAX_ATTEMPTS) {
                    s_auto_coordinator.slot_state[scanner_id] =
                        FW_AUTO_SLOT_READY_QUEUED;
                    s_auto_coordinator.pending_mask |=
                        (uint8_t)(1u << scanner_id);
                    retry = true;
                } else {
                    s_auto_coordinator.slot_state[scanner_id] =
                        auto_relay_error_is_retryable(result.error)
                            ? FW_AUTO_SLOT_FAILED
                            : FW_AUTO_SLOT_REFUSED;
                    terminal = true;
                }
                if (terminal) {
                    s_auto_coordinator.bound_hardware_id[scanner_id][0] = '\0';
                }
                if (!auto_coordinator_save_locked()) {
                    (void)before;
                    auto_coordinator_fail_closed_after_save_failure_locked(
                        "relay_result");
                    retry = false;
                    identity_reset = false;
                } else if (identity_reset) {
                    auto_clear_ready_bindings_locked(scanner_id);
                    resume_scanner = true;
                } else if (retry) {
                    auto_advance_ready_binding_locked(
                        scanner_id, relay_generation,
                        ready_binding.manifest_crc32, &current_identity);
                } else if (terminal) {
                    auto_clear_ready_bindings_locked(scanner_id);
                }
            }
            worker_continues = s_auto_relay_worker_running;
            auto_coordinator_unlock();
            if (resume_scanner) {
                uart_rx_send_command_to_scanner(scanner_id,
                                                "{\"type\":\"start\"}");
            } else if (terminal && !result.ok) {
                uart_rx_send_command_to_scanner(scanner_id,
                                                "{\"type\":\"start\"}");
            }
            if (terminal) {
                /* A terminal BLE result opens the Wi-Fi slot. Prompt it only
                 * after that durable result has been considered above. */
                (void)auto_coordinator_reprompt_requested();
            }
            if (retry) {
                vTaskDelay(pdMS_TO_TICKS(1000u << attempt_number));
            }
            continue;
        }

        if (probe_scanner_id >= 0) {
            uart_rx_send_command_to_scanner(
                probe_scanner_id, "{\"type\":\"fw_check_now\"}");
            vTaskDelay(pdMS_TO_TICKS(FW_AUTO_READY_PROBE_DELAY_MS));
            continue;
        }
        if (probe_scanner_id == -2) {
            /* Exhausting BLE readiness is terminal and therefore opens the
             * Wi-Fi slot for its first bounded prompt. */
            (void)auto_coordinator_reprompt_requested();
            continue;
        }
        if (!worker_continues) {
            break;
        }
    }
    auto_coordinator_release_excluded_slots();
    vTaskDelete(NULL);
}

static bool auto_coordinator_start_worker(void)
{
    if (!auto_coordinator_lock()) {
        return false;
    }
    bool has_work = false;
    if (s_auto_coordinator_loaded) {
        for (int scanner_id = 0;
             scanner_id < FW_AUTO_UPDATE_SCANNER_COUNT; ++scanner_id) {
            uint8_t bit = (uint8_t)(1u << scanner_id);
            if ((s_auto_coordinator.target_slot_mask & bit) &&
                !auto_coordinator_slot_is_terminal(
                    s_auto_coordinator.slot_state[scanner_id])) {
                has_work = true;
                break;
            }
        }
    }
    if (!has_work || s_auto_relay_worker_running) {
        bool already_running = s_auto_relay_worker_running;
        auto_coordinator_unlock();
        return has_work ? already_running : true;
    }
    s_auto_relay_worker_running = true;
    auto_coordinator_unlock();

    BaseType_t ok = xTaskCreate(fw_auto_relay_task, "fw_auto_relay", 7168,
                                NULL, 5, NULL);
    if (ok != pdPASS) {
        if (auto_coordinator_lock()) {
            s_auto_relay_worker_running = false;
            auto_coordinator_unlock();
        }
        ESP_LOGE(TAG, "Failed to create durable scanner-update worker");
        return false;
    }
    return true;
}

static bool auto_coordinator_reprompt_requested(void)
{
    uint8_t send_mask = 0;
    if (!auto_coordinator_lock()) {
        return false;
    }
    fw_auto_coordinator_blob_t before = s_auto_coordinator;
    if (s_auto_coordinator_loaded) {
        for (int scanner_id = 0;
             scanner_id < FW_AUTO_UPDATE_SCANNER_COUNT; ++scanner_id) {
            uint8_t bit = (uint8_t)(1u << scanner_id);
            uint8_t state = s_auto_coordinator.slot_state[scanner_id];
            if (!(s_auto_coordinator.target_slot_mask & bit) ||
                !auto_coordinator_slot_gate_open_locked(scanner_id) ||
                auto_coordinator_slot_is_terminal(state) ||
                (state != FW_AUTO_SLOT_AWAITING_CHECK &&
                 state != FW_AUTO_SLOT_OFFERED)) {
                continue;
            }
            if (s_auto_coordinator.readiness_probe_attempts[scanner_id] >=
                FW_AUTO_READY_MAX_PROBES) {
                s_auto_coordinator.slot_state[scanner_id] = FW_AUTO_SLOT_FAILED;
                continue;
            }
            s_auto_coordinator.readiness_probe_attempts[scanner_id]++;
            send_mask |= bit;
        }
    }
    bool changed = memcmp(&before, &s_auto_coordinator,
                          sizeof(before)) != 0;
    if (changed && !auto_coordinator_save_locked()) {
        s_auto_coordinator = before;
        send_mask = 0;
    }
    auto_coordinator_unlock();

    for (int scanner_id = 0; scanner_id < FW_AUTO_UPDATE_SCANNER_COUNT;
         ++scanner_id) {
        if (send_mask & (uint8_t)(1u << scanner_id)) {
            uart_rx_send_command_to_scanner(
                scanner_id, "{\"type\":\"fw_check_now\"}");
        }
    }
    return changed ? send_mask != 0 : true;
}

static void auto_coordinator_release_excluded_slots(void)
{
    uint8_t target_slot_mask = 0;
    if (!auto_coordinator_lock()) {
        return;
    }
    if (s_auto_coordinator_loaded) {
        target_slot_mask = s_auto_coordinator.target_slot_mask;
    }
    auto_coordinator_unlock();

#ifdef FOF_BADGE_VARIANT
    /* Quiet mode is an explicit user request to keep scanners halted. A wake
     * will reassert active mode through the power runtime, so do not defeat
     * that request merely to release a scanner excluded from this update. */
    if (badge_power_runtime_is_quiet()) {
        return;
    }
#endif
    for (int scanner_id = 0; scanner_id < FW_AUTO_UPDATE_SCANNER_COUNT;
         ++scanner_id) {
        uint8_t bit = (uint8_t)(1u << scanner_id);
        if (!(target_slot_mask & bit)) {
            uart_rx_send_command_to_scanner(scanner_id,
                                            "{\"type\":\"start\"}");
        }
    }
}

static bool enqueue_auto_relay(
    int scanner_id,
    uint32_t generation,
    uint32_t manifest_crc32,
    const scanner_identity_snapshot_t *identity,
    bool legacy_mode)
{
    if (scanner_id < 0 || scanner_id >= FW_AUTO_UPDATE_SCANNER_COUNT ||
        !identity ||
        !auto_coordinator_lock()) {
        return false;
    }
    uint8_t bit = (uint8_t)(1u << scanner_id);
    int64_t now_ms = esp_timer_get_time() / 1000;
    fof_auto_identity_view_t identity_view = auto_identity_view(identity);
    fof_auto_offer_binding_t offer_view = auto_offer_binding_view(
        &s_auto_offer_bindings[scanner_id]);
    if (!s_auto_coordinator_loaded ||
        s_auto_coordinator.generation != generation ||
        s_auto_coordinator.manifest_crc32 != manifest_crc32 ||
        !(s_auto_coordinator.target_slot_mask & bit) ||
        !auto_coordinator_slot_gate_open_locked(scanner_id) ||
        !fof_auto_queue_state_allows((fof_auto_slot_state_t)
            s_auto_coordinator.slot_state[scanner_id]) ||
        s_auto_coordinator.relay_attempts[scanner_id] >=
            FW_AUTO_RELAY_MAX_ATTEMPTS ||
        !fof_auto_identity_is_fresh(
            &identity_view,
            s_auto_identity_generation_floor[scanner_id],
            now_ms, FW_AUTO_IDENTITY_MAX_AGE_MS) ||
        !auto_hardware_id_is_canonical(identity->hardware_id) ||
        (legacy_mode && !fof_auto_offer_binding_matches(
            &offer_view, generation, manifest_crc32, (uint8_t)scanner_id,
            identity->identity_generation, identity->hardware_id,
            now_ms, FW_AUTO_OFFER_BINDING_TTL_MS))) {
        auto_coordinator_unlock();
        return false;
    }

    fw_auto_coordinator_blob_t before = s_auto_coordinator;
    snprintf(s_auto_coordinator.bound_hardware_id[scanner_id],
             sizeof(s_auto_coordinator.bound_hardware_id[scanner_id]),
             "%s", identity->hardware_id);
    s_auto_coordinator.slot_state[scanner_id] = FW_AUTO_SLOT_READY_QUEUED;
    s_auto_coordinator.pending_mask |= bit;
    bool ok = auto_coordinator_save_locked();
    if (!ok) {
        s_auto_coordinator = before;
    } else {
        memset(&s_auto_ready_bindings[scanner_id], 0,
               sizeof(s_auto_ready_bindings[scanner_id]));
        fw_auto_offer_binding_t *binding =
            &s_auto_ready_bindings[scanner_id];
        binding->generation = generation;
        binding->manifest_crc32 = manifest_crc32;
        binding->slot = (uint8_t)scanner_id;
        binding->identity_generation = identity->identity_generation;
        snprintf(binding->hardware_id, sizeof(binding->hardware_id), "%s",
                 identity->hardware_id);
        binding->captured_ms = now_ms;
        s_auto_legacy_ready[scanner_id] = legacy_mode;
        if (!legacy_mode) {
            memset(&s_auto_offer_bindings[scanner_id], 0,
                   sizeof(s_auto_offer_bindings[scanner_id]));
        }
    }
    auto_coordinator_unlock();
    if (!ok) {
        return false;
    }
    /* Queue acceptance is the durable NVS transition above. A task-create
     * failure leaves READY_QUEUED recoverable by the next coordinator kick. */
    (void)auto_coordinator_start_worker();
    return true;
}

static bool auto_coordinator_record_scanner_check(
    int scanner_id, uint32_t generation, fw_auto_slot_state_t new_state)
{
    if (scanner_id < 0 || scanner_id >= FW_AUTO_UPDATE_SCANNER_COUNT ||
        !auto_coordinator_lock()) {
        return false;
    }
    uint8_t bit = (uint8_t)(1u << scanner_id);
    if (!s_auto_coordinator_loaded ||
        s_auto_coordinator.generation != generation ||
        !(s_auto_coordinator.target_slot_mask & bit)) {
        auto_coordinator_unlock();
        return false;
    }
    uint8_t current = s_auto_coordinator.slot_state[scanner_id];
    if (auto_coordinator_slot_is_terminal(current)) {
        /* Terminal outcomes are latched for this manifest generation.  Only
         * a newly committed generation may authorize another offer/relay. */
        bool already_recorded = current == (uint8_t)new_state;
        auto_coordinator_unlock();
        (void)auto_coordinator_start_worker();
        return already_recorded;
    }
    if (current == FW_AUTO_SLOT_READY_QUEUED ||
        current == FW_AUTO_SLOT_RELAYING ||
        current == FW_AUTO_SLOT_RECOVERING) {
        /* Once a relay result is durably reserved, scanner-originated boot
         * checks cannot own the terminal outcome. The worker still has to
         * finish post-reboot identity, rollback, role, and radio health proof. */
        auto_coordinator_unlock();
        return true;
    }
    fw_auto_coordinator_blob_t before = s_auto_coordinator;
    s_auto_coordinator.slot_state[scanner_id] = new_state;
    s_auto_coordinator.bound_hardware_id[scanner_id][0] = '\0';
    if (auto_coordinator_slot_is_terminal(new_state)) {
        s_auto_coordinator.pending_mask &= (uint8_t)~bit;
    }
    bool ok = auto_coordinator_save_locked();
    if (!ok) {
        s_auto_coordinator = before;
    } else {
        memset(&s_auto_offer_bindings[scanner_id], 0,
               sizeof(s_auto_offer_bindings[scanner_id]));
        s_auto_legacy_ready[scanner_id] = false;
    }
    auto_coordinator_unlock();
    if (ok && auto_coordinator_slot_is_terminal(new_state)) {
        (void)auto_coordinator_reprompt_requested();
        (void)auto_coordinator_start_worker();
    }
    return ok;
}

static bool auto_coordinator_record_current_identity(
    int scanner_id,
    const fw_store_info_t *info,
    const scanner_identity_snapshot_t *identity,
    const char *scanner_board,
    const char *scanner_version)
{
    if (scanner_id < 0 || scanner_id >= FW_AUTO_UPDATE_SCANNER_COUNT ||
        !info || !identity ||
        !auto_identity_matches_manifest(
            identity, info, scanner_board, scanner_version)) {
        return false;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    fof_auto_identity_view_t identity_view = auto_identity_view(identity);
    if (!auto_coordinator_lock()) {
        return false;
    }
    uint8_t bit = (uint8_t)(1u << scanner_id);
    uint8_t current = s_auto_coordinator.slot_state[scanner_id];
    if (!s_auto_coordinator_loaded ||
        s_auto_coordinator.generation != info->generation ||
        s_auto_coordinator.manifest_crc32 != info->manifest_crc32 ||
        !(s_auto_coordinator.target_slot_mask & bit) ||
        !auto_coordinator_slot_gate_open_locked(scanner_id) ||
        !fof_auto_identity_is_fresh(
            &identity_view,
            s_auto_identity_generation_floor[scanner_id],
            now_ms, FW_AUTO_IDENTITY_MAX_AGE_MS) ||
        (auto_coordinator_slot_is_terminal(current) &&
         current != FW_AUTO_SLOT_CURRENT) ||
        current == FW_AUTO_SLOT_READY_QUEUED ||
        current == FW_AUTO_SLOT_RELAYING ||
        current == FW_AUTO_SLOT_RECOVERING) {
        auto_coordinator_unlock();
        return false;
    }

    if (current == FW_AUTO_SLOT_CURRENT) {
        auto_coordinator_unlock();
        return true;
    }
    fw_auto_coordinator_blob_t before = s_auto_coordinator;
    s_auto_coordinator.slot_state[scanner_id] = FW_AUTO_SLOT_CURRENT;
    s_auto_coordinator.pending_mask &= (uint8_t)~bit;
    s_auto_coordinator.bound_hardware_id[scanner_id][0] = '\0';
    bool ok = auto_coordinator_save_locked();
    if (!ok) {
        s_auto_coordinator = before;
    } else {
        memset(&s_auto_offer_bindings[scanner_id], 0,
               sizeof(s_auto_offer_bindings[scanner_id]));
        s_auto_legacy_ready[scanner_id] = false;
    }
    auto_coordinator_unlock();
    if (ok) {
        (void)auto_coordinator_reprompt_requested();
        (void)auto_coordinator_start_worker();
    }
    return ok;
}

static bool auto_coordinator_record_legacy_offer(
    int scanner_id,
    const fw_store_info_t *info,
    const scanner_identity_snapshot_t *identity,
    const char *check_reason)
{
    if (scanner_id < 0 || scanner_id >= FW_AUTO_UPDATE_SCANNER_COUNT ||
        !info || !identity || !check_reason ||
        strcmp(check_reason, "manual") != 0 ||
        strcmp(identity->version,
               FOF_LEGACY_READY_BOOTSTRAP_VERSION) != 0 ||
        !auto_legacy_ready_authorized(
            info, identity, identity->board, identity->version,
            info->version, info->size, info->checksum)) {
        return false;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    fof_auto_identity_view_t identity_view = auto_identity_view(identity);
    if (!auto_coordinator_lock()) {
        return false;
    }
    uint8_t bit = (uint8_t)(1u << scanner_id);
    uint8_t current = s_auto_coordinator.slot_state[scanner_id];
    if (!s_auto_coordinator_loaded ||
        s_auto_coordinator.generation != info->generation ||
        s_auto_coordinator.manifest_crc32 != info->manifest_crc32 ||
        !(s_auto_coordinator.target_slot_mask & bit) ||
        !auto_coordinator_slot_gate_open_locked(scanner_id) ||
        (current != FW_AUTO_SLOT_AWAITING_CHECK &&
         current != FW_AUTO_SLOT_OFFERED) ||
        !fof_auto_identity_is_fresh(
            &identity_view,
            s_auto_identity_generation_floor[scanner_id],
            now_ms, FW_AUTO_IDENTITY_MAX_AGE_MS)) {
        auto_coordinator_unlock();
        return false;
    }

    fw_auto_coordinator_blob_t before = s_auto_coordinator;
    s_auto_coordinator.slot_state[scanner_id] = FW_AUTO_SLOT_OFFERED;
    s_auto_coordinator.pending_mask &= (uint8_t)~bit;
    s_auto_coordinator.bound_hardware_id[scanner_id][0] = '\0';
    if (!auto_coordinator_save_locked()) {
        s_auto_coordinator = before;
        auto_coordinator_unlock();
        return false;
    }

    fw_auto_offer_binding_t *binding = &s_auto_offer_bindings[scanner_id];
    memset(binding, 0, sizeof(*binding));
    binding->generation = info->generation;
    binding->manifest_crc32 = info->manifest_crc32;
    binding->slot = (uint8_t)scanner_id;
    binding->identity_generation = identity->identity_generation;
    snprintf(binding->hardware_id, sizeof(binding->hardware_id), "%s",
             identity->hardware_id);
    binding->captured_ms = now_ms;
    s_auto_legacy_ready[scanner_id] = false;
    auto_coordinator_unlock();
    return true;
}

typedef enum {
    AUTO_RECOVERY_CHECK_NOT_ACTIVE = 0,
    AUTO_RECOVERY_CHECK_HOLD,
    AUTO_RECOVERY_CHECK_CONVERGED,
    AUTO_RECOVERY_CHECK_REOFFER,
    AUTO_RECOVERY_CHECK_REFUSED,
    AUTO_RECOVERY_CHECK_PERSIST_FAILED,
} auto_recovery_check_result_t;

static auto_recovery_check_result_t
auto_coordinator_handle_recovery_check(
    int scanner_id,
    const fw_store_info_t *info,
    const scanner_identity_snapshot_t *identity,
    bool have_identity,
    const char *scanner_board,
    const char *scanner_version,
    const char *check_reason)
{
    if (scanner_id < 0 || scanner_id >= FW_AUTO_UPDATE_SCANNER_COUNT ||
        !info || !identity || !scanner_board || !scanner_version ||
        !check_reason) {
        return AUTO_RECOVERY_CHECK_NOT_ACTIVE;
    }

    const scanner_info_t *live = scanner_id == 0
        ? uart_rx_get_ble_scanner_info()
        : uart_rx_get_wifi_scanner_info();
    scanner_info_t scanner = {0};
    if (live) {
        scanner = *live;
    }
    bool peer_connected = scanner_id == 0
        ? uart_rx_is_wifi_scanner_connected()
        : uart_rx_is_ble_scanner_connected();
#ifdef FOF_BADGE_VARIANT
    badge_power_state_t power_state = {0};
    badge_power_runtime_snapshot(&power_state);
    bool quiet_expected = power_state.quiet;
    const char *expected_profile = peer_connected
        ? fof_policy_scan_profile_for_slot((uint8_t)scanner_id, false)
        : "hybrid_failover";
#else
    bool quiet_expected = false;
    const char *expected_profile = "hybrid_failover";
#endif

    bool target_contract_matches = have_identity && scanner.received &&
        scanner.identity_generation >= identity->identity_generation &&
        auto_identity_matches_manifest(
            identity, info, scanner_board, scanner_version) &&
        strcmp(scanner.hardware_id, identity->hardware_id) == 0 &&
        strcmp(scanner.version, identity->version) == 0 &&
        strcmp(scanner.board, info->name) == 0 &&
        strcmp(scanner.firmware_name, info->name) == 0 &&
        strcmp(scanner.app_project, info->project) == 0 &&
        strcmp(scanner.hardware_type, info->hardware) == 0;
    bool command_healthy = scanner.cmd_rx_count > 0 &&
        scanner.cmd_last_age_s >= 0 && scanner.cmd_last_age_s <= 45;
    bool profile_healthy = expected_profile &&
        strcmp(scanner.scan_profile, expected_profile) == 0;
    bool radio_healthy = false;
#ifdef FOF_BADGE_VARIANT
    if (quiet_expected) {
        radio_healthy = scanner.quiet_mode &&
            scanner.quiet_uart_commands && !scanner.quiet_tx_enabled &&
            scanner.wifi_paused && !scanner.ble_scanning;
    } else if (!scanner.quiet_mode && scanner.quiet_tx_enabled) {
        if (strcmp(expected_profile, "ble_primary") == 0) {
            radio_healthy = scanner.ble_scanning &&
                scanner.ble_host_active && scanner.ble_host_synced &&
                scanner.wifi_paused;
        } else if (strcmp(expected_profile, "wifi_primary") == 0) {
            radio_healthy = !scanner.ble_scanning && !scanner.wifi_paused;
        } else {
            radio_healthy = scanner.ble_scanning &&
                scanner.ble_host_active && scanner.ble_host_synced &&
                !scanner.wifi_paused;
        }
    }
#else
    (void)quiet_expected;
    (void)peer_connected;
    radio_healthy = scanner.ble_scanning && scanner.ble_host_active &&
        scanner.ble_host_synced && !scanner.wifi_paused;
#endif

    int64_t now_ms = esp_timer_get_time() / 1000;
    fof_auto_identity_view_t identity_view = auto_identity_view(identity);
    if (!auto_coordinator_lock()) {
        return AUTO_RECOVERY_CHECK_HOLD;
    }
    uint8_t bit = (uint8_t)(1u << scanner_id);
    if (!s_auto_coordinator_loaded ||
        s_auto_coordinator.generation != info->generation ||
        s_auto_coordinator.manifest_crc32 != info->manifest_crc32 ||
        !(s_auto_coordinator.target_slot_mask & bit) ||
        s_auto_coordinator.slot_state[scanner_id] !=
            FW_AUTO_SLOT_RECOVERING) {
        auto_coordinator_unlock();
        return AUTO_RECOVERY_CHECK_NOT_ACTIVE;
    }

    fof_auto_recovery_view_t recovery = {
        .manual_probe = strcmp(check_reason, "manual") == 0 &&
            s_auto_recovery_not_before_ms[scanner_id] > 0 &&
            now_ms >= s_auto_recovery_not_before_ms[scanner_id],
        .identity_fresh = fof_auto_identity_is_fresh(
            &identity_view, s_auto_identity_generation_floor[scanner_id],
            now_ms, FW_AUTO_IDENTITY_MAX_AGE_MS),
        .same_hardware_id = have_identity &&
            strcmp(identity->hardware_id,
                   s_auto_coordinator.bound_hardware_id[scanner_id]) == 0 &&
            strcmp(scanner.hardware_id,
                   s_auto_coordinator.bound_hardware_id[scanner_id]) == 0,
        .target_contract_matches = target_contract_matches,
        .rollback_clear = !scanner.rollback_pending,
        .recovery_normal = strcmp(scanner.recovery_mode, "normal") == 0,
        .command_healthy = command_healthy,
        .profile_healthy = profile_healthy,
        .radio_healthy = radio_healthy,
        .version_relation = staged_firmware_version_relation(
            info, scanner_board, scanner_version),
        .source_version = scanner_version,
    };
    fof_auto_recovery_decision_t decision =
        fof_auto_recovery_decide(&recovery);
    if (decision == FOF_AUTO_RECOVERY_HOLD) {
        auto_coordinator_unlock();
        return AUTO_RECOVERY_CHECK_HOLD;
    }

    fw_auto_slot_state_t new_state = FW_AUTO_SLOT_REFUSED;
    auto_recovery_check_result_t result = AUTO_RECOVERY_CHECK_REFUSED;
    if (decision == FOF_AUTO_RECOVERY_CONVERGED) {
        new_state = FW_AUTO_SLOT_CONVERGED;
        result = AUTO_RECOVERY_CHECK_CONVERGED;
    } else if (decision == FOF_AUTO_RECOVERY_REOFFER &&
               s_auto_coordinator.relay_attempts[scanner_id] <
                   FW_AUTO_RELAY_MAX_ATTEMPTS) {
        new_state = FW_AUTO_SLOT_OFFERED;
        result = AUTO_RECOVERY_CHECK_REOFFER;
    } else if (decision == FOF_AUTO_RECOVERY_REOFFER) {
        new_state = FW_AUTO_SLOT_FAILED;
    } else if (decision != FOF_AUTO_RECOVERY_REFUSED) {
        auto_coordinator_unlock();
        return AUTO_RECOVERY_CHECK_HOLD;
    }

    s_auto_coordinator.slot_state[scanner_id] = new_state;
    s_auto_coordinator.pending_mask &= (uint8_t)~bit;
    s_auto_coordinator.bound_hardware_id[scanner_id][0] = '\0';
    if (!auto_coordinator_save_locked()) {
        auto_coordinator_fail_closed_after_save_failure_locked(
            "recovery_result");
        auto_coordinator_unlock();
        return AUTO_RECOVERY_CHECK_PERSIST_FAILED;
    }

    auto_clear_ready_bindings_locked(scanner_id);
    s_auto_recovery_not_before_ms[scanner_id] = 0;
    s_auto_recovery_next_probe_ms[scanner_id] = 0;
    if (result == AUTO_RECOVERY_CHECK_REOFFER) {
        fw_auto_offer_binding_t *binding =
            &s_auto_offer_bindings[scanner_id];
        binding->generation = info->generation;
        binding->manifest_crc32 = info->manifest_crc32;
        binding->slot = (uint8_t)scanner_id;
        binding->identity_generation = identity->identity_generation;
        snprintf(binding->hardware_id, sizeof(binding->hardware_id), "%s",
                 identity->hardware_id);
        binding->captured_ms = now_ms;
    }
    auto_coordinator_unlock();
    return result;
}

bool fw_store_restore_auto_update_coordinator(void)
{
    fw_store_info_t info = {0};
    uint32_t identity_floors[FW_AUTO_UPDATE_SCANNER_COUNT] = {0};
    if (!fw_store_get_info(&info) || !info.stored) {
        ESP_LOGI(TAG, "No staged manifest; automatic-update coordinator idle");
        return true;
    }
    /* UART RX is already running at restore. Copy its small synchronized
     * identity snapshots before ever taking the coordinator mutex. */
    auto_capture_identity_floors(identity_floors);

    uint8_t serialized_blob[NVS_CONFIG_MAX_BLOB_SIZE] = {0};
    fw_auto_coordinator_blob_t blob = {0};
    size_t blob_size = 0;
    uint32_t manifest_crc32 = info.manifest_crc32;
    nvs_config_blob_read_status_t read_status = nvs_config_read_blob(
        NVS_FW_COORDINATOR, serialized_blob, sizeof(serialized_blob),
        &blob_size);
    if (read_status == NVS_CONFIG_BLOB_MISSING) {
        /* The committed manifest CRC includes its exact requested slot mask.
         * That makes the manifest a safe recovery journal for the tiny power-
         * loss window between manifest commit and coordinator commit. Rebuild
         * only an awaiting-check queue; every scanner still has to prove its
         * version and immutable identity before any relay is authorized. */
        ESP_LOGW(TAG,
                 "Coordinator missing; "
                 "rebuilding generation %lu mask=0x%02x from committed manifest",
                 (unsigned long)info.generation,
                 (unsigned)info.target_slot_mask);

        const esp_partition_t *partition = find_fw_partition();
        fw_store_info_t verified_info = {0};
        char validation_error[48] = {0};
        if (!partition ||
            !validate_staged_image(partition, info.size, info.checksum,
                                   info.name, info.version, info.sha256,
                                   &verified_info, validation_error,
                                   sizeof(validation_error))) {
            ESP_LOGE(TAG,
                     "Committed manifest image is not actionable during "
                     "coordinator recovery: %s",
                     validation_error[0] ? validation_error
                                         : "partition_missing");
            (void)invalidate_fw_metadata();
            (void)auto_coordinator_initialize_fail_closed(
                info.generation, manifest_crc32);
            auto_coordinator_release_excluded_slots();
            return true;
        }
        bool initialized = auto_coordinator_begin_generation(
            info.generation, info.target_slot_mask, manifest_crc32);
        if (!initialized) {
            (void)auto_coordinator_initialize_fail_closed(
                info.generation, manifest_crc32);
        }
        auto_coordinator_release_excluded_slots();
        if (!initialized) {
            return false;
        }
        (void)auto_coordinator_reprompt_requested();
        return auto_coordinator_start_worker();
    }
    if (read_status != NVS_CONFIG_BLOB_PRESENT) {
        ESP_LOGE(TAG,
                 "Coordinator record read failed; refusing manifest-journal "
                 "rebuild for generation %lu",
                 (unsigned long)info.generation);
        return false;
    }

    bool migrated_from_v2 = false;
    bool valid = false;
    if (blob_size == sizeof(fof_fw_coord_v2_t)) {
        fof_fw_coord_v3_t migrated_blob = {0};
        valid = fof_fw_coordinator_migrate_v2(
            serialized_blob, blob_size, info.generation, manifest_crc32,
            info.target_slot_mask, &migrated_blob);
        if (valid) {
            blob = migrated_blob;
            migrated_from_v2 = true;
        }
    } else if (blob_size == sizeof(blob)) {
        memcpy(&blob, serialized_blob, sizeof(blob));
        valid = auto_coordinator_blob_valid(&blob) &&
            blob.generation == info.generation &&
            blob.manifest_crc32 == manifest_crc32 &&
            (blob.fail_closed ||
             blob.target_slot_mask == info.target_slot_mask);
    }
    if (!valid) {
        ESP_LOGE(TAG,
                 "Present coordinator record is corrupt, unknown, or does "
                 "not match staged generation %lu; refusing fresh budgets",
                 (unsigned long)info.generation);
        return false;
    }

    if (!auto_coordinator_lock()) {
        return false;
    }
    s_auto_coordinator = blob;
    s_auto_coordinator_loaded = true;
    s_auto_relay_worker_running = false;
    if (migrated_from_v2) {
        if (!auto_coordinator_save_locked()) {
            s_auto_coordinator_loaded = false;
            memset(&s_auto_coordinator, 0, sizeof(s_auto_coordinator));
            auto_coordinator_unlock();
            ESP_LOGE(TAG,
                     "Schema 2 coordinator migration could not be committed; "
                     "aborting restore without scanner side effects");
            return false;
        }
    }
    auto_set_identity_floors_locked(identity_floors);
    bool changed = false;
    uint8_t recovery_mask = 0;
    for (int scanner_id = 0;
         !migrated_from_v2 && scanner_id < FW_AUTO_UPDATE_SCANNER_COUNT;
         ++scanner_id) {
        uint8_t bit = (uint8_t)(1u << scanner_id);
        if (!(blob.target_slot_mask & bit)) {
            continue;
        }
        if (s_auto_coordinator.slot_state[scanner_id] ==
            FW_AUTO_SLOT_RELAYING) {
            /* Preserve both the consumed attempt and bound immutable MAC.
             * Task 4 owns the abort/cooldown/probe recovery worker; Task 3
             * only makes that interrupted ownership durable and distinct. */
            s_auto_coordinator.slot_state[scanner_id] =
                FW_AUTO_SLOT_RECOVERING;
            s_auto_coordinator.pending_mask &= (uint8_t)~bit;
            s_auto_coordinator.readiness_probe_attempts[scanner_id] = 0;
            recovery_mask |= bit;
            ESP_LOGW(TAG,
                     "Scanner[%d] interrupted relay restored as recovering "
                     "for bound MAC %s",
                     scanner_id,
                     s_auto_coordinator.bound_hardware_id[scanner_id]);
            changed = true;
        } else if (s_auto_coordinator.slot_state[scanner_id] ==
                       FW_AUTO_SLOT_RECOVERING) {
            recovery_mask |= bit;
        } else if (
                   !auto_coordinator_slot_is_terminal(
                       s_auto_coordinator.slot_state[scanner_id])) {
            /* READY/OFFERED are volatile scanner-side facts. A reboot may
             * have cleared the scanner's readiness latch, so every durable
             * nonterminal state is re-authorized from an exact version check. */
            s_auto_coordinator.slot_state[scanner_id] =
                FW_AUTO_SLOT_AWAITING_CHECK;
            s_auto_coordinator.pending_mask &= (uint8_t)~bit;
            s_auto_coordinator.readiness_probe_attempts[scanner_id] = 0;
            s_auto_coordinator.bound_hardware_id[scanner_id][0] = '\0';
            changed = true;
        }
    }
    bool saved = !changed || auto_coordinator_save_locked();
    auto_coordinator_unlock();
    if (!saved) {
        return false;
    }
    auto_begin_recovery_after_restore(recovery_mask);

    ESP_LOGW(TAG,
             "Restored scanner-update coordinator generation=%lu mask=0x%02x",
             (unsigned long)blob.generation, blob.target_slot_mask);
    auto_coordinator_release_excluded_slots();
    (void)auto_coordinator_reprompt_requested();
    return auto_coordinator_start_worker();
}

void fw_store_handle_scanner_check(int scanner_id,
                                   const char *scanner_board,
                                   const char *scanner_version,
                                   const char *check_reason)
{
    fw_store_info_t info = {0};
    scanner_identity_snapshot_t identity = {0};
    bool have_identity = uart_rx_get_scanner_identity_snapshot(
        scanner_id, &identity);
    if (scanner_id < 0 || scanner_id >= FW_AUTO_UPDATE_SCANNER_COUNT ||
        !fw_store_get_info(&info)) {
        send_fw_offer(scanner_id, false, NULL, "no_staged_firmware");
        return;
    }
    if (!auto_coordinator_slot_requested(scanner_id, info.generation)) {
        ESP_LOGI(TAG,
                 "Scanner[%d] firmware check ignored: slot excluded from "
                 "staged generation %lu",
                 scanner_id, (unsigned long)info.generation);
        return;
    }
    if (!auto_coordinator_slot_gate_open(scanner_id, info.generation)) {
        ESP_LOGI(TAG,
                 "Scanner[%d] firmware check deferred until the BLE-primary "
                 "slot reaches a durable terminal state",
                 scanner_id);
        return;
    }
    if (!info.name[0] ||
        !staged_firmware_matches_scanner(&info, scanner_board)) {
        (void)auto_coordinator_record_scanner_check(
            scanner_id, info.generation, FW_AUTO_SLOT_REFUSED);
        send_fw_offer(scanner_id, false, &info,
                      info.name[0] ? "board_mismatch"
                                   : "staged_firmware_missing_name");
        return;
    }

    fof_firmware_version_relation_t relation =
        staged_firmware_version_relation(&info, scanner_board,
                                         scanner_version);
    auto_recovery_check_result_t recovery_result =
        auto_coordinator_handle_recovery_check(
            scanner_id, &info, &identity, have_identity,
            scanner_board, scanner_version, check_reason);
    if (recovery_result != AUTO_RECOVERY_CHECK_NOT_ACTIVE) {
        if (recovery_result == AUTO_RECOVERY_CHECK_REOFFER) {
            send_fw_offer(scanner_id, true, &info,
                          "staged_update_available");
        } else if (recovery_result == AUTO_RECOVERY_CHECK_CONVERGED) {
            send_fw_offer(scanner_id, false, &info, "recovered_converged");
            (void)auto_coordinator_reprompt_requested();
            (void)auto_coordinator_start_worker();
        } else if (recovery_result == AUTO_RECOVERY_CHECK_REFUSED) {
            send_fw_offer(scanner_id, false, &info, "recovery_refused");
            (void)auto_coordinator_reprompt_requested();
            (void)auto_coordinator_start_worker();
        } else if (recovery_result ==
                   AUTO_RECOVERY_CHECK_PERSIST_FAILED) {
            send_fw_offer(scanner_id, false, &info,
                          "coordinator_unavailable");
        } else {
            send_fw_offer(scanner_id, false, &info,
                          "recovery_proof_required");
        }
        return;
    }
    if (relation == FOF_VERSION_NEWER) {
        if (scanner_version &&
            strcmp(scanner_version,
                   FOF_LEGACY_READY_BOOTSTRAP_VERSION) == 0) {
            if (!have_identity ||
                !auto_identity_matches_manifest(
                    &identity, &info, scanner_board, scanner_version) ||
                !auto_coordinator_record_legacy_offer(
                    scanner_id, &info, &identity, check_reason)) {
                ESP_LOGW(TAG,
                         "Scanner[%d] exact legacy source not offered: "
                         "requires a post-floor complete manual identity",
                         scanner_id);
                send_fw_offer(scanner_id, false, &info,
                              "legacy_manual_identity_required");
                return;
            }
            send_fw_offer(scanner_id, true, &info,
                          "staged_update_available");
            return;
        }
        if (!auto_coordinator_record_scanner_check(
                scanner_id, info.generation, FW_AUTO_SLOT_OFFERED)) {
            send_fw_offer(scanner_id, false, &info, "coordinator_unavailable");
            return;
        }
        send_fw_offer(scanner_id, true, &info, "staged_update_available");
        return;
    }
    if (relation == FOF_VERSION_EQUAL) {
        if (!have_identity ||
            !auto_coordinator_record_current_identity(
                scanner_id, &info, &identity,
                scanner_board, scanner_version)) {
            send_fw_offer(scanner_id, false, &info,
                          "current_identity_required");
            return;
        }
        send_fw_offer(scanner_id, false, &info, "current");
        return;
    }
    if (relation == FOF_VERSION_OLDER) {
        if (!auto_coordinator_record_scanner_check(
                scanner_id, info.generation,
                FW_AUTO_SLOT_NEWER_SKIPPED)) {
            send_fw_offer(scanner_id, false, &info,
                          "coordinator_unavailable");
            return;
        }
        send_fw_offer(scanner_id, false, &info, "newer_skipped");
        return;
    }

    (void)auto_coordinator_record_scanner_check(
        scanner_id, info.generation, FW_AUTO_SLOT_REFUSED);
    send_fw_offer(scanner_id, false, &info,
                  firmware_relation_refusal_reason(relation));
}

bool fw_store_handle_scanner_ready(int scanner_id,
                                   const char *scanner_board,
                                   const char *scanner_version,
                                   const char *target_version,
                                   const char *target_name,
                                   const char *target_project,
                                   const char *target_hardware,
                                   const char *target_sha256,
                                   uint32_t target_generation,
                                   uint32_t target_size,
                                   uint32_t target_crc32)
{
    fw_store_info_t info = {0};
    scanner_identity_snapshot_t identity = {0};
    bool have_identity = uart_rx_get_scanner_identity_snapshot(
        scanner_id, &identity);
    if (scanner_id < 0 || scanner_id >= FW_AUTO_UPDATE_SCANNER_COUNT) {
        return false;
    }
    if (!auto_coordinator_slot_gate_open(scanner_id, target_generation)) {
        ESP_LOGI(TAG,
                 "Scanner[%d] fw_ready deferred by BLE-first slot gate",
                 scanner_id);
        return false;
    }
    if (!have_identity || !fw_store_get_info(&info) ||
        !auto_coordinator_slot_requested(scanner_id, target_generation) ||
        !info.name[0] ||
        !staged_firmware_is_newer_for_scanner(&info, scanner_board,
                                              scanner_version) ||
        !target_version || strcmp(target_version, info.version) != 0 ||
        !target_name || strcmp(target_name, info.name) != 0 ||
        !target_project || strcmp(target_project, info.project) != 0 ||
        !target_hardware || strcmp(target_hardware, info.hardware) != 0 ||
        !target_sha256 || strcmp(target_sha256, info.sha256) != 0 ||
        target_generation != info.generation ||
        target_size != info.size || target_crc32 != info.checksum ||
        !auto_identity_matches_manifest(
            &identity, &info, scanner_board, scanner_version)) {
        ESP_LOGW(TAG,
                 "Scanner[%d] fw_ready ignored: manifest/generation/scope "
                 "mismatch current=%s target=%s sha=%s generation=%lu",
                 scanner_id,
                 scanner_version ? scanner_version : "?",
                 target_version ? target_version : "?",
                 target_sha256 ? target_sha256 : "?",
                 (unsigned long)target_generation);
        return false;
    }
    if (!enqueue_auto_relay(scanner_id, target_generation,
                            info.manifest_crc32, &identity, false)) {
        ESP_LOGW(TAG,
                 "Scanner[%d] fw_ready refused: durable queue unavailable or "
                 "terminal for generation %lu",
                 scanner_id, (unsigned long)target_generation);
        return false;
    }
    ESP_LOGW(TAG,
             "Scanner[%d] fw_ready durably queued for generation %lu",
             scanner_id, (unsigned long)target_generation);
    return true;
}

bool fw_store_handle_legacy_scanner_ready(int scanner_id,
                                          const char *scanner_board,
                                          const char *scanner_version,
                                          const char *target_version,
                                          uint32_t target_size,
                                          uint32_t target_crc32)
{
    fw_store_info_t info = {0};
    scanner_identity_snapshot_t identity = {0};
    bool have_identity = uart_rx_get_scanner_identity_snapshot(
        scanner_id, &identity);
    if (scanner_id < 0 || scanner_id >= FW_AUTO_UPDATE_SCANNER_COUNT ||
        !have_identity || !fw_store_get_info(&info) ||
        !auto_legacy_ready_authorized(
            &info, &identity, scanner_board, scanner_version,
            target_version, target_size, target_crc32)) {
        ESP_LOGW(TAG,
                 "Scanner[%d] legacy fw_ready refused by exact identity and "
                 "manifest policy",
                 scanner_id);
        return false;
    }
    if (!enqueue_auto_relay(scanner_id, info.generation,
                            info.manifest_crc32, &identity, true)) {
        ESP_LOGW(TAG,
                 "Scanner[%d] legacy fw_ready refused: offered binding or "
                 "durable queue unavailable for generation %lu",
                 scanner_id, (unsigned long)info.generation);
        return false;
    }
    ESP_LOGW(TAG,
             "Scanner[%d] legacy fw_ready durably queued for generation %lu "
             "bound MAC %s",
             scanner_id, (unsigned long)info.generation,
             identity.hardware_id);
    return true;
}

/* ── GET /api/fw/info — check stored firmware status ─────────────────────── */

static esp_err_t fw_info_handler(httpd_req_t *req)
{
    fw_store_info_t info = {0};
    bool has_fw = read_fw_metadata(&info);

    int64_t now_ms = esp_timer_get_time() / 1000;
    enum { FW_INFO_RESP_LEN = 2200 };
    char *resp = (char *)psram_alloc(FW_INFO_RESP_LEN);
    if (!resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Firmware info alloc failed");
        return ESP_OK;
    }
    if (has_fw) {
        snprintf(resp, FW_INFO_RESP_LEN,
                 "{\"stored\":true,\"size\":%lu,\"checksum\":%lu,"
                 "\"version\":\"%s\",\"name\":\"%s\",\"project\":\"%s\","
                 "\"hardware\":\"%s\",\"sha256\":\"%s\","
                 "\"generation\":%lu,\"partition\":\"%s\","
                 "\"auto_update_enabled\":true,\"auto_relay_cooldown_s\":%d,"
                 "\"last_relay\":{\"ble\":{\"ok\":%s,\"stage\":\"%s\",\"error\":\"%s\","
                 "\"size\":%lu,\"bytes\":%lu,"
                 "\"chunks\":%d,\"nacks\":%d,\"retries\":%d,\"elapsed_s\":%lld,\"age_s\":%lld,"
                 "\"cmd_rx_before\":%lu,\"cmd_rx_after\":%lu,"
                 "\"fw_check_before\":%lu,\"fw_check_after\":%lu,"
                 "\"cmd_age_after_s\":%lld,\"scanner_ver\":\"%s\","
                 "\"scanner_fw_state\":\"%s\"},"
                 "\"wifi\":{\"ok\":%s,\"stage\":\"%s\",\"error\":\"%s\","
                 "\"size\":%lu,\"bytes\":%lu,"
                 "\"chunks\":%d,\"nacks\":%d,\"retries\":%d,\"elapsed_s\":%lld,\"age_s\":%lld,"
                 "\"cmd_rx_before\":%lu,\"cmd_rx_after\":%lu,"
                 "\"fw_check_before\":%lu,\"fw_check_after\":%lu,"
                 "\"cmd_age_after_s\":%lld,\"scanner_ver\":\"%s\","
                 "\"scanner_fw_state\":\"%s\"}}}",
                 (unsigned long)info.size, (unsigned long)info.checksum,
                 info.version, info.name, info.project, info.hardware, info.sha256,
                 (unsigned long)info.generation, info.partition,
                 0,
                 s_last_relay[0].ok ? "true" : "false",
                 s_last_relay[0].stage, s_last_relay[0].error,
                 (unsigned long)s_last_relay[0].size,
                 (unsigned long)s_last_relay[0].bytes,
                 s_last_relay[0].chunks, s_last_relay[0].nacks, s_last_relay[0].retries,
                 (long long)s_last_relay[0].elapsed_s,
                 (long long)(s_last_relay[0].finished_ms > 0 ? (now_ms - s_last_relay[0].finished_ms) / 1000 : -1),
                 (unsigned long)s_last_relay[0].cmd_rx_before,
                 (unsigned long)s_last_relay[0].cmd_rx_after,
                 (unsigned long)s_last_relay[0].fw_check_before,
                 (unsigned long)s_last_relay[0].fw_check_after,
                 (long long)s_last_relay[0].cmd_age_after_s,
                 s_last_relay[0].scanner_version,
                 s_last_relay[0].scanner_fw_state,
                 s_last_relay[1].ok ? "true" : "false",
                 s_last_relay[1].stage, s_last_relay[1].error,
                 (unsigned long)s_last_relay[1].size,
                 (unsigned long)s_last_relay[1].bytes,
                 s_last_relay[1].chunks, s_last_relay[1].nacks, s_last_relay[1].retries,
                 (long long)s_last_relay[1].elapsed_s,
                 (long long)(s_last_relay[1].finished_ms > 0 ? (now_ms - s_last_relay[1].finished_ms) / 1000 : -1),
                 (unsigned long)s_last_relay[1].cmd_rx_before,
                 (unsigned long)s_last_relay[1].cmd_rx_after,
                 (unsigned long)s_last_relay[1].fw_check_before,
                 (unsigned long)s_last_relay[1].fw_check_after,
                 (long long)s_last_relay[1].cmd_age_after_s,
                 s_last_relay[1].scanner_version,
                 s_last_relay[1].scanner_fw_state);
    } else {
        snprintf(resp, FW_INFO_RESP_LEN,
                 "{\"stored\":false,\"auto_update_enabled\":true,"
                 "\"auto_relay_cooldown_s\":%d}",
                 0);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    psram_free(resp);
    return ESP_OK;
}

/* ── POST /api/fw/trigger — ask scanner(s) to run fw_check now ─────────── */

static esp_err_t fw_trigger_handler(httpd_req_t *req)
{
    char query[96] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char uart_target[8] = "both";
    httpd_query_key_value(query, "uart", uart_target, sizeof(uart_target));

    bool target_ble = strcmp(uart_target, "ble") == 0 || strcmp(uart_target, "both") == 0;
    bool target_wifi = strcmp(uart_target, "wifi") == 0 || strcmp(uart_target, "both") == 0;
    if (!target_ble && !target_wifi) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "uart must be ble, wifi, or both");
        return ESP_OK;
    }

    const char *cmd = "{\"type\":\"fw_check_now\"}";
    bool ble_sent = target_ble ? uart_rx_send_command_to_scanner_checked(0, cmd) : false;
    bool wifi_sent = target_wifi ? uart_rx_send_command_to_scanner_checked(1, cmd) : false;
    bool ok = (!target_ble || ble_sent) && (!target_wifi || wifi_sent);

    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"ok\":%s,\"uart\":\"%s\",\"ble_sent\":%s,\"wifi_sent\":%s,"
             "\"error\":\"%s\"}",
             ok ? "true" : "false",
             uart_target,
             ble_sent ? "true" : "false",
             wifi_sent ? "true" : "false",
             ok ? "" : "scanner_command_ingress_unreachable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ── Registration ────────────────────────────────────────────────────────── */

static const httpd_uri_t uri_fw_upload = {
    .uri = "/api/fw/upload", .method = HTTP_POST, .handler = fw_upload_handler,
};
static const httpd_uri_t uri_fw_relay = {
    .uri = "/api/fw/relay", .method = HTTP_POST, .handler = fw_relay_handler,
};
static const httpd_uri_t uri_fw_info = {
    .uri = "/api/fw/info", .method = HTTP_GET, .handler = fw_info_handler,
};
static const httpd_uri_t uri_fw_trigger = {
    .uri = "/api/fw/trigger", .method = HTTP_POST, .handler = fw_trigger_handler,
};

void fw_store_register(httpd_handle_t server)
{
    esp_err_t r;
    r = httpd_register_uri_handler(server, &uri_fw_info);
    if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/fw/info: %s", esp_err_to_name(r));
#ifndef FOF_BADGE_VARIANT
    r = httpd_register_uri_handler(server, &uri_fw_upload);
    if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/fw/upload: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_fw_relay);
    if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/fw/relay: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_fw_trigger);
    if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/fw/trigger: %s", esp_err_to_name(r));
    ESP_LOGI(TAG, "Firmware store endpoints registered");
#else
    ESP_LOGI(TAG,
             "Badge firmware transport is USB staging + UART relay only; "
             "HTTP firmware mutations are not registered");
#endif
}
