# FoF Badge Factory Flasher Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship an MIT-licensed macOS production tool that identifies one blank uplink and two blank scanner ESP32-S3 boards from their assembled UART topology, selects a validated embedded or newer GitHub firmware bundle, flashes all three over USB, and records a verified PASS only after full runtime health proof.

**Architecture:** A tiny shared factory-probe image runs on all three blank boards and reports a nonce-bound, CRC-protected two-link peer graph. A Python host package owns bundle validation, GitHub selection, MAC-stable USB enumeration, manifest-driven esptool operations, runtime verification, records, and an ANSI/plain terminal interface. Pure policy modules are tested without hardware; USB, serial, esptool, and GitHub are injected boundaries; the final checkpoint uses the real badge trio.

**Tech Stack:** Python 3.11 standard library, pyserial, esptool 4.11, PlatformIO/ESP-IDF 5.5, C11, Unity native firmware tests, unittest/pytest, GitHub Actions, JSON/CSV/JSONL.

## Global Constraints

- First supported host is macOS; core policy remains portable.
- The operator plugs exactly one complete three-board badge into USB at a time.
- Blank-board classification uses reciprocal UART topology, never USB port order.
- Production flashing is forbidden unless the graph has exactly one two-peer center and two one-peer leaves.
- The center receives `uplink-s3-fof_badge`; both leaves receive `scanner-s3-combo-fof_badge`.
- Firmware transport is USB/UART only; no Wi-Fi, HTTP, BLE, or Android flashing path is added.
- Embedded firmware remains the offline-safe fallback; a GitHub bundle is selected only when complete, strictly newer, and fully validated.
- Every device is tracked by ESP32 base MAC across USB renumbering.
- Production writes are sequential; read-only discovery may be parallel.
- PASS requires region readback plus production identity and uplink `FOF_STATUS` health proof.
- Existing badge firmware behavior and unrelated dirty-worktree changes must not be modified.
- All new source remains under the repository's existing MIT License.
- No Nintendo artwork, sprites, text, or music is copied into the ANSI intro.

---

### Task 1: Pure topology protocol and graph validator

**Files:**
- Create: `tools/badge_flasher/__init__.py`
- Create: `tools/badge_flasher/models.py`
- Create: `tools/badge_flasher/topology.py`
- Create: `tools/badge_flasher/tests/__init__.py`
- Create: `tools/badge_flasher/tests/test_topology.py`

**Interfaces:**
- Produces: `ProbeReport(mac: str, session: str, peers: Mapping[str, str])`.
- Produces: `TopologyAssignment(uplink_mac: str, ble_leaf_mac: str, wifi_leaf_mac: str)`.
- Produces: `parse_probe_report(line: str, expected_session: str) -> ProbeReport`.
- Produces: `classify_topology(reports: Iterable[ProbeReport]) -> TopologyAssignment`.
- Errors: `TopologyError` with operator-safe diagnostics.

- [ ] **Step 1: Write failing graph and parser tests**

```python
def test_classifies_reciprocal_badge_star():
    reports = [
        ProbeReport("aa", "nonce", {"a": "bb", "b": "cc"}),
        ProbeReport("bb", "nonce", {"a": "aa"}),
        ProbeReport("cc", "nonce", {"a": "aa"}),
    ]
    assert classify_topology(reports) == TopologyAssignment("aa", "bb", "cc")

def test_rejects_partial_or_nonreciprocal_star():
    with pytest.raises(TopologyError, match="reciprocal"):
        classify_topology([
            ProbeReport("aa", "nonce", {"a": "bb", "b": "cc"}),
            ProbeReport("bb", "nonce", {}),
            ProbeReport("cc", "nonce", {"a": "aa"}),
        ])
```

- [ ] **Step 2: Run tests and verify RED**

Run: `python3 -m unittest discover -s tools/badge_flasher/tests -p 'test_topology.py' -v`

Expected: import failure because `models.py` and `topology.py` do not exist.

- [ ] **Step 3: Implement immutable models, strict JSON-line parsing, MAC normalization, session validation, peer deduplication, and reciprocal-star classification**

The accepted wire prefix is `FOF_FACTORY_PROBE:` followed by one JSON object:

```json
{"schema":1,"session":"32hex","mac":"E0:72:A1:00:00:01","peers":{"a":"E0:72:A1:00:00:02","b":"E0:72:A1:00:00:03"},"crc32":"89abcdef"}
```

CRC covers canonical compact JSON excluding `crc32`. Link A on the center maps
to the BLE physical slot; link B maps to Wi-Fi. Leaf reports must reciprocate on
their link A.

- [ ] **Step 4: Run topology tests and verify GREEN**

Run: `python3 -m unittest discover -s tools/badge_flasher/tests -p 'test_topology.py' -v`

Expected: all topology tests pass.

- [ ] **Step 5: Commit**

```bash
git add tools/badge_flasher
git commit -m "badge-flasher: validate factory topology"
```

---

### Task 2: Factory-probe firmware and native protocol tests

**Files:**
- Create: `esp32/factory-probe/platformio.ini`
- Create: `esp32/factory-probe/CMakeLists.txt`
- Create: `esp32/factory-probe/sdkconfig.defaults`
- Create: `esp32/factory-probe/main/CMakeLists.txt`
- Create: `esp32/factory-probe/main/main.c`
- Create: `esp32/shared/factory_probe_protocol.h`
- Create: `esp32/shared/factory_probe_protocol.c`
- Create: `esp32/test/test_factory_probe_protocol.c`
- Modify: `esp32/test/test_runner.c`
- Modify: `esp32/platformio.ini`

**Interfaces:**
- Consumes: UART links A `(TX=1,RX=2)` and B `(TX=3,RX=4)`.
- Produces: `fof_factory_probe_encode_frame`, `fof_factory_probe_parse_frame`, and bounded peer-table helpers.
- Produces: USB commands `FOF_PROBE_SESSION:<32hex>` and `FOF_PROBE_REPORT`.
- Produces: USB response prefix `FOF_FACTORY_PROBE:` compatible with Task 1.

- [ ] **Step 1: Add failing C tests to the native runner**

Tests cover canonical frames, CRC corruption, stale nonce, invalid MAC, self
peer, duplicate peer, link bounds, retry cap, and stable report construction.

- [ ] **Step 2: Run native tests and verify RED**

Run: `/Users/billh/.platformio/penv/bin/pio test -e test -f test_factory_probe_protocol`

Expected: compilation fails because the protocol module is absent.

- [ ] **Step 3: Implement the pure C protocol module**

Use fixed-size structs and buffers only. Frames are newline-delimited ASCII,
bounded below 256 bytes, and include schema, session, sender MAC, link, sequence,
and CRC32. No radio, NVS, heap allocation, or production headers are required.

- [ ] **Step 4: Implement the ESP-IDF probe application**

Initialize native USB console, derive the base MAC from eFuse, configure UART1
for link A and UART2 for link B at 115200 baud, wait for a host session nonce,
broadcast bounded challenges, collect distinct peers for three seconds, and
print a report on request. A GPIO link with no reciprocal response remains
absent rather than guessed.

- [ ] **Step 5: Run native tests and build the probe**

Run: `/Users/billh/.platformio/penv/bin/pio test -e test`

Expected: all existing 467 tests plus new probe tests pass.

Run: `/Users/billh/.platformio/penv/bin/pio run -e factory-probe-s3`

Expected: `SUCCESS` and probe binaries under
`esp32/factory-probe/.pio/build/factory-probe-s3/`.

- [ ] **Step 6: Commit**

```bash
git add esp32/factory-probe esp32/shared/factory_probe_protocol.* esp32/test esp32/platformio.ini
git commit -m "badge-flasher: add UART topology probe"
```

---

### Task 3: macOS device discovery and MAC-stable rebinding

**Files:**
- Create: `tools/badge_flasher/devices.py`
- Create: `tools/badge_flasher/tests/test_devices.py`

**Interfaces:**
- Produces: `UsbDevice(mac, port, chip, revision, flash_size, psram_size, location_id)`.
- Produces: `DeviceBackend.list_candidate_ports() -> list[str]`.
- Produces: `DeviceBackend.probe_rom(port: str) -> UsbDevice`.
- Produces: `DeviceBackend.rebind(expected_macs: set[str], timeout_s: float) -> dict[str, UsbDevice]`.
- Consumes: injectable command runner, clock, globber, and IORegistry reader.

- [ ] **Step 1: Write failing discovery/rebinding tests**

Tests cover exactly three S3s, unrelated serial devices, duplicate MACs,
port-name swaps after reset, one disappearing MAC, unexpected fourth S3, and
stable macOS location identifiers.

- [ ] **Step 2: Run tests and verify RED**

Run: `python3 -m unittest tools.badge_flasher.tests.test_devices -v`

Expected: import failure for `devices`.

- [ ] **Step 3: Implement esptool JSON/text parsing and fail-closed enumeration**

Use `/dev/cu.usbmodem*`, `/dev/cu.usbserial*`, `/dev/cu.wchusbserial*`, and
`/dev/cu.SLAB*`; resolve esptool through the current interpreter or PlatformIO
environment; normalize MACs; and never use a port name as persistent identity.

- [ ] **Step 4: Run tests and verify GREEN**

Run: `python3 -m unittest tools.badge_flasher.tests.test_devices -v`

Expected: all device tests pass.

- [ ] **Step 5: Commit**

```bash
git add tools/badge_flasher/devices.py tools/badge_flasher/tests/test_devices.py
git commit -m "badge-flasher: bind USB ports by chip MAC"
```

---

### Task 4: Factory bundle schema, embedded fallback, and GitHub selection

**Files:**
- Create: `tools/badge_flasher/bundles.py`
- Create: `tools/badge_flasher/tests/test_bundles.py`
- Create: `tools/badge_flasher/resources/.gitkeep`
- Create: `scripts/build_badge_factory_bundle.py`
- Create: `scripts/test_build_badge_factory_bundle.py`
- Modify: `.github/workflows/esp32-web-flasher.yml`

**Interfaces:**
- Produces: `FactoryBundle(manifest, root, source, bundle_sha256)`.
- Produces: `load_bundle(path: Path) -> FactoryBundle`.
- Produces: `select_bundle(embedded, releases, offline=False) -> FactoryBundle`.
- Produces: `build_badge_factory_bundle.py --output PATH --version VERSION`.
- Consumes: existing `esp32/scripts/firmware_version.py` identity parser.

- [ ] **Step 1: Write failing bundle policy tests**

Tests require exact schema, supported flasher floor, unique offsets, known
targets, matching app descriptors, matching target markers, SHA-256/size,
strictly ordered versions, complete releases, offline fallback, and rejection
of Android-only, draft, corrupt, mixed-version, or downgrade candidates.

- [ ] **Step 2: Run tests and verify RED**

Run: `python3 -m unittest tools.badge_flasher.tests.test_bundles -v`

Expected: import failure for `bundles`.

- [ ] **Step 3: Implement safe archive extraction and complete validation**

Reject absolute paths, `..`, links, duplicate members, oversized archives, and
undeclared files. Validate every image before returning a bundle. GitHub lookup
uses public release JSON with a bounded timeout and chooses only an asset named
`badge-factory-flasher-<tag>.zip`.

- [ ] **Step 4: Implement reproducible bundle builder**

The builder copies probe/uplink/scanner bootloaders, partition tables, OTA data,
and apps at manifest offsets, derives exact embedded identities, writes sorted
canonical JSON, computes hashes, and creates a deterministic ZIP.

- [ ] **Step 5: Extend CI release packaging**

Build `factory-probe-s3`, invoke the builder after all badge builds, upload the
ZIP as an Actions artifact, and attach it to `v*` GitHub releases alongside the
existing badge assets.

- [ ] **Step 6: Run tests and a local bundle build**

Run: `python3 -m unittest tools.badge_flasher.tests.test_bundles scripts.test_build_badge_factory_bundle -v`

Run: `python3 scripts/build_badge_factory_bundle.py --output /tmp/fof-badge-factory.zip`

Expected: tests pass and `load_bundle` accepts the generated ZIP.

- [ ] **Step 7: Commit**

```bash
git add tools/badge_flasher/bundles.py tools/badge_flasher/tests/test_bundles.py tools/badge_flasher/resources scripts/build_badge_factory_bundle.py scripts/test_build_badge_factory_bundle.py .github/workflows/esp32-web-flasher.yml
git commit -m "badge-flasher: package validated firmware bundles"
```

---

### Task 5: Manifest-driven flash engine, runtime verifier, and records

**Files:**
- Create: `tools/badge_flasher/flash.py`
- Create: `tools/badge_flasher/verify.py`
- Create: `tools/badge_flasher/records.py`
- Create: `tools/badge_flasher/tests/test_flash.py`
- Create: `tools/badge_flasher/tests/test_verify.py`
- Create: `tools/badge_flasher/tests/test_records.py`

**Interfaces:**
- Produces: `FlashEngine.flash_layout(device, layout) -> FlashEvidence`.
- Produces: `FlashEngine.verify_layout(device, layout) -> FlashEvidence`.
- Produces: `verify_runtime_identities(...) -> RuntimeEvidence`.
- Produces: `verify_uplink_status(status, assignment, version) -> RuntimeEvidence`.
- Produces: `ManufacturingLedger.record(result: BatchResult) -> None`.

- [ ] **Step 1: Write failing flash command and runtime-gate tests**

Assert exact esptool offsets/files, sequential call ordering, digest verification,
MAC rebinding after reset, no write after topology failure, and PASS rejection
for wrong MAC, role, version, target, PSRAM, rollback, safe mode, UART health,
radio health, or missing scanner.

- [ ] **Step 2: Run tests and verify RED**

Run: `python3 -m unittest tools.badge_flasher.tests.test_flash tools.badge_flasher.tests.test_verify tools.badge_flasher.tests.test_records -v`

Expected: imports fail for the new modules.

- [ ] **Step 3: Implement the flash engine and verification gates**

Use one `write_flash` invocation per device with manifest offsets and one
`verify_flash` invocation per layout. Capture stdout/stderr, verify the esptool
MAC matches the intended device, and hard reset only after successful readback.
Reuse the existing bounded `FOF_PING`/`FOF_STATUS` framing behavior without
changing production firmware.

- [ ] **Step 4: Implement append-only records**

Write CSV with stable headers and JSONL with one canonical event per phase.
Use atomic flush/fsync for PASS records. Redact tokens, credentials, raw command
environment, and unrelated serial bytes. Warn on previously passed MACs.

- [ ] **Step 5: Run tests and verify GREEN**

Run: `python3 -m unittest discover -s tools/badge_flasher/tests -v`

Expected: all host tests pass.

- [ ] **Step 6: Commit**

```bash
git add tools/badge_flasher/flash.py tools/badge_flasher/verify.py tools/badge_flasher/records.py tools/badge_flasher/tests
git commit -m "badge-flasher: verify production flash batches"
```

---

### Task 6: ANSI interface and production state machine

**Files:**
- Create: `tools/badge_flasher/ui.py`
- Create: `tools/badge_flasher/app.py`
- Create: `tools/badge_flasher/__main__.py`
- Create: `tools/badge_flasher/tests/test_ui.py`
- Create: `tools/badge_flasher/tests/test_app.py`
- Create: `scripts/badge-flasher`

**Interfaces:**
- Produces: `FactoryFlasherApp.run_one_batch() -> BatchResult`.
- Produces: `TerminalUI` and `PlainUI` implementing one event interface.
- CLI: `scripts/badge-flasher [--plain] [--no-intro] [--offline] [--bundle PATH] [--manual-map U S S] [--once] [--verbose]`.

- [ ] **Step 1: Write failing state-machine and snapshot tests**

Test WAITING -> PREFLIGHT -> PROBE -> TOPOLOGY -> FLASH -> VERIFY -> RECORD ->
UNPLUG; failure at each phase; refusal to record PASS early; `NO_COLOR` and
non-TTY plain mode; deterministic intro snapshots; Ctrl-C safety; and `--once`.

- [ ] **Step 2: Run tests and verify RED**

Run: `python3 -m unittest tools.badge_flasher.tests.test_ui tools.badge_flasher.tests.test_app -v`

Expected: imports fail for `ui` and `app`.

- [ ] **Step 3: Implement original ANSI intro and accessible dashboard**

Use generic ANSI rain/waterfall characters, an original cave/sword silhouette,
and the text `GAMECHANGERSAI // TRIFORCE BADGE FORGE`. Never delay longer than
two seconds total; any key skips; plain mode prints one line per phase.

- [ ] **Step 4: Implement dependency-injected orchestration**

The app selects the bundle before prompting, snapshots ports on Enter, records
ROM identities, flashes probes sequentially, starts one nonce session, validates
the topology, flashes production layouts sequentially, verifies all regions,
collects runtime proof, records the batch, and waits for all three MACs to leave.

- [ ] **Step 5: Run all host tests**

Run: `python3 -m unittest discover -s tools/badge_flasher/tests -v`

Expected: all host tests pass with no network or hardware requirement.

- [ ] **Step 6: Commit**

```bash
git add tools/badge_flasher scripts/badge-flasher
git commit -m "badge-flasher: add Triforce production console"
```

---

### Task 7: Embedded bundle, documentation, and software verification

**Files:**
- Modify: `README.md`
- Create: `docs/badge/factory-flasher.md`
- Modify: `.gitignore`
- Modify: `tools/badge_flasher/resources/` with generated safe bundle and manifest

**Interfaces:**
- Documents installation, one-badge workflow, offline use, GitHub update policy,
  manual mapping, log location, recovery, and release maintenance.

- [ ] **Step 1: Build fresh probe, scanner, and uplink artifacts**

Run: `/Users/billh/.platformio/penv/bin/pio run -e factory-probe-s3`

Run: `/Users/billh/.platformio/penv/bin/pio run -e scanner-s3-combo-fof_badge`
from `esp32/scanner`.

Run: `/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge`
from `esp32/uplink`.

Expected: all three builds report `SUCCESS`.

- [ ] **Step 2: Generate and validate the embedded safe bundle**

Run: `python3 scripts/build_badge_factory_bundle.py --output tools/badge_flasher/resources/embedded-badge-factory.zip`

Run: `python3 -m tools.badge_flasher --bundle tools/badge_flasher/resources/embedded-badge-factory.zip --plain --self-test`

Expected: exact version, target, project, hardware, offsets, and hashes validate.

- [ ] **Step 3: Write operator and maintainer documentation**

Document `./scripts/badge-flasher`, the three-cable assembled-badge requirement,
automatic topology proof, PASS criteria, failure recovery, local records, and
how CI refreshes the embedded/release bundle. State macOS and Python floors.

- [ ] **Step 4: Run complete software verification**

Run: `python3 -m unittest discover -s tools/badge_flasher/tests -v`

Run: `python3 -m unittest scripts.test_build_badge_factory_bundle -v`

Run: `/Users/billh/.platformio/penv/bin/pio test -e test`

Run: `git diff --check`

Expected: every command passes.

- [ ] **Step 5: Commit**

```bash
git add README.md docs/badge/factory-flasher.md .gitignore tools/badge_flasher/resources
git commit -m "badge-flasher: document factory workflow"
```

---

### Task 8: Real three-board hardware acceptance

**Files:**
- Create: `docs/badge/factory-flasher-acceptance.md`
- Modify only if a proven defect requires a TDD fix: files from Tasks 1-7.

**Interfaces:**
- Consumes: one fully assembled badge with all three XIAO ESP32-S3 USB ports connected.
- Produces: recorded MAC/role/version evidence and a release go/no-go result.

- [ ] **Step 1: Confirm exactly three connected S3 devices without writing**

Run: `./scripts/badge-flasher --plain --once --dry-run`

Expected: three unique MACs and a no-write preflight report.

- [ ] **Step 2: Run ten topology-only classifications**

Run: `./scripts/badge-flasher --plain --once --topology-only`

Expected: the same center MAC and link-A/link-B leaf mapping ten times despite
port enumeration changes.

- [ ] **Step 3: Run one complete offline factory flash**

Run: `./scripts/badge-flasher --offline --plain --once`

Expected: one uplink and two scanner layouts pass readback and runtime proof;
the ledger records `TRIFORCE VERIFIED`.

- [ ] **Step 4: Prove fail-closed hardware faults**

Disconnect one internal UART and rerun topology-only: expected classification
failure with no production flash. Interrupt one write, reconnect, and rerun:
expected complete recovery and final PASS.

- [ ] **Step 5: Record acceptance evidence and rerun full verification**

Record tool/bundle version, three MACs, roles, hashes, ten-run topology result,
fault-injection results, and final `FOF_STATUS` facts without secrets.

- [ ] **Step 6: Commit acceptance evidence**

```bash
git add docs/badge/factory-flasher-acceptance.md
git commit -m "badge-flasher: record hardware acceptance"
```

---

### Task 9: Final review and GitHub release readiness

**Files:**
- Modify only for review fixes: files introduced above.

**Interfaces:**
- Produces: a clean review diff, verified release asset naming, and operator handoff.

- [ ] **Step 1: Review the complete scoped diff**

Run: `git diff <design-commit>..HEAD -- tools/badge_flasher esp32/factory-probe esp32/shared/factory_probe_protocol.* esp32/test scripts/badge-flasher scripts/build_badge_factory_bundle.py .github/workflows/esp32-web-flasher.yml docs/badge/factory-flasher* README.md`

Expected: no unrelated firmware behavior changes.

- [ ] **Step 2: Run final verification from a fresh terminal**

Repeat host tests, native tests, all three firmware builds, embedded bundle
self-test, shell launcher smoke test, and `git diff --check`.

- [ ] **Step 3: Confirm release packaging contract**

Verify the workflow asset name matches `badge-factory-flasher-<tag>.zip` and the
host GitHub selector. Verify the embedded bundle remains usable with networking
disabled.

- [ ] **Step 4: Prepare the release handoff**

Report commit IDs, test counts, firmware/bundle hashes, real hardware MACs,
remaining physical assumptions, and the exact command operators should run.
