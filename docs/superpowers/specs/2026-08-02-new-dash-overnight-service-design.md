# New Dash Overnight Service Design

**Date:** 2026-08-02
**Status:** Approved for implementation

## Goal

Keep the standalone New Dash process running overnight on macOS with one USB
badge, automatically restart it after a process crash, and preserve every
accepted detection in the existing SQLite history for review the next morning.

## Scope

Add two public source scripts under `new-dash/`:

- `start.sh` prepares and starts the supervised service.
- `stop.sh` stops and unregisters the supervised service.

The existing foreground `run.sh`, USB protocol, browser application, storage
schema, legacy backend, Android application, and firmware remain unchanged.

## Runtime design

`start.sh` installs or refreshes the existing editable virtual environment,
then writes and registers a per-user macOS LaunchAgent named
`io.friendorfoe.new-dash`. The generated plist lives at:

```text
~/Library/LaunchAgents/io.friendorfoe.new-dash.plist
```

The LaunchAgent runs an internal service mode in `start.sh` using absolute
paths. Service mode executes New Dash through `/usr/bin/caffeinate -i`, which
prevents idle system sleep while the process is alive. It always supplies
`--no-browser` because the browser can remain open independently.

The LaunchAgent uses `RunAtLoad`, `KeepAlive`, and a short throttle interval.
If New Dash exits or crashes, `caffeinate` exits with it and `launchd` starts a
new service generation. A deliberate `stop.sh` unloads the LaunchAgent, so the
intentional stop is not restarted.

The default HTTP port is `18888` to preserve the accepted dashboard URL.
Automatic badge discovery remains the default for the single-badge workflow.
`start.sh` accepts only these optional overrides:

```text
--http-port PORT
--port /dev/cu.usbmodem...
```

Values are validated before they are stored. The small generated service
configuration is kept beneath the existing New Dash Application Support
directory and is read as data, never evaluated as shell code.

## Data and logs

The service does not override New Dash's existing database location or
retention policy:

```text
~/Library/Application Support/New Dash/new-dash.sqlite3
```

Consequently, Remote ID, Find My, and other accepted USB detections remain
available in History after a service restart. Stopping the service never
deletes history.

Standard output and error are written beneath:

```text
~/Library/Logs/New Dash/
```

`start.sh` prints the dashboard URL and log paths. `stop.sh` removes the
generated LaunchAgent and its service configuration but preserves logs,
history, the virtual environment, and source files.

## Safety and lifecycle behavior

- Repeated `start.sh` calls replace only the LaunchAgent owned by New Dash.
- `stop.sh` never kills a process by a broad name match and never touches an
  unrelated USB owner.
- A missing badge is not fatal; the existing serial reconnect loop continues.
- An occupied requested HTTP port is logged and retried by `launchd` rather
  than silently selecting a different URL.
- Startup failures remain visible in the persistent error log.
- The scripts do not require root privileges or modify system-wide services.
- `caffeinate -i` prevents idle sleep, but a normal MacBook can still suspend
  when its lid is closed. Overnight capture requires power and an open lid (or
  a separately configured supported clamshell setup).

## Final verification

Verification happens once after implementation:

1. Check both scripts with the POSIX shell syntax checker.
2. Start the LaunchAgent and verify `launchctl` reports it running.
3. Verify port `18888`, the selected USB device, fresh status, and a persisted
   detection through the real dashboard API.
4. Terminate one service generation and verify `launchd` replaces it.
5. Run `stop.sh` and verify the service stays stopped without deleting history.
6. Start it again and leave that supervised generation running overnight.

The existing full application test suites need not be rerun because this
change does not alter application code; the final script and physical-service
checks directly cover the changed lifecycle behavior.
