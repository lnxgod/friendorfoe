# CON CRUD Infection Rescue Countdown Design

## Status and scope

This private canary design supersedes only the infected-meter, infection
transition, and infected-death rules in
`2026-07-27-con-crud-property-game-and-factory-batch-design.md`. The existing
BLE protocol, one-uplink advertiser, scanner quorum and rate limiting, normal
human health, healer shield, scar ladder, display states, updater priority,
factory seed, and reboot behavior remain unchanged.

The feature must not be pushed, tagged, released, merged, or copied into the
public factory bundle before physical acceptance.

## Approved player rules

The infected meter becomes a rescue countdown:

- A human whose health reaches zero becomes infected with `45` rescue points.
- A factory-infected badge also starts an activated run with `45` rescue
  points.
- An infected badge loses one rescue point per elapsed whole minute even when
  no peers are nearby. An isolated newly infected player therefore has
  exactly 45 minutes to find a healer.
- Each qualified regular infected encounter removes
  `1 × distance_multiplier` rescue points.
- Each qualified super-zombie encounter removes
  `2 × distance_multiplier` rescue points.
- Existing RSSI multipliers remain `1` at `-53..-60 dBm`, `2` at
  `-46..-52 dBm`, and `3` at `-45 dBm` or stronger.
- Multiple infected peers continue to apply independently through the
  existing eight-peer table, three-packet quorum, and one-effect-per-peer-per-
  second limiter.
- Each qualified healer encounter adds the existing
  `10 × distance_multiplier` rescue points.
- Reaching 100 performs the existing cure transition and advances the
  existing scar ladder.
- Reaching zero before a cure causes permanent death for the rest of the run.
- A cured human does not decay passively. Only qualified infected encounters
  reduce healthy or cured human health.

This intentionally creates two races. A lone infected player has a slow
45-minute rescue window, while staying near infected players accelerates
death. Moving near a healer reverses the countdown toward a cure.

## Death and spreading

A dead badge remains game-active and advertises the infected role until
reboot. It ignores all later healing, damage, and cure effects.

- Dead human and infected-seed badges spread as regular infected badges.
- A healer killed at zero shield remains the existing two-times super zombie.
- Death does not stop normal privacy scanning, USB control, button reset, or
  firmware-update preemption.
- Reboot clears live death and returns the badge to its persistent factory
  seed with the game inactive.

No advertising-frequency change is made. Regular versus two-times damage
continues to use the existing authenticated `super` flag and receive-side
effect multiplier.

## Minimal state-machine implementation

The implementation reuses `badge_con_game_state_t.shield`; it adds no field:

- healthy role: current health;
- infected role: remaining rescue points;
- healer role: current shield.

On human-to-infected transition, set the role to infected, set the shared
meter to 45, clear the lazy-decay epoch to the encounter time, and emit the
existing infected effect. On infected activation, initialize the meter to 45.

Lazy time remains wrap-safe and task-free. It changes only infected state,
subtracting one point for each complete elapsed minute. If subtraction reaches
zero, it marks the state dead during the same snapshot or encounter call.

An infected peer encountering an infected recipient now subtracts the
distance multiplier, doubled only for a valid super peer. A healer continues
to add cure progress. Every path that reaches zero marks the recipient dead
before returning. Dead state remains infected with a zero meter.

The existing scar-ladder terminal cure remains valid. A cure that would
produce maximum health one still enters the same permanent-death state.

## Compatibility and memory boundary

- No BLE advertisement byte, UART line, parser, peer table, task, queue,
  framebuffer, NVS record, RTC record, or firmware-update command changes.
- No scanner firmware behavior changes are required.
- No new dynamic allocation or persistent write is introduced.
- Factory seed storage remains seed-only; the countdown resets on reboot.
- Existing critical indicators and update/recovery modes continue to override
  the game display and radio.
- The current strict scanner and uplink RAM/image ceilings remain unchanged.

## Verification

Native regression tests must prove:

1. Human infection begins at exactly 45.
2. Factory-infected activation begins at exactly 45.
3. Forty-four minutes leaves one point; 45 minutes causes permanent death.
4. Unsigned `uint32_t` time wrap preserves the same decay.
5. Regular and super infected proximity damage use the existing RSSI
   multipliers and mark death exactly at zero.
6. Healer progress can outrun decay and cure before zero.
7. A cured human at 49 remains 49 across elapsed time when isolated.
8. Dead normal/infected-seed snapshots advertise regular infected, while a
   dead healer snapshot advertises super infected.
9. Dead state ignores all later peer effects.
10. Existing normal-human, healer, scar-ladder, codec, encounter, update,
    memory, and strict image tests remain green.

The private canary must then be built under the unchanged strict ceilings.
Physical acceptance requires one normal, one infected, and one healer badge:
activate the run, infect a human, observe countdown and proximity
acceleration, cure it before zero, kill a player at zero, prove the death
screen, prove the dead badge still infects another badge, and prove an update
still preempts the game safely.
