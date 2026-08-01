# Badge USB Control and Flashing Hardening Design

## Goal

Make the badge's existing USB-C connection the dependable control and update
interface for every assembled badge without disconnecting its battery or
changing the PCB:

- Android uses USB for live badge detections, status, themes, palettes, display
  policy, navigation, and safe control commands.
- The laptop flasher uses USB to update the uplink and then stage one scanner
  image that the uplink relays to both scanner boards over their existing UARTs.
- Holding OK and Menu together for 10 seconds provides an independent recovery
  path. It enters ROM download mode when an active USB host is attached and
  performs a normal controlled reboot otherwise.
- The physical power-off gesture remains removed.

Android does not upload firmware. HTTP, Wi-Fi AP, LAN, and BLE are not firmware
mutation paths for the badge.

## Confirmed Failure Boundary

The connected uplink currently enumerates on macOS as the ESP32-S3 native USB
Serial/JTAG device at `/dev/cu.usbmodem101`, but:

- a bounded `FOF_PING` probe transmits and receives zero response bytes;
- the result is unchanged with CDC DTR asserted;
- esptool cannot synchronize after its native USB reset sequence;
- two installed Espressif OpenOCD versions find the correct USB VID/PID but fail
  in macOS USB descriptor access before reaching the CPU.

No probe erased or wrote flash. The evidence proves that visible USB
enumeration alone is not a usable application or ROM flashing transport.

The current firmware mixes standard ESP-IDF console VFS reads during the boot
configuration window with direct USB Serial/JTAG FIFO reads in the persistent
control task. It also permits several tasks to write logs and framed control
traffic to the same console. This design removes those mixed ownership paths
instead of relying on host reset behavior to repair them.

## Architecture

### 1. One Persistent USB Transport

The uplink has one dedicated USB control task for the lifetime of the
application. It owns all command and binary-upload input through the configured
ESP-IDF USB Serial/JTAG console VFS. Persistent control and the bounded boot
window use the same read primitive; application code no longer reads the USB RX
FIFO directly behind the driver's back.

The task supports two explicit input states:

1. **Framed command state** parses newline-delimited `FOF_*` commands.
2. **Binary upload state** reads exactly the declared number of bytes into the
   selected OTA or scanner-staging partition, then returns to framed commands.

Every state has an absolute size bound, an idle timeout, abort cleanup, and an
unambiguous terminal response. Malformed input cannot leave the transport stuck
in binary mode.

Small control responses have priority over optional telemetry. `FOF_PONG`,
upload acknowledgements, errors, and reboot/bootloader acknowledgements must
not wait indefinitely behind logs, detections, or investigation output.
Optional asynchronous frames may be dropped with a counter when the host is not
draining output; command responses may not be silently dropped.

### 2. Stable USB Application Contract

The existing protocol remains backward compatible:

- `FOF_PING` returns the exact running badge version.
- `FOF_STATUS` reports uplink identity, scanner identities and roles, USB
  health, staged scanner metadata, and update state.
- `FOF_CTL` continues to provide Android-safe status and control operations.
- `FOF_DET` continues to stream badge-sourced detections to Android.
- Existing scanner staging and relay frames remain available to the laptop.

New machine-readable USB health fields include RX bytes, valid commands,
responses, malformed lines, dropped optional frames, current parser state,
upload progress, and the ages of the last task heartbeat, received command, and
completed response. These distinguish "task scheduled" from "host command
completed."

Android remains unable to begin uplink or scanner firmware uploads. Firmware
commands stay outside its repository/UI surface, while the same connection can
continue using all non-firmware badge controls.

### 3. Uplink Application Update over USB

The normal laptop path updates the uplink through its running application,
without requiring esptool to reset the chip:

1. The laptop reads `FOF_STATUS` and validates exact uplink target, project,
   hardware type, current version, and hardware ID.
2. It begins an uplink OTA upload with the target version, size, CRC32, SHA-256,
   target, project, and hardware type.
   The host sends at most one device-granted 4 KiB credit window at a time so
   native USB cannot outrun flash writes and overflow the RX ring.
3. The uplink writes sequentially to `esp_ota_get_next_update_partition(NULL)`.
   The existing `ota_0` and `ota_1` partitions are each 2 MiB and remain
   separate from the 2 MiB `fw_scanner_s3` staging partition.
4. The uplink validates the actual ESP application descriptor and digest from
   the received bytes. Metadata supplied by the host cannot substitute for the
   embedded firmware identity.
5. Only a complete, exact badge-uplink image can pass `esp_ota_end()` and become
   the next boot partition. Downgrades and unordered same-core variants fail
   closed; exact same-version rewrites require the explicit recovery flag.
6. The uplink acknowledges the durable result, arms an expected reboot, and
   restarts. Existing pending-verify rollback protects the new image.
7. The laptop reconnects, requires the new uplink identity/version and healthy
   USB round trip, then stages the scanner image and verifies automatic UART
   convergence for both scanner slots.

An interrupted uplink upload leaves the running partition selected and aborts
the incomplete OTA handle. It does not alter the cached scanner image.

### 4. Ten-Second Physical Recovery Chord

The already-approved OK-plus-Menu state machine retains its exact 10,000 ms
boundary, release cancellation, boot-held suppression, one-shot behavior, and
32-bit timer-wrap safety.

At the threshold, the uplink samples fresh USB Start-of-Frame activity over a
short bounded window:

- **Active USB host:** show `USB FLASH? OK=YES MENU=RESET` after both buttons
  are released. A fresh OK press within five seconds marks the reset expected,
  forces ESP32-S3 ROM download boot, and restarts. Menu or timeout performs a
  normal software restart. The confirmation prevents an attached Android phone,
  which is also a USB host, from accidentally stranding the badge in ROM.
- **No active USB host:** mark the reset expected and perform a normal software
  restart.

A power-only charger that produces no USB bus frames counts as no host. The
gesture never toggles quiet/off mode. Quiet mode may remain available through
the USB control surface, but it is not a physical button action.

The laptop recovery flow waits for ROM enumeration before running esptool. It
does not assume that DTR/RTS can reset an unresponsive application.

### 5. USB Self-Recovery

The persistent USB task emits an internal heartbeat independently of host
traffic. The watchdog distinguishes:

- task creation failure;
- task heartbeat failure;
- active upload progress;
- an idle but healthy task;
- a host-attached task receiving data but failing to complete commands.

A task creation or heartbeat failure causes one expected normal restart into a
minimal USB-first boot. Recovery state is one-boot/RTC state rather than a
permanent NVS trap. The boot sequence starts the display, USB console, and
10-second button task before network or scanner work. Safe USB boot keeps
`FOF_PING`, `FOF_STATUS`, uplink OTA upload, and ROM-entry recovery available;
it may defer scanning but must not block firmware repair.

Long scanner relays and post-update health checks explicitly report progress so
they cannot be mistaken for a dead USB task. No watchdog restart is allowed
while flash writes or a scanner UART relay owns its transaction, except a real
task/panic watchdog reset handled by normal rollback rules.

### 6. Laptop Flasher User Flow

The supported operator flow remains one cable and one command:

1. Start the flasher; it detects the one connected badge uplink.
2. If `FOF_PING` and `FOF_STATUS` work, it performs application USB OTA for the
   uplink when needed.
3. If the application is silent, it displays
   `HOLD OK + MENU FOR 10 SECONDS, RELEASE, THEN PRESS OK` and waits for ROM
   download mode. It never tells the operator to disconnect the battery.
4. In ROM mode it flashes the complete uplink layout, verifies the write, and
   waits for the application USB contract.
5. It stages the scanner image once, waits for both scanner UART updates, and
   verifies immutable scanner MACs, exact versions, rollback-clear state,
   BLE-primary/Wi-Fi-primary slot roles, and live radio health.
6. It finishes by exercising `FOF_PING`, `FOF_STATUS`, and one reversible
   Android-equivalent control round trip, then restores the prior setting.

Blank uplinks already in ROM download mode skip the application probe. Multiple
ambiguous USB devices fail closed and require an explicit port.

## Safety and Compatibility

- No GPIO assignment, PCB trace, button wiring, partition offset, or flash size
  changes.
- No Android firmware upload feature.
- No HTTP/AP/LAN/BLE firmware mutation.
  Badge builds do not register the existing mutating HTTP OTA/store/relay
  handlers, do not expose HTTP Bootloader control, and do not start the
  backend-driven firmware auto-check worker. Read-only status may remain.
- No change to the scanner image format or scanner UART wiring.
- No version bump, factory-bundle replacement, tag, release, or GitHub push
  until physical battery-connected verification passes.
- The currently silent badge does not yet contain the new recovery chord. If
  neither its application USB nor native host reset/JTAG can be recovered, it
  needs a one-time existing-board bootstrap into ROM mode by holding the
  onboard XIAO BOOT control while momentarily pressing its onboard RESET
  control. The contacts and read-only ROM identity must be physically proven
  before any write. This limitation does not apply after the hardened firmware
  has been installed once.

## Verification

### Automated

- Native state-machine tests cover exact reset timing, host/no-host selection,
  charger behavior, early release, boot-held suppression, one-shot behavior,
  and timer wrap.
- Native protocol tests cover framed/binary transitions, exact byte counts,
  timeout/abort recovery, malformed lines, response priority, health counters,
  image identity, digest, downgrade rules, and rollback selection.
- Laptop tests cover application OTA, ROM fallback prompting, port
  re-enumeration, interrupted transfers, current-version skips, explicit
  same-version recovery, two-scanner relay, and final verification.
- Android tests confirm firmware commands remain unavailable while all existing
  status, detection, theme, palette, display-policy, navigation, and safe
  control frames still parse and round-trip.
- Complete native ESP32 tests, backend contract tests, Android unit tests, and
  scanner/uplink badge builds must pass.

### Physical Release Gate

Using one assembled uplink and two scanners with the battery continuously
connected:

Start with the exact candidate installed on the uplink and both scanners
running firmware versions strictly older than that candidate. The first gate
anchors that reachable pre-update state; the first laptop cycle then proves the
automatic transition to the exact final scanner candidate. No acceptance step
downgrades or directly USB-flashes a scanner after the session begins.

1. Prove Android can connect, receive badge detections/status, change and
   restore a theme, and reconnect after cable removal.
2. Perform three consecutive laptop update cycles that each verify the uplink
   and both scanners. At least one begins with both scanners strictly older and
   proves automatic coordinator convergence with zero manual relay commands.
3. During one cycle, interrupt an uplink upload and prove the prior firmware
   remains bootable and the next retry succeeds.
4. With USB attached, hold OK + Menu for 10 seconds, release both, press OK at
   the flash confirmation, and prove ROM enumeration, uplink flash, application
   return, scanner staging, and both UART updates.
5. Without a data host attached, hold OK + Menu for 10 seconds and prove a
   normal reboot rather than a persistent ROM wait.
6. Confirm the battery was never disconnected and the badge never entered a
   power-off mode.

Only after all six gates pass may the version be bumped. Because that changes
the firmware bytes, the automated gate and all six physical gates must then be
repeated on the exact final-version binaries before they replace factory
firmware or release artifacts.
