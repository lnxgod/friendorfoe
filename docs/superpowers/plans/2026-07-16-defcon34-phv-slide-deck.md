# DEF CON 34 PHV Slide Deck Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and visually verify a PowerPoint and submission-ready PDF containing 31 spoken slides plus 10 bibliography/backup slides for the accepted DEF CON 34 Packet Hacking Village talk, with embedded speaker notes, authentic project assets, and the accepted CFP chronology intact.

**Architecture:** The approved Markdown deck design remains the content source of truth. A plain JavaScript ES module parses each slide block, adds custom 16:9 layouts through `@oai/artifact-tool`, embeds real project images, stores image-generation prompts and rehearsal guidance in speaker notes, and exports PPTX plus per-slide QA renders. LibreOffice converts the verified PPTX to PDF after layout QA.

**Tech Stack:** Node.js ES modules, `@oai/artifact-tool`, ImageMagick, `qrencode`, bundled LibreOffice/soffice, Poppler, and the presentation render/layout QA scripts.

## Global Constraints

- Preserve all ten accepted CFP sections and their order.
- Produce exactly 31 spoken slides followed by 10 reference/bibliography slides, for 41 final slides.
- Keep the default speaker split near 75 percent Will and 25 percent Charles while preserving alternate assignments in speaker notes.
- Use real packet captures, screenshots, hardware photographs, and code excerpts as evidence.
- Treat generated imagery as conceptual; never use it as packet, hardware, or test evidence.
- Use 16:9 at 1280 x 720 pixels.
- Use at least 50pt for the deck title, 35pt for slide titles, 24pt for callouts, and 16pt for supporting text.
- Keep the visible slide copy audience-facing; production instructions belong only in notes.
- Keep C5 wording careful and AI coverage to roughly three minutes.
- Recorded demos are primary; live LVCC simulator output is an additive proof path.
- Do not expose private test-site coordinates, credentials, tokens, email addresses, or device identifiers.
- Do not alter or commit files under `docs/cfp/`.
- Final PDF filename: `DEF CON 34 - Packet Hacking Village - Will Hatzer & Charles Grow - Why Couldn't I See My Own Drone.pdf`.

---

## File Map

- Read: `docs/superpowers/specs/2026-07-16-defcon34-phv-slide-deck-design.md` - accepted slide order, visible copy, assets, and notes.
- Read: `docs/cfp/supporting-files/photos/*` - submitted hardware/prototype photographs.
- Read: `docs/cfp/supporting-files/screenshots/*` - redacted dashboard and packet-evidence screenshots.
- Create: `/private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/build-deck.mjs` - deck parser, custom theme, layouts, notes, rendering, and PPTX export.
- Create: `/private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/asset-manifest.txt` - asset provenance and redaction ledger.
- Create: `/private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/assets/repository-qr.png` - repository QR code.
- Create: `/private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/preview/` - rendered slide PNGs.
- Create: `/private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/layout/` - slide layout JSON.
- Create: `/private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/qa/` - montage, inspection snapshot, and QA results.
- Create: `outputs/DEF CON 34 - Packet Hacking Village - Will Hatzer & Charles Grow - Why Couldn't I See My Own Drone.pptx` - editable source deck.
- Create: `outputs/DEF CON 34 - Packet Hacking Village - Will Hatzer & Charles Grow - Why Couldn't I See My Own Drone.pdf` - organizer submission file.

## Task 1: Initialize Workspace And Validate Sources

**Files:**
- Read: `docs/superpowers/specs/2026-07-16-defcon34-phv-slide-deck-design.md`
- Create: `/private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/asset-manifest.txt`
- Create: `/private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/assets/repository-qr.png`

**Interfaces:**
- Consumes: approved Markdown outline and submitted CFP images.
- Produces: initialized artifact-tool workspace, verified source inventory, and repository QR image.

- [ ] **Step 1: Create the scratch and output directories**

Run:

```bash
mkdir -p /private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/assets
mkdir -p /private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/preview
mkdir -p /private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/layout
mkdir -p /private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/qa
mkdir -p outputs
```

Expected: all directories exist with no errors.

- [ ] **Step 2: Initialize artifact-tool resolution**

Run:

```bash
node /Users/billh/.codex/plugins/cache/openai-primary-runtime/presentations/26.715.12143/skills/presentations/container_tools/setup_artifact_tool_workspace.mjs --workspace /private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp
```

Expected: the scratch workspace can resolve `@oai/artifact-tool`.

- [ ] **Step 3: Verify the outline contract before authoring**

Run a Node assertion that checks:

```js
const assert = require("node:assert");
const fs = require("node:fs");
const source = fs.readFileSync(
  "docs/superpowers/specs/2026-07-16-defcon34-phv-slide-deck-design.md",
  "utf8",
);
const slides = [...source.matchAll(/^### Slide (\d+) - /gm)].map((m) => Number(m[1]));
assert.equal(slides.length, 31);
assert(slides.every((number, index) => number === index + 1));
assert.equal((source.match(/^\| 0:/gm) || []).length, 10);
```

Expected: process exits `0` with 31 sequential slides and 10 timing rows.

- [ ] **Step 4: Generate the repository QR image**

Run:

```bash
qrencode -o /private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/assets/repository-qr.png -s 8 -m 2 https://github.com/lnxgod/friendorfoe
```

Expected: a readable PNG QR code that resolves to the public repository.

- [ ] **Step 5: Write the asset manifest**

Record every used source asset, its slide number, crop intent, and whether it is evidence, documentary context, or conceptual support. Include this exact privacy statement:

```text
All visible packet-derived screenshots are the redacted CFP copies. No home coordinates, credentials, tokens, or private device identifiers may appear. LVCC coordinates are allowed only in material explicitly labeled SIMULATED.
```

Expected: each embedded image in the deck can be traced to an existing file or an explicit prompt in the approved design.

- [ ] **Step 6: Commit the plan only**

```bash
git add docs/superpowers/plans/2026-07-16-defcon34-phv-slide-deck.md
git commit -m "docs: plan DEF CON 34 PHV deck production"
```

Expected: only this plan document is committed; no CFP file changes.

## Task 2: Author The 31 Spoken Slides, 10 Backup Slides, And Speaker Notes

**Files:**
- Create: `/private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/build-deck.mjs`
- Create: `outputs/DEF CON 34 - Packet Hacking Village - Will Hatzer & Charles Grow - Why Couldn't I See My Own Drone.pptx`

**Interfaces:**
- Consumes: parsed slide blocks with `number`, `title`, `time`, `lead`, `alternate`, `visibleCopy`, `visual`, `speakerNotes`, `handoff`, and `endState`.
- Produces: `buildDeck(slides: SlideSpec[]): Promise<Presentation>` and an editable PPTX with 31 spoken slides, 10 backup/reference slides, and speaker notes on all 41 slides.

- [ ] **Step 1: Implement the Markdown slide parser**

The parser must return this exact shape:

```js
/**
 * @typedef {Object} SlideSpec
 * @property {number} number
 * @property {string} title
 * @property {string} time
 * @property {string} lead
 * @property {string} alternate
 * @property {string[]} visibleCopy
 * @property {string} visual
 * @property {string[]} speakerNotes
 * @property {string} handoff
 * @property {string} endState
 */
```

Parse only the `## Slide Outline` section and stop before `## Backup And Reference Slides`. Assert exactly 31 sequential results and reject any slide missing time, lead, visual, or notes.

- [ ] **Step 2: Implement the custom deck theme and primitives**

Create constants and helpers with these interfaces:

```js
const COLORS = {
  ink: "#080A0D",
  panel: "#11161B",
  white: "#F7F9FA",
  muted: "#A8B1B8",
  cyan: "#28D7E5",
  amber: "#FFB547",
  magenta: "#F15BB5",
  copper: "#C78A44",
};

function addSlideNumber(slide, number) {}
function addSectionMarker(slide, label, color) {}
function addTitle(slide, title, options = {}) {}
function addBodyLines(slide, lines, options = {}) {}
function addImage(slide, path, frame, options = {}) {}
function addEvidenceFooter(slide, sourceLabel) {}
function addSpeakerNotes(slide, spec) {}
```

Use flat compositions, square or slightly rounded image frames, and no card grids. Keep all content inside a 64px safe margin.

- [ ] **Step 3: Implement the layout families**

Use these exact layout functions:

```js
function layoutHero(slide, spec, visual) {}
function layoutClaim(slide, spec, accent) {}
function layoutPhotoStory(slide, spec, visual, options = {}) {}
function layoutEvidence(slide, spec, visual, options = {}) {}
function layoutComparison(slide, spec, left, right) {}
function layoutProcess(slide, spec, steps) {}
function layoutClosing(slide, spec, visual) {}
```

Map the approved slides to layouts as follows:

```js
const LAYOUT_BY_SLIDE = {
  1: "hero", 2: "photoStory", 3: "claim", 4: "process", 5: "process",
  6: "evidence", 7: "photoStory", 8: "process", 9: "comparison", 10: "evidence",
  11: "comparison", 12: "evidence", 13: "process", 14: "comparison", 15: "claim",
  16: "photoStory", 17: "process", 18: "process", 19: "claim", 20: "photoStory",
  21: "evidence", 22: "process", 23: "photoStory", 24: "claim", 25: "photoStory",
  26: "process", 27: "process", 28: "evidence", 29: "photoStory", 30: "evidence",
  31: "closing",
};
```

- [ ] **Step 4: Embed real visual assets and prompt metadata**

Use the exact existing CFP paths named in the approved design. Concrete images should carry meaningful alt text and the related IG prompt as regeneration metadata only when a prompt exists. Do not embed unresolved prompt-only images in the organizer PDF; use typography, a native explanatory diagram, or a real asset instead.

- [ ] **Step 5: Add complete speaker notes**

For every slide, call:

```js
slide.speakerNotes.textFrame.setText([
  `TIME: ${spec.time}`,
  `DEFAULT LEAD: ${spec.lead}`,
  `ALTERNATE DELIVERY: ${spec.alternate}`,
  "",
  ...spec.speakerNotes,
  spec.handoff ? `HANDOFF: ${spec.handoff}` : `END STATE: ${spec.endState}`,
  "",
  `VISUAL SOURCE/PLAN: ${spec.visual}`,
]);
slide.speakerNotes.setVisible(true);
```

Expected after the backup slides are appended: all 41 slides expose speaker notes through `presentation.inspect({ kind: "notes" })`.

- [ ] **Step 6: Export PPTX, previews, layouts, montage, and inspection data**

Before export, append the ten backup/reference slides in the exact order specified by the approved design: Remote ID field guide, DJI evidence guide, Bayesian fusion details, C5-to-S3 repo timeline, badge architecture, UART/recovery, simulator internals, passive-only boundary, build/resources, and bibliography. Set each backup slide's notes to `BACKUP ONLY - not part of the 50-minute spoken sequence.`

The builder must export:

```js
await writeBlob(`${PREVIEW_DIR}/slide-${stem}.png`, await presentation.export({ slide, format: "png", scale: 2 }));
await fs.writeFile(`${LAYOUT_DIR}/slide-${stem}.layout.json`, await (await slide.export({ format: "layout" })).text());
await writeBlob(`${QA_DIR}/deck-montage.webp`, await presentation.export({ format: "webp", montage: true, scale: 1 }));
await fs.writeFile(`${QA_DIR}/inspect.ndjson`, (await presentation.inspect({ kind: "slide,textbox,shape,image,notes,layout", maxChars: 200000 })).ndjson);
const pptx = await PresentationFile.exportPptx(presentation);
await pptx.save(FINAL_PPTX);
```

Expected: 41 PNG previews, 41 layout JSON files, one montage, one inspection file, and one PPTX.

## Task 3: Structural And Visual QA

**Files:**
- Read: `/private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/preview/*.png`
- Read: `/private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/layout/*.json`
- Modify: `/private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/tmp/build-deck.mjs`

**Interfaces:**
- Consumes: rendered deck and layout metadata.
- Produces: a 41-slide deck with no overflow, unintended overlap, broken crop, unreadable text, or missing notes.

- [ ] **Step 1: Run the presentation overflow test**

Run:

```bash
python3 /Users/billh/.codex/plugins/cache/openai-primary-runtime/presentations/26.715.12143/skills/presentations/container_tools/slides_test.py "outputs/DEF CON 34 - Packet Hacking Village - Will Hatzer & Charles Grow - Why Couldn't I See My Own Drone.pptx"
```

Expected: no off-canvas or overflow errors.

- [ ] **Step 2: Verify slide and notes counts**

Use artifact-tool inspection and assert:

```js
assert.equal(slideCount, 41);
assert.equal(notesCount, 41);
assert.equal(titleCount, 41);
```

Expected: every main and backup slide has one title and one speaker-notes object.

- [ ] **Step 3: Inspect the montage for deck-level flow**

Check that section transitions are visible without agenda slides, slide silhouettes vary, photographs are not repeatedly reused as decoration, and the AI section does not visually dominate the packet story.

Expected: the deck reads as a chronological investigation ending in the badge and demo.

- [ ] **Step 4: Inspect all 41 slides individually at full size**

For each preview, check:

```text
No clipped title
No title wraps unexpectedly
No body text below 16pt
No unreadable evidence crop
No unintended overlap
No placeholder visible to the audience
No private coordinates or identifiers
No fake packet or test evidence
```

Expected: every slide passes all checks. Fix the builder and rerun the full export after any change.

- [ ] **Step 5: Verify the accepted promise coverage in visible copy and notes**

Search the inspection and notes for:

```text
BLE OpenDroneID
Wi-Fi Beacon or DJI vendor IE
Wi-Fi promiscuous capture
Bayesian confidence or fusion
C5 and S3
921600
FOF-SIM-001 and FOF-SIM-002
LVCC and SIMULATED
Codex
recorded demo
```

Expected: every promise appears and is located in the accepted section.

## Task 4: Produce And Verify The Submission PDF

**Files:**
- Read: `outputs/DEF CON 34 - Packet Hacking Village - Will Hatzer & Charles Grow - Why Couldn't I See My Own Drone.pptx`
- Create: `outputs/DEF CON 34 - Packet Hacking Village - Will Hatzer & Charles Grow - Why Couldn't I See My Own Drone.pdf`

**Interfaces:**
- Consumes: visually approved PPTX.
- Produces: organizer-ready PDF with the correct filename and 41 pages.

- [ ] **Step 1: Convert through bundled LibreOffice**

Run:

```bash
/Users/billh/.cache/codex-runtimes/codex-primary-runtime/dependencies/bin/override/soffice --headless --convert-to pdf --outdir outputs "outputs/DEF CON 34 - Packet Hacking Village - Will Hatzer & Charles Grow - Why Couldn't I See My Own Drone.pptx"
```

Expected: PDF created with the organizer's exact filename.

- [ ] **Step 2: Verify PDF page count and metadata**

Run:

```bash
pdfinfo "outputs/DEF CON 34 - Packet Hacking Village - Will Hatzer & Charles Grow - Why Couldn't I See My Own Drone.pdf"
```

Expected: `Pages: 41`, 16:9 page dimensions, and no encryption.

- [ ] **Step 3: Render every PDF page and inspect the conversion**

Run:

```bash
python3 /Users/billh/.codex/plugins/cache/openai-primary-runtime/presentations/26.715.12143/skills/presentations/container_tools/render_slides.py "outputs/DEF CON 34 - Packet Hacking Village - Will Hatzer & Charles Grow - Why Couldn't I See My Own Drone.pptx"
```

Also render the PDF with Poppler and compare representative title, evidence, hardware, AI, demo, and closing pages.

Expected: PDF and PPTX renders agree, with no substituted fonts, missing images, or changed wrapping.

- [ ] **Step 4: Perform final privacy and filename checks**

Run `pdftotext` and search for private email addresses, phone numbers, home coordinates, credentials, or production notes. Confirm `SIMULATED - LVCC` is the only location-specific demo wording.

Expected: no private or internal production text in the audience-facing PDF.

## Task 5: Deliver The Review Package

**Files:**
- Deliver: `outputs/DEF CON 34 - Packet Hacking Village - Will Hatzer & Charles Grow - Why Couldn't I See My Own Drone.pptx`
- Deliver: `outputs/DEF CON 34 - Packet Hacking Village - Will Hatzer & Charles Grow - Why Couldn't I See My Own Drone.pdf`
- Retain: `/private/tmp/codex-presentations/019f0cf4-f37e-7521-842c-c8b1b773e1b4/defcon34-phv-deck/` for follow-up edits.

**Interfaces:**
- Consumes: verified PPTX/PDF and QA evidence.
- Produces: files Will and Charles can rehearse, review, and send to PHV.

- [ ] **Step 1: Report concrete verification evidence**

Include slide count, notes count, overflow-test result, PDF page count, and the exact assets that remain intentionally replaceable after Charles reviews.

- [ ] **Step 2: Keep the repository state explicit**

Do not push while the checkout is behind `origin/main`. Report the plan commit and whether binary outputs are tracked or left as local deliverables.

- [ ] **Step 3: Hand off the two final files**

Provide one clickable local link to the PPTX and one to the PDF. The PDF is the PHV submission artifact; the PPTX is the editable rehearsal source.
