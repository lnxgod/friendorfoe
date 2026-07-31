# CON CRUD Eight-Second Combat Cadence

## Goal

Stretch the physically validated CON CRUD round from roughly 30 seconds into
minutes without changing scanner operation, the BLE or UART protocol, game
roles, RSSI balance, task allocation, or the firmware update architecture.

The operator selected an exact change from one effect per peer per second to
one effect per peer every eight seconds. This is expected to make repeated
combat about eight times slower, or roughly four minutes for the observed
30-second scenario. The first qualified encounter remains responsive.

## Root cause

Passive infected rescue decay already removes only one point per minute. The
fast live game instead comes from the encounter limiter:

- a peer qualifies after three distinct packets within six seconds;
- each qualified peer currently applies a complete RSSI-scaled effect once per
  second;
- every nearby peer owns an independent cooldown, so multiple infected badges
  correctly stack.

The existing peer record already stores `emitted` and `last_emitted_ms`.
Changing the existing interval therefore needs no new state or allocation.

## Approved behavior

Change only `BADGE_CON_EFFECT_RATE_MS` from `1000U` to `8000U`.

- The three-packet quorum and six-second quorum window stay unchanged.
- The first qualified effect is still immediate.
- Later effects from that same peer are rate-limited until exactly 8,000 ms
  after its prior effect.
- Other peer identities remain independent and may qualify during that
  cooldown.
- The existing RSSI multipliers remain 1, 2, and 3.
- Regular infected, super-zombie, healer, scar, rescue, cure, and death values
  remain unchanged.
- Passive infected rescue decay remains one point per minute.

At close range, a regular infected badge can still infect a fresh 30-health
human on its first qualified hit. The slower cadence applies to every
subsequent attack, cure, or heal from that peer.

## Compatibility and memory boundary

The change must not alter:

- scanner scanning, forwarding, or radio profiles;
- BLE advertisements or advertising cadence;
- wire-frame, UART, NVS, RTC, USB, Android, or backend schemas;
- task, queue, timer, lock, peer-table, or structure allocation;
- updater, maintenance, rollback, or scanner-relay behavior;
- the normal four-lane display or its priority rules.

The private candidate advances directly from
`0.64.93-badge-defcon34` to `0.67.0-badge-defcon34`. Production, public
factory, GitHub release, and sensor-node firmware remain unchanged until
physical acceptance.

## Regression evidence

Native tests must prove:

1. the third strong packet still qualifies;
2. continuous distinct evidence is rate-limited through 7,999 ms after a
   qualified effect and qualifies at exactly 8,000 ms;
3. the cadence boundary remains correct across `uint32_t` timer wrap;
4. a second peer qualifies independently while the first peer cools down;
5. observer forwarding reflects the new cadence without changing packet
   validation;
6. existing RSSI, role, cure, reinfection, healer, death, protocol, display,
   scanner, and update tests remain green.

Fresh canary scanner and uplink builds must pass their strict artifact,
radio-isolation, internal-RAM, and app-image verifiers before any flash.

## Physical acceptance

Flash the three connected badges only through each uplink USB connection and
the internal scanner UARTs. Require all nine applications to report exact
`0.67.0-badge-defcon34`, normal recovery, idle OTA, correct scanner roles and
radio profiles, rollback clear, and zero crashes.

Activate the normal, healer, and infected seeds and verify:

- the first encounter still feels responsive;
- repeated effects from one nearby badge occur no faster than every eight
  seconds;
- two infected identities still stack independently;
- cure and reinfection still work;
- healer self-regeneration remains disabled;
- the game lasts minutes rather than seconds.

No public promotion occurs until that attended test passes.
