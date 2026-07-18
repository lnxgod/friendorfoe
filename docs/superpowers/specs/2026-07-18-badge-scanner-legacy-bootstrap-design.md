# Badge Scanner 0.64.68 Legacy Bootstrap Design

## Problem and Evidence

The physical badge uplink is running `0.64.69-badge-defcon34`, while both
attached scanners are healthy on `0.64.68-badge-live-follow`. The uplink
successfully stores and SHA-256-verifies the exact 1,192,736-byte scanner image,
but its durable coordinator exhausts three readiness probes with zero relay
attempts.

The `v0.64.68-live-follow` source proves the legacy receipt dialect and the
30-second OTA idle watchdog. That scanner emits a legacy `fw_ready` frame
containing only board, current version, target version, size, and CRC32. The new
uplink requires target name, project, hardware type, SHA-256, generation, and
`allow_same_version:false` as well. The new parser therefore rejects the old
receipt before transferring bytes. The old scanner also returns session-bound
but manifest-light `ota_ack`, 100-percent `ota_progress`, and `ota_done` frames
rather than the strict `.69` manifest receipts.

The tagged `.68` image does **not** emit the later extended scanner identity
fields (`firmware_name`, `app_project`, `hardware_type`, and immutable
`hardware_id`). The connected physical scanners do emit and have already been
observed with those exact fields while reporting the same `.68` version. The
bridge therefore targets only that identity-capable `.68` release-line build.
The tag documents its wire compatibility, but the tagged binary itself remains
ineligible and fails closed because it cannot supply a fresh complete identity.

## Selected Design

Add a one-hop compatibility bridge for the exact source version
`0.64.68-badge-live-follow`. The bridge does not treat the legacy packet as
authority. Authority remains the uplink's committed staged manifest and durable
coordinator.

A legacy `fw_ready` is accepted only when all of these conditions are true:

1. Every strict-receipt-only JSON field is absent, so a malformed partial `.69`
   receipt cannot fall back to legacy parsing.
2. The source version is exactly `0.64.68-badge-live-follow`.
3. The echoed board, current version, target version, size, and CRC32 are exact.
4. The uplink has received a complete scanner identity frame *after* the current
   coordinator generation began: target, project, hardware type, immutable MAC,
   board, and version all match the staged badge scanner contract and the
   legacy frame. At the accepted `fw_check`, the uplink captures that identity
   generation and MAC as a volatile offer snapshot. The following legacy
   `fw_ready` must match the same snapshot; a reset discards it and requires a
   fresh identity/check/offer sequence.
5. The staged image is still committed, has a valid SHA-256, and is strictly
   newer under the shared ordered-version policy.
6. The exact generation requests this slot, the BLE-first gate is open, and the
   durable slot state is exactly `offered` from a preceding accepted `fw_check`.

Only then may the coordinator durably transition the slot to `ready_queued`.
The missing legacy fields are bound internally to the already committed
manifest; they are never copied from untrusted defaults.

Identity is not read through the existing unsynchronized pointer getter for
this decision. The UART parser publishes a small, mutex-protected identity
snapshot containing only the contract fields, generation, completeness, and
receive time. Before `ota_begin`, the relay waits up to 12 seconds for one more
periodic identity generation from the stopped `.68` scanner and requires the
same MAC/version/contract. The bound MAC is persisted in the coordinator before
the relay reservation becomes durable.

## Relay Compatibility

The existing relay core already carries an explicit `legacy_mode` flag. For an
automatic relay it becomes true only when the pre-transfer command-health probe
again reports the exact legacy source version.

Legacy mode changes only receipt validation:

- `ota_ack` must parse as exact type `ota_ack` and match the freshly generated
  relay session ID through a dedicated legacy matcher; strict manifest receipt
  fields are not required and the strict manifest matcher is never called with
  a null manifest as a legacy shortcut.
- The final chunk must produce an exact session-bound 100-percent
  `ota_progress` with `received == total == staged size`. Silence is not success.
- `ota_done` must match the same fresh session and exact received size. Strict
  mode continues requiring the full immutable manifest receipt.

All byte and image safeguards remain active. The uplink recomputes the staged
raw-image SHA-256 before relay. Every UART chunk has CRC32 and bounded NACK
retransmission. The legacy scanner checks the complete image CRC32 and calls
`esp_ota_end`, which validates the ESP application image before setting its boot
partition. Success still requires a fresh post-reboot identity from the same
immutable MAC, exact `.69` target/project/hardware/version, normal recovery,
rollback clear, command ingress, and role-specific radio health.

After the first successful bootstrap, the scanner is `.69` and all future
updates use the strict generation/SHA/identity receipts. No other legacy source
version is accepted automatically.

## Rejected Alternatives

1. Manually flash both scanners once. This avoids compatibility code but fails
   the required laptop-to-uplink automatic update workflow and does not test the
   production path.
2. Accept any legacy receipt from any older version. This weakens generation and
   identity binding indefinitely and can turn malformed strict receipts into a
   downgrade path.
3. Force the strict parser to fill missing fields from the current manifest.
   This is fail-open because it cannot distinguish an intentionally legacy
   frame from a damaged or truncated strict frame.

## Failure and Recovery Behavior

- Any identity, version, manifest, coordinator, session, size, CRC, or state
  mismatch rejects the legacy receipt, resumes the scanner, and transfers no
  bytes.
- A failed legacy transfer consumes the same bounded attempt budget and retry
  policy as strict transfer.
- Uplink reset before relay reconstructs the current generation and requires a
  new identity, `fw_check`, and legacy receipt. A slot restored from durable
  `relaying` enters a distinct durable `recovering` state and retains its bound
  MAC and consumed attempt. The uplink sends an abort sentinel, then waits 35
  seconds without consuming readiness probes so a partial legacy binary OTA can
  cross its 30-second idle watchdog. Only afterward does it use the normal three
  bounded 20-second probes.
- A recovering slot can become `converged` only after a manual probe response
  plus a fresh, complete, exact target identity from the persisted MAC with
  rollback clear, recovery normal, command ingress, assigned scan profile, and
  role-specific radio health. If that same proven scanner remains `.68`, it may
  return to `offered` and consume another already-bounded relay attempt. A bare
  equal-version `fw_check` can never turn an interrupted relay into `current`.
- A scanner that boots the new image but loses the final UART receipt is accepted
  only through the existing stronger same-MAC post-reboot convergence proof.
- The currently failed physical generation is not mutated in place. Restaging
  the same verified image creates a new generation with fresh probe budgets.
- Wi-Fi's gate opens only after BLE is durably `converged` or was already exact
  `current`. BLE `failed`, `refused`, or `newer_skipped` remains terminal for its
  own accounting but does not authorize a Wi-Fi relay.

## Verification

Behavioral native policy tests and supplemental source-contract tests cover the
exact positive identity-capable
`.68` frame and negative cases for every missing/mismatched field, a tagged
`.68`-style incomplete identity, stale identity generation, malformed partial
strict receipt, wrong source version, stale/non-offered coordinator state, gate
closed, session mismatch, incomplete progress, wrong received size, recovery
cooldown/probe accounting beyond the 30-second legacy watchdog, interrupted
relay convergence decisions, durable-save refusal, BLE success gating, and strict-mode
non-regression. Builds must pass for badge and production uplinks plus the badge
scanner. The real hardware gate must show a new generation, a freshly observed
complete identity for each physical `.68` scanner, BLE relay attempt and
convergence first, Wi-Fi relay second, both exact immutable MACs on `.69`, zero
pending bits, stopped worker, rollback clear, and healthy role-specific radios.
