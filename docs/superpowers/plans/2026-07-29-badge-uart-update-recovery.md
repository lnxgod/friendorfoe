# Badge UART Update Recovery Plan

> **For agentic workers:** Use subagent-driven development and test-driven
> development. Each production change must be preceded by a regression that
> fails for the observed reason.

**Goal:** Repair the two confirmed update-path defects exposed by the first
`0.67.1` hardware canary, ship the fix as private `0.67.2-badge-defcon34`
firmware, and prove three complete badge graphs converge through their single
uplink USB connection before the 42-badge batch.

**Observed failure:** Badge `/dev/cu.usbmodem1101` updated its uplink and
durably stored the exact scanner image, but neither scanner left `0.67.0`.
The coordinator persisted identity acquisition as active without proving that
`fw_check_now` reached the scanners; its worker can also create the same active
state without sending a prompt. After the campaign stopped, the host read a
known connected-but-booting scanner placeholder and reported the empty
`firmware_name` as a target mismatch instead of waiting for identity readiness.

## Global constraints

- Work only in `.worktrees/defcon34-badge-final` on
  `codex/defcon34-badge-final`; preserve `.camera-before-zoom.jpg`.
- Keep scanning, game behavior, display behavior, roles, BLE protocol,
  partitions, factory images, and public releases unchanged.
- Scanner firmware must be delivered only through its uplink UART path. Do not
  direct-flash scanner USB or use a ROM-bootstrap fallback.
- Never send UART while holding the coordinator lock.
- A durable `NONE -> ACTIVE` identity-acquisition transition is valid only if
  it schedules checked `fw_check_now` delivery after unlock.
- A proven failed/partial UART delivery must durably return only the affected,
  nonterminal lanes to `NONE`. If that rollback cannot be persisted, fail
  closed; do not leave an apparently recoverable in-memory state.
- Prompt delivery consumes no scanner-readiness or relay-attempt budget.
  Same-generation terminal exhaustion stays terminal.
- Host recovery must use the existing bounded identity-readiness poll and the
  original operation deadline. It must not create a fresh unbounded wait or
  authorize any new mutation.
- Revalidate uplink identity, scanner identities/roles, firmware versions,
  partition identity, recovery state, and inactive game state after recovery.
- Keep scanner internal RAM at or below 180,224 bytes, scanner app at or below
  1,363,148 bytes, and uplink internal RAM at or below 212,992 bytes. First try
  the existing 1,468,016-byte uplink gate. If the durability fix alone crosses
  it after a bounded clean rebuild, permit only the smallest exact 16-byte
  alignment boundary containing the verified artifact and record the delta
  and remaining 2 MiB OTA-slot headroom.
- Do not push, tag, merge, publish, replace the factory bundle, or flash more
  than one badge graph at a time.

---

### Task 1: Make identity-acquisition prompts delivery-checked

**Files:**
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`
- Modify: `esp32/uplink/main/network/fw_store.c`

- [ ] Add focused regressions proving that both the explicit reprompt path and
  the worker fallback schedule `fw_check_now` after durable `NONE -> ACTIVE`.
- [ ] Add a regression proving checked send failure cannot leave affected
  nonterminal lanes durably `ACTIVE`.
- [ ] Prove the new regressions fail against the existing unchecked/omitted
  delivery paths.
- [ ] Reuse `fw_store_request_scanner_checks()` or the existing checked UART
  API; do not add a parallel transport.
- [ ] Persist state before send, send after unlock, and on checked failure
  persist the affected lanes back to `NONE` without spending readiness/relay
  budget. Fail closed if rollback persistence fails.
- [ ] Run the focused transport-contract tests and the complete ESP32 native
  suite, then commit locally.

---

### Task 2: Wait out post-recovery scanner placeholders

**Files:**
- Modify: `scripts/test_fof_badge_flash.py`
- Modify: `scripts/fof_badge_flash.py`

- [ ] Add a regression in which recovery first returns connected scanners with
  blank identity fields, then returns the correct identities; it must pass
  only after the existing readiness poll is used.
- [ ] Add a regression in which placeholders persist to the original deadline;
  it must fail closed without issuing another flash/update mutation.
- [ ] Prove both regressions fail against the current immediate capture.
- [ ] After maintenance recovery and before consuming scanner identities, use
  `wait_for_scanner_status_usb()` with the originally bound slots and remaining
  operation deadline.
- [ ] Preserve the original campaign failure transcript and all existing retry
  eligibility rules.
- [ ] Run the focused tests and the complete flasher/USB-hardening suites, then
  commit locally.

---

### Task 3: Build and verify private `0.67.2`

**Files:**
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`
- Modify: `backend/tests/test_firmware_build_version.py`
- Modify: `esp32/shared/version.h`
- Modify only if the exact verified artifact requires it:
  `esp32/scripts/verify_badge_uplink_build.py`

- [ ] Reconcile the obsolete transport-contract assertion that still requires
  the removed `SHIELD %3u%%` copy. Keep its four-lane/theme-schema checks and
  instead require the already-approved heart-HUD renderer seam; make the full
  transport-contract file green without changing production HUD code.
- [ ] Move only the private badge-canary identity to
  `0.67.2-badge-defcon34`, with a red/green version-contract test.
- [ ] Run all firmware-version, ESP32 native, flasher, USB-hardening, scanner
  strict-build, and uplink strict-build gates.
- [ ] Record exact scanner/uplink sizes, RAM, OTA headroom, and SHA-256 values.
- [ ] Freeze the verified artifacts used for hardware; do not rebuild between
  verification and flashing.

---

### Task 4: Prove three complete graphs through one uplink each

- [ ] Take a fresh read-only descriptor and hardware-identity census.
- [ ] Flash `/dev/cu.usbmodem1101` first. Require its uplink and both scanners
  to converge to exact `0.67.2`, with correct unique identities/roles, active
  radios, rollback clear, recovery normal, pending-verify false, zero crashes,
  USB/UART alive, seed preserved, and game inactive after boot.
- [ ] Only after badge 1 passes, flash and prove `/dev/cu.usbmodem1201`, then
  `/dev/cu.usbmodem1401`, sequentially with the same requirements.
- [ ] Do not use direct scanner USB, concurrent flashing, or an undocumented
  retry path.
- [ ] Leave HUD/button/game physical acceptance for the user’s return.

---

### Task 5: Final private release gate

- [ ] Run one broad whole-branch code review and address load-bearing findings.
- [ ] Run fresh full verification after the final source change.
- [ ] Record automated and hardware evidence locally.
- [ ] Leave the branch private and the 42-badge factory batch unstarted until
  the user completes hands-on acceptance.
