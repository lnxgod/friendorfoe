# Factory One-Plug Handoff Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore deterministic one-plug factory provisioning and offer verified role-only reassignment for an exact previously passed badge.

**Architecture:** Add a small, receipt-checked ESP32-S3 ROM-to-application handoff to the factory flash engine and use it after the final immutable-MAC rebind. Extend the private ledger with strict prior-PASS records so the interactive flasher can boot and reassign an exact accepted badge without probe firmware, erase, or production writes.

**Tech Stack:** Python 3, `unittest`, esptool 4.11, pyserial, append-only JSONL/CSV factory ledger.

## Global Constraints

- Keep `0.67.2-badge-defcon34` firmware and the embedded factory ZIP byte-for-byte unchanged.
- Keep every existing topology, ROM-MAC, write, explicit readback, seed, reboot-generation, and runtime-health gate.
- Never expose raw MACs, bundle hashes, or raw esptool transcripts in public output.
- Role-only reassignment requires one complete prior PASS for the exact trio, version, and bundle hash.
- A failed handoff or reassignment writes no PASS receipt.
- Preserve the unrelated `.camera-before-zoom.jpg`.
- Do not push or publish the private DEF CON game work.

---

### Task 1: Receipt-Checked ROM-to-Application Handoff

**Files:**
- Modify: `tools/badge_flasher/flash.py`
- Test: `tools/badge_flasher/tests/test_flash.py`

**Interfaces:**
- Consumes: `UsbDevice(mac, port, ...)` and the existing injected `FlashEngine` command runner.
- Produces: `FlashEngine.handoff_to_application(device: UsbDevice) -> None`.

- [ ] **Step 1: Write failing tests for the exact handoff command and receipts**

Add tests that require the exact no-stub masked write and reject incomplete
evidence:

```python
def test_handoff_clears_force_download_and_watchdog_resets_exact_mac(self):
    calls = []
    transcript = (
        "MAC: E0:72:A1:F9:47:FC\n"
        "Wrote 00000000, mask 00000001 to 6000812c\n"
        "Hard resetting with a watchdog...\n"
    )
    device = UsbDevice(
        "E0:72:A1:F9:47:FC", "/dev/cu.x", "ESP32-S3",
        "v0.2", "8MB", "8MB",
    )
    FlashEngine(lambda command: calls.append(command) or transcript) \
        .handoff_to_application(device)
    command = calls[0]
    self.assertIn("--no-stub", command)
    self.assertEqual(command[command.index("--before") + 1], "no_reset")
    self.assertEqual(
        command[command.index("--after") + 1],
        "watchdog_reset",
    )
    self.assertEqual(
        command[command.index("write_mem"):],
        ["write_mem", "0x6000812c", "0x0", "0x1"],
    )
    self.assertLess(command.index("--no-stub"), command.index("write_mem"))
```

Add table-driven failures for:

```python
(
    ("wrong MAC", "MAC: E0:72:A1:F9:49:84\n"
                  "Wrote 00000000, mask 00000001 to 6000812c\n"
                  "Hard resetting with a watchdog...\n"),
    ("missing clear", "MAC: E0:72:A1:F9:47:FC\n"
                      "Hard resetting with a watchdog...\n"),
    ("missing reset", "MAC: E0:72:A1:F9:47:FC\n"
                      "Wrote 00000000, mask 00000001 to 6000812c\n"),
    ("duplicate clear", "MAC: E0:72:A1:F9:47:FC\n"
                        "Wrote 00000000, mask 00000001 to 6000812c\n"
                        "Wrote 00000000, mask 00000001 to 6000812c\n"
                        "Hard resetting with a watchdog...\n"),
)
```

- [ ] **Step 2: Run the focused tests and verify RED**

Run:

```bash
python3 -m unittest \
  tools.badge_flasher.tests.test_flash.FlashTests.test_handoff_clears_force_download_and_watchdog_resets_exact_mac \
  tools.badge_flasher.tests.test_flash.FlashTests.test_handoff_rejects_incomplete_or_wrong_receipts \
  -v
```

Expected: errors because `handoff_to_application` does not exist.

- [ ] **Step 3: Implement the minimal handoff**

Add constants and a receipt parser local to `flash.py`:

```python
_FORCE_DOWNLOAD_CLEAR = (
    "Wrote 00000000, mask 00000001 to 6000812c"
)
_WATCHDOG_RESET = "Hard resetting with a watchdog..."

def _verify_handoff_receipt(output: str, expected_mac: str) -> None:
    if _reported_mac(output) != normalize_mac(expected_mac):
        raise FlashError("BADGE application handoff reached a different ESP32")
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    if lines.count(_FORCE_DOWNLOAD_CLEAR) != 1:
        raise FlashError(
            "BADGE application handoff did not prove force-download clear"
        )
    if lines.count(_WATCHDOG_RESET) != 1 or lines[-1] != _WATCHDOG_RESET:
        raise FlashError(
            "BADGE application handoff did not prove watchdog reset"
        )
    if lines.index(_FORCE_DOWNLOAD_CLEAR) >= lines.index(_WATCHDOG_RESET):
        raise FlashError("BADGE application handoff receipts are out of order")
```

Add the engine method:

```python
def handoff_to_application(self, device: UsbDevice) -> None:
    output = self._run([
        _python(), "-m", "esptool", "--chip", "esp32s3",
        "--port", device.port, "--baud", "115200",
        "--before", "no_reset", "--after", "watchdog_reset",
        "--no-stub", "--connect-attempts", "1",
        "write_mem", "0x6000812c", "0x0", "0x1",
    ])
    _verify_handoff_receipt(output, device.mac)
```

- [ ] **Step 4: Run focused and full flash tests**

Run:

```bash
python3 -m unittest tools.badge_flasher.tests.test_flash -v
```

Expected: all flash tests pass.

- [ ] **Step 5: Commit**

```bash
git add tools/badge_flasher/flash.py \
  tools/badge_flasher/tests/test_flash.py
git commit -m "factory: add proven ROM app handoff"
```

---

### Task 2: One-Plug Handoff Sequencing and Truthful Seed Errors

**Files:**
- Modify: `tools/badge_flasher/cli.py`
- Modify: `tools/badge_flasher/verify.py`
- Test: `tools/badge_flasher/tests/test_cli.py`
- Test: `tools/badge_flasher/tests/test_verify.py`

**Interfaces:**
- Consumes: `FlashEngine.handoff_to_application(device)`.
- Produces: `_handoff_factory_graph(engine, devices, assignment, plain) -> None`.
- Preserves: `provision_game_seed(...) -> SeedRebootProof`.

- [ ] **Step 1: Write failing sequence and bounded-retry tests**

Create a behavioral CLI helper test using three `UsbDevice` values:

```python
def test_factory_handoff_boots_scanners_before_uplink(self):
    calls = []
    cli._handoff_factory_graph(
        SimpleNamespace(
            handoff_to_application=lambda device: calls.append(device.mac)
        ),
        {
            ASSIGNMENT.uplink_mac: UPLINK,
            ASSIGNMENT.ble_leaf_mac: BLE,
            ASSIGNMENT.wifi_leaf_mac: WIFI,
        },
        ASSIGNMENT,
        plain=True,
    )
    self.assertEqual(
        calls,
        [
            ASSIGNMENT.ble_leaf_mac,
            ASSIGNMENT.wifi_leaf_mac,
            ASSIGNMENT.uplink_mac,
        ],
    )
```

Add a `run_one` test whose first `provision_game_seed` call raises
`VerificationError("game seed provisioning timed out: uplink silent")`.
Require exactly one new ROM rebind/handoff and one second seed call. Add a
second case where both seed calls fail; require exactly two calls and no
runtime verification.

- [ ] **Step 2: Write a failing diagnostic-priority test**

Build a descriptor-bound factory where the matching uplink opens but
`_query_fresh_status` times out, while later scanner candidates raise
`candidate is not the descriptor-bound uplink`.

```python
with self.assertRaisesRegex(
    VerificationError,
    "uplink pre-seed identity timed out waiting for fresh status",
):
    verify.provision_game_seed(
        UPLINK,
        "immune",
        VERSION,
        timeout_s=0.1,
        serial_factory=open_candidate,
        candidate_ports=lambda: [
            UPLINK.port, "/dev/cu.scanner-a", "/dev/cu.scanner-b",
        ],
    )
```

- [ ] **Step 3: Run the new tests and verify RED**

Run:

```bash
python3 -m unittest \
  tools.badge_flasher.tests.test_cli.CliSequenceTests.test_factory_handoff_boots_scanners_before_uplink \
  tools.badge_flasher.tests.test_cli.CliSequenceTests.test_seed_timeout_rebinds_and_handoffs_once_without_reflash \
  tools.badge_flasher.tests.test_cli.CliSequenceTests.test_second_seed_timeout_stops_without_runtime_gate \
  tools.badge_flasher.tests.test_verify.VerifyTests.test_seed_timeout_preserves_matching_uplink_error \
  -v
```

Expected: failures because the helper, retry, and error priority are absent.

- [ ] **Step 4: Implement handoff orchestration**

Add:

```python
def _handoff_factory_graph(
    engine: FlashEngine,
    devices: dict[str, UsbDevice],
    assignment: TopologyAssignment,
    plain: bool,
) -> None:
    for mac, label in (
        (assignment.ble_leaf_mac, "BLE-SCANNER"),
        (assignment.wifi_leaf_mac, "WIFI-SCANNER"),
        (assignment.uplink_mac, "UPLINK"),
    ):
        phase("BOOT", f"{label} force-clear/watchdog handoff", plain)
        engine.handoff_to_application(devices[mac])
```

Replace the three final `usb_jtag_app_reset` calls with this helper. Keep
`usb_jtag_app_reset` for disposable probe firmware only.

Wrap only the pre-seed timeout:

```python
try:
    reboot_proof = provision()
except VerificationError as first:
    if not str(first).startswith("game seed provisioning timed out:"):
        raise
    phase("RETRY", "UPLINK non-writing ROM/application handoff", plain)
    retry_rom = backend.rebind(set(devices), timeout_s=30)
    _handoff_factory_graph(engine, retry_rom, assignment, plain)
    reboot_proof = provision()
```

Do not repeat erase, write, verify, topology probe, or retry a firmware
rejection/identity mismatch.

- [ ] **Step 5: Preserve the matching-uplink error**

In `provision_game_seed`, track `matching_error` separately. Set it only after
the descriptor-bound opener returned a handle for the expected uplink. At the
deadline use it in preference to later wrong-port errors:

```python
matching_error: str | None = None
...
handle = _open_native_console(port, open_serial)
opened_expected = True
...
except (OSError, _RetryableApplicationPort) as exc:
    if opened_expected:
        matching_error = str(exc)
    else:
        last_error = str(exc)
...
detail = matching_error or last_error
raise VerificationError(f"game seed provisioning timed out: {detail}")
```

- [ ] **Step 6: Run focused and complete factory tests**

Run:

```bash
python3 -m unittest \
  tools.badge_flasher.tests.test_cli \
  tools.badge_flasher.tests.test_verify \
  -v
python3 -m unittest discover -s tools/badge_flasher/tests -v
```

Expected: all tests pass.

- [ ] **Step 7: Commit**

```bash
git add tools/badge_flasher/cli.py \
  tools/badge_flasher/verify.py \
  tools/badge_flasher/tests/test_cli.py \
  tools/badge_flasher/tests/test_verify.py
git commit -m "factory: recover one-plug app handoff"
```

---

### Task 3: Exact Prior-PASS Role-Only Reassignment

**Files:**
- Modify: `tools/badge_flasher/models.py`
- Modify: `tools/badge_flasher/records.py`
- Modify: `tools/badge_flasher/cli.py`
- Modify: `tools/badge_flasher/verify.py`
- Test: `tools/badge_flasher/tests/test_records.py`
- Test: `tools/badge_flasher/tests/test_cli.py`
- Test: `tools/badge_flasher/tests/test_verify.py`

**Interfaces:**
- Produces: `PassedFactoryRecord(version, bundle_sha256, assignment, game_seed)`.
- Produces: `ManufacturingLedger.passed_records() -> tuple[PassedFactoryRecord, ...]`.
- Produces: `verify_preseed_runtime(status, assignment, version, expected_target)`.
- Extends: `run_one(..., passed_records: tuple[PassedFactoryRecord, ...])`.
- Produces: a `BatchResult` with `phase="reassign"` and `devices=()`.

- [ ] **Step 1: Write failing strict-ledger tests**

Add an exact PASS JSONL fixture and assert:

```python
records = ManufacturingLedger(root).passed_records()
self.assertEqual(records, (
    PassedFactoryRecord(
        version="0.67.2-badge-defcon34",
        bundle_sha256="a" * 64,
        assignment=TopologyAssignment(
            "AA:BB:CC:DD:EE:01",
            "AA:BB:CC:DD:EE:02",
            "AA:BB:CC:DD:EE:03",
        ),
        game_seed="normal",
    ),
))
```

Add malformed PASS, partial-assignment, and duplicate-conflicting PASS fixtures
that must raise `LedgerError`; failed rows remain ignored.

- [ ] **Step 2: Write failing interactive reassignment tests**

Add cases proving:

- an exact trio/version/hash prompts `ALREADY PASSED`;
- `Y` calls no topology discovery and no `flash_and_verify`;
- the selected role is sent even when it equals the prior role;
- `N` produces `CANCELLED`, no failure row, and no mutation;
- partial/conflicting/stale-version matches fail closed;
- `--yes` cannot silently reassign;
- `--allow-rework` retains full verified-reflash behavior;
- success prints `REASSIGNED`, stores `phase="reassign"`, and `devices=[]`.

Use real `ManufacturingLedger` files and mock only hardware boundaries.

- [ ] **Step 3: Write a failing pre-seed runtime test**

Start from `valid_status()`, vary the prior seed freely, and require exact
identity, version, target, scanner assignments, profiles, radio health,
rollback state, USB/UART health, PSRAM, and safe/recovery state. Do not require
the newly selected role or a new expected-reboot generation before mutation.

```python
checked = verify.verify_preseed_runtime(
    valid_status(),
    ASSIGNMENT,
    VERSION,
    expected_target=verify.UPLINK_TARGET,
)
self.assertEqual(checked["hardware_id"], ASSIGNMENT.uplink_mac)
```

- [ ] **Step 4: Run the new tests and verify RED**

Run:

```bash
python3 -m unittest \
  tools.badge_flasher.tests.test_records.LedgerTests.test_passed_records_returns_exact_current_assignment \
  tools.badge_flasher.tests.test_records.LedgerTests.test_passed_records_rejects_malformed_or_conflicting_passes \
  tools.badge_flasher.tests.test_cli.CliSequenceTests.test_exact_pass_offers_role_only_reassignment \
  tools.badge_flasher.tests.test_cli.CliSequenceTests.test_role_only_reassignment_skips_all_flash_operations \
  tools.badge_flasher.tests.test_cli.CliSequenceTests.test_role_only_cancel_records_nothing \
  tools.badge_flasher.tests.test_cli.CliSequenceTests.test_role_only_requires_current_complete_pass \
  tools.badge_flasher.tests.test_cli.CliSequenceTests.test_noninteractive_passed_badge_fails_without_allow_rework \
  tools.badge_flasher.tests.test_cli.CliSequenceTests.test_allow_rework_retains_full_flash \
  tools.badge_flasher.tests.test_cli.CliSequenceTests.test_role_only_records_distinct_reassign_receipt \
  tools.badge_flasher.tests.test_verify.VerifyTests.test_preseed_runtime_ignores_old_role_but_requires_graph_health \
  -v
```

Expected: missing-symbol failures and the current hard failure on historical
MACs.

- [ ] **Step 5: Implement strict prior-PASS records**

Add:

```python
@dataclass(frozen=True, slots=True)
class PassedFactoryRecord:
    version: str
    bundle_sha256: str
    assignment: TopologyAssignment
    game_seed: str
```

Parse passed JSONL rows strictly. Normalize all assignment MACs, require three
distinct values, canonical version/hash/seed fields, and reject conflicting
records for the same intersecting hardware.

- [ ] **Step 6: Implement pre-seed verification**

Refactor the existing exact uplink/scanner checks into helpers shared by
`verify_status` and `verify_preseed_runtime`. Keep post-seed-only checks
(selected seed/state, shield zero, inactive game, expected reboot reason,
successor generation) exclusively in `verify_status`.

- [ ] **Step 7: Implement role-only reassignment**

After the initial ROM scan and before probe writes:

```python
prior = _resolve_prior_pass(
    set(devices),
    ledger_records,
    version=bundle.version,
    bundle_sha256=bundle.bundle_sha256,
)
```

For an exact interactive match, show the live Y/N prompt. On `Y`:

1. use the prior assignment;
2. perform `_handoff_factory_graph`;
3. query and call `verify_preseed_runtime`;
4. call the existing `provision_game_seed`;
5. call `wait_for_runtime`;
6. return `BatchResult(..., phase="reassign", devices=())`.

Add a private cancellation exception preserved through
`_run_factory_operation`, and catch it in `main` without recording a FAIL.

- [ ] **Step 8: Run complete factory tests**

Run:

```bash
python3 -m unittest discover -s tools/badge_flasher/tests -v
python3 -m unittest scripts.test_build_badge_factory_bundle -v
```

Expected: all tests pass.

- [ ] **Step 9: Commit**

```bash
git add tools/badge_flasher/models.py \
  tools/badge_flasher/records.py \
  tools/badge_flasher/cli.py \
  tools/badge_flasher/verify.py \
  tools/badge_flasher/tests/test_records.py \
  tools/badge_flasher/tests/test_cli.py \
  tools/badge_flasher/tests/test_verify.py
git commit -m "factory: reassign exact passed badge roles"
```

---

### Task 4: Frozen-Artifact Verification and Physical Canaries

**Files:**
- Modify: `docs/badge-factory-flasher.md`
- Modify: `docs/badge/con-crud-canary-acceptance.md`

**Interfaces:**
- Consumes: the completed host-only implementation.
- Produces: retained test and physical evidence; no firmware artifacts.

- [ ] **Step 1: Run static and complete host verification**

```bash
git diff --check
python3 -m unittest discover -s tools/badge_flasher/tests -v
python3 -m unittest scripts.test_build_badge_factory_bundle -v
shasum -a 256 \
  tools/badge_flasher/resources/badge-factory-flasher-embedded.zip
```

Require embedded ZIP SHA:
`038d83adcc3e6a561a9192e8bed26ec205e7e7c9374eb6ff800baf573bb44576`.

- [ ] **Step 2: Run one attended new-badge canary**

Select HEALER or INFECTED, connect one complete three-board badge once, and
require live stages:

```text
PROBE -> GRAPH -> FLASH/VERIFY x3 -> BOOT x3 -> SEED -> HEALTH -> PASS
```

Do not unplug between FLASH and PASS. Retain the opaque receipt and verify the
private ledger contains all three exact MACs, `phase="complete"`, selected
seed, and healthy runtime.

- [ ] **Step 3: Run one attended role-only canary**

Run the same connected badge again without `--allow-rework`, choose a different
role, answer `Y` to `REASSIGN ROLE ONLY`, and require:

```text
IDENT -> ALREADY PASSED -> BOOT -> SEED -> HEALTH -> REASSIGNED
```

Require no `PROBE` or `FLASH` stage and a private `phase="reassign"` row with
`devices=[]`.

- [ ] **Step 4: Update operator and acceptance documentation**

Document:

- one-plug watchdog handoff;
- the one bounded non-writing recovery;
- exact prior-PASS Y/N reassignment;
- `--allow-rework` still means full reflash;
- distinct PASS versus REASSIGNED evidence.

- [ ] **Step 5: Commit final evidence**

```bash
git add docs/badge-factory-flasher.md \
  docs/badge/con-crud-canary-acceptance.md
git commit -m "docs: accept one-plug factory reassignment"
```
