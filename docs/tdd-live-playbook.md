# TDD and Live-Code Playbook

Friend or Foe runs on live hardware, so code that compiles is not enough. The
default workflow going forward is test-first, changelog-first, and canary-first.

## Core rule

Every bug fix or behavior change should leave behind:

- a failing test or reproduction first
- the smallest code change that makes it pass
- an updated changelog entry that explains operator impact
- proof that the live targets still build and the risky path was exercised

## TDD loop by surface

### Backend

1. Reproduce the bug with `pytest`.
2. Add or tighten a test in `backend/tests/`.
3. Fix the code.
4. Run `pytest tests -q`.
5. If the change touches ingest, calibration, triangulation, or dashboard data
   shape, test the endpoint or serializer that operators actually see.

### ESP32 firmware

1. Add or tighten a native unit test in `esp32/test/` when the logic is pure.
2. If the change affects live fleet targets, rebuild all of these:
   - `scanner-s3-combo`
   - `scanner-s3-combo-seed`
   - `uplink-s3`
3. Prefer canary proof on real hardware before wider rollout.
4. Treat UART schema, status payloads, classification, and OTA flow as
   compatibility surfaces. When one changes, update the paired backend tests too.

### Android

1. Add/update a JVM test beside the feature when possible.
2. Rebuild the debug APK before closing the change.

## Changelog discipline

Update changelogs in the same change, not afterward.

- `CHANGELOG.md`
  - user-visible behavior
  - cross-surface changes
  - rollout notes and operator impact
- `esp32/CHANGELOG.md`
  - firmware internals
  - target/env changes
  - OTA/build/test notes

Good entries include:

- exact firmware or app versions
- affected targets or nodes
- what broke before
- what changed now
- how it was verified
- any remaining caveats

## Treat code as live

Before touching fleet firmware, assume the code is headed to production and do a
local preflight.

```bash
python3 scripts/preflight.py
```

Useful narrower loops:

```bash
python3 scripts/preflight.py backend
python3 scripts/preflight.py esp32-native esp32-live-builds
```

## Release and canary checklist

1. Write the failing test.
2. Make it pass.
3. Run local preflight for the affected surfaces.
4. Update changelogs with exact behavior and verification.
5. Deploy to one canary first.
6. Verify live status, versions, counters, and the user-visible behavior.
7. Only then roll the fleet.

## Current live firmware targets

These are the targets CI and local preflight must keep green because they map to
the deployed fleet:

- `scanner-s3-combo`
- `scanner-s3-combo-seed`
- `uplink-s3`
