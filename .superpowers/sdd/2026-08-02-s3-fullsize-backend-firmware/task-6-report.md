# Task 6 report: guarded Fullsize backend OTA rollouts

## Outcome

Implemented a separate, durable S3 Fullsize backend-OTA channel at base
`435f0570dafec7fcd406ce172de8a2ee2e717969`. The implementation does not alter
the existing BLE command models, unions, persistence, or `/commands/next`
routes. The intended commit subject is exactly:

```text
backend: add guarded Fullsize OTA rollouts
```

Independent-review hardening is delivered as the exact follow-up commit:

```text
backend: harden Fullsize rollout contracts
```

Pre-deployment protocol amendment round 2 binds recovery authorization to the
probe itself, before Task 7 can download or stage an equal image. Its exact
commit subject is:

```text
backend: bind Fullsize recovery mode to probes
```

## RED/GREEN evidence

The work was developed in focused TDD slices.

- Initial backend-OTA API/service tests: 30 expected failures because the
  separate routes, models, persistence, and state machine did not exist.
- Initial host serializer tests: 2 expected failures because top-level uplink
  `boot_id` and `topology_generation` were absent.
- Initial native upload-batch test: failed to build/validate against the new
  telemetry contract before the struct, runtime population, serializer, and
  fixture changes were implemented.
- Follow-up safety RED: 3 failures proved that successful probe did not yet
  revalidate the complete live binding, no-update scanner boot IDs were not
  persisted for later uplink convergence, and uint32 sequence exhaustion could
  mutate/persist before response validation. All three now pass.
- Follow-up ingestion RED: 2 failures proved bool/string coercion for uplink
  boot telemetry. Both fields now use strict nonzero uint32 validation.
- The first mandated native heartbeat gate exposed a stale heartbeat fixture:
  its context omitted the now-required server-authoritative identity and new
  uplink binding fields, and its exact token counts predated those fields. The
  fixture now exercises the complete Lite and Fullsize heartbeat contract.

Fix round 1 established 24 focused RED failures against `975b69a`:

- 14 exact-trio failures: malformed runtime versions, Python-equivalent
  bool/float slots, missing/wrong profiles, and a mismatched snapshot device ID
  reached activation;
- 3 progress failures: real scanner/uplink probe stages were rejected and
  counters could regress when stage advanced;
- 5 failure-attribution failures: empty/partial identity evidence was accepted
  after validation, writes, reboot wait, or convergence;
- 2 create-transaction failures: activation had neither a final SQLite
  immediate transaction/active lookup nor outage translation at that boundary.

All fix-round regressions are GREEN. Exact UART/type and apply-stage neighbors,
pre-validation all-empty failure attribution, and complete but nonmatching
observed failure identity were retained as positive characterization cases.

Fix round 2 added the complete amendment matrix before production changes. The
focused RED slice produced 30 expected failures (plus one already-green generic
invalid-character guard): missing probe mode and 14/15-key shapes, both-mode
duplicate polling, eight canonical/u32 running-version cases, 16 persisted
version-relation × mode outcomes, apply-time no-update, golden receipt line
order, and mode-only receipt mutation/rejection. After the envelope/receipt
slice, all 16 policy cases still exercised backend-authoritative comparison of
the fetched candidate with the persisted current component. The final
amendment slice is 30/30 GREEN, and the complete Task 6 service file is 113/113
GREEN.

Final focused GREEN gates:

```text
backend/.venv312/bin/pytest -q \
  tests/test_backend_ota_commands.py \
  tests/test_backend_node_commands.py \
  tests/test_scanner_ota_relay_paths.py \
  tests/test_backend_firmware_ingest.py
358 passed, 3 warnings in 6.28s

backend/.venv312/bin/python -m pytest -q \
  backend-firmware/tools/tests/test_serializer_fixture.py
7 passed in 3.34s

pio test -e backend-native \
  -f test_backend_upload_batch -f test_backend_heartbeat
20 test cases: 20 succeeded

pio test -e backend-native-fullsize \
  -f test_backend_upload_batch -f test_backend_heartbeat
20 test cases: 20 succeeded
```

The backend warnings are existing/the expected Pydantic `schema` shadowing
warnings. The host serializer gate reports the repository's existing
`pytest-asyncio` default-loop-scope deprecation warning.

## Exact channel and schemas

Only these routes were added, with the literal `/next` route declared before
the dynamic operation route:

```text
POST /nodes/{device_id}/backend-ota/rollouts
GET  /nodes/{device_id}/backend-ota/next
POST /nodes/{device_id}/backend-ota/{operation_id}/events
GET  /nodes/{device_id}/backend-ota/{operation_id}
```

Request parsing reads raw UTF-8 JSON with duplicate-key detection before strict
Pydantic validation. The create request has exactly required
`components="all"` plus optional/defaulted
`apply_mode="newer_only"|"same_version_recovery"`; missing, individual,
unknown, skip, duplicate, coerced, and extra fields return 422 without a row.

The amended probe response/command has exactly 14 keys:

```text
schema, operation_id, type, component, catalog_name, expected_sha256,
expected_size, expected_uplink_mac, expected_uplink_boot_id,
expected_target_mac, expected_target_boot_id,
expected_topology_generation, apply_mode, next_sequence
```

Apply has those same 14 keys, changes `type`, and adds exactly
`probe_receipt_sha256` (15 keys total). Default rollouts emit
`apply_mode="newer_only"`; explicit recovery emits
`apply_mode="same_version_recovery"` in both phases. Duplicate polls retain
that mode byte-for-byte. Operation IDs come only from
`secrets.token_hex(16)` and remain the same 32 lowercase hexadecimal characters
through every persisted phase and restart.

Exact event shapes are:

- begin: 6 keys (`schema`, `operation_id`, `sequence`, `type`, `component`,
  `catalog_name`);
- progress: those 6 plus `stage`, `received`, `total`, `retry_count` (10);
- end: those 6 plus the 15 required terminal identity, outcome, health, and
  receipt fields (21).

The exact acknowledgement has 8 keys: `ok`, `operation_id`,
`accepted_sequence`, `next_sequence`, `current_component`, `current_action`,
`terminal`, and `duplicate`. History has exactly 9 keys: `operation_id`,
`device_id`, `state`, `apply_mode`, `current_component`, `current_action`,
`next_sequence`, `terminal`, and ordered `events`.

The existing BLE focused tests pass in the combined backend gate, proving its
poll/result/history response bodies and 204 behavior remain unchanged.

## Immutable preflight and firmware binding

Creation performs the required order before activating a row:

1. Strict request validation.
2. Fresh preflight of one exact healthy Fullsize uplink plus scanner0 bound by
   `uart=ble, slot=0` and scanner1 by `uart=wifi, slot=1`, independent of list
   order. Server-authoritative identities, three distinct MACs, healthy role,
   radio, ingress, OTA, rollback, nonzero boot IDs, and uplink-owned nonzero
   topology generation are mandatory. Scanner `role_generation` cannot replace
   topology generation. UARTs are exact strings, slots are exact integers
   (never bool/float), profiles are exactly `ble_primary`/`wifi_primary`, and
   every runtime version has the exact canonical backend form
   `uint32.uint32.uint32-backend`. Leading `v`, wrong/missing/extended suffixes,
   invalid separators, and any numeric component above `UINT32_MAX` fail before
   a firmware fetch or active row. Leading-zero numeric components retain the
   firmware comparator's semantics: identical spellings may be equal, while
   different spellings of the same numeric core are unordered. The returned
   snapshot device ID must equal the requested route device ID.
3. Fetch scanner bytes exactly once, immediately revalidate the complete
   binding with a fresh clock, then validate embedded image identity/capacity
   and derive version/size/SHA-256 from those same bytes.
4. Fetch uplink bytes exactly once and repeat the immediate complete-binding
   check and same-byte metadata derivation.
5. Perform a final synchronous complete-binding check. Only then enter the
   database boundary: SQLite `BEGIN IMMEDIATE` (or the portable PostgreSQL
   transaction), active-row `SELECT ... FOR UPDATE`, and insert with the unique
   active key as the final race guard. No write lock spans firmware awaits.

Tests prove invalid scanner bytes stop before the uplink fetch, staleness is
recomputed after awaited fetches, changes after either fetch/final check abort,
and no rollout calls `get_catalog()`, `get_firmware_version()`, compatibility,
legacy-trigger, legacy identity-rebind, or direct legacy relay helpers.

## Durable state-machine proof

`BackendOtaRollout` and `BackendOtaEvent` are dedicated tables. A nullable
unique `active_key=device_id` enforces one active rollout and is cleared only on
whole-rollout terminal completion/failure. The rollout persists immutable
binding/image JSON, apply mode, current component/action, accepted probe
receipt, global next sequence, begin/stage/counter state, converged scanner boot
IDs, and lifecycle timestamps. Events persist exact raw UTF-8 text plus body
SHA-256 under unique `(operation_id, sequence)`.

The serialized lifecycle is:

```text
scanner0 probe -> scanner0 apply + heartbeat convergence
-> scanner1 probe -> scanner1 apply + heartbeat convergence
-> uplink probe -> uplink apply + uplink/scanner heartbeat convergence
```

Before either successful probe tuple advances, the backend compares the exact
fetched-image version with the persisted original running version using the
firmware's three-uint32 ordering and exact normalized spelling, then combines
that relation with the persisted mode. Newer requires `complete/eligible` in
either mode. Equal requires `no_update/no_update` in `newer_only` and
`complete/eligible` in `same_version_recovery`. Older, unordered, or invalid
candidates cannot advance through eligible or no-update under either mode;
their `failed/rejected` evidence remains reportable. Apply-time no-update is
still 409. Therefore recovery authorizes equality only and can never downgrade.

Successful probe and no-update both recheck the complete live heartbeat against
the phase-appropriate original/prior-converged trio before advancing. Apply
requires positive writes, exact image identity/version, bound MAC/topology,
new target boot ID, all three health booleans, and matching live convergence.
No-update records scanner boot convergence even without an apply. Uplink apply
also requires both exact scanners to have rejoined healthy at their persisted
converged boot IDs.

Only the five specified terminal state/decision pairs are accepted, with exact
error and write-count semantics. Begin must be first. Probe/apply and
component-specific stages are enforced; stage order cannot regress. Scanner
probe allows metadata/download/validate/stage/UART relay/convergence but not
reboot wait; uplink probe also excludes UART relay. Apply allows every stage
for scanners and excludes UART relay for uplink. Byte/retry counters are
globally nondecreasing and total is stable for the entire current command
phase; they reset only at probe-to-apply or component advancement. Global
sequence is strict uint32 and exhaustion rejects before any event insertion or
rollout mutation.

Failed/rejected events may use an all-empty identity only during a probe before
validation and before any image write. Partial empty tuples are always
rejected. Validation-or-later progress, any positive write, and an apply phase
require four nonempty canonical identity fields. Complete observed identity is
retained even when it differs from the expected image, preserving useful
identity-mismatch evidence. Receipt verification remains mandatory in every
case.

SQLite uses `BEGIN IMMEDIATE`; row reads use `SELECT ... FOR UPDATE`; unique
active/event keys remain the final race guards. Tests prove simultaneous create
produces one active row, simultaneous byte-identical events produce one stored
event plus one idempotent replay, and existing-sequence lookup precedes current
phase/terminal checks. Reordered JSON or any raw-byte change at the same
sequence returns 409. Restart/resume, terminal history, completed replacement,
duplicate polling, duplicate replay after advancement, and retryable store
outage (`503`, `Retry-After: 1`) are covered.

For creation specifically, the immediate transaction and active lookup occur
only after both firmware awaits and all four binding snapshots. An
`OperationalError` from that final boundary is rolled back and translated to
the same retryable backend-OTA unavailable error.

## Canonical receipt proof

`backend-firmware/test/fixtures/backend_ota_receipt_v1.json` contains named
probe/apply vectors with trusted persisted command fields, end fields excluding
the receipt, exact UTF-8 with one final LF, byte-identical hex, and lowercase
SHA-256:

```text
probe: fe251f806c754ce75978ea3da2dba33787f61e033423f60271449a2873365778
apply: 24b481545a5be0c3e995031b1c46b50ee7dfe150f0a895a7b6997b309cfa9038
```

The amended v1 preimage inserts `apply_mode=newer_only` immediately after
`command_type` in both vectors. The probe preimage is 778 bytes (1556 hex
characters); apply is 783 bytes (1566 hex characters). Tests prove UTF-8 equals
the fixture hex, hashes equal the fixture digests, builder output matches exact
bytes/line order/boolean `0|1` normalization, one final LF is present, and
apply binds the accepted probe digest. Changing only `apply_mode` changes the
digest, and the backend rejects a receipt computed for the other mode. The
receipt field itself is never hashed.

## Uplink heartbeat telemetry and Task 7 carry-forward

The shared upload context and JSON now carry uplink `boot_id` and
`topology_generation`; the uplink runtime populates them directly from
`s_runtime.boot_id` and `s_runtime.topology_generation`. Both serializer
profiles, native heartbeat fixtures, and generated backend fixtures include
them. Native builders reject zero. Backend ingestion accepts omission for old
traffic but, when supplied, requires strict nonzero uint32 values and preserves
both as sticky heartbeat fields. Fullsize rollout preflight requires both.

Task 7 must keep these protocol values distinct in scanner-side
`ota_read_binding()`: scanner `role_generation` is scanner-owned role state and
must never be substituted for the uplink-owned `topology_generation` carried by
the command/heartbeat binding. The scanner's own boot binding likewise remains
the exact scanner boot ID; prior no-update components can legitimately retain
their existing boot IDs and are persisted as converged.

Task 7 must now require `apply_mode` in both strict Fullsize envelope decoders
and bind it into both terminal receipts before download/cache/write. Preserve
the existing Lite `backend_ota_maintenance_run_probe(...)` signature and its
newer-only behavior byte-for-byte. Fullsize needs a profile-only mode-aware
probe entry point backed by a shared internal staging helper: equality returns
zero-write no-update in newer-only, but fully downloads/validates with zero
probe writes and exposes apply in recovery. Newer remains eligible in both;
older/unordered/invalid reject in both. Do not permit apply-time no-update.

## Changed-path and protected-path audit

Task-owned changes are confined to:

```text
backend/app/models/db_models.py
backend/app/models/schemas.py
backend/app/routers/nodes.py
backend/app/services/backend_node_status.py
backend/app/services/backend_ota_commands.py
backend/tests/fixtures/backend_firmware_detection_batch.json
backend/tests/fixtures/backend_firmware_fullsize_detection_batch.json
backend/tests/test_backend_firmware_ingest.py
backend/tests/test_backend_ota_commands.py
backend-firmware/shared/backend_upload_batch.c
backend-firmware/shared/backend_upload_batch.h
backend-firmware/uplink/main/main.c
backend-firmware/test/fixtures/backend_ota_receipt_v1.json
backend-firmware/test/support/backend_serializer_fixture.c
backend-firmware/test/test_backend_heartbeat/test_main.c
backend-firmware/test/test_backend_upload_batch/test_main.c
backend-firmware/tools/tests/test_serializer_fixture.py
docs/superpowers/plans/2026-08-02-s3-fullsize-backend-firmware.md
.superpowers/sdd/2026-08-02-s3-fullsize-backend-firmware/task-6-report.md
```

No `android/`, protected `esp32/`, deployment, workflow, database-content,
factory-flasher, native Badge, or APK paths changed. No hardware or deployment
commands ran. The only verification concern is that the system `python3` did
not provide this repository's pytest environment, so the host serializer gate
used the checked backend `.venv312` Python explicitly; the requested test file
and assertions were unchanged.

Fix round 1 changes only the backend OTA service, its focused test file, and
this report. It does not touch C/native firmware, so the already-green native
gates were intentionally not rerun.

Fix round 2 changes only the backend OTA schema/service, focused Task 6 tests,
undeployed Fullsize receipt fixture, Task 6/7 implementation plan, and this
report. No C/native/Lite path changed, so native gates were again intentionally
not rerun.
