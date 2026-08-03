# Backend Badge Lite Fast USB and Recovery AP Dashboard Design

## Status

Approved in conversation on 2026-08-03 and amended with the user's recovery-AP and acknowledged-New-Dash rules. This design applies only to the separate Backend Badge Lite firmware family on branch `codex/backend-firmware`.

## Goal

Make the no-screen Backend Badge Lite uplink useful in three modes without changing the native badge firmware:

1. Standalone sensor with Wi-Fi/HTTP upload and threat LEDs.
2. USB-connected live sensor for New Dash, with the same established machine protocol available to Android after its separate exact-family allowlist change.
3. Automatic Lite recovery AP when Wi-Fi is unconfigured or cannot associate, with persistent device configuration and a small local dashboard backed by nonpersistent PSRAM event history.

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
- Do not modify the FastAPI service under `backend/` or any New Dash client in this firmware implementation. Deliver their required USB protocol changes as a handoff specification only.
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
- `backend_usb_service`: ESP-IDF driver setup, RX framing, required/optional queues, acknowledged live-session state, and the sole TX worker.
- `backend_usb_config`: staged compatibility settings plus atomic JSON configuration through the existing canonical commit/reconnect boundary.
- `backend_lite_ap_policy`: Lite-only recovery eligibility driven by Wi-Fi association and an acknowledged USB live lease; Fullsize keeps its existing AP policy.
- `backend_dashboard_event`: projection from canonical observations into one compact event used by USB and the dashboard.
- `backend_event_ring`: fixed-capacity, PSRAM-only rolling history with sequence/cursor snapshots.
- extensions to `backend_config_portal`: Lite recovery identity, dashboard HTML, and bounded status/event routes.

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

Lite also adds an acknowledged live-session exchange for New Dash:

- request: `FOF_LIVE_START:{"client":"new_dash","protocol":1}`
- response: `FOF_LIVE_READY:{"session_id":"<boot-unique-id>","heartbeat_ms":5000,"lease_ms":15000}`
- firmware heartbeat: `FOF_LIVE_HEARTBEAT:{"session_id":"<id>","sequence":<n>}`
- host acknowledgement: `FOF_LIVE_ACK:{"session_id":"<id>","sequence":<n>}`
- request: `FOF_LIVE_STOP:{"session_id":"<id>"}`
- response: `FOF_LIVE_STOPPED:{"session_id":"<id>"}`

`FOF_LIVE_START` alone does not suppress the recovery AP. The first acknowledgement matching the current session and heartbeat confirms delivery, and each later matching acknowledgement renews a 15-second lease. A stale session, future sequence, duplicate sequence used after expiry, generic `FOF_PING`, or USB enumeration without protocol traffic never counts as confirmed delivery. Lease expiry and `FOF_LIVE_STOP` immediately clear confirmation. This is the sole USB condition allowed to suppress the Lite recovery AP.

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

`FOF_AP_START` remains parseable for old tools, but it cannot override the Lite eligibility rules. It succeeds only while Wi-Fi is unconfigured or a complete configured-network association pass has failed and there is no acknowledged live USB lease; otherwise it returns an explicit `recovery_ap_not_needed` or `usb_live_confirmed` error.

### USB configuration

USB and the AP portal share one configuration record, validator, commit function, and Wi-Fi reconnect path. No second NVS schema is introduced.

For native-badge-compatible basic setup, Lite accepts staged commands for `wifi_ssid`, `wifi_pass`, `backend_url`, `device_id`, and `ap_pass` using `FOF_SET:<key>=<value>`, replies with `FOF_OK:<key>`, and commits only after `FOF_SAVE`. `wifi_ssid` and `wifi_pass` address network slot 0 without exposing or silently deleting other saved slots. A failed validation or commit leaves the active record unchanged and returns `FOF_ERROR:<reason>`; successful commit returns `FOF_SAVED` and starts Wi-Fi association.

For complete Lite setup, New Dash may use:

- `FOF_CONFIG_GET`
- `FOF_CONFIG:<redacted-JSON>`
- `FOF_CONFIG_SET:<JSON>` using the same bounded schema as the AP `/api/config` update
- `FOF_CONFIG_OK:{"generation":<n>,"reconnect":true}`
- `FOF_CONFIG_ERROR:{"reason":"<stable-code>"}`

The JSON command supports up to four saved networks, backend URL, display name, AP password, automatic-update preference with the existing explicit confirmation, and optional location. It is validated and committed atomically rather than staged. Readback reports SSIDs and password-presence booleans but never password values. Every USB configuration reply is a required frame. Configuration traffic by itself does not claim a confirmed New Dash live session and therefore does not suppress a needed recovery AP.

Screen navigation, theme, display policy, and screen-debug controls return `FOF_CTL_ERROR` with `unsupported_capability`. They are not silently accepted.

### Firmware-update boundary

Badge-style host-supplied binary firmware upload is not part of this live-USB implementation. Direct ROM/USB flashing and the existing guarded Lite USB commands that trigger backend-validated OTA remain supported. A later binary-upload bridge must retain exact Lite target/family validation before it can be enabled in Android or New Dash.

### USB status

`FOF_STATUS` includes:

- truthful Lite firmware identity and hardware MAC
- `mode` and `mode_label`
- Wi-Fi, backend, AP, scanner, OTA, LED, upload queue, and USB health
- recent threat counts and bounded scanner summaries
- recovery reason, configured-network pass state, backend upload counters, and last-success age
- the capabilities `display_none`, `usb_live`, `usb_live_ack`, `usb_buffered`, `usb_config`, `http_uplink`, `config_ap`, `ap_dashboard`, `remote_ota`, and `uart_relay_ota`
- USB queue depths, optional drops, required failures, bytes transmitted, bytes received, host-connected state, live-session ID, last acknowledged heartbeat sequence, confirmation state, and lease remaining

The status schema is additive for New Dash. Android must explicitly allow the separate Lite identity before it treats the connection as verified or enables any mutation.

## Lite Recovery AP Mode

### Eligibility and lifetime

Recovery mode is derived runtime state, not a stored setting or a user latch.

- Start the Lite AP immediately when configuration is missing, invalid, or contains no Wi-Fi networks.
- With valid saved networks, attempt every configured network using the existing 15-second per-network timeout. Start the Lite AP only after one complete association pass fails.
- Keep station retries running in AP+STA mode. Close the AP immediately after the station receives an IP address.
- A backend HTTP outage never opens the AP while station Wi-Fi is connected.
- A valid, acknowledged New Dash live-session lease also closes or suppresses the AP. USB enumeration, power-only attachment, PING/STATUS traffic, configuration traffic, and an unacknowledged `FOF_LIVE_START` do not.
- When an acknowledged live lease expires or stops and Wi-Fi is still unconfigured or join-failed, reopen the AP automatically.
- After a previously working Wi-Fi link drops, retry a complete configured-network pass before opening the AP, avoiding AP churn for a brief link interruption.
- Saving a new configuration clears the join-failed state and starts a fresh association pass. The AP remains available until Wi-Fi joins or a New Dash live lease becomes confirmed.

The recovery AP SSID is Lite-specific: `FriendOrFoe-Lite-<last-six-MAC>`. Fullsize AP naming and behavior are unchanged.

Saved Wi-Fi networks, backend URL, device name, location, AP password, and update preference persist through the existing canonical configuration commit path. Recovery eligibility and event history are nonpersistent.

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

Register the existing setup and configuration routes first. Treat every dashboard route as optional: failure to register one must disable the dashboard route set without rolling back the core setup server. The existing `/api/status` and USB status expose the dashboard-disabled reason even when `/api/dashboard/status` itself is unavailable.

`limit` defaults to 25 and is capped at 50. Responses are serialized from a bounded snapshot and sent in chunks; the HTTP handler never holds a ring or runtime lock while sending network data. Invalid cursors or limits receive a 400 response. A cursor older than the retained window returns the oldest retained sequence plus `cursor_reset:true` so the browser can recover without ambiguity.

The browser polls status and events once per second. The dashboard shows:

- Lite identity and current operating mode
- scanner 0/1 health
- Wi-Fi, backend, USB, and AP state
- threat counters and current LED/threat state
- a rolling table with time, class/label, manufacturer/ID, source, RSSI, and distance
- links between Dashboard and Setup
- the exact recovery reason (`wifi_unconfigured` or `wifi_join_failed`) and a notice that the AP closes after Wi-Fi joins or New Dash confirms live delivery

The page keeps no browser database. Refresh reconstructs the view from the current PSRAM window.

## Client Compatibility

New Dash already discovers ESP32-S3 USB Serial/JTAG, sends `FOF_PING`, polls `FOF_STATUS`, and parses `FOF_DET`, so the basic Lite stream remains compatible. Its separate client change must additionally perform the `FOF_LIVE_START`/heartbeat acknowledgement lease before firmware may suppress the recovery AP. It should read `display_none`, hide navigation/theme/display-policy mutations, and use the USB configuration commands for Lite setup.

Android already has the USB host transport and frame parsers, but its verified identity gate currently accepts only the native badge. A separate Android change must allow the exact Lite family tuple and gate mutations using capabilities. It must never route a native badge image to Lite or a Lite image to the native badge. No APK is changed or released by this firmware work.

### Delivery sequencing

This firmware branch delivers the truthful Lite wire protocol, a host-side protocol fixture, and a separate New Dash/backend handoff document. No FastAPI or New Dash source is modified. The fixture must prove live subscription, heartbeat acknowledgement, AP suppression/lease expiry, redacted configuration read, atomic configuration write, and detection parsing. Current Android builds remain intentionally blocked by their native-badge-only identity gate until a separately reviewed Android client change allows the exact Lite tuple. Therefore this branch must not be described or released as Android-ready by itself, and completing this firmware work does not claim that an unchanged Android APK can connect to Lite.

## Failure and Backpressure Behavior

- Network upload is authoritative and enqueued before optional local fan-out.
- USB disconnect, host stalls, malformed commands, or optional-queue exhaustion do not pause scanners or HTTP upload.
- AP clients cannot consume the upload FIFO or USB queue; they read snapshots of the independent event ring.
- A slow dashboard response cannot hold the coordinator lock.
- A failed ring allocation or dashboard route registration leaves the setup portal usable; the existing setup status and USB status report the degraded reason.
- Required USB replies have bounded wait/flush behavior. A wedged host increments health counters and the service returns to accepting new sessions without rebooting the sensor pipeline.
- An invalid, stale, or missing live acknowledgement cannot suppress the recovery AP. Lease expiry is evaluated from monotonic time and fails open to AP recovery when Wi-Fi is unavailable.
- USB configuration uses the existing validator and transactional commit/reconnect callbacks; malformed or failed changes do not partially update NVS.
- Screen-only controls fail explicitly and do not mutate configuration.
- Existing OTA locks and target validation remain authoritative. USB/dashboard work cannot bypass OTA ownership.

## Performance and Memory Acceptance

- PSRAM dashboard history is at most 64 KiB in addition to existing allocations.
- No new unbounded queue, response, string, or browser payload is allowed.
- On a XIAO ESP32-S3 hardware smoke test, the buffered machine-frame path must sustain at least 64 KiB/s for 60 seconds with zero malformed required frames and zero required-response loss.
- Under a representative detection burst with an active host, canonical-event enqueue-to-USB latency after coordinator release must remain below 100 ms at the 95th percentile.
- With USB disconnected or intentionally unread, backend upload and UART scanner health must remain unchanged; only optional USB drop telemetry may increase.
- A confirmed live lease suppresses an eligible recovery AP within one 500 ms network-policy tick; expiry restores it within one tick when Wi-Fi remains unavailable.
- The dashboard must show a newly canonicalized event within two one-second polls.

## Focused Verification

Verification is intentionally scoped to this feature rather than repeatedly running the entire backend suite:

1. Pure C tests for USB command framing, exact `FOF_DET` projection, truthful status identity, live-session heartbeat/ack/lease behavior, required/optional priority, and bounded failure behavior.
2. Pure C tests for ring wrap, cursor recovery, sequence behavior, PSRAM allocation failure, and snapshot limits.
3. Pure C policy tests for unconfigured boot, a complete failed Wi-Fi pass, successful association, transient disconnect retry, confirmed USB suppression, stale/wrong acknowledgement rejection, and lease expiry.
4. USB configuration tests for staged compatibility keys, atomic JSON updates, redaction, validation/commit failure rollback, and reconnect behavior.
5. Portal contract tests for all new routes, AP-local enforcement, redaction, limits, Lite-specific SSID, and setup availability after optional dashboard-route failure.
6. Build-contract checks proving the compatibility layer exists only in the Backend Lite uplink, `backend/` is untouched, and protected native badge/scanner paths are unchanged.
7. A host-side New Dash protocol fixture that exercises live acknowledgement, lease renewal/expiry, configuration, and detections without changing New Dash source.
8. One `uplink-s3-backend` firmware build and size report.
9. After separate explicit flash approval, one hardware smoke test for PING/STATUS/DET, acknowledged live suppression, lease-expiry AP recovery, USB configuration/reconnect, throughput, USB-stall isolation, AP dashboard polling, reboot-cleared history, and continued backend upload.

## Completion Criteria

The feature is complete when:

- Backend Lite operates standalone with its existing HTTP uplink and LEDs.
- Connecting USB exposes compatible live status and detections without disabling HTTP.
- The host fixture can confirm and maintain a New Dash live lease without firmware identity spoofing.
- The Lite AP provides Setup and Dashboard only while Wi-Fi is unconfigured/join-failed and no acknowledged New Dash live lease exists.
- USB can read redacted configuration and atomically update Wi-Fi/backend settings through the canonical commit path.
- The dashboard history is bounded, PSRAM-only, and empty after reboot.
- Native badge firmware, scanner firmware, native factory assets, and native release identity remain byte-for-byte untouched by this work.
- The FastAPI backend and New Dash source remain untouched, with their exact follow-on requirements captured in a handoff specification.
- The Lite wire protocol and fixtures needed by a future Android client are documented, but this firmware branch makes no claim that an unchanged Android APK accepts Lite.
- Android follow-on requirements require exact-family validation and cannot accidentally enable cross-family flashing.
