# New Dash README Design

## Goal

Make the source-built New Dash path obvious to an open-source user who has a
factory Friend or Foe badge and a Mac: clone the repository, connect the badge
uplink over USB-C, run one command, and receive live badge data in a browser.

The documentation must state clearly that a compatible factory badge does not
need to be reflashed and that New Dash is independent of the legacy backend.

## Documentation ownership

`new-dash/README.md` owns the complete setup and operating guide. The root
`README.md` contains only a concise New Dash introduction, the no-reflash
promise, the shortest launch command, and a link to the complete guide. This
avoids duplicated instructions drifting apart while keeping New Dash easy to
discover from the repository front page.

## New Dash README structure

The guide will lead with a compact quick-start sequence before explaining
features or internals:

1. Confirm the small prerequisites list.
2. Clone or download the repository.
3. Connect the badge uplink USB-C port with a data-capable cable.
4. Run `cd new-dash` followed by `./run.sh`.
5. Explain that the browser opens automatically and that badge discovery keeps
   retrying if the badge is temporarily disconnected.

The quick start will explicitly separate three dependency categories:

- Required: macOS, Python 3.11 or newer, a compatible factory badge, a
  data-capable USB-C cable, and first-run network access when Python packages
  are not already cached.
- Automatic: `run.sh` and `start.sh` create `.venv` and install the project,
  including the sole runtime dependency, `pyserial>=3.5,<4`.
- Not required: badge reflashing, Android, the legacy FastAPI backend, Docker,
  PostgreSQL, Redis, Node.js, or a separate USB driver for the factory badge's
  native USB serial connection.

After the quick start, the guide will retain the existing detailed sections for
explicit port selection, the always-on `start.sh`/`stop.sh` macOS LaunchAgent,
browser views, local history, troubleshooting, and contributor tests. The
always-on section will remain secondary to `run.sh` so a first-time user reaches
the dashboard before encountering service-management details.

## Root README reference

The root README will add a short, prominent New Dash section near the badge
console material. It will describe New Dash as the compact single-badge USB
browser console, state that current factory badges use the same native USB
protocol as Android without reflashing, show the two-line source launch, and
link to `new-dash/README.md` for requirements and deployment.

The root README will continue to describe `backend/` as the separate multi-node
platform. It will not copy the full New Dash requirements, troubleshooting, or
service instructions.

## Failure guidance

The quick-start path will name the likely setup failures without expanding into
a long installation manual:

- `python3` missing or older than 3.11: install a current Python release, then
  rerun the script.
- Badge not discovered: confirm the uplink port and data-capable cable.
- Multiple serial devices: launch with an explicit `/dev/cu.usbmodem...` port.
- Port already owned: close a flasher, serial monitor, Android bridge, or other
  New Dash instance.
- First dependency installation unavailable: restore network access and rerun;
  subsequent launches reuse `.venv`.

## Verification

Because this change is documentation-only, completion requires:

- checking every documented command against the checked-in scripts and
  `pyproject.toml`;
- checking local Markdown links and paths;
- scanning for contradictory claims about operating systems, dependencies,
  reflashing, backend requirements, and foreground versus service launch;
- running `git diff --check` before commit and push.

No product test suite is required for the README-only change. The already
completed physical badge test remains the runtime evidence for native USB,
Remote ID, and drone visibility.

## Out of scope

This documentation update does not add Windows or Linux support, package a
binary application, change firmware, change the USB protocol, modify New Dash
runtime behavior, or merge New Dash with the legacy backend.

## Success criteria

An unfamiliar Mac user can identify the prerequisites, understand what is
installed automatically, learn that no reflash or legacy backend is required,
and reach the browser dashboard from the repository documentation without
having to inspect scripts or Android code.
