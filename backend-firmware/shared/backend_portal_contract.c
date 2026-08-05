#include "backend_portal_contract.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "backend_json_reader.h"
#include "backend_json_writer.h"

static const backend_portal_route_t REQUIRED_ROUTES[] = {
    { BACKEND_PORTAL_GET, "/", BACKEND_PORTAL_ROOT },
    { BACKEND_PORTAL_GET, "/api/status", BACKEND_PORTAL_STATUS },
    { BACKEND_PORTAL_GET, "/api/config", BACKEND_PORTAL_CONFIG_GET },
    { BACKEND_PORTAL_POST, "/api/config", BACKEND_PORTAL_CONFIG_POST },
    { BACKEND_PORTAL_POST, "/api/backend/test",
      BACKEND_PORTAL_BACKEND_TEST },
};

#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
static const backend_portal_route_t DASHBOARD_ROUTES[] = {
    { BACKEND_PORTAL_GET, "/dashboard", BACKEND_PORTAL_DASHBOARD },
    { BACKEND_PORTAL_GET, "/api/dashboard/status",
      BACKEND_PORTAL_DASHBOARD_STATUS },
    { BACKEND_PORTAL_GET, "/api/events", BACKEND_PORTAL_EVENTS },
};
#endif

const backend_portal_route_t *backend_portal_required_routes(
    size_t *out_count)
{
    if (out_count) {
        *out_count =
            sizeof(REQUIRED_ROUTES) / sizeof(REQUIRED_ROUTES[0]);
    }
    return REQUIRED_ROUTES;
}

const backend_portal_route_t *backend_portal_dashboard_routes(
    size_t *out_count)
{
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    if (out_count) {
        *out_count =
            sizeof(DASHBOARD_ROUTES) / sizeof(DASHBOARD_ROUTES[0]);
    }
    return DASHBOARD_ROUTES;
#else
    if (out_count) {
        *out_count = 0U;
    }
    return NULL;
#endif
}

bool backend_portal_route_lookup(
    backend_portal_method_t method,
    const char *path,
    backend_portal_route_id_t *out)
{
    if (!path || !out) {
        return false;
    }
    size_t route_count = 0U;
    const backend_portal_route_t *routes =
        backend_portal_required_routes(&route_count);
    for (size_t index = 0; index < route_count; ++index) {
        if (routes[index].method == method &&
            strcmp(routes[index].path, path) == 0) {
            *out = routes[index].id;
            return true;
        }
    }
    routes = backend_portal_dashboard_routes(&route_count);
    for (size_t index = 0; index < route_count; ++index) {
        if (routes[index].method == method &&
            strcmp(routes[index].path, path) == 0) {
            *out = routes[index].id;
            return true;
        }
    }
    return false;
}

static bool append_bool(backend_json_writer_t *writer, bool value)
{
    return backend_json_append(writer, value ? "true" : "false");
}

size_t backend_portal_render_redacted_config(
    const backend_config_record_t *config,
    char *output,
    size_t capacity)
{
    if (!output || capacity == 0) {
        return 0;
    }
    output[0] = '\0';
    if (!config || config->network_count > BACKEND_CONFIG_MAX_NETWORKS) {
        return 0;
    }

    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    if (!backend_json_append_format(
            &writer,
            "{\"schema_version\":%u,\"generation\":%lu,\"networks\":[",
            (unsigned)config->schema_version,
            (unsigned long)config->generation)) {
        return 0;
    }
    for (uint8_t index = 0; index < config->network_count; ++index) {
        if ((index != 0 && !backend_json_append(&writer, ",")) ||
            !backend_json_append(&writer, "{\"ssid\":") ||
            !backend_json_append_escaped(
                &writer, config->networks[index].ssid) ||
            !backend_json_append(&writer, ",\"password_set\":") ||
            !append_bool(
                &writer, config->networks[index].password[0] != '\0') ||
            !backend_json_append(&writer, "}")) {
            return 0;
        }
    }
    if (!backend_json_append(&writer, "],\"backend_url\":") ||
        !backend_json_append_escaped(&writer, config->backend_url) ||
        !backend_json_append(&writer, ",\"device_id\":") ||
        !backend_json_append_escaped(&writer, config->device_id) ||
        !backend_json_append(&writer, ",\"display_name\":") ||
        !backend_json_append_escaped(&writer, config->display_name) ||
        !backend_json_append(&writer, ",\"ap_password_set\":") ||
        !append_bool(&writer, config->ap_password[0] != '\0') ||
        !backend_json_append(&writer, ",\"auto_update_enabled\":") ||
        !append_bool(&writer, config->auto_update_enabled) ||
        !backend_json_append(&writer, ",\"has_location\":") ||
        !append_bool(&writer, config->has_location)) {
        return 0;
    }
    if (config->has_location) {
        if (!isfinite(config->latitude) || !isfinite(config->longitude) ||
            !isfinite(config->altitude_m) ||
            !backend_json_append_format(
                &writer,
                ",\"latitude\":%.8f,\"longitude\":%.8f,"
                "\"altitude_m\":%.3f}",
                config->latitude,
                config->longitude,
                (double)config->altitude_m)) {
            return 0;
        }
    } else if (!backend_json_append(
                   &writer,
                   ",\"latitude\":null,\"longitude\":null,"
                   "\"altitude_m\":null}")) {
        return 0;
    }
    return backend_json_writer_finish(&writer);
}

static bool token_string_equal(
    const char *json,
    const backend_json_token_t *token,
    const char *expected)
{
    char key[40];
    return backend_json_copy_string(json, token, key, sizeof(key)) &&
           strcmp(key, expected) == 0;
}

static bool root_key_allowed(
    const char *json, const backend_json_token_t *token)
{
    static const char *const allowed[] = {
        "networks",
        "backend_url",
        "display_name",
        "ap_password",
        "auto_update_enabled",
        "confirm_auto_update",
        "has_location",
        "latitude",
        "longitude",
        "altitude_m",
    };
    for (size_t index = 0; index < sizeof(allowed) / sizeof(allowed[0]);
         ++index) {
        if (token_string_equal(json, token, allowed[index])) {
            return true;
        }
    }
    return false;
}

static bool object_has_only_allowed_keys(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    size_t object_index,
    bool (*allowed)(const char *, const backend_json_token_t *))
{
    if (!json || !tokens || object_index >= token_count || !allowed ||
        tokens[object_index].kind != BACKEND_JSON_OBJECT ||
        (tokens[object_index].child_count & 1U) != 0U) {
        return false;
    }
    size_t direct_child = 0;
    for (size_t index = object_index + 1U; index < token_count; ++index) {
        if (tokens[index].parent != (int16_t)object_index) {
            continue;
        }
        if ((direct_child & 1U) == 0U &&
            (tokens[index].kind != BACKEND_JSON_STRING ||
             !allowed(json, &tokens[index]))) {
            return false;
        }
        direct_child++;
    }
    return direct_child == tokens[object_index].child_count;
}

static bool object_find(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    size_t object_index,
    const char *key,
    size_t *value_index,
    bool *present)
{
    if (!value_index || !present) {
        return false;
    }
    *present = backend_json_object_find(
        json, tokens, token_count, object_index, key, value_index);
    return true;
}

static bool copy_optional_string(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *key,
    char *output,
    size_t capacity)
{
    size_t value_index = 0;
    bool present = false;
    if (!object_find(
            json, tokens, token_count, 0, key,
            &value_index, &present)) {
        return false;
    }
    return !present || backend_json_copy_string(
        json, &tokens[value_index], output, capacity);
}

static bool get_optional_bool(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *key,
    bool *value,
    bool *present)
{
    size_t value_index = 0;
    if (!object_find(
            json, tokens, token_count, 0, key,
            &value_index, present)) {
        return false;
    }
    return !*present || backend_json_get_bool(
        json, &tokens[value_index], value);
}

static bool get_optional_double(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *key,
    double *value,
    bool *present)
{
    size_t value_index = 0;
    if (!object_find(
            json, tokens, token_count, 0, key,
            &value_index, present)) {
        return false;
    }
    return !*present || backend_json_get_double(
        json, &tokens[value_index], value);
}

static bool network_key_allowed(
    const char *json, const backend_json_token_t *token)
{
    return token_string_equal(json, token, "ssid") ||
           token_string_equal(json, token, "password");
}

static bool parse_networks(
    const backend_config_record_t *current,
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    backend_config_record_t *candidate,
    bool *present)
{
    size_t array_index = 0;
    if (!object_find(
            json, tokens, token_count, 0, "networks",
            &array_index, present)) {
        return false;
    }
    if (!*present) {
        return true;
    }
    if (tokens[array_index].kind != BACKEND_JSON_ARRAY ||
        tokens[array_index].child_count > BACKEND_CONFIG_MAX_NETWORKS) {
        return false;
    }

    backend_wifi_network_t parsed[BACKEND_CONFIG_MAX_NETWORKS];
    memset(parsed, 0, sizeof(parsed));
    uint8_t parsed_count = 0;
    for (size_t index = array_index + 1U; index < token_count; ++index) {
        if (tokens[index].parent != (int16_t)array_index) {
            continue;
        }
        if (parsed_count >= BACKEND_CONFIG_MAX_NETWORKS ||
            tokens[index].kind != BACKEND_JSON_OBJECT ||
            !object_has_only_allowed_keys(
                json, tokens, token_count, index, network_key_allowed)) {
            return false;
        }
        size_t ssid_index = 0;
        if (!backend_json_object_find(
                json, tokens, token_count, index, "ssid", &ssid_index) ||
            !backend_json_copy_string(
                json,
                &tokens[ssid_index],
                parsed[parsed_count].ssid,
                sizeof(parsed[parsed_count].ssid)) ||
            parsed[parsed_count].ssid[0] == '\0') {
            return false;
        }

        size_t password_index = 0;
        if (backend_json_object_find(
                json, tokens, token_count, index,
                "password", &password_index)) {
            if (!backend_json_copy_string(
                    json,
                    &tokens[password_index],
                    parsed[parsed_count].password,
                    sizeof(parsed[parsed_count].password))) {
                return false;
            }
        } else {
            for (uint8_t saved = 0; saved < current->network_count;
                 ++saved) {
                if (strcmp(
                        parsed[parsed_count].ssid,
                        current->networks[saved].ssid) != 0) {
                    continue;
                }
                memcpy(
                    parsed[parsed_count].password,
                    current->networks[saved].password,
                    sizeof(parsed[parsed_count].password));
                break;
            }
        }
        parsed_count++;
    }
    if (parsed_count != tokens[array_index].child_count) {
        return false;
    }
    memset(candidate->networks, 0, sizeof(candidate->networks));
    memcpy(candidate->networks, parsed, sizeof(parsed));
    candidate->network_count = parsed_count;
    return true;
}

backend_portal_update_result_t backend_portal_parse_config_update(
    const backend_config_record_t *current,
    const char *json,
    size_t length,
    backend_config_record_t *out_candidate)
{
    if (!current || !json || !out_candidate || length == 0 ||
        length > BACKEND_PORTAL_CONFIG_BODY_MAX ||
        current->generation == UINT32_MAX) {
        return BACKEND_PORTAL_UPDATE_INVALID_ARGUMENT;
    }

    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    size_t token_count = 0;
    if (backend_json_parse(
            json, length, tokens,
            sizeof(tokens) / sizeof(tokens[0]), &token_count) !=
            BACKEND_JSON_OK ||
        token_count == 0 || tokens[0].kind != BACKEND_JSON_OBJECT) {
        return BACKEND_PORTAL_UPDATE_INVALID_JSON;
    }
    if (!object_has_only_allowed_keys(
            json, tokens, token_count, 0, root_key_allowed)) {
        return BACKEND_PORTAL_UPDATE_UNKNOWN_FIELD;
    }

    backend_config_record_t candidate = *current;
    candidate.schema_version = BACKEND_CONFIG_SCHEMA_VERSION;
    candidate.generation = current->generation + 1U;
    if (candidate.ap_password[0] == '\0') {
        static const char default_password[] = "friendorfoe";
        memcpy(
            candidate.ap_password,
            default_password,
            sizeof(default_password));
    }

    bool networks_present = false;
    if (!parse_networks(
            current, json, tokens, token_count,
            &candidate, &networks_present) ||
        !copy_optional_string(
            json, tokens, token_count, "backend_url",
            candidate.backend_url, sizeof(candidate.backend_url)) ||
        !copy_optional_string(
            json, tokens, token_count, "display_name",
            candidate.display_name, sizeof(candidate.display_name)) ||
        !copy_optional_string(
            json, tokens, token_count, "ap_password",
            candidate.ap_password, sizeof(candidate.ap_password))) {
        (void)networks_present;
        return BACKEND_PORTAL_UPDATE_INVALID_JSON;
    }

    bool auto_update_present = false;
    bool auto_update_value = candidate.auto_update_enabled;
    bool confirmation_present = false;
    bool confirmation_value = false;
    bool has_location_present = false;
    bool has_location_value = candidate.has_location;
    if (!get_optional_bool(
            json, tokens, token_count, "auto_update_enabled",
            &auto_update_value, &auto_update_present) ||
        !get_optional_bool(
            json, tokens, token_count, "confirm_auto_update",
            &confirmation_value, &confirmation_present) ||
        !get_optional_bool(
            json, tokens, token_count, "has_location",
            &has_location_value, &has_location_present)) {
        return BACKEND_PORTAL_UPDATE_INVALID_JSON;
    }
    if (!current->auto_update_enabled && auto_update_present &&
        auto_update_value &&
        (!confirmation_present || !confirmation_value)) {
        return BACKEND_PORTAL_UPDATE_CONFIRMATION_REQUIRED;
    }
    if (auto_update_present) {
        candidate.auto_update_enabled = auto_update_value;
    }
    if (has_location_present) {
        candidate.has_location = has_location_value;
    }

    bool latitude_present = false;
    bool longitude_present = false;
    bool altitude_present = false;
    double latitude = candidate.latitude;
    double longitude = candidate.longitude;
    double altitude = candidate.altitude_m;
    if (!get_optional_double(
            json, tokens, token_count, "latitude",
            &latitude, &latitude_present) ||
        !get_optional_double(
            json, tokens, token_count, "longitude",
            &longitude, &longitude_present) ||
        !get_optional_double(
            json, tokens, token_count, "altitude_m",
            &altitude, &altitude_present)) {
        return BACKEND_PORTAL_UPDATE_INVALID_JSON;
    }
    if (latitude_present) {
        candidate.latitude = latitude;
    }
    if (longitude_present) {
        candidate.longitude = longitude;
    }
    if (altitude_present) {
        candidate.altitude_m = (float)altitude;
    }
    if (!candidate.has_location) {
        candidate.latitude = 0.0;
        candidate.longitude = 0.0;
        candidate.altitude_m = 0.0f;
    }

    if (
#if !defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        candidate.network_count == 0 ||
#endif
        backend_config_validate(&candidate) != BACKEND_CONFIG_VALID) {
        return BACKEND_PORTAL_UPDATE_INVALID_CONFIG;
    }
    *out_candidate = candidate;
    return BACKEND_PORTAL_UPDATE_OK;
}
