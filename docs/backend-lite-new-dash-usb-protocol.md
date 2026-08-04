# Backend Badge Lite USB handoff for New Dash

## Scope

This is the client handoff for the no-screen Backend Badge Lite uplink. It
documents firmware target `uplink-s3-backend` only. The native badge, the S3
Fullsize backend platform, scanners, and host-supplied badge firmware upload
remain separate families. Android and New Dash may accept this Lite target only
through the exact identity and capability gate documented below; native-badge
matching and behavior remain unchanged.

The FastAPI backend needs no ingestion change for this feature. Badge Lite
continues its existing HTTP upload while USB is connected. New Dash must add
the acknowledged live exchange and the configuration client described below;
it may consume `FOF_DET` directly for live display without disabling or
duplicating the authoritative HTTP path.

## USB discovery and trust gate

Badge Lite uses the ESP32-S3 USB Serial/JTAG interface:

| Property | Value |
| --- | --- |
| USB VID | `0x303A` |
| USB PID | `0x1001` |
| Host serial setting | nominal `115200` baud |
| Wire implementation | native USB Serial/JTAG, not a UART baud-limited link |
| Driver buffers | 8192-byte RX and 8192-byte TX rings |

VID/PID identifies an Espressif USB Serial/JTAG candidate, not a trusted
firmware family. Other ESP32-S3 images can enumerate the same way. New Dash
must open a candidate, send `FOF_PING` and `FOF_STATUS`, and require all of the
following before enabling any mutation:

```text
product_family = badge_lite
target         = uplink-s3-backend
project        = fof_backend_uplink
hardware       = seeed_xiao_esp32s3
mode           = headless
```

The version in `FOF_PONG:<version>` must equal `FOF_STATUS.version`. Never
accept `fof_badge_uplink`, `uplink-s3-fof_badge`, the Fullsize tuple, a scanner
tuple, or VID/PID alone. If multiple serial candidates exist, probe each as
read-only and bind only the candidate whose fresh response passes this gate.
Re-enumeration can change the operating-system port name, so rediscover after
reset or OTA rather than retaining a path indefinitely.

## Framing and limits

Commands and responses are UTF-8, one frame per line, terminated by LF. A host
builder normally returns the line without LF and the serial writer appends one.
CRLF input is tolerated by the host fixture, but New Dash should send LF.

| Limit | Contract |
| --- | --- |
| Host command | at most 2047 bytes before LF |
| `FOF_STATUS` frame | at most 16384 bytes |
| `FOF_DET` frame | at most 1535 bytes |
| USB driver write | no individual call larger than 4096 bytes |
| Required response queue | 4 complete frames |
| Optional event queue | 32 complete frames |

Parse complete lines only. Ignore unrelated log lines. Do not search for a
machine prefix inside a partial or concatenated line. Required replies are
dequeued before optional detections/logs. Optional frames may be dropped under
host backpressure; USB pressure never pauses scanners or HTTP upload.

## Identity and capabilities

The `capabilities` array for this firmware contains:

```text
display_none
usb_live
usb_live_ack
usb_buffered
usb_config
http_uplink
config_ap
ap_dashboard
remote_ota
uart_relay_ota
```

`display_none` is authoritative: New Dash must hide navigation, theme, display
policy, screen-debug, and other LCD controls. It must not infer a display from
native-badge protocol compatibility. Additive status fields are allowed, but
the exact identity tuple and the capabilities needed for an operation remain
mandatory.

## Commands and responses

| Direction | Frame | Meaning |
| --- | --- | --- |
| Lite to host | `FOF_READY` | Dispatcher is ready. |
| Host to Lite | `FOF_PING` | Read-only liveness probe. |
| Lite to host | `FOF_PONG:<version>` | Firmware version, not sufficient identity proof by itself. |
| Host to Lite | `FOF_STATUS` | Request one bounded status snapshot. |
| Lite to host | `FOF_STATUS:<json>` | Truthful Lite identity, capabilities, Wi-Fi/backend/scanner/AP/LED/OTA/upload/USB/live/history state, plus a bounded native-compatible active `entities` array. |
| Lite to host | `FOF_DET:<json>` | Optional canonical detection event. |
| Lite to host | `FOF_INV:<json>` | Optional completed investigation result when available. |
| Lite to host | `FOF_CTL_OK:<json>` | Supported headless control accepted. |
| Lite to host | `FOF_CTL_ERROR:<json>` | Stable command failure such as `unsupported_capability`. |
| Host to Lite | `FOF_CONFIG_GET` | Request redacted canonical configuration. |
| Lite to host | `FOF_CONFIG:<json>` | Redacted configuration. |
| Host to Lite | `FOF_CONFIG_SET:<json>` | Validate, commit, and reconnect one atomic configuration update. |
| Lite to host | `FOF_CONFIG_OK:{"generation":<n>,"reconnect":<bool>}` | Commit succeeded; boolean reports immediate reconnect result. |
| Lite to host | `FOF_CONFIG_ERROR:{"reason":"<code>"}` | No atomic update was committed. |

Legacy read/control aliases remain available: `FOF_BACKEND_STATUS`,
`FOF_AP_START`, `FOF_BACKEND_OTA_STATUS`, guarded
`FOF_BACKEND_OTA_PROBE ...`, and guarded `FOF_BACKEND_OTA_APPLY ...`.
`FOF_AP_START` cannot override recovery eligibility.

The bounded status snapshot keeps the compact two-entry `scanner` connection
array and adds two richer `scanner_summaries`. A summary reports the slot,
connection and identity validity, whether scanner status is available, bounded
scanner identity, numeric scan profile (`0` through `3`), command/radio/role
health, RX/TX-drop counters, and uptime. Identity, profile, errors, and uptime
are `null` when scanner status is unavailable. `threats` reports active flags,
current counts, and last-seen ages; an age of `-1` means unavailable. The
upload queue capacity is `512`. Legal `led` values are `healthy`,
`network_degraded`, `drone`, `meta`, `drone_meta`, `fatal`, and `uart_lost`.
`backend.last_success_age_s` is `null` until the uploader has received its
first successful backend acknowledgement; afterward it is the elapsed whole
seconds, saturated to an unsigned 32-bit value.

USB status also carries at most eight active `entities`, using the native badge
field contract (`label`, `class`, `category`, `code`, `display_id`, `source`,
`source_id`, `score`, `confidence_pct`, `last_seen_s`, RSSI and event counters,
and `stale`). Drone and operator `lat`/`lon` fields are included only when the
scanner supplied valid nonzero coordinate pairs. Lite normalizes the active
Meta class to `meta` while preserving the existing `FOF_DET.badge_class` value
`meta_glasses`. Remote ID and Meta entries expire after 90 seconds; SSID/OUI
drone clues expire after 15 seconds. The recovery-AP dashboard continues to use
its redacted status plus the session event endpoint; active entities are added
only to the USB `FOF_STATUS` frame.

## Acknowledged live delivery

New Dash must not treat an open port, PING, STATUS, configuration traffic, or
`FOF_LIVE_START` as live delivery. It should send `FOF_LIVE_START` only after
its reader and downstream live-view path are active, then acknowledge every
fresh heartbeat immediately after parsing the complete frame.

```text
New Dash -> Lite  FOF_LIVE_START:{"client":"new_dash","protocol":1}
Lite -> New Dash  FOF_LIVE_READY:{"session_id":"<id>","heartbeat_ms":5000,"lease_ms":15000}
Lite -> New Dash  FOF_LIVE_HEARTBEAT:{"session_id":"<id>","sequence":1}
New Dash -> Lite  FOF_LIVE_ACK:{"session_id":"<id>","sequence":1}
                  [matching ACK grants/renews a 15000 ms firmware lease]
Lite -> New Dash  FOF_LIVE_HEARTBEAT:{"session_id":"<id>","sequence":2}
New Dash -> Lite  FOF_LIVE_ACK:{"session_id":"<id>","sequence":2}
New Dash -> Lite  FOF_LIVE_STOP:{"session_id":"<id>"}
Lite -> New Dash  FOF_LIVE_STOPPED:{"session_id":"<id>"}
```

The firmware makes a heartbeat acknowledgeable only after the entire frame is
physically written. An ACK is valid only for the current `LIVE_READY` session
and the latest successfully transmitted heartbeat sequence. If that heartbeat
was completed at monotonic time `last_sent_ms`, the firmware accepts it only
when `last_sent_ms <= ack_now_ms < last_sent_ms + 15000`; the exact 15000 ms
boundary is rejected. Deadline arithmetic saturates at signed 64-bit maximum,
and that exact saturated boundary also fails open. Wrong-session, older,
future, duplicate, replayed, or expired acknowledgements do not confirm
delivery. A new `LIVE_START`, reconnect, reboot, or `LIVE_STOP` invalidates
prior session state. Never reuse a buffered ACK after reconnect.

New Dash does not know the firmware's physical transmit-completion timestamp.
It must parse a complete current-session heartbeat and emit its ACK
immediately; firmware remains the freshness authority. The fixture's injected
`sent_at_ms`/`now_ms` values exist only to model boundary behavior
deterministically and must not be treated as host-observable wire data.

A matching ACK is also the only USB evidence allowed to suppress an eligible
recovery AP. If ACKs stop, the lease fails open after 15000 ms and firmware
reevaluates the AP within its next 500 ms network-policy tick.

## Recovery AP truth table

Recovery state is derived, not a user latch. The SSID is
`FriendOrFoe-Lite-<last-six-MAC>`.

| Wi-Fi configured | Station has IP | Complete saved-network pass failed | Current acknowledged USB lease | Lite AP |
| --- | --- | --- | --- | --- |
| no | no | n/a | no | on, reason `wifi_unconfigured` |
| no | no | n/a | yes | off; reason remains available for reopening |
| yes | no | no | either | off while the initial/retry association pass runs |
| yes | no | yes | no | on, reason `wifi_join_failed` |
| yes | no | yes | yes | off; reason remains available for reopening |
| either | yes | either | either | off |

Backend HTTP reachability does not open the AP while station Wi-Fi is
connected. Saving configuration starts a fresh association pass. If Wi-Fi is
still unavailable when a live lease stops or expires, the AP reopens
automatically.

`FOF_AP_START` returns `FOF_CTL_ERROR:{"reason":"recovery_ap_not_needed"}`
when Wi-Fi recovery is not eligible and
`FOF_CTL_ERROR:{"reason":"usb_live_confirmed"}` while a confirmed lease is
active. It is not a force-on switch.

## Configuration schema and redaction

Readback has this shape:

```json
{
  "schema_version": 1,
  "generation": 9,
  "networks": [
    {"ssid": "Lab", "password_set": true}
  ],
  "backend_url": "http://10.0.0.2:8000",
  "device_id": "uplink_CB77A4",
  "display_name": "Lite Lab",
  "ap_password_set": true,
  "auto_update_enabled": false,
  "has_location": false,
  "latitude": null,
  "longitude": null,
  "altitude_m": null
}
```

No response contains a Wi-Fi password or AP password. Only the
`password_set` and `ap_password_set` booleans disclose presence. Treat any raw
`password`, `wifi_pass`, `wifi_password`, or `ap_password` member in a
`FOF_CONFIG` response as a protocol/security failure and do not log it.

`FOF_CONFIG_SET` accepts a JSON object containing only these optional fields:

- `networks`: zero to four objects, each with required `ssid` and optional
  `password`. Omitting `password` for an SSID already saved preserves that
  network's existing password. Supplying the array replaces network order and
  membership atomically. For Lite only, `"networks":[]` clears every saved
  network and leaves the recovery AP eligible; do not extend this empty-list
  rule to Fullsize firmware.
- `backend_url`: HTTP URL used by the existing uplink.
- `display_name`: human label; this does not imply a display.
- `ap_password`: 8 to 63 bytes.
- `auto_update_enabled` and `confirm_auto_update`: enabling automatic update
  from false requires both booleans true in the same request.
- `has_location`, `latitude`, `longitude`, and `altitude_m`: when location is
  enabled, all values must be finite and latitude/longitude must be in range.

The atomic schema intentionally does not accept `device_id`; use the compatible
staged command for that field if needed. All input is validated before the
canonical configuration record is committed. A successful commit increments
`generation` by exactly one and starts Wi-Fi reassociation. A
`FOF_CONFIG_ERROR` leaves the generation and active record unchanged; discard
the failed candidate rather than updating New Dash state from it.

`FOF_CONFIG_OK:{"generation":10,"reconnect":false}` means the configuration
was committed but the immediate reconnect callback failed. Do not resend the
same mutation blindly; rediscover the device and read `FOF_CONFIG_GET` to
reconcile the saved generation. `FOF_CONFIG_ERROR` means the atomic update was
not committed.

The compatible staged path is:

```text
FOF_SET:wifi_ssid=<value>   -> FOF_OK:wifi_ssid
FOF_SET:wifi_pass=<value>   -> FOF_OK:wifi_pass
FOF_SET:backend_url=<value> -> FOF_OK:backend_url
FOF_SET:device_id=<value>   -> FOF_OK:device_id
FOF_SET:ap_pass=<value>     -> FOF_OK:ap_pass
FOF_SAVE                    -> FOF_SAVED
```

The staged Wi-Fi fields modify slot 0 without deleting slots 1-3 until a
successful save. Validation and commit failures return
`FOF_ERROR:<stable-reason>` without changing the active record.
`FOF_ERROR:reconnect_failed` is the exception: the record was saved but the
immediate reconnect failed, so rediscover and reconcile configuration before
retrying. Configuration traffic alone never confirms a live session.

## Detection frame

`FOF_DET` contains exactly these compatibility fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `id` | string | Detection identifier. |
| `manufacturer` | string | Projected manufacturer. |
| `badge_label` | string | `Drone`, `Meta Glasses`, or empty when unclassified. |
| `badge_class` | string | `drone`, `meta_glasses`, or empty. |
| `badge_entity_key` | string | Stable projected entity key. |
| `source` | unsigned integer | Firmware detection-source value. |
| `confidence` | finite number | Canonical confidence. |
| `threat_score` | integer | Rounded/clamped score, normally 0-100. |
| `rssi` | integer | Signed RSSI in dBm. |

The event is post-deduplication. It is an optional live copy: loss or queue
exhaustion may create sequence gaps from the host's perspective and is not a
reason to stop reading. HTTP ingestion remains independent and authoritative.

## Reconnect and retry guidance

1. On open, drain partial input to the next LF and perform a fresh read-only
   PING/STATUS identity gate.
2. Start a new live session only after New Dash's consumer is ready.
3. ACK each current-session heartbeat once. Do not synthesize ACKs on a timer.
4. If no heartbeat arrives for slightly more than 5000 ms, keep reading; after
   15000 ms without a valid exchange, assume the firmware lease is no longer
   confirmed and send a fresh `LIVE_START` rather than replaying an ACK. The
   firmware serializes this transition behind its TX gate, increments an
   internal generation, and purges/skips queued READY/heartbeat controls from
   the previous generation before publishing the new READY.
5. On disconnect/reset/OTA, discard the session ID, pending commands, and
   buffered machine lines, rediscover the port, and read STATUS again.
6. PING, STATUS, CONFIG_GET, and a fresh LIVE_START are safe to retry after a
   transport failure. Reconcile `generation` before retrying CONFIG_SET or
   staged SAVE.

The fixture's `LiveStartAttempt.generation` is a deterministic contract-model
tag, not a field in `FOF_LIVE_START` or `FOF_LIVE_READY`. A real host cannot
classify an arbitrarily delayed READY by its JSON content; retry safety comes
from the firmware TX-gate synchronization and stale-generation purge described
above. New Dash should still discard its local prior attempt/session whenever
it sends a fresh START.

## Unsupported screen controls

Commands beginning with screen/navigation/theme/display families, including
screen-only `FOF_SET` aliases, return:

```text
FOF_CTL_ERROR:{"reason":"unsupported_capability"}
```

Do not send them and do not present their UI. Badge Lite has no screen state to
configure.

## OTA family boundary

This USB feature does not accept a host-supplied firmware binary. Direct ROM
USB flashing remains a separate recovery operation. In application mode, only
the existing guarded backend-catalog commands are available:

```text
FOF_BACKEND_OTA_STATUS
FOF_BACKEND_OTA_PROBE <component> <exact-catalog-name> <sha256-or-*>
FOF_BACKEND_OTA_APPLY <component> <sha256> <newer-only|same-version-recovery> <expected-MAC> <expected-boot-id> <expected-topology-generation>
```

The exact Lite catalog names are `uplink-s3-backend` for component `uplink`
and `scanner-s3-combo-backend` for components `scanner0` and `scanner1`.

Before any update mutation, New Dash must revalidate the exact Lite identity
and the catalog target/project/hardware. Never offer or route a native badge,
Fullsize, or scanner image to the Lite uplink slot. An application reset may
change the serial path and always requires a fresh identity gate.

Android supports the same exact Lite tuple as a second, fail-closed product
kind. It binds Lite live-session frames to that verified USB owner, preserves
the existing native-badge identity gate, keeps cross-family flashing disabled,
and hides or blocks LCD/theme/navigation/display-policy controls for
`display_none` devices.

## New Dash implementation checklist

- Discover `0x303A:0x1001`, but trust only a fresh exact PONG/STATUS handshake.
- Add `FOF_LIVE_START`, parse `FOF_LIVE_READY`, and ACK only complete matching
  current-session heartbeats while the New Dash live consumer is active.
- Keep reading `FOF_DET` while firmware continues HTTP uplink; do not make USB
  attachment disable or replace HTTP ingestion.
- Add redacted CONFIG_GET plus identity-gated CONFIG_SET/staged configuration.
- Hide all screen controls when `display_none` is present.
- Surface recovery reason, AP state, USB queue/drop health, live confirmation,
  lease remaining, and scanner status from `FOF_STATUS`.
- On reconnect or update, discard the session and repeat the exact identity
  gate before mutation.

## Acceptance transcript

Values below are illustrative, but every shown frame is a complete literal
line. The host writes LF after each host frame.

```text
> FOF_PING
< FOF_PONG:0.2.0-backend
> FOF_STATUS
< FOF_STATUS:{"product_family":"badge_lite","target":"uplink-s3-backend","project":"fof_backend_uplink","hardware":"seeed_xiao_esp32s3","version":"0.2.0-backend","firmware_name":"uplink-s3-backend","app_project":"fof_backend_uplink","hardware_type":"seeed_xiao_esp32s3","hardware_id":"AA:BB:CC:DD:EE:FF","mac":"AA:BB:CC:DD:EE:FF","boot_id":305419896,"mode":"headless","mode_label":"Backend Badge Lite","config_generation":9,"capabilities":["display_none","usb_live","usb_live_ack","usb_buffered","usb_config","http_uplink","config_ap","ap_dashboard","remote_ota","uart_relay_ota"],"wifi":{"configured":false,"connected":false,"full_pass_failed":false},"recovery":{"reason":"wifi_unconfigured","ap_running":true},"scanner":[{"slot":0,"connected":true,"identity_valid":true},{"slot":1,"connected":false,"identity_valid":false}],"threats":{"drone_active":false,"meta_active":false,"drone_count":0,"meta_count":0,"drone_last_seen_age_ms":-1,"meta_last_seen_age_ms":-1},"led":"network_degraded","ota_ready":true,"upload":{"depth":0,"capacity":512,"dropped":0,"ok":0,"failed":0,"retries":0},"usb":{"available":true,"host_connected":true,"required_depth":0,"optional_depth":0,"optional_drops":0,"required_failures":0,"bytes_transmitted":0,"bytes_received":0,"output_poisoned":false},"live":{"started":false,"session_id":"","last_ack_sequence":0,"confirmed":false,"lease_remaining_ms":0},"history":{"available":true,"count":0,"contention_drops":0},"dashboard":{"enabled":true,"degraded_reason":null},"backend":{"reachable":false,"last_success_age_s":null},"counts":{"drone":0,"meta":0,"tracker":0,"wifi_anomaly":0,"ble":0,"other":0},"scanners":[{"slot":0,"connected":true,"identity_valid":true,"status_available":true,"identity":{"target":"scanner-s3-combo-backend","project":"fof_backend_scanner","hardware":"seeed_xiao_esp32s3","version":"0.2.0-backend"},"profile":1,"health":{"command":true,"radio":true,"role_acked":true},"errors":{"rx":0,"tx_drops":0},"uptime_ms":9000},{"slot":1,"connected":false,"identity_valid":false,"status_available":false,"identity":null,"profile":null,"health":{"command":false,"radio":false,"role_acked":false},"errors":null,"uptime_ms":null}],"entities":[],"scanner_summaries":[{"slot":0,"connected":true,"identity_valid":true,"status_available":true,"identity":{"target":"scanner-s3-combo-backend","project":"fof_backend_scanner","hardware":"seeed_xiao_esp32s3","version":"0.2.0-backend"},"profile":1,"health":{"command":true,"radio":true,"role_acked":true},"errors":{"rx":0,"tx_drops":0},"uptime_ms":9000},{"slot":1,"connected":false,"identity_valid":false,"status_available":false,"identity":null,"profile":null,"health":{"command":false,"radio":false,"role_acked":false},"errors":null,"uptime_ms":null}]}
> FOF_CONFIG_GET
< FOF_CONFIG:{"schema_version":1,"generation":9,"networks":[{"ssid":"Lab","password_set":true}],"backend_url":"http://10.0.0.2:8000","device_id":"uplink_CB77A4","display_name":"Lite Lab","ap_password_set":true,"auto_update_enabled":false,"has_location":false,"latitude":null,"longitude":null,"altitude_m":null}
> FOF_LIVE_START:{"client":"new_dash","protocol":1}
< FOF_LIVE_READY:{"session_id":"0123456789abcdef0123456789abcdef","heartbeat_ms":5000,"lease_ms":15000}
< FOF_LIVE_HEARTBEAT:{"session_id":"0123456789abcdef0123456789abcdef","sequence":1}
> FOF_LIVE_ACK:{"session_id":"0123456789abcdef0123456789abcdef","sequence":1}
< FOF_DET:{"id":"rid-1","manufacturer":"DJI","badge_label":"Drone","badge_class":"drone","badge_entity_key":"drone:rid-1","source":2,"confidence":0.91,"threat_score":91,"rssi":-54}
> FOF_CONFIG_SET:{"networks":[{"ssid":"Lab","password":"replacement-secret"}],"backend_url":"http://10.0.0.2:8000","display_name":"Lite Lab"}
< FOF_CONFIG_OK:{"generation":10,"reconnect":true}
```

## Host fixture

The standard-library-only transcript fixture is
`backend-firmware/tools/backend_lite_usb_fixture.py`. It does not discover or
open hardware. From the `backend-firmware/` directory:

```python
from tools.backend_lite_usb_fixture import (
    LiveHandshake,
    build_config_set,
    verify_lite_handshake,
)

device = verify_lite_handshake(pong_line, status_line)
handshake = LiveHandshake()
attempt = handshake.start()
session = handshake.accept_ready(attempt, live_ready_line)
# Deterministic contract-test inputs, not timestamps carried on the wire:
session.observe_heartbeat(heartbeat_line, sent_at_ms=1_000)
ack_line = session.ack(heartbeat_line, now_ms=1_001)
config_line = build_config_set(device, {"display_name": "Lite Lab"})
```

Production New Dash must not invent or estimate `sent_at_ms`; after receiving a
complete current heartbeat, it emits the matching ACK immediately and lets the
firmware enforce the physical-send freshness window.

Its tests use literal transcripts and require no serial port:

```bash
python -m pytest tools/tests/test_backend_lite_usb_fixture.py -q
```
