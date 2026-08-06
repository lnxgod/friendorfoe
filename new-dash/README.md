# New Dash

New Dash is the compact, source-first browser console for one Friend or Foe
full-size badge or Backend Badge Lite connected to a Mac through the uplink
board's USB-C port. It shows live detections, makes Remote ID evidence
prominent, maps positioned Remote ID, keeps bounded local history, and exposes
badge diagnostics plus a small capability-gated control surface.

New Dash uses the same factory-firmware command signaling as the working
Android USB path and `scripts/fof_badge_debug_bridge.py`. Existing factory
badges do not need to be reflashed or reconfigured.

New Dash is a separate application from `../backend/`. It does not import that
backend, use its database or configuration, start its processes, or implement
its multi-node ingest features. Windows and Linux are not supported or tested
in this first release.

## Requirements

Required:

- A Mac running macOS.
- Python 3.11 or newer, available as `python3`.
- A compatible Friend or Foe full-size factory badge or Backend Badge Lite and
  a data-capable USB-C cable connected to the badge uplink board.
- Network access on the first run if the Python packages are not already
  cached.

New Dash handles these automatically:

- Creates or reuses `new-dash/.venv`.
- Installs New Dash and its only runtime dependency, `pyserial>=3.5,<4`.
- Opens a loopback-only browser dashboard and keeps retrying if the badge is
  temporarily disconnected.

You do **not** need to reflash a compatible factory badge or install Android,
the legacy FastAPI backend, Docker, PostgreSQL, Redis, Node.js, or a separate
USB driver for the badge's native USB serial connection.

## Badge compatibility

- **Full-size factory badge:** New Dash uses the established `FOF_PING`,
  `FOF_STATUS`, and `FOF_DET` USB contract, retains full detection and Remote ID
  behavior, and exposes only the allowlisted display controls described below.
- **Backend Badge Lite:** New Dash requires the exact trusted headless identity
  (`badge_lite`, `uplink-s3-backend`, `fof_backend_uplink`,
  `seeed_xiao_esp32s3`) plus `display_none`, `usb_live`, and `usb_live_ack`
  capabilities. It starts an acknowledged-live session, ACKs the current
  session heartbeat, and sends a best-effort stop when the connection closes.
  Detection, scanner-health, history, map, and export behavior remain available;
  display controls are hidden because Lite has no screen.

Identity and capabilities are checked fail-closed. A partial Lite identity,
version mismatch, missing live-session capability, or non-badge serial device
is rejected instead of silently falling back to the full-size protocol.

## Quick start

1. On a Mac with Python 3.11 or newer, connect the badge **uplink** USB-C port
   with a data-capable cable. It is fine to connect it before or after launch.
2. From a clone or downloaded copy of this repository, run:

   ```sh
   cd new-dash
   ./run.sh
   ```

3. New Dash creates its local environment on the first run, opens the
   loopback-only dashboard automatically, verifies the badge, and begins
   showing live detections. If the browser does not open, use the local URL
   printed in the terminal (normally `http://127.0.0.1:8765`).

No firmware or badge configuration change is required. If port 8765 is
occupied, New Dash selects another available loopback port.

If the complete three-board assembly exposes several ESP32 USB ports, leave
New Dash running and press **Find ESP32 uplink**. The browser asks the local
bridge to try each detected Espressif board. Scanner boards fail the verified
`FOF_PING` handshake; the first real uplink to answer `FOF_PONG` is connected
without a terminal restart. The port list remains available to choose which
board is tried first. `--port` remains available for unattended service setups.

To select a badge port explicitly:

```sh
./run.sh --port /dev/cu.usbmodem101
```

The manual equivalent is:

```sh
cd new-dash
python3 -m venv .venv
.venv/bin/python -m pip install -e .
.venv/bin/python -m new_dash
```

Available launch options are:

```text
--port /dev/cu.usbmodem101
--http-port 8765
--no-browser
--data-dir PATH
--retention-days 30
--max-observations 50000
--remote-id-hold-seconds 120
--max-remote-id-entities 512
```

`--http-port` still binds only to `127.0.0.1`. An explicitly requested busy
HTTP port is reported as an error instead of silently changing ports.

## Overnight macOS service

For unattended overnight capture, register the per-user macOS service from
`new-dash/`:

```sh
./start.sh
./start.sh --http-port 18888 --port /dev/cu.usbmodem1101
./start.sh --remote-id-hold-seconds 120 --max-remote-id-entities 512
./stop.sh
```

`start.sh` creates or reuses the local virtual environment, installs New Dash,
and replaces only its own `io.friendorfoe.new-dash` LaunchAgent. The service
restarts after a crash, continues retrying when the USB badge disconnects and
reconnects, and uses `caffeinate` to keep the Mac awake while it runs. Its
history remains at `~/Library/Application Support/New Dash/new-dash.sqlite3`;
service logs remain at `~/Library/Logs/New Dash/service.log` and
`~/Library/Logs/New Dash/service-error.log` after `./stop.sh`.
The service does not rotate these log files automatically; if long-running use
makes that necessary, stop the service first, then archive or clear the logs.

The Mac must remain powered with its lid open for overnight capture. Stop any
other serial owner, such as a flasher, serial monitor, Android bridge, or
foreground New Dash process, before starting this service. `run.sh` remains
the foreground developer launcher.

## Browser views

- **Live** shows USB/freshness/scanner health, threat counts, active badge
  classifications, and prominent Remote ID details including available drone
  and operator locations. Native `FOF_DET` notifications also appear in a
  separate **Recent USB detections** section, so recovery firmware can still
  show Remote ID and Find My events when its active entity snapshot is
  temporarily unavailable; those events are not presented as map tracks.
- **Map** shows positioned Remote ID drone/operator markers, a subdued
  drone-to-operator link, and cyan dots for distinct host-observed coordinates.
  Formation persistence is adjustable from 1 to 120 minutes and defaults to
  120 minutes. Dots refresh every five seconds without resetting manual zoom,
  pan, or entity selection. **Clear & start timer** begins a client-only drawing
  session: existing dots and live markers disappear immediately and new USB
  observations redraw the formation while elapsed time is shown. The action
  never deletes saved history. Formation drawing fetches up to 8,192 distinct
  coordinates; a `+` after the visible dot count explicitly reports that more
  saved positions remain beyond the drawing budget. Separately, the host keeps
  the newest live GPS
  position for up to 512 Remote ID identities, fades each marker as its GPS age
  approaches two minutes, and removes it after 120 seconds without a newer GPS
  observation. Public OpenStreetMap tiles provide the optional basemap; markers
  and coordinates remain available when tiles are offline.
- **History** provides newest-first time, kind, class, source, identity-text,
  and positioned filters, cursor pagination, details, CSV/JSON export, and a
  typed confirmation before local clearing. Clearing history does not stop the
  live USB connection.
- **Badge** shows scanner roles and health, firmware/status, safe and recovery
  facts, reporting/counters, memory diagnostics, and the allowlisted controls.

For a full-size badge, the only USB mutations available in New Dash are:

- transient display navigation: `next`, `detail`, `page`, and `back`;
- complete version-1 theme Apply/Reset: palette `field`, `night`, `neon`, or
  `mono`; background `dark`, `dim`, or `scanline`; brightness 25–100; and six
  RGB565 accents (`drone`, `meta`, `tracker`, `flock`, `wifi_attack`, `clear`);
- complete version-1 display-policy Apply/Reset for the 13 firmware classes
  `drone`, `meta`, `tracker`, `wifi_attack`, `skimmer`, `camera`, `flock`,
  `lock`, `hid`, `beacon`, `event_badge`, `auracast`, and `scanner_status`,
  using a Boolean enabled value, lane `off`/`lower`/`top`/`both`, minimum
  proximity `present`/`near`/`close`, and integer priority 0–100.

Apply and Reset wait for firmware acceptance; the browser does not claim a
change before the corresponding status arrives. There is no firmware upload,
reboot, bootloader, recovery/safe-mode mutation, arbitrary JSON, or raw serial
control surface.

For a freshly verified Backend Badge Lite that advertises `usb_config`, New
Dash also exposes two loopback-only, same-origin, control-token-protected API
operations:

- `POST /api/config/read` with `{}` returns the exact schema-version-1 Lite
  configuration with secrets redacted. Network and AP credentials are reported
  only as `password_set`/`ap_password_set` booleans.
- `POST /api/config` atomically validates and sends a nonempty subset of
  `networks`, `backend_url`, `display_name`, `ap_password`,
  `auto_update_enabled`, `confirm_auto_update`, `has_location`, `latitude`,
  `longitude`, and `altitude_m`. Up to four unique SSIDs are accepted;
  auto-update enablement requires explicit confirmation; enabled fixed location
  requires all three finite coordinates.

The API returns the firmware's committed configuration generation and whether
the badge must reconnect. It is not available to the full-size badge, never
returns stored passwords, and does not add firmware upload, reboot, or raw
serial access. The Badge view reports status-derived configuration generation
and connectivity diagnostics; redacted full configuration is available through
`/api/config/read`, and the browser intentionally has no password-entry form.

## Local data and telemetry limits

The default database is:

```text
~/Library/Application Support/New Dash/new-dash.sqlite3
```

History is pruned to 30 days and then to at most 50,000 observations by
default. Both positive limits can be changed at launch. `--data-dir` selects a
different directory; New Dash appends `new-dash.sqlite3` within it.

Map position retention is separate from append-only history. By default the
New Dash host process keeps the newest positioned Remote ID observation per
identity for 120 seconds, bounded to 512 identities, and rebuilds that
short-lived set from SQLite after a restart. `--remote-id-hold-seconds` and
`--max-remote-id-entities` change those positive bounds for either `run.sh` or
the macOS `start.sh` service.

The Map formation-persistence control is another, client-selected view over
saved positioned track history. It requests distinct coordinates within the
selected 1–120 minute window and never changes retention, deletes rows, or
claims that every over-the-air packet was captured. **Clear & start timer**
adds an exact in-memory session cutoff; refreshing the page restores the saved
trail view.

New Dash displays the badge firmware's capped, processed status and event
summaries. Firmware classification remains authoritative. Map trails are
**host-observed** from status received over USB, with bounded age adjustment;
they are not a raw packet capture and do not claim complete observations,
exact source timestamps, heading, velocity, or precision flight telemetry.

## Troubleshooting

- **No badge:** connect the uplink board's USB-C port. New Dash stays open and
  keeps retrying; scanner-board ports are not substitutes for the uplink.
- **Multiple devices:** press **Find ESP32 uplink**. New Dash checks each listed
  board and connects only the one that answers the verified `FOF_PING`
  handshake as an uplink.
- **Port busy:** close a flasher, serial monitor, Android bridge, or another New
  Dash process that owns the device, then retry.
- **Wrong device or ROM bootloader:** an Espressif serial port is not enough;
  New Dash requires a valid `FOF_PING`/`FOF_PONG` badge response. Boot the badge
  application firmware and select its uplink port.
- **Lite identity or capability rejected:** New Dash accepts Lite only when the
  complete trusted identity and acknowledged-live capabilities are present.
  Use a compatible Lite build; do not weaken identity checks or flash a scanner
  to work around a rejected uplink.
- **Safe USB:** USB status may be healthy while sensing is intentionally
  disabled. The Badge view reports the safe-mode reason; do not interpret a
  live USB link as healthy scanners.
- **Scanner health:** disconnected, stale, recovery, crash, or policy mismatch
  facts are shown separately for each scanner. Resolve the badge-reported
  scanner condition before trusting an empty detection list.
- **Stale or reconnecting:** the last valid snapshot remains visible with its
  age while New Dash closes, rediscovers, verifies, and polls the badge again.
  Reconnect the cable if the state does not recover.
- **Offline map tiles:** an offline-basemap notice means public tiles could not
  load. Checked-in map code, coordinates, markers, and host-observed trails
  still work; reconnect the Mac to load the optional background.

## Test and contribute

Run the Python suite and static compile check from `new-dash/`:

```sh
PYTHONPATH=src python3 -m unittest discover -s tests -v
python3 -m compileall -q src tests
```

Browser behavior tests use Node only as contributor tooling; Node is not a New
Dash runtime dependency:

```sh
node --test tests/browser_behavior_test.mjs
```

For a manual fixture UI pass:

```sh
PYTHONPATH=src python3 tests/browser_fixture_server.py --port 8876
```

Then open `http://127.0.0.1:8876/` and check Live, Map, History, and Badge at
desktop and narrow widths. Physical-hardware smoke checks should be read-only:
verify PONG, repeated STATUS, scanner health, and reconnect without applying a
control.

The future Windows/Linux extension boundary is the injected port-enumeration
adapter used by `discover_badge_ports()` in `src/new_dash/serial_transport.py`.
A platform port should implement discovery metadata behind that boundary while
leaving verification, protocol, application, storage, and browser code shared.
That is a contribution direction, not a claim of current Windows/Linux support.
