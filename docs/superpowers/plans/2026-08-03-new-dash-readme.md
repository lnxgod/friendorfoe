# New Dash README Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make New Dash discoverable from the repository front page and give Mac users a complete plug-in-and-run source deployment guide in `new-dash/README.md`.

**Architecture:** `new-dash/README.md` remains the single owner of requirements, quick start, persistent service setup, data locations, and troubleshooting. The root `README.md` adds only a concise no-reflash introduction, the shortest launch command, and a relative link to the complete guide so setup instructions cannot drift between two files.

**Tech Stack:** GitHub-flavored Markdown, POSIX shell commands already implemented by `run.sh`, `start.sh`, and `stop.sh`, Python 3.11+, and `pyserial>=3.5,<4`.

## Global Constraints

- Support claims remain limited to macOS; Windows and Linux stay explicitly unsupported and untested for this release.
- A compatible factory badge uses its existing native USB protocol and does not require reflashing.
- New Dash remains separate from `backend/` and requires no Docker, PostgreSQL, Redis, Android, or Node.js at runtime.
- `run.sh` is the primary first-use path; `start.sh`/`stop.sh` remain the secondary always-on deployment path.
- The scripts create `.venv` and install the project plus `pyserial>=3.5,<4` automatically.
- Do not change application code, scripts, firmware, dependencies, or product behavior.
- Verification is documentation-only: check claims, commands, links, and `git diff --check`; do not run the product test suites.

---

### Task 1: Publish the plug-in-and-run README path

**Files:**
- Modify: `new-dash/README.md`
- Modify: `README.md`

**Interfaces:**
- Consumes: `new-dash/run.sh`, `new-dash/start.sh`, `new-dash/stop.sh`, and `new-dash/pyproject.toml` as the authoritative command and dependency contracts.
- Produces: one detailed New Dash setup guide and one stable repository-front-page link to it.

- [ ] **Step 1: Restructure the beginning of the New Dash guide**

Keep the existing product introduction and separation from `../backend/`, then place these sections before the current detailed launch options:

````markdown
## Requirements

Required:

- A Mac running macOS.
- Python 3.11 or newer, available as `python3`.
- A compatible Friend or Foe factory badge and a data-capable USB-C cable connected to the badge uplink board.
- Network access on the first run if the Python packages are not already cached.

New Dash handles these automatically:

- Creates or reuses `new-dash/.venv`.
- Installs New Dash and its only runtime dependency, `pyserial>=3.5,<4`.
- Opens a loopback-only browser dashboard and keeps retrying if the badge is temporarily disconnected.

You do **not** need to reflash a compatible factory badge or install Android, the legacy FastAPI backend, Docker, PostgreSQL, Redis, Node.js, or a separate USB driver for the badge's native USB serial connection.

## Plug in and run

From a clone or downloaded copy of this repository:

```sh
cd new-dash
./run.sh
```

Connect the badge uplink USB-C port before or after launch. New Dash opens the selected local URL automatically, verifies the badge using the same factory-firmware USB signaling as Android, and begins showing live detections. No firmware or badge configuration change is required.
````

Retain the existing explicit `--port` example, manual virtual-environment equivalent, launch option list, overnight service, browser views, local-data explanation, troubleshooting, and contributor instructions. Remove repeated prerequisite or no-reflash prose where the new sections make it redundant, without removing technical caveats.

- [ ] **Step 2: Add the concise root README reference**

Insert a section after `## Android As Badge Console` and before `## From Badge To Sensor Platform` with this copy:

````markdown
## New Dash: One Badge, One USB Cable

New Dash brings the factory badge's native Android-compatible USB feed to a compact browser dashboard on macOS. It runs directly from source, discovers one badge uplink, shows live detections and Remote ID, and keeps local history without using the legacy multi-node backend or reflashing a compatible factory badge.

```sh
cd new-dash
./run.sh
```

See the [New Dash source and deployment guide](new-dash/README.md) for requirements, explicit USB-port selection, and the auto-restarting overnight service.
````

Do not duplicate the full requirements, service commands, data paths, or troubleshooting in the root README. Leave the existing `Repo Map` and `Build And Test` references intact unless a sentence must be shortened to avoid repetition.

- [ ] **Step 3: Verify every documented contract once**

Read the changed README passages beside the authoritative files and confirm:

```text
pyproject.toml: requires-python = ">=3.11" and dependencies = ["pyserial>=3.5,<4"]
run.sh: creates .venv, installs with pip, starts new_dash, and permits badge auto-discovery
start.sh: macOS-only LaunchAgent, default HTTP port 18888, optional /dev/cu.* badge port
stop.sh: stops only the New Dash LaunchAgent and preserves local history
```

Run:

```sh
git diff --check
test -f new-dash/README.md
test -x new-dash/run.sh
test -x new-dash/start.sh
test -x new-dash/stop.sh
```

Expected: every command exits `0`, the relative link target exists, and the diff contains no claims of Windows/Linux support, automatic badge flashing, or a required legacy backend.

- [ ] **Step 4: Review and commit the documentation together**

Inspect only the intended documentation changes:

```sh
git diff -- README.md new-dash/README.md docs/superpowers/plans/2026-08-03-new-dash-readme.md
git status --short
```

Expected: the two README files and this implementation plan are the only new changes after the already committed design spec.

Commit:

```sh
git add README.md new-dash/README.md docs/superpowers/plans/2026-08-03-new-dash-readme.md
git commit -m "docs: publish New Dash plug-in setup"
```

- [ ] **Step 5: Push the completed branch**

Run:

```sh
git push origin codex/new-dash
```

Expected: `origin/codex/new-dash` advances to the documentation commit and contains both the reviewed design spec and final README changes.
