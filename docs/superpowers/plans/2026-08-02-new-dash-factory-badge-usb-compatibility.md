# New Dash Factory Badge USB Compatibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make New Dash communicate with the existing factory badge firmware exactly as the working Android/debug-bridge path does, without reflashing or changing the badge.

**Architecture:** Keep the current one-owner serial lifecycle, exact USB discovery, post-write PONG verification, polling, parsing, and browser application unchanged. The compatibility fix is only to leave DTR and RTS at PySerial's adapter defaults before and after `open()`, backed by a fake-serial regression that rejects any future control-line mutation and by a read-only factory-badge smoke test.

**Tech Stack:** Python 3.11+, PySerial 3.x, `unittest`, existing New Dash HTTP/browser client, factory `fof_badge_uplink` firmware `0.67.2-badge-defcon34` on macOS.

## Global Constraints

- This plan supersedes only the DTR/RTS requirements in `docs/superpowers/plans/2026-08-02-new-dash.md`; all other approved New Dash constraints remain in force.
- New Dash must not assign `serial_port.dtr`, assign `serial_port.rts`, call `serial_port.setDTR(...)`, or call `serial_port.setRTS(...)` before or after opening the port.
- Keep nominal baud 115200, read timeout 0.1 seconds, write timeout 3.0 seconds, exclusive access when supported, stale-input clearing, and the exact verification write `b"\nFOF_PING\n"`.
- Keep the three-second post-write nonempty `FOF_PONG` identity check; boot logs, stale input, empty PONGs, and other Espressif devices must not verify.
- Do not change, flash, reboot, reset, or send display-control mutations to the factory badge during acceptance testing.
- Do not modify `android/`, `backend/`, `esp32/`, or `scripts/`.
- Every production behavior change follows red-green-refactor: observe the focused regression fail, apply the minimum fix, then run focused and full suites.

---

## File and Interface Map

- `new-dash/tests/fakes.py`
  - Keep `FakeSerial` as an unopened PySerial-compatible test double.
  - Permit `open()` while `_dtr` and `_rts` remain untouched (`None`) so tests model adapter-default signaling.
  - Continue recording any property or method mutation in `actions` so regressions can detect it.
- `new-dash/tests/test_serial_transport.py`
  - Assert the safe configuration sequence goes directly from exclusive access to `open()`, input reset, and PING.
  - Assert no `dtr`, `rts`, `setDTR`, or `setRTS` action occurs, including when `exclusive` is unsupported.
- `new-dash/src/new_dash/serial_transport.py`
  - `BadgeSerialTransport._open_safely(identity: PortIdentity) -> Any` configures all non-control-line settings and opens the handle.
  - `BadgeSerialTransport._run_session(stop_event: threading.Event) -> _SessionOutcome` resets stale input and performs the unchanged PING/PONG verification.
- `new-dash/README.md`
  - State that New Dash uses the same factory-firmware command signaling as the Android/debug-bridge path and does not require a badge firmware change.

---

### Task 1: Lock In Factory-Compatible Serial Signaling

**Files:**
- Modify: `new-dash/tests/fakes.py:136-159`
- Modify: `new-dash/tests/test_serial_transport.py:178-225`
- Modify: `new-dash/tests/test_serial_transport.py:512-527`
- Modify: `new-dash/src/new_dash/serial_transport.py:449-451`
- Modify: `new-dash/src/new_dash/serial_transport.py:896-910`
- Modify: `new-dash/README.md:1-18`

**Interfaces:**
- Consumes: the existing `serial_factory: Callable[[], Any]`, unopened PySerial-compatible handle, `PortIdentity`, and `_write_if_running(...)` handshake flow.
- Produces: unchanged `BadgeSerialTransport` public behavior, but with adapter-default DTR/RTS signaling compatible with Android and factory badge firmware.

- [ ] **Step 1: Make the fake model adapter defaults and write the failing regression**

Remove the control-line assertion from `FakeSerial.open()` while retaining the recording property setters and methods:

```python
def open(self) -> None:
    if self.is_open:
        raise RuntimeError("already open")
    self.actions.append(("open",))
    if self.open_exception is not None:
        raise self.open_exception
    self.is_open = True
    self.opened.set()
```

Rename the first verified-session test to `test_factory_compatible_configuration_leaves_control_lines_at_defaults` and replace its action assertion with:

```python
expected_before_ping = [
    ("set", "port", "/dev/cu.usbmodem101"),
    ("set", "baudrate", 115200),
    ("set", "timeout", 0.1),
    ("set", "write_timeout", 3.0),
    ("set", "exclusive", True),
    ("open",),
    ("reset_input_buffer",),
]
self.assertEqual(fake.actions[: len(expected_before_ping)], expected_before_ping)
self.assertEqual(
    fake.actions[len(expected_before_ping)],
    ("write", b"\nFOF_PING\n"),
)
control_line_actions = [
    action
    for action in fake.actions
    if action[0] in {"setDTR", "setRTS"}
    or action[:2] in {("set", "dtr"), ("set", "rts")}
]
self.assertEqual(control_line_actions, [])
```

In `test_unsupported_exclusive_attribute_preserves_safe_open_sequence`, replace the old pre-open DTR/RTS ordering assertions with the same `control_line_actions` filter and `self.assertEqual(control_line_actions, [])`.

- [ ] **Step 2: Run the focused tests and verify the regression fails for the intended reason**

Run:

```sh
cd new-dash
PYTHONPATH=src python3 -m unittest \
  tests.test_serial_transport.VerifiedSessionTest.test_factory_compatible_configuration_leaves_control_lines_at_defaults \
  tests.test_serial_transport.VerifiedSessionTest.test_unsupported_exclusive_attribute_preserves_safe_open_sequence -v
```

Expected: both tests fail because `BadgeSerialTransport` still records `("set", "dtr", False)`, `("set", "rts", False)`, `("setDTR", False)`, and `("setRTS", False)`.

- [ ] **Step 3: Apply the minimum production fix**

In `_run_session`, change the open/reset sequence to:

```python
serial_port = self._open_safely(identity)
serial_port.reset_input_buffer()
```

In `_open_safely`, keep the existing port, baud, timeout, write-timeout, and best-effort exclusive configuration, then open without touching control lines:

```python
try:
    serial_port.exclusive = True
except (AttributeError, NotImplementedError):
    pass
serial_port.open()
return serial_port
```

- [ ] **Step 4: Run the focused regression and complete serial suite**

Run:

```sh
cd new-dash
PYTHONPATH=src python3 -m unittest \
  tests.test_serial_transport.VerifiedSessionTest.test_factory_compatible_configuration_leaves_control_lines_at_defaults \
  tests.test_serial_transport.VerifiedSessionTest.test_unsupported_exclusive_attribute_preserves_safe_open_sequence -v
PYTHONPATH=src python3 -m unittest tests.test_serial_transport -v
```

Expected: the two focused tests pass and all 60-plus serial transport tests pass.

- [ ] **Step 5: Document unchanged-firmware compatibility**

After the introductory paragraph in `new-dash/README.md`, add:

```markdown
New Dash uses the same factory-firmware command signaling as the working
Android USB path and `scripts/fof_badge_debug_bridge.py`. Existing factory
badges do not need to be reflashed or reconfigured.
```

- [ ] **Step 6: Run all automated verification**

Run:

```sh
cd new-dash
PYTHONPATH=src python3 -m unittest discover -s tests -v
python3 -m compileall -q src tests
node --test tests/browser_behavior_test.mjs
```

Expected: all Python and browser tests pass, apart from the existing environment-dependent occupied-port skip when that port is already owned; compileall exits zero.

- [ ] **Step 7: Review scope and commit the compatibility fix**

Run:

```sh
git diff --check
git status --short
git diff -- new-dash/src/new_dash/serial_transport.py new-dash/tests/fakes.py new-dash/tests/test_serial_transport.py new-dash/README.md
git add new-dash/src/new_dash/serial_transport.py new-dash/tests/fakes.py new-dash/tests/test_serial_transport.py new-dash/README.md
git commit -m "new-dash: support factory badge USB signaling"
```

Expected: only the four listed New Dash files are committed; legacy application and firmware directories have no diff.

---

### Task 2: Prove the Factory Badge and Browser Path End to End

**Files:**
- Verify only: factory badge at `/dev/cu.usbmodem1101`
- Verify only: `new-dash/src/new_dash/__main__.py`
- Verify only: `new-dash/src/new_dash/static/index.html`

**Interfaces:**
- Consumes: the plugged-in factory badge, New Dash CLI, `GET /api/state`, and the loopback browser UI.
- Produces: read-only evidence for verified PONG, repeated status, captured detections, browser-visible connection state, and physical disconnect/reconnect recovery.

- [ ] **Step 1: Confirm the intended single factory badge port is present and unowned**

Run:

```sh
cd new-dash
.venv/bin/python -m serial.tools.list_ports -v
lsof /dev/cu.usbmodem1101
```

Expected: `/dev/cu.usbmodem1101` is the one connected Espressif USB Serial/JTAG uplink; `lsof` prints no owner. Stop and report instead of guessing if the port is absent, owned, or multiple physical badge devices are present.

- [ ] **Step 2: Launch the exact source tree against the factory badge**

Run New Dash as a long-lived process:

```sh
cd new-dash
./run.sh --no-browser --http-port 18888 --port /dev/cu.usbmodem1101 --data-dir /private/tmp/new-dash-factory-smoke
```

Expected: the server binds `http://127.0.0.1:18888/` and the serial worker remains running without `wrong_device`.

- [ ] **Step 3: Verify live firmware identity and two distinct status snapshots**

Fetch `/api/state` twice at least 2.5 seconds apart and assert each envelope has:

```python
state = envelope["data"]
assert state["connection"]["phase"] == "live"
assert state["connection"]["port"] == "/dev/cu.usbmodem1101"
assert state["connection"]["firmware_version"] == "0.67.2-badge-defcon34"
assert state["freshness"]["state"] == "fresh"
assert state["status"]["version"] == "0.67.2-badge-defcon34"
assert state["status"]["uptime_s"] is not None
```

Expected: both assertions pass and the second `uptime_s` is greater than the first, proving repeated `FOF_STATUS` capture rather than a single boot response.

- [ ] **Step 4: Verify a read-only detection reaches the HTTP snapshot**

Poll `GET /api/state` for up to 45 seconds without sending a mutation and accept either a nonempty `recent_events` array or a nonempty `status.entities` array:

```python
assert state["recent_events"] or state["status"]["entities"]
```

Expected: the factory badge's observed radio data reaches New Dash. If no ambient detection occurs in 45 seconds, report the inconclusive environmental condition separately; do not fabricate a detection or weaken parsing.

- [ ] **Step 5: Verify the rendered browser state**

Open `http://127.0.0.1:18888/` with the in-app browser and inspect the rendered page. Assert:

```javascript
await page.locator("#connection-phase").textContent() === "Live"
await page.locator("#connection-firmware").textContent() === "FW 0.67.2-badge-defcon34"
await page.locator("#connection-port").textContent() === "/dev/cu.usbmodem1101"
```

Also confirm the Live view contains the `Remote ID`, `Drone clues`, and `Other observations` sections so any future Remote ID entity has a visible destination.

- [ ] **Step 6: Verify physical disconnect and reconnect recovery**

Ask the user to unplug the badge, observe `/api/state` leave `live`, then ask them to reconnect the same badge. Within the existing retry window, assert the state returns to:

```python
assert state["connection"]["phase"] == "live"
assert state["connection"]["port"] == "/dev/cu.usbmodem1101"
assert state["freshness"]["state"] == "fresh"
```

Expected: New Dash closes the detached handle, rediscovers the same physical identity, obtains a fresh PONG, and resumes status polling without restarting the process.

- [ ] **Step 7: Stop cleanly and record the evidence**

Send one interrupt to the New Dash process and wait for it to exit.

Expected: HTTP, serial, application, and SQLite workers stop cleanly; `lsof /dev/cu.usbmodem1101` again prints no owner. Record exact automated totals, firmware version, port, repeated-status result, detection result, browser result, and reconnect result in the implementation handoff.
