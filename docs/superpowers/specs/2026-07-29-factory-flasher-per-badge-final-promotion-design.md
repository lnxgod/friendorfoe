# Factory Flasher Per-Badge Menu and Final Firmware Promotion Design

## Goal

Make the macOS factory flasher ready for the DEF CON badge production run by:

1. prompting for the game role before every physical badge;
2. preserving the existing fail-closed three-board topology and verification flow;
3. recording the selected role with the badge's three MAC addresses and release evidence; and
4. replacing the factory flasher's offline firmware bundle with the exact
   `0.67.2-badge-defcon34` uplink and scanner application images already
   accepted on hardware.

This is a local, private promotion. It does not authorize a GitHub push,
release, tag, merge, or public web-flasher update.

## Accepted Firmware Authority

The promotion must copy, not rebuild, the two physically accepted application
images:

| Role | Accepted build output | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| Uplink | `esp32/uplink/.pio/build/uplink-s3-fof_badge-con-crud-canary/firmware.bin` | 1,468,464 | `78ef3b6dafe61e8e2fdc3fb28447372aaf76da38cd57ca0961828bbbdc08c434` |
| Scanner | `esp32/scanner/.pio/build/scanner-s3-combo-fof_badge-con-crud-canary/firmware.bin` | 1,216,800 | `2d0e84501baf3bc929eed03a0b9c1f0272ed66baa9b81dd4513d6dc3fa2c032b` |

Both images embed version `0.67.2-badge-defcon34`. The existing factory probe
remains version `1.0.0`; it is a disposable topology-discovery image and is
not part of the badge runtime.

Promotion adds a named accepted-release profile to
`scripts/build_badge_factory_bundle.py`. That profile selects the two canary
build directories and validates the exact version, size, and SHA-256 above
before creating an archive. The existing production profile remains available
for older/public builds, so the builder never changes meanings silently.

The bundle builder copies the accepted application images and their matching
PlatformIO bootloader, partition table, and initial OTA-data artifacts. It
continues deriving offsets from the compiled partition tables, producing a
deterministic ZIP, and reloading the completed archive through the existing
strict bundle validator.

The resulting archive replaces:

`tools/badge_flasher/resources/badge-factory-flasher-embedded.zip`

Loading that embedded archive must prove:

- bundle version `0.67.2-badge-defcon34`;
- exact accepted uplink and scanner application hashes;
- production runtime identities `fof_badge_uplink` and
  `fof_badge_scanner`;
- exact safe ESP32-S3 offsets and 8 MiB layouts;
- complete per-file size and SHA-256 agreement; and
- unchanged factory-probe identity `1.0.0`.

The promotion does not compile firmware, alter firmware source, change the
game balance, or change either accepted application byte.

## Per-Badge Operator Flow

The normal `flash-badges.command` flow retains the existing ANSI
GameChangers AI Badge Forge banner. Before each badge, it displays a compact
old-school BBS menu:

```text
+==================================================+
| GAMECHANGERS AI // SELECT NEXT BADGE             |
+==================================================+
| [1] HUMAN       // RED                           |
| [2] INFECTED    // GREEN                         |
| [3] HEALER      // HOT PINK                      |
+==================================================+
SELECT [1-3] >
```

ANSI mode uses the existing cyan, gold, green, red, and purple palette.
`--plain` renders identical words without escape sequences. The terminal is
not cleared between badges, so the operator retains visible PASS/FAIL history.

The user-facing choices map to firmware seed values:

| Menu | Operator label | Firmware/ledger seed |
| ---: | --- | --- |
| 1 | HUMAN | `normal` |
| 2 | INFECTED | `infected` |
| 3 | HEALER | `immune` |

After a valid selection, the flasher asks:

```text
ARM NEXT BADGE AS HEALER? [Y/N] >
```

`Y` locks the role only for that next badge. `N` returns to the role menu.
Blank, unknown, or malformed input has no default and is retried. `Q` at the
role menu, end-of-file, or Ctrl-C exits cleanly before probing, erasing, or
writing hardware.

The sequence repeats independently for each unit:

1. select and confirm a role;
2. plug one complete three-board badge into USB and press Enter;
3. discover and prove the reciprocal UART topology;
4. flash the BLE scanner, Wi-Fi scanner, and uplink;
5. provision the selected seed on the proven uplink;
6. reboot and verify the complete badge graph;
7. fsync the PASS or FAIL manufacturing record;
8. remove the badge and continue to the next role menu.

Bundle loading and validation happen once when the process starts. The role is
stored in a fresh local variable for each loop iteration and passed explicitly
to flashing, seed provisioning, verification, and failure recording. The
implementation must not mutate `argparse.Namespace.game_role`, because a
retry or failure must never inherit the previous badge's interactive role.

## Scripted and Noninteractive Operation

`--game-role normal|infected|immune` remains an intentional automation
override. When present, it skips the menu and uses that exact role.

The parser default changes from `normal` to no role. Therefore interactive
operation can never silently manufacture a human badge.

`--yes` is accepted only together with both `--once` and an explicit
`--game-role`. Any other `--yes` combination exits with command-line status
`2` before bundle selection, network access, device enumeration, ledger
writes, or hardware mutation.

The double-click wrapper uses the validated embedded archive in offline mode
for the private production run. Direct CLI users may still explicitly request
the existing GitHub bundle-selection behavior, but the double-click path
cannot replace the private accepted archive with a remote release.

## Manufacturing Evidence

The existing fsync-backed `badge-factory.jsonl` remains the authoritative
private JSON event log. For each successful badge it already contains:

- the selected canonical `game_seed`;
- uplink, BLE-scanner, and Wi-Fi-scanner MAC addresses;
- firmware version;
- validated bundle SHA-256;
- badge ID and opaque receipt;
- runtime verification evidence; and
- complete flash evidence for all three boards.

A failure after role confirmation records that same selected role. Operator
cancellation before hardware work writes neither a false PASS nor a false
FAIL. Existing CSV compatibility remains unchanged; JSONL is the role-aware
manufacturing source of truth.

## Failure and Recovery Policy

- Role input must complete before the first device scan or flash command.
- Bundle validation failure stops the process before the role menu can lead to
  hardware work.
- Any topology, flash, seed, reboot, health, version, scanner-role, rollback,
  or ledger failure suppresses PASS.
- A failed badge remains eligible only through the existing explicit rework
  path.
- The bundle is selected once, so a network or filesystem change cannot alter
  firmware halfway through the production batch.
- The accepted application hashes are checked both while building the bundle
  and while verifying the embedded resource.

## Verification

Implementation follows red-green test-driven development.

Host tests must prove:

- all three menu numbers map to the exact firmware seed;
- plain and ANSI menu output use the intended labels;
- blank and invalid selection retry without a default;
- confirmation `N` returns to selection and invalid confirmation retries;
- `Q`, EOF, and Ctrl-C exit before device or ledger work;
- two interactive iterations can choose different roles without leakage;
- the exact selected role reaches `run_one`, PASS output, and failure records;
- explicit `--game-role` bypasses the menu;
- unsafe `--yes` combinations fail before bundle or hardware work;
- one connect prompt exists per badge;
- the bundle is selected exactly once outside the badge loop;
- the accepted-release profile rejects the wrong version, size, or hash;
- the produced and embedded bundle load through the strict validator;
- the embedded uplink and scanner application hashes equal the accepted
  hashes above; and
- all existing factory flasher, bundle, verification, ledger, redaction, and
  firmware artifact tests remain green.

Release verification must also:

1. build the deterministic accepted-release bundle without compiling firmware;
2. load the embedded resource and print its exact version and application
   hashes;
3. run the full factory-tool unit suite;
4. run relevant firmware artifact/version verifiers against the frozen
   outputs;
5. confirm `git diff --check` and that no firmware source or accepted
   application binary changed; and
6. perform one physical end-to-end factory canary before authorizing the
   remaining production batch.

The physical canary is a packaging and manufacturing-flow test. It does not
reopen game-balance acceptance unless its final runtime evidence or operator
inspection exposes a genuine firmware defect.

## Superseded Requirement

This design supersedes only the session-wide role lock in
`2026-07-27-con-crud-property-game-and-factory-batch-design.md`. The current
owner decision requires a new role prompt for every badge. All topology,
verification, inventory, privacy, firmware, and no-publication constraints
from the earlier design remain in force.
