# Badge USB Hardening Acceptance

## Release state

The hardened badge firmware is **not factory/release approved** until every
automated gate and all six physical gates below pass on one battery-connected
uplink plus its two scanners. The sequence deliberately begins with the final
uplink candidate and both scanners running firmware versions strictly older
than that candidate, then proves their automatic transition to the exact final
scanner candidate. After the release version changes, repeat that complete
sequence with the rebuilt candidate artifacts.

Evidence is appended locally under
`artifacts/badge-usb-hardening/acceptance.jsonl`. That directory is ignored by
Git because it contains hardware IDs. The verifier deliberately rejects SSIDs,
ambient detections, nearby device addresses, and raw RF payloads.

The JSONL verifier detects malformed records, inconsistent hardware bindings,
out-of-order gates, duplicated or missing records, failed gates, and
semantically inconsistent checkpoint/aggregate data. This is not
cryptographic provenance: an attacker able to replace the entire file could
construct a fully self-consistent rewrite. If evidence must remain trustworthy
against that threat, sign the completed file or publish its hash to an
independent append-only system immediately after the final read-only audit.

## One-time bootstrap limitation

A badge still running firmware from before USB hardening cannot prove the
immutable application identity required for application OTA and does not have
the new confirmed recovery chord. The assembled badge has no accessible
BOOT/RESET workflow, so install the hardened uplink once through its guarded
legacy USB bridge before starting acceptance:

1. Keep the battery connected.
2. Select the uplink's explicit USB port and invoke
   `scripts/fof_badge_flash.py --legacy-usb-bootstrap --only all`.
3. Require one stable global serial-port census with zero pre-existing ROM
   devices before sending any recovery command.
4. Require the exact allowlisted legacy application identity and health
   fields, one `FOF_BOOTLOADER` command, and the exact
   `FOF_BOOTLOADER:OK` acknowledgement on the same serial handle.
5. Require a unique ESP32-S3 ROM identity with 8 MB flash, 8 MB embedded
   PSRAM, and a captured base MAC before any write.
6. Use the guarded complete-layout write, independent verify, application
   return by that base MAC, one scanner stage, and automatic convergence of
   both scanner slots.

The legacy status proof, acknowledgement, ROM port/base MAC, and guarded
write/verify transcript must be recorded in
`docs/badge/xiao-uplink-bootstrap.md`. This one-time installation is setup,
not a passing recovery-chord gate.

## Automated gate

From the repository worktree:

```bash
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test

cd ..
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests -v

/Users/billh/.platformio/penv/bin/python -m unittest \
  scripts.test_badge_mutation_entrypoints \
  scripts.test_bound_rom \
  scripts.test_build_badge_factory_bundle \
  scripts.test_esptool_provenance \
  scripts.test_fof_badge_debug_bridge \
  scripts.test_fof_badge_flash \
  scripts.test_fof_badge_flash_phase_a_json \
  scripts.test_fof_badge_flash_phase_a_serial \
  scripts.test_recover_fof_badge \
  scripts.test_secure_artifact_tree \
  scripts.test_usb_descriptor_binding \
  scripts.test_user_visible_redaction \
  scripts.test_verified_badge_artifacts \
  scripts.test_verify_badge_usb_hardening -v

backend/.venv/bin/python -m pytest \
  scripts/test_fof_flash_release.py -q

cd android
./gradlew testDebugUnitTest assembleDebug

cd ../esp32/scanner
/Users/billh/.platformio/penv/bin/pio run -e scanner-s3-combo-fof_badge

cd ../uplink
/Users/billh/.platformio/penv/bin/pio run \
  -e uplink-s3-fof_badge -t clean
/Users/billh/.platformio/penv/bin/pio run \
  -e uplink-s3-fof_badge
```

The local unittest command intentionally uses PlatformIO's Python environment,
which owns the flasher dependencies. The release-version pytest uses the
repository backend virtual environment. GitHub Actions runs the same 14
unittest modules plus the release-version pytest after `cd scripts` with the
CI runner's `python`; those unittest module names therefore omit the
`scripts.` prefix.

The uplink application must remain below its 2 MiB OTA slot. Decode the
generated partition table and require the application address to be `0x20000`
in every generated flash manifest.

### Deferred mutation-inventory owner

Before Phase E/E0 closes, the Phase E hardening owner must add the public
`badge_ble_investigation_start_with_timeout` entry point to
`scripts/badge_mutation_entrypoints.json` and its exact inventory backstop.
The C9a USB schema work deliberately does not rewrite the currently frozen
inventory count while that acceptance batch is in flight.

## Acceptance session

The same three immutable hardware IDs must remain in the same physical slots
for every gate. Create a private session JSON:

```json
{
  "session_id": "defcon34-canary-001",
  "uplink_hardware_id": "00:00:00:00:00:01",
  "ble_hardware_id": "00:00:00:00:00:02",
  "wifi_hardware_id": "00:00:00:00:00:03"
}
```

Do not commit this file. A PASS facts file contains only the gate-specific
observations listed below; it must not contain a caller-authored `snapshot`.
Store the session file in an owner-controlled private directory:

```bash
mkdir -m 700 /private/path/defcon34-canary-001
chmod 600 /private/path/defcon34-canary-001/session.json
```

Mutating gates require that explicit session file to be one owner-owned,
owner-only regular file with exactly one hard link. Its parent directory must
be owned by the invoking user and must not be group/world writable. The
session path may not traverse a user-controlled symlink. It may be in the same
directory as the evidence file, nested beneath it, or copied to another
private directory; its location is not part of a mutating operation's
identity.

The recorder queries a live badge port, validates the response against the
exact session and expected version, issues the immutable snapshot itself, and
appends it with:

```bash
/Users/billh/.platformio/penv/bin/python \
  scripts/verify_badge_usb_hardening.py \
  --gate GATE_NAME \
  --port /dev/cu.usbmodemXXXX \
  --session-file /private/path/session.json \
  --facts-file /private/path/facts.json \
  --evidence artifacts/badge-usb-hardening/acceptance.jsonl \
  --expected-version EXACT_VERSION
```

For a failed manual observation, pass `--status FAIL` with a facts object
containing a recognized privacy-safe `error` code and optional `phase` code;
no live port is required. The recorder appends and exits nonzero for FAIL; it
never rewrites earlier evidence. It rejects free-form failure text so SSIDs or
ambient RF observations cannot leak into the evidence file.

## Gate 1 — Android control and reconnect

Before Gate 1, install the exact final candidate on the uplink only. Leave both
identity-bound scanners on firmware versions strictly older than that candidate.
Do not downgrade or directly USB-flash the scanners after the acceptance
session begins. Gate 1 records this reachable pre-update state: candidate
uplink, both exact scanner boards and roles, live radios, and each strictly
older scanner version. This is the immutable starting point consumed by Gate 2
cycle 1.

With all three boards on the continuously connected battery:

- connect Android over USB;
- receive a current status and one live badge-sourced detection;
- change and restore a theme;
- remove and reconnect the USB cable;
- confirm Android exposes no firmware upload, relay, or ROM-entry action.

Record `android-control-reconnect` only after all observations are true.

## Gate 2 — three complete laptop cycles

Gate 1 must already have anchored the same acceptance session and its
strictly-older scanner pre-state in the evidence file. Without changing boards
or versions between the Gate 1 snapshot and this command, run the first
machine-gated cycle:

```bash
/Users/billh/.platformio/penv/bin/python \
  scripts/verify_badge_usb_hardening.py \
  --gate three-update-cycles \
  --cycle 1 \
  --port /dev/cu.usbmodemXXXX \
  --session-file /private/path/session.json \
  --evidence artifacts/badge-usb-hardening/acceptance.jsonl
```

Cycle 1 fails closed unless both connected scanner slots still match the
identity-bound, strictly-older state required by Gate 1. The verifier invokes
the fixed production USB flow, requires exactly one scanner upload with slot
mask `3`, proves both older scanners converged through the coordinator with at
least one attempt each and zero host relay commands, restores the theme,
re-queries the live badge, and appends a machine-issued `CHECKPOINT`.

Before any repository, artifact, or device operation, the verifier proves that
the selected session contains the exact next allowed cycle prefix with no
earlier failure, duplicate, or out-of-order record. It then acquires two
independent state layers.

The diagnostic mirror remains in the flasher's fixed private Application
Support root:

```text
~/Library/Application Support/FoF Badge Flasher/acceptance-state
```

The verifier securely opens or creates every state-root component relative to
the trusted home directory, rejects symlinks and group/world-writable
components, and synchronizes each newly created directory and its parent.
Mirror names have the form `op-<sha256>.retained`.

The non-detachable run-once authority is the hidden append-only registry:

```text
~/.fof-badge-acceptance-operation-registry-v3.jsonl
```

Here `~` is resolved from the effective user's account record, never a
caller-supplied `HOME`. The registry is a direct child of that home, whose
parent must be root-owned and not group/world writable. It is one private
owner-owned regular file, opened no-follow relative to held directory
capabilities, locked for the entire mutating gate, and protected by the macOS
`UF_APPEND` kernel flag. The file and name/inode binding are revalidated before
every append. There is no CLI option for relocating either state layer.

While holding both registry locks, the verifier requires one global
arming-protocol fence with the exact privacy-safe record
`{"event":"ARMING_PROTOCOL_FENCE","schema":3}`. A fresh registry synchronizes
that fence before its first `PREPARED`. For an existing registry with no
fence:

- every historical `STARTED` remains permanently terminal;
- historical `CANCELLED` transitions remain unchanged;
- if no operation is currently `PREPARED`, the verifier appends the one fence
  with `fsync` and macOS `F_FULLFSYNC`, but every exact operation whose
  `PREPARED` lineage predates that fence remains legacy-ambiguous even when
  its current transition is `CANCELLED`; and
- if any operation is still `PREPARED`, the registry is legacy-ambiguous.
  The verifier appends nothing, dispatches nothing, and requires manual
  investigation from both original and copied evidence paths.

The fence is never relocated to a new filename and no old history is ignored.
It globally proves which later `PREPARED` attempts were created under the
arm-before-sidecar protocol. The parser rejects a duplicate fence, extra fence
fields, a fence placed while any earlier operation is still `PREPARED`, or a
post-fence retry of an operation whose most recent `CANCELLED` attempt began
before the fence.

The registry uses strict durable transitions bound to a random attempt ID:

- `PREPARED` reserves an operation before fallible pre-action setup.
- `RESERVATION_ARMED` is a durable event on the matching `PREPARED`
  attempt. It is appended and fully synchronized before sidecar creation can
  begin. The operation's logical state remains `PREPARED`.
- `CANCELLED` follows only a clean pre-action abort after the mirror and
  transient evidence reservation are both proven absent or safely removed.
  Only an unarmed stale `PREPARED` recorded after the global fence is
  recovered this way on the next invocation.
- `STARTED` follows only the exact matching armed `PREPARED`, is synchronized
  with both `fsync` and macOS `F_FULLFSYNC`, and is permanently terminal.

A complete unarmed post-fence `PREPARED` whose sync reports failure can be
recovered because neither sidecar creation nor a hardware callback was yet
legal. A pre-fence `PREPARED` is always ambiguous, even when it has no
`RESERVATION_ARMED` event: the preceding verifier version could create a
durable sidecar before that event existed. It is never automatically
cancelled.

A pre-fence `PREPARED` followed by `CANCELLED` is also ambiguous for that
exact operation. An older verifier could swallow a reservation-cleanup
failure and append `CANCELLED` while the original evidence sidecar remained;
a copied evidence path cannot disprove that state. The historical transition
is preserved and the global fence may still authorize unrelated operations,
but the same operation digest can never append a new `PREPARED` without
manual state repair.

An armed stale post-fence `PREPARED` is also not automatically recovered:
process loss can occur after the sidecar becomes durable but before `STARTED`,
and a copied evidence path cannot prove that the original sidecar was removed.
It therefore remains `PREPARED`, retains the mirror and any sidecar, and
requires explicit manual investigation. This deliberately also makes a crash
after arming but before sidecar creation fail closed rather than guessing that
cleanup is safe.

A malformed or torn registry tail, including a torn fence, likewise fails
closed; it is never guessed to be `CANCELLED` or silently discarded. If a
complete fence write reports a full-sync failure, that invocation dispatches
nothing. A later successful post-fence `PREPARED` full-sync also flushes the
earlier complete fence on the same append-only descriptor before sidecar
creation becomes legal.

`STARTED` is the final prerequisite and is durably re-read before the hardware
callback is dispatched. Replacing the Application Support root or an ancestor
immediately after its final validation therefore detaches only the mirror: a
copied pristine evidence/session invocation still observes terminal `STARTED`
and refuses to run.

The operation digest shared by both layers is computed from domain-separated
canonical JSON containing the schema, session ID, normalized
uplink/BLE/Wi-Fi hardware IDs, exact firmware version, platform and firmware
target identifiers, gate, phase, and cycle. The registry contains only that
digest, random attempt IDs, schema, transition/event names, and the global
fence. Neither state layer exposes a raw session ID, MAC address, session path,
or evidence path. Copying or renaming the session or evidence file, changing
path spelling or case, or nesting the session cannot create a second operation
namespace.

The mirror is created with exclusive no-follow semantics. Cleanup ownership is
constructed immediately after that exclusive open, before the first `fstat`.
Its content, file, and containing directory are synchronized and its identity
is revalidated. Any clean failure before `STARTED` first proves the transient
reservation absent or validates and removes it, then validates and removes
exactly the mirror and synchronizes both directories before appending
`CANCELLED`. A reservation-removal failure is never discarded and never
permits mirror cleanup or `CANCELLED`; it raises the manual-repair error while
the armed `PREPARED` and mirror remain as blockers, with the sidecar retained
or restored whenever its canonical pathname can still be secured.

Once `STARTED` is present, neither it nor the mirror is removed—not after PASS,
FAIL, an ordinary exception, `KeyboardInterrupt`, process termination, or
power loss. Cycles 1, 2, and 3 have distinct digests, so later cycles remain
possible while the same cycle cannot run twice through the cooperative
verifier.

`UF_APPEND` is an owner-controlled macOS flag. This design protects the tested
namespace-replacement schedules and cooperating invocations, but it is not a
privilege boundary against a malicious process already running as the same
user; that process can deliberately clear the flag or alter the verifier
itself. A root-managed service or system append flag would be required for
that stronger threat model. Operators must never clear the registry flag,
rename the registry, or edit its contents.

The verifier also holds a private transient sidecar named
`.<evidence-filename>.mutating-gate-reservation` beside the evidence file (for
the default path, `.acceptance.jsonl.mutating-gate-reservation`). That sidecar
binds the open evidence inode to the session, gate, phase, and cycle and guards
in-process evidence recording. It is synchronized before the action. Before
`STARTED`, it may be removed only by the verified cleanup sequence that leads
to `CANCELLED`; after `STARTED`, it may be removed only after durable PASS or
FAIL evidence. If its cleanup becomes indeterminate after `STARTED`, it remains
as an additional blocker and terminal registry `STARTED` remains authoritative.
If cleanup becomes indeterminate before `STARTED`, the armed registry attempt
remains logically `PREPARED` and blocks every copied-path retry. Reserved
checkpoint recording requires the live reservation capability, not a
caller-supplied evidence descriptor.

Do not delete a retained mirror or clear/edit the registry to retry an
operation. To abandon and manually reset an acceptance session, archive its
evidence and session file for diagnosis, then create a new session ID and
repeat the physical acceptance run from Gate 1. Registry history and retained
mirrors are expected to accumulate. Deleting or editing state cannot convert
an indeterminate action into a valid gate.

If registry validation or pre-action cleanup fails, stop the run. Preserve the
session file, evidence, registry bytes, registry hash, owner/mode/flags, and
mirror and sidecar trees, including their bytes, hashes, owners, and modes;
mark that session failed; and escalate the bundle to the release maintainer.
The verifier intentionally has no repair, clear, relocate, or force-retry
option. Any maintainer-approved host-state rebuild is a discarded acceptance
run and must use a new session ID and repeat every gate.

Without changing versions or boards, run cycles 2 and 3:

```bash
/Users/billh/.platformio/penv/bin/python \
  scripts/verify_badge_usb_hardening.py \
  --gate three-update-cycles \
  --cycle 2 \
  --port /dev/cu.usbmodemXXXX \
  --session-file /private/path/session.json \
  --evidence artifacts/badge-usb-hardening/acceptance.jsonl

/Users/billh/.platformio/penv/bin/python \
  scripts/verify_badge_usb_hardening.py \
  --gate three-update-cycles \
  --cycle 3 \
  --port /dev/cu.usbmodemXXXX \
  --session-file /private/path/session.json \
  --evidence artifacts/badge-usb-hardening/acceptance.jsonl
```

Cycles 2 and 3 fail closed unless both scanners begin exactly current. Their
same-version recovery rewrites are selected internally; do not invoke the
flasher directly or add a manual recovery/relay flag. Each successful cycle
appends one ordered `CHECKPOINT`. When cycle 3 succeeds, its checkpoint and the
sole aggregate `three-update-cycles` `PASS` are appended in one locked
transaction and synchronized before the lock is released.

Every checkpoint proves the exact uplink/scanner versions and immutable IDs,
strictly advancing stage generation, clear rollback state,
BLE-primary/Wi-Fi-primary slot roles, live radios, fresh PING/STATUS responses,
the requested stage slot mask, the observed final coordinator pending mask,
and exact theme restoration. It does not claim an unobserved pre-stage pending
mask. Caller-authored Gate 2 facts are rejected.

## Gate 3 — interrupted application upload

This deterministic fault injection is intentionally unavailable in the normal
flasher:

```bash
/Users/billh/.platformio/penv/bin/python \
  scripts/verify_badge_usb_hardening.py \
  --gate interrupted-upload \
  --abort-after 65536 \
  --port /dev/cu.usbmodemXXXX \
  --session-file /private/path/session.json \
  --evidence artifacts/badge-usb-hardening/acceptance.jsonl
```

The selected session must already have the exact Gate 1 plus completed Gate 2
evidence prefix. The explicit private `--session-file` is mandatory; the
command never derives a new identity from the evidence or currently attached
boards. A conflicting session, unsafe/shared anchor, prior failure, duplicate,
or out-of-order record is rejected before any artifact or device access. Gate 3
uses the same append-only registry authority, retained diagnostic mirror,
transient evidence sidecar, path binding, abnormal-exit, and no-retry rules
described for Gate 2.

The command writes exactly 65,536 bytes, requires the matching `credit`
receipt with `received=65536`, the exact target partition, total size, and next
credit within one monotonic overall deadline, and never writes byte 65,537. It
then closes the serial connection without a terminal command, waits seven
seconds for the five-second device idle abort, reconnects by immutable uplink
MAC, proves the old partition/version and scanner cache are unchanged,
requires parser state `command`, retries the uplink application OTA, and
records PASS only after the retry boots cleanly.

## Gate 4 — confirmed chord-to-ROM recovery

With a live USB data host attached, hold OK+Menu for 10 seconds, release both,
then press OK at `USB FLASH? OK=YES MENU=RESET`. Require:

- protocol-proven ROM presence and baseline base-MAC continuity;
- complete four-region uplink write plus independent readback verification;
- application return with `last_expected_reboot_reason=button_usb_rom`;
- one scanner stage and both UART updates;
- no battery disconnect.

Record `chord-rom-recovery` only after the complete path returns healthy.

The machine-run Gate 4 command requires the same private session used by Gates
1–3 and their exact completed evidence prefix. Put the uplink into ROM with the
physical chord first. After pressing OK once, do not press either button again
and do not power-cycle the badge. The LCD/backlight can remain frozen on
`USB FLASH?`, and macOS can retain the same `/dev/cu.usbmodem...` path,
descriptor identity, and inode across the application-to-ROM transition. That
unchanged appearance is not evidence of failure. Run the command immediately
against the exact same session-bound path unless a complete descriptor census
proves the same serial and physical USB location at a new path:

```bash
/Users/billh/.platformio/penv/bin/python \
  scripts/verify_badge_usb_hardening.py \
  --gate chord-rom-recovery \
  --run-chord-rom-recovery \
  --port /dev/cu.usbmodemXXXX \
  --session-file /private/path/session.json \
  --evidence artifacts/badge-usb-hardening/acceptance.jsonl
```

The runner freezes and validates the exact candidate artifacts before its USB
census, requires the selected ROM descriptor to match the retained uplink
identity, and strengthens that binding to the descriptor's physical USB
location before opening it. It actively probes that selected descriptor even
when the pathname and device metadata did not change. A responsive application
is rejected before any mutation, so this gate cannot silently fall back to
application OTA. PASS is derived only after the guarded four-region
write/readback, application return, one scanner stage, two same-version UART
relays, final scanner health proof, and `button_usb_rom` reboot evidence.
Evidence is reserved and appended atomically; an interrupted or uncertain
mutation never records PASS.

If an operator power-cycles before the ROM probe completes, the physical gate
must be repeated; retained application status is diagnostic only and cannot
replace the guarded write/readback. Interpret
`last_expected_reboot_reason` as follows:

- `button_usb_rom`: the prompt accepted OK and requested ROM; investigate the
  host probe/descriptor handling rather than changing the chord.
- `button_reboot`: Menu, both buttons at the prompt, or the prompt timeout
  selected an application reboot.
- an unchanged older value: the current confirmation was not accepted.

## Gate 5 — no-host normal reboot

Without a data host, hold OK+Menu for 10 seconds. Repeat with a power-only
charger. Both cases must reboot the application rather than wait in ROM. After
reconnecting, require
`last_expected_reboot_reason=button_reboot`.

Record `no-host-reboot` only from the returned machine status, not solely from
watching the display.

## Gate 6 — power-state audit

Confirm for the entire run:

- the battery remained connected;
- the physical chord never entered quiet or power-off mode;
- no persistent safe mode or reboot loop occurred.

Record `power-state-audit` last.

## Final read-only evidence audit

After Gate 6, audit the completed file without connecting to or changing a
badge:

```bash
/Users/billh/.platformio/penv/bin/python \
  scripts/verify_badge_usb_hardening.py \
  --verify-complete \
  --session-file /private/path/session.json \
  --evidence artifacts/badge-usb-hardening/acceptance.jsonl \
  --expected-version EXACT_FINAL_VERSION
```

The selected session must contain exactly nine records in this order: Gate 1
`PASS`; Gate 2 cycle 1, 2, and 3 `CHECKPOINT`s; one Gate 2 aggregate `PASS`;
then Gate 3, Gate 4, Gate 5, and Gate 6 `PASS` records. Any extra, missing,
duplicated, failed, out-of-order, wrong-version, wrong-binding, or
semantically inconsistent record fails the audit. `--verify-complete` is
read-only and does not accept a port, facts file, cycle, or non-PASS status.
The audit revalidates the evidence pathname after acquiring its shared lock and
again immediately before returning success, including a final check that no
transient evidence-sidecar reservation appeared. The operation-specific
fixed-state mirrors and append-only registry history are expected to remain
after successful mutating gates and are not completion failures.

## Promotion rule

Any missing or failed gate blocks factory bundle replacement, tagging, pushing,
and publishing. A user-authorized local provisional source identity may be
used only for canary validation; it does not change backend readiness, public
manifests, factory artifacts, or the status of any physical gate. After all
provisional gates pass, select the final release identity, rebuild, repeat
every automated and physical gate on the exact final binaries, run
`--verify-complete` against that exact final version, compare hashes, and only
then promote the factory and release artifacts. Provisional evidence never
authorizes a factory-bundle replacement, tag, push, or release.
