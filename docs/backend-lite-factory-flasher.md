# Backend Badge Lite Factory Flasher

The Backend Badge Lite factory flasher programs one complete headless Lite
assembly at a time on macOS. A Lite is exactly three Seeed XIAO ESP32-S3
boards, each with 8 MB flash and 8 MB PSRAM:

- scanner0, running the production ComboFO scanner image;
- scanner1, running the same production ComboFO scanner image; and
- the center/uplink board, running the Backend Badge Lite uplink image.

All three native USB ports are connected for the factory cycle. USB port order
does not assign a role. The tool writes a disposable probe to all three boards,
discovers the reciprocal two-link UART graph, and uses the physical graph to
assign BLE scanner, Wi-Fi scanner, and Lite uplink roles.

This is a separate product and a separate factory entry point from both of the
existing flashers:

- The native/full-badge factory flasher writes a native badge uplink and asks
  for a game role. Never use it for a Lite.
- The Backend Lite maintenance web flasher intentionally publishes only the
  `uplink-s3-backend` image. It does not identify or program scanner boards.
- The Lite factory flasher intentionally erases and programs all three Lite
  boards, then proves the resulting graph and runtime.

The implementation reuses the established native-badge factory's USB device
inventory, disposable topology probe, eFuse-MAC rebinding, flash engine, and
explicit readback machinery. It keeps a separate Lite bundle contract,
operator workflow, runtime verifier, and manufacturing ledger.

## Factory-only safety boundary

This is a destructive blank-board/rework process. Every production flash
erases its target before writing. Use it only when all three connected boards
are known to belong to a headless Backend Badge Lite assembly.

Before connecting the assembly:

1. Disconnect every unrelated ESP32/ESP32-S3 device from the Mac.
2. Confirm the unit has exactly three XIAO ESP32-S3 boards with 8 MB flash and
   8 MB PSRAM.
3. Confirm it is not a native/full badge and not a configured field unit.
4. Connect all three USB ports only when the tool asks for the unit.
5. Leave every port connected until the tool prints an explicit `PASS` or
   `FAIL`.

The interactive tool prints the erase and product warning once at startup.
After that, connect one complete assembly and press Enter once per unit. The
disposable probe proves the wiring graph and assigns all three roles
automatically. A blank XIAO board cannot electrically prove whether an
operator intended it for a Lite or for a native badge, so keep native/full
badges and unrelated ESP32 devices away from the Lite factory station.

Unattended operation is deliberately narrower. `--yes` is accepted only with
both `--once` and the exact product acknowledgment
`--confirm-product badge_lite`.

## Quick start

Install PlatformIO Core once so its Python environment contains the USB and
esptool dependencies. From `backend-firmware/`, double-click
`flash-lite-badges.command` in Finder or run:

```sh
./flash-lite-badges.command
```

The launcher always adds `--offline` and uses:

```text
tools/lite_factory_flasher/resources/lite-factory-flasher-embedded.zip
```

It therefore does not require GitHub or an internet connection on the factory
Mac. The Python entry point is also offline by default; checking final GitHub
backend releases requires the explicit `--online` option. The command loops
for a batch: remove the prior unit, connect one complete Lite assembly, press
Enter once, and wait for `PASS` or `FAIL`. Then swap in the next assembly and
press Enter again. No typed product token or second removal prompt is required.
To run one explicitly acknowledged unit without prompts:

```sh
./flash-lite-badges.command \
  --yes --once --confirm-product badge_lite
```

For diagnostics, an operator can invoke the Python entry point directly from
the repository root:

```sh
$HOME/.platformio/penv/bin/python \
  backend-firmware/tools/lite_badge_factory.py --offline --once
```

`--bundle PATH` selects a local immutable ZIP bundle after the same strict
manifest, identity, size, partition mapping, and digest validation. A local
candidate that is not already in the flasher's trusted release-digest registry also
requires `--once --accept-candidate-sha256 DIGEST`, using the exact digest
printed by the deterministic builder. A local bundle older than the embedded
version is rejected unless an intentional rework also adds
`--allow-downgrade`. A unit whose complete three-MAC graph already has a PASS
record is rejected unless the operator adds `--allow-rework`; rework must
rediscover the same authoritative role graph, not merely the same three MACs.

## Pinned mixed-version bundle

Lite is intentionally a mixed-version assembly. The deterministic factory
bundle contains exactly these three layouts:

| Layout | Project | Target | Version | Production use |
| --- | --- | --- | --- | --- |
| probe | `fof_badge_factory_probe` | `factory-probe-s3` | `1.0.0` | Disposable graph discovery on all three boards |
| scanner | `fof_badge_scanner` | `scanner-s3-combo-fof_badge` | `0.67.2-badge-defcon34` | The identical production image on scanner0 and scanner1 |
| uplink | `fof_backend_uplink` | `uplink-s3-backend` | `0.2.0-backend` | The topology-selected center board |

All three layouts declare hardware identity `seeed_xiao_esp32s3`. The builder
copies probe and scanner bytes from the already accepted native-badge factory
bundle only after its archive digest exactly matches:

```text
038d83adcc3e6a561a9192e8bed26ec205e7e7c9374eb6ff800baf573bb44576
```

It does not compile or substitute a Backend scanner. In particular,
`scanner-s3-combo-backend` is forbidden by the Lite bundle validator. The
uplink bytes must match the checked-in Backend release index and must embed the
exact Backend project, target, hardware, and version. A native badge uplink is
also forbidden. The production loader independently pins every accepted probe
and scanner part by size and SHA-256, including their bootloaders, partition
tables, OTA data, and applications. Embedded and explicitly online release
selection is additionally limited to complete bundle digests reviewed in this
flasher version; manifest-supplied hashes alone are not a release trust root.

The production write order is BLE scanner, Wi-Fi scanner, then Lite uplink.
The scanner boards use one shared production artifact; their physical role and
profile come from the proven wiring graph, not from distinct scanner binaries.

## Probe, flash, and PASS gates

The tool fails closed unless exactly three ESP32-S3 candidates are present. It
tracks every board by immutable eFuse base MAC across USB renumbering and
requires 8 MB flash plus 8 MB PSRAM on each board.

For every unit, the factory cycle is:

1. Validate the complete bundle before touching hardware.
2. Inventory exactly three supported boards and reject any MAC from the prior
   still-connected unit.
3. Erase/write the disposable topology probe to every board, verify the write,
   and perform an explicit digest readback.
4. Accept only one complete reciprocal graph with a center/uplink and the two
   expected scanner leaves. Missing, extra, duplicated, stale, or
   nonreciprocal links fail before production images are written.
5. Erase/write the two pinned production scanner images and the Backend Lite
   uplink, with write verification and a separate readback for every declared
   flash region.
6. Rebind all boards to their original eFuse MACs and hand each board from ROM
   to its application without reflashing.
7. Require a fresh uplink PONG/status session, exact Backend uplink identity,
   blank factory configuration, both exact scanner identities and topology
   MACs, correct profiles, acknowledged roles, healthy radios/command paths,
   and zero scanner transport errors.
8. Sample the runtime twice for stability.
9. Reset and hand off all three boards again without writing, repeat the
   runtime checks, and prove that every boot ID changed while configuration,
   identities, roles, and health remained exact.
10. Append and fsync a PASS record only after every gate succeeds.

The blank-configuration gate requires the factory schema/generation, no saved
Wi-Fi networks, the default Backend URL, the MAC-derived default device and
display names, a provisioned factory AP password, automatic update disabled,
and no saved location. The runtime gate verifies the uplink target
`uplink-s3-backend`, project `fof_backend_uplink`, and bundle version. Each
scanner must report target `scanner-s3-combo-fof_badge`, project
`fof_badge_scanner`, hardware `seeed_xiao_esp32s3`, pinned scanner version,
the topology-bound hardware MAC, the expected profile, `role_acked`, healthy
command/radio state, and clean RX/drop counters.

`FAIL` means the unit is not approved even if one or more writes completed.
Keep the three boards together as a rework unit and rerun the complete factory
cycle after correcting the reported condition. The tool never converts a
partial result into PASS.

## Private manufacturing records

The default ledger directory is:

```text
~/Documents/FoF Backend Badge Lite Factory/
```

It contains:

```text
lite-factory.jsonl
lite-factory.csv
```

The directory is forced to mode `0700`; each append-only record file is forced
to `0600`, flushed, and fsync'd. `lite-factory.jsonl` is the sole authoritative
PASS commit and rework index; `lite-factory.csv` is a rebuildable operator
projection. A CSV projection failure therefore produces a warning after the
durable JSONL PASS instead of misreporting the unit as FAIL. PASS records
include the three topology-bound MACs, artifact evidence, runtime/reboot
evidence, bundle digest, and a random receipt. FAIL attempts are recorded
separately and do not create a prior-PASS approval. Public terminal output is
scrubbed.

The flasher holds a per-user, process-wide advisory lock from before USB
inventory until the authoritative record commit. The lock is independent of
the selected `--records` directory, so choosing another ledger cannot create a
second hardware writer. A second factory process fails before hardware access,
preventing interleaved resets, erases, writes, or ledger appends.

Use `--records PATH` only to select another private manufacturing directory.
Do not delete or edit the ledger to bypass a prior-PASS or graph-conflict
check.

## Build an offline candidate bundle

From the repository root, build the deterministic ZIP with:

```sh
python3 backend-firmware/tools/build_lite_factory_bundle.py \
  --output /tmp/lite-factory-flasher-v0.2.0-backend.zip \
  --version 0.2.0-backend
```

The defaults read:

- the pinned accepted scanner/probe source at
  `tools/badge_flasher/resources/badge-factory-flasher-embedded.zip`;
- `backend-firmware/release/backend-release-index.json`; and
- the indexed artifacts under `backend-firmware/web-flasher/firmware/`.

The builder validates the accepted source archive hash, every part size and
digest, exact partition offsets, embedded ESP-IDF app identities, required
target/hardware markers, and the requested Backend version. It writes sorted
members with fixed timestamps into a sibling temporary file, reloads that file
through the production bundle validator, fsyncs it, and only then atomically
replaces the requested output. A failed build cannot truncate an existing
known-good bundle. Building a ZIP is not permission to flash it; use
`--bundle PATH --once --accept-candidate-sha256 DIGEST` only in a controlled
candidate acceptance run.

## Candidate and release boundary

The checked-in Lite factory tool and embedded mixed-version bundle are a local
factory candidate. Development of this tool did not flash connected hardware
and did not modify Lite, scanner, native-badge, Android, or Backend firmware.
It assembled existing pinned scanner/probe bytes with the verified Backend
uplink artifact and added host-side factory orchestration and verification.

The combined Lite factory cycle has not, by that fact alone, received physical
three-board hardware acceptance, and this documentation does not claim that it
has been publicly released. Do not publish a `lite-factory-flasher-v*.zip`,
create a public release, or treat a successful host-side test as factory
acceptance until a controlled hardware run has demonstrated the complete
probe, write/readback, runtime, and second-reboot PASS sequence and its
evidence has been reviewed. Keep release/tag automation isolated from unrelated
Android, native-badge, scanner, and Backend firmware release workflows before
creating any release tag.
