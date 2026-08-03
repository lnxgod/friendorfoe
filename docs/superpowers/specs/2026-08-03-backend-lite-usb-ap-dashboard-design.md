# Backend Badge Lite Fast USB and Standalone AP Dashboard Design

## Status

Approved in conversation on 2026-08-03. This design applies only to the separate Backend Badge Lite firmware family on branch `codex/backend-firmware`.

## Goal

Make the no-screen Backend Badge Lite uplink useful in three modes without changing the native badge firmware:

1. Standalone sensor with Wi-Fi/HTTP upload and threat LEDs.
2. USB-connected live sensor for New Dash, with the same established machine protocol available to Android after its separate exact-family allowlist change.
3. Session-only standalone AP with persistent device configuration and a small local dashboard backed by nonpersistent PSRAM event history.

The uplink must keep its truthful Backend Lite identity. USB and the AP dashboard are additional consumers of the same canonical detections; neither may interrupt scanning or backend upload.

## Protected Boundaries

- Do not modify `esp32/`, the native badge firmware, its factory bundle, or its release targets.
- Do not modify either Backend Lite scanner image for this feature. Scanner-to-uplink UART remains unchanged at 921600 baud.
- Implement firmware changes under `backend-firmware/` and keep the targets `uplink-s3-backend`, `scanner-s3-combo-backend`, and their projects distinct from native badge targets.
- Do not make Lite report `uplink-s3-fof_badge` or `fof_badge_uplink` over USB. Its identity remains:
  - product family: `badge_lite`
  - target: `uplink-s3-backend`
  - project: `fof_backend_uplink`
  - hardware: `seeed_xiao_esp32s3`
- Android family acceptance and New Dash capability-based UI changes are follow-on client changes on their own branches. They are not permission to modify or publish an APK in this firmware branch.
- No device is flashed as part of implementation or build verification. Hardware flashing remains a separately approved action.

## Selected Approach

Add a backend-owned, headless compatibility layer around the existing ESP32-S3 USB Serial/JTAG interface. Keep newline-delimited JSON so current tools can consume the stream, but replace synchronous `printf`/`fgets` traffic with the buffered ESP-IDF USB Serial/JTAG driver and a sole output owner.

This is preferred over two alternatives:

- A TinyUSB CDC replacement could offer more descriptor control, but it changes enumeration and complicates ROM recovery, flashing, and current New Dash/Android discovery.
- A vendor-specific binary protocol could maximize throughput, but it would require new host implementations and would no longer be plug-compatible with the native badge protocol.

The AP dashboard uses bounded cursor polling rather than WebSockets or Server-Sent Events. Polling is easier to recover, bounds every response, and needs no long-lived HTTP connection on the embedded server.

## Architecture

### Canonical detection fan-out

The current coordinator remains the sole deduplication authority. After its 500 ms merge window produces one canonical `backend_detection_observation_t`, the fan-out proceeds in this order:

1. Attempt to enqueue the existing HTTP batch exactly as today and retain that result as the coordinator sink result.
2. Independently project the observation into the compact headless event schema, even when the HTTP queue is temporarily unavailable.
3. Best-effort append the compact event to the PSRAM ring.
4. Best-effort enqueue the compact event for USB transmission.

HTTP enqueue remains the coordinator sink's success criterion. PSRAM or USB failure never changes the sink result and never causes scanner flow control. Conversely, a full or unavailable HTTP queue does not blind an attached USB host or the current-boot dashboard. This keeps local viewing useful during backend outages while preventing a disconnected or slow host from backpressuring UART scanners or backend delivery.

Projection and queue operations must not write USB or serialize HTTP while holding the uplink runtime lock. The USB transmitter is the only machine-frame writer. Normal ESP-IDF logs may share the console, but they may not interleave bytes inside a `FOF_*` frame.

### Components

The implementation will introduce focused backend-owned units:

- `backend_usb_protocol`: pure parsing and encoding for compatible headless USB commands and frames.
- `backend_usb_service`: ESP-IDF driver setup, RX framing, required/optional queues, host state, and the sole TX worker.
- `backend_dashboard_event`: projection from canonical observations into one compact event used by USB and the dashboard.
- `backend_event_ring`: fixed-capacity, PSRAM-only rolling history with sequence/cursor snapshots.
- extensions to `backend_config_portal`: standalone-session control, dashboard HTML, and bounded status/event routes.

These units do not include display code and do not compile provenance-only files from `backend-firmware/vendor/`.

## Fast USB Contract

### Physical and driver behavior

- Retain ESP32-S3 USB Serial/JTAG and the existing Espressif USB identity used by New Dash and Android.
- Host tools may continue opening the serial abstraction at nominal 115200. That value is compatibility metadata, not a UART wire-rate limit for USB Serial/JTAG.
- Install the buffered ESP-IDF USB Serial/JTAG driver with 8192-byte RX and 8192-byte TX rings.
- Read and write in bounded blocks. The TX worker writes complete machine frames in calls of at most 4096 bytes; physical USB packet splitting is handled by the driver.
- After the driver starts, route ESP-IDF logging and existing Lite status-line helpers through the USB service. No task may call the USB driver or console writer directly. Logs are optional queue entries and may be dropped under pressure; required machine replies and complete `FOF_*` frames remain atomic.
- Use one bounded required-response queue and one bounded optional-event queue. Required command replies are serviced first. Optional detection frames may be dropped when the host is absent or slow, and every drop increments telemetry.
- A required reply that cannot be queued is returned as an explicit command failure when possible and increments `required_response_failures`; it is never silently reported as successful.
- Commands remain capped at 2047 bytes. Machine status frames remain capped at 8192 bytes for Android compatibility. `FOF_DET` frames remain below 2048 bytes.

### Compatible frames and commands

Lite adds these established machine records:

- startup: `FOF_READY`
- request: `FOF_PING`
- response: `FOF_PONG:<backend-lite-version>`
- request: `FOF_STATUS`
- response: `FOF_STATUS:<JSON>`
- live event: `FOF_DET:<JSON>`
- supported headless control success: `FOF_CTL_OK:<JSON>`
- supported headless control failure: `FOF_CTL_ERROR:<JSON>`
- investigation results: `FOF_INV:<JSON>` when backed by the existing Lite investigation core

The exact `FOF_DET` compatibility fields are:

- `id`
- `manufacturer`
- `badge_label`
- `badge_class`
- `badge_entity_key`
- `source`
- `confidence`
- `threat_score`
- `rssi`

The existing commands remain valid aliases:

- `FOF_BACKEND_STATUS`
- `FOF_AP_START`
- `FOF_BACKEND_OTA_STATUS`
- Lite `FOF_BACKEND_OTA_PROBE ...`
- Lite `FOF_BACKEND_OTA_APPLY ...`

Screen navigation, theme, display policy, and screen-debug controls return `FOF_CTL_ERROR` with `unsupported_capability`. They are not silently accepted. Wi-Fi and backend configuration remain owned by the AP portal rather than a second USB-specific NVS schema.

Badge-style host-supplied binary firmware upload is not part of this live-USB implementation. Direct ROM/USB flashing and the existing guarded Lite USB commands that trigger backend-validated OTA remain supported. A later binary-upload bridge must retain exact Lite target/family validation before it can be enabled in Android or New Dash.

### USB status

`FOF_STATUS` includes:

- truthful Lite firmware identity and hardware MAC
- `mode` and `mode_label`
- Wi-Fi, backend, AP, scanner, OTA, LED, upload queue, and USB health
- recent threat counts and bounded scanner summaries
- `reporting.standalone`, backend upload counters, and last-success age
- the capabilities `display_none`, `usb_live`, `usb_buffered`, `http_uplink`, `config_ap`, `ap_dashboard`, `remote_ota`, and `uart_relay_ota`
- USB queue depths, optional drops, required failures, bytes transmitted, bytes received, and host-connected state

The status schema is additive for New Dash. Android must explicitly allow the separate Lite identity before it treats the connection as verified or enables any mutation.

## Standalone AP Mode

### Lifetime

Standalone AP is a runtime-only latch. It is not stored in NVS.

- `FOF_AP_START` retains its existing behavior: start the configuration AP and allow the current success policy to close it.
- `FOF_AP_STANDALONE` starts the AP and latches standalone mode for the current boot.
- The portal provides “Keep standalone for this session” and “Exit standalone” actions.
- Standalone mode ends only on reboot, explicit exit, or fatal network teardown.
- Station Wi-Fi and backend HTTP upload may continue while the AP is latched; this is AP+STA operation, not a switch to local-only sensing.
- A boot with invalid configuration still opens the AP automatically, but it is not permanently latched unless the user selects standalone mode.

Saved Wi-Fi networks, backend URL, device name, location, AP password, and update preference continue to persist through the existing canonical configuration commit path. Only the standalone latch and event history are nonpersistent.

### Security boundary

- Dashboard and configuration routes are served only while the AP HTTP server is running.
- Existing AP-local destination checks apply to every new route; the dashboard is never exposed on the station interface.
- The configured AP password protects both setup and dashboard access. The factory password remains a first-boot default and the UI continues prompting users to change it.
- Configuration responses remain redacted. Dashboard APIs expose detection/status data but never Wi-Fi passwords, backend credentials, OTA secrets, or raw NVS records.
- No external scripts, fonts, CDNs, or internet assets are required.

## Ephemeral Dashboard and Event Ring

### PSRAM ring

- Allocate the ring from PSRAM only. Never fall back to internal RAM.
- Hard cap allocation at 65536 bytes.
- Store 128 fixed-size compact event records, each compile-time asserted to be no larger than 512 bytes.
- Begin event sequence numbers at 1. On the practically unreachable `uint64_t` wrap, clear the ring and restart at 1.
- Record canonical post-deduplication events even when the AP is closed, so opening the dashboard shows recent activity from the current boot.
- Reboot clears the ring. No event is written to flash, NVS, SPIFFS, the backend database, or a local file.
- Allocation failure disables history and reports `history_available:false`; scanning, LEDs, USB, AP configuration, and HTTP upload continue.
- Ring contention may drop a dashboard-history copy rather than block the coordinator. The drop counter is exposed in status.

Each compact record contains only dashboard/USB-relevant values: sequence, observation timestamp validity/time, detection ID, manufacturer/model, badge label/class/entity key, source, confidence, threat score, RSSI, estimated distance, available aircraft/operator coordinates, and scanner-slot mask.

### Portal routes

Keep the existing setup page and routes. Add:

- `GET /dashboard`: embedded live dashboard HTML/CSS/JavaScript
- `GET /api/dashboard/status`: bounded Lite/scanner/network/USB/ring status
- `GET /api/events?after=<sequence>&limit=<count>`: events after the cursor
- `POST /api/standalone/start`: latch standalone mode for this boot
- `POST /api/standalone/exit`: release the latch and return control to normal AP policy

Register the existing setup and configuration routes first. Treat every dashboard route as optional: failure to register one must disable the dashboard route set without rolling back the core setup server. The existing `/api/status` and USB status expose the dashboard-disabled reason even when `/api/dashboard/status` itself is unavailable.

`limit` defaults to 25 and is capped at 50. Responses are serialized from a bounded snapshot and sent in chunks; the HTTP handler never holds a ring or runtime lock while sending network data. Invalid cursors or limits receive a 400 response. A cursor older than the retained window returns the oldest retained sequence plus `cursor_reset:true` so the browser can recover without ambiguity.

The browser polls status and events once per second. The dashboard shows:

- Lite identity and current operating mode
- scanner 0/1 health
- Wi-Fi, backend, USB, and AP state
- threat counters and current LED/threat state
- a rolling table with time, class/label, manufacturer/ID, source, RSSI, and distance
- links between Dashboard and Setup
- standalone-session start/exit controls

The page keeps no browser database. Refresh reconstructs the view from the current PSRAM window.

## Client Compatibility

New Dash already discovers ESP32-S3 USB Serial/JTAG, sends `FOF_PING`, polls `FOF_STATUS`, and parses `FOF_DET`. The firmware protocol is therefore sufficient for live viewing. Its separate client branch should read `display_none` and hide all navigation, theme, and display-policy mutations for Lite.

Android already has the USB host transport and frame parsers, but its verified identity gate currently accepts only the native badge. A separate Android change must allow the exact Lite family tuple and gate mutations using capabilities. It must never route a native badge image to Lite or a Lite image to the native badge. No APK is changed or released by this firmware work.

### Delivery sequencing

This firmware branch delivers the truthful Lite wire protocol and fixtures that a host can consume. New Dash live viewing is part of the firmware acceptance because its current transport already accepts these frames. Current Android builds remain intentionally blocked by their native-badge-only identity gate until a separately reviewed Android client change allows the exact Lite tuple. Therefore this branch must not be described or released as Android-ready by itself, and completing this firmware work does not claim that an unchanged Android APK can connect to Lite.

## Failure and Backpressure Behavior

- Network upload is authoritative and enqueued before optional local fan-out.
- USB disconnect, host stalls, malformed commands, or optional-queue exhaustion do not pause scanners or HTTP upload.
- AP clients cannot consume the upload FIFO or USB queue; they read snapshots of the independent event ring.
- A slow dashboard response cannot hold the coordinator lock.
- A failed ring allocation or dashboard route registration leaves the setup portal usable; the existing setup status and USB status report the degraded reason.
- Required USB replies have bounded wait/flush behavior. A wedged host increments health counters and the service returns to accepting new sessions without rebooting the sensor pipeline.
- Screen-only controls fail explicitly and do not mutate configuration.
- Existing OTA locks and target validation remain authoritative. USB/dashboard work cannot bypass OTA ownership.

## Performance and Memory Acceptance

- PSRAM dashboard history is at most 64 KiB in addition to existing allocations.
- No new unbounded queue, response, string, or browser payload is allowed.
- On a XIAO ESP32-S3 hardware smoke test, the buffered machine-frame path must sustain at least 64 KiB/s for 60 seconds with zero malformed required frames and zero required-response loss.
- Under a representative detection burst with an active host, canonical-event enqueue-to-USB latency after coordinator release must remain below 100 ms at the 95th percentile.
- With USB disconnected or intentionally unread, backend upload and UART scanner health must remain unchanged; only optional USB drop telemetry may increase.
- The dashboard must show a newly canonicalized event within two one-second polls.

## Focused Verification

Verification is intentionally scoped to this feature rather than repeatedly running the entire backend suite:

1. Pure C tests for USB command framing, exact `FOF_DET` projection, truthful status identity, required/optional priority, and bounded failure behavior.
2. Pure C tests for ring wrap, cursor recovery, sequence behavior, PSRAM allocation failure, and snapshot limits.
3. Portal contract tests for all new routes, AP-local enforcement, redaction, limits, and standalone session transitions.
4. Build-contract checks proving the compatibility layer exists only in the Backend Lite uplink and protected native badge paths are unchanged.
5. One `uplink-s3-backend` firmware build and size report.
6. After separate explicit flash approval, one hardware smoke test for PING/STATUS/DET, USB throughput, USB-stall isolation, AP dashboard polling, setup save/reconnect, reboot clearing history, and continued backend upload.

## Completion Criteria

The feature is complete when:

- Backend Lite operates standalone with its existing HTTP uplink and LEDs.
- Connecting USB exposes compatible live status and detections without disabling HTTP.
- New Dash can open and consume the Lite stream without firmware identity spoofing.
- The AP can be latched for the current session and provides both Setup and Dashboard.
- The dashboard history is bounded, PSRAM-only, and empty after reboot.
- Native badge firmware, scanner firmware, native factory assets, and native release identity remain byte-for-byte untouched by this work.
- The Lite wire protocol and fixtures needed by a future Android client are documented, but this firmware branch makes no claim that an unchanged Android APK accepts Lite.
- Android follow-on requirements require exact-family validation and cannot accidentally enable cross-family flashing.
