#include "backend_config_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_json_writer.h"

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#endif

#ifdef UNIT_TESTING
static backend_config_portal_test_platform_hooks_t s_test_platform_hooks;
static bool s_test_platform_hooks_installed;

void backend_config_portal_set_test_platform_hooks(
    const backend_config_portal_test_platform_hooks_t *hooks)
{
    memset(&s_test_platform_hooks, 0, sizeof(s_test_platform_hooks));
    s_test_platform_hooks_installed = false;
    if (hooks) {
        s_test_platform_hooks = *hooks;
        s_test_platform_hooks_installed = true;
    }
}
#endif

static const char PORTAL_HTML[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>FriendOrFoe Backend Lite</title><style>"
    "body{font:16px system-ui;max-width:46rem;margin:2rem auto;padding:0 1rem}"
    "fieldset{margin:1rem 0;padding:1rem}label{display:block;margin:.6rem 0}"
    "input{box-sizing:border-box;width:100%;padding:.5rem}"
    "input[type=checkbox]{width:auto}button{margin:.5rem .5rem .5rem 0;padding:.6rem}"
    "#result{white-space:pre-wrap}</style></head><body>"
    "<h1>Backend Lite setup</h1><p>This portal changes sensor configuration only.</p>"
    "<form id=\"config\"><fieldset><legend>Ordered Wi-Fi networks</legend>"
    "<label>SSID 1<input name=\"ssid0\" maxlength=\"32\"></label>"
    "<label>Password 1<input name=\"password0\" type=\"password\" maxlength=\"64\"></label>"
    "<label>SSID 2<input name=\"ssid1\" maxlength=\"32\"></label>"
    "<label>Password 2<input name=\"password1\" type=\"password\" maxlength=\"64\"></label>"
    "<label>SSID 3<input name=\"ssid2\" maxlength=\"32\"></label>"
    "<label>Password 3<input name=\"password2\" type=\"password\" maxlength=\"64\"></label>"
    "<label>SSID 4<input name=\"ssid3\" maxlength=\"32\"></label>"
    "<label>Password 4<input name=\"password3\" type=\"password\" maxlength=\"64\"></label>"
    "</fieldset><fieldset><legend>Backend node</legend>"
    "<label>Backend URL<input name=\"backend_url\" required></label>"
    "<label>Display name<input name=\"display_name\" maxlength=\"64\"></label>"
    "<label>New AP password<input name=\"ap_password\" type=\"password\" minlength=\"8\" maxlength=\"63\"></label>"
    "</fieldset><fieldset><legend>Fixed location (optional)</legend>"
    "<label><input name=\"has_location\" type=\"checkbox\"> Use fixed location</label>"
    "<label>Latitude<input name=\"latitude\" type=\"number\" step=\"any\"></label>"
    "<label>Longitude<input name=\"longitude\" type=\"number\" step=\"any\"></label>"
    "<label>Altitude meters<input name=\"altitude_m\" type=\"number\" step=\"any\"></label>"
    "</fieldset><fieldset><legend>Automatic updates</legend>"
    "<p>Automatic updates are future firmware-write authorization and default off.</p>"
    "<label><input name=\"auto_update_enabled\" type=\"checkbox\"> Authorize automatic firmware writes</label>"
    "<label><input name=\"confirm_auto_update\" type=\"checkbox\"> Confirm this authorization</label>"
    "</fieldset><button type=\"submit\">Save and reconnect</button>"
    "<button type=\"button\" id=\"test\">Test backend</button></form>"
    "<pre id=\"result\"></pre><script>"
    "const f=document.getElementById('config'),r=document.getElementById('result');"
    "async function load(){const c=await(await fetch('/api/config')).json();"
    "c.networks.forEach((n,i)=>f.elements['ssid'+i].value=n.ssid);"
    "f.elements.backend_url.value=c.backend_url;f.elements.display_name.value=c.display_name;"
    "f.elements.has_location.checked=c.has_location;"
    "f.elements.latitude.value=c.latitude===null?'':c.latitude;"
    "f.elements.longitude.value=c.longitude===null?'':c.longitude;"
    "f.elements.altitude_m.value=c.altitude_m===null?'':c.altitude_m;"
    "f.elements.auto_update_enabled.checked=c.auto_update_enabled;}"
    "f.addEventListener('submit',async e=>{e.preventDefault();const networks=[];"
    "for(let i=0;i<4;i++){const ssid=f.elements['ssid'+i].value;"
    "if(ssid){const n={ssid};const p=f.elements['password'+i].value;if(p)n.password=p;networks.push(n);}}"
    "const b={networks,backend_url:f.elements.backend_url.value,"
    "display_name:f.elements.display_name.value,has_location:f.elements.has_location.checked,"
    "auto_update_enabled:f.elements.auto_update_enabled.checked,"
    "confirm_auto_update:f.elements.confirm_auto_update.checked};"
    "if(f.elements.ap_password.value)b.ap_password=f.elements.ap_password.value;"
    "if(b.has_location){b.latitude=Number(f.elements.latitude.value);"
    "b.longitude=Number(f.elements.longitude.value);b.altitude_m=Number(f.elements.altitude_m.value);}"
    "const q=await fetch('/api/config',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(b)});"
    "r.textContent=await q.text();if(q.ok)await load();});"
    "document.getElementById('test').onclick=async()=>{const q=await fetch('/api/backend/test',{method:'POST'});r.textContent=await q.text();};"
    "load().catch(e=>r.textContent=String(e));</script></body></html>";

const char *backend_config_portal_html(void)
{
    return PORTAL_HTML;
}

bool backend_config_portal_local_ipv4_allowed(const uint8_t address[4])
{
    static const uint8_t ap_address[4] = {192U, 168U, 4U, 1U};
    return address && memcmp(address, ap_address, sizeof(ap_address)) == 0;
}

#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
static bool output_contains_config_value(
    const char *output,
    const char *value,
    size_t value_capacity)
{
    const char *terminator = memchr(value, '\0', value_capacity);
    return terminator && terminator != value && strstr(output, value) != NULL;
}

bool backend_config_portal_dashboard_status(
    backend_config_portal_t *portal,
    char *output,
    size_t capacity,
    size_t *out_length)
{
    if (out_length) {
        *out_length = 0U;
    }
    if (output && capacity > 0U) {
        output[0] = '\0';
    }
    if (!portal || !portal->initialized || !output || capacity == 0U ||
        !out_length || !portal->ops.dashboard_status ||
        !portal->ops.dashboard_status(
            portal->ops.context, output, capacity, out_length) ||
        *out_length >= capacity || output[*out_length] != '\0' ||
        strlen(output) != *out_length ||
        !backend_dashboard_status_is_redacted(output, *out_length)) {
        if (output && capacity > 0U) {
            output[0] = '\0';
        }
        if (out_length) {
            *out_length = 0U;
        }
        return false;
    }
    for (uint8_t index = 0U; index < portal->config.network_count;
         ++index) {
        if (output_contains_config_value(
                output,
                portal->config.networks[index].ssid,
                sizeof(portal->config.networks[index].ssid)) ||
            output_contains_config_value(
                output,
                portal->config.networks[index].password,
                sizeof(portal->config.networks[index].password))) {
            output[0] = '\0';
            *out_length = 0U;
            return false;
        }
    }
    if (output_contains_config_value(
            output,
            portal->config.backend_url,
            sizeof(portal->config.backend_url)) ||
        output_contains_config_value(
            output,
            portal->config.ap_password,
            sizeof(portal->config.ap_password))) {
        output[0] = '\0';
        *out_length = 0U;
        return false;
    }
    return true;
}

bool backend_config_portal_copy_dashboard_events(
    backend_config_portal_t *portal,
    backend_dashboard_query_t query,
    backend_dashboard_event_t events[BACKEND_DASHBOARD_MAX_LIMIT],
    backend_event_ring_snapshot_t *snapshot)
{
    if (!portal || !portal->initialized || !events || !snapshot ||
        !portal->ops.event_snapshot || query.limit == 0U ||
        query.limit > BACKEND_DASHBOARD_MAX_LIMIT) {
        return false;
    }
    memset(
        events,
        0,
        BACKEND_DASHBOARD_MAX_LIMIT * sizeof(events[0]));
    memset(snapshot, 0, sizeof(*snapshot));
    if (!portal->ops.event_snapshot(
            portal->ops.context,
            query.after,
            query.limit,
            events,
            BACKEND_DASHBOARD_MAX_LIMIT,
            snapshot) ||
        snapshot->count > query.limit ||
        snapshot->count > BACKEND_DASHBOARD_MAX_LIMIT) {
        memset(snapshot, 0, sizeof(*snapshot));
        return false;
    }
    return true;
}
#endif

const char *backend_config_portal_update_response(
    backend_portal_update_result_t result, int *out_status_code)
{
    int status_code = 400;
    const char *response =
        "{\"status\":\"invalid_config\",\"saved\":false}";
    switch (result) {
    case BACKEND_PORTAL_UPDATE_OK:
        status_code = 200;
        response = "{\"status\":\"ok\",\"saved\":true}";
        break;
    case BACKEND_PORTAL_UPDATE_CONFIRMATION_REQUIRED:
        response =
            "{\"status\":\"confirmation_required\",\"saved\":false}";
        break;
    case BACKEND_PORTAL_UPDATE_COMMIT_FAILED:
        status_code = 503;
        response = "{\"status\":\"commit_failed\",\"saved\":false}";
        break;
    case BACKEND_PORTAL_UPDATE_RECONNECT_FAILED:
        status_code = 503;
        response =
            "{\"status\":\"reconnect_failed\",\"saved\":true}";
        break;
    default:
        break;
    }
    if (out_status_code) {
        *out_status_code = status_code;
    }
    return response;
}

bool backend_config_portal_init(
    backend_config_portal_t *portal,
    const backend_config_record_t *current,
    const backend_config_portal_ops_t *ops)
{
    if (!portal || !current || !ops || !ops->commit_config ||
        !ops->reconnect_wifi) {
        return false;
    }
    memset(portal, 0, sizeof(*portal));
    portal->config = *current;
    portal->ops = *ops;
    portal->initialized = true;
    return true;
}

backend_portal_update_result_t backend_config_portal_apply_update(
    backend_config_portal_t *portal,
    const char *json,
    size_t length,
    int64_t now_ms)
{
    if (!portal || !portal->initialized) {
        return BACKEND_PORTAL_UPDATE_INVALID_ARGUMENT;
    }
    backend_config_record_t candidate;
    const backend_portal_update_result_t parsed =
        backend_portal_parse_config_update(
            &portal->config, json, length, &candidate);
    if (parsed != BACKEND_PORTAL_UPDATE_OK) {
        return parsed;
    }
    if (!portal->ops.commit_config(
            portal->ops.context, &candidate)) {
        return BACKEND_PORTAL_UPDATE_COMMIT_FAILED;
    }

    portal->config = candidate;
    if (!portal->ops.reconnect_wifi(
            portal->ops.context, &portal->config, now_ms)) {
        return BACKEND_PORTAL_UPDATE_RECONNECT_FAILED;
    }
    return BACKEND_PORTAL_UPDATE_OK;
}

bool backend_config_portal_test_backend(
    backend_config_portal_t *portal,
    backend_portal_backend_test_result_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!portal || !portal->initialized || !portal->ops.backend_get) {
        return false;
    }
    int status_code = 0;
    const bool complete = portal->ops.backend_get(
        portal->ops.context,
        portal->config.backend_url,
        "/health",
        BACKEND_CONFIG_PORTAL_BACKEND_TEST_TIMEOUT_MS,
        &status_code);
    out->transport_complete = complete;
    out->status_code = status_code;
    out->healthy = complete && status_code >= 200 && status_code < 300;
    return true;
}

bool backend_config_portal_handle_usb_line(
    backend_config_portal_t *portal,
    const char *line,
    size_t length)
{
    static const char command[] = "FOF_AP_START";
    if (!portal || !portal->initialized || !line) {
        return false;
    }
    while (length > 0 &&
           (line[length - 1U] == '\r' || line[length - 1U] == '\n')) {
        length--;
    }
    if (length != sizeof(command) - 1U ||
        memcmp(line, command, sizeof(command) - 1U) != 0) {
        return false;
    }
    portal->usb_start_requested = true;
    return true;
}

bool backend_config_portal_take_usb_start_request(
    backend_config_portal_t *portal)
{
    if (!portal || !portal->initialized ||
        !portal->usb_start_requested) {
        return false;
    }
    portal->usb_start_requested = false;
    return true;
}

static bool copy_bounded(
    char *output,
    size_t capacity,
    const char *value,
    size_t value_capacity)
{
    if (!output || capacity == 0 || !value || value_capacity == 0) {
        return false;
    }
    const char *terminator = memchr(value, '\0', value_capacity);
    if (!terminator) {
        return false;
    }
    const size_t length = (size_t)(terminator - value);
    if (length >= capacity) {
        return false;
    }
    memcpy(output, value, length + 1U);
    return true;
}

bool backend_config_portal_build_ap_config(
    const backend_config_portal_t *portal,
    const uint8_t sta_mac[6],
    backend_config_portal_ap_config_t *out)
{
    if (!portal || !portal->initialized || !sta_mac || !out) {
        return false;
    }
    backend_config_portal_ap_config_t config;
    memset(&config, 0, sizeof(config));
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    static const char AP_SSID_FORMAT[] =
        "FriendOrFoe-Lite-%02X%02X%02X";
#else
    static const char AP_SSID_FORMAT[] =
        "FriendOrFoe-Backend-%02X%02X%02X";
#endif
    const int written = snprintf(
        config.ssid,
        sizeof(config.ssid),
        AP_SSID_FORMAT,
        sta_mac[3],
        sta_mac[4],
        sta_mac[5]);
    if (written < 0 || (size_t)written >= sizeof(config.ssid)) {
        return false;
    }
    const char *password = portal->config.ap_password[0] != '\0'
        ? portal->config.ap_password
        : BACKEND_CONFIG_PORTAL_DEFAULT_PASSWORD;
    const size_t password_capacity = portal->config.ap_password[0] != '\0'
        ? sizeof(portal->config.ap_password)
        : sizeof(BACKEND_CONFIG_PORTAL_DEFAULT_PASSWORD);
    if (!copy_bounded(
            config.password,
            sizeof(config.password),
            password,
            password_capacity) ||
        !copy_bounded(
            config.ipv4,
            sizeof(config.ipv4),
            BACKEND_CONFIG_PORTAL_IPV4,
            sizeof(BACKEND_CONFIG_PORTAL_IPV4))) {
        return false;
    }
    config.channel = BACKEND_CONFIG_PORTAL_CHANNEL;
    config.max_clients = BACKEND_CONFIG_PORTAL_MAX_CLIENTS;
    *out = config;
    return true;
}

#ifdef ESP_PLATFORM
static esp_err_t send_json(httpd_req_t *request, const char *json)
{
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_service_unavailable(
    httpd_req_t *request, const char *message)
{
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_send(request, message, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_update_result(
    httpd_req_t *request, backend_portal_update_result_t result)
{
    int status_code = 0;
    const char *response = backend_config_portal_update_response(
        result, &status_code);
    if (status_code == 503) {
        httpd_resp_set_status(request, "503 Service Unavailable");
    } else if (status_code == 400) {
        httpd_resp_set_status(request, "400 Bad Request");
    }
    return send_json(request, response);
}

static esp_err_t read_request_body(
    httpd_req_t *request, char *output, size_t capacity, size_t *out_length)
{
    if (request->content_len <= 0 ||
        (size_t)request->content_len >= capacity ||
        (size_t)request->content_len > BACKEND_PORTAL_CONFIG_BODY_MAX) {
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < (size_t)request->content_len) {
        const int chunk = httpd_req_recv(
            request,
            output + received,
            (size_t)request->content_len - received);
        if (chunk <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)chunk;
    }
    output[received] = '\0';
    *out_length = received;
    return ESP_OK;
}

static backend_portal_method_t portal_method(httpd_method_t method)
{
    return method == HTTP_POST ? BACKEND_PORTAL_POST : BACKEND_PORTAL_GET;
}

static bool request_uses_ap_local_destination(httpd_req_t *request)
{
    if (!request) {
        return false;
    }
    const int socket_fd = httpd_req_to_sockfd(request);
    struct sockaddr_storage local_address;
    socklen_t address_length = sizeof(local_address);
    memset(&local_address, 0, sizeof(local_address));
    if (socket_fd < 0 ||
        getsockname(
            socket_fd,
            (struct sockaddr *)&local_address,
            &address_length) != 0 ||
        local_address.ss_family != AF_INET ||
        address_length < sizeof(struct sockaddr_in)) {
        return false;
    }
    const struct sockaddr_in *ipv4 =
        (const struct sockaddr_in *)&local_address;
    const uint32_t host_address = ntohl(ipv4->sin_addr.s_addr);
    const uint8_t octets[4] = {
        (uint8_t)(host_address >> 24U),
        (uint8_t)(host_address >> 16U),
        (uint8_t)(host_address >> 8U),
        (uint8_t)host_address,
    };
    return backend_config_portal_local_ipv4_allowed(octets);
}

#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
static esp_err_t send_dashboard_events(
    httpd_req_t *request,
    backend_config_portal_t *portal)
{
    char query_text[128];
    const size_t query_length = httpd_req_get_url_query_len(request);
    const char *query = NULL;
    if (query_length > 0U) {
        if (query_length >= sizeof(query_text) ||
            httpd_req_get_url_query_str(
                request, query_text, sizeof(query_text)) != ESP_OK) {
            return httpd_resp_send_err(
                request, HTTPD_400_BAD_REQUEST,
                "invalid cursor or limit");
        }
        query = query_text;
    }

    backend_dashboard_query_t parsed = {
        .after = 0U,
        .limit = BACKEND_DASHBOARD_DEFAULT_LIMIT,
    };
    if (!backend_dashboard_query_parse(query, &parsed) ||
        parsed.limit > BACKEND_DASHBOARD_MAX_LIMIT) {
        return httpd_resp_send_err(
            request, HTTPD_400_BAD_REQUEST,
            "invalid cursor or limit");
    }

    backend_dashboard_event_t *events = calloc(
        BACKEND_DASHBOARD_MAX_LIMIT, sizeof(events[0]));
    char *event_json = malloc(4096U);
    if (!events || !event_json) {
        free(events);
        free(event_json);
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR,
            "event buffer unavailable");
    }
    backend_event_ring_snapshot_t snapshot;
    if (!backend_config_portal_copy_dashboard_events(
            portal, parsed, events, &snapshot)) {
        free(events);
        free(event_json);
        return send_service_unavailable(
            request, "event snapshot unavailable");
    }

    char prefix[192];
    if (backend_dashboard_snapshot_encode_prefix(
            &snapshot, prefix, sizeof(prefix)) == 0U) {
        free(events);
        free(event_json);
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR,
            "event metadata unavailable");
    }
    httpd_resp_set_type(request, "application/json");
    esp_err_t result = httpd_resp_send_chunk(
        request, prefix, HTTPD_RESP_USE_STRLEN);
    for (size_t index = 0U;
         result == ESP_OK && index < snapshot.count; ++index) {
        if (index > 0U) {
            result = httpd_resp_send_chunk(request, ",", 1U);
        }
        if (result == ESP_OK &&
            backend_dashboard_event_encode_json(
                &events[index], event_json, 4096U) == 0U) {
            result = ESP_FAIL;
        }
        if (result == ESP_OK) {
            result = httpd_resp_send_chunk(
                request, event_json, HTTPD_RESP_USE_STRLEN);
        }
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(
            request,
            backend_dashboard_snapshot_suffix(),
            HTTPD_RESP_USE_STRLEN);
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, NULL, 0U);
    }
    free(events);
    free(event_json);
    return result;
}
#endif

static esp_err_t portal_http_handler(httpd_req_t *request)
{
    if (!request_uses_ap_local_destination(request)) {
        return httpd_resp_send_err(
            request, HTTPD_403_FORBIDDEN, "AP interface only");
    }
    backend_config_portal_t *portal = request->user_ctx;
    backend_portal_route_id_t route = BACKEND_PORTAL_ROOT;
    if (!portal || !backend_portal_route_lookup(
            portal_method(request->method), request->uri, &route)) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "not found");
    }
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    if ((route == BACKEND_PORTAL_DASHBOARD ||
         route == BACKEND_PORTAL_DASHBOARD_STATUS ||
         route == BACKEND_PORTAL_EVENTS) &&
        !portal->dashboard_routes_enabled) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "not found");
    }
    if (route == BACKEND_PORTAL_DASHBOARD) {
        httpd_resp_set_type(request, "text/html; charset=utf-8");
        return httpd_resp_send(
            request,
            backend_dashboard_page_html(),
            HTTPD_RESP_USE_STRLEN);
    }
    if (route == BACKEND_PORTAL_DASHBOARD_STATUS) {
        char status[2048];
        size_t status_length = 0U;
        if (!backend_config_portal_dashboard_status(
                portal,
                status,
                sizeof(status),
                &status_length)) {
            return send_service_unavailable(
                request, "dashboard status unavailable");
        }
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_send(request, status, status_length);
    }
    if (route == BACKEND_PORTAL_EVENTS) {
        return send_dashboard_events(request, portal);
    }
#endif
    if (route == BACKEND_PORTAL_ROOT) {
        httpd_resp_set_type(request, "text/html; charset=utf-8");
        return httpd_resp_send(
            request, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    }
    if (route == BACKEND_PORTAL_STATUS) {
        char status[160];
        backend_json_writer_t writer;
        backend_json_writer_init(&writer, status, sizeof(status));
        if (!backend_json_append_format(
                &writer,
                "{\"status\":\"ok\",\"ap_running\":%s,"
                "\"config_generation\":%lu}",
                portal->running ? "true" : "false",
                (unsigned long)portal->config.generation) ||
            backend_json_writer_finish(&writer) == 0) {
            return httpd_resp_send_err(
                request, HTTPD_500_INTERNAL_SERVER_ERROR, "status error");
        }
        return send_json(request, status);
    }
    if (route == BACKEND_PORTAL_CONFIG_GET) {
        char config_json[BACKEND_PORTAL_CONFIG_BODY_MAX + 1U];
        if (backend_portal_render_redacted_config(
                &portal->config,
                config_json,
                sizeof(config_json)) == 0) {
            return httpd_resp_send_err(
                request, HTTPD_500_INTERNAL_SERVER_ERROR, "config error");
        }
        return send_json(request, config_json);
    }
    if (route == BACKEND_PORTAL_CONFIG_POST) {
        char body[BACKEND_PORTAL_CONFIG_BODY_MAX + 1U];
        size_t length = 0;
        if (read_request_body(
                request, body, sizeof(body), &length) != ESP_OK) {
            return httpd_resp_send_err(
                request, HTTPD_400_BAD_REQUEST, "invalid body");
        }
        const backend_portal_update_result_t result =
            backend_config_portal_apply_update(
                portal, body, length, esp_timer_get_time() / 1000);
        return send_update_result(request, result);
    }

    backend_portal_backend_test_result_t result;
    if (!backend_config_portal_test_backend(portal, &result)) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return send_json(request, "{\"status\":\"test_unavailable\"}");
    }
    char response[128];
    const int written = snprintf(
        response,
        sizeof(response),
        "{\"transport_complete\":%s,\"status_code\":%d,"
        "\"healthy\":%s}",
        result.transport_complete ? "true" : "false",
        result.status_code,
        result.healthy ? "true" : "false");
    if (written < 0 || (size_t)written >= sizeof(response)) {
        return ESP_FAIL;
    }
    return send_json(request, response);
}

static httpd_method_t http_method(backend_portal_method_t method)
{
    return method == BACKEND_PORTAL_POST ? HTTP_POST : HTTP_GET;
}

static bool configure_static_ap_address(backend_config_portal_t *portal)
{
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (!ap_netif) {
        return false;
    }
    const esp_err_t stop_result = esp_netif_dhcps_stop(ap_netif);
    if (stop_result != ESP_OK &&
        stop_result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return false;
    }
    esp_netif_ip_info_t address;
    memset(&address, 0, sizeof(address));
    IP4_ADDR(&address.ip, 192, 168, 4, 1);
    IP4_ADDR(&address.gw, 192, 168, 4, 1);
    IP4_ADDR(&address.netmask, 255, 255, 255, 0);
    if (esp_netif_set_ip_info(ap_netif, &address) != ESP_OK ||
        esp_netif_dhcps_start(ap_netif) != ESP_OK) {
        return false;
    }
    portal->ap_netif = ap_netif;
    return true;
}

static bool ensure_wifi_started(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        return true;
    }
    const esp_err_t netif_result = esp_netif_init();
    if (netif_result != ESP_OK && netif_result != ESP_ERR_INVALID_STATE) {
        return false;
    }
    const esp_err_t loop_result = esp_event_loop_create_default();
    if (loop_result != ESP_OK && loop_result != ESP_ERR_INVALID_STATE) {
        return false;
    }
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    return esp_wifi_init(&init_config) == ESP_OK &&
           esp_wifi_set_storage(WIFI_STORAGE_RAM) == ESP_OK;
}
#endif

static bool platform_set_ap_config(
    backend_config_portal_t *portal,
    const backend_config_portal_ap_config_t *ap_config)
{
#ifdef UNIT_TESTING
    if (s_test_platform_hooks_installed) {
        return s_test_platform_hooks.set_ap_config &&
               s_test_platform_hooks.set_ap_config(
                   s_test_platform_hooks.context, ap_config);
    }
#endif
#ifdef ESP_PLATFORM
    if (!ensure_wifi_started() || !configure_static_ap_address(portal)) {
        return false;
    }
    wifi_config_t wifi_ap;
    memset(&wifi_ap, 0, sizeof(wifi_ap));
    memcpy(wifi_ap.ap.ssid, ap_config->ssid, strlen(ap_config->ssid));
    wifi_ap.ap.ssid_len = strlen(ap_config->ssid);
    memcpy(
        wifi_ap.ap.password,
        ap_config->password,
        strlen(ap_config->password));
    wifi_ap.ap.channel = ap_config->channel;
    wifi_ap.ap.max_connection = ap_config->max_clients;
    wifi_ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_ap.ap.pmf_cfg.capable = true;
    wifi_ap.ap.pmf_cfg.required = false;
    return esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK &&
           esp_wifi_set_config(WIFI_IF_AP, &wifi_ap) == ESP_OK;
#else
    (void)portal;
    (void)ap_config;
    return true;
#endif
}

static bool platform_start_ap(void)
{
#ifdef UNIT_TESTING
    if (s_test_platform_hooks_installed) {
        return s_test_platform_hooks.start_ap &&
               s_test_platform_hooks.start_ap(
                   s_test_platform_hooks.context);
    }
#endif
#ifdef ESP_PLATFORM
    return esp_wifi_start() == ESP_OK;
#else
    return true;
#endif
}

static bool platform_start_http(backend_config_portal_t *portal)
{
#ifdef UNIT_TESTING
    if (s_test_platform_hooks_installed) {
        return s_test_platform_hooks.start_http &&
               s_test_platform_hooks.start_http(
                   s_test_platform_hooks.context);
    }
#endif
#ifdef ESP_PLATFORM
    if (portal->server) {
        return false;
    }
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.stack_size = 16384U;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &server_config) != ESP_OK) {
        return false;
    }
    portal->server = server;
    return true;
#else
    (void)portal;
    return true;
#endif
}

static bool platform_register_route(
    backend_config_portal_t *portal,
    const backend_portal_route_t *route)
{
#ifdef UNIT_TESTING
    if (s_test_platform_hooks_installed) {
        return s_test_platform_hooks.register_route &&
               s_test_platform_hooks.register_route(
                   s_test_platform_hooks.context, route);
    }
#endif
#ifdef ESP_PLATFORM
    if (!portal->server || !route) {
        return false;
    }
    const httpd_uri_t uri = {
        .uri = route->path,
        .method = http_method(route->method),
        .handler = portal_http_handler,
        .user_ctx = portal,
    };
    return httpd_register_uri_handler(
        (httpd_handle_t)portal->server, &uri) == ESP_OK;
#else
    (void)portal;
    (void)route;
    return true;
#endif
}

#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
static bool platform_unregister_route(
    backend_config_portal_t *portal,
    const backend_portal_route_t *route)
{
#ifdef UNIT_TESTING
    if (s_test_platform_hooks_installed) {
        return s_test_platform_hooks.unregister_route &&
               s_test_platform_hooks.unregister_route(
                   s_test_platform_hooks.context, route);
    }
#endif
#ifdef ESP_PLATFORM
    if (!portal->server || !route) {
        return false;
    }
    return httpd_unregister_uri_handler(
        (httpd_handle_t)portal->server,
        route->path,
        http_method(route->method)) == ESP_OK;
#else
    (void)portal;
    (void)route;
    return true;
#endif
}
#endif

static bool platform_rollback(backend_config_portal_t *portal)
{
    bool success = true;
#ifdef UNIT_TESTING
    if (s_test_platform_hooks_installed) {
        success = s_test_platform_hooks.rollback &&
                  s_test_platform_hooks.rollback(
                      s_test_platform_hooks.context);
        portal->server = NULL;
        portal->running = false;
        portal->dashboard_routes_enabled = false;
        return success;
    }
#endif
#ifdef ESP_PLATFORM
    if (portal->server) {
        if (httpd_stop((httpd_handle_t)portal->server) == ESP_OK) {
            portal->server = NULL;
        } else {
            success = false;
        }
    }
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        success = false;
    }
#else
    portal->server = NULL;
#endif
    portal->running = false;
    portal->dashboard_routes_enabled = false;
    return success;
}

bool backend_config_portal_start(
    backend_config_portal_t *portal,
    const uint8_t sta_mac[6])
{
    if (!portal || !portal->initialized || !sta_mac) {
        return false;
    }
    if (portal->running) {
        return true;
    }
    backend_config_portal_ap_config_t ap_config;
    if (!backend_config_portal_build_ap_config(
            portal, sta_mac, &ap_config)) {
        return false;
    }
    if (!platform_set_ap_config(portal, &ap_config) ||
        !platform_start_ap() || !platform_start_http(portal)) {
        (void)platform_rollback(portal);
        return false;
    }
    size_t route_count = 0U;
    const backend_portal_route_t *routes =
        backend_portal_required_routes(&route_count);
    for (size_t index = 0; index < route_count; ++index) {
        if (!platform_register_route(portal, &routes[index])) {
            (void)platform_rollback(portal);
            return false;
        }
    }
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    routes = backend_portal_dashboard_routes(&route_count);
    size_t registered = 0U;
    for (; registered < route_count; ++registered) {
        if (!platform_register_route(portal, &routes[registered])) {
            while (registered > 0U) {
                registered--;
                (void)platform_unregister_route(
                    portal, &routes[registered]);
            }
            portal->dashboard_routes_enabled = false;
            portal->dashboard_failure_reason =
                "route_registration_failed";
            portal->running = true;
            return true;
        }
    }
    portal->dashboard_routes_enabled = true;
    portal->dashboard_failure_reason = NULL;
#endif
    portal->running = true;
    return true;
}

bool backend_config_portal_stop(backend_config_portal_t *portal)
{
    if (!portal || !portal->initialized) {
        return false;
    }
    if (!portal->running && !portal->server) {
        return true;
    }
    return platform_rollback(portal);
}

bool backend_config_portal_is_running(
    const backend_config_portal_t *portal)
{
    return portal && portal->initialized && portal->running;
}
