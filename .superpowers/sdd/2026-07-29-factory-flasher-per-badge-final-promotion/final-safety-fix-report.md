# Final Factory Flasher Safety Fix Report

Date: 2026-07-30

## Scope

- Active `KeyboardInterrupt` from `run_one` now records the selected role and
  validated bundle identity as a fsync-backed JSONL FAIL row, gives the
  operator explicit rework guidance, and exits 130 without PASS output.
- EOF/Ctrl-C at the removal prompt preserves the previously fsync-backed PASS
  as the sole record and truthfully reports that it is already recorded.
- Role-menu and connection-prompt Q/EOF/Ctrl-C paths remain pre-hardware
  cancellations: exit 130, do not call `run_one`, and add no JSONL PASS/FAIL
  row.
- The canary acceptance ledger now reflects verifier constant
  `UPLINK_CANARY_MAX_APP_BYTES = 1_468_464`, with 628,688 bytes remaining in
  the 2 MiB OTA app slot. It explicitly describes this as the exact accepted
  `.67.2` ceiling, with no open-ended relaxation.

## TDD evidence

The following new behavioral tests were added before the CLI change:

- `test_active_operation_ctrl_c_records_rework_failure`
- `test_removal_prompt_cancellation_keeps_the_recorded_pass`
- `test_connection_prompt_cancellation_writes_no_factory_record`

Their initial focused run failed as intended: active Ctrl-C did not create the
required JSONL FAIL record, and post-PASS EOF/Ctrl-C incorrectly said no PASS
record had been written. The pre-hardware connection cancellation test already
passed as a characterization of the retained safety behavior. After the
minimal control-flow change, the same focused test command passed.

`test_role_menu_cancellation_writes_no_factory_record` was then added as an
explicit characterization for retained pre-role Q/EOF/Ctrl-C behavior and
passed.

## Verification commands

```sh
/Users/billh/.platformio/penv/bin/python -m unittest discover \
  -s tools/badge_flasher/tests -v
/Users/billh/.platformio/penv/bin/python -m unittest \
  scripts.test_build_badge_factory_bundle -v
shasum -a 256 \
  esp32/uplink/.pio/build/uplink-s3-fof_badge-con-crud-canary/firmware.bin \
  esp32/scanner/.pio/build/scanner-s3-combo-fof_badge-con-crud-canary/firmware.bin \
  tools/badge_flasher/resources/badge-factory-flasher-embedded.zip
git diff --check
```

The accepted artifacts remain exact:

- Uplink: `1,468,464` bytes,
  `78ef3b6dafe61e8e2fdc3fb28447372aaf76da38cd57ca0961828bbbdc08c434`
- Scanner: `1,216,800` bytes,
  `2d0e84501baf3bc929eed03a0b9c1f0272ed66baa9b81dd4513d6dc3fa2c032b`
- Embedded ZIP:
  `038d83adcc3e6a561a9192e8bed26ec205e7e7c9374eb6ff800baf573bb44576`

No firmware, embedded ZIP, builder, wrapper, public/web/GitHub asset, or
private ledger was modified. `.camera-before-zoom.jpg` remains preserved as
untracked user content.
