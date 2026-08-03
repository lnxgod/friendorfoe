# New Dash Talk Screenshots Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Capture and publish four truthful full-HD screenshots of the live USB-connected New Dash for direct use in the Packet Village talk.

**Architecture:** The running loopback dashboard remains the only image source. Browser control captures one visible 1920 by 1080 viewport for each New Dash route, while a narrow `.gitignore` exception permits only the approved talk PNG directory; the existing CFP supporting-files README becomes the asset manifest.

**Tech Stack:** New Dash loopback web UI, in-app browser control, PNG screenshots, GitHub-flavored Markdown, Git.

## Global Constraints

- Capture current factory-badge USB data only; do not use browser fixtures, generated images, mockups, or rewritten values.
- The Remote ID target is simulator-generated and must be labeled that way in repository documentation.
- Do not redact the live map, coordinates, MAC/BSSID values, hardware identifiers, operator identifiers, firmware details, or other rendered dashboard data.
- Capture exactly four visible desktop viewports at 1920 by 1080 pixels: Live, Map, History, and Badge.
- Do not restart New Dash, stop the LaunchAgent, clear history, apply controls, or otherwise mutate badge/dashboard state.
- If simulator Remote ID is absent from Live or Map, stop and ask the user to run the simulator again.
- Do not run product test suites; verify the image files, dimensions, visual content, manifest, tracking rule, and text diff only.

---

### Task 1: Capture the four live New Dash views

**Files:**
- Modify: `.gitignore`
- Create: `docs/cfp/supporting-files/screenshots/new-dash/01-live-simulator-remote-id.png`
- Create: `docs/cfp/supporting-files/screenshots/new-dash/02-map-simulator-remote-id.png`
- Create: `docs/cfp/supporting-files/screenshots/new-dash/03-history-usb-observations.png`
- Create: `docs/cfp/supporting-files/screenshots/new-dash/04-badge-usb-health.png`

**Interfaces:**
- Consumes: the already running `http://127.0.0.1:18888/` dashboard and its `#live`, `#map`, `#history`, and `#badge` routes.
- Produces: four immutable PNG files for the manifest and slide deck.

- [ ] **Step 1: Add the narrow PNG tracking exception**

Use `apply_patch` to add this line immediately after the existing `!docs/examples/screenshots/*.png` exception in `.gitignore`:

```gitignore
!docs/cfp/supporting-files/screenshots/new-dash/*.png
```

Do not modify the global `*.png` rule or add a wider exception.

- [ ] **Step 2: Prepare the approved asset directory**

Run:

```sh
mkdir -p docs/cfp/supporting-files/screenshots/new-dash
```

Expected: the directory exists and no other screenshot directory is created.

- [ ] **Step 3: Inspect live state before capture**

Use the in-app browser control skill to inspect `http://127.0.0.1:18888/#live` without clicking a mutation control. Confirm the page reports a verified USB connection and that simulator Remote ID evidence is visibly present. Then inspect `#map` and confirm a simulator drone marker or equivalent Remote ID position is visible.

If either required simulator view is absent, stop without capturing substitutes and ask the user to run the simulator again.

- [ ] **Step 4: Capture the Live and Map viewports**

Set the browser viewport to exactly 1920 by 1080. Navigate to `#live`, wait for current content to render, and save the visible viewport to:

```text
docs/cfp/supporting-files/screenshots/new-dash/01-live-simulator-remote-id.png
```

Navigate to `#map`, wait for markers, connecting line, coordinates, and map context to render, and save the visible viewport to:

```text
docs/cfp/supporting-files/screenshots/new-dash/02-map-simulator-remote-id.png
```

Do not capture browser chrome, a full-page scroll, loading overlays, dialogs, or another tab.

- [ ] **Step 5: Capture the History and Badge viewports**

Navigate to `#history`, wait for the retained USB observations table to render, and save the visible viewport to:

```text
docs/cfp/supporting-files/screenshots/new-dash/03-history-usb-observations.png
```

Navigate to `#badge`, wait for firmware, scanner roles/health, USB diagnostics, and recovery state to render, and save the visible viewport to:

```text
docs/cfp/supporting-files/screenshots/new-dash/04-badge-usb-health.png
```

Do not clear history, submit controls, reset settings, or alter filters solely to manufacture a detection.

- [ ] **Step 6: Verify the captured image files**

Run `file` and `sips` against all four exact paths. Expected: every file is a PNG and every file reports `pixelWidth: 1920` and `pixelHeight: 1080`.

Use the local image viewer on each PNG at original detail. Confirm the filename matches the visible route, the dashboard occupies the frame, and no required view is blank, loading, obstructed, or mislabeled.

### Task 2: Document and publish the screenshot set

**Files:**
- Modify: `docs/cfp/supporting-files/README.md`
- Create: `docs/superpowers/plans/2026-08-03-new-dash-talk-screenshots.md`

**Interfaces:**
- Consumes: the four verified PNG paths from Task 1 and their actual capture timestamp.
- Produces: a repository manifest that gives speakers truthful captions and simulator provenance.

- [ ] **Step 1: Add the New Dash screenshot manifest**

Use `apply_patch` to append a `### New Dash screenshots` subsection under the existing `## Screenshots` section. Include the actual local capture date and time and this exact provenance statement:

```markdown
These are unredacted live captures from a factory badge connected to New Dash over USB on macOS. The Remote ID aircraft shown in the Live and Map images was generated by the project simulator; it is not presented as an unknown real-world drone.
```

List the four files with these slide-use descriptions:

```markdown
- `screenshots/new-dash/01-live-simulator-remote-id.png` — verified USB connection, live counts, and simulator Remote ID evidence.
- `screenshots/new-dash/02-map-simulator-remote-id.png` — simulator drone/operator position and live map context.
- `screenshots/new-dash/03-history-usb-observations.png` — locally retained observations received from the badge over USB.
- `screenshots/new-dash/04-badge-usb-health.png` — firmware, scanner roles/health, USB diagnostics, and recovery state.
```

- [ ] **Step 2: Verify tracking and text changes**

Run:

```sh
git status --short --untracked-files=all
git diff --check
git diff -- .gitignore docs/cfp/supporting-files/README.md docs/superpowers/plans/2026-08-03-new-dash-talk-screenshots.md
```

Expected: only `.gitignore`, the supporting-files README, this plan, and the four approved PNGs are new or modified after the already committed design spec. The four PNG paths must appear in `git status` despite the global PNG ignore rule.

- [ ] **Step 3: Commit the assets and documentation together**

Run:

```sh
git add .gitignore docs/cfp/supporting-files/README.md docs/cfp/supporting-files/screenshots/new-dash docs/superpowers/plans/2026-08-03-new-dash-talk-screenshots.md
git commit -m "docs: add New Dash talk screenshots"
```

Expected: one commit contains the four PNGs, narrow ignore exception, manifest, and implementation plan.

- [ ] **Step 4: Push the completed New Dash branch**

Run:

```sh
git push origin codex/new-dash
```

Expected: `origin/codex/new-dash` advances to the screenshot commit while the active service worktree remains in place.
