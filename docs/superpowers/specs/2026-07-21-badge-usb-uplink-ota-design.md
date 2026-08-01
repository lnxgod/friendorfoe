# Badge USB Uplink OTA Design

## Goal

Expose the isolated uplink OTA adapter through a dedicated USB line-and-binary
protocol in normal and startup recovery-only modes without changing scanner
upload behavior or the existing `FOF_CTL` `ota` ROM-download alias.

## Chosen architecture

Use an uplink-only transactional coordinator layered beside the existing
scanner upload path. The alternatives were to make the generic stream parser
transactional for both targets, which risks scanner regressions, or to add a
second USB reader/ring, which duplicates framing and consumes more internal
RAM. The chosen coordinator adds a two-phase uplink peek/commit API to the
existing stream: bytes are exposed without changing parser ownership, and are
committed only after the adapter durably accepts them.

The dedicated command is exactly
`FOF_CTL:{"cmd":"uplink_ota_begin",...}`. `serial_config` classifies and
strictly parses its bounded manifest. Both normal and recovery-only transport
branches call the same dedicated handler; recovery-only never invokes the full
control dispatcher. The handler begins the adapter, emits and drains READY,
then arms the binary parser and coordinator policy before returning to the read
loop. A parser-arm failure aborts the adapter and emits one terminal result.

## Binary and credit flow

The coordinator owns the transport count, one credit window, receipt fence,
finish latch, cleanup latch, and terminal-receipt latch. Its action enum is:

- `CONTINUE`: accept another chunk from the current credit.
- `RETRY_PENDING`: retain the exact attempted chunk and perform no new read.
- `WAIT_RECEIPT`: emit and drain READY or CREDIT before activating credit.
- `FINISH`: the exact image is durable; call adapter finish once while parser
  ownership is still uplink.
- `ABORT_DROP`: abort/terminalize once, clear parser and upload state, and drop
  the remainder of the current OS read.
- `COMMITTED_RESTART`: attempt committed output and drain, then restart the app
  unconditionally.

Each adapter write is at most 512 bytes. A read containing bytes beyond the
current 4096-byte credit or final short window is a protocol violation; it is
aborted once and the entire unread remainder is dropped, never interpreted as
line commands. Retryable `PHASE_NONE` adapter results retain an exact pending
chunk without advancing stream, offset, upload accounting, or durable count.
The timeout predicate is nonmutating so adapter cleanup precedes parser clear.

## Results and cleanup

Every uplink adapter response uses one `FOF_UPLINK_OTA:` prefix and bounded
JSON. READY, CREDIT, BUSY, ABORTED, and COMMITTED include the applicable
partition, durable received/total, credit, retryable, and reboot-required
fields. `FOF_UPLINK_UPLOAD` is not used by the dedicated path.

Adapter terminal results are emitted once. Transport/backend/timeout failures
retry abort without accepting new input until exact cleanup becomes terminal;
stream and upload health clear once. A committed result is an
irreversible boundary: no abort, resume, or operation release follows it.
Emission and drain are both attempted, then app restart occurs regardless of
either result.

If explicit USB rollback unexpectedly returns, runtime clears both the RTC
expected-reboot marker and persisted expected reason, emits a terminal failure,
and leaves the USB recovery surface running.

## Scope boundaries

Task5D HTTP/non-USB mutation lockdown and status are excluded. Laptop/Android
sender work, flashing, publishing, tags, and version changes are excluded.
The scanner upload path remains on its legacy uncredited begin and existing
stream-feed behavior. Startup firmware-operation token ordering is unchanged.

## Verification

Native tests cover manifest field validation, bounded frames, transactional
stream behavior, 4095+1 credit boundaries, 5000-byte final-short flow, exact
credit multiples, extra bytes, complete-plus-line reads, retry-pending chunks,
receipt failures, timeout ordering, cleanup/terminal latches, and committed
emit/drain failure restart. Focused source contracts prove normal/recovery
routing, dedicated parsing, scanner isolation, rollback-return handling, and
the irreversible restart ordering. Full native ASan/backend suites, clean
badge/non-badge builds, and strict manifest verification close the gate.
