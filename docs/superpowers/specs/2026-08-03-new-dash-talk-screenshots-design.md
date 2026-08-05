# New Dash Talk Screenshots Design

## Goal

Capture a small, reusable set of real New Dash browser screenshots that can be
dropped directly into the Packet Village talk. The screenshots must show the
working Mac-to-badge USB path and current badge data rather than a fabricated UI
or deterministic browser fixture.

## Source and truthfulness

All captures come from the running New Dash instance connected to the factory
badge over USB. The Remote ID target is produced by the user's simulator. The
asset manifest must label it as simulator evidence so the screenshots are not
presented as an encounter with an unknown real aircraft.

No dashboard data is redacted. The user explicitly permits visible map
location, coordinates, MAC/BSSID values, hardware identifiers, operator
identifiers, firmware details, and other data currently rendered by New Dash.
The capture process must not add synthetic detections, rewrite displayed
values, or post-process the images in a way that changes their evidentiary
meaning.

## Screenshot set

Capture four desktop PNG images at a 1920 by 1080 browser viewport:

1. `01-live-simulator-remote-id.png` — the Live view with verified USB status,
   current threat counts, simulator Remote ID evidence, and recent USB
   detections when present.
2. `02-map-simulator-remote-id.png` — the Map view with the current simulator
   drone/operator markers, connecting line, coordinates, and live map context
   when available.
3. `03-history-usb-observations.png` — the History view showing retained
   observations from the USB-connected badge, including Remote ID rows when
   the simulator has produced persistable observations.
4. `04-badge-usb-health.png` — the Badge view showing verified firmware,
   scanner roles/health, USB diagnostics, and recovery state.

If simulator Remote ID is not present when the Live or Map capture is ready,
pause and ask the user to run the simulator again. Do not substitute fixture
data or imply an empty view contains a drone. History and Badge captures may
truthfully show whatever current retained/live state exists.

## Repository layout

Store the files under:

```text
docs/cfp/supporting-files/screenshots/new-dash/
```

The repository currently ignores PNG files globally. Add only this narrow
exception to `.gitignore`:

```gitignore
!docs/cfp/supporting-files/screenshots/new-dash/*.png
```

Do not relax the global PNG rule for other generated screenshots.

Update `docs/cfp/supporting-files/README.md` with a **New Dash screenshots**
subsection listing the four relative paths and concise slide-use captions. The
subsection must state that the images are live factory-badge USB captures, that
the Remote ID aircraft is simulator-generated, that the images are unredacted
by user choice, and when they were captured.

## Capture method

Use the in-app browser against the already running loopback New Dash server.
Set a 1920 by 1080 viewport, select each requested route, wait for the view to
finish rendering, and capture the visible viewport rather than a full-page
scroll. Do not restart the dashboard, stop the LaunchAgent, clear history, or
apply badge controls.

The browser UI may be navigated between Live, Map, History, and Badge. Capture
is otherwise read-only. The exact screenshot mechanism must write the PNG files
directly into the approved repository directory without an image-generation or
mockup step.

## Verification

For each PNG:

- confirm the file is a valid 1920 by 1080 PNG;
- inspect the rendered image at full detail;
- confirm the requested New Dash view is visible and not covered by a dialog,
  stale loading overlay, browser chrome, or another tab;
- confirm simulator labeling in the manifest is accurate and no caption calls
  the target a real-world drone;
- confirm the files are tracked through the narrow `.gitignore` exception;
- run `git diff --check` for the text changes.

No product test suite is required because capture does not change New Dash
runtime behavior. The running badge connection and visible dashboard state are
the validation source.

## Out of scope

This work does not edit the slide deck, generate a composite contact sheet,
capture mobile layouts, alter New Dash code, modify badge state, create fixture
data, redact or blur the approved visible data, or change the existing CFP
photos and historical backend screenshot descriptions.

## Success criteria

The repository contains four truthful, full-HD, individually usable New Dash
screenshots with stable filenames and a manifest that lets a speaker understand
what each image proves and that the Remote ID target came from a simulator.
