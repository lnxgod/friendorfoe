#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_config_portal.h"
#include "backend_dashboard_page.h"
#include "backend_json_reader.h"
#include "backend_portal_contract.h"
#include "backend_wifi_manager.h"
#include "../support/backend_test_main.h"

static backend_config_record_t config_fixture(bool auto_update_enabled)
{
    backend_config_record_t config;
    memset(&config, 0, sizeof(config));
    config.schema_version = BACKEND_CONFIG_SCHEMA_VERSION;
    config.generation = 17;
    config.network_count = 2;
    strcpy(config.networks[0].ssid, "Field \"One\"");
    strcpy(config.networks[0].password, "wifi-secret-one");
    strcpy(config.networks[1].ssid, "Field Two");
    config.networks[1].password[0] = '\0';
    strcpy(config.backend_url, "http://10.0.0.2:8000/base");
    strcpy(config.device_id, "uplink_CB77A4");
    strcpy(config.display_name, "Lite Front Yard");
    strcpy(config.ap_password, "portal-secret");
    config.auto_update_enabled = auto_update_enabled;
    config.has_location = true;
    config.latitude = 37.7749;
    config.longitude = -122.4194;
    config.altitude_m = 16.5f;
    return config;
}

static bool object_value(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    size_t object_index,
    const char *key,
    size_t *out_index)
{
    return backend_json_object_find(
        json, tokens, token_count, object_index, key, out_index);
}

void setUp(void)
{
    backend_config_portal_set_test_platform_hooks(NULL);
}

void tearDown(void)
{
    backend_config_portal_set_test_platform_hooks(NULL);
}

void test_required_route_registry_contains_exactly_the_five_config_routes(void)
{
    static const struct {
        backend_portal_method_t method;
        const char *path;
        backend_portal_route_id_t id;
    } expected[] = {
        {BACKEND_PORTAL_GET, "/", BACKEND_PORTAL_ROOT},
        {BACKEND_PORTAL_GET, "/api/status", BACKEND_PORTAL_STATUS},
        {BACKEND_PORTAL_GET, "/api/config", BACKEND_PORTAL_CONFIG_GET},
        {BACKEND_PORTAL_POST, "/api/config", BACKEND_PORTAL_CONFIG_POST},
        {BACKEND_PORTAL_POST, "/api/backend/test",
         BACKEND_PORTAL_BACKEND_TEST},
    };

    size_t route_count = 0;
    const backend_portal_route_t *routes =
        backend_portal_required_routes(&route_count);
    TEST_ASSERT_NOT_NULL(routes);
    TEST_ASSERT_EQUAL_UINT32(
        sizeof(expected) / sizeof(expected[0]), route_count);
    for (size_t index = 0; index < route_count; ++index) {
        TEST_ASSERT_EQUAL(expected[index].method, routes[index].method);
        TEST_ASSERT_EQUAL_STRING(expected[index].path, routes[index].path);
        TEST_ASSERT_EQUAL(expected[index].id, routes[index].id);

        backend_portal_route_id_t actual_id = BACKEND_PORTAL_ROOT;
        TEST_ASSERT_TRUE(backend_portal_route_lookup(
            expected[index].method, expected[index].path, &actual_id));
        TEST_ASSERT_EQUAL(expected[index].id, actual_id);
    }

    backend_portal_route_id_t sentinel = BACKEND_PORTAL_STATUS;
    TEST_ASSERT_FALSE(backend_portal_route_lookup(
        BACKEND_PORTAL_POST, "/api/status", &sentinel));
    TEST_ASSERT_FALSE(backend_portal_route_lookup(
        BACKEND_PORTAL_GET, "/api/ota", &sentinel));
    TEST_ASSERT_FALSE(backend_portal_route_lookup(
        BACKEND_PORTAL_GET, "/firmware", &sentinel));
    TEST_ASSERT_FALSE(backend_portal_route_lookup(
        BACKEND_PORTAL_GET, NULL, &sentinel));
    TEST_ASSERT_EQUAL(BACKEND_PORTAL_STATUS, sentinel);
}

void test_dashboard_route_registry_is_lite_only(void)
{
    size_t route_count = 99U;
    const backend_portal_route_t *routes =
        backend_portal_dashboard_routes(&route_count);
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    static const struct {
        const char *path;
        backend_portal_route_id_t id;
    } expected[] = {
        {"/dashboard", BACKEND_PORTAL_DASHBOARD},
        {"/api/dashboard/status", BACKEND_PORTAL_DASHBOARD_STATUS},
        {"/api/events", BACKEND_PORTAL_EVENTS},
    };
    TEST_ASSERT_NOT_NULL(routes);
    TEST_ASSERT_EQUAL_UINT32(3U, route_count);
    for (size_t index = 0; index < route_count; ++index) {
        TEST_ASSERT_EQUAL(BACKEND_PORTAL_GET, routes[index].method);
        TEST_ASSERT_EQUAL_STRING(expected[index].path, routes[index].path);
        TEST_ASSERT_EQUAL(expected[index].id, routes[index].id);
    }
#else
    TEST_ASSERT_NULL(routes);
    TEST_ASSERT_EQUAL_UINT32(0U, route_count);
#endif
}

void test_dispatch_route_uses_only_the_bounded_uri_path(void)
{
    backend_portal_route_id_t route = BACKEND_PORTAL_ROOT;
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    TEST_ASSERT_TRUE(backend_config_portal_route_from_uri(
        BACKEND_PORTAL_GET,
        "/api/events?after=40&limit=25",
        &route));
    TEST_ASSERT_EQUAL(BACKEND_PORTAL_EVENTS, route);
#else
    TEST_ASSERT_FALSE(backend_config_portal_route_from_uri(
        BACKEND_PORTAL_GET,
        "/api/events?after=40&limit=25",
        &route));
#endif

    TEST_ASSERT_TRUE(backend_config_portal_route_from_uri(
        BACKEND_PORTAL_GET, "/", &route));
    TEST_ASSERT_EQUAL(BACKEND_PORTAL_ROOT, route);
    TEST_ASSERT_TRUE(backend_config_portal_route_from_uri(
        BACKEND_PORTAL_GET, "/api/status", &route));
    TEST_ASSERT_EQUAL(BACKEND_PORTAL_STATUS, route);

    route = BACKEND_PORTAL_STATUS;
    TEST_ASSERT_FALSE(backend_config_portal_route_from_uri(
        BACKEND_PORTAL_GET, NULL, &route));
    TEST_ASSERT_FALSE(backend_config_portal_route_from_uri(
        BACKEND_PORTAL_GET, "", &route));
    TEST_ASSERT_FALSE(backend_config_portal_route_from_uri(
        BACKEND_PORTAL_GET, "api/events?after=40", &route));
    TEST_ASSERT_FALSE(backend_config_portal_route_from_uri(
        BACKEND_PORTAL_GET,
        "/api/events-too-long-to-match-because-this-path-keeps-growing-"
        "past-the-bounded-dispatch-buffer-without-a-query-delimiter",
        &route));
    TEST_ASSERT_FALSE(backend_config_portal_route_from_uri(
        BACKEND_PORTAL_GET, "/api/events?after=40", NULL));
    TEST_ASSERT_EQUAL(BACKEND_PORTAL_STATUS, route);
}

void test_redacted_config_is_parseable_and_contains_no_secret_values(void)
{
    const backend_config_record_t config = config_fixture(false);
    char json[1024];
    const size_t length = backend_portal_render_redacted_config(
        &config, json, sizeof(json));
    TEST_ASSERT_GREATER_THAN_UINT32(0, length);
    TEST_ASSERT_EQUAL_UINT32(length, strlen(json));
    TEST_ASSERT_NULL(strstr(json, "wifi-secret-one"));
    TEST_ASSERT_NULL(strstr(json, "portal-secret"));

    backend_json_token_t tokens[96];
    size_t token_count = 0;
    TEST_ASSERT_EQUAL(
        BACKEND_JSON_OK,
        backend_json_parse(
            json, length, tokens, sizeof(tokens) / sizeof(tokens[0]),
            &token_count));
    TEST_ASSERT_EQUAL(BACKEND_JSON_OBJECT, tokens[0].kind);

    size_t networks_index = 0;
    TEST_ASSERT_TRUE(object_value(
        json, tokens, token_count, 0, "networks", &networks_index));
    TEST_ASSERT_EQUAL(BACKEND_JSON_ARRAY, tokens[networks_index].kind);
    TEST_ASSERT_EQUAL_UINT16(2, tokens[networks_index].child_count);

    size_t first_network = networks_index + 1U;
    size_t ssid_index = 0;
    size_t password_set_index = 0;
    char ssid[33];
    bool password_set = false;
    TEST_ASSERT_TRUE(object_value(
        json, tokens, token_count, first_network, "ssid", &ssid_index));
    TEST_ASSERT_TRUE(backend_json_copy_string(
        json, &tokens[ssid_index], ssid, sizeof(ssid)));
    TEST_ASSERT_EQUAL_STRING("Field \"One\"", ssid);
    TEST_ASSERT_TRUE(object_value(
        json, tokens, token_count, first_network,
        "password_set", &password_set_index));
    TEST_ASSERT_TRUE(backend_json_get_bool(
        json, &tokens[password_set_index], &password_set));
    TEST_ASSERT_TRUE(password_set);

    size_t second_network = 0;
    unsigned network_seen = 0;
    for (size_t index = networks_index + 1U; index < token_count; ++index) {
        if (tokens[index].parent == (int16_t)networks_index) {
            if (network_seen == 1U) {
                second_network = index;
                break;
            }
            network_seen++;
        }
    }
    TEST_ASSERT_NOT_EQUAL(0, second_network);
    TEST_ASSERT_TRUE(object_value(
        json, tokens, token_count, second_network,
        "password_set", &password_set_index));
    TEST_ASSERT_TRUE(backend_json_get_bool(
        json, &tokens[password_set_index], &password_set));
    TEST_ASSERT_FALSE(password_set);

    size_t ap_password_set_index = 0;
    TEST_ASSERT_TRUE(object_value(
        json, tokens, token_count, 0,
        "ap_password_set", &ap_password_set_index));
    TEST_ASSERT_TRUE(backend_json_get_bool(
        json, &tokens[ap_password_set_index], &password_set));
    TEST_ASSERT_TRUE(password_set);

    size_t auto_update_index = 0;
    bool auto_update = true;
    TEST_ASSERT_TRUE(object_value(
        json, tokens, token_count, 0,
        "auto_update_enabled", &auto_update_index));
    TEST_ASSERT_TRUE(backend_json_get_bool(
        json, &tokens[auto_update_index], &auto_update));
    TEST_ASSERT_FALSE(auto_update);
}

static void fill_exact_string(char *output, size_t length, char value)
{
    memset(output, value, length);
    output[length] = '\0';
}

void test_redacted_config_fits_contract_capacity_at_maximum_escaping(void)
{
    backend_config_record_t config = config_fixture(false);
    config.network_count = BACKEND_CONFIG_MAX_NETWORKS;
    for (uint8_t index = 0; index < config.network_count; ++index) {
        fill_exact_string(config.networks[index].ssid, 32U, '\x01');
        fill_exact_string(config.networks[index].password, 64U, 'p');
    }
    memcpy(config.backend_url, "http://", 7U);
    memset(config.backend_url + 7U, '"', 184U);
    config.backend_url[191] = '\0';
    fill_exact_string(config.device_id, 32U, '\x01');
    fill_exact_string(config.display_name, 64U, '\x01');
    fill_exact_string(config.ap_password, 63U, 'p');
    TEST_ASSERT_EQUAL(
        BACKEND_CONFIG_VALID, backend_config_validate(&config));

    char json[BACKEND_PORTAL_CONFIG_BODY_MAX + 1U];
    const size_t length = backend_portal_render_redacted_config(
        &config, json, sizeof(json));
    TEST_ASSERT_GREATER_THAN_UINT32(1536U, length);
    TEST_ASSERT_LESS_THAN_UINT32(sizeof(json), length);
    backend_json_token_t tokens[96];
    size_t token_count = 0;
    TEST_ASSERT_EQUAL(
        BACKEND_JSON_OK,
        backend_json_parse(
            json, length, tokens, sizeof(tokens) / sizeof(tokens[0]),
            &token_count));
    TEST_ASSERT_NULL(strstr(json, config.networks[0].password));
    TEST_ASSERT_NULL(strstr(json, config.ap_password));
}

void test_render_redaction_fails_closed_without_partial_json(void)
{
    const backend_config_record_t config = config_fixture(false);
    char output[32];
    memset(output, 'X', sizeof(output));
    TEST_ASSERT_EQUAL_UINT32(
        0,
        backend_portal_render_redacted_config(
            &config, output, sizeof(output)));
    TEST_ASSERT_EQUAL_CHAR('\0', output[0]);
    TEST_ASSERT_EQUAL_UINT32(
        0,
        backend_portal_render_redacted_config(NULL, output, sizeof(output)));
    TEST_ASSERT_EQUAL_UINT32(
        0,
        backend_portal_render_redacted_config(&config, NULL, 0));
}

void test_config_update_preserves_omitted_fields_and_validates_candidate(void)
{
    const backend_config_record_t current = config_fixture(false);
    static const char update[] =
        "{\"display_name\":\"Lite Back Lot\","
        "\"networks\":["
        "{\"ssid\":\"Primary\",\"password\":\"new-secret\"},"
        "{\"ssid\":\"Fallback\",\"password\":\"\"}],"
        "\"has_location\":false}";
    backend_config_record_t candidate;
    memset(&candidate, 0xA5, sizeof(candidate));

    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_portal_parse_config_update(
            &current, update, sizeof(update) - 1U, &candidate));
    TEST_ASSERT_EQUAL_UINT32(current.generation + 1U, candidate.generation);
    TEST_ASSERT_EQUAL_STRING(current.backend_url, candidate.backend_url);
    TEST_ASSERT_EQUAL_STRING(current.device_id, candidate.device_id);
    TEST_ASSERT_EQUAL_STRING("Lite Back Lot", candidate.display_name);
    TEST_ASSERT_EQUAL_UINT8(2, candidate.network_count);
    TEST_ASSERT_EQUAL_STRING("Primary", candidate.networks[0].ssid);
    TEST_ASSERT_EQUAL_STRING("new-secret", candidate.networks[0].password);
    TEST_ASSERT_FALSE(candidate.auto_update_enabled);
    TEST_ASSERT_FALSE(candidate.has_location);
    TEST_ASSERT_DOUBLE_WITHIN(0.0, 0.0, candidate.latitude);
    TEST_ASSERT_DOUBLE_WITHIN(0.0, 0.0, candidate.longitude);
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, candidate.altitude_m);
}

void test_omitted_passwords_follow_matching_ssids_through_reorder_and_delete(void)
{
    backend_config_record_t current = config_fixture(false);
    strcpy(current.networks[1].password, "wifi-secret-two");
    static const char reordered[] =
        "{\"networks\":[{\"ssid\":\"Field Two\"},"
        "{\"ssid\":\"Field \\\"One\\\"\"}]}";
    backend_config_record_t candidate;
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_portal_parse_config_update(
            &current, reordered, sizeof(reordered) - 1U, &candidate));
    TEST_ASSERT_EQUAL_STRING("Field Two", candidate.networks[0].ssid);
    TEST_ASSERT_EQUAL_STRING(
        "wifi-secret-two", candidate.networks[0].password);
    TEST_ASSERT_EQUAL_STRING(
        "Field \"One\"", candidate.networks[1].ssid);
    TEST_ASSERT_EQUAL_STRING(
        "wifi-secret-one", candidate.networks[1].password);

    static const char deleted[] =
        "{\"networks\":[{\"ssid\":\"Field Two\"}]}";
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_portal_parse_config_update(
            &current, deleted, sizeof(deleted) - 1U, &candidate));
    TEST_ASSERT_EQUAL_UINT8(1, candidate.network_count);
    TEST_ASSERT_EQUAL_STRING("Field Two", candidate.networks[0].ssid);
    TEST_ASSERT_EQUAL_STRING(
        "wifi-secret-two", candidate.networks[0].password);
}

void test_auto_update_enable_requires_explicit_same_request_confirmation(void)
{
    const backend_config_record_t current = config_fixture(false);
    static const char enable_without_confirmation[] =
        "{\"auto_update_enabled\":true}";
    static const char enable_with_false_confirmation[] =
        "{\"auto_update_enabled\":true,"
        "\"confirm_auto_update\":false}";
    static const char confirmation_without_enable[] =
        "{\"confirm_auto_update\":true}";
    static const char enabled[] =
        "{\"auto_update_enabled\":true,"
        "\"confirm_auto_update\":true}";
    backend_config_record_t candidate;

    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_CONFIRMATION_REQUIRED,
        backend_portal_parse_config_update(
            &current, enable_without_confirmation,
            sizeof(enable_without_confirmation) - 1U, &candidate));
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_CONFIRMATION_REQUIRED,
        backend_portal_parse_config_update(
            &current, enable_with_false_confirmation,
            sizeof(enable_with_false_confirmation) - 1U, &candidate));
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_portal_parse_config_update(
            &current, confirmation_without_enable,
            sizeof(confirmation_without_enable) - 1U, &candidate));
    TEST_ASSERT_FALSE(candidate.auto_update_enabled);
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_portal_parse_config_update(
            &current, enabled, sizeof(enabled) - 1U, &candidate));
    TEST_ASSERT_TRUE(candidate.auto_update_enabled);

    const backend_config_record_t current_enabled = config_fixture(true);
    static const char disable[] = "{\"auto_update_enabled\":false}";
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_portal_parse_config_update(
            &current_enabled, disable, sizeof(disable) - 1U, &candidate));
    TEST_ASSERT_FALSE(candidate.auto_update_enabled);
}

void test_config_update_rejects_unknown_catalog_and_malformed_fields(void)
{
    const backend_config_record_t current = config_fixture(false);
    backend_config_record_t candidate;
    memset(&candidate, 0x5A, sizeof(candidate));
    const backend_config_record_t sentinel = candidate;

    static const char catalog[] =
        "{\"catalog_reachable\":true,\"auto_update_enabled\":true,"
        "\"confirm_auto_update\":true}";
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_UNKNOWN_FIELD,
        backend_portal_parse_config_update(
            &current, catalog, sizeof(catalog) - 1U, &candidate));
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &candidate, sizeof(candidate));

    static const char wrong_bool[] =
        "{\"auto_update_enabled\":1}";
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_INVALID_JSON,
        backend_portal_parse_config_update(
            &current, wrong_bool, sizeof(wrong_bool) - 1U, &candidate));

    static const char no_networks[] = "{\"networks\":[]}";
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_portal_parse_config_update(
            &current, no_networks, sizeof(no_networks) - 1U, &candidate));
    TEST_ASSERT_EQUAL_UINT8(0U, candidate.network_count);
    candidate = sentinel;
#else
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_INVALID_CONFIG,
        backend_portal_parse_config_update(
            &current, no_networks, sizeof(no_networks) - 1U, &candidate));
#endif

    static const char duplicate[] =
        "{\"display_name\":\"first\",\"display_name\":\"second\"}";
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_INVALID_JSON,
        backend_portal_parse_config_update(
            &current, duplicate, sizeof(duplicate) - 1U, &candidate));
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &candidate, sizeof(candidate));
}

typedef struct {
    bool commit_result;
    bool reconnect_result;
    bool health_transport_result;
    int health_status;
    unsigned commit_calls;
    unsigned reconnect_calls;
    unsigned health_calls;
    uint32_t committed_generation;
    backend_wifi_manager_t *wifi_manager;
    char health_url[192];
    char health_path[32];
    uint32_t health_timeout_ms;
    const char *dashboard_status;
    unsigned dashboard_status_calls;
    unsigned event_snapshot_calls;
    unsigned transaction_begin_calls;
    unsigned transaction_end_calls;
    unsigned transaction_depth;
    uint64_t event_after;
    size_t event_limit;
    size_t event_capacity;
} portal_fixture_t;

static bool fake_transaction_begin(void *context)
{
    portal_fixture_t *fixture = context;
    ++fixture->transaction_begin_calls;
    ++fixture->transaction_depth;
    return true;
}

static void fake_transaction_end(void *context)
{
    portal_fixture_t *fixture = context;
    ++fixture->transaction_end_calls;
    TEST_ASSERT_EQUAL_UINT32(1U, fixture->transaction_depth);
    --fixture->transaction_depth;
}

static bool fake_commit(
    void *context, const backend_config_record_t *candidate)
{
    portal_fixture_t *fixture = context;
    fixture->commit_calls++;
    fixture->committed_generation = candidate->generation;
    return fixture->commit_result;
}

static bool fake_reconnect(
    void *context, const backend_config_record_t *committed, int64_t now_ms)
{
    (void)now_ms;
    portal_fixture_t *fixture = context;
    fixture->reconnect_calls++;
    TEST_ASSERT_EQUAL_UINT32(
        fixture->committed_generation, committed->generation);
    const bool applied = !fixture->wifi_manager ||
        backend_wifi_manager_apply_committed_config(
            fixture->wifi_manager, committed, now_ms);
    return fixture->reconnect_result && applied;
}

static bool fake_health_get(
    void *context,
    const char *base_url,
    const char *path,
    uint32_t timeout_ms,
    int *status_code)
{
    portal_fixture_t *fixture = context;
    fixture->health_calls++;
    strcpy(fixture->health_url, base_url);
    strcpy(fixture->health_path, path);
    fixture->health_timeout_ms = timeout_ms;
    *status_code = fixture->health_status;
    return fixture->health_transport_result;
}

static bool fake_dashboard_status(
    void *context,
    char *output,
    size_t capacity,
    size_t *out_length)
{
    portal_fixture_t *fixture = context;
    fixture->dashboard_status_calls++;
    if (!fixture->dashboard_status) {
        return false;
    }
    const size_t length = strlen(fixture->dashboard_status);
    if (length >= capacity) {
        return false;
    }
    memcpy(output, fixture->dashboard_status, length + 1U);
    *out_length = length;
    return true;
}

static bool fake_event_snapshot(
    void *context,
    uint64_t after,
    size_t limit,
    backend_dashboard_event_t *events,
    size_t event_capacity,
    backend_event_ring_snapshot_t *snapshot)
{
    portal_fixture_t *fixture = context;
    fixture->event_snapshot_calls++;
    fixture->event_after = after;
    fixture->event_limit = limit;
    fixture->event_capacity = event_capacity;
    TEST_ASSERT_NOT_NULL(events);
    TEST_ASSERT_NOT_NULL(snapshot);
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->count = 1U;
    snapshot->oldest_sequence = 40U;
    snapshot->newest_sequence = 44U;
    snapshot->cursor_reset = true;
    events[0].sequence = 40U;
    return true;
}

static backend_config_portal_ops_t fixture_ops(portal_fixture_t *fixture)
{
    const backend_config_portal_ops_t ops = {
        .context = fixture,
        .commit_config = fake_commit,
        .reconnect_wifi = fake_reconnect,
        .backend_get = fake_health_get,
        .dashboard_status = fake_dashboard_status,
        .event_snapshot = fake_event_snapshot,
        .begin_config_transaction = fake_transaction_begin,
        .end_config_transaction = fake_transaction_end,
    };
    return ops;
}

void test_config_update_is_bracketed_by_one_transaction(void)
{
    const backend_config_record_t current = config_fixture(false);
    portal_fixture_t fixture = {
        .commit_result = true,
        .reconnect_result = true,
    };
    const backend_config_portal_ops_t ops = fixture_ops(&fixture);
    backend_config_portal_t portal;
    TEST_ASSERT_TRUE(backend_config_portal_init(&portal, &current, &ops));
    static const char update[] = "{\"display_name\":\"Serialized\"}";

    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_config_portal_apply_update(
            &portal, update, sizeof(update) - 1U, 1234));
    TEST_ASSERT_EQUAL_UINT32(1U, fixture.transaction_begin_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, fixture.transaction_end_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, fixture.transaction_depth);
    TEST_ASSERT_EQUAL_UINT32(1U, fixture.commit_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, fixture.reconnect_calls);

    fixture.transaction_begin_calls = 0U;
    fixture.transaction_end_calls = 0U;
    backend_config_record_t snapshot;
    memset(&snapshot, 0xA5, sizeof(snapshot));
    TEST_ASSERT_TRUE(backend_config_portal_snapshot_config(
        &portal, &snapshot));
    TEST_ASSERT_EQUAL_UINT32(1U, fixture.transaction_begin_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, fixture.transaction_end_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, fixture.transaction_depth);
    TEST_ASSERT_EQUAL_MEMORY(&portal.config, &snapshot, sizeof(snapshot));
}

#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
void test_lite_can_atomically_commit_zero_network_recovery_config(void)
{
    const backend_config_record_t current = config_fixture(false);
    portal_fixture_t fixture = {
        .commit_result = true,
        .reconnect_result = true,
    };
    const backend_config_portal_ops_t ops = fixture_ops(&fixture);
    backend_config_portal_t portal;
    TEST_ASSERT_TRUE(backend_config_portal_init(&portal, &current, &ops));
    static const char update[] = "{\"networks\":[]}";

    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_config_portal_apply_update(
            &portal, update, sizeof(update) - 1U, 1500));
    TEST_ASSERT_EQUAL_UINT32(1U, fixture.commit_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, fixture.reconnect_calls);
    TEST_ASSERT_EQUAL_UINT8(0U, portal.config.network_count);
    TEST_ASSERT_EQUAL_UINT32(current.generation + 1U,
                             portal.config.generation);
}
#endif

#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
void test_dashboard_query_defaults_and_requires_full_unsigned_values(void)
{
    backend_dashboard_query_t query = {.after = 999U, .limit = 999U};
    TEST_ASSERT_TRUE(backend_dashboard_query_parse(NULL, &query));
    TEST_ASSERT_EQUAL_UINT64(0U, query.after);
    TEST_ASSERT_EQUAL_UINT32(25U, query.limit);

    TEST_ASSERT_TRUE(backend_dashboard_query_parse(
        "after=18446744073709551615&limit=50", &query));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, query.after);
    TEST_ASSERT_EQUAL_UINT32(50U, query.limit);
    TEST_ASSERT_TRUE(backend_dashboard_query_parse("limit=25", &query));
    TEST_ASSERT_EQUAL_UINT64(0U, query.after);
    TEST_ASSERT_EQUAL_UINT32(25U, query.limit);

    static const char *const invalid[] = {
        "limit=51",
        "limit=0",
        "limit=25x",
        "after=",
        "after=-1",
        "after=+1",
        "after=18446744073709551616",
        "after=1x",
        "after=1&after=2",
        "unknown=1",
    };
    for (size_t index = 0U;
         index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        TEST_ASSERT_FALSE(
            backend_dashboard_query_parse(invalid[index], &query));
    }
    TEST_ASSERT_FALSE(backend_dashboard_query_parse("after=1", NULL));
}

void test_stale_cursor_metadata_is_serialized_before_events(void)
{
    const backend_event_ring_snapshot_t snapshot = {
        .count = 2U,
        .oldest_sequence = 40U,
        .newest_sequence = 44U,
        .cursor_reset = true,
    };
    char output[192];
    const size_t length = backend_dashboard_snapshot_encode_prefix(
        &snapshot, output, sizeof(output));
    TEST_ASSERT_EQUAL_UINT32(strlen(output), length);
    TEST_ASSERT_EQUAL_STRING(
        "{\"count\":2,\"oldest_sequence\":40,\"newest_sequence\":44,"
        "\"cursor_reset\":true,\"events\":[",
        output);
    TEST_ASSERT_EQUAL_STRING("]}", backend_dashboard_snapshot_suffix());
}

void test_dashboard_status_is_redacted_and_event_copy_is_bounded(void)
{
    const backend_config_record_t current = config_fixture(false);
    portal_fixture_t fixture = {
        .dashboard_status =
            "{\"wifi_connected\":true,\"dropped_contention\":2}",
    };
    const backend_config_portal_ops_t ops = fixture_ops(&fixture);
    backend_config_portal_t portal;
    TEST_ASSERT_TRUE(backend_config_portal_init(&portal, &current, &ops));

    char status[160];
    size_t status_length = 0U;
    TEST_ASSERT_TRUE(backend_config_portal_dashboard_status(
        &portal, status, sizeof(status), &status_length));
    TEST_ASSERT_EQUAL_UINT32(strlen(status), status_length);
    TEST_ASSERT_NULL(strstr(status, "password"));
    TEST_ASSERT_NULL(strstr(status, "ssid"));
    TEST_ASSERT_NULL(strstr(status, "backend_url"));
    TEST_ASSERT_NULL(strstr(status, "portal-secret"));
    TEST_ASSERT_NULL(strstr(status, "wifi-secret-one"));

    fixture.dashboard_status = "{\"password\":\"portal-secret\"}";
    TEST_ASSERT_FALSE(backend_config_portal_dashboard_status(
        &portal, status, sizeof(status), &status_length));
    TEST_ASSERT_EQUAL_UINT32(0U, status_length);
    TEST_ASSERT_EQUAL_UINT32(2U, fixture.transaction_begin_calls);
    TEST_ASSERT_EQUAL_UINT32(2U, fixture.transaction_end_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, fixture.transaction_depth);

    backend_dashboard_event_t events[50];
    backend_event_ring_snapshot_t snapshot;
    const backend_dashboard_query_t query = {.after = 39U, .limit = 50U};
    TEST_ASSERT_TRUE(backend_config_portal_copy_dashboard_events(
        &portal, query, events, &snapshot));
    TEST_ASSERT_EQUAL_UINT32(1U, fixture.event_snapshot_calls);
    TEST_ASSERT_EQUAL_UINT64(39U, fixture.event_after);
    TEST_ASSERT_EQUAL_UINT32(50U, fixture.event_limit);
    TEST_ASSERT_EQUAL_UINT32(50U, fixture.event_capacity);
    TEST_ASSERT_EQUAL_UINT32(1U, snapshot.count);
    TEST_ASSERT_EQUAL_UINT64(40U, events[0].sequence);

    const backend_dashboard_query_t excessive = {
        .after = 0U,
        .limit = 51U,
    };
    TEST_ASSERT_FALSE(backend_config_portal_copy_dashboard_events(
        &portal, excessive, events, &snapshot));
    TEST_ASSERT_EQUAL_UINT32(1U, fixture.event_snapshot_calls);
}

void test_dashboard_status_capacity_accepts_exact_rich_two_scanner_payload(void)
{
    enum { RICH_TWO_SCANNER_STATUS_LENGTH = 2073U };
    static const char prefix[] =
        "{\"scanner_summaries\":["
        "{\"slot\":0,\"identity\":{\"target\":\"wifi-scanner\"}},"
        "{\"slot\":1,\"identity\":{\"target\":\"ble-scanner\"}}],"
        "\"padding\":\"";
    static const char suffix[] = "\"}";
    static char rich_status[RICH_TWO_SCANNER_STATUS_LENGTH + 1U];
    static char output[BACKEND_CONFIG_PORTAL_DASHBOARD_STATUS_CAPACITY];
    const size_t padding_length = RICH_TWO_SCANNER_STATUS_LENGTH -
        (sizeof(prefix) - 1U) - (sizeof(suffix) - 1U);
    memcpy(rich_status, prefix, sizeof(prefix) - 1U);
    memset(
        rich_status + sizeof(prefix) - 1U,
        'x',
        padding_length);
    memcpy(
        rich_status + sizeof(prefix) - 1U + padding_length,
        suffix,
        sizeof(suffix));
    TEST_ASSERT_EQUAL_UINT32(
        RICH_TWO_SCANNER_STATUS_LENGTH, strlen(rich_status));
    TEST_ASSERT_GREATER_THAN_UINT32(2047U, strlen(rich_status));
    TEST_ASSERT_EQUAL_UINT32(
        8192U, BACKEND_CONFIG_PORTAL_DASHBOARD_STATUS_CAPACITY);

    const backend_config_record_t current = config_fixture(false);
    portal_fixture_t fixture = {.dashboard_status = rich_status};
    const backend_config_portal_ops_t ops = fixture_ops(&fixture);
    backend_config_portal_t portal;
    TEST_ASSERT_TRUE(backend_config_portal_init(&portal, &current, &ops));

    size_t output_length = 0U;
    TEST_ASSERT_TRUE(backend_config_portal_dashboard_status(
        &portal, output, sizeof(output), &output_length));
    TEST_ASSERT_EQUAL_UINT32(
        RICH_TWO_SCANNER_STATUS_LENGTH, output_length);
    TEST_ASSERT_EQUAL_STRING(rich_status, output);
    TEST_ASSERT_EQUAL_UINT32(1U, fixture.dashboard_status_calls);
}
#endif

void test_portal_reconnects_only_after_atomic_commit_succeeds(void)
{
    const backend_config_record_t current = config_fixture(false);
    backend_wifi_manager_t wifi_manager;
    TEST_ASSERT_TRUE(backend_wifi_manager_init(
        &wifi_manager, &current, 100));
    TEST_ASSERT_TRUE(backend_wifi_manager_handle_event(
        &wifi_manager, BACKEND_WIFI_EVENT_AUTH_FAILED, 200));
    TEST_ASSERT_EQUAL_UINT8(1, wifi_manager.policy.network_index);
    portal_fixture_t fixture = {
        .commit_result = false,
        .reconnect_result = true,
        .wifi_manager = &wifi_manager,
    };
    const backend_config_portal_ops_t ops = fixture_ops(&fixture);
    backend_config_portal_t portal;
    TEST_ASSERT_TRUE(backend_config_portal_init(&portal, &current, &ops));
    static const char update[] = "{\"display_name\":\"Committed\"}";

    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_COMMIT_FAILED,
        backend_config_portal_apply_update(
            &portal, update, sizeof(update) - 1U, 1000));
    TEST_ASSERT_EQUAL_UINT32(1, fixture.commit_calls);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.reconnect_calls);
    TEST_ASSERT_EQUAL_UINT32(17, portal.config.generation);
    TEST_ASSERT_EQUAL_STRING("Lite Front Yard", portal.config.display_name);
    TEST_ASSERT_EQUAL_UINT32(17, wifi_manager.policy.config_generation);
    TEST_ASSERT_EQUAL_UINT8(1, wifi_manager.policy.network_index);

    fixture.commit_result = true;
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_config_portal_apply_update(
            &portal, update, sizeof(update) - 1U, 2000));
    TEST_ASSERT_EQUAL_UINT32(2, fixture.commit_calls);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.reconnect_calls);
    TEST_ASSERT_EQUAL_UINT32(18, portal.config.generation);
    TEST_ASSERT_EQUAL_STRING("Committed", portal.config.display_name);
    TEST_ASSERT_EQUAL_UINT32(18, wifi_manager.policy.config_generation);
    TEST_ASSERT_EQUAL_UINT8(0, wifi_manager.policy.network_index);
}

void test_reconnect_failure_reports_config_saved_distinct_from_commit_failure(void)
{
    const backend_config_record_t current = config_fixture(false);
    portal_fixture_t fixture = {
        .commit_result = true,
        .reconnect_result = false,
    };
    const backend_config_portal_ops_t ops = fixture_ops(&fixture);
    backend_config_portal_t portal;
    TEST_ASSERT_TRUE(backend_config_portal_init(&portal, &current, &ops));
    static const char update[] = "{\"display_name\":\"Persisted\"}";

    const backend_portal_update_result_t result =
        backend_config_portal_apply_update(
            &portal, update, sizeof(update) - 1U, 9000);
    TEST_ASSERT_EQUAL(BACKEND_PORTAL_UPDATE_RECONNECT_FAILED, result);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.commit_calls);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.reconnect_calls);
    TEST_ASSERT_EQUAL_UINT32(18, portal.config.generation);
    TEST_ASSERT_EQUAL_STRING("Persisted", portal.config.display_name);

    int status_code = 0;
    TEST_ASSERT_EQUAL_STRING(
        "{\"status\":\"reconnect_failed\",\"saved\":true}",
        backend_config_portal_update_response(result, &status_code));
    TEST_ASSERT_EQUAL_INT(503, status_code);
    TEST_ASSERT_EQUAL_STRING(
        "{\"status\":\"commit_failed\",\"saved\":false}",
        backend_config_portal_update_response(
            BACKEND_PORTAL_UPDATE_COMMIT_FAILED, &status_code));
    TEST_ASSERT_EQUAL_INT(503, status_code);
}

void test_backend_health_test_is_bounded_and_cannot_change_update_authority(void)
{
    const backend_config_record_t current = config_fixture(false);
    portal_fixture_t fixture = {
        .commit_result = true,
        .reconnect_result = true,
        .health_transport_result = true,
        .health_status = 200,
    };
    const backend_config_portal_ops_t ops = fixture_ops(&fixture);
    backend_config_portal_t portal;
    TEST_ASSERT_TRUE(backend_config_portal_init(&portal, &current, &ops));

    backend_portal_backend_test_result_t result;
    TEST_ASSERT_TRUE(backend_config_portal_test_backend(&portal, &result));
    TEST_ASSERT_TRUE(result.transport_complete);
    TEST_ASSERT_EQUAL_INT(200, result.status_code);
    TEST_ASSERT_TRUE(result.healthy);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.health_calls);
    TEST_ASSERT_EQUAL_STRING(current.backend_url, fixture.health_url);
    TEST_ASSERT_EQUAL_STRING("/health", fixture.health_path);
    TEST_ASSERT_EQUAL_UINT32(
        BACKEND_CONFIG_PORTAL_BACKEND_TEST_TIMEOUT_MS,
        fixture.health_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(1U, fixture.transaction_begin_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, fixture.transaction_end_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, fixture.transaction_depth);
    TEST_ASSERT_FALSE(portal.config.auto_update_enabled);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.commit_calls);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.reconnect_calls);
}

void test_usb_command_and_ap_identity_are_exact(void)
{
    const backend_config_record_t current = config_fixture(false);
    portal_fixture_t fixture = {0};
    const backend_config_portal_ops_t ops = fixture_ops(&fixture);
    backend_config_portal_t portal;
    TEST_ASSERT_TRUE(backend_config_portal_init(&portal, &current, &ops));

    static const char near_match[] = "FOF_AP_START_NOW";
    static const char command[] = "FOF_AP_START\r\n";
    TEST_ASSERT_FALSE(backend_config_portal_handle_usb_line(
        &portal, near_match, sizeof(near_match) - 1U));
    TEST_ASSERT_FALSE(backend_config_portal_take_usb_start_request(&portal));
    TEST_ASSERT_TRUE(backend_config_portal_handle_usb_line(
        &portal, command, sizeof(command) - 1U));
    TEST_ASSERT_TRUE(backend_config_portal_take_usb_start_request(&portal));
    TEST_ASSERT_FALSE(backend_config_portal_take_usb_start_request(&portal));

    const uint8_t mac[6] = {0x02, 0x10, 0x20, 0xCB, 0x77, 0xA4};
    backend_config_portal_ap_config_t ap_config;
    TEST_ASSERT_TRUE(backend_config_portal_build_ap_config(
        &portal, mac, &ap_config));
    TEST_ASSERT_EQUAL_UINT32(1U, fixture.transaction_begin_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, fixture.transaction_end_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, fixture.transaction_depth);
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    TEST_ASSERT_EQUAL_STRING("FriendOrFoe-Lite-CB77A4", ap_config.ssid);
#else
    TEST_ASSERT_EQUAL_STRING("FriendOrFoe-Backend-CB77A4", ap_config.ssid);
#endif
    TEST_ASSERT_EQUAL_STRING("portal-secret", ap_config.password);
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", ap_config.ipv4);
    TEST_ASSERT_EQUAL_UINT8(1, ap_config.channel);
    TEST_ASSERT_EQUAL_UINT8(4, ap_config.max_clients);

    backend_config_portal_t default_password_portal = portal;
    default_password_portal.config.ap_password[0] = '\0';
    TEST_ASSERT_TRUE(backend_config_portal_build_ap_config(
        &default_password_portal, mac, &ap_config));
    TEST_ASSERT_EQUAL_STRING("friendorfoe", ap_config.password);

    TEST_ASSERT_FALSE(backend_config_portal_is_running(&portal));
    TEST_ASSERT_TRUE(backend_config_portal_start(&portal, mac));
    TEST_ASSERT_TRUE(backend_config_portal_is_running(&portal));
    TEST_ASSERT_TRUE(backend_config_portal_start(&portal, mac));
    TEST_ASSERT_TRUE(backend_config_portal_stop(&portal));
    TEST_ASSERT_FALSE(backend_config_portal_is_running(&portal));
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    TEST_ASSERT_FALSE(portal.dashboard_routes_enabled);
#endif
    TEST_ASSERT_TRUE(backend_config_portal_stop(&portal));
}

void test_only_the_ap_local_ipv4_destination_is_allowed(void)
{
    const uint8_t ap_address[4] = {192, 168, 4, 1};
    const uint8_t sta_address[4] = {10, 0, 0, 25};
    const uint8_t loopback[4] = {127, 0, 0, 1};
    TEST_ASSERT_TRUE(
        backend_config_portal_local_ipv4_allowed(ap_address));
    TEST_ASSERT_FALSE(
        backend_config_portal_local_ipv4_allowed(sta_address));
    TEST_ASSERT_FALSE(
        backend_config_portal_local_ipv4_allowed(loopback));
    TEST_ASSERT_FALSE(backend_config_portal_local_ipv4_allowed(NULL));
}

typedef enum {
    PORTAL_FAIL_SET_AP_CONFIG = 0,
    PORTAL_FAIL_START_AP,
    PORTAL_FAIL_START_HTTP,
    PORTAL_FAIL_REGISTER_ROUTE,
} portal_fail_stage_t;

typedef struct {
    portal_fail_stage_t fail_stage;
    unsigned fail_route_call;
    unsigned set_config_calls;
    unsigned start_ap_calls;
    unsigned start_http_calls;
    unsigned register_route_calls;
    unsigned unregister_route_calls;
    backend_portal_route_id_t registered[8];
    backend_portal_route_id_t unregistered[3];
    unsigned rollback_calls;
} activation_fixture_t;

static bool activation_set_config(
    void *context, const backend_config_portal_ap_config_t *config)
{
    activation_fixture_t *fixture = context;
    fixture->set_config_calls++;
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", config->ipv4);
    return fixture->fail_stage != PORTAL_FAIL_SET_AP_CONFIG;
}

static bool activation_start_ap(void *context)
{
    activation_fixture_t *fixture = context;
    fixture->start_ap_calls++;
    return fixture->fail_stage != PORTAL_FAIL_START_AP;
}

static bool activation_start_http(void *context)
{
    activation_fixture_t *fixture = context;
    fixture->start_http_calls++;
    return fixture->fail_stage != PORTAL_FAIL_START_HTTP;
}

static bool activation_register_route(
    void *context, const backend_portal_route_t *route)
{
    activation_fixture_t *fixture = context;
    fixture->register_route_calls++;
    TEST_ASSERT_NOT_NULL(route);
    fixture->registered[fixture->register_route_calls - 1U] = route->id;
    return fixture->fail_stage != PORTAL_FAIL_REGISTER_ROUTE ||
           fixture->register_route_calls != fixture->fail_route_call;
}

static bool activation_unregister_route(
    void *context, const backend_portal_route_t *route)
{
    activation_fixture_t *fixture = context;
    TEST_ASSERT_NOT_NULL(route);
    if (fixture->unregister_route_calls < 3U) {
        fixture->unregistered[fixture->unregister_route_calls] = route->id;
    }
    fixture->unregister_route_calls++;
    return true;
}

static bool activation_rollback(void *context)
{
    activation_fixture_t *fixture = context;
    fixture->rollback_calls++;
    return true;
}

void test_every_ap_activation_failure_rolls_back_to_no_portal(void)
{
    const backend_config_record_t current = config_fixture(false);
    portal_fixture_t portal_fixture = {0};
    const backend_config_portal_ops_t ops = fixture_ops(&portal_fixture);
    const uint8_t mac[6] = {0x02, 0x10, 0x20, 0xCB, 0x77, 0xA4};

    for (int stage = PORTAL_FAIL_SET_AP_CONFIG;
         stage <= PORTAL_FAIL_REGISTER_ROUTE;
         ++stage) {
        activation_fixture_t activation = {
            .fail_stage = (portal_fail_stage_t)stage,
            .fail_route_call = 3U,
        };
        const backend_config_portal_test_platform_hooks_t hooks = {
            .context = &activation,
            .set_ap_config = activation_set_config,
            .start_ap = activation_start_ap,
            .start_http = activation_start_http,
            .register_route = activation_register_route,
            .unregister_route = activation_unregister_route,
            .rollback = activation_rollback,
        };
        backend_config_portal_set_test_platform_hooks(&hooks);
        backend_config_portal_t portal;
        TEST_ASSERT_TRUE(backend_config_portal_init(
            &portal, &current, &ops));
        TEST_ASSERT_FALSE(backend_config_portal_start(&portal, mac));
        TEST_ASSERT_FALSE(backend_config_portal_is_running(&portal));
        TEST_ASSERT_EQUAL_UINT32(1, activation.rollback_calls);
    }
}

#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
void test_each_optional_route_failure_preserves_the_required_portal(void)
{
    const backend_config_record_t current = config_fixture(false);
    portal_fixture_t portal_fixture = {0};
    const backend_config_portal_ops_t ops = fixture_ops(&portal_fixture);
    const uint8_t mac[6] = {0x02, 0x10, 0x20, 0xCB, 0x77, 0xA4};

    for (unsigned optional_index = 0U; optional_index < 3U;
         ++optional_index) {
        activation_fixture_t activation = {
            .fail_stage = PORTAL_FAIL_REGISTER_ROUTE,
            .fail_route_call = 6U + optional_index,
        };
        const backend_config_portal_test_platform_hooks_t hooks = {
            .context = &activation,
            .set_ap_config = activation_set_config,
            .start_ap = activation_start_ap,
            .start_http = activation_start_http,
            .register_route = activation_register_route,
            .unregister_route = activation_unregister_route,
            .rollback = activation_rollback,
        };
        backend_config_portal_set_test_platform_hooks(&hooks);
        backend_config_portal_t portal;
        TEST_ASSERT_TRUE(backend_config_portal_init(
            &portal, &current, &ops));
        TEST_ASSERT_TRUE(backend_config_portal_start(&portal, mac));
        TEST_ASSERT_TRUE(backend_config_portal_is_running(&portal));
        TEST_ASSERT_FALSE(portal.dashboard_routes_enabled);
        TEST_ASSERT_EQUAL_STRING(
            "route_registration_failed", portal.dashboard_failure_reason);
        TEST_ASSERT_EQUAL_UINT32(0U, activation.rollback_calls);
        TEST_ASSERT_EQUAL_UINT32(
            6U + optional_index, activation.register_route_calls);
        TEST_ASSERT_EQUAL_UINT32(
            optional_index, activation.unregister_route_calls);
        for (unsigned required = 0U; required < 5U; ++required) {
            TEST_ASSERT_EQUAL(
                BACKEND_PORTAL_ROOT + required,
                activation.registered[required]);
        }
        for (unsigned removed = 0U; removed < optional_index; ++removed) {
            TEST_ASSERT_EQUAL(
                BACKEND_PORTAL_DASHBOARD + optional_index - removed - 1U,
                activation.unregistered[removed]);
        }
    }
}
#endif

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_required_route_registry_contains_exactly_the_five_config_routes);
    BACKEND_RUN_TEST(test_dashboard_route_registry_is_lite_only);
    BACKEND_RUN_TEST(test_dispatch_route_uses_only_the_bounded_uri_path);
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    BACKEND_RUN_TEST(
        test_dashboard_query_defaults_and_requires_full_unsigned_values);
    BACKEND_RUN_TEST(
        test_stale_cursor_metadata_is_serialized_before_events);
    BACKEND_RUN_TEST(
        test_dashboard_status_is_redacted_and_event_copy_is_bounded);
    BACKEND_RUN_TEST(
        test_dashboard_status_capacity_accepts_exact_rich_two_scanner_payload);
#endif
    BACKEND_RUN_TEST(
        test_redacted_config_is_parseable_and_contains_no_secret_values);
    BACKEND_RUN_TEST(
        test_redacted_config_fits_contract_capacity_at_maximum_escaping);
    BACKEND_RUN_TEST(test_render_redaction_fails_closed_without_partial_json);
    BACKEND_RUN_TEST(
        test_config_update_preserves_omitted_fields_and_validates_candidate);
    BACKEND_RUN_TEST(
        test_omitted_passwords_follow_matching_ssids_through_reorder_and_delete);
    BACKEND_RUN_TEST(
        test_auto_update_enable_requires_explicit_same_request_confirmation);
    BACKEND_RUN_TEST(
        test_config_update_rejects_unknown_catalog_and_malformed_fields);
    BACKEND_RUN_TEST(
        test_portal_reconnects_only_after_atomic_commit_succeeds);
    BACKEND_RUN_TEST(test_config_update_is_bracketed_by_one_transaction);
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    BACKEND_RUN_TEST(
        test_lite_can_atomically_commit_zero_network_recovery_config);
#endif
    BACKEND_RUN_TEST(
        test_reconnect_failure_reports_config_saved_distinct_from_commit_failure);
    BACKEND_RUN_TEST(
        test_backend_health_test_is_bounded_and_cannot_change_update_authority);
    BACKEND_RUN_TEST(test_usb_command_and_ap_identity_are_exact);
    BACKEND_RUN_TEST(test_only_the_ap_local_ipv4_destination_is_allowed);
    BACKEND_RUN_TEST(
        test_every_ap_activation_failure_rolls_back_to_no_portal);
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    BACKEND_RUN_TEST(
        test_each_optional_route_failure_preserves_the_required_portal);
#endif
    return UNITY_END();
}
