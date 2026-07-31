#include "unity.h"

#include "badge_con_protocol.h"
#include "badge_con_vhci_policy.h"

#include <string.h>

#define RECORDED_COMMANDS 12U
#define RECORDED_COMMAND_BYTES 40U

typedef struct {
    uint8_t commands[RECORDED_COMMANDS][RECORDED_COMMAND_BYTES];
    size_t sizes[RECORDED_COMMANDS];
    size_t count;
    bool accept;
} fake_vhci_transport_t;

static bool fake_send(void *context, const uint8_t *bytes, size_t size)
{
    fake_vhci_transport_t *fake = context;
    if (!fake || !fake->accept || !bytes || size == 0U ||
        size > RECORDED_COMMAND_BYTES ||
        fake->count >= RECORDED_COMMANDS) {
        return false;
    }
    memcpy(fake->commands[fake->count], bytes, size);
    fake->sizes[fake->count] = size;
    fake->count++;
    return true;
}

static uint16_t recorded_opcode(const fake_vhci_transport_t *fake,
                                size_t index)
{
    TEST_ASSERT_NOT_NULL(fake);
    TEST_ASSERT_LESS_THAN(fake->count, index);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(4U, fake->sizes[index]);
    TEST_ASSERT_EQUAL_HEX8(0x01U, fake->commands[index][0]);
    return (uint16_t)fake->commands[index][1] |
           (uint16_t)((uint16_t)fake->commands[index][2] << 8U);
}

static void complete_command(badge_con_vhci_policy_t *policy,
                             uint16_t opcode,
                             uint8_t status,
                             uint32_t now_ms)
{
    const uint8_t event[] = {
        0x04U, 0x0EU, 0x04U, 0x01U,
        (uint8_t)(opcode & 0xFFU),
        (uint8_t)(opcode >> 8U),
        status,
    };
    badge_con_vhci_policy_on_hci_event(
        policy, event, sizeof(event), now_ms);
}

static void command_status(badge_con_vhci_policy_t *policy,
                           uint16_t opcode,
                           uint8_t status,
                           uint32_t now_ms)
{
    const uint8_t event[] = {
        0x04U, 0x0FU, 0x04U, status, 0x01U,
        (uint8_t)(opcode & 0xFFU),
        (uint8_t)(opcode >> 8U),
    };
    badge_con_vhci_policy_on_hci_event(
        policy, event, sizeof(event), now_ms);
}

static void prepare_policy(badge_con_vhci_policy_t *policy,
                           fake_vhci_transport_t *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->accept = true;
    badge_con_vhci_transport_t transport = {
        .context = fake,
        .send = fake_send,
    };
    badge_con_vhci_policy_init(
        policy, 0xA1B2C3U, 0x07U, 0x2AU, &transport);
    badge_con_vhci_policy_set_controller_initialized(policy, true);
}

static void drive_to_advertising(badge_con_vhci_policy_t *policy,
                                 fake_vhci_transport_t *fake,
                                 uint32_t now_ms)
{
    badge_con_vhci_policy_set_game_active(policy, true);
    badge_con_vhci_policy_set_self_ready(policy, true);
    badge_con_vhci_policy_set_inhibited(policy, false);

    badge_con_vhci_policy_poll(policy, now_ms);
    TEST_ASSERT_EQUAL_HEX16(0x2006U, recorded_opcode(fake, 0U));
    complete_command(policy, 0x2006U, 0U, now_ms);

    badge_con_vhci_policy_poll(policy, now_ms);
    TEST_ASSERT_EQUAL_HEX16(0x2008U, recorded_opcode(fake, 1U));
    complete_command(policy, 0x2008U, 0U, now_ms);

    badge_con_vhci_policy_poll(policy, now_ms);
    TEST_ASSERT_EQUAL_HEX16(0x200AU, recorded_opcode(fake, 2U));
    complete_command(policy, 0x200AU, 0U, now_ms);
}

void test_badge_con_vhci_policy_gates_and_orders_exact_hci_commands(void)
{
    badge_con_vhci_policy_t policy;
    fake_vhci_transport_t fake;
    prepare_policy(&policy, &fake);

    badge_con_vhci_policy_poll(&policy, 10U);
    TEST_ASSERT_EQUAL_UINT32(0U, fake.count);

    badge_con_vhci_policy_set_game_active(&policy, true);
    badge_con_vhci_policy_poll(&policy, 10U);
    TEST_ASSERT_EQUAL_UINT32(0U, fake.count);

    badge_con_vhci_policy_set_self_ready(&policy, true);
    badge_con_vhci_policy_set_inhibited(&policy, true);
    badge_con_vhci_policy_poll(&policy, 10U);
    TEST_ASSERT_EQUAL_UINT32(0U, fake.count);

    badge_con_vhci_policy_set_inhibited(&policy, false);
    drive_to_advertising(&policy, &fake, 10U);

    static const uint8_t expected_parameters[] = {
        0x01U, 0x06U, 0x20U, 0x0FU,
        0x40U, 0x06U, 0x40U, 0x06U,
        0x03U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x07U, 0x00U,
    };
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected_parameters), fake.sizes[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        expected_parameters, fake.commands[0], sizeof(expected_parameters));

    uint8_t expected_advertisement[BADGE_CON_LEGACY_ADV_BYTES] = {0};
    TEST_ASSERT_TRUE(badge_con_build_legacy_advertisement(
        BADGE_CON_ROLE_NORMAL, false,
        0xA1B2C3U, 0x07U, 0x2AU,
        expected_advertisement));
    TEST_ASSERT_EQUAL_UINT32(36U, fake.sizes[1]);
    TEST_ASSERT_EQUAL_HEX8(31U, fake.commands[1][4]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        expected_advertisement, fake.commands[1] + 5U,
        sizeof(expected_advertisement));
    TEST_ASSERT_EQUAL_HEX8(0x01U, fake.commands[2][4]);

    badge_con_vhci_snapshot_t snapshot = {0};
    badge_con_vhci_policy_snapshot(&policy, &snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_VHCI_ADVERTISING, snapshot.state);
    TEST_ASSERT_TRUE(snapshot.controller_initialized);
    TEST_ASSERT_TRUE(snapshot.advertising);
    TEST_ASSERT_FALSE(snapshot.inhibited);
    TEST_ASSERT_EQUAL_HEX8(0x2AU, snapshot.sequence);
}

void test_badge_con_vhci_policy_refreshes_next_epoch_and_disables_before_quiet(void)
{
    badge_con_vhci_policy_t policy;
    fake_vhci_transport_t fake;
    prepare_policy(&policy, &fake);
    drive_to_advertising(&policy, &fake, 100U);

    badge_con_vhci_policy_set_identity_state(
        &policy, BADGE_CON_ROLE_INFECTED, false);
    badge_con_vhci_policy_poll(&policy, 1099U);
    TEST_ASSERT_EQUAL_UINT32(3U, fake.count);

    badge_con_vhci_policy_poll(&policy, 1100U);
    TEST_ASSERT_EQUAL_UINT32(4U, fake.count);
    TEST_ASSERT_EQUAL_HEX16(0x2008U, recorded_opcode(&fake, 3U));
    uint8_t expected_advertisement[BADGE_CON_LEGACY_ADV_BYTES] = {0};
    TEST_ASSERT_TRUE(badge_con_build_legacy_advertisement(
        BADGE_CON_ROLE_INFECTED, false,
        0xA1B2C3U, 0x07U, 0x2BU,
        expected_advertisement));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        expected_advertisement, fake.commands[3] + 5U,
        sizeof(expected_advertisement));
    complete_command(&policy, 0x2008U, 0U, 1100U);

    badge_con_vhci_policy_set_inhibited(&policy, true);
    badge_con_vhci_policy_poll(&policy, 1101U);
    TEST_ASSERT_EQUAL_UINT32(5U, fake.count);
    TEST_ASSERT_EQUAL_HEX16(0x200AU, recorded_opcode(&fake, 4U));
    TEST_ASSERT_EQUAL_HEX8(0x00U, fake.commands[4][4]);

    badge_con_vhci_snapshot_t snapshot = {0};
    badge_con_vhci_policy_snapshot(&policy, &snapshot);
    TEST_ASSERT_TRUE(snapshot.advertising);
    TEST_ASSERT_TRUE(snapshot.inhibited);
    TEST_ASSERT_EQUAL(BADGE_CON_VHCI_DISABLE, snapshot.state);
    TEST_ASSERT_FALSE(badge_con_vhci_policy_radio_quiesced(&policy));

    complete_command(&policy, 0x200AU, 0U, 1101U);
    badge_con_vhci_policy_snapshot(&policy, &snapshot);
    TEST_ASSERT_FALSE(snapshot.advertising);
    TEST_ASSERT_EQUAL(BADGE_CON_VHCI_OFF, snapshot.state);
    TEST_ASSERT_TRUE(badge_con_vhci_policy_radio_quiesced(&policy));
}

void test_badge_con_vhci_policy_emits_super_only_at_existing_epoch(void)
{
    badge_con_vhci_policy_t policy;
    fake_vhci_transport_t fake;
    prepare_policy(&policy, &fake);
    drive_to_advertising(&policy, &fake, 100U);

    badge_con_vhci_policy_set_identity_state(
        &policy, BADGE_CON_ROLE_INFECTED, false);
    badge_con_vhci_policy_poll(&policy, 1099U);
    TEST_ASSERT_EQUAL_UINT32(3U, fake.count);
    badge_con_vhci_policy_poll(&policy, 1100U);
    TEST_ASSERT_EQUAL_UINT32(4U, fake.count);
    TEST_ASSERT_EQUAL_HEX16(0x2008U, recorded_opcode(&fake, 3U));
    badge_con_packet_t regular = {0};
    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_VALID,
        badge_con_parse_advertisement(
            fake.commands[3] + 5U, BADGE_CON_LEGACY_ADV_BYTES,
            -45, &regular));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, regular.role);
    TEST_ASSERT_FALSE(regular.super);
    TEST_ASSERT_EQUAL_HEX8(0x2BU, regular.sequence);
    complete_command(&policy, 0x2008U, 0U, 1100U);

    badge_con_vhci_policy_set_identity_state(
        &policy, BADGE_CON_ROLE_INFECTED, true);
    badge_con_vhci_policy_poll(&policy, 2099U);
    TEST_ASSERT_EQUAL_UINT32(4U, fake.count);
    badge_con_vhci_policy_poll(&policy, 2100U);
    TEST_ASSERT_EQUAL_UINT32(5U, fake.count);
    TEST_ASSERT_EQUAL_HEX16(0x2008U, recorded_opcode(&fake, 4U));
    badge_con_packet_t super = {0};
    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_VALID,
        badge_con_parse_advertisement(
            fake.commands[4] + 5U, BADGE_CON_LEGACY_ADV_BYTES,
            -45, &super));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, super.role);
    TEST_ASSERT_TRUE(super.super);
    TEST_ASSERT_EQUAL_HEX8(0x2CU, super.sequence);
    complete_command(&policy, 0x2008U, 0U, 2100U);

    badge_con_vhci_policy_set_identity_state(
        &policy, BADGE_CON_ROLE_NORMAL, true);
    badge_con_vhci_policy_poll(&policy, 3100U);
    TEST_ASSERT_EQUAL_UINT32(6U, fake.count);
    badge_con_packet_t unchanged = {0};
    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_VALID,
        badge_con_parse_advertisement(
            fake.commands[5] + 5U, BADGE_CON_LEGACY_ADV_BYTES,
            -45, &unchanged));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, unchanged.role);
    TEST_ASSERT_TRUE(unchanged.super);
}

void test_badge_con_vhci_policy_retries_twice_then_fails_terminally(void)
{
    badge_con_vhci_policy_t policy;
    fake_vhci_transport_t fake;
    prepare_policy(&policy, &fake);
    badge_con_vhci_policy_set_game_active(&policy, true);
    badge_con_vhci_policy_set_self_ready(&policy, true);
    badge_con_vhci_policy_set_inhibited(&policy, false);

    badge_con_vhci_policy_poll(&policy, 0U);
    complete_command(&policy, 0x2006U, 0x0CU, 1U);
    badge_con_vhci_policy_poll(&policy, 1U);
    command_status(&policy, 0x2006U, 0x0CU, 2U);
    badge_con_vhci_policy_poll(&policy, 2U);
    complete_command(&policy, 0x2006U, 0x0CU, 3U);

    TEST_ASSERT_EQUAL_UINT32(3U, fake.count);
    TEST_ASSERT_EQUAL_HEX16(0x2006U, recorded_opcode(&fake, 0U));
    TEST_ASSERT_EQUAL_HEX16(0x2006U, recorded_opcode(&fake, 1U));
    TEST_ASSERT_EQUAL_HEX16(0x2006U, recorded_opcode(&fake, 2U));

    badge_con_vhci_snapshot_t snapshot = {0};
    badge_con_vhci_policy_snapshot(&policy, &snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_VHCI_FAILED, snapshot.state);
    TEST_ASSERT_FALSE(snapshot.advertising);
    TEST_ASSERT_EQUAL_UINT8(2U, snapshot.retries);
    TEST_ASSERT_EQUAL_STRING("hci_status", snapshot.failure);
}

void test_badge_con_vhci_policy_times_out_bounded_and_ignores_other_opcodes(void)
{
    badge_con_vhci_policy_t policy;
    fake_vhci_transport_t fake;
    prepare_policy(&policy, &fake);
    badge_con_vhci_policy_set_game_active(&policy, true);
    badge_con_vhci_policy_set_self_ready(&policy, true);
    badge_con_vhci_policy_set_inhibited(&policy, false);

    badge_con_vhci_policy_poll(&policy, UINT32_MAX - 100U);
    complete_command(&policy, 0x2008U, 0U, UINT32_MAX - 90U);
    badge_con_vhci_policy_poll(
        &policy,
        (uint32_t)(UINT32_MAX - 100U +
                   BADGE_CON_VHCI_COMMAND_DEADLINE_MS - 1U));
    TEST_ASSERT_EQUAL_UINT32(1U, fake.count);
    badge_con_vhci_policy_poll(
        &policy,
        (uint32_t)(UINT32_MAX - 100U +
                   BADGE_CON_VHCI_COMMAND_DEADLINE_MS));
    TEST_ASSERT_EQUAL_UINT32(2U, fake.count);

    badge_con_vhci_policy_poll(
        &policy,
        (uint32_t)(UINT32_MAX - 100U +
                   (2U * BADGE_CON_VHCI_COMMAND_DEADLINE_MS)));
    TEST_ASSERT_EQUAL_UINT32(3U, fake.count);
    badge_con_vhci_policy_poll(
        &policy,
        (uint32_t)(UINT32_MAX - 100U +
                   (3U * BADGE_CON_VHCI_COMMAND_DEADLINE_MS)));

    badge_con_vhci_snapshot_t snapshot = {0};
    badge_con_vhci_policy_snapshot(&policy, &snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_VHCI_FAILED, snapshot.state);
    TEST_ASSERT_EQUAL_STRING("hci_timeout", snapshot.failure);
}

void test_badge_con_vhci_epoch_gate_rejects_stale_or_equal_permits(void)
{
    badge_con_vhci_epoch_gate_t gate = {0};

    TEST_ASSERT_TRUE(badge_con_vhci_epoch_gate_apply(
        &gate, 10U, false));
    TEST_ASSERT_FALSE(badge_con_vhci_epoch_gate_matches_inhibit(
        &gate, 10U));

    TEST_ASSERT_TRUE(badge_con_vhci_epoch_gate_apply(
        &gate, 11U, true));
    TEST_ASSERT_TRUE(badge_con_vhci_epoch_gate_matches_inhibit(
        &gate, 11U));

    TEST_ASSERT_FALSE(badge_con_vhci_epoch_gate_apply(
        &gate, 10U, false));
    TEST_ASSERT_FALSE(badge_con_vhci_epoch_gate_apply(
        &gate, 11U, false));
    TEST_ASSERT_TRUE(badge_con_vhci_epoch_gate_matches_inhibit(
        &gate, 11U));

    TEST_ASSERT_TRUE(badge_con_vhci_epoch_gate_apply(
        &gate, 12U, false));
    TEST_ASSERT_FALSE(badge_con_vhci_epoch_gate_matches_inhibit(
        &gate, 11U));
    TEST_ASSERT_FALSE(badge_con_vhci_epoch_gate_matches_inhibit(
        &gate, 12U));
}

void test_badge_con_vhci_epoch_gate_accepts_wrap_safe_newer_inhibit(void)
{
    badge_con_vhci_epoch_gate_t gate = {0};

    TEST_ASSERT_TRUE(badge_con_vhci_epoch_gate_apply(
        &gate, UINT32_MAX, false));
    TEST_ASSERT_TRUE(badge_con_vhci_epoch_gate_apply(
        &gate, 1U, true));
    TEST_ASSERT_TRUE(badge_con_vhci_epoch_gate_matches_inhibit(
        &gate, 1U));
}

void test_badge_con_vhci_policy_never_reports_quiet_with_command_in_flight(void)
{
    badge_con_vhci_policy_t policy;
    fake_vhci_transport_t fake;
    prepare_policy(&policy, &fake);
    badge_con_vhci_policy_set_game_active(&policy, true);
    badge_con_vhci_policy_set_self_ready(&policy, true);
    badge_con_vhci_policy_set_inhibited(&policy, false);

    badge_con_vhci_policy_poll(&policy, 100U);
    TEST_ASSERT_EQUAL_HEX16(0x2006U, recorded_opcode(&fake, 0U));

    badge_con_vhci_policy_set_inhibited(&policy, true);
    TEST_ASSERT_FALSE(badge_con_vhci_policy_radio_quiesced(&policy));

    complete_command(&policy, 0x2006U, 0U, 101U);
    badge_con_vhci_policy_poll(&policy, 101U);
    TEST_ASSERT_TRUE(badge_con_vhci_policy_radio_quiesced(&policy));
}
