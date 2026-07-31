#include "unity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "badge_usb_health_policy.h"
#include "badge_usb_stream.h"
#include "badge_usb_transport_policy.h"
#include "serial_config.h"
#include "serial_config_ingress.h"

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))
#define WIRE_FIXTURE(text)                                                   \
    {(const uint8_t *)(text), sizeof(text) - 1U}

typedef struct {
    const uint8_t *bytes;
    size_t length;
} wire_fixture_t;

typedef struct {
    uint64_t rx_bytes;
    uint32_t malformed_lines;
    uint32_t recognized_health_updates;
    uint32_t valid_commands;
    uint32_t usb_liveness_notes;
    uint32_t responses_completed;
    uint32_t required_response_failures;
    uint32_t hard_unanswered_required_responses;
    uint32_t enqueued_required_responses;
    uint32_t dropped_progress_frames;
    uint32_t dropped_optional_frames;
    uint32_t handler_calls;
    uint32_t recovery_executor_calls;
    uint32_t booting_rejections;
    uint32_t recovery_only_rejections;
    uint32_t unknown_rejections;
    uint32_t parser_error_rejections;
    uint32_t nvs_writes;
    uint32_t reset_calls;
    uint32_t allocation_calls;
    uint32_t token_acquisitions;
    uint32_t uart_writes;
    uint32_t binary_mode_entries;
    uint32_t partition_writes;
    uint32_t ota_writes;
    uint32_t ack_emissions;
    uint32_t reboot_requests;
    uint32_t rejection_frames;
    int64_t now_ms;
    int64_t last_rx_ms;
    int64_t last_command_ms;
    int64_t last_response_ms;
    int64_t oldest_hard_unanswered_response_ms;
    int64_t oldest_enqueued_response_ms;
    badge_usb_emit_result_t dispatch_emit_result;
    badge_usb_emit_result_t rejection_emit_result;
    badge_usb_control_schema_id_t last_control_schema_id;
    badge_usb_control_handler_kind_t last_control_handler_kind;
    uint8_t last_line[2048];
    size_t last_line_length;
} ingress_effects_t;

typedef struct {
    badge_usb_stream_t stream;
    char line[2048];
    ingress_effects_t effects;
    bool dispatch_ready;
    bool recovery_only;
} ingress_harness_t;

static bool normal_line_is_recognized(void *context,
                                      const uint8_t *line,
                                      size_t line_byte_len)
{
    (void)context;
    return serial_config_line_is_recognized(line, line_byte_len);
}

static bool recovery_line_is_allowed(void *context,
                                     const uint8_t *line,
                                     size_t line_byte_len)
{
    (void)context;
    return serial_config_recovery_command_classify(line, line_byte_len) !=
           SERIAL_CONFIG_RECOVERY_DENIED;
}

static void note_recognized(void *context)
{
    ingress_effects_t *effects = context;
    effects->recognized_health_updates++;
    effects->valid_commands++;
    effects->last_rx_ms = effects->now_ms;
    effects->last_command_ms = effects->now_ms;
}

static void model_note_required_failure(
    ingress_effects_t *effects, badge_usb_emit_result_t result)
{
    effects->required_response_failures++;
    if (result == BADGE_USB_EMIT_ENQUEUED) {
        effects->enqueued_required_responses++;
        if (effects->oldest_enqueued_response_ms < 0 ||
            effects->now_ms < effects->oldest_enqueued_response_ms) {
            effects->oldest_enqueued_response_ms = effects->now_ms;
        }
        return;
    }
    effects->hard_unanswered_required_responses++;
    if (effects->oldest_hard_unanswered_response_ms < 0 ||
        effects->now_ms < effects->oldest_hard_unanswered_response_ms) {
        effects->oldest_hard_unanswered_response_ms = effects->now_ms;
    }
}

static void model_apply_emit(
    ingress_effects_t *effects,
    badge_usb_emit_result_t result,
    badge_usb_frame_priority_t priority,
    badge_usb_emit_health_mode_t health_mode)
{
    badge_usb_emit_health_effect_t effect =
        badge_usb_emit_health_effect_decide(result, priority, health_mode);
    if (effect == BADGE_USB_EMIT_HEALTH_EFFECT_COMPLETED) {
        if (effects->enqueued_required_responses > 0U) {
            effects->responses_completed +=
                effects->enqueued_required_responses;
            effects->enqueued_required_responses = 0U;
            effects->oldest_enqueued_response_ms = -1;
            effects->last_response_ms = effects->now_ms;
        }
        if (priority == BADGE_USB_FRAME_REQUIRED) {
            effects->responses_completed++;
            effects->last_response_ms = effects->now_ms;
        }
    } else if (effect ==
               BADGE_USB_EMIT_HEALTH_EFFECT_REQUIRED_ENQUEUED ||
               effect ==
               BADGE_USB_EMIT_HEALTH_EFFECT_REQUIRED_HARD_FAILURE) {
        model_note_required_failure(effects, result);
    } else if (effect ==
               BADGE_USB_EMIT_HEALTH_EFFECT_PROGRESS_DROP) {
        effects->dropped_progress_frames++;
    } else if (effect ==
               BADGE_USB_EMIT_HEALTH_EFFECT_OPTIONAL_DROP) {
        effects->dropped_optional_frames++;
    }
}

static bool model_emit(
    ingress_effects_t *effects,
    badge_usb_emit_result_t result,
    badge_usb_frame_priority_t priority,
    badge_usb_emit_health_mode_t health_mode)
{
    model_apply_emit(effects, result, priority, health_mode);
    return result == BADGE_USB_EMIT_COMPLETED ||
           (result == BADGE_USB_EMIT_ENQUEUED &&
            priority != BADGE_USB_FRAME_REQUIRED);
}

static bool emit_rejection_common(ingress_effects_t *effects)
{
    effects->rejection_frames++;
    return model_emit(
        effects, effects->rejection_emit_result,
        BADGE_USB_FRAME_REQUIRED, BADGE_USB_EMIT_HEALTH_NEUTRAL);
}

static bool emit_booting(void *context)
{
    ingress_effects_t *effects = context;
    effects->booting_rejections++;
    return emit_rejection_common(effects);
}

static bool emit_recovery_only(void *context)
{
    ingress_effects_t *effects = context;
    effects->recovery_only_rejections++;
    return emit_rejection_common(effects);
}

static bool emit_unknown(void *context)
{
    ingress_effects_t *effects = context;
    effects->unknown_rejections++;
    return emit_rejection_common(effects);
}

static bool dispatch_with_all_effects(ingress_effects_t *effects,
                                      const uint8_t *line,
                                      size_t line_byte_len,
                                      bool recovery)
{
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(sizeof(effects->last_line), line_byte_len);
    serial_config_ingress_result_t ingress = {0};
    if (recovery) {
        serial_config_recovery_command_t recovery_command =
            serial_config_recovery_command_classify(
                line, line_byte_len);
        TEST_ASSERT_NOT_EQUAL(
            SERIAL_CONFIG_RECOVERY_DENIED, recovery_command);
        if (recovery_command != SERIAL_CONFIG_RECOVERY_ROM_BOOT) {
            TEST_ASSERT_TRUE(serial_config_ingress_authorize(
                line, line_byte_len, &ingress));
        }
    } else {
        TEST_ASSERT_TRUE(serial_config_ingress_authorize(
            line, line_byte_len, &ingress));
    }
    effects->last_control_schema_id = ingress.control_schema_id;
    effects->last_control_handler_kind = ingress.control_handler_kind;
    memcpy(effects->last_line, line, line_byte_len);
    effects->last_line_length = line_byte_len;
    effects->handler_calls++;
    effects->usb_liveness_notes++;
    effects->recovery_executor_calls += recovery ? 1U : 0U;
    effects->nvs_writes++;
    effects->reset_calls++;
    effects->allocation_calls++;
    effects->token_acquisitions++;
    effects->uart_writes++;
    effects->binary_mode_entries++;
    effects->partition_writes++;
    effects->ota_writes++;
    effects->ack_emissions++;
    effects->reboot_requests++;
    return model_emit(
        effects, effects->dispatch_emit_result,
        BADGE_USB_FRAME_REQUIRED, BADGE_USB_EMIT_HEALTH_TRACKED);
}

static bool dispatch_normal(void *context,
                            const uint8_t *line,
                            size_t line_byte_len)
{
    return dispatch_with_all_effects(context, line, line_byte_len, false);
}

static bool dispatch_recovery(void *context,
                              const uint8_t *line,
                              size_t line_byte_len)
{
    return dispatch_with_all_effects(context, line, line_byte_len, true);
}

static badge_usb_line_dispatch_hooks_t ingress_hooks(ingress_effects_t *effects)
{
    return (badge_usb_line_dispatch_hooks_t) {
        .context = effects,
        .normal_line_is_recognized = normal_line_is_recognized,
        .recovery_line_is_allowed = recovery_line_is_allowed,
        .note_recognized = note_recognized,
        .emit_booting = emit_booting,
        .emit_recovery_only = emit_recovery_only,
        .emit_unknown = emit_unknown,
        .dispatch_normal_line = dispatch_normal,
        .dispatch_recovery_line = dispatch_recovery,
    };
}

static void harness_init(ingress_harness_t *harness,
                         bool dispatch_ready,
                         bool recovery_only)
{
    memset(harness, 0, sizeof(*harness));
    badge_usb_stream_init(&harness->stream, harness->line,
                          sizeof(harness->line));
    harness->dispatch_ready = dispatch_ready;
    harness->recovery_only = recovery_only;
    harness->effects.now_ms = 1000;
    harness->effects.last_rx_ms = -1;
    harness->effects.last_command_ms = -1;
    harness->effects.last_response_ms = -1;
    harness->effects.oldest_hard_unanswered_response_ms = -1;
    harness->effects.oldest_enqueued_response_ms = -1;
    harness->effects.dispatch_emit_result = BADGE_USB_EMIT_COMPLETED;
    harness->effects.rejection_emit_result = BADGE_USB_EMIT_COMPLETED;
}

static void harness_feed_chunk(ingress_harness_t *harness,
                               const uint8_t *bytes,
                               size_t length)
{
    harness->effects.rx_bytes += length;
    size_t offset = 0U;
    while (offset < length) {
        badge_usb_stream_result_t result;
        badge_usb_stream_event_t event = badge_usb_stream_feed(
            &harness->stream, bytes + offset, length - offset, 1U, &result);
        TEST_ASSERT_GREATER_THAN_UINT(0U, result.input_consumed);
        offset += result.input_consumed;
        if (event == BADGE_USB_EVENT_LINE) {
            badge_usb_line_dispatch_hooks_t hooks =
                ingress_hooks(&harness->effects);
            (void)badge_usb_line_dispatch_run(
                (const uint8_t *)result.line, result.line_byte_len,
                harness->dispatch_ready, harness->recovery_only, &hooks);
        } else if (event == BADGE_USB_EVENT_ERROR) {
            harness->effects.malformed_lines++;
            harness->effects.parser_error_rejections++;
            (void)emit_rejection_common(&harness->effects);
        }
    }
}

static void harness_feed_wire(ingress_harness_t *harness,
                              const uint8_t *bytes,
                              size_t length)
{
    harness_feed_chunk(harness, bytes, length);
}

static void assert_no_protected_effects(const ingress_effects_t *effects)
{
    TEST_ASSERT_EQUAL_UINT32(0U, effects->recognized_health_updates);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->valid_commands);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->usb_liveness_notes);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->responses_completed);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->required_response_failures);
    TEST_ASSERT_EQUAL_UINT32(
        0U, effects->hard_unanswered_required_responses);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->enqueued_required_responses);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->dropped_progress_frames);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->dropped_optional_frames);
    TEST_ASSERT_EQUAL_INT64(-1, effects->last_rx_ms);
    TEST_ASSERT_EQUAL_INT64(-1, effects->last_command_ms);
    TEST_ASSERT_EQUAL_INT64(-1, effects->last_response_ms);
    TEST_ASSERT_EQUAL_INT64(
        -1, effects->oldest_hard_unanswered_response_ms);
    TEST_ASSERT_EQUAL_INT64(-1, effects->oldest_enqueued_response_ms);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->handler_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->recovery_executor_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->nvs_writes);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->reset_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->allocation_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->token_acquisitions);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->uart_writes);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->binary_mode_entries);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->partition_writes);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->ota_writes);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->ack_emissions);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->reboot_requests);
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_SCHEMA_NONE,
        effects->last_control_schema_id);
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_HANDLER_NONE,
        effects->last_control_handler_kind);
    TEST_ASSERT_EQUAL_UINT(0U, effects->last_line_length);
}

static void assert_one_normal_dispatch(const ingress_effects_t *effects)
{
    TEST_ASSERT_EQUAL_UINT32(1U, effects->recognized_health_updates);
    TEST_ASSERT_EQUAL_UINT32(1U, effects->valid_commands);
    TEST_ASSERT_EQUAL_UINT32(1U, effects->usb_liveness_notes);
    TEST_ASSERT_EQUAL_INT64(effects->now_ms, effects->last_rx_ms);
    TEST_ASSERT_EQUAL_INT64(effects->now_ms, effects->last_command_ms);
    TEST_ASSERT_EQUAL_UINT32(1U, effects->responses_completed);
    TEST_ASSERT_EQUAL_INT64(effects->now_ms, effects->last_response_ms);
    TEST_ASSERT_EQUAL_UINT32(1U, effects->handler_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, effects->recovery_executor_calls);
}

static badge_usb_health_action_t model_health_decide(
    const ingress_effects_t *effects, int64_t decision_now_ms)
{
    int64_t oldest_unanswered_ms =
        effects->oldest_hard_unanswered_response_ms;
    if (effects->oldest_enqueued_response_ms >= 0 &&
        (oldest_unanswered_ms < 0 ||
         effects->oldest_enqueued_response_ms < oldest_unanswered_ms)) {
        oldest_unanswered_ms = effects->oldest_enqueued_response_ms;
    }
    badge_usb_health_inputs_t inputs = {
        .task_started = true,
        .host_connected = true,
        .now_ms = decision_now_ms,
        .task_heartbeat_ms = decision_now_ms,
        .last_rx_ms = effects->last_rx_ms,
        .last_command_ms = effects->last_command_ms,
        .last_response_ms = effects->last_response_ms,
        .oldest_unanswered_command_ms = oldest_unanswered_ms,
        .last_transaction_progress_ms = -1,
        .boot_grace_ms = 0,
        .stale_after_ms = 3000,
    };
    return badge_usb_health_decide(&inputs);
}

static const wire_fixture_t VALID_USB_FIRMWARE_LINES[] = {
    WIRE_FIXTURE(
        "FOF_CTL:{\"cmd\":\"fw_relay\",\"uart\":\"uart1\","
        "\"expected_generation\":1,\"expected_hardware_id\":\"hw\","
        "\"allow_same_version\":false}\n"),
    WIRE_FIXTURE(
        "FOF_CTL:{\"cmd\":\"fw_relay\",\"uart\":\"uart1\","
        "\"expected_generation\":1,\"expected_hardware_id\":\"hw\","
        "\"allow_same_version\":false,\"force\":true,"
        "\"skip_command_probe\":true}\n"),
    WIRE_FIXTURE(
        "FOF_CTL:{\"cmd\":\"fw_upload_begin\",\"name\":\"scanner\","
        "\"target\":\"scanner\",\"project\":\"fof_scanner\","
        "\"hardware_type\":\"esp32s3\",\"version\":\"1\","
        "\"size\":1,\"crc32\":2,\"sha256\":\"aa\","
        "\"slot_mask\":3,\"flow_control\":\"credit-v1\"}\n"),
    WIRE_FIXTURE(
        "FOF_CTL:{\"cmd\":\"fw_upload_begin\",\"name\":\"scanner\","
        "\"target\":\"scanner\",\"project\":\"fof_scanner\","
        "\"hardware_type\":\"esp32s3\",\"version\":\"1\","
        "\"size\":1,\"crc32\":2,\"sha256\":\"aa\","
        "\"slot_mask\":3,\"flow_control\":\"credit-v1\","
        "\"session\":\"0123456789ABCDEF\"}\n"),
    WIRE_FIXTURE(
        "FOF_CTL:{\"cmd\":\"uplink_ota_begin\",\"target\":\"uplink\","
        "\"project\":\"fof_uplink\",\"hardware_type\":\"esp32s3\","
        "\"version\":\"1\",\"size\":1,\"crc32\":2,"
        "\"sha256\":\"aa\",\"flow_control\":\"credit-v1\","
        "\"recovery_rewrite_same_version\":false}\n"),
    WIRE_FIXTURE(
        "FOF_CTL:{\"cmd\":\"uplink_ota_begin\",\"target\":\"uplink\","
        "\"project\":\"fof_uplink\",\"hardware_type\":\"esp32s3\","
        "\"version\":\"1\",\"size\":1,\"crc32\":2,"
        "\"sha256\":\"aa\",\"flow_control\":\"credit-v1\","
        "\"recovery_rewrite_same_version\":false,"
        "\"session\":\"0123456789ABCDEF\"}\n"),
    WIRE_FIXTURE("FOF_CTL:{\"cmd\":\"fw_check\"}\n"),
    WIRE_FIXTURE(
        "FOF_CTL:{\"cmd\":\"fw_check\",\"uart\":\"uart1\"}\n"),
    WIRE_FIXTURE("FOF_CTL:{\"cmd\":\"fw_check_now\"}\n"),
    WIRE_FIXTURE(
        "FOF_CTL:{\"cmd\":\"fw_check_now\",\"uart\":\"uart2\"}\n"),
};

static const wire_fixture_t INVALID_USB_FIRMWARE_SHAPES[] = {
    WIRE_FIXTURE(
        "FOF_CTL:{\"cmd\":\"fw_relay\",\"uart\":\"uart1\","
        "\"expected_generation\":1,\"expected_hardware_id\":\"hw\","
        "\"allow_same_version\":false,\"extra\":true}\n"),
    WIRE_FIXTURE(
        "FOF_CTL:{\"cmd\":\"fw_relay\",\"uart\":\"uart1\","
        "\"expected_generation\":1,\"expected_hardware_id\":\"hw\","
        "\"allow_same_version\":false,\"force\":true,"
        "\"skip_command_probe\":true,\"extra\":true}\n"),
    WIRE_FIXTURE(
        "FOF_CTL:{\"cmd\":\"fw_upload_begin\",\"name\":\"scanner\","
        "\"target\":\"scanner\",\"project\":\"fof_scanner\","
        "\"hardware_type\":\"esp32s3\",\"version\":\"1\","
        "\"size\":1,\"crc32\":2,\"sha256\":\"aa\","
        "\"slot_mask\":3,\"flow_control\":\"credit-v1\",\"extra\":true}\n"),
    WIRE_FIXTURE(
        "FOF_CTL:{\"cmd\":\"uplink_ota_begin\",\"target\":\"uplink\","
        "\"project\":\"fof_uplink\",\"hardware_type\":\"esp32s3\","
        "\"version\":\"1\",\"size\":1,\"crc32\":2,"
        "\"sha256\":\"aa\",\"flow_control\":\"credit-v1\","
        "\"recovery_rewrite_same_version\":false,\"extra\":true}\n"),
    WIRE_FIXTURE("FOF_CTL:{\"cmd\":\"fw_check\",\"extra\":true}\n"),
    WIRE_FIXTURE(
        "FOF_CTL:{\"cmd\":\"fw_check\",\"uart\":\"uart1\","
        "\"extra\":true}\n"),
    WIRE_FIXTURE("FOF_CTL:{\"cmd\":\"fw_check_now\",\"extra\":true}\n"),
    WIRE_FIXTURE(
        "FOF_CTL:{\"cmd\":\"fw_check_now\",\"uart\":\"uart2\","
        "\"extra\":true}\n"),
};

void test_badge_usb_phase_a_ingress_preserves_length_and_rejects_embedded_nul(
    void)
{
    static const uint8_t wire[] =
        "FOF_CTL:{\"cmd\":\"fw_check\"}\0{\"cmd\":\"reboot\"}\n";
    ingress_harness_t harness;
    harness_init(&harness, true, false);

    harness_feed_wire(&harness, wire, sizeof(wire) - 1U);

    assert_no_protected_effects(&harness.effects);
    TEST_ASSERT_EQUAL_UINT32(1U, harness.effects.rejection_frames);
}

void test_badge_usb_phase_a_ingress_rejects_selector_corruptions_before_effects(
    void)
{
    static const wire_fixture_t cases[] = {
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"fw_check\",\"cmd\":\"fw_check\"}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"c\\u006dd\":\"fw_check\"}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"fw_\\u0063heck\"}\n"),
        WIRE_FIXTURE("FOF_CTL:{\"cmd\":true}\n"),
        WIRE_FIXTURE("FOF_CTL:{\"cmd\":\"fw_unknown\"}\n"),
        WIRE_FIXTURE("FOF_CTL:{\"cmd\":\"fw_check\"}{}\n"),
    };

    for (size_t i = 0U; i < ARRAY_SIZE(cases); i++) {
        ingress_harness_t harness;
        harness_init(&harness, true, false);
        harness_feed_wire(&harness, cases[i].bytes, cases[i].length);
        assert_no_protected_effects(&harness.effects);
        TEST_ASSERT_EQUAL_UINT32(1U, harness.effects.rejection_frames);
    }
}

void test_badge_usb_phase_a_ingress_validates_all_closed_usb_firmware_shapes(
    void)
{
    TEST_ASSERT_EQUAL_UINT(10U, ARRAY_SIZE(VALID_USB_FIRMWARE_LINES));
    TEST_ASSERT_EQUAL_UINT(8U, ARRAY_SIZE(INVALID_USB_FIRMWARE_SHAPES));

    for (size_t i = 0U; i < ARRAY_SIZE(VALID_USB_FIRMWARE_LINES); i++) {
        ingress_harness_t harness;
        harness_init(&harness, true, false);
        harness_feed_wire(
            &harness, VALID_USB_FIRMWARE_LINES[i].bytes,
            VALID_USB_FIRMWARE_LINES[i].length);
        assert_one_normal_dispatch(&harness.effects);
        TEST_ASSERT_EQUAL_UINT(
            VALID_USB_FIRMWARE_LINES[i].length - 1U,
            harness.effects.last_line_length);
        TEST_ASSERT_EQUAL_MEMORY(
            VALID_USB_FIRMWARE_LINES[i].bytes, harness.effects.last_line,
            harness.effects.last_line_length);
    }

    for (size_t i = 0U; i < ARRAY_SIZE(INVALID_USB_FIRMWARE_SHAPES); i++) {
        ingress_harness_t harness;
        harness_init(&harness, true, false);
        harness_feed_wire(
            &harness, INVALID_USB_FIRMWARE_SHAPES[i].bytes,
            INVALID_USB_FIRMWARE_SHAPES[i].length);
        assert_no_protected_effects(&harness.effects);
        TEST_ASSERT_EQUAL_UINT32(1U, harness.effects.rejection_frames);
    }
}

void test_badge_usb_phase_a_ingress_fragmented_lf_and_crlf_stay_fail_closed(
    void)
{
    static const uint8_t lf_wire[] =
        "FOF_CTL:{\"cmd\":\"fw_check\"}\0junk\n";
    static const uint8_t crlf_wire[] =
        "FOF_CTL:{\"cmd\":\"fw_check\"}\0junk\r\n";

    for (size_t split = 1U; split < sizeof(lf_wire) - 1U; split++) {
        ingress_harness_t harness;
        harness_init(&harness, true, false);
        harness_feed_chunk(&harness, lf_wire, split);
        harness_feed_chunk(
            &harness, lf_wire + split, sizeof(lf_wire) - 1U - split);
        assert_no_protected_effects(&harness.effects);
        TEST_ASSERT_EQUAL_UINT32(1U, harness.effects.rejection_frames);
    }

    for (size_t split = 1U; split < sizeof(crlf_wire) - 1U; split++) {
        ingress_harness_t harness;
        harness_init(&harness, true, false);
        harness_feed_chunk(&harness, crlf_wire, split);
        harness_feed_chunk(
            &harness, crlf_wire + split,
            sizeof(crlf_wire) - 1U - split);
        assert_no_protected_effects(&harness.effects);
        TEST_ASSERT_EQUAL_UINT32(1U, harness.effects.rejection_frames);
    }
}

void test_badge_usb_phase_a_ingress_coalesced_invalid_then_valid_isolated(void)
{
    static const uint8_t wire[] =
        "FOF_CTL:{\"cmd\":\"fw_check\"}\0junk\nFOF_PING\n";
    ingress_harness_t harness;
    harness_init(&harness, true, false);

    harness_feed_wire(&harness, wire, sizeof(wire) - 1U);

    TEST_ASSERT_EQUAL_UINT32(1U, harness.effects.rejection_frames);
    assert_one_normal_dispatch(&harness.effects);
    TEST_ASSERT_EQUAL_UINT(sizeof("FOF_PING") - 1U,
                           harness.effects.last_line_length);
    TEST_ASSERT_EQUAL_MEMORY(
        "FOF_PING", harness.effects.last_line,
        harness.effects.last_line_length);
}

void test_badge_usb_phase_a_ingress_plain_commands_require_exact_spans(void)
{
    static const wire_fixture_t accepted[] = {
        WIRE_FIXTURE("FOF_PING\n"),
        WIRE_FIXTURE("FOF_STATUS\n"),
        WIRE_FIXTURE("FOF_SAVE\n"),
        WIRE_FIXTURE("FOF_REBOOT\n"),
    };
    static const wire_fixture_t rejected[] = {
        WIRE_FIXTURE("FOF_PINGX\n"),
        WIRE_FIXTURE("FOF_STATUS \n"),
        WIRE_FIXTURE("FOF_SAVE\0X\n"),
        WIRE_FIXTURE("FOF_REBOOTX\n"),
        WIRE_FIXTURE("FOF_BOOTLOADER\n"),
        WIRE_FIXTURE("FOF_DOWNLOAD\n"),
        WIRE_FIXTURE("FOF_FLASH\n"),
    };

    for (size_t i = 0U; i < ARRAY_SIZE(accepted); i++) {
        ingress_harness_t harness;
        harness_init(&harness, true, false);
        harness_feed_wire(&harness, accepted[i].bytes, accepted[i].length);
        assert_one_normal_dispatch(&harness.effects);
        TEST_ASSERT_EQUAL_UINT(accepted[i].length - 1U,
                               harness.effects.last_line_length);
    }

    for (size_t i = 0U; i < ARRAY_SIZE(rejected); i++) {
        ingress_harness_t harness;
        harness_init(&harness, true, false);
        harness_feed_wire(&harness, rejected[i].bytes, rejected[i].length);
        assert_no_protected_effects(&harness.effects);
        TEST_ASSERT_EQUAL_UINT32(1U, harness.effects.rejection_frames);
    }
}

void test_badge_usb_phase_a_ingress_recovery_reauthorizes_raw_spans(void)
{
    static const wire_fixture_t accepted[] = {
        WIRE_FIXTURE("FOF_PING\n"),
        WIRE_FIXTURE("FOF_STATUS\n"),
        WIRE_FIXTURE("FOF_REBOOT\n"),
        WIRE_FIXTURE("FOF_BOOTLOADER\n"),
        WIRE_FIXTURE("FOF_DOWNLOAD\n"),
        WIRE_FIXTURE("FOF_FLASH\n"),
        WIRE_FIXTURE("FOF_CTL:{\"cmd\":\"status\"}\n"),
        WIRE_FIXTURE("FOF_CTL:{\"cmd\":\"reboot\"}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"uplink_ota_begin\",\"target\":\"uplink\","
            "\"project\":\"fof_uplink\",\"hardware_type\":\"esp32s3\","
            "\"version\":\"1\",\"size\":1,\"crc32\":2,"
            "\"sha256\":\"aa\",\"flow_control\":\"credit-v1\","
            "\"recovery_rewrite_same_version\":false}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"uplink_ota_begin\",\"target\":\"uplink\","
            "\"project\":\"fof_uplink\",\"hardware_type\":\"esp32s3\","
            "\"version\":\"1\",\"size\":1,\"crc32\":2,"
            "\"sha256\":\"aa\",\"flow_control\":\"credit-v1\","
            "\"recovery_rewrite_same_version\":false,"
            "\"session\":\"0123456789ABCDEF\"}\n"),
    };
    static const wire_fixture_t rejected[] = {
        WIRE_FIXTURE("FOF_SAVE\n"),
        WIRE_FIXTURE("FOF_CTL:{\"cmd\":\"fw_check\"}\n"),
        WIRE_FIXTURE("FOF_REBOOT\0FOF_FLASH\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"uplink_ota_begin\",\"target\":\"uplink\","
            "\"project\":\"fof_uplink\",\"hardware_type\":\"esp32s3\","
            "\"version\":\"1\",\"size\":1,\"crc32\":2,"
            "\"sha256\":\"aa\",\"flow_control\":\"credit-v1\","
            "\"recovery_rewrite_same_version\":false,\"extra\":true}\n"),
    };

    for (size_t i = 0U; i < ARRAY_SIZE(accepted); i++) {
        ingress_harness_t harness;
        harness_init(&harness, true, true);
        harness_feed_wire(&harness, accepted[i].bytes, accepted[i].length);
        TEST_ASSERT_EQUAL_UINT32(1U,
                                 harness.effects.recognized_health_updates);
        TEST_ASSERT_EQUAL_UINT32(1U, harness.effects.handler_calls);
        TEST_ASSERT_EQUAL_UINT32(
            1U, harness.effects.recovery_executor_calls);
    }

    for (size_t i = 0U; i < ARRAY_SIZE(rejected); i++) {
        ingress_harness_t harness;
        harness_init(&harness, true, true);
        harness_feed_wire(&harness, rejected[i].bytes, rejected[i].length);
        assert_no_protected_effects(&harness.effects);
        TEST_ASSERT_EQUAL_UINT32(1U, harness.effects.rejection_frames);
    }
}

void test_badge_usb_phase_a_ingress_transitional_nonfirmware_allowlist_only(
    void)
{
    static const wire_fixture_t exact_controls[] = {
        WIRE_FIXTURE("FOF_CTL:{\"cmd\":\"status\"}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"power_mode\",\"mode\":\"quiet\"}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"set_mode\",\"mode\":\"usb\","
            "\"persist\":true}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"set_mode\",\"mode\":\"backend\","
            "\"ttl_s\":7500}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"set_backend\",\"url\":\"https://x\","
            "\"wifi_ssid\":\"FoF\",\"wifi_pass\":\"secret\","
            "\"enable\":true,\"ttl_s\":7500}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"set_display_debug\",\"enabled\":false}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"network\",\"mode\":\"local_ap\","
            "\"ttl_s\":7500}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"safe_mode\",\"enabled\":true,"
            "\"reason\":\"USB recovery\"}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"ble_investigate\","
            "\"request_id\":\"req-1\",\"mode\":\"gatt\","
            "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":7500}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"ble_investigation_chunk\","
            "\"request_id\":\"req-1\",\"seq\":0}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"badge_display_policy_reset\","
            "\"persist\":false}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"badge_theme_reset\",\"persist\":false}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"display_nav\",\"action\":\"next\"}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"scanner_display\",\"uart\":\"all\","
            "\"button_enabled\":true,\"view\":\"privacy\",\"page\":-1,"
            "\"page_lock\":false,\"auto_page\":true}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"scanner_display\",\"uart\":\"0\","
            "\"trigger_enabled\":true,\"view\":\"prv\",\"page\":0,"
            "\"page_lock\":true,\"auto_page\":false}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"scanner_display\",\"uart\":\"1\","
            "\"boot_enabled\":false,\"view\":\"drone\",\"page\":1,"
            "\"page_lock\":false,\"auto_page\":false}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"scanner_trigger\",\"uart\":\"ble\","
            "\"enabled\":true}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"trigger\",\"uart\":\"wifi\","
            "\"enabled\":false}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"scanner_safe_mode\",\"uart\":\"*\","
            "\"enabled\":true}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"scanner_recovery\",\"uart\":\"all\","
            "\"enabled\":false}\n"),
        WIRE_FIXTURE("FOF_CTL:{\"cmd\":\"reboot\"}\n"),
    };

    for (size_t i = 0U; i < ARRAY_SIZE(exact_controls); i++) {
        ingress_harness_t harness;
        harness_init(&harness, true, false);
        harness_feed_wire(
            &harness, exact_controls[i].bytes, exact_controls[i].length);
        assert_one_normal_dispatch(&harness.effects);
    }

    static const wire_fixture_t rejected[] = {
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"power_mode\"}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"power_mode\",\"mode\":\"quiet\","
            "\"extra\":true}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"set_mode\",\"mode\":\"usb\","
            "\"persist\":true,\"ttl_s\":7500}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"ble_investigate\","
            "\"request_id\":\"req-1\",\"mode\":\"gatt\","
            "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":12001}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"not_a_registered_control\"}\n"),
    };
    for (size_t i = 0U; i < ARRAY_SIZE(rejected); i++) {
        ingress_harness_t harness;
        harness_init(&harness, true, false);
        harness_feed_wire(
            &harness, rejected[i].bytes, rejected[i].length);
        assert_no_protected_effects(&harness.effects);
        TEST_ASSERT_EQUAL_UINT32(1U, harness.effects.rejection_frames);
    }
}

void test_badge_usb_phase_a_dispatch_exposes_authorized_control_route(void)
{
    static const uint8_t wire[] =
        "FOF_CTL:{\"cmd\":\"network\",\"mode\":\"backend\","
        "\"ttl_s\":7500}\n";
    ingress_harness_t harness;
    harness_init(&harness, true, false);
    harness_feed_wire(&harness, wire, sizeof(wire) - 1U);

    assert_one_normal_dispatch(&harness.effects);
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_SCHEMA_NETWORK,
        harness.effects.last_control_schema_id);
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_HANDLER_NETWORK,
        harness.effects.last_control_handler_kind);
}

void test_badge_usb_phase_a_ingress_fof_set_preserves_first_equals_remainder(
    void)
{
    static const uint8_t line[] = "FOF_SET:wifi_pass=a=b==";
    serial_config_set_parts_t parts;

    TEST_ASSERT_TRUE(serial_config_ingress_parse_set(
        line, sizeof(line) - 1U, &parts));
    TEST_ASSERT_EQUAL_UINT(sizeof("wifi_pass") - 1U, parts.key_len);
    TEST_ASSERT_EQUAL_MEMORY("wifi_pass", parts.key, parts.key_len);
    TEST_ASSERT_EQUAL_UINT(sizeof("a=b==") - 1U, parts.value_len);
    TEST_ASSERT_EQUAL_MEMORY("a=b==", parts.value, parts.value_len);

    static const uint8_t valid_wire[] = "FOF_SET:wifi_pass=a=b==\n";
    ingress_harness_t valid;
    harness_init(&valid, true, false);
    harness_feed_wire(&valid, valid_wire, sizeof(valid_wire) - 1U);
    assert_one_normal_dispatch(&valid.effects);
    TEST_ASSERT_EQUAL_UINT(sizeof(line) - 1U,
                           valid.effects.last_line_length);
    TEST_ASSERT_EQUAL_MEMORY(
        line, valid.effects.last_line, valid.effects.last_line_length);

    static const uint8_t invalid_wire[] =
        "FOF_SET:wifi_pass=a=b\0==\n";
    ingress_harness_t invalid;
    harness_init(&invalid, true, false);
    harness_feed_wire(&invalid, invalid_wire, sizeof(invalid_wire) - 1U);
    assert_no_protected_effects(&invalid.effects);
    TEST_ASSERT_EQUAL_UINT32(1U, invalid.effects.rejection_frames);
}

void test_badge_usb_phase_a_ingress_game_seed_is_exact_and_closed(void)
{
    static const wire_fixture_t accepted[] = {
        WIRE_FIXTURE("FOF_SET:game_seed=normal\n"),
        WIRE_FIXTURE("FOF_SET:game_seed=infected\n"),
        WIRE_FIXTURE("FOF_SET:game_seed=immune\n"),
    };
    for (size_t i = 0U; i < ARRAY_SIZE(accepted); i++) {
        ingress_harness_t harness;
        harness_init(&harness, true, false);
        harness_feed_wire(
            &harness, accepted[i].bytes, accepted[i].length);
        assert_one_normal_dispatch(&harness.effects);
    }

    static const wire_fixture_t rejected[] = {
        WIRE_FIXTURE("FOF_SET:game_seed=Normal\n"),
        WIRE_FIXTURE("FOF_SET:game_seed= infected\n"),
        WIRE_FIXTURE("FOF_SET:game_seed=infected \n"),
        WIRE_FIXTURE("FOF_SET:game_seed=infection\n"),
        WIRE_FIXTURE("FOF_SET:game_seed=immune-now\n"),
        WIRE_FIXTURE("FOF_SET:game_seed=unknown\n"),
        WIRE_FIXTURE("FOF_SET:game_seed=immune\0suffix\n"),
        WIRE_FIXTURE("FOF_SET:game_role=immune\n"),
    };
    for (size_t i = 0U; i < ARRAY_SIZE(rejected); i++) {
        ingress_harness_t harness;
        harness_init(&harness, true, false);
        harness_feed_wire(
            &harness, rejected[i].bytes, rejected[i].length);
        assert_no_protected_effects(&harness.effects);
        TEST_ASSERT_EQUAL_UINT32(1U, harness.effects.rejection_frames);
    }
}

void test_badge_usb_phase_a_rejection_emissions_are_health_neutral_for_every_outcome(
    void)
{
    static const wire_fixture_t rejected_lines[] = {
        WIRE_FIXTURE("FOF_PING\0X\n"),
        WIRE_FIXTURE("FOF_PINGX\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"fw_check\",\"cmd\":\"fw_check\"}\n"),
        WIRE_FIXTURE(
            "FOF_CTL:{\"cmd\":\"fw_check\",\"extra\":true}\n"),
    };
    static const badge_usb_emit_result_t outcomes[] = {
        BADGE_USB_EMIT_COMPLETED,
        BADGE_USB_EMIT_ENQUEUED,
        BADGE_USB_EMIT_DROPPED,
        BADGE_USB_EMIT_FAILED,
        BADGE_USB_EMIT_POISONED,
    };

    for (size_t outcome = 0U; outcome < ARRAY_SIZE(outcomes); outcome++) {
        TEST_ASSERT_EQUAL(
            BADGE_USB_EMIT_HEALTH_EFFECT_NONE,
            badge_usb_emit_health_effect_decide(
                outcomes[outcome], BADGE_USB_FRAME_REQUIRED,
                BADGE_USB_EMIT_HEALTH_NEUTRAL));

        for (size_t rejected = 0U;
             rejected < ARRAY_SIZE(rejected_lines); rejected++) {
            ingress_harness_t harness;
            harness_init(&harness, true, false);
            harness.effects.rejection_emit_result = outcomes[outcome];
            harness_feed_wire(
                &harness, rejected_lines[rejected].bytes,
                rejected_lines[rejected].length);

            assert_no_protected_effects(&harness.effects);
            TEST_ASSERT_EQUAL_UINT64(
                rejected_lines[rejected].length,
                harness.effects.rx_bytes);
            TEST_ASSERT_EQUAL_UINT32(
                1U, harness.effects.rejection_frames);
            TEST_ASSERT_EQUAL(
                BADGE_USB_HEALTH_OK,
                model_health_decide(&harness.effects, 4000));
        }

        uint8_t overflow_line[2049];
        memset(overflow_line, 'A', sizeof(overflow_line));
        overflow_line[sizeof(overflow_line) - 1U] = '\n';
        ingress_harness_t overflow;
        harness_init(&overflow, true, false);
        overflow.effects.rejection_emit_result = outcomes[outcome];
        harness_feed_wire(
            &overflow, overflow_line, sizeof(overflow_line));
        assert_no_protected_effects(&overflow.effects);
        TEST_ASSERT_EQUAL_UINT32(1U, overflow.effects.malformed_lines);
        TEST_ASSERT_EQUAL_UINT32(
            1U, overflow.effects.parser_error_rejections);
        TEST_ASSERT_EQUAL_UINT32(1U, overflow.effects.rejection_frames);
        TEST_ASSERT_EQUAL(
            BADGE_USB_HEALTH_OK,
            model_health_decide(&overflow.effects, 4000));
    }

    /* A successfully drained rejection must not "rescue" an older required
     * response that was enqueued but never proven drained. */
    static const uint8_t unknown_wire[] = "FOF_NOPE\n";
    ingress_harness_t pending;
    harness_init(&pending, true, false);
    pending.effects.required_response_failures = 1U;
    pending.effects.enqueued_required_responses = 1U;
    pending.effects.oldest_enqueued_response_ms = 500;
    pending.effects.rejection_emit_result = BADGE_USB_EMIT_COMPLETED;
    harness_feed_wire(
        &pending, unknown_wire, sizeof(unknown_wire) - 1U);

    TEST_ASSERT_EQUAL_UINT32(1U, pending.effects.unknown_rejections);
    TEST_ASSERT_EQUAL_UINT32(1U, pending.effects.rejection_frames);
    TEST_ASSERT_EQUAL_UINT32(
        1U, pending.effects.required_response_failures);
    TEST_ASSERT_EQUAL_UINT32(
        1U, pending.effects.enqueued_required_responses);
    TEST_ASSERT_EQUAL_INT64(
        500, pending.effects.oldest_enqueued_response_ms);
    TEST_ASSERT_EQUAL_UINT32(0U, pending.effects.responses_completed);
    TEST_ASSERT_EQUAL_INT64(-1, pending.effects.last_response_ms);
    TEST_ASSERT_EQUAL_UINT32(0U, pending.effects.valid_commands);
    TEST_ASSERT_EQUAL_UINT32(0U, pending.effects.handler_calls);
    TEST_ASSERT_EQUAL(
        BADGE_USB_HEALTH_RESTART_SAFE_USB,
        model_health_decide(&pending.effects, 4000));
}

void test_badge_usb_phase_a_bare_and_in_frame_cr_rejections_are_health_neutral(
    void)
{
    static const wire_fixture_t cases[] = {
        WIRE_FIXTURE("FOF_PING\rX\n"),
        WIRE_FIXTURE("\rX\n"),
    };

    for (size_t i = 0U; i < ARRAY_SIZE(cases); i++) {
        ingress_harness_t harness;
        harness_init(&harness, true, false);
        harness_feed_wire(
            &harness, cases[i].bytes, cases[i].length);

        assert_no_protected_effects(&harness.effects);
        TEST_ASSERT_EQUAL_UINT64(
            cases[i].length, harness.effects.rx_bytes);
        TEST_ASSERT_EQUAL_UINT32(0U, harness.effects.rejection_frames);
        TEST_ASSERT_EQUAL(
            BADGE_USB_HEALTH_OK,
            model_health_decide(&harness.effects, 4000));
    }
}

void test_badge_usb_phase_a_dispatch_matrix_is_booting_first_and_auth_only(
    void)
{
    typedef struct {
        bool recovery_only;
        bool dispatch_ready;
        bool recognized;
        uint32_t booting;
        uint32_t recovery_rejection;
        uint32_t unknown;
        uint32_t normal_dispatch;
        uint32_t recovery_dispatch;
        uint32_t trusted_notes;
    } dispatch_case_t;
    static const dispatch_case_t cases[] = {
        {false, true,  true,  0U, 0U, 0U, 1U, 0U, 1U},
        {false, true,  false, 0U, 0U, 1U, 0U, 0U, 0U},
        {true,  true,  true,  0U, 0U, 0U, 0U, 1U, 1U},
        {true,  true,  false, 0U, 1U, 0U, 0U, 0U, 0U},
        {false, false, true,  1U, 0U, 0U, 0U, 0U, 0U},
        {false, false, false, 1U, 0U, 0U, 0U, 0U, 0U},
        {true,  false, true,  1U, 0U, 0U, 0U, 0U, 0U},
        {true,  false, false, 1U, 0U, 0U, 0U, 0U, 0U},
    };
    static const uint8_t recognized[] = "FOF_PING\n";
    static const uint8_t invalid[] = "FOF_NOPE\n";

    for (size_t i = 0U; i < ARRAY_SIZE(cases); i++) {
        ingress_harness_t harness;
        harness_init(
            &harness, cases[i].dispatch_ready, cases[i].recovery_only);
        const uint8_t *wire =
            cases[i].recognized ? recognized : invalid;
        size_t wire_len = cases[i].recognized
            ? sizeof(recognized) - 1U : sizeof(invalid) - 1U;
        harness_feed_wire(&harness, wire, wire_len);

        TEST_ASSERT_EQUAL_UINT32(
            cases[i].booting, harness.effects.booting_rejections);
        TEST_ASSERT_EQUAL_UINT32(
            cases[i].recovery_rejection,
            harness.effects.recovery_only_rejections);
        TEST_ASSERT_EQUAL_UINT32(
            cases[i].unknown, harness.effects.unknown_rejections);
        TEST_ASSERT_EQUAL_UINT32(
            cases[i].normal_dispatch, harness.effects.handler_calls -
                harness.effects.recovery_executor_calls);
        TEST_ASSERT_EQUAL_UINT32(
            cases[i].recovery_dispatch,
            harness.effects.recovery_executor_calls);
        TEST_ASSERT_EQUAL_UINT32(
            cases[i].trusted_notes,
            harness.effects.recognized_health_updates);
        TEST_ASSERT_EQUAL_UINT32(
            cases[i].trusted_notes, harness.effects.valid_commands);

        if (cases[i].trusted_notes > 0U) {
            TEST_ASSERT_EQUAL_INT64(
                harness.effects.now_ms, harness.effects.last_rx_ms);
            TEST_ASSERT_EQUAL_INT64(
                harness.effects.now_ms, harness.effects.last_command_ms);
        } else {
            TEST_ASSERT_EQUAL_INT64(-1, harness.effects.last_rx_ms);
            TEST_ASSERT_EQUAL_INT64(-1, harness.effects.last_command_ms);
        }
        if (!cases[i].dispatch_ready || !cases[i].recognized) {
            assert_no_protected_effects(&harness.effects);
            TEST_ASSERT_EQUAL(
                BADGE_USB_HEALTH_OK,
                model_health_decide(&harness.effects, 4000));
        }
    }
}

void test_badge_usb_phase_a_authorized_pre_ready_booting_stays_health_neutral_past_stale(
    void)
{
    static const uint8_t wire[] = "FOF_PING\n";
    static const badge_usb_emit_result_t outcomes[] = {
        BADGE_USB_EMIT_COMPLETED,
        BADGE_USB_EMIT_ENQUEUED,
        BADGE_USB_EMIT_DROPPED,
        BADGE_USB_EMIT_FAILED,
        BADGE_USB_EMIT_POISONED,
    };

    for (size_t recovery = 0U; recovery < 2U; recovery++) {
        for (size_t outcome = 0U;
             outcome < ARRAY_SIZE(outcomes); outcome++) {
            ingress_harness_t harness;
            harness_init(&harness, false, recovery != 0U);
            harness.effects.rejection_emit_result = outcomes[outcome];
            harness_feed_wire(&harness, wire, sizeof(wire) - 1U);

            assert_no_protected_effects(&harness.effects);
            TEST_ASSERT_EQUAL_UINT32(
                1U, harness.effects.booting_rejections);
            TEST_ASSERT_EQUAL_UINT32(
                1U, harness.effects.rejection_frames);
            TEST_ASSERT_EQUAL(
                BADGE_USB_HEALTH_OK,
                model_health_decide(&harness.effects, 4000));
        }
    }
}

void test_badge_usb_phase_a_authorized_command_tracks_genuine_unanswered_reply(
    void)
{
    static const uint8_t wire[] = "FOF_PING\n";
    ingress_harness_t completed;
    harness_init(&completed, true, false);
    harness_feed_wire(&completed, wire, sizeof(wire) - 1U);

    assert_one_normal_dispatch(&completed.effects);
    TEST_ASSERT_EQUAL(
        BADGE_USB_HEALTH_OK,
        model_health_decide(&completed.effects, 4000));

    ingress_harness_t failed;
    harness_init(&failed, true, false);
    failed.effects.dispatch_emit_result = BADGE_USB_EMIT_FAILED;
    harness_feed_wire(&failed, wire, sizeof(wire) - 1U);

    TEST_ASSERT_EQUAL_UINT32(
        1U, failed.effects.recognized_health_updates);
    TEST_ASSERT_EQUAL_UINT32(1U, failed.effects.valid_commands);
    TEST_ASSERT_EQUAL_INT64(
        failed.effects.now_ms, failed.effects.last_rx_ms);
    TEST_ASSERT_EQUAL_INT64(
        failed.effects.now_ms, failed.effects.last_command_ms);
    TEST_ASSERT_EQUAL_UINT32(
        1U, failed.effects.required_response_failures);
    TEST_ASSERT_EQUAL_UINT32(
        1U, failed.effects.hard_unanswered_required_responses);
    TEST_ASSERT_EQUAL_INT64(
        failed.effects.now_ms,
        failed.effects.oldest_hard_unanswered_response_ms);
    TEST_ASSERT_EQUAL(
        BADGE_USB_HEALTH_RESTART_SAFE_USB,
        model_health_decide(&failed.effects, 4000));
}
