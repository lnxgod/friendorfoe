# DEF CON 34 PHV Narrative-Lanes Redesign

## Decision

Rebuild the accepted Friend or Foe deck around five narrative lanes instead of strict chronology:

1. Will discovers the packet problem.
2. Packet requirements define the platform.
3. Charles turns the proven platform into badge hardware.
4. Will reconnects the hardware to the integration and physical-layer failures.
5. AI-assisted engineering, recorded proof, and the attendee handoff close the story.

This preserves the accepted CFP story while making speaker ownership and cause-and-effect easier to follow. Parallel work remains explicit in the notes so the cleaner presentation order does not rewrite the real timeline.

## Delivery Contract

- Target 40 minutes of planned material inside the accepted 50-minute slot.
- Leave 10 minutes for playback friction, live badge behavior, transitions, and questions.
- Keep the badge as the result of the investigation, not the opening product pitch.
- Keep packet interpretation practical. Do not turn the talk into a byte-by-byte teardown.
- Keep weak evidence weak: SSIDs, OUIs, and RSSI are clues rather than verdicts.
- Keep Remote ID localization technically accurate: structured Remote ID already reports coordinates; multi-node localization was pursued for cheaper or non-compliant drones and other BLE/Wi-Fi devices.
- Keep the C5 claim scoped to this project: dual-band switching delays and the resulting capture cadence did not meet the project timeline, so the production platform standardized on S3.
- Keep the main talk passive and non-invasive.
- Use recorded proof as the dependable demo. Live badges are additional evidence, not a dependency.
- Do not show private test-site coordinates, SSIDs, MAC addresses, or other household identifiers.

## Speaker Ownership

Charles owns exactly five visible slides:

- About Charles
- Box architecture became badge architecture
- Research, design, sourcing, and antenna choices
- Prototype ladder and center-core validation
- Manufacturing 45 badges

These slides retain the existing amber `CHARLES / ...` labels and `CHARLES SLIDE n OF 5` note markers. Charles's bio copy is content-locked; the redesign may move or restyle that slide but must not rewrite it.

Will owns the packet-discovery, platform-decision, UART/CHOMP, AI, proof, take-home, and closing slides. The expected Charles speaking time is about 6 minutes 25 seconds.

## Main-Talk Order

The source column uses the current deck's visible slide numbers before this rebuild.

| New | Source | Lead | Target | Purpose |
| --- | --- | --- | --- | --- |
| 1 | 1 | Will | 0:00-0:20 | Title and question |
| 2 | 2 | Will | 0:20-0:45 | About Will |
| 3 | 3 | Charles | 0:45-1:10 | About Charles |
| 4 | 4 | Will | 1:10-2:30 | DJI Mini 2 does not appear in Android Remote ID |
| 5 | 5 | Will | 2:30-3:45 | Parser, transport, receiver, or absent packet? |
| 6 | 7 | Will | 3:45-5:30 | Remote ID paths versus DJI Wi-Fi clues |
| 7 | 8 | Will | 5:30-7:00 | SSIDs and OUIs are leads, not proof |
| 8 | 9 | Will | 7:00-8:20 | Build a controlled Remote ID source |
| 9 | 10 | Will | 8:20-9:40 | Two deterministic BLE Remote ID simulations |
| 10 | 12 | Will | 9:40-12:00 | Phone experiment becomes persistent sensor platform |
| 11 | 13 | Will | 12:00-13:30 | Fox-hunting instincts meet bursty BLE |
| 12 | 14 | Will | 13:30-15:00 | RSSI is not distance |
| 13 | 22 | Will | 15:00-16:15 | Privacy-device signatures expand the scope |
| 14 | 24 | Will | 16:15-18:00 | Evidence strength controls confidence |
| 15 | 16 | Will | 18:00-19:45 | Packet cadence selects C5 versus S3 |
| 16 | 18 | Charles | 19:45-21:15 | The box architecture becomes the badge architecture |
| 17 | 25 | Charles | 21:15-22:45 | Research, design, sourcing, and external antennas |
| 18 | 26 | Charles | 22:45-24:15 | Prototype ladder and center-core proof |
| 19 | 27 | Charles | 24:15-25:45 | Nine hours to assemble 45 badges to flash-ready |
| 20 | 19 | Will | 25:45-27:15 | Internal UART packet path |
| 21 | 21 | Will | 27:15-29:45 | CHOMP, rate sweep, and the solder stinger |
| 22 | 28 | Will | 29:45-31:15 | AI reduced the cost of curiosity |
| 23 | 29 | Will | 31:15-33:00 | AI put real hardware in the loop |
| 24 | 30 | Will | 33:00-37:00 | Recorded badge and Android proof; live badges second |
| 25 | 32 | Will | 37:00-38:45 | Take it home, reflash it, and explore |
| 26 | 33 | Will | 38:45-40:00 | Closing lessons |

All skipped slides move behind the closing slide. The former live slide 15, `A Packet Is an Observation, Not a Verdict`, becomes backup because slides 7 and 14 now carry that lesson more concretely.

## Slide Redesigns

### Source 8: SSIDs Are Leads, Not Proof

- Remove the backend screenshot from the main slide.
- Use a flat evidence ladder built from native slide shapes: structured Remote ID, vendor information element, SSID/OUI clue.
- Add one tightly cropped, redacted real packet or catalog excerpt only if it is already available and legible.
- Keep the spoken distinction: probe requests reveal what a client seeks, not necessarily what is present.
- Move the full redacted Wi-Fi/probe backend view to bonus material.

### Source 12: Persistent Sensor Platform

- Lead with the real open sensor-box photograph and a simple five-rig topology.
- Explain that five sets of three nodes were deployed at approximate front-yard locations.
- Tell the WLED outage story verbally: setup-mode Wi-Fi traffic mapped coherently while BLE localization stayed messy.
- Do not recreate or imply a missing triangulation screenshot.
- State that this experiment concerned cheaper/non-compliant drones and other BLE/Wi-Fi devices, not the GPS coordinates already contained in Remote ID.

### Sources 22 and 24: Scope and Confidence

- Move both slides before the hardware choice so Will's packet-discovery lane reaches its conclusion before Charles begins.
- Use the real badge/privacy-device photograph for scope.
- Replace backend-heavy confidence visuals with a native evidence-strength model and a real badge display crop.
- Preserve source labels, decay, negative cases, and the ability to say `unknown`.
- Move detailed backend confidence screenshots to bonus material.

### Source 16: Packet Cadence Selected the Hardware

- Keep this Will-led as the bridge between discovery and Charles's hardware lane.
- Reframe the title around the engineering criterion, not the chip contest.
- Show: dual-band promise, switching delay measured in seconds on the project path, then S3 standardization for dependable 2.4 GHz Wi-Fi and BLE capture.
- Do not claim a universal C5 defect or invent an exact delay that was not recorded.
- End with: the selected packet architecture is what Charles had to make wearable.

### Sources 18, 25, 26, and 27: Charles's Hardware Lane

- Source 18 pairs the real three-board sensor box with the final badge back. The core line is: `The badge is the boards from the box, with PCB traces instead of wires.`
- Source 25 covers component choice, 45-board sourcing, local fabrication, external 2.4 GHz Wi-Fi/BLE antennas, cable pass-through, and one copper triangle beneath each antenna.
- Source 26 shows the progression from breadboard/box to plastic fit check, center-core board, and first full PCB. The prototype board, not the plastic mockup, carries the USB-C clearance lesson.
- Source 27 covers nine hours of in-house assembly to flash-ready, scissors used to trim pre-soldered display boards, all battery leads repinned for polarity, and the panelized triangle-cost lesson.
- Keep these four slides visually documentary and lightly worded. Charles supplies the detail aloud.

### Sources 19 and 21: Integration and Physical-Layer Debugging

- Put these after Charles's physical build so the deck reconnects copper to the internal packet path.
- Add a one-sentence note that Will's boxed-node software work and Charles's badge work occurred in parallel at separate benches.
- Source 19 shows scanner-to-uplink newline JSON at 921600 baud and the staged update/recovery path.
- Source 21 remains Will-led. Show the BERT-inspired CHOMP loop, clean 115200 behavior, corruption above roughly 512 kbps on one prototype, comparison against known-good prototypes, the thin solder stinger, and clean 921600 after removal.
- Use the existing signal-integrity illustration and real prototype image; do not recreate the original failure.

### Source 28: AI Reduced the Cost of Curiosity

- Replace the model/commit timeline with one image-led slide.
- Title: `AI Reduced the Cost of Curiosity.`
- Four minimal labels: `RESEARCH`, `CODE + FIRMWARE`, `HARDWARE DESIGN`, `TEST + DEBUG`.
- Use real project imagery from the app, simulator, boards, and bench. Do not use robot or synthetic-engineer art.
- Closing line: `AI suggested. We tested. Real packets and boards decided.`
- Keep model chronology out of the visible slide. The notes may state that several systems contributed and Codex later became the default engineering partner.

### Source 29: AI Put the Hardware in the Loop

- Remove `Claude 4.7` from the title and visible copy.
- Title: `AI Put the Hardware in the Loop.`
- Make the dominant visual a native loop: `ASK -> CHANGE -> FLASH -> OBSERVE -> REPEAT`.
- Pair it with one real bench/hardware photograph.
- Limit examples to: Remote ID simulator, UART update/recovery, and physical-layer debugging.
- No visible model names, commit IDs, chunk counts, or vendor comparison.
- Keep the two historical issues separate in the notes: the earlier working relay-update path and the later CHOMP/stinger durability investigation.

### Source 30: Recorded Proof

- Title: `The Badge Sees the Signal. Android Makes It Legible.`
- Use a large recorded-video frame showing the badge detecting Remote ID and privacy-device types.
- Place one real Android companion screenshot beside it.
- Captions: `RECORDED BADGE DETECTION` and `ANDROID COMPANION`.
- Footer: `Recorded proof first. Live badges second.`
- Remove backend dashboards from this main slide.
- Until the recording and Android screenshot are supplied, use a real badge still in the video frame and an explicitly labeled production slot for the Android image. Never fabricate either artifact.

## Bonus and Backup Order

Place bonus material immediately after the closing slide, followed by the existing appendices:

1. Backend detection/dashboard screenshots removed from source slide 30.
2. Redacted Wi-Fi/probe backend evidence removed from source slide 8.
3. Former main slide 15: packet observation versus verdict.
4. Existing skipped packet, real-flight, platform, update, math, and live-demo slides.
5. Existing technical appendices and bibliography.

All backup slides remain skipped by default. Their notes and source labels must make clear whether content is real evidence, a conceptual diagram, or simulated test data.

## Visual System

- Preserve the existing 16:9 documentary visual system and palette.
- Prefer one claim and one strong real image per slide.
- Keep visible text sparse enough to scan from the back of a village room.
- Maintain the amber Charles tags only on his five slides.
- Do not introduce corporate gradients, model logos as decoration, fake terminals, or generated evidence.
- Generated imagery is acceptable only for clearly labeled concepts such as the CHOMP signal-integrity illustration.
- Renumber every visible slide after reordering; manual number labels must match physical deck order.

## Notes and Timing

- Rewrite notes to follow the 40-minute timing table.
- Preserve verified technical detail in notes even when visible copy is reduced.
- Add handoffs at each lane boundary:
  - Packet discovery to platform: packet cadence selected the hardware.
  - Platform to Charles: this is the architecture Charles made wearable.
  - Charles to integration: the boards still had to exchange and update packets reliably.
  - Integration to AI: AI accelerated the experiment loop but did not decide truth.
  - AI to proof: show the resulting behavior on real hardware.
- Keep the parallel-work clarification in the hardware/integration transition.

## Verification

Before declaring the rebuild complete:

1. Render every slide and inspect the full contact sheet.
2. Inspect slides 7, 10, 14-24, and both bonus backend slides at full size.
3. Confirm Charles has exactly five visible amber-tagged slides and no additional default speaking assignments.
4. Confirm the main run ends at 40:00 before bonus material.
5. Confirm source slide 15 is skipped and no backend screenshot remains in the main talk.
6. Confirm slide 24 contains no fabricated Android UI or video evidence.
7. Confirm no private coordinates, SSIDs, MAC addresses, home maps, or other sensitive identifiers are visible.
8. Confirm all live-demo dependencies have recorded or still-image fallbacks.
9. Confirm the closing slide is the last unskipped slide.
