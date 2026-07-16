# DEF CON 34 PHV Slide Deck Design

## Talk Contract

- **Accepted title:** Why Couldn't I See My Own Drone? Remote ID, ESP32s, and the Packet Trail to Friend or Foe
- **Speakers:** Will "OGThorne" Hatzer and Charles "OhYou_" Grow
- **Venue:** Packet Hacking Village Creator Stage, DEF CON 34
- **Accepted length:** 50 minutes
- **Rehearsal target:** 47-49 minutes, with a hard stop at 50 minutes
- **Primary source:** The CFP body emailed to `cfp2026@wallofsheep.com` on June 29, 2026
- **Supporting source:** The submitted CFP ZIP and repo verification notes
- **Delivery requirement:** PDF requested as soon as possible; all demo videos must be local files

The emailed CFP and the later repository copy use slightly different wording. This deck follows their shared chronology and the promises in the emailed abstract. Later repo wording is used only where it improves technical accuracy.

## Communication Job

By the end, Packet Hacking Village attendees should understand how a missing Remote ID detection became a disciplined packet-investigation loop, and feel capable of using low-cost hardware plus AI-assisted research to build honest RF tools without mistaking weak clues for proof.

## Non-Negotiable Promises

The deck must deliver every promise on this list:

- Preserve the ten accepted sections and their order.
- Show one understandable BLE Remote ID example and one Wi-Fi/DJI evidence example.
- Explain why ESP32 promiscuous capture exposed evidence the Android path did not.
- Show the Remote ID simulator as a known-good packet source.
- Explain Bayesian fusion at a practical level without turning the talk into a math lecture.
- Show how negative cases, source labels, confidence, and decay prevent fearware.
- Keep C5 claims careful: the path did not provide the reliable packet stream needed on the project timeline; do not claim an unsupported single root cause.
- Keep AI to roughly three minutes and frame model output as hypotheses tested against packets and hardware.
- Keep the badge as the result of the investigation, not the opening product pitch.
- Make the recorded demo the dependable technical path; treat live RF as additional proof.
- Keep the talk passive. No deauth, jamming, credential capture, packet injection, or active attacks.
- Include both speakers, with a practical default split of about 75 percent Will and 25 percent Charles.

## Accepted Timing Spine

| Time | Accepted section | Deck slides |
| --- | --- | --- |
| 0:00-0:03 | Why can't I see my own drone? | 1-2 |
| 0:03-0:09 | The missing Remote ID packet | 3-6 |
| 0:09-0:15 | Getting below the phone API | 7-10 |
| 0:15-0:21 | Packets kept proving our assumptions wrong | 11-13 |
| 0:21-0:27 | The C5 looked good on paper | 14-16 |
| 0:27-0:33 | UART, firmware updates, and the physical layer | 17-19 |
| 0:33-0:38 | Privacy devices changed the scope | 20-22 |
| 0:38-0:42 | Charles's hardware journey | 23-25 |
| 0:42-0:45 | AI at the workbench, short version | 26-27 |
| 0:45-0:50 | Recorded demo, physical badge, and what attendees can build | 28-31 |

## Speaker Choreography

### Default: 75/25

Will carries the origin, Android investigation, packet interpretation, C5 decision, confidence model, AI story, and closing. Charles owns the physical RF moments, S3 badge architecture, badge evolution, and live simulator/badge operation.

Charles's planned speaking windows:

- Slide 11: fox-hunting intuition and why BLE felt different.
- Slides 16-17: three-board S3 design and physical packet path.
- Slides 23-25: badge mockup, USB-C failure, PCB/RF tradeoffs.
- Slide 29: live LVCC simulator and badge proof.

This produces approximately 11-13 minutes of Charles speaking time without forcing constant handoffs.

### Alternate: Interwoven

Use the same slides and notes, but add Charles at the physical edge of each chapter:

- Slide 4: one line about silence being evidence at the bench.
- Slide 7: why a cheap ESP32 was worth trying.
- Slide 11: lead the fox-hunting comparison.
- Slides 14-17: share the C5/S3 and UART decisions.
- Slide 20: explain why privacy devices changed the RF target set.
- Slides 23-25: lead the hardware chapter.
- Slide 27: give one concrete example of AI advice that still needed bench validation.
- Slides 28-29: operate and narrate the demo.

The interwoven version should be chosen only after rehearsal. The default version is less fragile and better suited to a quieter co-speaker.

## Visual Direction

- Use 16:9 widescreen.
- Favor full-bleed documentary photos, large packet excerpts, and a few simple native diagrams.
- Palette: near-black, white, signal cyan, warning amber, detection magenta, and copper/gold sampled from the badge.
- Avoid corporate gradients, dashboard-card layouts, robot imagery, fake terminal output, and decorative circuit-board wallpaper.
- Visible text should normally be one claim plus one supporting line.
- Generated imagery is conceptual only. Never present an AI-generated packet capture, device screen, board, test result, or bench failure as evidence.
- Real packet captures, screenshots, hardware photos, and code excerpts receive small source labels.
- Do not show private test-site coordinates. LVCC coordinates may appear only in clearly labeled simulated Remote ID material.

## Slide Outline

### Slide 1 - Why Couldn't I See My Own Drone?

- **Time:** 0:00-0:25
- **Default lead:** Will
- **Alternate delivery:** Will opens; Charles gets a brief introduction by name.
- **Visible copy:**
  - Why Couldn't I See My Own Drone?
  - Remote ID, ESP32s, and the Packet Trail to Friend or Foe
  - Will Hatzer + Charles Grow
- **Visual:** Full-bleed real badge photograph. Prefer `docs/cfp/supporting-files/photos/04-live-badge-boot-version-field-signals.jpg`; replace it with a clean final-badge portrait when available.
- **Speaker notes:**
  - Introduce Will and Charles in one sentence each.
  - Mention Team Charity Case and GameChangersAI lightly; do not lead with employers.
  - Say: "We didn't set out to build a DEF CON badge. We were trying to answer a question."
  - Move immediately into the question.
- **Handoff:** "The first version of that question lived on my phone."

### Slide 2 - The App Could See Airplanes. My Drone Was Invisible.

- **Time:** 0:25-3:00
- **Default lead:** Will
- **Alternate delivery:** Charles closes with: "At this point, there was no badge and no custom RF hardware."
- **Visible copy:**
  - ADS-B worked.
  - The drone did not appear.
- **Visual:** Real Android ADS-B/AR screenshot beside a real photo of Will's DJI drone. If neither is usable, use **IG-01** as a conceptual opener and label it "conceptual." Do not fabricate an app screen.
- **Speaker notes:**
  - Give the local context without sensationalizing it: people were worried about unexplained aircraft and drones.
  - The practical question was: "What is actually flying near me, and should I care?"
  - ADS-B and AR made ordinary aircraft legible quickly.
  - The first useful telemetry already existed; the app made it understandable.
  - Set up the next expectation: Remote ID should do something similar for drones.
- **Handoff:** "So I added Remote ID and took my own drone outside."

### Slide 3 - Remote ID Should Have Made the Drone Appear.

- **Time:** 3:00-4:15
- **Default lead:** Will
- **Alternate delivery:** Charles can hold the original drone or controller while Will speaks.
- **Visible copy:**
  - Add OpenDroneID support.
  - Launch the drone.
  - See nothing.
- **Visual:** Three-frame progression using real screenshots or a simple native sequence. No fake UI.
- **Speaker notes:**
  - Explain Remote ID in one plain sentence: participating drones broadcast identity and flight information over local wireless transports.
  - The Android parser existed and the drone still did not appear.
  - Resist the temptation to say the app was broken or the drone was hiding.
  - State the question that drove the investigation: "Where, exactly, should this packet exist?"
- **Handoff:** "That missing packet turned out to be the most useful packet in the project."

### Slide 4 - No Packet Is Evidence Too.

- **Time:** 4:15-5:45
- **Default lead:** Will
- **Alternate delivery:** Charles adds: "On the bench, silence narrows the failure tree, but only if you trust the receiver."
- **Visible copy:**
  - No transmitter?
  - Wrong transport?
  - OS hid it?
  - Parser wrong?
- **Visual:** Simple native four-branch failure tree. Keep it flat and readable.
- **Speaker notes:**
  - A scanner that prints nothing does not tell you which layer failed.
  - List the four possibilities without resolving them yet.
  - This became the project's repeatable troubleshooting model.
  - The key move was to stop arguing with assumptions and create controlled evidence.
- **Handoff:** "Before we could control the evidence, we had to understand the paths."

### Slide 5 - One Standard Can Travel Through Several Packet Paths.

- **Time:** 5:45-7:15
- **Default lead:** Will
- **Alternate delivery:** Charles points to each radio path while Will explains it.
- **Visible copy:**
  - BLE advertisements
  - Wi-Fi Beacon Remote ID
  - DJI vendor information
  - SSID/OUI clues
- **Visual:** One simple native diagram: drone at left, four packet/evidence paths, receivers at right. Use solid lines for decoded Remote ID and dotted lines for heuristic clues.
- **Speaker notes:**
  - BLE OpenDroneID and Wi-Fi Beacon Remote ID can carry structured Remote ID data.
  - DJI vendor information elements are another packet source.
  - SSIDs and OUIs are clues, not equivalent to structured Remote ID.
  - Android device/API support changes which paths are visible.
  - Promise the audience one BLE example and one Wi-Fi/DJI example later in the recorded demo.
- **Handoff:** "My drone did give us Wi-Fi clues, but clues create a different problem."

### Slide 6 - We Followed DJI's Wi-Fi Trail Without Calling It Proof.

- **Time:** 7:15-9:00
- **Default lead:** Will
- **Alternate delivery:** Charles can describe what was observable with external RF tools.
- **Visible copy:**
  - Structured packet: stronger evidence
  - Vendor IE: useful evidence
  - SSID/OUI: clue
- **Visual:** Real redacted Wi-Fi evidence screenshot plus a tightly cropped code excerpt from the SSID/OUI catalog. Use `docs/cfp/supporting-files/screenshots/03-wifi-probe-identity-ssid-evidence-redacted.png` as one option.
- **Speaker notes:**
  - Explain the research loop: collect public prefixes and references, add patterns, then write negative cases.
  - A drone-looking SSID must not become a confirmed drone by itself.
  - Probe requests report what a client seeks, not necessarily what is present.
  - Introduce the evidence-strength idea without explaining Bayesian fusion yet.
- **Handoff:** "We still needed one thing the air could not argue with: a packet we generated ourselves."

### Slide 7 - We Needed a Packet We Could Trust.

- **Time:** 9:00-10:30
- **Default lead:** Will
- **Alternate delivery:** Charles explains why an independent receiver matters.
- **Visible copy:**
  - Known transmitter
  - Known fields
  - Known timing
- **Visual:** Real early simulator photo `docs/cfp/supporting-files/photos/01-early-esp32-oled-drone-simulator-rssi-location.jpg`, with the existing coordinate redaction preserved.
- **Speaker notes:**
  - Phone APIs, unknown drone behavior, and quiet air created too many variables.
  - A controlled transmitter lets the team test parser behavior without guessing.
  - Mention Ubertooth/BLE sanity checks only at a high level; this is not a packet-dissection section.
  - The goal was not to emulate every drone. It was to create known-good OpenDroneID messages.
- **Handoff:** "That debugging fixture became its own little flight simulator."

### Slide 8 - The ESP32 Simulator Gave Us Known-Good Remote ID.

- **Time:** 10:30-11:45
- **Default lead:** Will
- **Alternate delivery:** Charles holds the simulator while Will explains the packet path.
- **Visible copy:**
  - Two `FOF-SIM` aircraft
  - BLE OpenDroneID
  - Deterministic flight paths
- **Visual:** Real simulator board photo and a native mini-diagram of the two orbit paths. Do not show private coordinates.
- **Speaker notes:**
  - The current simulator broadcasts two BLE OpenDroneID aircraft.
  - Each aircraft has a unique serial and a generated orbit with location, altitude, heading, and speed.
  - For DEF CON, a separate demo build will center both orbits on LVCC without changing normal defaults.
  - The simulator will be powered and verified before the talk begins.
- **Handoff:** "A simulator proves the parser. Real hardware tells you whether the world agrees."

### Slide 9 - Real Remote ID Hardware Closed the Loop.

- **Time:** 11:45-13:00
- **Default lead:** Will
- **Alternate delivery:** Charles shows the Puck or known-good hardware as a physical prop.
- **Visible copy:**
  - Simulator
  - Known-good hardware
  - Real aircraft captures
- **Visual:** **R-03**, a real photo of the XHover Puck/known-good Remote ID hardware and a redacted capture from a real flight. No generated substitute should be presented as evidence.
- **Speaker notes:**
  - The simulator separated parser correctness from air uncertainty.
  - Additional Remote ID hardware and another drone provided real-world validation.
  - Later captures of other Remote ID aircraft confirmed that the receive path worked outside the lab.
  - Keep the lesson focused: controlled fixture first, field evidence second.
- **Handoff:** "Once we trusted the packets, the phone itself became the next limitation."

### Slide 10 - Cheap ESP32 Nodes Got Below the Phone API.

- **Time:** 13:00-15:00
- **Default lead:** Will
- **Alternate delivery:** Charles gives the final 30 seconds on why the boards were cheap enough to deploy widely.
- **Visible copy:**
  - BLE scanning + Wi-Fi promiscuous capture
  - Persistent collection
  - Multiple observation points
- **Visual:** Photo of the early ESP32 hardware plus **IG-02** for the WLED contrast if no real deployment visualization is safe to use.
- **Speaker notes:**
  - ESP32s were inexpensive enough to become persistent sensors instead of occasional test tools.
  - Wi-Fi promiscuous capture exposed management-frame and vendor evidence that normal Android application APIs did not reliably surface.
  - Multiple nodes started collecting BLE, Wi-Fi, and Remote ID observations.
  - Clarify that structured Remote ID can include GPS; triangulation was aimed at cheaper/non-compliant drones and other BLE/Wi-Fi devices.
  - Tell the WLED outage story carefully: AP/setup-mode Wi-Fi packets were stable enough to map, showing that multi-node sensing worked while BLE localization remained slippery.
  - Do not show the private test site's coordinates.
- **Handoff:** "That was the moment we learned the sensor network was not broken. Our BLE assumptions were."

### Slide 11 - We Already Knew RF. We Still Had to Relearn It.

- **Time:** 15:00-17:00
- **Default lead:** Charles
- **Alternate delivery:** Charles leads; Will asks the setup question: "Why didn't fox-hunting intuition transfer?"
- **Visible copy:**
  - A fox beacon behaves like a beacon.
  - BLE often does not.
- **Visual:** Prefer **R-04**, a real photo of Charles's receive-only fox-hunting gear. Use **IG-03** only as a clearly conceptual fallback.
- **Speaker notes:**
  - Briefly establish that both speakers are ham radio operators with fox-hunting experience.
  - Show the receive-only gear physically if practical.
  - A fox transmitter is comparatively stable and intentional.
  - BLE advertisements are bursty, randomized, filtered by receivers, and affected by body position and the environment.
  - Deliver the line: "I already knew RF. I still had to relearn RF."
- **Handoff:** "The first bad assumption was the one every RSSI project makes."

### Slide 12 - RSSI Is Not Distance.

- **Time:** 17:00-19:00
- **Default lead:** Will, with a short Charles example
- **Alternate delivery:** Charles explains the physical causes; Will explains the software consequence.
- **Visible copy:**
  - Same device.
  - Same location.
  - Different answer.
- **Visual:** Native conceptual chart or real controlled measurement data if collected before slide production. Do not invent measurements. **IG-04** may be used only as a conceptual background.
- **Speaker notes:**
  - Multipath, antenna orientation, bodies, transmit power, receiver differences, and burst timing all move RSSI.
  - RSSI can support a trend or bounded estimate after calibration; it is not a tape measure.
  - Remote ID with reported GPS is a different localization problem from ordinary BLE/Wi-Fi devices.
  - The system should communicate uncertainty instead of manufacturing precision.
- **Handoff:** "That forced us to separate what the packet said from what we inferred."

### Slide 13 - A Packet Is an Observation, Not a Verdict.

- **Time:** 19:00-21:00
- **Default lead:** Will
- **Alternate delivery:** Charles gives one example from field testing; Will defines the evidence ladder.
- **Visible copy:**
  - Saw: a packet
  - Inferred: a device class
  - Claimed: only what the evidence supports
- **Visual:** Simple native evidence ladder. Strong evidence at the top, heuristic clues below. Avoid decorative complexity.
- **Speaker notes:**
  - Raw collection is easy; honest interpretation is hard.
  - Structured Remote ID is stronger than a matching SSID.
  - Multiple independent observations can strengthen confidence, but repetition from one weak source does not magically become proof.
  - This principle guides the later privacy-device work.
- **Handoff:** "The same discipline had to apply to the hardware platform itself."

### Slide 14 - The C5 Looked Perfect on Paper.

- **Time:** 21:00-23:00
- **Default lead:** Will
- **Alternate delivery:** Charles introduces the hardware appeal; Will gives the software/packet result.
- **Visible copy:**
  - 5 GHz support
  - Newer radio
  - More theoretical visibility
- **Visual:** Real C5 board photo beside the relevant repo timeline. Use **R-05**; do not use an invented product render.
- **Speaker notes:**
  - Explain why 5 GHz visibility sounded valuable.
  - The repo shows a real dual-band/interleaved C5 path, not a paper exercise.
  - Avoid saying the C5 is bad or that one silicon flaw caused everything.
  - Set up the engineering criterion: useful hardware is hardware that produces repeatable packet data on the available timeline.
- **Handoff:** "The feature list was right. The packet stream was not reliable enough for us."

### Slide 15 - Packet Reliability Beat the Spec Sheet.

- **Time:** 23:00-25:00
- **Default lead:** Will
- **Alternate delivery:** Charles covers bench stability; Will covers capture cadence and release risk.
- **Visible copy:**
  - Reliable capture
  - Repeatable builds
  - Recoverable firmware
- **Visual:** Native comparison with only verified statements: C5 explored for dual-band; current release standardized on S3. A commit timeline is preferable to a scorecard.
- **Speaker notes:**
  - Scan cadence, tooling maturity, and firmware stability collectively consumed time and reduced trust in the data path.
  - The team stopped trying to win the spec sheet.
  - The decision was about the project deadline and packet reliability, not a universal benchmark.
  - Standardizing let the team return to packet interpretation.
- **Handoff:** "The practical answer was three familiar S3 boards with explicit jobs."

### Slide 16 - Three S3 Boards Gave Each Radio Job Room to Breathe.

- **Time:** 25:00-27:00
- **Default lead:** Charles
- **Alternate delivery:** Charles leads; Will names the runtime roles and software boundary.
- **Visible copy:**
  - BLE-primary scanner
  - Wi-Fi-primary scanner
  - Uplink/controller
- **Visual:** Use `docs/cfp/supporting-files/photos/07-final-badge-hardware-copper-triangle-s3-gps-layout.jpg`, annotated with three restrained labels. Also keep a simple architecture version for the appendix.
- **Speaker notes:**
  - Explain the current three-board Seeed XIAO ESP32-S3 badge architecture.
  - The two scanner boards share the scanner firmware image; the uplink assigns active roles/profiles.
  - This is not an argument that three MCUs are elegant in the abstract. It was a testable, recoverable platform that met the project needs.
  - Transition from radio roles to the inter-board packet path.
- **Handoff:** "Once we split the jobs, we had to make three computers behave like one badge."

### Slide 17 - The Packet Pipeline Crossed Three Boards at 921600 Baud.

- **Time:** 27:00-29:00
- **Default lead:** Charles
- **Alternate delivery:** Charles explains the wires; Will explains the framing and host controls.
- **Visible copy:**
  - Scanner -> newline JSON -> uplink
  - `921600` baud
  - `FOF_STATUS` over USB
- **Visual:** One native architecture diagram with arrows behind the nodes. Use the final hardware photo as a faint background only if labels remain readable.
- **Speaker notes:**
  - Scanner-to-uplink communication uses newline-framed JSON at 921600 baud.
  - USB status/control includes commands such as `FOF_STATUS`.
  - Keep the explanation at the system level; do not read the schema aloud.
  - The important lesson is that a packet sensor also needs a trustworthy internal packet path.
- **Handoff:** "Then we tried to update all three boards without taking the badge apart."

### Slide 18 - Updating the Badge Became Its Own Protocol.

- **Time:** 29:00-31:00
- **Default lead:** Will
- **Alternate delivery:** Charles explains physical access; Will explains staged relay and recovery.
- **Visible copy:**
  - Stage firmware
  - Relay to scanners
  - Verify identity and status
  - Recover when it fails
- **Visual:** Simple four-step native flow; supporting screenshot from the flashing/recovery tools in the appendix.
- **Speaker notes:**
  - GPIO conflicts, USB behavior, UART framing, OTA rollback, and telemetry became first-class engineering problems.
  - Scanner firmware can be staged and relayed through the uplink.
  - Recovery tooling exists because field hardware eventually enters a weird state.
  - Tie this back to attendee value: design for debugging and reflashing on day one.
- **Handoff:** "One failure survived every software change because it was never a software failure."

### Slide 19 - The Software Bug Was a Wire.

- **Time:** 31:00-33:00
- **Default lead:** Will
- **Alternate delivery:** Charles shows a recreated cable while Will tells the story.
- **Visible copy:**
  - High-speed UART instability
  - One stray wire stub
  - Physical fix, software symptom gone
- **Visual:** Prefer **R-06**, a staged macro photograph of the actual or accurately recreated wire condition. **IG-05** is allowed only as a labeled illustration, never evidence.
- **Speaker notes:**
  - Tell the bench anecdote plainly: firmware updates became unstable at high speed.
  - The debugging conversation eventually pushed inspection below the code.
  - A small wire stinger/stub was enough to destabilize the physical link.
  - Cleaning the wire fixed the apparent firmware bug.
  - Land the packet-village lesson: packet projects fail below the packet layer too.
- **Handoff:** "Once the platform stayed up, it started showing us much more than drones."

### Slide 20 - The Project Stopped Being Only About Drones.

- **Time:** 33:00-34:30
- **Default lead:** Will
- **Alternate delivery:** Charles names the physical devices; Will explains the detection scope.
- **Visible copy:**
  - Smart glasses
  - Trackers
  - Cameras and controllers
  - Strange Wi-Fi behavior
- **Visual:** Use `docs/cfp/supporting-files/photos/02-live-badge-remote-id-meta-glasses-detection.jpg` or `docs/cfp/supporting-files/photos/06-live-badge-remote-id-meta-glasses-alt-view.jpg` with the existing location redaction intact.
- **Speaker notes:**
  - The same passive BLE/Wi-Fi platform could surface privacy-relevant device behavior.
  - Do not claim universal detection or identity.
  - Explain that privacy detections combine observable signatures with strict confidence and display policy.
  - This scope expansion created a stronger need for conservative interpretation.
- **Handoff:** "More signatures meant more ways to be confidently wrong."

### Slide 21 - Confidence Had to Survive Mixed Evidence.

- **Time:** 34:30-36:30
- **Default lead:** Will
- **Alternate delivery:** Charles provides a field example; Will explains the fusion model.
- **Visible copy:**
  - Start with prior odds.
  - Add evidence by source strength.
  - Decay stale observations.
- **Visual:** Use `docs/cfp/supporting-files/screenshots/04-all-detections-confidence-evidence-redacted.png` with one enlarged evidence explanation. Add a small native log-odds equation only if it remains legible.
- **Speaker notes:**
  - Give the practical Bayesian explanation: each source moves confidence according to how diagnostic it is.
  - Independent structured evidence moves confidence more than a generic name or OUI.
  - Old observations decay rather than remaining permanent truth.
  - Negative cases matter because they reveal which features are not diagnostic.
  - Keep this to the decision model; detailed likelihood ratios belong in the appendix.
- **Handoff:** "The math was useful only if the screen stayed honest."

### Slide 22 - Weak Evidence Presented as Certainty Becomes Fearware.

- **Time:** 36:30-38:00
- **Default lead:** Will
- **Alternate delivery:** Charles closes with the field-debugging version of the rule.
- **Visible copy:**
  - SSID: clue
  - OUI: clue
  - Structured Remote ID: stronger evidence
  - Unknown stays unknown
- **Visual:** Native evidence ladder over a dark background. No alarm iconography or surveillance imagery.
- **Speaker notes:**
  - A privacy tool that labels every weak match as a threat teaches the wrong lesson.
  - Source labels, confidence, decay, and strict badge display rules are part of technical correctness.
  - Say: "The system should be allowed to say, 'I saw something interesting and I do not know what it is.'"
  - Transition to the physical object that had to communicate those limits.
- **Handoff:** "Then Charles had to fit all of that honesty into something we could wear."

### Slide 23 - The First Badge Existed in Plastic and Marker.

- **Time:** 38:00-39:15
- **Default lead:** Charles
- **Alternate delivery:** Charles leads; Will supplies only the timeline transition.
- **Visible copy:**
  - Place the parts.
  - Route the wires.
  - Find the problems before copper.
- **Visual:** `docs/cfp/supporting-files/photos/08-plastic-mechanical-fit-check-screen-gps-usbc.jpg` full bleed, with minimal callouts.
- **Speaker notes:**
  - Explain how the physical mockup established display, GPS, board, cable, and connector placement.
  - The goal was not a polished prop; it was to discover mechanical mistakes cheaply.
  - Mention the center electronics prototype before the full run.
- **Handoff:** "The mockup looked plausible right up until we tried the most ordinary operation."

### Slide 24 - It Looked Good Until We Tried USB-C.

- **Time:** 39:15-40:30
- **Default lead:** Charles
- **Alternate delivery:** Charles tells the story; Will advances the before/after image.
- **Visible copy:**
  - The screen blocked the cable.
  - Debug access is a hardware feature.
- **Visual:** Before/after crop using the plastic prototype and final hardware. Use arrows sparingly.
- **Speaker notes:**
  - Deliver the line: "The first version looked good until we tried to plug in a USB-C cable."
  - Explain how display size/position, board orientation, and connector clearance forced another iteration.
  - Connect this to field repairability and reflashing, not aesthetics alone.
- **Handoff:** "The layout still had to work electrically and as RF hardware."

### Slide 25 - The Final Layout Had to Work as RF Hardware.

- **Time:** 40:30-42:00
- **Default lead:** Charles
- **Alternate delivery:** Charles leads; Will adds the runtime-role sentence.
- **Visible copy:**
  - Three S3 boards
  - Copper, keepouts, antennas
  - Display and debug access
- **Visual:** Pair `docs/cfp/supporting-files/photos/03-badge-pcb-render-triangle-layout-buttons-battery.jpg` with `docs/cfp/supporting-files/photos/07-final-badge-hardware-copper-triangle-s3-gps-layout.jpg`. Replace with final production front/back photographs when available.
- **Speaker notes:**
  - Walk through component placement, GPIO conflicts, board bring-up, hand soldering, RF/mechanical constraints, and deadline pressure.
  - If Charles confirms it for the final talk, explain how the decorative copper/Triforce area participates in the ground-plane strategy. Present it as his design account, not a repo-proven benchmark.
  - The badge is the last chapter because the packet platform dictated its shape.
- **Handoff:** "We got there faster because we were not solving every unfamiliar problem alone."

### Slide 26 - AI Reduced the Cost of Asking the Next Question.

- **Time:** 42:00-43:20
- **Default lead:** Will
- **Alternate delivery:** Will covers the timeline; Charles names one hardware question he explored separately.
- **Visible copy:**
  - Claude-heavy first implementation
  - Multi-model research and review
  - Codex became the daily engineering partner
- **Visual:** Real, redacted README/commit-history timeline. Optional **IG-06** may support the workbench mood but must not replace real evidence.
- **Speaker notes:**
  - The earliest Android implementation was Claude-heavy.
  - Different models were used for code, design, research, and review.
  - Will and Charles had separate bench sessions and tool conversations before the work consolidated.
  - During the badge phase, Codex became the tool repeatedly used for firmware, tests, PCB reasoning, documentation, and release work.
  - Do not turn this into an OpenAI advertisement.
- **Handoff:** "The useful part was not generating more code. It was shortening the experiment loop."

### Slide 27 - Every Model Answer Still Had to Survive the Bench.

- **Time:** 43:20-45:00
- **Default lead:** Will, with one Charles example
- **Alternate delivery:** Charles gives the hardware example; Will gives the parser example and conclusion.
- **Visible copy:**
  - Observe -> hypothesize -> simulate -> test -> assign confidence -> repeat
- **Visual:** One clean native loop diagram. No robot, magic wand, or glowing brain imagery.
- **Speaker notes:**
  - AI helped research packet formats, scaffold parsers, propose tests, debug UART, and reason about hardware.
  - Models also hallucinated packet layouts and proposed impossible workarounds.
  - Humans chose the question and experiment; packets and boards decided what was true.
  - Deliver the line: "The biggest thing AI changed was not writing code. It reduced the cost of curiosity."
- **Handoff:** "Here is what that loop produced when we run it end to end."

### Slide 28 - Recorded Proof First; Live RF as a Bonus.

- **Time:** 45:00-46:20
- **Default lead:** Will
- **Alternate delivery:** Charles starts the recording; Will narrates the first half.
- **Visible copy:**
  - Known packet
  - Decoded evidence
  - Badge decision
  - Android/dashboard view
- **Visual:** Embedded local MP4 with a strong poster frame. Prepare separate clips for BLE OpenDroneID and Wi-Fi/DJI evidence rather than one long screen recording.
- **Speaker notes:**
  - Explain that DEF CON RF should not control whether the technical story works.
  - Recorded sequence: simulator or known-good source, packet/capture evidence, parser output, confidence, badge display, Android/dashboard.
  - Include one BLE Remote ID example and one Wi-Fi Beacon RID or DJI vendor-IE example.
  - Keep the packet walkthrough high-level: expected field, observed packet, interpretation, confidence.
- **Handoff:** "The recording is our control. The room gives us the live experiment."

### Slide 29 - Two Simulated Drones Over LVCC.

- **Time:** 46:20-47:50
- **Default lead:** Charles operates; Will supplies one short explanatory line if needed.
- **Alternate delivery:** Charles operates and narrates the entire live beat.
- **Visible copy:**
  - `FOF-SIM-001`
  - `FOF-SIM-002`
  - SIMULATED - LVCC
- **Visual:** Live badge plus mirrored Android map/console. Use **R-07** as the backup screenshot if the live display path fails.
- **Speaker notes:**
  - The ESP32 simulator is already powered before the talk.
  - Both BLE OpenDroneID aircraft orbit a configured LVCC center using the existing two-drone behavior.
  - Explicitly state that these are simulated test aircraft and no physical drones are being flown.
  - Point out serial, location, and badge evidence without waiting for perfect RF timing.
  - If live output is noisy, say so once, show the prepared screenshot, and move on.
- **Handoff:** "The useful part is not that our badge can see these two packets. It is that the whole stack is open for the next experiment."

### Slide 30 - Take It Home. Reflash It. Build Something Better.

- **Time:** 47:50-49:00
- **Default lead:** Will
- **Alternate delivery:** Charles holds the badge while Will closes the practical section.
- **Visible copy:**
  - Android collector
  - ESP32 firmware
  - Simulator and fixtures
  - Flashing and recovery paths
  - Open source
- **Visual:** Final badge connected by USB-C beside the repository QR code. Prefer **R-08**, a real staged photograph; **IG-07** is a fallback concept only.
- **Speaker notes:**
  - The badge can become a home drone/RF sensor after DEF CON.
  - More importantly, attendees can reflash it, change parsers, add safe signatures, or use the simulator to test an idea.
  - The boring setup work is already represented in the repo: firmware layout, scanner/uplink split, UART protocol, recovery, Android control, and backend ingest.
  - Mention GameChangersAI once as the reason the platform is intended to keep teaching.
  - Do not imply that receiving a badge is required to use the code.
- **Handoff:** "So what was the answer to the question we started with?"

### Slide 31 - The Answer Was Never One Packet.

- **Time:** 49:00-50:00
- **Default lead:** Will; Charles joins the final line.
- **Alternate delivery:** Will gives the first three lines; Charles gives the final sentence.
- **Visible copy:**
  - Start with the question.
  - Let packets prove you wrong.
  - Keep weak evidence weak.
  - Build the next experiment.
- **Visual:** Full-bleed final badge photograph or **IG-08** created from the real badge as a reference. No "Thank you" title.
- **Speaker notes:**
  - The original drone was not visible through the Remote ID path the app expected.
  - Solving that mystery required changing receivers, creating known-good packets, testing real hardware, and respecting uncertainty.
  - Cheap sensors became useful when the team stopped asking them to be certain.
  - AI accelerated the loop; hardware and captures retained the final vote.
  - Final line, together or by Charles: "The best outcome is not that you use our badge. It is that you build something better."
- **End state:** Stop here. Bibliography and backup slides follow but are not part of the spoken ending.

## Backup And Reference Slides

These slides remain after the closing slide in the PDF but are not presented unless needed.

1. **Remote ID field guide:** BLE OpenDroneID Basic ID and Location, Wi-Fi Beacon RID, and which fields matter to the demo.
2. **DJI evidence guide:** vendor information element, SSID/OUI clue, and the confidence distinction.
3. **Bayesian fusion details:** log-odds equation, source likelihood categories, decay, and one worked example using sanitized values.
4. **C5-to-S3 repo timeline:** commits showing C5 exploration, S3 combo firmware, and S3-only production artifacts.
5. **Badge architecture:** scanner roles, uplink, UART direction, USB host control, and backend/Android path.
6. **UART and recovery:** 921600 scanner link, 115200 host-control context, staged scanner relay, and recovery tooling.
7. **Simulator internals:** two BLE advertisers, `FOF-SIM-001/002`, orbit behavior, and dedicated LVCC configuration.
8. **Passive-only boundary:** no attacks, injection, jamming, deauth, credentials, or person tracking.
9. **Build/resources:** repository paths, flashing docs, parser fixtures, simulator, and redacted API samples.
10. **Bibliography:** FAA Remote ID material, OpenDroneID/ASTM references permitted for citation, Espressif documentation, project repository files, and any external signature references actually shown.

## Image Generation Placeholders

Generated visuals are allowed only for concepts. Real packet evidence and hardware failures require real assets.

### IG-01 - Missing Packet Opener

- **Used on:** Slide 2 fallback
- **Prompt:** "Wide documentary-style scene of a small unbranded consumer drone in an ordinary California suburban sky at late afternoon, a person below holding a phone with the screen turned away and unreadable, subtle Bluetooth and Wi-Fi signal traces fading before reaching the phone, realistic engineering-documentary mood, curious rather than threatening, subject on right with clean negative space on left, charcoal sky with cyan, amber, and magenta signal accents, no text, no logos, no fake interface, 16:9."

### IG-02 - Multi-Node Wi-Fi Contrast

- **Used on:** Slide 10 fallback
- **Prompt:** "Clean bird's-eye technical illustration of a private test property with three small passive RF sensor nodes and several smart LED controllers returning in Wi-Fi access-point mode after an outage, stable Wi-Fi packet paths visible between devices and sensors while scattered BLE observations appear irregular, dark neutral background, cyan Wi-Fi, magenta BLE, amber event markers, no addresses, no coordinates, no labels, no people, 16:9."

### IG-03 - Fox-Hunting Gear Still Life

- **Used on:** Slide 11 fallback
- **Prompt:** "Realistic documentary still life of receive-only amateur-radio fox-hunting equipment on the open tailgate of a car: compact directional Yagi antenna, handheld receiver, coax cable, notebook and map, natural daylight, used practical gear, no people, no readable call signs, no text, no logos, balanced 16:9 composition with negative space for a short title."

### IG-04 - Bursty RSSI Concept

- **Used on:** Slide 12 background only
- **Prompt:** "Scientific conceptual visualization of bursty Bluetooth observations from one stationary device producing a wide cloud of inconsistent signal-strength samples because of multipath and body shadowing, black background, restrained cyan and magenta points, subtle amber reflection paths, no numbers, no axes, no text, no implication that the values are measured data, 16:9."

### IG-05 - UART Physical-Layer Illustration

- **Used on:** Slide 19 only if no real recreation photo exists
- **Prompt:** "Extreme macro technical illustration of a high-speed UART jumper connection between two embedded boards, highlighting one tiny stray wire stub creating signal integrity trouble, realistic copper strands and solder, dark electronics bench, shallow depth of field, no text, no logos, clearly illustrative rather than forensic, 16:9."

### IG-06 - AI At The Workbench

- **Used on:** Slide 26 as optional support
- **Prompt:** "Documentary engineering workbench with human hands debugging an ESP32 board beside a laptop showing unreadable code and packet traces, oscilloscope probes, PCB notes and datasheets, warm practical bench light with cyan and copper accents, no robot, no glowing brain, no logos, no readable confidential text, realistic and imperfect, 16:9."

### IG-07 - Reflash And Explore

- **Used on:** Slide 30 fallback
- **Prompt:** "Realistic maker workbench scene using the supplied Friend or Foe triangular badge as an exact visual reference, badge connected over USB-C to a laptop beside an ESP32 programmer and handwritten experiment notes, open-source learning atmosphere rather than product advertising, charcoal, cyan, copper and amber palette, no readable screen text, negative space for a QR code, 16:9."

### IG-08 - Closing Badge Portrait

- **Used on:** Slide 31 fallback
- **Prompt:** "Hero documentary photograph using the supplied Friend or Foe badge as an exact reference, assembled triangular badge resting on a real electronics bench with subtle packet-capture equipment in the background, crisp badge details, honest used hardware, black and copper palette with cyan display light, clean dark negative space for four short closing lines, no invented components, no text, no logos beyond those already on the real badge, 16:9."

## Real Asset Capture List

These assets should be collected before slide production. Each must be scrubbed for home coordinates, Wi-Fi credentials, email addresses, tokens, and private device identifiers.

- **R-01:** Android ADS-B/AR screen and Will's original DJI drone.
- **R-02:** Clean BLE OpenDroneID packet/capture from the repo simulator.
- **R-03:** XHover Puck or other known-good Remote ID hardware plus sanitized real capture.
- **R-04:** Charles's fox-hunting antenna and receiver gear.
- **R-05:** C5 and S3 boards photographed side by side.
- **R-06:** Actual or accurately recreated UART wire-stinger condition.
- **R-07:** Backup screenshot of both LVCC-based `FOF-SIM` drones in the Android map/console and badge output.
- **R-08:** Final assembled badge front/back and a USB-C reflashing photograph.
- **R-09:** Two short recorded demos: BLE Remote ID path and Wi-Fi/DJI evidence path.
- **R-10:** Redacted Codex/README/commit-history evidence for the AI timeline.

## Demo Runbook

- Build a dedicated LVCC simulator profile; do not replace the normal simulator defaults.
- Preserve the existing two-drone behavior and identifiers.
- Clearly label both outputs as simulated.
- Power and verify the simulator before the talk starts.
- Verify both badges and the Android display before leaving for Speaker Ops.
- Keep the recorded BLE and Wi-Fi/DJI clips on the presentation laptop and a second USB device.
- Use local media only; do not depend on internet access.
- Prepare a still screenshot for every live display step.
- If live detection does not appear promptly, acknowledge the RF environment once and advance to the prepared screenshot.
- Do not expose private test-site coordinates. Only intentionally simulated LVCC coordinates may be shown.
- Bring the original prototype, plastic fit check, final badge, simulator, known-good Remote ID hardware, and receive-only fox-hunting gear if stage/table space permits.

## Production And Delivery

- Build both PowerPoint and PDF versions.
- Use the organizer's PDF naming convention:
  - `DEF CON 34 - Packet Hacking Village - Will Hatzer & Charles Grow - Why Couldn't I See My Own Drone.pdf`
- Embed or package videos in common formats and test playback offline.
- Put bibliography and source slides after the spoken closing.
- Use source footers on packet captures, standards material, screenshots, and external device references.
- Rehearse the default 75/25 version first, then test the interwoven version with Charles.
- The deck must reach the closing slide by 49:00 in rehearsal.
- Export and visually inspect every slide at presentation resolution before delivery.

## Approval Gate

Will and Charles should review this document before slide production, with special attention to:

- Whether Charles is comfortable with Slides 11, 16-17, 23-25, and 29.
- Whether the Triforce/ground-plane account is technically accurate enough to say on stage.
- Whether the UART wire-stinger anecdote can be recreated accurately.
- Whether the real Wi-Fi/DJI example is clear without a deep packet teardown.
- Whether the two-drone LVCC simulator profile and live display path are ready to rehearse.
