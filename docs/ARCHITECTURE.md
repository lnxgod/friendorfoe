# Architecture

Friend or Foe is three platforms that share one detection pipeline: an
Android AR viewer, a Python FastAPI backend, and ESP32-S3 sensor firmware.
Detection logic — Bayesian fusion, OpenDroneID parsing, DJI IE parsing,
WiFi Beacon RID parsing — is implemented byte-for-byte identically on
Android (Kotlin) and ESP32 (C). This is a deliberate correctness signal:
two independent stacks producing the same answer for the same payload.

## End-to-End Pipeline

```
       ┌─────────── ESP32-S3 Scanner (dual-core) ────────────┐
       │  Core 0 (radio):    WiFi promiscuous + BLE NimBLE    │
       │  Core 1 (proc):     5 parsers + Bayesian fuse        │
       │                                                      │
       │  Parsers:  OpenDroneID │ DJI DroneID IE │ WiFi       │
       │            Beacon RID  │ SSID prefix    │ OUI        │
       │                                                      │
       │  ↓ JSON over UART1 @ 921 600 baud (newline-framed)   │
       └───────────────────────┬──────────────────────────────┘
                               │
       ┌───────────────────────┴──────────────────────────────┐
       │              ESP32-S3 Uplink (HTTP relay)            │
       │  • Receives detections from up to 2 scanners         │
       │  • fw_store cache: latest scanner image, persistent  │
       │  • fw_auto_check task: pulls backend every 30 min    │
       │  • OTA rollback: bad self-image reverts on WiFi fail │
       │                                                      │
       │  ↓ POST /detections/drones (batched JSON)            │
       └───────────────────────┬──────────────────────────────┘
                               │
       ┌───────────────────────┴──────────────────────────────┐
       │           Backend (FastAPI + Postgres + Redis)        │
       │                                                      │
       │  Ingest → enrich (OUI, BLE company-ID, signature)     │
       │         → classify (drone/tracker/glasses/AP)         │
       │         → entity_tracker (cross-source correlation)   │
       │         → triangulate (Gauss-Newton / circle / RSSI)  │
       │         → anomaly_detector (evil twin, karma, etc.)   │
       │         → Postgres + 50k-item ring buffer             │
       │                                                      │
       │  Serves: /detections/grouped, /map, /probes,          │
       │          /firmware/latest/{name} (OTA pull endpoint)  │
       │          /static/dashboard.html (operator console)    │
       └───────────────────────┬──────────────────────────────┘
                               │
       ┌───────────────────────┴──────────────────────────────┐
       │              Android (Kotlin + Compose)               │
       │                                                      │
       │  ARCore + compass-math fallback for floating labels  │
       │  Local detection sources (BLE RID, WiFi, ADS-B)      │
       │  Same Bayesian engine + same parsers as ESP32        │
       │  SkyObjectRepository merges remote + local           │
       └─────────────────────────────────────────────────────────┘
```

## Hardware Topology

A typical site is one or more **uplinks** (ESP32-S3 N16R8) each carrying
two **scanners** (S3-combo or S3-combo-seed) over UART. The uplink talks
to the backend over WiFi STA. Phones run the Android app independently —
they don't require a backend, but a deployed backend gives them
multi-source enrichment and triangulation.

Scanner-to-uplink wiring is documented in `docs/wiring-diagram.html`.

## Partitioning (S3 boards)

| Slot | Purpose |
|---|---|
| `nvs` | WiFi creds, calibration, fw_store metadata, crash counter |
| `otadata` | dual-OTA bookkeeping |
| `ota_0` / `ota_1` | active + standby app slots, used by ESP-IDF rollback |
| `fw_store` (uplink) / `fw_scanner_*` (16 MB uplink) | cached scanner firmware for over-UART relay |
| `fw_self` (16 MB uplink only) | reserved for staged self-OTA |
| `storage` | offline detection queue + node config |

Two layers of OTA safety: ESP-IDF rollback at the slot level (any boot
that doesn't `mark_valid` reverts on next watchdog), and a crash-counter
in NVS that drives a `fw_check` re-request to the uplink when running on
a validated-but-still-crashing image. Both layers are described in
[`../esp32/CHANGELOG.md`](../esp32/CHANGELOG.md) entries svc155 and svc156.

## Data Flow Highlights

- **Bayesian fusion runs on the scanner**, not the backend. Each source
  emits a likelihood ratio; the scanner composes log-odds with 30 s
  half-life decay. The backend receives a fused confidence per device.
  See [`BAYESIAN_FUSION.md`](BAYESIAN_FUSION.md).
- **Triangulation runs on the backend** because it requires multi-sensor
  RSSI from positioned nodes. Three or more sensors → Gauss-Newton NLLS;
  two → circle-circle intersection; one → RSSI range circle.
  See [`TRIANGULATION.md`](TRIANGULATION.md).
- **Entity tracking** (`backend/app/services/entity_tracker.py`)
  correlates BLE fingerprints, WiFi probe-request IE hashes, and AP
  hotspots into a single logical entity, surviving MAC randomization.
- **Auto-OTA** is pull-based: uplinks poll
  `GET /nodes/firmware/latest/{name}` every 30 min; if version differs
  they fetch `/nodes/firmware/download/{name}`. Self-OTAs the uplink and
  refreshes the cached scanner image so the connected scanner can pick
  it up via the existing `fw_check` / `fw_offer` / `fw_ready` flow.

## Reproducibility

Each platform has its own build path; full commands in the top-level
[`../README.md`](../README.md) and [`../CLAUDE.md`](../CLAUDE.md).
Native (host-only) tests cover detection policy, SSID patterns, OpenDroneID
parsing, DJI IE parsing, scanner rollback policy, and fw_auto_check
decision logic — `cd esp32 && pio test -e test`. Backend tests cover the
HTTP API, classifier sources, RF anomaly detector, triangulation,
entity grouping, and the new firmware-pull endpoints — `cd backend && pytest`.
