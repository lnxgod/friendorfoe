# CON CRUD Property Game and Factory Batch Design

## Status and scope

This design supersedes the activation persistence, encounter rules, visual
states, and factory-selection sections of
`2026-07-25-con-crud-ble-game-design.md`. The proven controller-only BLE,
scanner observation, USB maintenance, and uplink-to-scanner UART update
architecture remain in place.

The current accepted baseline is private firmware `0.64.89-badge-defcon34`.
The enhanced game remains private until it passes the automated and physical
gates below. It must not be pushed, tagged, released, merged, or copied into
the production factory bundle before acceptance.

The immediate goal is a balanced three-role game that can be tested on three
complete badges and then provisioned across approximately 45 badges. The game
must remain an overlay on the normal badge. Privacy and drone detection,
the four display lanes, USB control, buttons, and firmware updating remain the
badge's primary functions.

## Non-negotiable safety boundary

- The uplink remains the only game advertiser.
- The BLE-primary scanner listens for game frames while retaining its current
  normal scanning work. The Wi-Fi-primary scanner remains unchanged except for
  shared fixed-width game-frame codec compatibility.
- Scanner boards never advertise the game.
- No new FreeRTOS task, dynamic allocation, detection queue, peer table,
  bitmap, framebuffer, packet length, advertising frequency, or UART line
  length is introduced.
- The existing eight-peer encounter table, three-packet quorum, update
  preemption, USB transport, scanner coordinator, and both UART relay lanes
  retain priority.
- Firmware update, safe/recovery mode, scanner failure, and other critical
  indicators override game LED colors and game screens when necessary.
- A dead or game-active badge continues normal scanning and remains
  controllable over USB.
- Existing strict scanner and uplink RAM and app-image gates may not be
  increased to make the feature pass.

## Persistent seed and reboot behavior

Factory provisioning assigns exactly one immutable seed to the uplink:

- `normal` — human;
- `infected` — infected seed; or
- `immune` — immune/healer seed.

Of the game fields, only the seed persists in NVS. Current health, shield,
cure progress, scar level, active state, infection, super-zombie state, and
death are live-run state and reset on every reboot, including firmware-update
reboots. Existing legacy live-game NVS fields are not allowed to restore a
run.

After reboot, the normal badge interface returns and the game is inactive.
The player must trigger the existing Easter egg again to begin another run.
Activation continues to use the already-approved SSID, exact Hell, Michigan
Remote ID, and physical-button paths. Dismissing the Easter presentation
starts the game from the persistent seed.

The start state is:

| Seed | Live role | Current value | Maximum |
| --- | --- | ---: | ---: |
| Human | Human | 30 health | 100 health |
| Infected | Infected | 0 cure progress | 100 cure progress |
| Immune/healer | Immune | 100 shield | 100 shield |

## Qualified encounters and distance

The accepted BLE wire frame, self-echo suppression, and allocation-free
scanner-to-uplink forwarding remain unchanged in size and cadence.

A peer qualifies only after the BLE-primary scanner sees three distinct
sequence values from the same peer and boot session within six seconds.
Every packet must be at least `-60 dBm`. Weak, duplicate, stale, malformed,
wrong-round, self-originated, or unauthenticated frames do not count.

After quorum, each peer can produce at most one effect per second. The existing
per-peer limiter means several nearby badges contribute independent effects
without a new crowd counter.

The receiving uplink derives a distance multiplier from the RSSI already
present in the packet:

| RSSI | Multiplier |
| --- | ---: |
| `-45 dBm` or stronger | 3 |
| `-46` through `-52 dBm` | 2 |
| `-53` through `-60 dBm` | 1 |
| weaker than `-60 dBm` | ignored |

No distance history, smoothing buffer, floating-point distance calculation,
or GPS state is added.

## Health, healing, infection, and immune balance

### Human receiving an encounter

- A regular infected peer removes `10 × distance_multiplier` health.
- A super zombie removes `20 × distance_multiplier` health.
- An immune peer restores `5 × distance_multiplier` health, capped at the
  human's scar-limited maximum.
- A normal peer has no effect.
- Damage that reaches or crosses zero immediately changes the human to
  infected with zero cure progress.

The existing lazy one-point-per-minute decay remains for human health and
infected cure progress. It adds no timer task.

### Infected receiving an encounter

- An immune peer adds `10 × distance_multiplier` cure progress.
- Normal, infected, and super-zombie peers have no effect.
- At 100 cure progress, the cure either returns the player to human at the
  next scar-limited maximum or causes permanent death as defined below.

### Immune/healer receiving an encounter

- Immune shield naturally regenerates by one point every two seconds, capped
  at 100.
- Regeneration is lazy: elapsed whole two-second ticks are applied during an
  existing snapshot or encounter call. No timer or task is added.
- A regular infected peer removes `1 × distance_multiplier` shield.
- A super zombie removes `2 × distance_multiplier` shield.
- Another immune peer restores `5 × distance_multiplier` shield, capped at
  100.
- Each qualified infected peer applies independently. Three close regular
  infected peers therefore remove approximately nine shield points per
  second before natural regeneration.
- Damage that reaches or crosses zero immediately kills the immune player and
  converts that badge to a super zombie until reboot.

At a shared timestamp, lazy regeneration is applied once before the encounter
effect. Further peer effects at the same timestamp do not earn extra
regeneration.

## Scar progression and permanent death

One byte of live game state records a scar level. The maximum human health is
derived from a constant table rather than stored independently:

`100 → 50 → 25 → 12 → 6 → 3 → 1`

Each successful cure advances one level. Cures that produce maximum health
`50`, `25`, `12`, `6`, or `3` return the player to human with current health
equal to that maximum. A cure that would reduce maximum health to `1` does not
restore the player. It enters permanent death for the rest of the run.

Dead players:

- ignore healing and cure effects;
- advertise as infected so the zombie side continues to spread;
- retain normal scanning, USB control, button reset, and update behavior; and
- return to their persistent factory seed only after reboot.

A dead normal or infected-seed player is a regular zombie. An immune-seed
player killed at zero shield is a super zombie. Super status is therefore
derived from the persistent seed and live death state; it does not require a
second persistent field.

## Super-zombie protocol extension

Super zombies keep the existing advertisement interval. Faster advertising is
rejected because it increases RF airtime and scanner traffic and does not
reliably double effects through the existing one-second limiter.

Instead, bit 2 of the existing authenticated version/role byte becomes a
`super` flag:

- the service payload remains 10 bytes;
- the legacy advertisement remains 31 bytes;
- the flag is covered by the existing SipHash tag;
- the flag is valid only with the infected role; and
- every other reserved-bit or role/flag combination fails closed.

The fixed UART role nibble carries `role | 0x4`. Regular infected remains
`1`; super infected is `5`. The UART line and buffer sizes remain unchanged.
The scanner parser forwards the flag, and the receiving uplink applies the
two-times damage rule.

New firmware accepts both regular version-1 frames and the authenticated
super flag. Older scanners reject the previously reserved bit rather than
misinterpreting it. A complete three-board badge must therefore converge to
the accepted firmware before it can receive factory PASS.

## Screen and LED states

Rendering uses the existing framebuffer, drawing primitives, fonts, and
GameChangersAI logo. No new bitmap or animation buffer is permitted.

- **Human:** the normal interface remains, with a green treatment and a
  current/max health HUD.
- **Cured human:** cyan treatment plus a persistent `CURED` and current
  maximum indicator makes the reduced ceiling obvious.
- **Infected:** a toxic purple/green canvas, procedural contamination marks,
  an unmistakable `INFECTED` label, and a red LED.
- **Immune/healer:** a hot-pink/inverted treatment, a prominent
  `IMMUNE / HEALER` shield meter, and a blue LED.
- **Super zombie:** a more aggressive infected treatment, an explicit
  `SUPER ZOMBIE ×2` label, and a rapid red/purple LED pattern.
- **Dead:** the normal four-lane information is replaced by
  `YOU DIED OF CON CRUD`, the GameChangersAI logo, `REBOOT TO FIX`, and a
  bounded procedural glitch treatment. A dead immune conversion also shows
  `SUPER ZOMBIE ×2`.

The existing WS2812 task and GPIO 48 driver are reused. Game behavior adds
patterns or a packed color override to that task; it does not create another
task. Update, recovery, scanner-loss, and hardware-error patterns override
game colors.

Full independent hex control of chrome, backgrounds, every game state, and LED
colors is explicitly deferred to a separate Theme v2 project. This build
retains the current Android arbitrary-hex/RGB565 editor for six signal accents
and the current named chrome/background presets. It does not claim that the
entire interface is arbitrarily color-configurable.

## Memory and storage policy

- Scar level is one byte of live state; maximum health is derived.
- Super-zombie state is derived from seed plus death.
- RSSI reuses the existing signed byte in `badge_con_packet_t`.
- Multiple attackers reuse the existing eight-peer encounter table.
- Natural regeneration reuses the existing wrap-safe lazy time bookkeeping.
- Game state produces no encounter-time NVS writes.
- Protocol and UART buffers do not grow.
- Procedural display effects reuse the existing framebuffer.
- The existing LED task and RMT device are reused.

The unchanged automated ceilings are:

| Target | Internal RAM | App image |
| --- | ---: | ---: |
| Scanner | 180,224 bytes | 1,363,148 bytes |
| Uplink | 212,992 bytes | 1,468,006 bytes |

The `.89` uplink baseline has only 3,892 bytes of internal-RAM headroom and
710 bytes of verifier app-image headroom. Implementation must simplify or
reuse existing code as needed; it must not raise either ceiling. The scanner
baseline has 21,620 bytes of internal-RAM headroom.

## Factory flasher batch selection

The normal double-click/interactive factory flow asks exactly once at startup:

```text
WHAT TYPE OF BADGE BATCH ARE WE FLASHING?
  1 — HUMAN
  2 — INFECTED
  3 — IMMUNE / HEALER
```

There is no silent interactive default. The operator confirms the selection
before any port is probed or erased. The role remains locked and prominently
displayed for every badge processed until that flasher process exits.

The existing `--game-role` option remains only for automated tests and
intentional scripted operation. A person using `flash-badges.command` does
not need to remember or enter command-line options. A noninteractive process
without an explicit role fails before hardware mutation.

Topology discovery remains fail-closed and unchanged: the disposable probe
must still prove exactly one uplink center, one BLE-primary leaf, and one
Wi-Fi-primary leaf. Both scanners receive the same scanner artifact. Only the
proven uplink receives the selected persistent game seed.

Factory PASS continues to require exact post-reboot seed, inactive game,
zero live value, three-board identity, firmware version, scanner roles,
radios, USB, UART, rollback, and health proof.

## Private manufacturing inventory

The append-only, fsync-backed `badge-factory.jsonl` remains the authoritative
event history. Each PASS and FAIL gains a batch identifier and selected batch
type.

After each successful PASS record, the flasher atomically rebuilds:

```text
~/Documents/FoF Badge Factory/badge-inventory.json
```

The human-readable private inventory uses a versioned schema. Each physical
badge entry contains:

- opaque factory receipt and badge ID;
- batch ID and selected badge type;
- uplink MAC;
- BLE-scanner MAC;
- Wi-Fi-scanner MAC;
- firmware version and validated bundle SHA-256;
- current successful assignment;
- complete rework/history entries; and
- `color: null` for later manual annotation.

The inventory contains raw MAC addresses because it is a private manufacturing
record. Public terminal output retains the current fixed aliases and opaque
receipt and does not reveal MACs. Color is metadata only and never changes
firmware, the game seed, or factory PASS.

Inventory updates use a temporary file in the same directory, file fsync,
atomic rename, and directory fsync. A ledger or inventory failure prevents the
tool from printing PASS. Rework with a changed type requires the existing
explicit rework authorization and appends history rather than overwriting it.

## Automated verification

Native tests must cover:

- exact RSSI tier boundaries at `-45/-46/-52/-53/-60/-61`;
- human start health, healing cap, infection, and immediate zero crossing;
- immune lazy regeneration, cap, wrap-safe time, and encounter ordering;
- independent regular and super-zombie damage;
- sequential effects from several peer identities without a new crowd table;
- scar maxima `100/50/25/12/6/3/1`;
- cure, reduced maximums, permanent death, and ignored post-death healing;
- immune death and derived super-zombie state;
- reset of every live field on all reboot paths while retaining the seed;
- authenticated super flag construction, parsing, tag rejection, and invalid
  role/flag combinations;
- unchanged advertisement, service-payload, UART line, wire, and buffer sizes;
- unchanged self-echo, quorum, duplicate, expiry, table-capacity, and rate
  limits;
- display-state selection and safety/update override priority;
- LED-state selection without a second task;
- startup batch prompt, invalid/EOF handling, session lock, and automation
  override;
- JSONL batch history, three-MAC inventory grouping, rework history, nullable
  color, atomic-write failure, and PASS suppression; and
- all existing update, USB, scanner, flasher, Android, and redaction tests.

Both canary firmware targets and unchanged production targets must build.
Strict verifiers must pass without changing their budgets or radio invariants.

## Three-badge physical acceptance

The three connected uplinks are:

- Badge A, current accepted `.89`;
- Badge B, current accepted `.89`; and
- Badge C, healthy legacy `.76`, which must be upgraded as a complete
  three-board graph before game testing.

Physical acceptance uses one exact candidate artifact set and requires:

1. all three uplinks and all six scanner leaves report the exact candidate,
   immutable identities, correct fixed roles, live radios, clear rollback,
   zero crash loop, USB health, and UART health;
2. a normal seed starts at 30/100 and is infected at the expected close,
   medium, and far qualified encounter counts;
3. an immune badge heals a human and another immune badge with distance-scaled
   effects;
4. an immune badge regenerates at one point per two seconds without monotonic
   heap loss;
5. a regular infected badge kills an immune badge, producing the exact death
   screen and authenticated super advertisement;
6. the converted super zombie applies exactly twice regular attack damage;
7. two or more infected identities drain one immune badge independently;
8. cure cycles enforce the scar table and the final attempted cure produces
   the locked CON CRUD death screen;
9. human, cured, infected, immune, super-zombie, and dead displays and LEDs are
   visually distinct, while update/error indicators retain priority;
10. reboot clears active state, health, shield, cure, scars, super status, and
    death, preserves the selected factory seed, and requires Easter activation
    again;
11. normal Remote ID, DJI, privacy, Wi-Fi, and BLE scanner counters continue
    advancing without a game-frame detection flood;
12. Android/Mac USB status, navigation, existing theme/palette commands, and
    reconnect remain healthy;
13. one uplink stages the exact scanner candidate and updates both scanner
    lanes through the existing UART coordinator, followed by exact version,
    identity, role, radio, rollback, heap, stack, and crash proof; and
14. a bounded simultaneous game/scanning/USB soak shows no crash, watchdog,
    UART overflow, queue regression, or monotonic internal-heap decline.

The factory flasher is then tested with a local validated candidate bundle:

1. launch without arguments and choose each of the three batch types in
   separate bounded runs;
2. prove the choice occurs before probing or erasing;
3. flash and verify one complete badge;
4. inspect the exact seed after reboot;
5. verify JSONL and atomic inventory contain all three MACs, type, version,
   bundle hash, receipt, batch, history, and null color; and
6. confirm a failed seed, health gate, or inventory write cannot print PASS.

## Promotion and rollout

Passing unit tests or a successful build is not sufficient for promotion.
The exact candidate must pass the complete three-badge physical matrix.

After acceptance:

1. retain hashes and memory/health evidence;
2. locally commit the accepted firmware and tools;
3. update the local factory-bundle artifacts to those exact hashes;
4. run one end-to-end factory-flasher canary and inspect the private inventory;
5. begin a small property-test batch before the remaining badges; and
6. preserve the ability to stop and rework a batch if live balance or RF
   coexistence differs from the bench.

Nothing in this design authorizes a public GitHub push or release before
DEF CON.
