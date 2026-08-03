#include "backend_dashboard_page.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char DASHBOARD_HTML[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>FriendOrFoe Lite recovery</title><style>"
    ":root{color-scheme:dark}body{font:15px system-ui;margin:0;background:#101418;color:#e8eef3}"
    "main{max-width:72rem;margin:auto;padding:1.25rem}h1{font-size:1.35rem}"
    "section{background:#192128;border:1px solid #30404d;border-radius:.6rem;padding:1rem;margin:1rem 0}"
    "pre{white-space:pre-wrap;overflow-wrap:anywhere}table{width:100%;border-collapse:collapse}"
    "th,td{text-align:left;padding:.45rem;border-bottom:1px solid #30404d}"
    "small{color:#9fb0bd}</style></head><body><main>"
    "<h1>Backend Lite recovery dashboard</h1>"
    "<small>Session-only live view. Reloading clears browser state.</small>"
    "<section><h2>Status</h2><pre id=\"status\">Waiting...</pre></section>"
    "<section><h2>Recent events</h2><table><thead><tr><th>Seq</th><th>Type</th>"
    "<th>ID</th><th>RSSI</th><th>Distance</th></tr></thead><tbody id=\"events\"></tbody></table></section>"
    "<script>let after=0;const rows=[];const s=document.getElementById('status'),"
    "b=document.getElementById('events');function text(v){return v==null?'':String(v)}"
    "function render(){b.textContent='';for(const e of rows){const tr=document.createElement('tr');"
    "for(const v of[e.sequence,e.badge_label||e.badge_class,e.id,e.rssi,e.distance_m]){"
    "const td=document.createElement('td');td.textContent=text(v);tr.appendChild(td)}b.appendChild(tr)}}"
    "async function poll(){try{const [sr,er]=await Promise.all([fetch('/api/dashboard/status'),"
    "fetch('/api/events?after='+after+'&limit=25')]);if(!sr.ok||!er.ok)throw Error('request failed');"
    "const st=await sr.json(),ev=await er.json();s.textContent=JSON.stringify(st,null,2);"
    "if(ev.cursor_reset)rows.length=0;for(const e of ev.events){rows.push(e);after=e.sequence}"
    "if(rows.length>50)rows.splice(0,rows.length-50);if(ev.newest_sequence>after&&ev.events.length===0)"
    "after=ev.newest_sequence;render()}catch(e){s.textContent=String(e)}}poll();setInterval(poll,1000);"
    "</script></main></body></html>";

static bool parse_unsigned_segment(
    const char *value,
    size_t length,
    uint64_t *out)
{
    if (!value || !out || length == 0U || length > 20U) {
        return false;
    }
    char buffer[21];
    for (size_t index = 0U; index < length; ++index) {
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
        buffer[index] = value[index];
    }
    buffer[length] = '\0';
    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = strtoull(buffer, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' ||
        (unsigned long long)(uint64_t)parsed != parsed) {
        return false;
    }
    *out = (uint64_t)parsed;
    return true;
}

bool backend_dashboard_query_parse(
    const char *query,
    backend_dashboard_query_t *out)
{
    if (!out) {
        return false;
    }
    backend_dashboard_query_t parsed = {
        .after = 0U,
        .limit = BACKEND_DASHBOARD_DEFAULT_LIMIT,
    };
    if (!query || query[0] == '\0') {
        *out = parsed;
        return true;
    }

    bool saw_after = false;
    bool saw_limit = false;
    const char *field = query;
    while (*field != '\0') {
        const char *end = strchr(field, '&');
        const size_t field_length = end
            ? (size_t)(end - field)
            : strlen(field);
        const char *equals = memchr(field, '=', field_length);
        if (!equals) {
            return false;
        }
        const size_t key_length = (size_t)(equals - field);
        const char *value = equals + 1;
        const size_t value_length =
            field_length - key_length - 1U;
        uint64_t number = 0U;
        if (key_length == 5U && memcmp(field, "after", 5U) == 0) {
            if (saw_after ||
                !parse_unsigned_segment(value, value_length, &number)) {
                return false;
            }
            parsed.after = number;
            saw_after = true;
        } else if (key_length == 5U &&
                   memcmp(field, "limit", 5U) == 0) {
            if (saw_limit ||
                !parse_unsigned_segment(value, value_length, &number) ||
                number == 0U || number > BACKEND_DASHBOARD_MAX_LIMIT) {
                return false;
            }
            parsed.limit = (size_t)number;
            saw_limit = true;
        } else {
            return false;
        }
        if (!end) {
            break;
        }
        field = end + 1;
        if (*field == '\0') {
            return false;
        }
    }
    *out = parsed;
    return true;
}

size_t backend_dashboard_snapshot_encode_prefix(
    const backend_event_ring_snapshot_t *snapshot,
    char *output,
    size_t capacity)
{
    if (output && capacity > 0U) {
        output[0] = '\0';
    }
    if (!snapshot || !output || capacity == 0U ||
        snapshot->count > BACKEND_DASHBOARD_MAX_LIMIT) {
        return 0U;
    }
    const int written = snprintf(
        output,
        capacity,
        "{\"count\":%zu,\"oldest_sequence\":%" PRIu64
        ",\"newest_sequence\":%" PRIu64
        ",\"cursor_reset\":%s,\"events\":[",
        snapshot->count,
        snapshot->oldest_sequence,
        snapshot->newest_sequence,
        snapshot->cursor_reset ? "true" : "false");
    if (written < 0 || (size_t)written >= capacity) {
        output[0] = '\0';
        return 0U;
    }
    return (size_t)written;
}

const char *backend_dashboard_snapshot_suffix(void)
{
    return "]}";
}

static char ascii_lower(char value)
{
    return value >= 'A' && value <= 'Z'
        ? (char)(value + ('a' - 'A'))
        : value;
}

static bool contains_case_insensitive(
    const char *text,
    size_t length,
    const char *needle)
{
    const size_t needle_length = strlen(needle);
    if (needle_length == 0U || needle_length > length) {
        return false;
    }
    for (size_t start = 0U; start + needle_length <= length; ++start) {
        size_t index = 0U;
        while (index < needle_length &&
               ascii_lower(text[start + index]) ==
                   ascii_lower(needle[index])) {
            index++;
        }
        if (index == needle_length) {
            return true;
        }
    }
    return false;
}

bool backend_dashboard_status_is_redacted(
    const char *json,
    size_t length)
{
    static const char *const forbidden[] = {
        "password",
        "passphrase",
        "ssid",
        "backend_url",
        "credential",
        "secret",
        "token",
    };
    if (!json || length < 2U || json[0] != '{' ||
        json[length - 1U] != '}' || strlen(json) != length) {
        return false;
    }
    for (size_t index = 0U;
         index < sizeof(forbidden) / sizeof(forbidden[0]); ++index) {
        if (contains_case_insensitive(json, length, forbidden[index])) {
            return false;
        }
    }
    return true;
}

const char *backend_dashboard_page_html(void)
{
    return DASHBOARD_HTML;
}
