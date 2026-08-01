# Factory One-Plug ROM Handoff Design

## Goals

Keep factory flashing a single-plug operation. After all three ESP32-S3
images pass write and explicit readback verification, reliably boot the exact
three proven boards into their applications and seed the selected game role.

When the connected three-board badge already has a complete factory PASS for
the exact accepted release, offer an interactive role-only reassignment
instead of requiring a redundant erase and reflash.

The accepted `0.67.2-badge-defcon34` firmware and embedded factory bundle are
frozen and must not change.

## Root Cause

The factory flasher currently leaves all boards in ROM after its final MAC
rebind, then attempts to boot them by briefly opening native USB serial,
releasing DTR/RTS, waiting 1.5 seconds, and closing the handle. Role
provisioning subsequently uses reset-neutral descriptor-bound opens.

This split handoff is timing-sensitive. A board can remain visible to macOS
while its application USB protocol is silent. The role command is then never
sent, so an erased uplink displays the safe HUMAN default. The error report is
also misleading because a later scanner-port rejection can overwrite the
earlier matching-uplink timeout.

## New-Badge Handoff

After the existing final three-MAC ROM rebind:

1. Hand off the BLE scanner, Wi-Fi scanner, and uplink in that order.
2. For each exact ROM-bound device, run the updater's existing proven
   application handoff:
   - use esptool without a stub;
   - clear the ESP32-S3 forced-download bit with the exact masked write to
     `0x6000812c`;
   - finish with `watchdog_reset`;
   - require the exact expected MAC, force-download-clear receipt, and
     watchdog-reset receipt.
3. Poll reset-neutral, descriptor-bound application ports for the exact
   uplink PONG/status.
4. Only after exact application readiness, run the existing seed,
   acknowledged reboot, successor-generation, scanner-health, and runtime
   gates.
5. If initial application readiness remains silent, permit one bounded,
   non-writing recovery: reset the same descriptor serial and physical
   location into ROM, re-prove its MAC, and repeat the handoff. Do not erase
   or reflash.
6. If the retry fails, fail closed with the matching uplink error and no PASS
   receipt.

The existing seed transaction remains idempotent. Reassigning the same role is
valid and must still receive an exact acknowledgment and fresh reboot proof.

## Previously Passed Badge

After the initial three-board ROM identity scan, compare the complete MAC set
against prior PASS records:

1. With no matching PASS record, follow the normal topology-probe and flash
   path.
2. A partial match, conflicting assignment, mixed badges, or older/different
   release fails closed.
3. One exact prior assignment with the current version and bundle hash shows:

   ```text
   ALREADY PASSED // REASSIGN ROLE ONLY? [Y/N]
   ```

4. `N` cancels without changing the badge. `Y` skips probe firmware, erase,
   production writes, and flash readback.
5. Boot the ledger-proven BLE scanner, Wi-Fi scanner, and uplink with the same
   exact-MAC application handoff used by the new-badge path.
6. Before mutation, require the exact uplink identity/version/target and the
   two exact scanner MAC/role/profile/version/health records from the prior
   assignment.
7. Seed the role already selected in the interactive menu, require its exact
   acknowledgment, reboot proof, selected role/state, inactive game, zero
   shield, and complete runtime health.
8. Record a distinct `reassign` operation and opaque receipt. Do not claim new
   flash write/readback evidence.

The existing explicit `--allow-rework` path retains its full-reflash meaning.
Unattended operation does not silently choose role-only reassignment.

## Safety Invariants

- All existing topology, ROM-MAC, write, and explicit readback gates remain.
- Every handoff stays bound to the expected immutable MAC.
- No raw MAC, bundle hash, or esptool transcript becomes public output.
- Seed commands remain descriptor-bound and reset-neutral.
- A failed handoff never creates a PASS record.
- Role-only reassignment requires one complete prior PASS for the exact trio,
  version, and bundle hash.
- Role-only output and ledger evidence cannot be confused with a new flash.
- No firmware image, bundle, partition layout, Android code, or badge runtime
  behavior changes.

## Verification

Implementation must use test-driven development:

- command construction requires the exact no-stub clear plus watchdog reset;
- receipt parsing rejects wrong MACs, missing/duplicate clear receipts, and
  missing/early watchdog receipts;
- boot order remains BLE, Wi-Fi, uplink;
- one silent-app recovery retries only the non-writing handoff;
- a second failure stops without seeding or PASS;
- the matching uplink diagnostic cannot be overwritten by scanner mismatch;
- a complete exact prior PASS offers role-only reassignment;
- partial, conflicting, stale-version, or noninteractive prior matches cannot
  enter role-only reassignment;
- reassignment performs no probe, erase, write, or readback operation;
- same-role reassignment remains acknowledged, rebooted, and verified;
- the complete factory suite and bundle-builder suite pass;
- the embedded ZIP and accepted application hashes remain unchanged.

One attended three-board factory canary must then prove write/readback, one-plug
handoff, selected non-HUMAN seed, reboot proof, and both scanner health gates.
One already-passed badge must also prove role-only reassignment without any
flash operation.
