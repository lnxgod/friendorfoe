# GameChangersAI Badge Easter Egg Animation Design

**Date:** 2026-07-18

**Target:** FoF Badge uplink firmware (`uplink-s3-fof_badge`)

**Branch:** `codex/defcon34-badge-final`

## Objective

Replace the current Hell, Michigan Easter egg presentation with a two-stage
GameChangersAI thank-you experience while preserving the already working
trigger transport, scanner behavior, normal four-lane interface, and one-shot
per-boot safety contract.

## Interaction Contract

The existing BLE Remote ID, exact `fof-goblue` SSID, and temporary badge-button
triggers remain unchanged. Triggering enters the `THANKS` presentation state.

The visible state sequence is:

```text
ARMED -> THANKS -> BOUNCE -> CONSUMED
```

- `THANKS` is a static full-screen overlay containing the existing Wall of
  Sheep mark and the exact text `Thank you from` and `GameChangers AI`.
- The first debounced badge-button press while `THANKS` is visible is consumed
  and advances to `BOUNCE`. Its matching release cannot leak into normal button
  handling.
- `BOUNCE` displays the official GameChangersAI logo moving diagonally like the
  classic DVD screensaver. The logo changes color only when it collides with a
  screen edge.
- The next debounced badge-button press while `BOUNCE` is visible is consumed,
  returns to the normal badge interface, and advances the Easter egg to
  `CONSUMED`.
- `CONSUMED` ignores every Easter egg trigger until the badge reboots. The latch
  remains RAM-only, so reboot intentionally returns the machine to `ARMED`.
- Either physical badge button performs the `THANKS -> BOUNCE` and
  `BOUNCE -> CONSUMED` transitions.

No remotely supplied text is rendered, and malformed or repeated trigger
events remain inert under the existing validated trigger contract.

## Presentation

### Thank-you screen

The thank-you screen keeps the existing blacklight-purple visual treatment and
embedded 72x72 Wall of Sheep asset. The former `Welcome to Hell`,
`Just Kidding`, and `Defcon 34 FoF` copy is removed. The replacement composition
centers the Wall of Sheep mark and gives both thank-you lines enough separation
to remain legible on the 160x160 ST7735 panel.

### DVD bounce

The animation uses the canonical logo published by GameChangersAI at:

`https://gamechangersai.org/assets/gamechangers-128.png`

The retrieved source is a 128x128 RGBA PNG with SHA-256:

`903d20f0b3d52c8b5b785686680cbb5e884ea17a5636fdf381e9752ade92efce`

An offline converter verifies that hash and emits a 64x64 indexed derivative.
The indexed pixels and base palette are `const` firmware data so the badge does
not carry a PNG decoder or allocate image memory at runtime. The generated file
retains source attribution, the verified hash, and its reproducible conversion
command.

The logo travels inside the full 160x160 display bounds using signed integer
position and velocity. A pure animation step clamps both axes, reverses the
corresponding velocity at each collision, and advances a fixed vivid RGB565
color cycle once per update containing one or more collisions. Corner hits
reverse both axes but advance the color only once. The background remains black
so every palette color has strong contrast.

Animation state is initialized deterministically when entering `BOUNCE`; it is
not persisted and does not consume entropy, NVS, network, BLE, Wi-Fi, or scanner
resources.

## Architecture and Performance

The existing scanner-to-uplink Easter egg event path remains unchanged. Only
the shared Easter egg presentation state, badge button transition handling, and
ST7735 rendering are extended.

The animation reuses the existing display task and frame buffer. It adds no new
FreeRTOS task, timer, queue, mutex, or heap allocation. Normal four-lane display
cadence remains unchanged. The display task may use a modest shorter delay only
while the state is `BOUNCE`; it immediately restores the existing badge cadence
when the state changes. UART receive and scanner update tasks retain their
existing priorities and are not modified.

If the logo converter rejects the source hash, generation fails without
changing generated firmware assets. Invalid animation dimensions or state
values fall back to bounded initial coordinates rather than writing outside the
frame buffer.

## Verification

Native tests will prove:

- the complete `ARMED -> THANKS -> BOUNCE -> CONSUMED` sequence;
- invalid, repeated, and post-consumption triggers remain rejected;
- one button batch creates at most one state transition and consumes its
  matching release;
- animation steps never place the 64x64 logo outside the 160x160 display;
- horizontal, vertical, and corner collisions reverse the correct velocities;
- each collision update advances exactly one color and non-collision updates do
  not change color;
- the removed Hell copy no longer appears in the firmware renderer; and
- the official logo converter rejects any source whose SHA-256 changes.

Release verification includes the complete ESP32 native suite, fresh scanner
and uplink badge builds, image-size review, USB command health checks, UART relay
flashing of both scanner slots, immutable scanner identity/version checks, boot
health, and physical LCD/button validation on the connected test badge.

## Safe Checkpoint and Publishing

The unrelated `.camera-before-zoom.jpg` file is excluded from version control.
No state is labeled safe merely because it compiles. After all automated and
physical verification succeeds, the release changes are committed on
`codex/defcon34-badge-final`, an annotated
`defcon34-valid-safe-v0.64.76` tag is created, and the branch and tag are pushed
to GitHub. This workflow does not merge or push directly to `main`.
