# Badge Factory Batch Selection and Inventory Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the double-click factory flasher ask once which badge role is being produced, lock that role for the batch, and maintain a durable private three-MAC inventory for approximately 45 badges.

**Architecture:** Add a fail-closed startup batch selection before hardware mutation, carry one batch ID/type through every result, retain the append-only JSONL as the event source, and atomically rebuild a human-readable inventory after each successful ledger record. Package the exact physically accepted private game artifacts only after the firmware plan passes.

**Tech Stack:** Python 3 standard library, `unittest`, existing esptool/device backend, JSONL/CSV manufacturing ledger, atomic POSIX file replacement, and the existing reproducible factory ZIP builder.

## Global Constraints

- Begin only after every gate in `2026-07-27-con-crud-property-game-firmware.md` passes for the exact candidate hashes.
- Work only in `/Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final`.
- Keep all work local and private. Do not push, tag, release, merge, or fetch/publish a secret candidate through GitHub.
- Keep topology discovery and flash order unchanged: disposable probe, BLE scanner, Wi-Fi scanner, uplink; only the proven uplink receives the game seed.
- A normal interactive run has no default badge type and prompts exactly once before any USB probe or erase.
- `--game-role` remains available only for explicit scripted/test operation.
- `--yes` without `--game-role` fails before hardware mutation.
- Raw MAC addresses remain in private ledger/inventory only and never enter terminal output.
- Ledger or inventory failure prevents the terminal from printing PASS or updating in-memory passed-MAC state.
- Preserve existing inventory colors during rebuild; color remains metadata and does not affect firmware, seed, or PASS.
- Commit each green task locally with the specified subject.

## File Structure

### Modify

- `tools/badge_flasher/models.py`
  - Add immutable batch identity to a flash result.
- `tools/badge_flasher/cli.py`
  - Prompt once, confirm, lock batch role/ID, record results, and suppress PASS on inventory failure.
- `tools/badge_flasher/records.py`
  - Add batch fields to authoritative JSONL/CSV events.
- `tools/badge_flasher/tests/test_cli.py`
- `tools/badge_flasher/tests/test_records.py`
- `tools/badge_flasher/tests/test_redaction.py`
  - Cover startup ordering, locking, failure behavior, and private/public boundaries.
- `scripts/build_badge_factory_bundle.py`
- `scripts/test_build_badge_factory_bundle.py`
  - Allow an explicit private-game artifact source without changing the default production builder.
- `docs/badge/README.md`
- `docs/badge-factory-flasher.md`
  - Document the interactive batch and private inventory workflow.
- `flash-badges.command`
  - Retain the no-CLI double-click entry and document the new in-app prompt.
- `tools/badge_flasher/resources/badge-factory-flasher-embedded.zip`
  - Replace only after exact firmware acceptance and one local bundle canary.

### Create

- `tools/badge_flasher/inventory.py`
  - Parse authoritative events, preserve annotations, and atomically write schema version 1.
- `tools/badge_flasher/tests/test_inventory.py`
  - Cover grouping, history, color preservation, atomicity, and failures.

---

## Task 1: Add One Fail-Closed Interactive Batch Selection

**Files:**

- Modify: `tools/badge_flasher/cli.py`
- Test: `tools/badge_flasher/tests/test_cli.py`

- [ ] Change parser tests to require `game_role is None` when no option is supplied. Retain exact accepted scripted values `normal`, `infected`, and `immune`; continue rejecting every other value.
- [ ] Add failing tests for the startup menu:

```text
WHAT TYPE OF BADGE BATCH ARE WE FLASHING?
  1 - HUMAN
  2 - INFECTED
  3 - IMMUNE / HEALER
```

- [ ] Cover selection by number, explicit confirmation, invalid input retry, declined confirmation returning to selection, EOF, KeyboardInterrupt, and `--yes` without explicit role.
- [ ] Add a failing ordering test using mocks that proves role confirmation occurs before `choose_bundle`, device backend construction, candidate-port listing, probe, erase, or write.
- [ ] Add a failing repeat-loop test proving the prompt executes once and the same selected role is passed to every `run_one`.
- [ ] Run and confirm failure:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python3 -m unittest tools.badge_flasher.tests.test_cli -v
```

- [ ] Change `--game-role` to `default=None`.
- [ ] Implement the exact interface `select_batch_role(explicit_role: str | None, *, automated: bool, prompt: Callable[[str], str] = _prompt_operator) -> str`.

- [ ] If `explicit_role` is supplied, validate and return it without a prompt. If `automated` is true and no role is supplied, raise `_FactoryArgumentError` before any hardware or bundle work.
- [ ] For the interactive path, map `1/2/3` to `normal/infected/immune`, print the human-readable selection, and require an exact `Y` confirmation. EOF and interruption exit without mutation or PASS.
- [ ] Assign the returned value to one session-local `batch_role` before bundle selection and the repeat loop. Pass it explicitly to `run_one`; do not mutate/reparse it per badge.
- [ ] Keep `flash-badges.command` argument-free.
- [ ] Run the CLI tests and confirm green.
- [ ] Commit:

```sh
git add tools/badge_flasher/cli.py tools/badge_flasher/tests/test_cli.py flash-badges.command
git commit -m "factory: prompt once for badge batch role"
```

## Task 2: Carry a Stable Batch Identifier Through Every Event

**Files:**

- Modify: `tools/badge_flasher/models.py`
- Modify: `tools/badge_flasher/cli.py`
- Modify: `tools/badge_flasher/records.py`
- Test: `tools/badge_flasher/tests/test_cli.py`
- Test: `tools/badge_flasher/tests/test_records.py`
- Test: `tools/badge_flasher/tests/test_redaction.py`

- [ ] Add failing tests that one process creates exactly one opaque batch ID, reuses it for every PASS/FAIL, and a new process gets a different ID.
- [ ] Require the format `batch_YYYYMMDDTHHMMSSZ_{random4}`, where `{random4}` is four characters from the existing Crockford alphabet generated with `secrets`.
- [ ] Add `batch_id: str` and `batch_type: str` as required `BatchResult` fields. Update every constructor and fixture found by:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
rg -n 'BatchResult\(' tools scripts
```

- [ ] Add batch fields to PASS and FAIL JSONL payloads and CSV columns. Keep `game_seed` for backward compatibility and assert `batch_type == game_seed`.
- [ ] Add tests proving batch ID, badge ID, bundle hash, and MACs remain absent from public terminal output; only selected role and opaque receipt are printed.
- [ ] Run all factory-flasher tests:

```sh
python3 -m unittest discover -s tools/badge_flasher/tests -v
```

- [ ] Commit:

```sh
git add tools/badge_flasher/models.py tools/badge_flasher/cli.py tools/badge_flasher/records.py tools/badge_flasher/tests
git commit -m "factory: bind manufacturing events to one batch"
```

## Task 3: Build the Versioned Private Inventory

**Files:**

- Create: `tools/badge_flasher/inventory.py`
- Create: `tools/badge_flasher/tests/test_inventory.py`

- [ ] Write failing tests from synthetic JSONL for:
  - one badge with all three exact MACs;
  - multiple badges sorted by badge ID;
  - repeated/rework PASS events grouped by immutable uplink MAC;
  - current assignment taken from the newest successful event;
  - full successful assignment history retained oldest to newest;
  - receipt, badge ID, batch, role, version, hash, and all three MACs;
  - `color: null` for a new badge;
  - preservation of an existing string color annotation by uplink MAC;
  - malformed/truncated JSONL causing failure rather than partial output;
  - invalid or duplicate physical assignments causing failure;
  - no temporary file remaining after success or handled failure.
- [ ] Define schema version 1 exactly:

```json
{
  "schema_version": 1,
  "generated_at": "UTC ISO-8601",
  "badges": [
    {
      "badge_id": "D3E4F5",
      "receipt": "rcpt_K7M2Q9W4",
      "batch_id": "batch_20260727T120000Z_ABCD",
      "batch_type": "immune",
      "uplink_mac": "AA:BB:CC:DD:EE:01",
      "ble_scanner_mac": "AA:BB:CC:DD:EE:02",
      "wifi_scanner_mac": "AA:BB:CC:DD:EE:03",
      "firmware_version": "0.64.90-badge-defcon34",
      "bundle_sha256": "64 lowercase hexadecimal characters",
      "current_assignment": {
        "timestamp": "UTC ISO-8601",
        "batch_id": "batch_20260727T120000Z_ABCD",
        "batch_type": "immune",
        "receipt": "rcpt_K7M2Q9W4"
      },
      "history": [],
      "color": null
    }
  ]
}
```

- [ ] Each `history` entry contains timestamp, batch ID/type, receipt, firmware version, bundle SHA-256, and the three-board assignment. It contains no runtime dump.
- [ ] Implement `InventoryError(RuntimeError)` and `BadgeInventory(directory: Path)` with a public `rebuild_from_ledger() -> None` method.

- [ ] Parse only `passed is true` events with a complete assignment. Treat the append-only `badge-factory.jsonl` as authoritative.
- [ ] If an existing inventory has schema version 1, preserve each `color` value when it is `None` or a string, keyed by uppercase uplink MAC. Reject malformed annotations rather than silently deleting them.
- [ ] Write `badge-inventory.json.tmp` in the same directory, flush and `os.fsync` the file, `os.replace` it onto `badge-inventory.json`, then open and `os.fsync` the directory. Clean up the temp file on exceptions.
- [ ] Run:

```sh
python3 -m unittest tools.badge_flasher.tests.test_inventory -v
```

- [ ] Commit:

```sh
git add tools/badge_flasher/inventory.py tools/badge_flasher/tests/test_inventory.py
git commit -m "factory: maintain atomic private badge inventory"
```

## Task 4: Integrate Ledger, Inventory, and PASS Suppression

**Files:**

- Modify: `tools/badge_flasher/cli.py`
- Modify: `tools/badge_flasher/records.py`
- Modify: `tools/badge_flasher/inventory.py`
- Test: `tools/badge_flasher/tests/test_cli.py`
- Test: `tools/badge_flasher/tests/test_records.py`
- Test: `tools/badge_flasher/tests/test_inventory.py`
- Test: `tools/badge_flasher/tests/test_redaction.py`

- [ ] Add failing integration tests proving the success order is:
  1. guarded flash and runtime proof;
  2. fsync JSONL/CSV event;
  3. atomic inventory rebuild;
  4. update process-local passed MACs;
  5. print PASS.
- [ ] Add failing tests for JSONL failure, CSV failure, inventory parse failure, inventory file fsync failure, replace failure, and directory fsync failure. No case may print PASS or add MACs to the process-local passed set.
- [ ] Add a test proving an inventory failure after a durable JSONL event requires explicit `--allow-rework` on retry and retains history rather than erasing the first event.
- [ ] Instantiate `BadgeInventory(args.records)` beside `ManufacturingLedger`.
- [ ] On a successful `run_one`, call `ledger.record(result)` and then `inventory.rebuild_from_ledger()` inside the scrubbed operation boundary. Move `known_passed_macs.update` and PASS output after both calls.
- [ ] On a flash/provision/health failure, call `record_failure` with the locked batch ID/type. Do not rebuild inventory for an unassigned failure.
- [ ] Scrub exception text at the existing public boundary. Keep raw private identities only in durable records.
- [ ] Run:

```sh
python3 -m unittest discover -s tools/badge_flasher/tests -v
python3 -m unittest scripts.test_user_visible_redaction -v
```

- [ ] Commit:

```sh
git add tools/badge_flasher/cli.py tools/badge_flasher/records.py tools/badge_flasher/inventory.py tools/badge_flasher/tests scripts/test_user_visible_redaction.py
git commit -m "factory: require durable inventory before PASS"
```

## Task 5: Package the Exact Accepted Private Candidate

**Prerequisite:** The firmware acceptance ledger names exact accepted `.90` uplink/scanner artifact hashes and every physical gate is PASS.

**Files:**

- Modify: `scripts/build_badge_factory_bundle.py`
- Modify: `scripts/test_build_badge_factory_bundle.py`

- [ ] Add failing builder tests proving the default still reads production badge build directories and an explicit private-game switch reads:
  - `esp32/uplink/.pio/build/uplink-s3-fof_badge-con-crud-canary`
  - `esp32/scanner/.pio/build/scanner-s3-combo-fof_badge-con-crud-canary`
- [ ] Add failing tests proving the private-game bundle refuses mismatched versions, mismatched manifest hashes, non-canary identities, an unaccepted manifest, missing physical evidence, or absent probe artifacts.
- [ ] Add arguments:

```text
--private-game-canary
--acceptance-manifest <path>
```

- [ ] Require `--acceptance-manifest` with `--private-game-canary`. Validate schema version 1, `"physically_accepted": true`, candidate version, nonempty physical evidence, and exact 64-character lowercase uplink/scanner SHA-256 values. Read the binaries once, compare exact hashes, and package those exact bytes.
- [ ] Keep the default production builder unchanged. Do not change `FOF_VERSION_BADGE`, production environments, GitHub release discovery, or public release assets.
- [ ] Build the factory probe if its accepted artifact is not already present:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32/factory-probe
/Users/billh/.platformio/penv/bin/pio run -e factory-probe-s3
```

- [ ] Run builder tests:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python3 -m unittest scripts.test_build_badge_factory_bundle -v
```

- [ ] Build a local candidate ZIP using the exact accepted hashes:

```sh
python3 scripts/build_badge_factory_bundle.py \
  --private-game-canary \
  --acceptance-manifest docs/badge/con-crud-0.64.90-acceptance.json \
  --version 0.64.90-badge-defcon34 \
  --output /private/tmp/fof-badge-factory-0.64.90-private.zip
```

- [ ] Validate the ZIP with `load_bundle`, compare its embedded app hashes to the ledger, and keep it outside the embedded resource until Task 6 passes.
- [ ] Commit:

```sh
git add scripts/build_badge_factory_bundle.py scripts/test_build_badge_factory_bundle.py
git commit -m "factory: build exact accepted private game bundle"
```

## Task 6: Run One End-to-End Factory Canary

**Files:**

- Modify: `docs/badge/README.md`
- Modify: `docs/badge-factory-flasher.md`
- Modify: `docs/badge/con-crud-canary-acceptance.md`
- Use private local records under `~/Documents/FoF Badge Factory`

- [ ] Start with a complete known test badge connected by all three USB cables.
- [ ] Run the flasher with the local accepted bundle and no role option:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python3 scripts/fof_badge_factory.py \
  --bundle /private/tmp/fof-badge-factory-0.64.90-private.zip \
  --offline \
  --once
```

- [ ] Select each role in a separate bounded dry/mocked run and select one intended role for the real flash. Prove the role prompt/confirmation appears before USB probing.
- [ ] Require the normal factory graph proof, erase/write/readback of both scanners and uplink, exact seed provisioning, post-reboot inactive/zero game state, exact `.90` on all three boards, scanner roles/radios, USB/UART/rollback/health proof, durable ledger, and atomic inventory.
- [ ] Inspect private `badge-factory.jsonl`, `badge-factory.csv`, and `badge-inventory.json`. Require all three raw MACs, badge/receipt, batch ID/type, exact version/hash, complete assignment/history, and null or preserved color.
- [ ] Induce one bounded inventory write failure in a temporary records directory and prove the terminal cannot print PASS.
- [ ] Confirm the public transcript contains no raw MAC, badge ID, bundle hash, filesystem secret, or exception identity.
- [ ] Record exact evidence in the acceptance ledger.
- [ ] Commit:

```sh
git add docs/badge/README.md docs/badge-factory-flasher.md docs/badge/con-crud-canary-acceptance.md
git commit -m "factory: accept private game batch workflow"
```

## Task 7: Embed the Accepted Bundle and Run the Final Private Gate

**Files:**

- Modify: `tools/badge_flasher/resources/badge-factory-flasher-embedded.zip`
- Modify: `docs/badge/README.md`
- Modify: `docs/badge-factory-flasher.md`
- Modify: `docs/badge/con-crud-canary-acceptance.md`

- [ ] Copy the byte-for-byte accepted local ZIP into the embedded resource only after Task 6 passes.
- [ ] Re-run:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python3 -m unittest discover -s tools/badge_flasher/tests -v
python3 -m unittest scripts.test_build_badge_factory_bundle -v
python3 -m unittest scripts.test_fof_badge_flash -v
python3 -m unittest scripts.test_user_visible_redaction -v
```

- [ ] Launch `flash-badges.command` without command-line arguments. Verify the BBS banner, role selection, confirmation, locked batch display, and plug-badge prompt.
- [ ] Run one embedded-resource canary on a complete test badge and verify the exact same hashes and private inventory behavior as Task 6.
- [ ] Update documentation with:
  - interactive three-role flow;
  - batch lock semantics;
  - private inventory location and schema;
  - explicit rework behavior;
  - later manual `color` annotation preservation;
  - no public GitHub release before DEF CON.
- [ ] Commit the exact private bundle and docs locally:

```sh
git add tools/badge_flasher/resources/badge-factory-flasher-embedded.zip docs/badge/README.md docs/badge-factory-flasher.md docs/badge/con-crud-canary-acceptance.md
git commit -m "v0.64.90: seal private DEF CON factory bundle"
```

## Task 8: Final Manufacturing Readiness Review

- [ ] Inspect the entire branch diff from design commit `286e588`.
- [ ] Confirm the embedded uplink/scanner binary hashes match the exact physically accepted artifacts.
- [ ] Confirm the factory probe remains the accepted topology-only artifact.
- [ ] Confirm the role is selected once, inventory is private and atomic, public output is redacted, and a ledger/inventory failure cannot print PASS.
- [ ] Confirm there was no public push, tag, release, merge, or Theme v2 scope expansion.
- [ ] Flash a small property-test batch before starting the remaining approximately 45 badges. Stop the batch if live RF balance, game behavior, scanning, USB, or UART updating differs from the accepted bench evidence.
