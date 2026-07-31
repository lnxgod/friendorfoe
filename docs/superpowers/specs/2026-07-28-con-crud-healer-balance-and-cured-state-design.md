# CON CRUD Healer Balance and Cured-State Design

## Goal

Correct the live-game imbalance where a healer rapidly cures nearby infected
players while taking negligible damage, and remove the misleading permanent
`CURED` role label after a successful cure.

This remains a private DEFCON 34 canary. It must not be pushed, tagged,
released, merged, copied into the public factory bundle, or installed on
connected badges before the focused tests and firmware build pass.

## Root cause

The current receive-side game policy applies three advantages to a healer:

- a healer adds `10 × RSSI multiplier` cure points to each infected peer;
- each regular infected peer removes only `1 × RSSI multiplier` health from
  the healer;
- the healer regenerates one health point every two seconds without another
  healer nearby.

The per-peer encounter table already applies effects independently. Two
infected badges therefore already generate two separate attack streams, but
their combined damage is too small to compete with one healer.

The display selects the permanent `CURED` presentation whenever a normal
player has a nonzero scar level. The underlying normal-role state is intended
to remain infectable, but the permanent label makes later attacks and
reinfection appear broken.

## Approved balance

All balance remains integer, receive-side, bounded, and allocation-free:

- A healer applies `12 × RSSI multiplier` cure points to an infected player.
  This is the approved 1.2-times healer pressure relative to a regular
  infected attack unit of 10.
- Each regular infected peer removes `10 × RSSI multiplier` health from a
  healer.
- A super zombie continues to apply twice the regular infected damage.
- Multiple infected peers remain independent. Two regular infected peers
  therefore apply `10 × first RSSI multiplier + 10 × second RSSI multiplier`;
  at equal distance this is `20 × RSSI multiplier`, without adding
  group-count state.
- A healer receives no passive regeneration.
- A healer may still regain health from another healer at the existing
  `5 × RSSI multiplier` rate.
- Existing RSSI bands remain unchanged: `1` at `-53..-60 dBm`, `2` at
  `-46..-52 dBm`, and `3` at `-45 dBm` or stronger.

Normal-human damage, infected rescue decay, infection thresholds, scar
maximums, permanent death, dead spreading, super-zombie derivation, packet
quorum, and per-peer rate limiting remain unchanged.

## Cured-state presentation and reinfection

After a successful cure:

- the authoritative role remains `normal`;
- the scar level and reduced maximum health remain intact;
- the persistent role label becomes `HUMAN`;
- the existing `CURED` activity label remains visible for roughly three
  seconds after the infected-to-normal transition;
- later qualified infected encounters drain the reduced health and can infect
  the player again.

No new state or timer is introduced. The display already derives the
three-second activity from the role transition, so removing the permanent
cured presentation is sufficient.

## Compatibility and memory boundary

- No BLE payload, advertising interval, scanner firmware, UART frame, peer
  table, update path, factory seed, NVS record, RTC record, or Android command
  changes.
- No new task, queue, lock, allocation, framebuffer, or persistent field.
- The four detection lanes and their priority behavior remain unchanged.
- Update, recovery, scanner-health, and terminal-death displays continue to
  override game presentation.
- Existing strict uplink image and internal-RAM ceilings remain unchanged.

## Verification

Focused native regressions must prove:

1. A healer does not regenerate while isolated, including across timer wrap.
2. Another healer still restores healer health using the existing RSSI bands.
3. Regular and super zombies damage a healer at the approved rates.
4. Two regular zombie effects aggregate without new group state.
5. A healer cures infected players at the approved 1.2-times rate.
6. A cured player presents as `HUMAN` after the transient event.
7. A cured player retains its reduced maximum and becomes infected again when
   later damage reaches the threshold.
8. Existing game, protocol, scanner, update-priority, and display tests remain
   green.

After focused native tests pass, build the private canary uplink once under
the existing strict memory/image verifier and report the resulting margins.
Do not flash connected hardware without a separate attended instruction.
