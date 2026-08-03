# New Dash Overnight Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add start/stop scripts that run New Dash as a crash-restarting, sleep-inhibiting per-user macOS service and leave it collecting USB badge history overnight.

**Architecture:** `new-dash/start.sh` has a public installer/controller path and a private `--service` path used by a generated LaunchAgent. The LaunchAgent owns a `caffeinate -i`-wrapped New Dash process with `KeepAlive`; `new-dash/stop.sh` unloads only that exact service and removes generated service metadata without touching history or logs.

**Tech Stack:** POSIX `sh`, macOS `launchctl`, LaunchAgent plist XML, `caffeinate`, Python virtual environment, existing New Dash Python/SQLite application.

## Global Constraints

- macOS is the only supported service platform in this release.
- LaunchAgent label is exactly `io.friendorfoe.new-dash`.
- Default HTTP endpoint is exactly `http://127.0.0.1:18888/`.
- Automatic single-badge discovery is the default; the only device override is `--port /dev/cu.*`.
- `start.sh` accepts only `--http-port PORT` and `--port PATH`; service mode is private.
- Background mode always supplies `--no-browser`.
- History remains at `~/Library/Application Support/New Dash/new-dash.sqlite3` with existing retention defaults.
- Logs remain beneath `~/Library/Logs/New Dash/`.
- `stop.sh` preserves history, logs, source, and `.venv`.
- Do not modify the legacy backend, Android application, firmware, New Dash protocol, storage schema, or browser application.
- Per user direction, do not use TDD or rerun the application suites; implement first and perform the single final lifecycle/hardware verification in Task 2.

---

### Task 1: Add the macOS service controllers and operator documentation

**Files:**
- Create: `new-dash/start.sh`
- Create: `new-dash/stop.sh`
- Modify: `new-dash/README.md`

**Interfaces:**
- Consumes: existing `new-dash/.venv`, `python3`, `new_dash` module, default SQLite path, and serial reconnect behavior.
- Produces: `./start.sh [--http-port PORT] [--port /dev/cu.*]`, internal `./start.sh --service`, and idempotent `./stop.sh`.

- [ ] **Step 1: Implement `start.sh` public argument and environment handling**

Use POSIX shell with `set -eu`. Resolve the source directory from the script
location, reject non-Darwin systems, require a nonempty existing `HOME`, and
define these exact paths:

```sh
NEW_DASH_LABEL=io.friendorfoe.new-dash
NEW_DASH_DOMAIN="gui/$(id -u)"
NEW_DASH_TARGET="$NEW_DASH_DOMAIN/$NEW_DASH_LABEL"
NEW_DASH_SUPPORT_DIR="$HOME/Library/Application Support/New Dash"
NEW_DASH_CONFIG="$NEW_DASH_SUPPORT_DIR/service.conf"
NEW_DASH_LOG_DIR="$HOME/Library/Logs/New Dash"
NEW_DASH_STDOUT="$NEW_DASH_LOG_DIR/service.log"
NEW_DASH_STDERR="$NEW_DASH_LOG_DIR/service-error.log"
NEW_DASH_PLIST="$HOME/Library/LaunchAgents/$NEW_DASH_LABEL.plist"
```

Default to HTTP port `18888` and an empty badge-port override. Parse only
`--http-port VALUE`, `--port VALUE`, and `--help`. Reject missing values,
unknown arguments, non-decimal or out-of-range ports, and explicit devices
outside `/dev/cu.*`.

- [ ] **Step 2: Implement the private `--service` execution path**

Recognize `--service` only when it is the sole argument. Read exactly two
lines from `service.conf` without evaluating them as shell:

```sh
NEW_DASH_HTTP_PORT=
NEW_DASH_BADGE_PORT=
{
    IFS= read -r NEW_DASH_HTTP_PORT
    IFS= read -r NEW_DASH_BADGE_PORT || :
} < "$NEW_DASH_CONFIG"
```

Revalidate both values, set `PYTHONUNBUFFERED=1`, construct the New Dash
arguments with `set --`, and replace the shell with:

```sh
exec /usr/bin/caffeinate -i "$NEW_DASH_VENV/bin/python" -m new_dash "$@"
```

The resulting Python arguments must always include `--no-browser` and
`--http-port`, and include `--port` only when the second configuration line is
nonempty.

- [ ] **Step 3: Implement setup, safe configuration writes, and plist generation**

In public mode, create/reuse `.venv`, run editable installation once before
registering the service, and create the support/log/LaunchAgents directories.
Use `umask 077`, temporary files in the destination directories, atomic `mv`,
and a cleanup trap.

Write only the validated HTTP port and badge path as the two configuration
lines. Generate a LaunchAgent with these exact semantic keys:

```xml
<key>Label</key><string>io.friendorfoe.new-dash</string>
<key>ProgramArguments</key>
<array>
  <string>/bin/sh</string>
  <string>$NEW_DASH_START_XML</string>
  <string>--service</string>
</array>
<key>RunAtLoad</key><true/>
<key>KeepAlive</key><true/>
<key>ThrottleInterval</key><integer>5</integer>
<key>StandardOutPath</key><string>$NEW_DASH_STDOUT_XML</string>
<key>StandardErrorPath</key><string>$NEW_DASH_STDERR_XML</string>
```

XML-escape every generated path before interpolation. Set plist and config
permissions to `600`.

- [ ] **Step 4: Implement exact LaunchAgent replacement and user output**

If `launchctl print "$NEW_DASH_TARGET"` succeeds, unload only that target.
Then register the generated plist with:

```sh
launchctl bootstrap "$NEW_DASH_DOMAIN" "$NEW_DASH_PLIST"
```

Print the dashboard URL, exact service label, stop command, and both log paths.
Do not search for, signal, or kill any unrelated process.

- [ ] **Step 5: Implement `stop.sh`**

Resolve the same label, target, plist, and config paths. If the exact target is
loaded, call:

```sh
launchctl bootout "$NEW_DASH_TARGET"
```

Treat an already-unloaded service as a successful idempotent stop. Remove only
the exact generated plist and service config. Print that history and logs were
preserved, including their locations.

- [ ] **Step 6: Document foreground and overnight operation**

Add an `Overnight macOS service` section to `new-dash/README.md` with:

```sh
./start.sh
./start.sh --http-port 18888 --port /dev/cu.usbmodem1101
./stop.sh
```

Explain crash restart, USB reconnect, `caffeinate`, persistent history/log
paths, open-lid/power requirement, and that `run.sh` remains the foreground
developer launcher. State that `start.sh` replaces only its own LaunchAgent
and that another serial owner must be stopped before overnight capture.

- [ ] **Step 7: Review the complete Task 1 diff once**

Run:

```sh
git diff --check
git diff -- new-dash/start.sh new-dash/stop.sh new-dash/README.md
```

Expected: no whitespace errors; the diff contains only the two scripts and the
documented service behavior.

- [ ] **Step 8: Commit the reviewed Task 1 implementation**

```sh
git add new-dash/start.sh new-dash/stop.sh new-dash/README.md
git commit -m "new-dash: add supervised overnight service"
```

Expected: the task review range contains one implementation commit with only
the two scripts and README update.

---

### Task 2: Perform the single final lifecycle and physical-badge verification

**Files:**
- Verify: `new-dash/start.sh`
- Verify: `new-dash/stop.sh`
- Runtime only: `~/Library/LaunchAgents/io.friendorfoe.new-dash.plist`
- Runtime only: `~/Library/Application Support/New Dash/service.conf`
- Runtime only: `~/Library/Logs/New Dash/`

**Interfaces:**
- Consumes: completed Task 1 scripts and the currently attached badge at `/dev/cu.usbmodem1101`.
- Produces: one supervised overnight service at `http://127.0.0.1:18888/` with crash restart and persistent history proven.

- [ ] **Step 1: Run the final static script checks**

Run once after both scripts and README are complete:

```sh
/bin/sh -n start.sh
/bin/sh -n stop.sh
git diff --check
```

Expected: every command exits `0` with no output.

- [ ] **Step 2: Migrate the known manual server without broad process matching**

Resolve the current owners with `lsof` for TCP port `18888` and
`/dev/cu.usbmodem1101`. Confirm both descriptors belong to the same known New
Dash PID before sending that exact PID `SIGINT`; wait until both resources are
released. Do not use `pkill`, `killall`, or a process-name match.

- [ ] **Step 3: Start and verify the LaunchAgent**

Run:

```sh
./start.sh --http-port 18888 --port /dev/cu.usbmodem1101
launchctl print "gui/$(id -u)/io.friendorfoe.new-dash"
```

Verify that `lsof` shows the service owning TCP `127.0.0.1:18888` and the exact
USB device. Query `/api/state` and require `connection.phase=live`,
`connection.detail=verified`, fresh status, normal recovery, and healthy
sensing.

- [ ] **Step 4: Prove crash restart once**

Resolve the exact Python PID from the USB descriptor and confirm its parent
chain belongs to the LaunchAgent's `caffeinate` generation. Send `SIGKILL` to
that exact Python PID. Poll `lsof` and `/api/state` until a different Python
PID owns the USB device and the API is live/fresh again. Require recovery
within 30 seconds.

- [ ] **Step 5: Prove persistence and deliberate stop semantics**

Query `/api/history?limit=1`, record the newest row ID, and run `./stop.sh`.
Verify the exact LaunchAgent target is absent and port `18888` is released.
Confirm the SQLite database and log files still exist; do not delete or edit
them.

- [ ] **Step 6: Start the final overnight generation**

Run `./start.sh --http-port 18888 --port /dev/cu.usbmodem1101` again. Verify a
new live/fresh API sample and a history row at or after the recorded row ID.
Leave this LaunchAgent loaded and running. Report the URL, service label, log
paths, stop command, and the open-lid/power requirement.

---

### Task 3: Publish the verified overnight-service change

**Files:**
- Verify: committed Task 1 implementation and planning records

**Interfaces:**
- Consumes: green final verification from Task 2.
- Produces: updated remote branch `origin/codex/new-dash` containing the overnight service support.

- [ ] **Step 1: Confirm the reviewed branch is clean and ahead of its upstream**

```sh
git status -sb
git log --oneline origin/codex/new-dash..HEAD
```

Expected: the worktree is clean and the unpublished list contains the design,
plan, and reviewed Task 1 implementation commits.

- [ ] **Step 2: Push the verified branch**

```sh
git push origin codex/new-dash
```

Expected: GitHub advances `origin/codex/new-dash` to the new implementation
commit without force-pushing.
