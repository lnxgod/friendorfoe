# CON CRUD Role and Activity Ticker Design

## Goal

Make the active CON CRUD role unmistakable without replacing or reducing the
badge's four detection lanes, and briefly explain qualified game effects as
they happen. Correct the observed cured-player regression where a revived
human falls from 50 to 49 without an attack.

## Release boundary

- This remains a private DEFCON 34 canary and is not pushed, tagged, merged,
  released, or installed into the public factory bundle.
- The frozen `.90` artifacts remain retained evidence. This patch becomes a
  new `.91` candidate with new exact hashes and physical acceptance.
- Both scanners keep their current receive-only game observer and normal
  BLE/Remote ID/Wi-Fi work. No scanner display, queue, radio, or UART behavior
  changes.
- Update, recovery, USB-flash, scanner-error, and terminal-death screens keep
  priority over game presentation.
- The strict uplink image and internal-RAM ceilings are not relaxed.

## Gameplay correction

The current lazy-time policy decrements every non-healer value once per
minute. That makes a cured human fall from its 50-point scar maximum to 49
without receiving an infected packet. Normal and cured humans will no longer
decay passively. An infected player's accumulated cure progress will continue
to fade by one point per minute, and a healer will continue to regenerate one
point per two seconds. Damage, healing, infection, cure, scar maxima, death,
and super-zombie rules remain unchanged.

## Presentation policy

Internal and over-the-air role identifiers remain `normal`, `infected`, and
`immune` for compatibility. Human-facing display copy uses:

| Presentation state | Display role |
| --- | --- |
| Normal human | `HUMAN` |
| Revived human | `CURED` |
| Infected | `INFECTED` |
| Immune seed | `HEALER` |
| Derived super infected | `SUPER ZOMBIE` |

The four 34–37 pixel detection lanes remain unchanged. When the game is
active and both scanner lanes are healthy, the 12-pixel bottom strip becomes
a game ticker:

- First line: centered 5x7 role text.
- Second line: a recent activity label for three seconds, otherwise the
  current `HEALTH n/max` or `CURE n/100` value.

If a scanner is unhealthy or the badge is in safe/recovery mode, the existing
scanner/USB health strip wins instead of the game ticker.

## Activity derivation

No new BLE messages, queues, persistence, locks, background timers, or
protocol fields are added. The display already snapshots authoritative game
state every frame. Canary-only display state compares the previous role and
shield with the current snapshot, then holds one of these labels for 12
existing display frames (about three seconds):

- `UNDER ATTACK` when human/healer health decreases;
- `HEALING` when health or cure progress increases;
- `CURE FADING` when infected cure progress decreases;
- `INFECTED` on a human-to-infected transition;
- `CURED` on an infected-to-human transition.

Initial activation produces no false activity. Terminal death continues to
use the existing full-screen death presentation.

## Verification

- Native tests prove cured humans do not decay and infected cure progress
  still decays. The clean uplink build and physical display check verify the
  exact role/activity copy, including `HEALER`.
- The complete native suite, updater tests, strict scanner/uplink artifact
  verifiers, firmware-version tests, and relevant host regressions must pass.
- The uplink build must remain within the existing 1,468,006-byte app-image
  and 212,992-byte internal-RAM ceilings. Scanner ceilings remain unchanged.
- Flash `.91` through each connected uplink, relay the exact scanner artifact
  to both UART lanes, re-seed A=`normal`, B=`infected`, C=`immune`, and prove
  all three complete graphs healthy.
- Physically run A+B with C inactive to verify `HUMAN`/`INFECTED`, infection,
  and `UNDER ATTACK`; then activate C to verify `HEALER`, `HEALING`, and
  `CURED`.
