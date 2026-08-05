# Backend Badge Lite Factory Flasher

This package is the host-side factory implementation for a complete headless
Backend Badge Lite assembly. It is intentionally contained under
`backend-firmware/` and is separate from the native/full-badge factory tool.

## Operator quick start

From `backend-firmware/` on macOS, double-click
`flash-lite-badges.command` or run:

```sh
./flash-lite-badges.command
```

The launcher uses the checked-in offline bundle. At each prompt:

1. Remove the previous unit and every unrelated Espressif USB device.
2. Connect exactly one complete Lite assembly: three Seeed XIAO ESP32-S3
   boards, each with 8 MB flash and 8 MB PSRAM.
3. Press Enter once and leave all three USB ports connected until `PASS` or
   `FAIL` is printed.
4. Ship only a unit with an explicit `PASS // BACKEND BADGE LITE` receipt.

The operation erases and rewrites all three boards. Never connect a native
badge, a configured field unit, or unrelated ESP32 hardware to this factory
station.

For one non-interactive, explicitly acknowledged unit:

```sh
./flash-lite-badges.command \
  --yes --once --confirm-product badge_lite
```

## What a PASS proves

The factory cycle fails closed unless it can:

- inventory exactly three supported boards and bind them by eFuse MAC;
- flash and read back a disposable topology probe on every board;
- prove one reciprocal two-link graph and assign uplink, BLE, and Wi-Fi roles;
- erase, write, verify, and explicitly read back every production image;
- rebind all post-flash USB identities to the original MACs;
- bind runtime status to the exact Lite USB descriptor and expected uplink;
- prove blank configuration, exact scanner identities and profiles, healthy
  radios and command paths, stable transport counters, and runtime progress;
- reboot all three boards and repeat the runtime proof with fresh boot IDs;
- append and fsync the authoritative JSONL PASS record.

A `FAIL` never approves a partially written unit. Keep all three boards
together for rework and rerun the complete cycle after correcting the error.

## Records and the process lock

Private manufacturing evidence defaults to:

```text
~/Documents/FoF Backend Badge Lite Factory/
```

`lite-factory.jsonl` is authoritative; `lite-factory.csv` is a rebuildable
operator projection. Do not edit or delete either file to bypass a prior PASS
or topology conflict.

One factory process owns a per-user advisory lock for its whole batch session,
including the prompt for the next unit. If another terminal prints
`another Lite factory process is already running`, return to the existing
factory window and either continue there or press Control-C at the prompt. The
lock is released when that process exits; it is not a stale lock file that
should be deleted.

## Package map

| Path | Purpose |
| --- | --- |
| `cli.py` | Batch workflow, prompts, safety acknowledgements, flashing order, and PASS/FAIL handling |
| `bundles.py` | Immutable bundle loading, identity/digest validation, and release trust registry |
| `verify.py` | USB rebinding plus descriptor-bound runtime and reboot gates |
| `records.py` | Private append-only ledger, permissions, fsync, rework checks, and process lock |
| `models.py` | Factory result and prior-PASS data contracts |
| `public_output.py` | User-visible output scrubbing boundary |
| `resources/lite-factory-flasher-embedded.zip` | Pinned offline probe/scanner/uplink bundle |

The thin executable entry point is `../lite_badge_factory.py`; the macOS
launcher is `../../flash-lite-badges.command`. The deterministic candidate
builder is `../build_lite_factory_bundle.py`.

## Focused development checks

From `backend-firmware/`:

```sh
PYTHONPATH=. ../backend/.venv312/bin/pytest \
  tools/tests/test_lite_factory_bundle.py \
  tools/tests/test_lite_factory_cli.py \
  tools/tests/test_lite_factory_records.py \
  tools/tests/test_lite_factory_verify.py -q
```

The full design, pinned artifact identities, factory gates, and release
boundary are documented in
[`../../../docs/backend-lite-factory-flasher.md`](../../../docs/backend-lite-factory-flasher.md).
