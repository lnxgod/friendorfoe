# Task 2 report: unified badge USB transport ownership

## Outcome

Implemented one lifetime ESP-IDF USB Serial/JTAG transport owner for uplink
firmware. The transport installs the supported driver once, owns the only
`STDIN_FILENO` read loop, gates command dispatch until startup dependencies are
ready, feeds the shared command/binary stream parser, and serializes all
machine output through one recursive transaction lock.

Required responses are chunked to the 2,048-byte TX ring, fully checked, and
counted complete only after a bounded TX drain. A partial enqueue poisons the
output stream so a later required JSON response cannot append to corrupted
bytes; a completely enqueued frame whose required drain times out fails the
transaction without poisoning future commands. Disconnected optional boot
output is host-gated and cannot poison a later `FOF_PING`. Progress,
investigations, detections, and ESP logs use their documented lower priorities
and drop counters.

The fix-cycle moved the production transport arbitration into an
allocation-free native-tested policy seam. It rechecks poison state after lock
acquisition, drops nested same-owner low-priority writes, chunks large frames,
uses the exact 5,000 ms binary idle timeout, and samples time immediately
before every parser feed and timeout poll. Readiness remains closed if the
transport lease cannot initialize. Before readiness, every complete nonempty
line receives deterministic `FOF_ERROR:booting`; malformed or unknown input
does not reach a command handler or count as a valid command. `FOF_READY`
remains an optional, host-gated compatibility banner with a 250 ms bounded
drain.

Scanner upload activation is now gated on successful completion of the
required terminal USB response. Validated image metadata is first persisted
with a fail-closed coordinator record; only a successful terminal drain arms
and starts the coordinator. Terminal failure, arming failure, reboot in the
intermediate window, abort, or trailing-command cleanup cannot activate the
staged scanner image and clears upload health state.

`FOF_STATUS` now freezes immutable target/project/hardware identity, the base
MAC, running partition and rollback/recovery state, plus the frozen USB health
schema. Full status is rendered into a bounded PSRAM buffer below the Android
64 KiB line limit; allocation or render overflow falls back to a bounded
internal-memory repair status.

The badge build now gives ESP-IDF CMake and PlatformIO the same custom
partition-table source of truth. The generated application manifests all place
`fof_badge_uplink.bin` at the decoded `ota_0` offset, `0x20000`.

## TDD evidence

Initial required RED command:

```text
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests/test_badge_firmware_transport_contract.py backend/tests/test_firmware_build_version.py -v
```

Initial result: 109 collected, 98 passed, 11 failed. The failures identified
the missing transport API/driver ownership, mixed readers, missing readiness
gate and status/output routing, and the stale `0x10000` generated app offset.

The first fix-cycle RED pass exposed missing policy seams and five focused
wiring failures. Later RED passes specifically caught valid-command inflation,
required-drain poisoning, missing optional readiness compatibility, stale
per-feed time, option-led manifest mappings, classifier/handler divergence,
pre-ready unknown input, permissive trailing JSON, duplicate manifest keys,
the missing 250 ms readiness bound, and the reset window before durable
terminal authorization.

Final focused command:

```text
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests/test_badge_firmware_transport_contract.py backend/tests/test_firmware_build_version.py -q
```

Final result after the cumulative-review follow-up: 124 passed in 0.22s.

Additional regression evidence:

- `backend/tests/test_badge_quiet_mode_contract.py -v`: 9 passed.
- `cd esp32 && /Users/billh/.platformio/penv/bin/pio test -e test`:
  525/525 passed in 1.790s, including fragmented/coalesced command and binary
  parser boundaries, exact timeout and timer-wrap behavior, output arbitration,
  readiness/reentry, host disconnect, large chunks, upload terminal gating,
  health cleanup, and recovery after a fully enqueued drain timeout.
- `git diff --check`: clean.

An independent fix-cycle re-review reported no critical, important, or minor
findings and considered the change ready for a clean firmware build and the
hardware acceptance gate. A later cumulative review found one Important build
enforcement gap: the verifier compared only the CSV and binary `ota_0` offset,
so drift in any other partition row could pass.

The cumulative-review follow-up used a strict RED/GREEN cycle. The new
regression kept `ota_0` at `0x20000`, changed only `ota_1` from `0x220000` to
`0x230000`, and failed because the old verifier returned no errors (`1 failed
in 0.10s`). A second RED test proved that an unavailable partition generator
did not yet fail closed. The implementation now runs ESP-IDF's
`gen_esp32part.py` against the supplied CSV in an automatically cleaned
temporary directory and byte-compares the complete regenerated table with
`partitions.bin`; an explicitly missing or undiscoverable generator is a hard
error. The two new tests passed in 0.11s, and the complete focused build
verification file passed 28/28 in 0.20s.

An independent read-only review of the follow-up reported no critical,
important, or minor findings. The native suite was not rerun for this
script/test/report-only follow-up because no shared or firmware C changed; its
prior 525/525 result remains the native evidence.

## Build and partition preflight

Final clean build:

```text
cd esp32/uplink
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge -t clean
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
```

Result: success in 61.763s; RAM 173,428 / 327,680 bytes (52.9%); flash
1,202,489 / 2,097,152 bytes (57.3%). Existing unrelated unused-symbol warnings
remain. The mandatory post-build verifier printed
`FoF: badge uplink flash manifests and referenced paths verified`.

The verifier was also run directly after the clean build:

```text
python3 esp32/scripts/verify_badge_uplink_build.py \
  --build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge \
  --partition-source esp32/uplink/partitions_s3_fof_badge_8mb.csv
```

Result: `badge uplink manifests: strict verification passed`.

Partition proof:

- Generated `partitions.bin` decoded `ota_0` at `0x20000`, size 2 MiB.
- Recompiling `partitions_s3_fof_badge_8mb.csv` produced a byte-identical
  binary. Both SHA-256 values were
  `0730efe516b42ac83a484c509291564b9bc1f891c122e3d3525293f0baa686bd`.
- `flash_args`, `flash_app_args`, and `flash_project_args` each contain exactly
  `0x20000 fof_badge_uplink.bin` and no `0x10000` application entry.
- `flasher_args.json` reports app offset `0x20000` and no `0x10000` flash file.
- All four manifests are mandatory. Option-led text mappings, duplicate raw
  JSON keys, decoded duplicate offsets, path escapes, missing referenced files,
  alias mismatches, CSV/binary partition drift, and any alternate app offset
  fail verification.
- CSV/binary validation regenerates every row with the installed ESP-IDF
  partition generator and compares the entire binary, covering labels, types,
  subtypes, offsets, sizes, flags, the MD5 trailer, and padding rather than only
  `ota_0`. Generator discovery checks an explicit override, `IDF_PATH`, and the
  active/default PlatformIO package roots and fails closed if none is usable.
- Generated sdkconfig selects `CONFIG_PARTITION_TABLE_CUSTOM` and
  `partitions_s3_fof_badge_8mb.csv`.
- The post-build hook materializes the ESP-IDF manifest aliases as regular,
  byte-identical files and the workflow invokes the verifier explicitly as a
  second gate.

Clean-build SHA-256 values:

- `firmware.bin` and `fof_badge_uplink.bin`:
  `0b65f1b09d3b71bf0936c1ce57799138f2030ae31c6107e02b8fd38cac883f85`
- `partitions.bin` and `partition_table/partition-table.bin`:
  `0730efe516b42ac83a484c509291564b9bc1f891c122e3d3525293f0baa686bd`
- `bootloader/bootloader.bin`:
  `faafbd97036d95b5fdc4a02add84119e075bd3f9c2a4327f95f66a1e237cd9b3`

The verifier was rerun directly against this exact clean artifact after the
full-table enforcement change and passed with the same hashes. No second full
firmware rebuild was needed because this follow-up changed only the standalone
verifier and its Python tests; the post-build hook and build inputs were not
changed.

## Files changed

- Added `esp32/uplink/main/core/badge_usb_transport.{c,h}`.
- Added `esp32/shared/badge_usb_transport_policy.{c,h}` and native policy
  tests used by the production transport.
- Reworked `serial_config.{c,h}`, `main.c`, and `fw_store.c` around the single
  transport, strict command classification, terminal upload authorization, and
  bounded renderer.
- Added the USB Serial/JTAG component dependency and aligned PlatformIO plus
  badge sdkconfig partition settings.
- Added strict build verification scripts, post-build wiring, CI enforcement,
  and transport/build regression contracts.

## Scope boundary

No firmware was flashed and no physical badge/Android host was connected, so
USB enumeration, SOF sampling, real host backpressure/reset behavior, Android
host interoperability, actual scanner activation, and on-device recovery UI
remain physically unverified. No version bump, release/factory artifact,
push, or tag was performed. The pre-existing untracked
`.camera-before-zoom.jpg` was not modified or staged.
