# Badge Two-Button Reset Design

## Goal

Replace the unreliable two-button quiet/off shortcut with a single, predictable
software-reset gesture: holding both physical badge buttons continuously for 10
seconds reboots the uplink badge.

## Behavior

- Both buttons must be stably pressed together for 10,000 milliseconds.
- Releasing either button before 10 seconds cancels the gesture.
- The reset fires once per hold and requires a full release before it can arm
  again.
- Buttons already held while the badge boots are ignored until both are
  released, preventing a reset loop.
- A button press already consumed by another UI action cannot become a reset
  chord until both buttons are released.
- Elapsed-time calculation remains safe across the 32-bit millisecond timer
  wrap.
- The reboot is marked as an expected software reboot before `esp_restart()` so
  badge health and rollback logic do not count it as a crash.

## Compatibility

- Remove the physical nine-second quiet/off toggle only.
- Keep the quiet-mode runtime and USB command surface intact; Android/USB
  control remains compatible.
- Do not change scanner firmware behavior, radio scanning, OTA/UART updating,
  themes, Easter eggs, or factory bundles as part of this change.

## Validation

- Native tests prove the exact 10-second boundary, cancellation, one-shot
  behavior, boot-held safety, consumed-input safety, one-button rejection, and
  timer wrap.
- The backend source contract proves the badge display runtime calls the
  expected-reboot guard and `esp_restart()` and no longer calls the power toggle
  for the button chord.
- Build both scanner and uplink badge targets to guard shared-code and uplink
  integration compatibility.
