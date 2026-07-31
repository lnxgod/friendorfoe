# CON CRUD BLE Game and Update-Safe Factory Provisioning Design

## Goal

Add a lightweight badge-to-badge CON CRUD game without changing the badge's
four privacy lanes, weakening normal scanner coverage, or making USB/UART
firmware updates less reliable.

The first implementation is a provisional `0.64.79-badge-defcon34` canary.
The existing production and factory firmware remain unpromoted until the
complete physical gate in this document passes.

## Non-negotiable boundaries

- The uplink is the only game advertiser on each badge.
- Neither scanner advertises or forwards the game over RF. Existing BLE active
  scan requests and bounded GATT investigation remain unchanged so the game
  does not weaken current scan-response-name or privacy coverage.
- The Wi-Fi-primary scanner continues its existing job unchanged.
- The game uses BLE only. It never uses Wi-Fi, an SSID, Remote ID, or a normal
  privacy detection as its badge-to-badge transport.
- Game traffic never enters or changes the four normal display lanes.
- USB Android control, uplink self-OTA, scanner staging, and both UART relays
  always take priority over the game.
- The existing 10-second OK+Menu reset/recovery chord always takes priority
  over every Easter-egg or game button action.
- No MAC address, stable hardware identifier, or game pseudonym is displayed
  to an operator or player.
- The canary has no alternate game transport. If initialized 8 MiB PSRAM or
  either uplink-BLE internal-memory gate cannot be proven, the game advertiser
  fails closed for the rest of that boot. It never falls back to Wi-Fi, an
  SSID, Remote ID, scanner advertising, or another RF path. Persisted game
  state is left unchanged, and the existing privacy scanners, USB control,
  buttons, `prepare_update`, and uplink-to-scanner UART updating remain
  available. Any such failure on supported R8 hardware blocks promotion.

## Activation and player-visible behavior

Factory provisioning seeds every uplink as exactly one of `normal`,
`infected`, or `immune`. The default is `normal`. Both scanner boards always
receive the same scanner image.

The seed exists before the game is active. The game activates only after the
badge accepts one of the existing Easter-egg launch paths and the player
dismisses the Easter presentation:

- SSID bytes exactly equal to `GameChangersAI-67`;
- Remote ID Basic ID exactly equal to `fof-michagain`, with the existing exact
  Hell, Michigan coordinates and geodetic altitude of 666 metres; or
- the existing one-shot physical-button Easter launch.

Activation is persisted. A normal reboot or an update reboot does not silently
remove the badge from the game. The existing Easter-egg 90-second radio
cooldown remains unchanged and does not limit game encounters.

The normal role keeps the player's selected theme and existing dashboard.
When game-active, it adds a compact bottom `SHIELD nn%` HUD without moving or
redefining the four lanes. Infected badges use a derived purple-and-green
chrome treatment. Immune badges use a derived pink treatment and display
`SHIELD 100%`. These are temporary render-time overrides built from existing
theme roles; they do not overwrite the user's palette or increase the theme
schema.

## Encounter and shield rules

The uplink advertises one non-connectable legacy BLE frame approximately once
per second while the game is active and no update-maintenance inhibit exists.

The BLE-primary scanner accepts a peer only after receiving three distinct
sequence values from the same peer and boot session within six seconds, all at
RSSI `-60 dBm` or stronger. Weak, duplicate, stale, malformed, wrong-round,
self-originated, or unauthenticated frames do not count and do not reach the
uplink UART.

The third packet both establishes the quorum and applies the first effect.
Each later distinct strong packet may apply at most one additional effect per
second:

- An immune peer adds five shield points to a normal badge, capped at 100.
- An immune peer adds ten cure points to an infected badge. At 100, the badge
  becomes normal with 50 shield.
- An infected peer removes ten shield points from a normal badge. A packet
  that reaches zero is absorbed; the next qualified infected packet infects
  the unshielded badge.
- An infected peer has no effect on an immune badge.
- A normal peer does not change role or shield.
- Repeated encounters between already infected peers are idempotent.

The same percentage meter represents shield for a normal badge and cure
progress for an infected badge. It decays lazily by one point per minute while
the game is active. Immune badges remain fixed at 100. No timer task is added.
The state machine applies elapsed decay when it processes an encounter or
produces a display snapshot, using wrap-safe unsigned time arithmetic.

The seed role, current role, activation flag, and coarse shield checkpoint are
stored in the existing `fof_config` NVS namespace. Role and activation changes
are committed immediately. Shield is committed only when it crosses a
10-point boundary. A versioned RTC snapshot preserves the exact shield value
across an expected software/update reboot. A cold power loss may restore the
last 10-point NVS checkpoint. Invalid version, checksum, enum, or range data
fails closed to the configured seed, an inactive game, and zero shield.

## BLE wire protocol

The game uses Service Data for the custom 128-bit UUID
`f0f34c01-6761-6d65-6368-616e67657273`. It does not claim another
organization's Bluetooth Company ID.

The legacy 31-byte advertisement contains the standard Flags structure plus
the UUID and this exact 10-byte service payload:

| Byte | Meaning |
| --- | --- |
| 0 | High nibble protocol version `1`; low two bits role (`0=normal`, `1=infected`, `2=immune`); remaining bits zero |
| 1 | Game round `34` (`0x22`) |
| 2-4 | Random nonzero 24-bit per-boot pseudonym |
| 5 | Random nonzero boot-session nonce |
| 6 | Sequence value, incremented once per one-second payload epoch |
| 7-9 | Low 24 bits of SipHash-2-4 over bytes 0-6 |

SipHash uses the public 16-byte ASCII canary key `FoF-DC34-CONCRUD`. This is
only an accidental-frame and casual-spoofing check; the open-source firmware
does not claim cheat-proof authentication.

The pseudonym, nonce, and sequence are never persisted or displayed. Before
advertising, the uplink sends its current identity to both scanner slots using
this exact canonical command:

```json
{"type":"crud_self","v":1,"round":34,"peer":"A1B2C3","session":"07"}
```

Only the BLE-primary scanner consumes it. That scanner drops matching radio
frames locally so self-echo cannot consume UART bandwidth or affect game
state. It acknowledges successful installation with this exact canonical
message:

```json
{"type":"crud_self_ack","v":1,"round":34,"peer":"A1B2C3","session":"07"}
```

The uplink does not begin game advertising until the BLE-primary scanner
returns the exact matching acknowledgment. The uplink also rejects its own
pseudonym and session on UART as defense in depth.

The scanner forwards qualified peer packets through a new allocation-free
line fast path:

```text
FOF_CRUD:1,22,2,A1B2C3,07,2A,-058
```

The fields are one decimal protocol-version digit, two uppercase hexadecimal
round digits, one decimal role digit, six uppercase pseudonym hex digits, two
uppercase session hex digits, two uppercase sequence hex digits, and a
negative four-character RSSI from `-127` through `-001`. The uplink parses
this line before cJSON and normal detection ingress. Noncanonical width, case,
range, delimiter, suffix, or extra bytes fail closed.

The scanner uses a fixed eight-peer encounter table and a bounded coalesced
pending queue. It performs UUID, payload, tag, RSSI, self-echo, duplicate,
quorum, and rate-limit checks before UART transmission. An expired entry is
replaced before a new peer is accepted; if all eight entries remain active,
the new peer is dropped. The callback never allocates or blocks.

The controller uses a one-second legacy advertising interval. Repeated
physical advertising events may carry the same sequence if the controller
retries an epoch; scanners treat those as duplicates. “Distinct sequence”
therefore means distinct payload epochs, which the uplink advances once per
second before updating advertising data.

## Uplink implementation and memory isolation

The existing release uplink intentionally compiles Bluetooth out. The game
therefore begins in separate canary build environments for the uplink and
scanner. Production build environments retain their current Bluetooth and
radio configuration until promotion is explicitly approved.

The uplink canary uses controller-only VHCI advertising: no NimBLE host,
Bluedroid host, GATT server, connections, scanning, security, or discoverable
device name. Controller configuration permits one advertising activity only.

The current badge-only detection queue contains 48 entries of 864 bytes and is
not consumed by the badge ingress path. The canary removes that queue only
after a regression test proves badge detections continue to use the existing
direct ingestion path. This is expected to reclaim approximately 41.5 KiB of
internal heap.

Game code adds no task, display frame buffer, heap-backed radio frame, cJSON
object, dynamic peer table, or per-frame allocation. Fixed-size static HCI
command/data arrays are permitted. VHCI commands run through one static
bounded state machine owned by an existing low-priority runtime loop. Display
rendering reuses the current framebuffer and DMA chunk.

The 40,960-byte ST7735 framebuffer is PSRAM-only in the canary. It must never
fall back to internal SRAM. A failed framebuffer allocation leaves the panel
headless while startup continues far enough to preserve buttons, USB control,
scanner UARTs, and firmware recovery; display health remains failed so that an
unhealthy OTA image cannot be promoted as a valid badge.

Immediately before the first game-controller initialization, the uplink must
prove initialized PSRAM, at least 8 MiB total PSRAM, at least 5 MiB free PSRAM,
at least 24 KiB free internal heap, and at least a 16 KiB largest internal
block. Admission failure is sticky until reboot and fails only the game VHCI
state machine. `CONFIG_SPIRAM_IGNORE_NOTFOUND=y` remains enabled so a PSRAM
fault cannot brick USB or UART update recovery.

## Update-preemption lifecycle

Trying to release Bluetooth memory and restart it in the same boot is not the
production strategy. ESP-IDF controller memory release is irreversible until
reset, and a reversible disable retains the controller's initialized memory.

Every host-initiated badge update begins with:

```text
FOF_CTL:{"cmd":"prepare_update","session":"0123456789ABCDEF"}
```

The session is exactly 16 uppercase hexadecimal characters. Unless the badge
is already in the same maintenance session, the first accepted request writes
a versioned RTC `PREPARING` marker before it latches sticky preemption. Marker
failure changes neither preemption nor radio state. From that point a
background owner, rather than the USB request handler, nonblockingly advances
the same exact session even if the response is dropped or the host
disconnects. A currently owned operation returns an exact retryable waiting or
busy receipt; the host retries the same session every 250 milliseconds under a
single 30-second preparation deadline. A different session fails with a
nonretryable conflict and cannot replace the marker.

The normal boot never accepts firmware bytes. Once no firmware operation owns
mutation and the durable coordinator is suspended, the background owner marks
the session reboot-armed. Exact radio quiescence is preferred, but an
unprovable controller shutdown, event overflow, or stalled display owner may
produce the internal `REBOOT_SAFE` outcome: restarting the chip is itself the
RF cutoff, and bytes remain prohibited until the radio-free maintenance boot.
Thus every accepted `PREPARING` session reaches a bounded expected reboot
without depending on the host remaining connected or on a successful VHCI
acknowledgment.

When the USB handler observes either exact quiescence or `REBOOT_SAFE`, it
emits and drains:

```text
FOF_UPDATE_MODE:{"ok":true,"phase":"rebooting","session":"0123456789ABCDEF","retryable":true,"reboot_required":true}
```

It then arms an expected software reboot and restarts. Failure to deliver the
receipt does not cancel that already-owned reboot. The background owner uses
the same required-response/drain and expected-reboot path when no host handler
is present. Repeating the same session is idempotent. This deterministic
preparation happens even when the game has not yet been activated.

If the host lost the reboot receipt and reconnects after the exact session is
already active, repeating `prepare_update` returns an `active` observation with
`ok:true`, `retryable:false`, and `reboot_required:false`; it never schedules a
second reboot.

The one canary-bootstrap exception is the exact trusted
`0.64.78-badge-defcon34` uplink, which has Bluetooth compiled out and cannot
understand `prepare_update`. New host software may use the already-hardened
direct inactive-partition OTA only for that exact `.78` uplink identity and
only when the target is `0.64.79-badge-defcon34`. After the `.79` uplink proves
fresh application health, the host must issue `prepare_update`, reconnect to
`.79` update maintenance, and only then stage either scanner. Unknown,
older, newer, same-version, or Bluetooth-capable source images never receive
this exception.

The next boot reports `recovery_mode:"update_maintenance"`. This is distinct
from the existing emergency `safe_usb` mode:

- BLE is never initialized, and its controller memory is released for the
  current boot.
- USB control, display status, scanner UART drivers, firmware staging, and the
  durable scanner-update coordinator remain available.
- Normal RF detections and game rendering are paused.

The host re-enumerates USB, privately proves the same uplink hardware identity,
and retries the interrupted phase from byte zero. Partial uplink writes remain
in the inactive OTA partition. The maintenance marker survives the committed
uplink OTA reboot so the new uplink can stage and relay the single scanner
image to both physical UART lanes before game radio can return.

Maintenance status exposes two separate reconciliation journals. The exact
seven-member `update_uplink` object reports idle, receiving, or the
RTC-persisted committed uplink manifest. The exact eight-member
`update_scanner` object reports phase, current maintenance session, target,
canonical SHA-256, size, slot mask, received bytes, and generation. A live
scanner parser is `receiving` with generation zero; a durable manifest is
`committed` with a nonzero generation and `received == size`; no manifest is
the all-zero/empty `idle` shape. Snapshot uncertainty is reported as an
invalid `unknown` phase so new host software fails before transmitting bytes.
Neither journal authorizes offset resume: an interrupted, noncommitted
transfer is aborted or allowed to time out and retransmitted from byte zero.

The same response exposes a compact six-member `update_campaign` receipt from
the durable scanner coordinator: generation, target and pending masks, worker
state, the two bounded readiness-probe counters, and exactly two per-slot
state/attempt entries. The host accepts a terminal campaign only when its
generation and target mask equal the committed `update_scanner` manifest,
`pending_mask == 0`, the worker is stopped, and every requested lane is
`converged` or `current`. `newer_skipped`, `refused`, and `failed` are safe
terminal failures: they never authorize a downgrade or `finish_update`, and
firmware returns to normal mode with the durable receipt intact. Full scanner
identity, immutable MAC continuity, role/profile, and radio health are proved
from normal-mode status immediately before preparation and again after
`finish_update`; they are intentionally not duplicated in the low-memory
maintenance response.

An `uplink_ota_begin` or scanner-stage begin received outside
update-maintenance mode accepts no binary bytes and returns the
protocol-native `update_maintenance_required` error. It does not invent a
session; new flasher software always issues `prepare_update` first. Older
software fails visibly and leaves the badge unchanged for a safe rerun.

Maintenance ends only after:

- the uplink image is valid and rollback is cleared;
- both requested scanner lanes are terminal;
- successful lanes prove a new boot identity, exact version/project/hardware,
  role/profile, UART ingress, radio health, and rollback clearance; and
- no firmware operation owns the mutation token.

The coordinator must attempt the Wi-Fi lane after any terminal BLE-lane
outcome, not only `converged` or `current`. A new fail-busy campaign-state API
atomically classifies operation ownership and durable coordinator state so the
game cannot resume in the current staging-to-worker scheduling gap.

On success, explicit abort, terminal failure, or bounded inactivity with no
active firmware operation, the badge clears maintenance, performs an expected
reboot, restores its persisted game state, and resumes advertising. Durable
scanner retry state remains available on the next boot. No updater or badge
retries forever.

## Factory role provisioning

The factory flasher adds:

```text
--game-role {normal,infected,immune}
```

The default is `normal`. The chosen role is shown before the operator confirms
the batch. Every erase-all production flash explicitly writes the selection;
the tool never inherits an old value.

After flashing all three production images, the tool sends the uplink:

```text
FOF_SET:game_seed=infected
```

The firmware accepts only the three exact lowercase values. The operation
atomically writes the seed, resets current role to the seed, clears activation
and shield, and acknowledges `FOF_OK:game_seed`. The tool then reboots the
uplink and repeats the complete runtime health gate.

Factory PASS additionally requires fresh status fields:

```json
{
  "game_seed": "infected",
  "game_state": "infected",
  "game_active": false,
  "game_shield": 0
}
```

The manufacturing ledger records the selected role with the existing private
hardware evidence. User-visible output replaces the MAC-derived badge label
with an opaque random receipt:

```text
PASS // GAME ROLE infected // RECEIPT rcpt_K7M2Q9W4
```

The receipt is stored only in the host ledger and is not a device identity.
The factory bundle continues to contain one uplink image and one scanner image.

## Failure handling

- Invalid or unauthenticated BLE input changes no state and emits no detection.
- Peer-table or UART pressure drops game evidence before normal detections,
  firmware traffic, or command traffic.
- Firmware activity inhibits new game transmissions before reserving update
  memory or UART ownership.
- Failure to establish the update-maintenance marker aborts before OTA bytes.
- Failure to deliver the reboot-required receipt still performs the expected
  reboot after a bounded drain attempt; the host reconnect path is idempotent.
- A host disconnect during upload follows the existing transactional abort,
  then returns to normal mode after bounded inactivity.
- A scanner relay interruption uses the existing durable coordinator recovery
  and retry budget.
- Factory role acknowledgment without post-reboot status proof is not PASS.
- Invalid NVS game data never grants immunity.

## Canary verification and promotion gate

All production changes follow red-green-refactor test cycles.

Automated coverage includes:

- game state transitions, shield clamp/decay, cure, infection, immunity,
  activation, wrap-safe timing, persistence validation, and invalid-state
  fallback;
- exact BLE payload construction/parsing, every field boundary, tag failure,
  duplicate and stale sequences, self-echo, RSSI `-60/-61`, quorum at
  packet 2/3, six-second expiry, sequence wrap, peer-table replacement, and
  allocation-free scanner forwarding;
- exact UART grammar and rejection of whitespace, lowercase hex, extra fields,
  duplicate delimiters, embedded NUL, suffix bytes, and out-of-range RSSI;
- proof that valid game frames never become Remote ID, generic BLE privacy,
  or four-lane detections;
- update preparation, receipt drain, expected reboot, maintenance boot,
  same-device host rebind, from-zero retry, inactive-partition interruption,
  uplink commit, both scanner outcomes, coordinator dependency failure, host
  disappearance, and return to game;
- factory CLI choices/default, seed acknowledgment, mandatory rebooted proof,
  role/receipt ledger fields, and user-visible identifier redaction;
- dual-button reset priority while Easter or game rendering is visible.

Before any canary flash, both canary targets and both unchanged production
targets must build, and all native, verifier, factory-flasher, backend, Android,
and existing firmware tests must remain green.

The first physical gate uses two complete badges and requires:

1. no self-encounter and no state change below `-60 dBm`;
2. infection, shield drain, immunity charging, and cure at close range;
3. continued DJI/Remote ID/privacy scanning and unchanged four-lane behavior;
4. Android USB status, theme, palette, and display controls during game mode;
5. an uplink update that automatically preempts the game, re-enumerates,
   retries, commits, stages one scanner image, updates both UART lanes, proves
   both scanners healthy, and resumes the prior game state;
6. successful recovery from one unplug during uplink upload and one reboot
   during scanner relay;
7. the 10-second OK+Menu path from the Easter screen, game screen, dashboard,
   and USB-attached state; and
8. a 30-minute simultaneous game/scanning/USB soak with no crash, watchdog,
   detection flood, or monotonic heap decline.

Runtime acceptance requires at least 24 KiB free internal heap, a 16 KiB
largest free internal block, 12 KiB minimum-ever free internal heap, and all
existing task-stack health floors throughout the normal-mode soak. Update mode
must meet or exceed the current `.78` updater's pre-transfer heap and largest
block because BLE is fully released for that boot.

The same runtime evidence must report exactly 8,388,608 bytes of PSRAM, at
least 5 MiB free PSRAM at idle, and a positive largest PSRAM block no larger
than free PSRAM. The game may advertise only after both PSRAM and internal
memory admission pass.

The canary is not copied into the embedded factory bundle, tagged, pushed,
published, or called release-safe until the physical gate passes three
consecutive full update cycles on both test badges.
