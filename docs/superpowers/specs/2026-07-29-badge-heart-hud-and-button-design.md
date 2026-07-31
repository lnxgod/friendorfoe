# Badge Heart HUD and Button Design

## Goal

Make CON CRUD health and activity immediately readable, and make the existing
detail view and Easter egg easier to reach, without changing scanning, game
combat, BLE protocol, task allocation, USB/UART flashing, update recovery, or
the two-button reset path.

## Display behavior

The existing live-game bottom strip keeps its role row (`HUMAN`, `INFECTED`,
`SUPER ZOMBIE`, or `HEALER`). A cured player remains `HUMAN`; the existing
temporary cured activity becomes a pink strip pulse rather than a persistent
role. The cramped second row no longer renders `HEALTH`, `CURE`, or transient
activity words.

Instead, the second row always renders one small filled heart followed by the
current health and current maximum as numbers, for example `♥ 49/50`.

- Human and cured-human heart/readout: red (`RGB565 0xF800`).
- Infected and super-zombie heart/readout: green (`RGB565 0x07E0`).
- Healer heart/readout: bright pink (`RGB565 0xF81F`).
- Inactive and permanent-death screens retain their existing presentations.
- Scanner-failure and safe-USB messages retain priority over the game HUD.

The heart is a fixed 7-by-5 pixel framebuffer primitive. It uses no font
glyph, image asset, heap allocation, persistent buffer, or new display task.

The panel backlight is hardwired to 3.3 V and cannot change color in software.
For the approved minimal substitute, an existing transient game activity
temporarily pulses only the bottom strip background:

- attack or cure-fading activity uses an attack tint;
- healing or cured activity uses a healer/pink tint;
- infection activity uses an infected/green tint.

The heart and numeric readout remain visible throughout the pulse. The pulse
uses the already-existing activity state, expiration window, render cadence,
and millisecond clock. It adds no timer, task, queue, protocol field, or game
state. When activity expires, the strip returns to its normal themed
background.

## Button behavior

The physical OK/Triforce button gains two actions while the Easter
presentation is inactive:

- A quick tap performs the exact existing detail action currently reached by
  a Menu double-tap.
- A continuous hold reaching 6,000 milliseconds triggers the existing
  button-launched Easter egg exactly once.
- A release between the existing long-press boundary and 6,000 milliseconds
  performs no action, preventing an abandoned Easter hold from unexpectedly
  opening the detail view.

The six-second action is independent of the existing Menu long-press boundary;
the global Menu gesture timings are not changed. Once the Easter presentation
is active, its existing advance/dismiss button behavior remains authoritative.
Existing SSID and exact Remote ID Easter triggers and their cooldown remain
unchanged.

The existing both-buttons-for-10-seconds reboot chord has highest priority.
When both buttons form a chord, one-button detail and Easter dispatch are
suppressed, and the chord remains one-shot until both buttons are released.
Menu single-tap navigation, Menu double-tap detail, Menu long-press
investigation, and all other button behavior remain unchanged.

## Implementation boundaries

- Add a pure presentation mapping for live CON CRUD HUD role, color, current
  value, maximum value, and visibility; make the renderer consume that mapping.
- Add a small allocation-free heart drawing helper local to the display
  renderer.
- Add a dedicated pure OK-button hold policy rather than changing the shared
  Menu gesture thresholds.
- Reuse the existing detail action and Easter trigger entry points.
- Apply the new OK mapping only to `FOF_DC34_GAME_CANARY`; keep the non-canary
  immediate OK Easter behavior unchanged.
- Do not change scanner firmware behavior, BLE advertisement or UART formats,
  CON CRUD health math or cadence, display framebuffer allocation, Android
  commands, firmware partitions, updater architecture, or factory promotion.

## Verification

Native regression tests must cover:

- human and cured-human red HUD mapping;
- infected and super-zombie green HUD mapping;
- healer bright-pink HUD mapping;
- inactive and dead HUD suppression;
- heart geometry remaining within the 160-pixel display and bottom strip;
- OK quick-tap detail dispatch;
- no action for an incomplete long hold;
- one Easter dispatch at the exact 6,000-millisecond boundary;
- no duplicate dispatch on release after the Easter hold;
- both-button chord suppression and the unchanged exact 10-second reset;
- unchanged Menu single, double, and long dispatch.

Then run the existing native, version, flasher, USB-hardening, strict artifact,
scanner build, and uplink build gates. Only after those pass may the private
canary be flashed through the existing uplink path for physical confirmation
of role colors, numeric readability, activity pulse, tap-to-detail,
six-second Easter launch, and ten-second reboot.

The private `0.67.1` uplink app gate has one exact alignment exception:
1,468,016 bytes, about 70.001% of the 2 MiB OTA slot. This is the first
16-byte boundary above the former 70% threshold, a 10-byte increase that
still leaves 629,136 bytes of OTA-slot headroom. The exception is not
open-ended, does not allow another aligned block, and does not change the
212,992-byte uplink internal-RAM gate.

No public push, release, tag, merge, or factory-image promotion is part of this
change.
