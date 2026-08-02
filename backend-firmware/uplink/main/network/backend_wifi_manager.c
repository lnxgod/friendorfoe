#include "backend_wifi_manager.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_wifi.h"
#endif

static bool elapsed_at_least(
    int64_t now_ms, int64_t started_ms, int64_t duration_ms)
{
    return started_ms >= 0 && now_ms >= started_ms &&
           now_ms - started_ms >= duration_ms;
}

static int64_t retry_delay_ms(uint8_t exponent)
{
    int64_t delay_ms = BACKEND_WIFI_RETRY_BASE_MS;
    for (uint8_t index = 0; index < exponent; ++index) {
        if (delay_ms >= BACKEND_WIFI_RETRY_CAP_MS / 2) {
            return BACKEND_WIFI_RETRY_CAP_MS;
        }
        delay_ms *= 2;
    }
    return delay_ms > BACKEND_WIFI_RETRY_CAP_MS
        ? BACKEND_WIFI_RETRY_CAP_MS : delay_ms;
}

void backend_wifi_policy_init(backend_wifi_policy_t *state)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->attempt_started_ms = -1;
    state->retry_after_ms = -1;
}

static backend_wifi_action_t reset_for_generation(
    backend_wifi_policy_t *state,
    const backend_config_record_t *config,
    int64_t now_ms)
{
    state->config_generation = config->generation;
    state->network_count = config->network_count;
    state->network_index = 0;
    state->retry_exponent = 0;
    state->attempt_started_ms = now_ms;
    state->retry_after_ms = -1;
    state->connected = false;
    return BACKEND_WIFI_CONNECT_NETWORK;
}

static backend_wifi_action_t schedule_retry(
    backend_wifi_policy_t *state, int64_t now_ms)
{
    const int64_t delay_ms = retry_delay_ms(state->retry_exponent);
    state->network_index = 0;
    state->connected = false;
    state->attempt_started_ms = -1;
    if (now_ms > INT64_MAX - delay_ms) {
        state->retry_after_ms = INT64_MAX;
    } else {
        state->retry_after_ms = now_ms + delay_ms;
    }
    if (delay_ms < BACKEND_WIFI_RETRY_CAP_MS &&
        state->retry_exponent < UINT8_MAX) {
        state->retry_exponent++;
    }
    return BACKEND_WIFI_WAIT_RETRY;
}

static backend_wifi_action_t advance_network(
    backend_wifi_policy_t *state, int64_t now_ms)
{
    if (state->network_index + 1U < state->network_count) {
        state->network_index++;
        state->connected = false;
        state->attempt_started_ms = now_ms;
        state->retry_after_ms = -1;
        return BACKEND_WIFI_CONNECT_NETWORK;
    }
    return schedule_retry(state, now_ms);
}

backend_wifi_action_t backend_wifi_policy_update(
    backend_wifi_policy_t *state,
    const backend_config_record_t *config,
    backend_wifi_event_t event,
    int64_t now_ms)
{
    if (!state || !config || config->network_count == 0 ||
        config->network_count > BACKEND_CONFIG_MAX_NETWORKS ||
        backend_config_validate(config) != BACKEND_CONFIG_VALID) {
        return BACKEND_WIFI_NO_CHANGE;
    }

    if (state->config_generation != config->generation ||
        state->network_count != config->network_count) {
        return reset_for_generation(state, config, now_ms);
    }

    switch (event) {
    case BACKEND_WIFI_EVENT_CONNECTED:
        state->connected = true;
        state->retry_exponent = 0;
        state->retry_after_ms = -1;
        return BACKEND_WIFI_NO_CHANGE;
    case BACKEND_WIFI_EVENT_AUTH_FAILED:
    case BACKEND_WIFI_EVENT_NO_AP:
        if (state->retry_after_ms >= 0) {
            return BACKEND_WIFI_WAIT_RETRY;
        }
        return advance_network(state, now_ms);
    case BACKEND_WIFI_EVENT_DISCONNECTED:
        if (!state->connected) {
            return BACKEND_WIFI_NO_CHANGE;
        }
        state->connected = false;
        state->network_index = 0;
        return schedule_retry(state, now_ms);
    case BACKEND_WIFI_EVENT_TICK:
        if (state->connected) {
            return BACKEND_WIFI_NO_CHANGE;
        }
        if (state->retry_after_ms >= 0) {
            if (now_ms < state->retry_after_ms) {
                return BACKEND_WIFI_NO_CHANGE;
            }
            state->retry_after_ms = -1;
            state->attempt_started_ms = now_ms;
            state->network_index = 0;
            return BACKEND_WIFI_CONNECT_NETWORK;
        }
        if (elapsed_at_least(
                now_ms,
                state->attempt_started_ms,
                BACKEND_WIFI_ATTEMPT_TIMEOUT_MS)) {
            return advance_network(state, now_ms);
        }
        return BACKEND_WIFI_NO_CHANGE;
    default:
        return BACKEND_WIFI_NO_CHANGE;
    }
}

size_t backend_wifi_policy_render_status(
    const backend_wifi_policy_t *state,
    char *output,
    size_t capacity)
{
    if (!state || !output || capacity == 0) {
        return 0;
    }
    output[0] = '\0';
    const int written = snprintf(
        output,
        capacity,
        "{\"config_generation\":%lu,\"network_count\":%u,"
        "\"network_index\":%u,\"connected\":%s,"
        "\"retry_after_ms\":%lld}",
        (unsigned long)state->config_generation,
        (unsigned)state->network_count,
        (unsigned)state->network_index,
        state->connected ? "true" : "false",
        (long long)state->retry_after_ms);
    if (written < 0 || (size_t)written >= capacity) {
        output[0] = '\0';
        return 0;
    }
    return (size_t)written;
}

static bool apply_action(
    backend_wifi_manager_t *manager, backend_wifi_action_t action)
{
    if (action != BACKEND_WIFI_CONNECT_NETWORK) {
        return true;
    }
    if (!manager || manager->policy.network_index >=
                        manager->config.network_count) {
        return false;
    }
#ifdef ESP_PLATFORM
    const backend_wifi_network_t *network =
        &manager->config.networks[manager->policy.network_index];
    wifi_config_t station_config;
    memset(&station_config, 0, sizeof(station_config));
    memcpy(station_config.sta.ssid, network->ssid, strlen(network->ssid));
    memcpy(
        station_config.sta.password,
        network->password,
        strlen(network->password));
    station_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    station_config.sta.pmf_cfg.capable = true;
    station_config.sta.pmf_cfg.required = false;
    if (esp_wifi_disconnect() != ESP_OK && manager->policy.connected) {
        return false;
    }
    if (esp_wifi_set_config(WIFI_IF_STA, &station_config) != ESP_OK ||
        esp_wifi_connect() != ESP_OK) {
        return false;
    }
#endif
    return true;
}

bool backend_wifi_manager_init(
    backend_wifi_manager_t *manager,
    const backend_config_record_t *config,
    int64_t now_ms)
{
    if (!manager || !config || config->network_count == 0 ||
        backend_config_validate(config) != BACKEND_CONFIG_VALID) {
        return false;
    }
    memset(manager, 0, sizeof(*manager));
    manager->config = *config;
    backend_wifi_policy_init(&manager->policy);
    manager->initialized = true;
    const backend_wifi_action_t action = backend_wifi_policy_update(
        &manager->policy, &manager->config, BACKEND_WIFI_EVENT_TICK, now_ms);
    return apply_action(manager, action);
}

bool backend_wifi_manager_handle_event(
    backend_wifi_manager_t *manager,
    backend_wifi_event_t event,
    int64_t now_ms)
{
    if (!manager || !manager->initialized) {
        return false;
    }
    const backend_wifi_action_t action = backend_wifi_policy_update(
        &manager->policy, &manager->config, event, now_ms);
    return apply_action(manager, action);
}

bool backend_wifi_manager_apply_committed_config(
    backend_wifi_manager_t *manager,
    const backend_config_record_t *committed,
    int64_t now_ms)
{
    if (!manager || !manager->initialized || !committed ||
        committed->network_count == 0 ||
        backend_config_validate(committed) != BACKEND_CONFIG_VALID) {
        return false;
    }
    manager->config = *committed;
    const backend_wifi_action_t action = backend_wifi_policy_update(
        &manager->policy,
        &manager->config,
        BACKEND_WIFI_EVENT_TICK,
        now_ms);
    return action == BACKEND_WIFI_CONNECT_NETWORK &&
           apply_action(manager, action);
}

const backend_wifi_network_t *backend_wifi_manager_active_network(
    const backend_wifi_manager_t *manager)
{
    if (!manager || !manager->initialized ||
        manager->policy.network_index >= manager->config.network_count) {
        return NULL;
    }
    return &manager->config.networks[manager->policy.network_index];
}
