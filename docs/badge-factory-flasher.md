# FoF Badge Factory Flasher

The badge factory flasher programs one assembled three-board DEF CON badge at a
time on macOS. All three XIAO ESP32-S3 USB ports are connected together. The
operator selects the game role for each badge; the tool determines board roles
and ports itself.

## Quick start

1. Install PlatformIO Core once.
2. Double-click `flash-badges.command`, or run:

   ```bash
   ./flash-badges.command
   ```

3. Choose and confirm the next badge's game role:

   - `1` — `HUMAN` (`BLACK`) → `normal`
   - `2` — `INFECTED` → `infected`
   - `3` — `HEALER` → `immune`

   The menu has no default. Confirm the displayed selection with `Y` before
   the first hardware probe. Use `N` to return to the menu.
4. When prompted, connect exactly one complete badge—all three USB ports—to
   the Mac, then press Enter.
5. Do not disconnect the badge until the terminal
   prints `PASS`.
6. Unplug all three USB ports from the completed badge, then press Enter to
   confirm removal. The loop returns to Step 3: choose and confirm the next
   badge's role, and connect that badge only when prompted.

Once you press Enter to begin a badge, do not press Ctrl-C or unplug any USB
port. Wait for an explicit `PASS` or `FAIL` and follow the displayed rework
instruction.

The double-click wrapper always uses the embedded, validated offline bundle;
it does not contact GitHub. For an unattended automation or acceptance cycle,
the only supported form is an explicit one-shot role selection:

```bash
./flash-badges.command --yes --once --game-role ROLE
```

Replace `ROLE` with `normal`, `infected`, or `immune`. `--game-role` is the
explicit scripted override; without it, the interactive `1`/`2`/`3` menu is
required for every badge. For example:

```bash
./flash-badges.command --game-role normal
./flash-badges.command --game-role infected
./flash-badges.command --game-role immune
```

Every selected role is written explicitly; it never means “leave the previous
NVS value alone.”

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
- the uplink answers a fresh native-USB `FOF_PING`;
- the uplink accepts the exact selected
  `FOF_SET:game_seed=normal|infected|immune` command;
- a second fresh PONG/status after the seed acknowledgment binds the exact
  uplink hardware ID and records its integer expected-reboot generation;
- the uplink returns exact `FOF_REBOOT:OK`, the old handle is closed, and
  application-port discovery uses descriptor-bound reset-neutral opens
  without driving DTR/RTS; native USB may retain or re-enumerate its path;
- the fresh port session is accepted only when an exact PONG precedes status,
  the same uplink hardware ID is bound, the reboot reason is `usb_reboot`, and
  the reboot generation is the exact wrap-aware successor of the pre-reboot
  generation;
- the uplink reports the exact production target, project, hardware, and version;
- both scanners report the expected MAC, role, profile, and version;
- safe/recovery/rollback modes are inactive; and
- uplink USB, scanner UART, BLE scanning, and Wi-Fi scanning are healthy;
- fresh post-reboot status reports the selected seed and current state,
  `game_active:false`, and integer `game_shield:0`.

The scanner boards still receive one shared scanner artifact. Factory game
selection changes uplink state only; it does not create separate BLE/Wi-Fi
scanner binaries.

After the controlled uplink start, application discovery never uses esptool:
ROM probing would reset a candidate and destroy the boot-freshness evidence.
Wrong identities, stale buffered status before the exact PONG, stale
generations, disconnected ports, and ports that are still booting are closed
and retried within the bounded gate. A persistent native USB path is never
treated as reboot proof; the fresh descriptor-bound session and exact
successor generation provide that proof.
Transport churn during the seed acknowledgment, post-ack status, or reboot
receipt restarts the complete idempotent seed transaction after rediscovering
the exact MAC/version/target. Explicit firmware errors, wrong acknowledgment
keys, and identity changes still fail closed.

The public success line is intentionally opaque:

```text
PASS // GAME ROLE infected // RECEIPT rcpt_K7M2Q9W4
```

The receipt is generated from cryptographic host randomness only after every
fresh runtime gate passes. It is not derived from a MAC, badge ID, bundle hash,
time, role, or device state. Public output uses only the fixed `BADGE`,
`UPLINK`, `BLE-SCANNER`, and `WIFI-SCANNER` aliases. The private JSONL retains
the hardware evidence, selected role, receipt, and four safe game fields. The
append-only CSV keeps its existing header and row shape as the rework index.
Failed attempts store the selected role with a null receipt.

Manufacturing evidence is fsync'd to:

```text
~/Documents/FoF Badge Factory/badge-factory.csv
~/Documents/FoF Badge Factory/badge-factory.jsonl
```

Both PASS and FAIL attempts are recorded. On a later run, a badge whose MACs
already have a PASS record is rejected unless the operator explicitly adds
`--allow-rework`. During a continuous session, all three boards from the prior
badge must be unplugged before the next cycle can start.

### Reassigning an already-passed badge

An already-passed badge is not silently reseeded. When its complete prior PASS
matches the selected bundle, `IDENT` is followed by the visible `ALREADY
PASSED // REASSIGN ROLE ONLY? [Y/N]` prompt. `Y` is the exact prior-PASS
decision that permits role-only reassignment; `N` leaves the badge unchanged.
The role-only path proves the existing graph and completes boot, health, and
seed/reboot gates without `PROBE` or `FLASH`.

For a record that is not an exact complete current prior PASS, or to
intentionally rework a matching badge, use `--allow-rework`. This always takes
the full factory path: `PROBE`, complete UART `GRAPH`, and all three production
flashes before the new seed/reboot/health gates. A full rework prints `PASS`;
a role-only path prints `REASSIGNED`. Neither result permits the next badge
until all three USB connections have been removed.

## Firmware bundles

`tools/badge_flasher/resources/badge-factory-flasher-embedded.zip` is the
offline factory bundle. `flash-badges.command` selects it by default. The
underlying flasher can check final public GitHub releases only when it is
invoked without `--offline`; a remote bundle is used only if it is strictly
newer and every file, size, digest, target identity, layout, and
flasher-version requirement validates. Drafts, prereleases, mixed versions,
same-core variants, incomplete archives, and downgrades are ignored.

### CON CRUD canary boundary

The embedded bundle is the owner-approved local-only
`0.67.2-badge-defcon34` factory promotion. Its uplink and scanner applications
are pinned to the accepted hashes recorded in the canary acceptance ledger.
An embedded or released image that rejects `game_seed` fails the batch; the
flasher never silently downgrades or pretends the role was seeded.

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

After the ROM/application handoff, allow the badge three seconds to settle
before the seed/runtime gate. The seed acknowledgement is bounded to 60
seconds. While that bounded wait is active, the exact transient response
`FOF_ERROR:booting` is retried; any other explicit error remains fail-closed.
Only a seed-provisioning timeout gets one bounded non-writing
ROM/application-handoff recovery attempt. It does not write firmware, seed, or
erase during that attempt. A second timeout or a failed identity/health check
is `FAIL` and requires a complete rerun.
