# PSRAM Allocation Policy (S3 production nodes)

Applies to `uplink-s3` and `scanner-s3-combo` firmware (N16R8 hardware: 8 MB
octal PSRAM, 16 MB flash). Other targets keep their existing allocation
behavior; legacy ESP32 nodes have no PSRAM and this document does not apply.

## The rules

1. **Regular `malloc()` / `calloc()` allocate from internal SRAM.** The
   sdkconfig sets `CONFIG_SPIRAM_USE_CAPS_ALLOC=y` (NOT `USE_MALLOC`) so
   default allocations never silently migrate to external RAM. This keeps
   RID parsing, hot-path buffers, and the legacy heap-stability patterns
   (raw sockets + static buffers + confidence filter + backpressure) behaving
   exactly as they do on non-PSRAM builds.

2. **Large / non-critical buffers go through `psram_alloc()`** (from
   `esp32/shared/psram_alloc.h`). The helper tries PSRAM first and falls
   back to internal SRAM if PSRAM is unavailable or exhausted.

3. **Use `psram_alloc_strict()` when you'd rather skip the feature than
   consume internal SRAM.** Returns NULL on non-PSRAM boards; callers treat
   that as "feature disabled".

4. **Never allocate the following from PSRAM:**
   - Anything in the RID parse / emit path (BLE Remote ID, ASTM F3411
     frames, DJI IE, WiFi Beacon RID, drone_detection_t for RID sources).
   - UART RX / TX buffers and scanner↔uplink JSON serialization scratch.
   - ISR-reachable data and task stacks (FreeRTOS requires internal SRAM
     for these).
   - WiFi driver internal buffers (the driver chooses for itself; we do
     set `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` so lwIP pbufs benefit
     from PSRAM where safe).
   - cJSON scratch for outbound detection payloads — we rely on static
     `snprintf` buffers for HTTP POSTs.

5. **Things that SHOULD go to PSRAM:**
   - **Uplink offline detection queue** — large ring buffer so a WiFi
     outage doesn't drop detections. 2 MB covers ~10 minutes of steady
     traffic.
   - **Uplink HTTP upload buffer** — 64 KB replaces the current 4 KB
     static so we can batch more detections per round trip.
   - **Uplink firmware-staging scratch** during `/api/fw/upload` — lets
     upload buffers grow beyond 4 KB for faster staging.
   - **Scanner BLE dedup cache** — enlarged MAC → last-seen table for
     better MAC-rotation tracking without rate-limiting RID.
   - **Scanner BLE-JA3 entity table** — on-scanner fingerprint map so we
     emit entity hints to the uplink (reduces backend work).
   - **Scanner WiFi probe-IE ledger** — longer TTL history of probe-
     request fingerprints.

6. **All RID frames bypass rate-limits and dedup caches.** The cache is
   consulted only for non-RID traffic. See `feedback_rid_top_priority.md`.

## Observability

At runtime, call `psram_free_size()` and `psram_total_size()` for diagnostics.
Targets to hold in live telemetry:

- Internal SRAM free: ≥ 150 KB at idle (matches pre-PSRAM baseline).
- PSRAM free: ≥ 5 MB at idle (out of 8 MB).
- Heap guard threshold: 32 KB free internal → warn; 4 KB free → reboot
  (unchanged from legacy policy; PSRAM exhaustion never reboots).

If either budget goes red in Pool's 24 h soak, investigate the largest
recent PSRAM allocation before shipping to the rest of the fleet.

## When NOT to use `psram_alloc()`

- For tiny allocations (< 512 bytes). PSRAM access is ~3-5 × slower than
  internal SRAM and has worse caching behavior for small objects.
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` already forces allocations
  < 16 KB to internal regardless of where `heap_caps_malloc(SPIRAM)` is
  called — but be explicit and use `malloc()` for small things.
- For anything in a tight loop or inner decoder path. Prefer a static
  internal buffer.
