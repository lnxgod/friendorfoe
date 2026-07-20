# FoF Badge Factory Flasher

The badge factory flasher programs one assembled three-board DEF CON badge at a
time on macOS. All three XIAO ESP32-S3 USB ports are connected together; the
operator does not choose roles or ports.

## Quick start

1. Install PlatformIO Core once.
2. Connect exactly one complete badge (all three USB ports) to the Mac.
3. Double-click `flash-badges.command`, or run:

   ```bash
   ./flash-badges.command
   ```

4. Press Enter when prompted. Do not disconnect the badge until the terminal
   prints `PASS`.
5. Remove the completed badge and connect the next one.

The terminal stays open for the next badge. For automation or acceptance
testing, run one cycle with:

```bash
./flash-badges.command --once --yes
```

Use `--offline` to skip the public GitHub release check. The validated firmware
embedded in the repository remains available without a network connection.
Passing `--bundle PATH` selects only that local validated bundle and never
contacts GitHub. An older local bundle is rejected unless the operator adds the
explicit `--allow-downgrade` rework flag.

## How role detection works

The LCD is write-only and cannot identify the uplink board. USB port order is
also not stable. The flasher therefore writes the same tiny disposable probe to
all three blank boards. Each board announces its eFuse base MAC over USB and
tests two physical UART links with a fresh session nonce and CRC-protected
frames.

The only accepted graph is:

- one center with reciprocal peers on link A and link B;
- one link-A leaf connected to the center's link A (BLE scanner);
- one link-A leaf connected to the center's link B (Wi-Fi scanner).

Any missing, extra, duplicate, stale, nonreciprocal, or unexpected ESP32-S3
causes a fail-closed stop before production firmware is written.

## Flash and PASS gates

The tool tracks boards by immutable eFuse MAC across USB renumbering. Production
writes are sequential: BLE scanner, Wi-Fi scanner, then uplink. The bundle
builder reads the compiled partition table itself; OTA badge images are written
at `0x20000` with their OTA-data initializer at `0xF000`. It does not trust the
occasionally stale PlatformIO application offset.

A badge receives `PASS` only after:

- all three chips identify as ESP32-S3 with 8 MB flash and 8 MB PSRAM;
- the UART topology graph is complete and reciprocal;
- every declared flash region passes esptool write verification;
- every declared region passes a separate digest readback;
- all boards rebind to the same expected eFuse MACs;
- the uplink reports the exact production target, project, hardware, and version;
- both scanners report the expected MAC, role, profile, and version;
- safe/recovery/rollback modes are inactive; and
- uplink USB, scanner UART, BLE scanning, and Wi-Fi scanning are healthy.

Manufacturing evidence is fsync'd to:

```text
~/Documents/FoF Badge Factory/badge-factory.csv
~/Documents/FoF Badge Factory/badge-factory.jsonl
```

Both PASS and FAIL attempts are recorded. On a later run, a badge whose MACs
already have a PASS record is rejected unless the operator explicitly adds
`--allow-rework`. During a continuous session, all three boards from the prior
badge must be unplugged before the next cycle can start.

## Firmware bundles

`tools/badge_flasher/resources/badge-factory-flasher-embedded.zip` is the
offline fallback. On startup the tool checks final public GitHub releases for a
complete `badge-factory-flasher-v*.zip`. A remote bundle is used only if it is
strictly newer and every file, size, digest, target identity, layout, and
flasher-version requirement validates. Drafts, prereleases, mixed versions,
same-core variants, incomplete archives, and downgrades are ignored.

To build a bundle from current PlatformIO outputs:

```bash
python3 scripts/build_badge_factory_bundle.py \
  --output /tmp/badge-factory-flasher.zip
```

## Recovery behavior

`FAIL` means the badge is not approved even if one or more writes succeeded.
Leave it connected, correct the reported condition, and rerun the complete
cycle. The tool erases each target during production programming, so rerunning
is the normal recovery path for a factory badge.
