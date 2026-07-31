# Badge Easter Egg Radio Retrigger Design

## Goal

Allow the badge Easter egg to be launched repeatedly by the two exact radio
triggers without letting ordinary button presses or unrelated radio traffic
relaunch it indefinitely.

The accepted radio triggers remain:

- Wi-Fi SSID bytes exactly equal to `GameChangersAI-67`.
- Remote ID Basic ID exactly equal to `fof-michagain`, with the existing exact
  Hell, Michigan coordinates and geodetic altitude of 666 metres.

This change does not relax any trigger matching rule.

## User-visible behavior

1. After boot, either exact radio trigger launches the thank-you screen
   immediately.
2. Radio detections received while the Easter egg is visible are ignored. They
   do not restart the presentation or reset a timer.
3. The first button press advances the thank-you screen to the bouncing-logo
   screen. The next button press dismisses it.
4. Dismissal starts a 90-second cooldown.
5. During the cooldown, exact SSID and Remote ID detections are ignored.
6. At or after 90 seconds, either exact radio trigger may launch the Easter egg
   again, even if the transmitter remained continuously visible.
7. A physical-button launch remains one-shot until reboot. The radio retrigger
   path does not re-arm repeated button launches.
8. Reboot clears the in-memory state and permits an immediate first launch.

## State-machine design

The shared, platform-independent Easter egg state machine owns the cooldown.
It records whether a cooldown is active and the 32-bit monotonic millisecond
timestamp at which the Easter egg was dismissed. Elapsed time is calculated
with unsigned subtraction, making the 90-second comparison safe across the
32-bit timer wrap.

Runtime entry points supply the current monotonic time when triggering,
advancing, or dismissing the state machine. The shared state machine accepts a
radio retrigger only when all of the following are true:

- The source is the exact Wi-Fi or Remote ID trigger.
- The Easter egg is not currently visible.
- No cooldown is active, or at least 90,000 milliseconds have elapsed since
  dismissal.

The first launch still uses the existing armed state. A successful radio
retrigger returns the presentation to the thank-you phase and records the
radio source. Rejected triggers do not alter state or extend the cooldown.

## Scope and safety

Only shared Easter egg state, the uplink runtime adapter, and their tests are
changed. Scanner matching, approved presentation copy, display rendering,
four-lane UI behavior, privacy filters, firmware versions, and update transport
are unchanged. No badge is flashed as part of this implementation.

## Verification

Native regression tests will be written before production changes and will
cover:

- First radio trigger succeeds immediately.
- A radio trigger while visible is ignored.
- Retrigger at 89,999 milliseconds is rejected.
- Retrigger at exactly 90,000 milliseconds succeeds.
- Continuous-source retrigger succeeds after the cooldown without requiring a
  disappearance event.
- Rejected triggers do not extend the cooldown.
- Cooldown comparison works across a 32-bit timestamp wrap.
- Physical-button activation remains one-shot.
- Reboot initialization clears the cooldown.

After the focused red-green cycle, the complete ESP32 native test suite and the
uplink badge firmware build will be run. Firmware size will be reported, and no
hardware flash, version bump, tag, or push will be performed without a separate
request.
