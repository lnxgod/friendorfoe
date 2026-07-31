#include "badge_con_vhci_policy.h"

#include <string.h>

#define HCI_COMMAND_PACKET 0x01U
#define HCI_EVENT_PACKET 0x04U
#define HCI_EVENT_COMMAND_COMPLETE 0x0EU
#define HCI_EVENT_COMMAND_STATUS 0x0FU
#define HCI_LE_SET_ADVERTISING_PARAMETERS 0x2006U
#define HCI_LE_SET_ADVERTISING_DATA 0x2008U
#define HCI_LE_SET_ADVERTISING_ENABLE 0x200AU

static bool role_is_valid(badge_con_role_t role)
{
    return role == BADGE_CON_ROLE_NORMAL ||
           role == BADGE_CON_ROLE_INFECTED ||
           role == BADGE_CON_ROLE_IMMUNE;
}

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool epoch_is_newer(uint32_t candidate, uint32_t current)
{
    if (candidate == current) {
        return false;
    }
    if (current == 0U) {
        return candidate != 0U;
    }
    if (candidate == 0U) {
        return false;
    }
    return (int32_t)(candidate - current) > 0;
}

bool badge_con_vhci_epoch_gate_apply(
    badge_con_vhci_epoch_gate_t *gate,
    uint32_t epoch,
    bool inhibited)
{
    if (!gate) {
        return false;
    }
    if (!gate->observed || epoch_is_newer(epoch, gate->epoch)) {
        gate->epoch = epoch;
        gate->observed = true;
        gate->inhibited = inhibited;
        return true;
    }
    if (epoch != gate->epoch) {
        return false;
    }
    if (gate->inhibited && !inhibited) {
        return false;
    }
    gate->inhibited = gate->inhibited || inhibited;
    return true;
}

bool badge_con_vhci_epoch_gate_matches_inhibit(
    const badge_con_vhci_epoch_gate_t *gate,
    uint32_t epoch)
{
    return gate && gate->observed && gate->inhibited &&
           gate->epoch == epoch;
}

static bool desired_advertising(const badge_con_vhci_policy_t *policy)
{
    return policy && policy->controller_initialized &&
           policy->game_active && policy->self_ready &&
           !policy->inhibited;
}

static void enter_state(badge_con_vhci_policy_t *policy,
                        badge_con_vhci_state_t state,
                        bool refresh_while_advertising)
{
    if (!policy) {
        return;
    }
    policy->state = state;
    policy->command_in_flight = false;
    policy->pending_opcode = 0U;
    policy->command_deadline_ms = 0U;
    policy->retries = 0U;
    policy->refresh_while_advertising = refresh_while_advertising;
}

void badge_con_vhci_policy_fail(badge_con_vhci_policy_t *policy,
                                const char *failure)
{
    if (!policy || policy->state == BADGE_CON_VHCI_FAILED) {
        return;
    }
    policy->state = BADGE_CON_VHCI_FAILED;
    policy->command_in_flight = false;
    policy->pending_opcode = 0U;
    policy->command_deadline_ms = 0U;
    policy->failure = failure ? failure : "failed";
}

void badge_con_vhci_policy_init(
    badge_con_vhci_policy_t *policy,
    uint32_t peer,
    uint8_t session,
    uint8_t initial_sequence,
    const badge_con_vhci_transport_t *transport)
{
    if (!policy) {
        return;
    }
    memset(policy, 0, sizeof(*policy));
    policy->state = BADGE_CON_VHCI_INIT_CONTROLLER;
    policy->role = BADGE_CON_ROLE_NORMAL;
    policy->peer = peer;
    policy->session = session;
    policy->sequence = initial_sequence;
    policy->inhibited = true;
    if (transport) {
        policy->transport = *transport;
    }

    uint8_t payload[BADGE_CON_SERVICE_PAYLOAD_BYTES] = {0};
    if (!badge_con_build_service_payload(
            policy->role, false,
            peer, session, initial_sequence, payload)) {
        badge_con_vhci_policy_fail(policy, "identity");
    } else if (!policy->transport.send) {
        badge_con_vhci_policy_fail(policy, "transport");
    }
}

void badge_con_vhci_policy_set_controller_initialized(
    badge_con_vhci_policy_t *policy, bool initialized)
{
    if (!policy || policy->state == BADGE_CON_VHCI_FAILED) {
        return;
    }
    if (!initialized && policy->controller_initialized) {
        policy->radio_state_uncertain =
            policy->advertising || policy->command_in_flight;
        badge_con_vhci_policy_fail(policy, "controller_lost");
        return;
    }
    policy->controller_initialized = initialized;
    if (initialized && policy->state == BADGE_CON_VHCI_INIT_CONTROLLER) {
        enter_state(policy, BADGE_CON_VHCI_OFF, false);
    }
}

void badge_con_vhci_policy_set_identity_state(
    badge_con_vhci_policy_t *policy,
    badge_con_role_t role,
    bool super)
{
    if (policy && role_is_valid(role) &&
        (!super || role == BADGE_CON_ROLE_INFECTED)) {
        policy->role = role;
        policy->super = super;
    }
}

void badge_con_vhci_policy_set_game_active(
    badge_con_vhci_policy_t *policy, bool active)
{
    if (policy) {
        policy->game_active = active;
    }
}

void badge_con_vhci_policy_set_self_ready(
    badge_con_vhci_policy_t *policy, bool ready)
{
    if (policy) {
        policy->self_ready = ready;
    }
}

void badge_con_vhci_policy_set_inhibited(
    badge_con_vhci_policy_t *policy, bool inhibited)
{
    if (policy) {
        policy->inhibited = inhibited;
    }
}

static bool build_command(const badge_con_vhci_policy_t *policy,
                          uint8_t command[36],
                          size_t *size_out,
                          uint16_t *opcode_out)
{
    if (!policy || !command || !size_out || !opcode_out) {
        return false;
    }
    memset(command, 0, 36U);
    command[0] = HCI_COMMAND_PACKET;

    switch (policy->state) {
    case BADGE_CON_VHCI_SET_PARAMS: {
        static const uint8_t parameters[] = {
            HCI_COMMAND_PACKET, 0x06U, 0x20U, 0x0FU,
            0x40U, 0x06U, 0x40U, 0x06U,
            0x03U, /* ADV_NONCONN_IND: non-connectable, non-scannable */
            0x00U, /* public own address */
            0x00U, /* public direct address type; ignored for ADV_NONCONN_IND */
            0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
            0x07U, /* all three primary advertising channels */
            0x00U, /* allow all */
        };
        memcpy(command, parameters, sizeof(parameters));
        *size_out = sizeof(parameters);
        *opcode_out = HCI_LE_SET_ADVERTISING_PARAMETERS;
        return true;
    }
    case BADGE_CON_VHCI_SET_DATA:
        command[1] = 0x08U;
        command[2] = 0x20U;
        command[3] = 32U;
        command[4] = BADGE_CON_LEGACY_ADV_BYTES;
        if (!badge_con_build_legacy_advertisement(
                policy->role, policy->super,
                policy->peer, policy->session,
                policy->sequence, command + 5U)) {
            return false;
        }
        *size_out = 36U;
        *opcode_out = HCI_LE_SET_ADVERTISING_DATA;
        return true;
    case BADGE_CON_VHCI_ENABLE:
    case BADGE_CON_VHCI_DISABLE:
        command[1] = 0x0AU;
        command[2] = 0x20U;
        command[3] = 1U;
        command[4] =
            policy->state == BADGE_CON_VHCI_ENABLE ? 1U : 0U;
        *size_out = 5U;
        *opcode_out = HCI_LE_SET_ADVERTISING_ENABLE;
        return true;
    default:
        return false;
    }
}

static void attempt_command(badge_con_vhci_policy_t *policy,
                            uint32_t now_ms)
{
    if (!policy || policy->command_in_flight ||
        policy->state == BADGE_CON_VHCI_FAILED) {
        return;
    }
    uint8_t command[36] = {0};
    size_t command_size = 0U;
    uint16_t opcode = 0U;
    if (!build_command(policy, command, &command_size, &opcode)) {
        badge_con_vhci_policy_fail(policy, "command_build");
        return;
    }
    if (policy->transport.send(
            policy->transport.context, command, command_size)) {
        policy->command_in_flight = true;
        policy->pending_opcode = opcode;
        policy->command_deadline_ms =
            now_ms + BADGE_CON_VHCI_COMMAND_DEADLINE_MS;
        if (policy->state == BADGE_CON_VHCI_ENABLE) {
            policy->radio_state_uncertain = true;
        }
    } else if (policy->command_deadline_ms == 0U) {
        policy->command_deadline_ms =
            now_ms + BADGE_CON_VHCI_COMMAND_DEADLINE_MS;
    }
}

static void command_failed(badge_con_vhci_policy_t *policy,
                           const char *failure)
{
    if (!policy || policy->state == BADGE_CON_VHCI_FAILED) {
        return;
    }
    policy->command_in_flight = false;
    policy->pending_opcode = 0U;
    policy->command_deadline_ms = 0U;
    if (policy->retries >= BADGE_CON_VHCI_MAX_RETRIES) {
        badge_con_vhci_policy_fail(policy, failure);
        return;
    }
    policy->retries++;
}

static void command_succeeded(badge_con_vhci_policy_t *policy,
                              uint32_t now_ms)
{
    if (!policy) {
        return;
    }
    badge_con_vhci_state_t completed = policy->state;
    bool refreshed = policy->refresh_while_advertising;
    policy->command_in_flight = false;
    policy->pending_opcode = 0U;
    policy->command_deadline_ms = 0U;
    policy->retries = 0U;

    switch (completed) {
    case BADGE_CON_VHCI_SET_PARAMS:
        enter_state(policy, BADGE_CON_VHCI_SET_DATA, false);
        break;
    case BADGE_CON_VHCI_SET_DATA:
        if (refreshed) {
            policy->last_frame_ms = now_ms;
            enter_state(policy, BADGE_CON_VHCI_ADVERTISING, false);
        } else {
            enter_state(policy, BADGE_CON_VHCI_ENABLE, false);
        }
        break;
    case BADGE_CON_VHCI_ENABLE:
        policy->advertising = true;
        policy->radio_state_uncertain = false;
        policy->last_frame_ms = now_ms;
        enter_state(policy, BADGE_CON_VHCI_ADVERTISING, false);
        break;
    case BADGE_CON_VHCI_DISABLE:
        policy->advertising = false;
        policy->radio_state_uncertain = false;
        enter_state(policy, BADGE_CON_VHCI_OFF, false);
        break;
    default:
        badge_con_vhci_policy_fail(policy, "state");
        break;
    }
}

void badge_con_vhci_policy_on_hci_event(
    badge_con_vhci_policy_t *policy,
    const uint8_t *bytes,
    size_t size,
    uint32_t now_ms)
{
    if (!policy || !policy->command_in_flight || !bytes ||
        size < 3U || bytes[0] != HCI_EVENT_PACKET) {
        return;
    }
    size_t packet_size = (size_t)bytes[2] + 3U;
    if (packet_size != size) {
        return;
    }

    uint16_t opcode;
    uint8_t status;
    if (bytes[1] == HCI_EVENT_COMMAND_COMPLETE && size >= 7U &&
        bytes[2] >= 4U) {
        opcode = (uint16_t)bytes[4] |
                 (uint16_t)((uint16_t)bytes[5] << 8U);
        status = bytes[6];
    } else if (bytes[1] == HCI_EVENT_COMMAND_STATUS &&
               size >= 7U && bytes[2] == 4U) {
        status = bytes[3];
        opcode = (uint16_t)bytes[5] |
                 (uint16_t)((uint16_t)bytes[6] << 8U);
    } else {
        return;
    }

    if (opcode != policy->pending_opcode) {
        return;
    }
    if (status != 0U) {
        if (policy->state == BADGE_CON_VHCI_ENABLE) {
            policy->radio_state_uncertain = false;
        }
        command_failed(policy, "hci_status");
        return;
    }
    command_succeeded(policy, now_ms);
}

void badge_con_vhci_policy_poll(badge_con_vhci_policy_t *policy,
                                uint32_t now_ms)
{
    if (!policy || policy->state == BADGE_CON_VHCI_FAILED ||
        policy->state == BADGE_CON_VHCI_INIT_CONTROLLER) {
        return;
    }

    if (policy->command_in_flight) {
        if (!deadline_reached(now_ms, policy->command_deadline_ms)) {
            return;
        }
        command_failed(policy, "hci_timeout");
        if (policy->state == BADGE_CON_VHCI_FAILED) {
            return;
        }
    } else if (policy->command_deadline_ms != 0U &&
               deadline_reached(now_ms, policy->command_deadline_ms)) {
        command_failed(policy, "hci_unavailable");
        if (policy->state == BADGE_CON_VHCI_FAILED) {
            return;
        }
    }

    bool desired = desired_advertising(policy);
    if (!desired) {
        if (policy->command_in_flight) {
            return;
        }
        if (policy->advertising) {
            if (policy->state != BADGE_CON_VHCI_DISABLE) {
                enter_state(policy, BADGE_CON_VHCI_DISABLE, false);
            }
        } else if (policy->state != BADGE_CON_VHCI_OFF) {
            enter_state(policy, BADGE_CON_VHCI_OFF, false);
        }
    } else if (!policy->command_in_flight) {
        if (policy->state == BADGE_CON_VHCI_OFF) {
            enter_state(policy, BADGE_CON_VHCI_SET_PARAMS, false);
        } else if (policy->state == BADGE_CON_VHCI_ADVERTISING &&
                   (uint32_t)(now_ms - policy->last_frame_ms) >=
                       BADGE_CON_VHCI_PAYLOAD_EPOCH_MS) {
            policy->sequence++;
            enter_state(policy, BADGE_CON_VHCI_SET_DATA, true);
        }
    }

    if (policy->state == BADGE_CON_VHCI_SET_PARAMS ||
        policy->state == BADGE_CON_VHCI_SET_DATA ||
        policy->state == BADGE_CON_VHCI_ENABLE ||
        policy->state == BADGE_CON_VHCI_DISABLE) {
        attempt_command(policy, now_ms);
    }
}

void badge_con_vhci_policy_snapshot(
    const badge_con_vhci_policy_t *policy,
    badge_con_vhci_snapshot_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!policy) {
        out->state = BADGE_CON_VHCI_FAILED;
        out->failure = "invalid";
        return;
    }
    *out = (badge_con_vhci_snapshot_t) {
        .state = policy->state,
        .controller_initialized = policy->controller_initialized,
        .advertising = policy->advertising,
        .inhibited = policy->inhibited,
        .command_in_flight = policy->command_in_flight,
        .radio_state_uncertain = policy->radio_state_uncertain,
        .sequence = policy->sequence,
        .last_frame_ms = policy->last_frame_ms,
        .command_deadline_ms = policy->command_deadline_ms,
        .retries = policy->retries,
        .failure = policy->failure,
    };
}

bool badge_con_vhci_policy_radio_quiesced(
    const badge_con_vhci_policy_t *policy)
{
    return policy && !policy->advertising &&
           !policy->radio_state_uncertain &&
           !policy->command_in_flight &&
           (policy->state == BADGE_CON_VHCI_OFF ||
            policy->state == BADGE_CON_VHCI_FAILED);
}
