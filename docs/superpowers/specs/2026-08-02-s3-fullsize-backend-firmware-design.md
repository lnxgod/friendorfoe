# S3 Fullsize Backend Firmware Design

**Date:** 2026-08-02

**Status:** Approved for implementation planning

**Branch:** `codex/backend-firmware`

**Extends:** `2026-08-01-backend-sensor-firmware-design.md`

## Goal

Add the classic 16 MB, three-board ESP32-S3 sensor assembly to the isolated
backend firmware family without modifying the accepted native Badge firmware
or changing the existing Badge Lite hardware contract. The upgraded assembly
is managed as **S3 Fullsize**, runs headless, uploads detections to the FastAPI
backend, uses its RGB LEDs for local threat indication, and supports guarded
same-family remote updates after a one-time attended USB conversion.

Only the all-S3 trio is supported. Mixed ESP32, ESP32-C5, or older OLED-node
assemblies are explicitly outside this design.

## Supported Product Families

The platform exposes three product families. Human-facing labels and stable
machine identities are deliberately separate.

| Product family | Firmware line | Physical hardware | Presentation |
| --- | --- | --- | --- |
| `badge` | `native_badge` | Three Seeed XIAO ESP32-S3 boards | Existing native Badge screen and controls |
| `badge_lite` | `backend` | Three Seeed XIAO ESP32-S3 boards without a screen | Single onboard yellow LED |
| `s3_fullsize` | `backend` | Three full-size ESP32-S3 N16R8 boards | GPIO48 WS2812 RGB LED; OLED unused |

The accepted native Badge release remains exactly
`0.67.2-badge-defcon34`. Its source, targets, manifests, artifacts, factory
bundle, and flashing tools are protected inputs and are not modified by this
work.

Badge Lite retains its existing firmware identities so already-built or
installed backend images do not become orphaned:

| Component | Target | Project | Hardware profile |
| --- | --- | --- | --- |
| Badge Lite uplink | `uplink-s3-backend` | `fof_backend_uplink` | `seeed_xiao_esp32s3` |
| Badge Lite scanner | `scanner-s3-combo-backend` | `fof_backend_scanner` | `seeed_xiao_esp32s3` |

S3 Fullsize receives distinct identities:

| Component | Target | Project | Hardware profile |
| --- | --- | --- | --- |
| S3 Fullsize uplink | `uplink-s3-fullsize-backend` | `fof_backend_uplink_fullsize` | `esp32s3_n16r8_fullsize` |
| S3 Fullsize scanner | `scanner-s3-combo-fullsize-backend` | `fof_backend_scanner_fullsize` | `esp32s3_n16r8_fullsize` |

One backend release builds all four backend images at the same
`FOF_VERSION_BACKEND`. Target, project, hardware profile, and product family
remain independent of the version string and prevent cross-family updates.

## Isolation and Source Architecture

All new implementation remains under the existing top-level
`backend-firmware/` tree and the backend service. Nothing under the protected
native `esp32/` or `android/` trees is changed.

The recommended implementation uses one backend sensing codebase with two
compile-time hardware profiles:

- Badge Lite selects the existing 8 MB XIAO partitions, GPIO1-4 UART wiring,
  and active-low GPIO21 yellow-LED driver.
- S3 Fullsize selects 16 MB partitions, classic GPIO15-18 UART wiring, and the
  GPIO48 WS2812 RGB-LED driver.

Detector, policy, UART protocol, AP, HTTP upload, buffering, health, command,
and OTA logic remain shared. Hardware-dependent values live behind a small,
explicit profile boundary. Builds fail when no profile or conflicting
profiles are selected. Badge Lite stays the existing default PlatformIO
environment; Fullsize is never selected implicitly.

This avoids a second drifting copy of the backend firmware while retaining a
hard source boundary from the native Badge firmware.

## Hardware Contract

One S3 Fullsize unit consists of one uplink and two identical scanner boards.
The uplink assigns scanner roles at runtime: slot 0 is `ble_primary`, slot 1 is
`wifi_primary`, and one surviving scanner may become `hybrid_failover`.

All links use 921600 baud, 8N1, 3.3-V signaling, no hardware flow control, and
a common ground.

| Link | Scanner pins | Uplink pins |
| --- | --- | --- |
| Slot 0 / BLE-primary | TX GPIO17, RX GPIO18 | RX GPIO18, TX GPIO17 |
| Slot 1 / Wi-Fi-primary | TX GPIO17, RX GPIO18 | RX GPIO16, TX GPIO15 |

The OLED is no longer part of the S3 Fullsize runtime contract. Fullsize builds
do not initialize the display, create a display task, compile display assets,
or treat OLED failure as a health fault. Existing OLED wiring may remain
physically attached but is ignored.

Each Fullsize board drives its onboard WS2812 RGB LED on GPIO48. No external
LED is required.

## Partition Layout

S3 Fullsize uses 16 MB flash-specific partition tables. NVS, OTA data, and PHY
offsets remain aligned with the original production layout so the attended
migration can preserve the NVS region.

The scanner layout retains two 3 MB OTA application slots, one 1 MB storage
partition, and reserved capacity. The uplink layout provides two 2 MB OTA
application slots, a 3 MB `fw_scanner_be` cache large enough for a Fullsize
scanner image, one 1 MB storage partition, and reserved capacity.

The bootloader, partition table, initial OTA data, and application are written
during the one-time USB conversion. The NVS region is not erased. New firmware
accepts compatible saved configuration where possible and starts its setup AP
when configuration is missing or invalid.

Partition labels consumed by runtime code stay stable. Image-size validation
uses the selected target's actual application-slot and scanner-cache
capacities rather than a shared 8 MB assumption.

## Management Identity

Firmware type must be visible throughout the management path. Every runtime
status report, heartbeat, USB status record, catalog entry, release manifest,
and OTA operation exposes or resolves these fields:

- `product_family`: `badge`, `badge_lite`, or `s3_fullsize`;
- `firmware_line`: `native_badge` or `backend`;
- `component`: `uplink` or `scanner`;
- `firmware_target`;
- application project;
- hardware profile;
- firmware version;
- full hardware MAC;
- runtime scanner role when the component is a scanner;
- capabilities such as display type, LED type, HTTP uplink, configuration AP,
  and remote OTA.

The backend derives `badge` for protected native Badge images from their
existing exact targets, so the native firmware does not need to change.
Backend firmware emits its family explicitly in status and heartbeat payloads;
the unique embedded target/project/hardware identity remains the authoritative
OTA binding.

Original `uplink-s3` and `scanner-s3-combo` 0.63 identities are recognized as
legacy S3 Fullsize devices with `migration_required: true`. They remain
ineligible for cross-family remote OTA. After USB conversion, all three boards
must report exact S3 Fullsize backend identities before the backend advertises
remote-update readiness.

Management surfaces group supported binaries under `Badge`, `Badge Lite`, and
`S3 Fullsize`. Scanner and uplink choices remain separate and always display
their exact target. The Badge `0.67.2-badge-defcon34` USB/factory choice remains
the default; backend images never masquerade as Badge images.

## Runtime Data Flow

Both Fullsize scanners use the same application image. The uplink supplies the
slot role and time, receives newline-delimited detection/status records over
UART, applies cross-slot deduplication, buffers observations during outages,
and uploads unfiltered detection evidence and heartbeats over HTTP.

The uplink owns the aggregate threat state. It sends the selected LED state to
both scanners so all three boards present the same nearby-threat condition.
Scanning and local threat indication continue while the backend is
unreachable. The bounded offline queue drains after connectivity returns.

Firmware delivery follows the same network-to-UART topology as the native
Badge: the uplink receives and validates scanner firmware over its backend
network connection, stages the exact scanner image locally, and relays it to
one scanner at a time over that scanner's existing UART link. Scanner boards
do not require infrastructure Wi-Fi, a configuration AP, or a direct backend
connection for updates.

The configuration AP exists only on the uplink. It configures up to four
ordered Wi-Fi networks, backend URL, device ID/display name, optional location,
AP password, and automatic-update preference. Scanner radio time is reserved
for sensing rather than independent configuration portals.

## LED Semantics

The logical LED states are shared with Badge Lite. Each hardware profile owns
only the physical rendering:

| State | S3 Fullsize RGB behavior | Badge Lite behavior |
| --- | --- | --- |
| Healthy | Brief green heartbeat | Existing brief yellow heartbeat |
| Backend/network degraded | Amber double pulse | Existing yellow degraded pattern |
| Drone nearby | Alternating purple and orange | Existing yellow drone timing pattern |
| Meta Glasses nearby | Alternating red and blue | Existing yellow Meta timing pattern |
| Drone and Meta | One complete drone sequence, then one complete Meta sequence | Existing combined timing sequence |
| Scanner UART lost | Repeating yellow warning | Existing long yellow blink |
| Fatal or unrecoverable state | Rapid red warning | Existing fatal yellow pattern |

Threat state takes priority over network degradation. Fatal state takes
priority over every other state. Pattern changes interrupt the current cycle
quickly rather than waiting for a full old-state sequence.

## Initial USB Conversion

The move from original 0.63 firmware to S3 Fullsize backend firmware is an
attended migration, not an OTA update.

For this initial conversion only:

1. Inventory all three USB devices without writing and bind each MAC to
   BLE scanner, Wi-Fi scanner, or uplink.
2. Read and hash one complete 16 MB flash backup per board.
3. Store backups and manifests in a private mode-0700 evidence directory;
   never commit them because they can contain credentials.
4. Generate explicit MAC-bound restore commands before the first write.
5. Flash and verify the BLE scanner.
6. Flash and verify the Wi-Fi scanner.
7. Flash the uplink only after both scanners pass.
8. Configure or validate the AP settings, then require exact three-board
   heartbeat and role evidence.

Any failure stops the sequence. Unmodified boards remain untouched, and a
failed converted board can be restored from its exact USB backup.

The complete flash backup is **not** repeated for routine updates. It is a
one-time bridge across the untrusted 0.63 identity and partition boundary.
Operators retain the original backups for disaster recovery.

## Same-Family Remote OTA

Remote OTA becomes available only after the initial conversion has established
exact S3 Fullsize identities and healthy scanner roles.

The uplink obtains its own image and the scanner image from exact backend
catalog names over the network. Uplink self-updates are written locally.
Scanner updates use the badge-style staged UART relay: the uplink downloads and
verifies the complete image, places it in the 3 MB `fw_scanner_be` cache,
pauses normal traffic for exactly one scanner, completes the guarded UART OTA
handshake, and streams sequenced chunks with integrity checks and bounded
retries. The other scanner and the uplink remain available. Scanner 0 and
scanner 1 are updated serially, never concurrently.

Before any network download, cache write, UART relay, or OTA-slot write, the
backend and device validate:

- product family and firmware line;
- component/image kind;
- target, project, and hardware profile;
- version policy and explicit same-version recovery mode when applicable;
- image size against the selected partition capacity;
- embedded identity record and ESP application descriptor agreement;
- SHA-256 and CRC32;
- operation token/generation and idle OTA state.

The uplink writes its inactive OTA slot. The UART relay writes each scanner's
inactive slot and returns progress, retry, completion, and error state through
the uplink to the backend. A component must boot, report its exact identity,
rejoin its assigned UART role, and satisfy health confirmation before its
update is accepted. Failure triggers ESP-IDF rollback or leaves the previously
accepted slot active. The uplink must restore normal UART detection traffic
after either success or a bounded failure.

Normal OTA does not capture a new full-flash backup. Its recovery layers are
the inactive application slot, health-gated rollback, the retained initial USB
backup, and attended USB recovery when both OTA slots are unusable.

Cross-family paths fail closed. Badge, Badge Lite, S3 Fullsize, legacy 0.63,
and unknown images cannot be substituted based only on chip type or filename.

## Backend and Catalog Changes

The FastAPI firmware catalog adds the two S3 Fullsize targets with exact
project, hardware, image-kind, partition-capacity, family, line, component,
and capability metadata. Existing Badge and Badge Lite entries gain consistent
family metadata without changing their binary identities.

Fleet/status responses show migration state and remote-update eligibility.
OTA endpoints require current runtime identity to match the requested catalog
family. Missing, stale, or contradictory identity produces a conflict response
without contacting the node.

The backend release index and private artifact bundle add both Fullsize images
and their bootloader, partition table, initial OTA data, manifest, hashes, and
flash offsets. Badge artifacts remain owned by the existing protected Badge
release/factory process.

## Failure Handling

- Invalid or conflicting hardware-profile build flags stop compilation.
- Missing firmware family or identity stops catalog ingestion and OTA.
- A scanner that fails role assignment remains radio-quiescent but keeps its
  command/recovery channel available.
- Loss of one scanner permits bounded hybrid failover; it does not silently
  relabel the missing component as healthy.
- Backend loss preserves local detection and bounded buffering while showing
  the degraded LED state when no higher-priority threat is active.
- AP configuration remains available after prolonged backend failure or an
  explicit USB request.
- Initial migration failure uses the MAC-bound original backup. Routine OTA
  failure uses inactive-slot rollback first and USB recovery only if needed.

## Verification Strategy

Software verification is focused on affected surfaces and consolidated to
avoid repeatedly running the full backend suite.

Required checks include:

- unit tests for hardware-profile selection, UART maps, LED colors/patterns,
  identity mapping, image capacities, and family capability metadata;
- build-contract tests for all four backend targets;
- clean PlatformIO builds for both Badge Lite images and both S3 Fullsize
  images;
- identity-record, application-descriptor, partition-table, manifest, release
  index, and hash verification for every backend artifact;
- focused FastAPI catalog, status, migration-readiness, and OTA compatibility
  tests;
- source-isolation and Git path audits proving no native Badge or Android
  source changed.

The attended Fullsize hardware canary verifies:

- one-time inventory, backup integrity, and restore-command generation;
- scanners-first USB conversion and exact post-boot identities;
- AP provisioning and backend reconnect;
- exact roles and continuous UART health;
- real RF drone and Meta detection with the specified RGB patterns on all
  three boards;
- offline buffering and later upload;
- network delivery to the uplink followed by serialized same-family UART
  recovery/update for the BLE scanner and Wi-Fi scanner;
- same-family network self-update for the uplink;
- post-update identity, role, upload, and rollback health.

Remote-update support is not declared ready until the attended canary proves
the full path after USB conversion.

## Documentation Deliverables

Documentation will include:

- a three-family support and identity matrix;
- exact Badge Lite and S3 Fullsize wiring diagrams;
- partition and artifact tables;
- LED meanings for yellow-only and RGB hardware;
- configuration-AP instructions;
- a dedicated S3 Fullsize one-time migration/restore runbook;
- a routine OTA and rollback runbook that explicitly does not require repeated
  full-flash backups;
- a network-to-uplink-to-UART scanner-update runbook with staging, progress,
  retry, interruption, and recovery behavior;
- management-field and migration-state definitions;
- unsupported mixed-generation hardware warnings.

## Non-Goals

- No native Badge firmware changes.
- No Android or APK changes.
- No OLED support in S3 Fullsize backend firmware.
- No ESP32-C5, classic ESP32, or mixed-generation assembly support.
- No direct remote conversion from 0.63.
- No universal binary that guesses wiring or flash size at runtime.
- No repeated full-flash backup requirement for normal OTA.
