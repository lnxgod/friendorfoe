# FoF Badge Factory Flasher Design

## Purpose

Build an open-source, macOS-first production tool that lets an operator plug
all three blank XIAO ESP32-S3 boards from one assembled FoF badge into USB,
press Enter once, and receive a verified result. The tool must determine which
physical board is the display/uplink board from the badge's internal UART
topology, flash one uplink and two scanners, and refuse to guess when the
hardware cannot be classified safely.

The primary production target is approximately 150 ESP32-S3 boards before
DEF CON. The interaction therefore optimizes for repeatability, clear failure
recovery, and proof rather than raw write speed.

## Operator Experience

The executable opens with a short ANSI animation inspired by the original NES
waterfall and sword reveal. It uses a GameChangersAI Triforce badge-flasher
mark and old-school BBS styling without reproducing copyrighted game art. The
animation can be skipped with any key and is automatically suppressed by
`--no-intro`, `NO_COLOR`, or a non-interactive terminal.

After the intro, the persistent production screen shows:

```text
  GAMECHANGERSAI // TRIFORCE BADGE FORGE

  Plug one complete badge into USB: UPLINK + SCANNER + SCANNER
  Three new ESP32-S3 devices required. Press ENTER when ready.

  UPLINK    [discovering]  MAC --:--:--:--:--:--  version --
  SCANNER 1 [waiting]      MAC --:--:--:--:--:--  role --
  SCANNER 2 [waiting]      MAC --:--:--:--:--:--  role --

  Batch: 0007     Passed: 6     Failed: 0     Firmware: embedded safe
```

Each phase updates in place: USB discovery, topology probe, classification,
artifact selection, erase/write, readback, boot, and trio verification. Success
ends with a large green `TRIFORCE VERIFIED - BADGE COMPLETE`. A failure remains
red and visible until the operator acknowledges it. The tool then asks the
operator to unplug the completed badge before accepting the next set of three
ports, preventing devices from adjacent badges from being mixed.

Plain text logs remain available with `--plain` for accessibility, CI, and
support capture. Color is never the only indication of state.

## Hardware Classification

### Why the display is not the detector

The badge's ST7735 panel uses write-only SPI: MOSI and clock are connected but
MISO is disabled. The ESP32 ROM bootloader cannot safely prove that a panel is
attached. Production firmware identity is also unavailable because the boards
are initially blank. The factory tool therefore detects the display/uplink
position from the assembled badge's UART wiring instead of attempting an
unreliable screen probe.

### Temporary topology probe

The repository will contain a small ESP-IDF factory-probe target. The same
probe image is temporarily written to all three blank boards. It performs no
radio scanning and does not touch NVS. On boot it:

1. Reports its chip MAC and probe protocol version over native USB serial.
2. Opens link A on TX GPIO1/RX GPIO2 and link B on TX GPIO3/RX GPIO4 at a
   conservative fixed baud rate.
3. Exchanges bounded challenge/response frames containing a session nonce,
   sender MAC, link identifier, sequence number, and CRC.
4. Reports the distinct peer MAC observed on each physical link.

The assembled badge wiring creates an unambiguous graph:

- the display/uplink position sees one peer on link A and a different peer on
  link B;
- each scanner position sees exactly one peer on link A and no peer on link B.

The host accepts classification only when all of these invariants hold:

- exactly three ESP32-S3 MACs were discovered;
- exactly one node reports two distinct peers;
- exactly two nodes report one peer each;
- all reported peer MACs belong to the same three-device session;
- the graph is reciprocal and forms one two-edge star;
- no duplicate MAC, unknown peer, self-loop, CRC failure, or stale session is
  present.

The center node becomes the uplink. Both leaf nodes receive the identical badge
scanner image. Their BLE-primary and Wi-Fi-primary roles are assigned later by
the production uplink over the already fixed physical UART slots.

### Classification fallback

Ambiguous topology never triggers a production flash. The UI explains which
link is missing and offers two explicit recovery choices:

- fix the badge UART wiring and rerun automatic discovery; or
- run `--manual-map UPLINK_PORT SCANNER_PORT SCANNER_PORT`, which displays the
  selected MAC mapping and requires the operator to type the six-character
  confirmation code shown by the tool.

There is no one-key “flash anyway” action. A future button-assisted fallback
may be added, but it is not required for the first production release.

## USB Identity And Port Stability

The host snapshots serial ports only after the operator presses Enter. Exactly
three newly connected ESP32-S3 devices are required unless three explicit
ports are supplied. Other serial devices are ignored after chip-family probing.

Before any firmware mutation, the tool runs an esptool ROM query on each port
and records:

- chip family and revision;
- base MAC address;
- flash size and detected PSRAM capability when available;
- macOS USB registry path/location identifier; and
- current serial device name.

MAC address is the durable device key. Device names such as
`/dev/cu.usbmodem101` are treated as temporary. After every reset, the host
re-enumerates ports and binds them back to the original MAC. If a MAC disappears
or two ports claim the same MAC, the batch stops.

Only one production flash operation runs at a time. Sequential writes are
slower than three-way parallel writes but make USB power, logs, and failure
ownership deterministic. Discovery and read-only verification may run in
parallel.

## Firmware Sources

### Embedded safe bundle

The distributed tool includes a known-good badge factory bundle containing:

- topology probe bootloader, partition table, and application;
- badge uplink bootloader, partition table, OTA data initializer, and app;
- badge scanner bootloader, partition table, OTA data initializer, and app;
- a JSON manifest with target, project, hardware, version, offsets, byte sizes,
  and SHA-256 digests for every image; and
- the bundle schema and minimum compatible flasher version.

These are bundled as package resources rather than base64 source literals, so
the repository remains reviewable and release artifacts remain reproducible.
The embedded bundle is the offline fallback and is never replaced in place.

### GitHub update check

At startup the tool queries public GitHub releases for
`lnxgod/friendorfoe`. It skips drafts, Android-only releases, releases missing a
complete badge factory bundle, and versions that cannot be strictly ordered.
It uses a remote bundle only when:

- the bundle schema is supported;
- both badge targets share the exact same embedded firmware version;
- every file matches the bundle manifest's size and SHA-256;
- the app descriptors contain the expected project, target, hardware, and
  version markers;
- the candidate is strictly newer than the embedded bundle; and
- the bundle is atomically downloaded and validated before the operator begins
  a batch.

If the network is offline, GitHub rate-limits the request, or validation fails,
the tool visibly selects `EMBEDDED SAFE` and continues. It never combines files
from different releases and never selects a release merely because its tag is
newer. `--offline` disables the check; `--bundle PATH` selects a locally
provided, fully validated bundle.

The GitHub Actions firmware workflow will publish the complete factory bundle
alongside the existing standalone and tarball assets. The tool and manifest
format are open source in this repository.

## Flashing Flow

One production batch follows this state machine:

1. **WAITING** - require the prior badge to be unplugged, then prompt for one
   complete three-board badge.
2. **ROM PREFLIGHT** - discover exactly three ESP32-S3 MACs and reject other
   chips, duplicate devices, undersized flash, or missing expected PSRAM.
3. **PROBE FLASH** - write the validated topology probe to each board and
   verify its application region by digest readback.
4. **TOPOLOGY** - start a unique probe session, collect all three reports, and
   validate the reciprocal star graph.
5. **PRODUCTION FLASH** - flash the uplink bundle to the center MAC, then flash
   the scanner bundle to each leaf MAC. Each full layout includes bootloader,
   partitions, OTA data, and application at manifest-defined offsets.
6. **FLASH VERIFY** - compare every written region against the selected bundle
   using esptool digest verification.
7. **BOOT VERIFY** - collect `FOF_IDENT` from all three production images and
   require exact target, project, hardware, and version values.
8. **TRIO VERIFY** - query `FOF_STATUS` from the uplink and require both scanner
   MACs, distinct BLE-primary/Wi-Fi-primary roles, live UART command paths,
   expected radio health, 8 MiB PSRAM, normal recovery state, rollback cleared,
   and matching firmware versions.
9. **RECORD** - append a durable batch record and display PASS or FAIL.
10. **UNPLUG** - wait until all three batch MACs leave USB before accepting the
    next badge.

If a write is interrupted, rerunning the batch begins from ROM preflight and
rewrites a complete layout. The probe is disposable; no recovery path depends
on it remaining installed.

## Verification And Failure Policy

The tool fails closed before or during production flashing for:

- anything other than exactly three selected ESP32-S3 devices;
- ambiguous or non-reciprocal topology;
- mismatched firmware identity, version, size, hash, offset, or chip family;
- a downgrade unless the operator supplied a local bundle and the explicit
  `--allow-downgrade` flag plus confirmation code;
- USB disappearance that cannot be rebound to the same MAC;
- flash readback mismatch;
- missing production `FOF_IDENT` output;
- scanner UART, role, radio, rollback, safe-mode, PSRAM, or version failures;
  or
- inability to prove the uplink status after bounded retries.

Failures do not automatically erase or retry indefinitely. The UI prints the
failed phase, device MAC, command summary, and a concise recovery instruction.
Low-level esptool output is captured in the batch log and available through
`--verbose`.

## Manufacturing Records

The default log directory is
`~/Library/Application Support/FoF Badge Flasher/`. Each run creates:

- an append-only CSV summary suitable for spreadsheet inventory;
- a JSON Lines event log with phase timestamps and machine-readable evidence;
  and
- one text transcript per failed batch.

Successful records include batch ID, UTC timestamp, tool version, bundle
source, firmware version, bundle SHA-256, all three MACs, final roles, USB
locations, flash verification results, runtime verification results, and
elapsed time. Logs contain no Wi-Fi credentials, access tokens, or unrelated
serial data.

The operator can add a human badge/asset label before pressing Enter. Reusing a
MAC already present in the local pass ledger produces a warning and requires
confirmation, preventing accidental duplicate work while still permitting
intentional reflashing.

## Repository Structure

The feature will be divided into focused units:

- `tools/badge_flasher/` - Python package and `python -m badge_flasher` entry;
- `tools/badge_flasher/ui.py` - ANSI intro and production dashboard;
- `tools/badge_flasher/devices.py` - macOS enumeration, esptool probing, and
  MAC-to-port rebinding;
- `tools/badge_flasher/topology.py` - probe report parsing and graph validation;
- `tools/badge_flasher/bundles.py` - embedded/local/GitHub bundle selection and
  validation;
- `tools/badge_flasher/flash.py` - manifest-driven writes and readback;
- `tools/badge_flasher/verify.py` - production identity and `FOF_STATUS` gates;
- `tools/badge_flasher/records.py` - CSV/JSONL manufacturing ledger;
- `tools/badge_flasher/resources/` - embedded factory bundle;
- `esp32/factory-probe/` - temporary topology-probe firmware; and
- `scripts/badge-flasher` - repository launcher with dependency checks.

The first supported environment is macOS with Python 3.11 or newer,
PlatformIO/esptool, and native USB access. Core topology, bundle, state-machine,
and record logic remains platform-neutral so Linux and Windows device adapters
can be added later.

## Open-Source And Supply-Chain Boundary

Source code, probe firmware, bundle schema, CI packaging, operator docs, and
tests live in the public repository under its existing MIT License. Distributed
source and binary packages include that license and preserve the existing
`Copyright (c) 2026 GAMECHANGERSai` notice.

The ANSI intro uses original GameChangersAI/FoF composition and generic
waterfall, sword, cave, and Triforce-inspired terminal geometry. It must not
embed Nintendo sprites, extracted art, music, or text.

## Testing

Host-side tests use Python's standard test tooling and mocks only at the USB,
esptool, serial, filesystem, clock, and GitHub boundaries. They cover:

- accepted and rejected three-node topology graphs;
- stale sessions, duplicate MACs, unknown peers, self-loops, and partial links;
- USB device renumbering and MAC rebinding;
- embedded, remote, offline, incomplete, corrupt, downgrade, and mixed-version
  bundle selection;
- exact manifest offset, size, identity, and digest validation;
- state-machine recovery from disconnects at every phase;
- PASS only after all flash and runtime gates succeed;
- accessible plain output and deterministic ANSI-disabled snapshots; and
- CSV/JSONL record integrity and secret exclusion.

The factory-probe firmware has native tests for frame encoding/decoding, CRC,
nonce/session rejection, peer deduplication, bounded retry timing, and topology
report construction. A pseudo-terminal integration harness runs three probe
instances as a virtual star and validates the complete host classification
without physical hardware.

Hardware acceptance uses the currently available three-board badge:

1. prove a correct automatic topology classification ten consecutive times;
2. prove swapped macOS USB enumeration order does not change role assignment;
3. flash the complete trio from the embedded bundle;
4. prove exact readback and runtime identities;
5. prove final BLE-primary and Wi-Fi-primary role health through `FOF_STATUS`;
6. disconnect one internal UART and confirm the tool refuses to classify;
7. interrupt one USB write and confirm a subsequent full rerun recovers; and
8. disconnect networking and confirm the embedded-safe workflow still passes.

The tool is ready for the 150-board run only after automated tests pass and the
physical acceptance checklist is recorded with real MACs and firmware version.

## Non-Goals

The first release does not:

- flash multiple physical badges simultaneously;
- infer roles from macOS port names or plug order;
- use Wi-Fi, BLE, HTTP, or Android as a firmware transport;
- modify the badge's production scanning or display behavior;
- accept an incomplete GitHub release;
- silently downgrade firmware;
- provide a general-purpose flasher for production sensor-node hardware; or
- reproduce proprietary NES assets.
