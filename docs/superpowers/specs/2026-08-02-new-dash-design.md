# New Dash Design

**Date:** 2026-08-02
**Status:** Approved for implementation

## Goal

Build **New Dash**, a compact, source-first macOS web application for one
Friend or Foe badge connected over USB-C. New Dash brings the useful Android
badge experience to a local browser: live detections, a Remote ID map and
details, searchable history, filters, diagnostics, and safe display controls.

New Dash is a new top-level application under `new-dash/`. It is completely
independent of the existing `backend/`: it does not import its code, use its
routes or database, launch its processes, or require its configuration. The
existing backend remains unchanged.

## Product Boundary

The first release supports:

- macOS;
- Python 3.11 or newer;
- one physical FoF badge connected through its uplink ESP32-S3 USB-C port;
- source installation and launch by open-source contributors;
- local browser access on the same Mac;
- persistent but bounded local history;
- read-only diagnostics and allowlisted display/theme controls.

The first release does not support:

- multiple badges;
- Windows or Linux as tested platforms;
- remote browser access or LAN binding;
- the existing backend's network-ingest contract;
- Redis, PostgreSQL, SQLAlchemy, Docker, Node, or a frontend build step;
- ADS-B, fleet management, node registration, correlation, triangulation,
  calibration, cloud enrichment, or firmware catalogs;
- firmware upload, scanner relay, uplink update, reboot, bootloader, recovery
  mutation, arbitrary serial commands, or arbitrary JSON controls;
- a packaged or notarized macOS application in the initial source release.

Windows and Linux may be added later by implementing platform-specific port
discovery behind the transport interface. Their future needs must not complicate
or be claimed by the macOS release.

## Chosen Approach

New Dash uses Python's standard library for HTTP serving, SQLite persistence,
threads, JSON, CSV export, and browser launching. PySerial is its only Python
runtime dependency. The browser client uses plain HTML, CSS, and JavaScript.
Small map assets are checked into New Dash so no package manager or CDN is
required to render the application shell.

This is intentionally smaller than a FastAPI application and easier for the
repository's contributors to build than a new Go, Rust, Electron, or Node
application. It also lets the implementation reuse the proven behavior of
`scripts/fof_badge_debug_bridge.py` without coupling New Dash to that script.

## Repository Layout

The planned source tree is:

```text
new-dash/
  README.md
  LICENSES.md
  pyproject.toml
  run.sh
  src/new_dash/
    __init__.py
    __main__.py
    application.py
    controls.py
    models.py
    protocol.py
    serial_transport.py
    storage.py
    web.py
    static/
      index.html
      app.js
      api.js
      ui.js
      views/
        live.js
        map.js
        history.js
        badge.js
      styles.css
      vendor/
  tests/
    fixtures/
    test_application.py
    test_controls.py
    test_protocol.py
    test_serial_transport.py
    test_storage.py
    test_web.py
```

Responsibilities remain narrow:

- `protocol.py` recognizes complete machine-prefixed lines and converts their
  JSON payloads into New Dash models.
- `serial_transport.py` discovers, verifies, owns, reads, writes, and reconnects
  the single USB badge.
- `storage.py` owns the New Dash SQLite schema, queries, retention, clear, and
  export behavior.
- `controls.py` validates the finite safe-control surface and produces firmware
  command JSON. It cannot accept arbitrary command names or raw serial data.
- `application.py` coordinates current state, persistence, status freshness,
  and transport lifecycle.
- `web.py` serves loopback HTTP routes and bundled static assets.
- `models.py` holds dependency-free typed data structures shared by those
  modules.

No New Dash runtime module imports anything from `backend/`, `android/`,
`esp32/`, or `scripts/`. Tests may copy representative protocol fixtures from
the Android and firmware contracts, with their origin recorded in fixture
comments.

## Launch Experience

From `new-dash/`, `./run.sh` creates or reuses a local virtual environment,
installs New Dash and PySerial, and runs the application. Manual equivalent
commands are documented for contributors who do not want the bootstrap script.
After installation, the direct entry point is:

```sh
python -m new_dash
```

By default New Dash binds only to `127.0.0.1:8765` and opens the dashboard in
the default browser. If 8765 is occupied, it binds an available loopback port,
prints the chosen URL, and opens that URL. Options include:

```text
--port /dev/cu.usbmodem101
--http-port 8765
--no-browser
--data-dir PATH
--retention-days 30
--max-observations 50000
```

The default macOS data directory is
`~/Library/Application Support/New Dash/`. Runtime data is never written into
the source tree unless `--data-dir` explicitly requests it.

## USB Contract

### Discovery and verification

Automatic discovery enumerates serial devices through PySerial rather than
matching path globs alone. A candidate must have Espressif vendor ID `0x303A`
and native USB Serial/JTAG product ID `0x1001`. Product and manufacturer text
are diagnostic hints, not substitutes for mismatched numeric IDs. macOS
`/dev/cu.*` and `/dev/tty.*` aliases with the same stable USB identity count as
one physical candidate, with the `/dev/cu.*` path preferred.

Exactly one physical candidate is opened automatically. Zero candidates produces a
waiting state. More than one produces an actionable ambiguous-device state and
does not guess. `--port` selects an explicit path but still requires a
successful application-level `FOF_PING` / `FOF_PONG:` exchange before the
transport becomes live.

The serial port uses the badge tools' established host settings:

- nominal baud: 115200;
- leave DTR and RTS at the serial adapter defaults;
- short bounded read timeout;
- newline-terminated UTF-8 writes;
- UTF-8 reads with replacement for invalid bytes.

New Dash must not force either control line before or after opening the port.
This matches the existing Android bulk-USB command path and
`scripts/fof_badge_debug_bridge.py`. Factory uplink firmware
`0.67.2-badge-defcon34` returns `FOF_PONG`, `FOF_STATUS`, and detections with
the established default signaling, while forcing DTR/RTS low suppresses its
command path. Exclusive access is requested where PySerial and macOS support
it. Stale input is cleared and the verification write is a leading newline
followed by `FOF_PING\n`, which resynchronizes any partial firmware command
line. Only a complete, nonempty PONG received after that write verifies the
session.

The nominal baud is a host convention for the ESP32-S3 native USB
Serial/JTAG console, not the internal 921600-baud scanner UART.

### Framing

The USB console mixes ESP-IDF logs and machine-readable records. New Dash
buffers complete lines, treats CR and LF as delimiters, ignores empty lines and
ordinary logs, and recognizes only these prefixes:

- `FOF_PONG:`
- `FOF_DET:`
- `FOF_STATUS:`
- `FOF_CTL_OK:`
- `FOF_CTL_ERROR:`

Firmware-upload prefixes and raw binary modes are deliberately unsupported.
The maximum buffered line is 64 KiB. An overlong line is discarded and counted.
Malformed or interleaved JSON is discarded and counted. Neither condition may
replace the last valid status snapshot.

On a verified connection, New Dash sends `FOF_STATUS` every two seconds. It
keeps one reader and one serialized writer for the port. Safe controls share
the writer and receive a matching success, error, or timeout result without
allowing status writes to interleave at the byte level.

Firmware accepts at most 2047 command bytes before the newline. New Dash
serializes controls compactly with non-finite JSON disabled and rejects any
longer command locally. Because control replies have no request ID, only one
mutation may be outstanding. A success reply must carry the message expected
for that command. After a timeout, New Dash reconnects and re-verifies before
accepting another mutation so a late reply cannot be assigned to a later
command.

### State and reconnection

The transport exposes these operator states:

```text
discovering -> connecting -> verifying -> live -> stale -> reconnecting
```

It also exposes distinct details for no badge, multiple candidates, explicit
port missing, port busy, wrong/non-responsive device, safe-USB mode, unhealthy
scanners, malformed input, and control timeout.

The latest valid status becomes stale six seconds after receipt. A read error,
path disappearance, or verification failure closes the handle and returns to
enumeration with bounded backoff. Backoff starts at one second, doubles to a
maximum of ten seconds, and resets after a verified connection. This permits a
rebooted badge to return with a different `/dev/cu.usbmodem...` path.

Automatic and explicit-port sessions follow a changed device path only through
a matching USB serial number or USB location learned from the verified device.
With neither stable property available, New Dash does not silently attach to a
different candidate after the original path disappears.

New Dash must distinguish a healthy USB control path from healthy sensing.
Firmware safe-USB mode intentionally keeps `FOF_PING` and `FOF_STATUS` working
while scanner ingestion may be disabled.

## Protocol Models

### Detection events

`FOF_DET` supplies:

```json
{
  "id": "...",
  "manufacturer": "...",
  "badge_label": "...",
  "badge_class": "...",
  "badge_entity_key": "...",
  "source": 0,
  "confidence": 0.95,
  "threat_score": 77.5,
  "rssi": -57
}
```

Source IDs are mapped as follows:

| ID | Source |
| --- | --- |
| 0 | `ble_rid` |
| 1 | `wifi_ssid` |
| 2 | `wifi_dji_ie` |
| 3 | `wifi_rid` |
| 4 | `wifi_oui` |
| 5 | `wifi_probe` |
| 6 | `ble_fingerprint` |
| 7 | `wifi_assoc` |
| 8 | `wifi_inventory` |

Sources 0 and 3 are explicit Remote ID. DJI vendor-IE evidence remains a
separate drone source and is not mislabeled as ASTM Remote ID.

### Status snapshots

`FOF_STATUS` is the authoritative current-state and map feed. A parsed status
requires an object root, a nonempty string version, and arrays for
`entities`/`scanners` when those fields are present. `uptime_s` is optional so
the intentional factory startup/recovery envelope remains compatible with the
working Android path; when present, it must be finite, numeric, and non-Boolean.
JSON NaN and Infinity are rejected. New Dash consumes:

- top-level firmware version, mode, threat score, counts, reset/recovery facts,
  reporting state, memory facts, display policy/theme, and display state;
- scanner connection, identity, role/profile, health, command, radio, firmware,
  and recovery facts;
- active entity identity, evidence, class/category, source, confidence, score,
  RSSI, age, counts, Wi-Fi fields, drone position, altitude, operator position,
  and operator identity.

Unknown status fields are ignored safely and may be retained in a compact
extras object where useful. Missing optional fields, including `uptime_s`,
stay absent; they are not converted to misleading zero values. The factory
`0.67.2-badge-defcon34` `startup_dependency` status omits uptime, entities, and
scanners but must still update freshness and surface its recovery/USB-health
diagnostics. Coordinates are accepted only when both latitude and longitude
are finite and within geographic bounds.

The firmware status list is a ranked, capped processed snapshot rather than a
raw packet stream. New Dash labels its position history **host-observed** and
does not claim exact source timestamps, heading, velocity, complete packet
capture, or precision flight telemetry. Receipt time and bounded `last_seen_s`
are used to estimate observation time.

## Runtime Data Flow

```text
FoF badge USB
    |
    v
serial transport -> protocol parser -> in-memory current state
                         |                       |
                         v                       v
              bounded persistence queue    loopback JSON API
                         |                       |
                         v                       v
                 SQLite observations          browser UI
```

1. The serial owner discovers and verifies the badge.
2. `FOF_DET` records update live state and enqueue event observations with host
   receipt timestamps without blocking the USB reader.
3. Every valid `FOF_STATUS` atomically replaces current status.
4. Positioned Remote ID entities enqueue track observations only when their
   identity counters or coordinates change; repeated identical polls do not
   create rows.
5. The application derives freshness, connection health, source labels, and
   safe summaries without rerunning the legacy backend classifier.
6. The browser polls current state once per second. History is queried only
   when needed.

The badge firmware's classification, labels, evidence, confidence, score, and
stale policy remain authoritative. New Dash does not duplicate the large
backend enrichment stack.

The persistence queue is bounded at 1,024 actions. On overflow or database
failure, New Dash retains live state, exposes a persistence-drop/error
diagnostic, and never claims the affected record was stored. Clear and prune
are ordered queue barriers so earlier queued rows cannot reappear afterward.

## SQLite Storage

New Dash uses one SQLite database with a schema version table and an
`observations` table. An observation is either `event` or `track` and contains:

- host receipt and estimated observation timestamps;
- stable entity key and event kind;
- source ID/code, class, category, label, display ID, and manufacturer;
- confidence, score/threat score, RSSI, and badge counters;
- drone latitude, longitude, and altitude when present;
- operator latitude, longitude, and identity when present;
- a compact JSON column for bounded additional detail.

Event identity prefers `badge_entity_key`, then source plus detection ID.
Status identity prefers source plus `display_id`, then BSSID, SSID, or label.
Track deduplication compares the latest stored point for that stable identity
and persists only a coordinate or firmware counter change. Identical
`FOF_DET` lines are retained because the protocol describes each emitted line
as an event and provides no sequence number.

Retention deletes records older than 30 days and then deletes oldest records
until the total is at most 50,000. Cleanup runs at startup and periodically,
not on every row. Both limits are configurable from the CLI, and zero is not a
valid unbounded shortcut. SQLite uses WAL mode and a small busy timeout so the
serial writer and HTTP history reader remain responsive.

History supports bounded, cursor-based filtering by time, kind, source, class,
identity text, and whether coordinates exist. CSV and JSON exports stream the
filtered result. Clearing history requires a specific confirmation request and
does not reset the live connection.

## Local HTTP Interface

The HTTP server binds only to IPv4 loopback. It sends no permissive CORS
headers. Mutation requests require same-origin requests plus a random
per-process control token embedded in the served application shell. This
prevents an unrelated website from casually issuing commands to a local badge.

The small API surface is:

- `GET /api/state` — connection, freshness, current status, active entities,
  counters, and recent event summary;
- `GET /api/history` — cursor-based filtered observations;
- `GET /api/history/export.csv` and `GET /api/history/export.json` — filtered
  exports;
- `POST /api/history/clear` — confirmed local-history deletion;
- `POST /api/control/display-nav` — validated display navigation action;
- `POST /api/control/theme` — validated complete theme command;
- `POST /api/control/theme/reset` — theme reset;
- `POST /api/control/display-policy` — validated complete display policy;
- `POST /api/control/display-policy/reset` — display-policy reset.

Every response uses a small consistent JSON envelope and explicit HTTP status.
Request bodies and query limits are bounded. There is no generic proxy route,
raw serial route, arbitrary `FOF_CTL` route, or firmware route.

Success envelopes are exactly `{"ok":true,"data":...}`. Error envelopes are
exactly `{"ok":false,"error":{"code":"...","message":"..."}}`. Current
state has stable top-level objects `connection`, `freshness`, `status`,
`recent_events`, and `diagnostics`. History returns `items` plus an opaque
`next_cursor`. Clearing history requires `{"confirm":"clear-history"}` after
the browser separately requires the operator to type `CLEAR`.

The allowlist mirrors the firmware's exact version-1 contracts:

- display navigation accepts only the canonical actions `next`, `detail`,
  `page`, and `back`;
- themes accept palette `field`, `night`, `neon`, or `mono`; background `dark`,
  `dim`, or `scanline`; brightness 25 through 100; and RGB565 values 0 through
  65535 for the six accents `drone`, `meta`, `tracker`, `flock`,
  `wifi_attack`, and `clear`;
- display policy accepts exactly the 13 firmware classes `drone`, `meta`,
  `tracker`, `wifi_attack`, `skimmer`, `camera`, `flock`, `lock`, `hid`,
  `beacon`, `event_badge`, `auracast`, and `scanner_status`; lane `off`,
  `lower`, `top`, or `both`; minimum proximity `present`, `near`, or `close`;
  and priority 0 through 100.

Theme and display-policy Apply/Reset commands set `persist: true` because they
are explicit operator settings. Navigation is transient. The web API does not
accept a client-provided command name or persistence override.

## Browser Experience

The application is responsive and keyboard usable, with four primary views.

### Live

The header always shows USB state, data freshness, firmware version, scanner
health, and the selected port. The Live view shows threat counts and current
non-stale entities. Remote ID is visually prominent and includes:

- display ID and source transport;
- confidence/score and RSSI;
- last-seen age;
- drone coordinates and altitude;
- operator coordinates and operator ID;
- an honest empty state when location fields are absent.

Other badge classifications remain visible with their firmware-provided label,
evidence, class, source, proximity, and signal facts.

### Map

The map shows positioned Remote ID drone and operator markers, a line between
them, and locally retained host-observed drone trails. Checked-in map client
assets render without a CDN. Public online tiles may enhance the background,
but the view retains coordinates, markers, trails, scale, and an explicit
offline-basemap notice if tiles are unavailable. No API key is required.

### History

History provides time, kind, class, source, identity text, and location filters,
newest-first pagination, details, CSV/JSON export, and confirmed clearing. It
does not imply that the USB feed contains every RF packet.

### Badge

Badge shows scanner roles/health, firmware/status facts, reset and safe-mode
facts, reporting state, command/radio counters, and memory diagnostics. It also
provides display navigation, complete theme editing/reset, and complete display
policy editing/reset.

Dashboard filters affect only New Dash presentation and are kept in browser
storage. Badge display-policy changes are labeled separately, show the current
firmware policy, require an explicit Apply, validate all enum/range values, and
report firmware acceptance or rejection. No optimistic UI claims a mutation
succeeded before `FOF_CTL_OK` arrives.

Dashboard presentation filters apply to Live and Map only. History query
filters are separate and session-local. Map trails request at most the newest
2,000 positioned track observations for currently active Remote ID identities.
Three tile errors before any successful tile load declare the basemap offline;
any successful tile clears that state. Theme or policy Apply remains disabled
unless firmware supplied a complete editable version-1 object; New Dash never
invents missing firmware settings.

## Error Handling

Operator-facing errors are actionable and non-destructive:

- no badge: ask the user to connect the uplink USB-C port;
- several Espressif ports: list them and request `--port`;
- busy port: identify likely exclusive ownership and suggest closing a flasher,
  serial monitor, or another New Dash instance;
- wrong device or ROM bootloader: report failed `FOF_PONG` verification;
- stale status: retain the last view with its age and reconnect state;
- safe-USB or scanner failure: distinguish USB health from sensing health;
- malformed/truncated status: keep the previous valid snapshot and count the
  rejected frame;
- invalid control: reject locally without writing to USB;
- firmware control error or timeout: show the exact bounded result and leave
  local state unchanged;
- database failure: keep the live dashboard running when possible, mark history
  unavailable, and avoid pretending a record was persisted;
- map tile failure: keep markers/coordinates and show an offline-basemap state.

Logs go to stderr with human-readable levels and never include Wi-Fi passwords,
arbitrary environment contents, or raw binary data.

## Testing Strategy

Development follows red-green-refactor. Production serial code is exercised
through an injected transport interface; tests do not require physical USB.

Protocol tests cover:

- valid `FOF_PONG`, `FOF_DET`, `FOF_STATUS`, control success, and control error;
- the exact source mapping, including Remote ID versus DJI evidence;
- Android status fixtures with location and operator fields;
- the captured factory `0.67.2-badge-defcon34` startup status without uptime;
- CR, LF, CRLF, chunk boundaries, ordinary ESP-IDF logs, invalid UTF-8,
  malformed JSON, interleaving, and overlong lines;
- missing optional fields, unknown additive fields, and invalid coordinates.

Serial tests cover:

- zero, one, and several candidates;
- explicit-port selection and application-level identity verification;
- absence of forced DTR/RTS mutations and factory-compatible newline writes;
- port busy, silent/wrong device, detach, stale status, changed device path,
  bounded backoff, and clean shutdown;
- serialized status/control writes and control timeout/error routing.

Storage tests cover:

- schema creation and migration guardrails;
- event retention, track deduplication, and restart behavior;
- 30-day and 50,000-row pruning boundaries;
- filtered cursor pagination, CSV/JSON export, and confirmed clear;
- concurrent serial-writer and HTTP-reader behavior.

HTTP and application tests cover:

- loopback-only binding and occupied-port fallback;
- static assets and every API contract;
- query/body limits, same-origin/token checks, and control allowlisting;
- stale/current state derivation and degraded database behavior;
- graceful start and shutdown without a badge.

Browser verification uses fixture state to cover live Remote ID details, map
markers/trails, offline-basemap behavior, filters/history, diagnostics,
controls, empty/error states, keyboard operation, and narrow screens. It may
use repository development tooling, but New Dash gains no Node runtime or
frontend build dependency.

When a badge is available, final verification includes a read-only hardware
check: automatic or explicit-port discovery, `FOF_PING`, repeated valid
`FOF_STATUS`, scanner health display, and unplug/reconnect. It never flashes,
reboots, or mutates badge configuration as part of the default check.

## Documentation

`new-dash/README.md` includes:

- macOS and Python prerequisites;
- bootstrap and manual source setup;
- normal launch and explicit-port launch;
- where local data lives;
- browser views and safe controls;
- common USB ownership, ambiguity, safe-USB, scanner-health, and map-tile
  troubleshooting;
- history retention/export/clear behavior;
- the precise v1 platform and telemetry limitations;
- contributor test commands and the transport extension boundary for later
  Windows/Linux work.

The repository root README gains a short New Dash entry and keeps the existing
backend entry clearly separate.

## Acceptance Criteria

New Dash v1 is complete when:

- `new-dash/` runs independently and `backend/` has no diff;
- a fresh Python 3.11+ environment can install and launch it from source with
  PySerial as the only Python runtime dependency;
- it binds only to loopback, opens its browser URL, and remains useful with no
  badge attached;
- it verifies and owns exactly one badge USB connection, polls status, records
  events, and reconnects after path changes;
- it renders active badge classifications and gives explicit Remote ID evidence
  priority without relabeling weaker drone evidence;
- it maps valid drone/operator coordinates and labels trails host-observed;
- history persists across restarts, remains bounded, filters correctly,
  exports correctly, and clears only after confirmation;
- diagnostics truthfully distinguish USB, scanner, safe-mode, and freshness
  states;
- only the approved display navigation, theme, and display-policy mutations can
  reach USB;
- automated protocol, serial, storage, application, and HTTP tests pass;
- browser fixture verification passes at desktop and narrow widths;
- a read-only factory-badge smoke check receives PONG, repeated status,
  detections, browser-visible live state, and a successful disconnect/reconnect;
  repeated startup status may be proven by increasing factory USB response
  counters when `uptime_s` is intentionally absent;
- documentation enables a contributor to build, run, test, and troubleshoot
  New Dash without consulting the legacy backend.
