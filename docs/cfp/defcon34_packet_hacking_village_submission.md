To: cfp2026@wallofsheep.com
Subject: DEF CON 34 Packet Hacking Village CFP - Why Couldn't I See My Own Drone?

# DEF CON 34 Packet Hacking Village CFP Submission

## 1. Primary Speaker Name

Will Hatzer

## 2. Primary Speaker Title And Company

Founder, GameChangersAI / Team Charity Case

## 3. Primary Speaker Email Address

bill.hatzer@gmail.com

## 4. Primary Speaker Phone Number

669-245-8895

## 5. Primary Speaker Twitter Name

@lnxgod

## Primary Speaker Mastodon Name

None

## Primary Speaker Additional Links

- Website: https://gamechangersai.org
- LinkedIn: https://www.linkedin.com/in/william-hatzer-88624928/
- YouTube: https://www.youtube.com/@CharityCase42

## 6. Additional Speakers

Charles Grow

- Title: Hardware Designer / RF Researcher
- Organization: Team Charity Case / GameChangersAI
- Handle: OhYou_
- Email: cgrow4@gmail.com
- Website: https://www.charitycase.net/

## Additional Email Addresses

cgrow4@gmail.com

## Is There A Specific Day Or Time You MUST Speak By?

No hard constraints.

## 7. Name Of Presentation

Why Couldn't I See My Own Drone? Remote ID, ESP32s, and the Packet Trail to Friend or Foe

## 8. Length Of Presentation

50 minutes

## 9. Abstract

Friend or Foe did not start as a badge. It started when Will added Remote ID to an Android airspace app and still could not see his own DJI drone because it was not broadcasting Remote ID. That missing packet pulled us into DJI Wi-Fi clues, SSID/OUI research, ESP32 capture, BLE sanity checks, Remote ID simulation, known-good Remote ID hardware, RSSI lies, a C5 path we abandoned, UART pain, and finally a reflashable S3 badge. We'll show how AI helped us build low-cost RF sensors without turning hints into proof.

## 10. Has This Talk Been Given Before?

No.

## 11. Speaker Bio

Will "OGThorne" Hatzer is a security engineer, adversarial researcher, founder of GameChangersAI, and Team Charity Case member. He currently works in security engineering at OpenAI and builds open-source tools across RF, Android, embedded systems, AI-assisted engineering, and defensive security. A former DEF CON Car Hacking Village speaker, his work includes a bot-detection patent and Hyundai BlueLink research later referenced by CISA.

Charles "OhYou_" Grow is a hardware designer, RF researcher, ham radio operator, and longtime Team Charity Case member. He designed the Friend or Foe badge hardware, turning wired ESP32 prototypes into a practical PCB while solving component layout, USB-C access, GPIO conflicts, RF ground-plane, and deadline problems. His background in fox hunting, electronics, cars, and field debugging helped make the project measurable and wearable.

## 12. Detailed Outline

### 0:00-0:03 - Why can't I see my own drone?

Friend or Foe started as an Android app, not a badge. The original question was simple: what is actually flying near me, and should I care?

Local concern about drones and airspace made that question practical instead of theoretical. The first version used Android to make nearby aircraft and drone activity visible through ADS-B and augmented reality.

Packet focus: the first useful data was not a custom sensor. It was existing aviation telemetry made legible.

### 0:03-0:09 - The missing Remote ID packet

After ADS-B worked, Will added FAA Remote ID / OpenDroneID support to Android and expected his own DJI drone to appear. It did not. The first real packet lesson was that the app was not necessarily broken: that drone was not broadcasting Remote ID, so the evidence had to come from somewhere else.

That sent us into DJI Wi-Fi behavior, Wi-Fi Beacon Remote ID, vendor information elements, SSIDs, OUIs, and the uncomfortable difference between "the standard is open," "this drone broadcasts it," and "this receiver can see it." AI helped with the boring but important loop: collect drone SSID prefixes, check public references, add patterns, then write negative cases so every drone-looking Wi-Fi name did not become a confirmed drone.

We will show the troubleshooting trail in code, fixtures, screenshots, and examples: Wi-Fi Beacon Remote ID, DJI DroneID vendor IEs, SSID/OUI matching, BLE sanity checks, probe-request sniffing, parser tests, and the Remote ID simulator we built when we needed known-good packets. The point is not just what we detected; it is why each source gets different confidence.

Packet focus: the project stopped being "can we look up aircraft?" and became "why did the packet I expected not exist?"

### 0:09-0:15 - Getting below the phone API

When the phone view did not explain enough, we went after lower-level visibility. ESP32s looked cheap enough to risk and capable enough to get closer to Wi-Fi and BLE evidence. Prior Bluetooth work made the next step feel possible, and BLE sanity checks helped us ask whether the phone API, parser, drone, or assumptions were the problem.

The Remote ID simulator came out of that debugging loop. It gave us known-good OpenDroneID packets so we could test the Android and ESP32 parser paths without guessing whether the air was quiet or the code was wrong. Later we bought real Remote ID hardware, including a different drone and an XHover Puck setup, and captured other Remote ID drones in the wild.

We then built early ESP32 nodes, deployed them at a private test site, and started collecting BLE, Wi-Fi, and Remote ID data from multiple points at once. Remote ID was not the localization problem when it was present; compliant messages can carry GPS. The hard part was understanding when those packets were absent or hidden by transports/APIs, and what to do with cheaper drones, controllers, smart glasses, trackers, and other BLE/Wi-Fi devices.

The useful field lesson came during a Wi-Fi outage. Several WLED devices fell back into AP/setup mode, and those Wi-Fi packets were stable enough for the nodes to make sense of them on a map. That did not solve triangulation for BLE. It showed that multi-node Wi-Fi observations could work, while BLE RSSI/localization was the slippery part.

Packet focus: ESP32s and test transmitters let us separate "no packet," "wrong packet," "OS hid the packet," "our parser is wrong," and "we are inferring too much from RSSI."

### 0:15-0:21 - Packets kept proving our assumptions wrong

Both speakers are ham radio operators and have experience with fox hunts and signal hunting. That helped us think about invisible RF, antennas, and physical reality. It also misled us.

- Bluetooth was not a clean beacon.
- RSSI was not distance.
- BLE randomization broke assumptions.
- Remote ID with GPS was a different problem from BLE/Wi-Fi triangulation.
- Wi-Fi sometimes gave more useful clues than Bluetooth.
- SSIDs were hints, not proof.
- Probe requests said what a client wanted, not necessarily what was present.

This section teaches the practical difference between "I saw a packet" and "I know what it means."

Packet focus: raw packet collection is easy. Honest packet interpretation is hard.

### 0:21-0:27 - The C5 looked good on paper

We tested multiple ESP variants. The ESP32-C5 looked attractive because 5 GHz support sounded useful for the sensor platform. In the repo you can see us build C5 dual-band/interleaved scanning, then move the current release path back to S3-only firmware.

The lesson is not "the C5 is bad." The lesson is that the C5 path was not producing the packet stream we needed reliably enough on the timeline we had. We stopped trying to win the spec sheet and standardized on the ESP32-S3 so we could spend time analyzing BLE, Wi-Fi, and Remote ID behavior instead of fighting the platform.

Packet focus: hardware choice was not about the newest radio. It was about reliable packet capture, repeatable builds, and a firmware loop we could trust.

### 0:27-0:33 - UART, firmware updates, and the physical layer

Making the sensor platform update reliably was harder than expected. We had multiple scanner and uplink devices wired together, so GPIO conflicts, USB behavior, UART framing, scanner relay flashing, OTA rollback, and status telemetry all became part of the project.

The current scanner-to-uplink path sends newline-framed JSON over UART at 921600 baud. Badge host control and debug paths use USB serial/status commands such as `FOF_STATUS`. Scanner firmware can be staged and relayed through the uplink.

We will also tell one bench story where the bug looked like firmware until the physical UART wiring got inspected. The fix was not another parser patch; it was cleaning up the physical layer.

Packet focus: packet projects fail below the packet layer too.

### 0:33-0:38 - Privacy devices changed the scope

Smart glasses, trackers, cameras, drone controllers, and strange Wi-Fi behavior expanded the project beyond airspace. As smart glasses became common, we added detection logic for Meta/Ray-Ban-style devices using observable BLE/Wi-Fi behavior. We also researched drone identifiers and SSID/signature behavior beyond Remote ID.

This is where the project had to learn humility. Weak evidence presented as certainty becomes fearware, so the system needed source labels, confidence, decay, strict badge display policy, and negative cases.

Packet focus: detection is not magic. It is a stack of packet observations, heuristics, confidence, and humility.

### 0:38-0:42 - Charles's hardware journey

Charles walks through the badge hardware evolution: physical mockups, component choices, early wired prototypes, S3 placement, board bring-up, display fit, USB-C access, GPIO conflicts, hand soldering, and the RF/mechanical tradeoffs that made the badge usable.

The first version looked good until we tried to plug in a USB-C cable. The badge art also had to share space with copper, keepouts, antenna behavior, and the ground-plane conversation. It could not just look cool; it had to keep the radios useful.

Packet focus: the badge layout was shaped by RF performance, debugging access, and field repairability, not only aesthetics.

### 0:42-0:46 - AI at the workbench

We used several AI systems during the project, including Claude, Gemini, Grok, and OpenAI Codex. The README history tells part of that story: the earliest Android implementation was Claude-heavy, then the project became multi-AI, with different tools used for code, design, review, and research.

The hardware work was not one clean shared thread either. Will and Charles each had their own bench sessions, troubleshooting loops, parallel agent sessions, and conversations with tools while the hardware and software were still taking shape. Later, as the badge work consolidated into the main repo workflow, Codex became the tool we kept reaching for.

AI mattered a lot, mostly in the least glamorous places: Android parsers, ESP32 firmware, UART flashing loops, PCB reasoning, antenna and ground-plane questions, RSSI research, signature catalogs, tests, docs, and release prep. It also helped us get unstuck when a bug looked like software but belonged to hardware.

AI did not invent Friend or Foe. Humans chose the questions, built the hardware, validated the results, and made the final calls. What AI changed was the cost of exploration: packet work that used to feel hard to even start became easier to research, test, and iterate on, without pretending the model knew the truth.

Packet focus: AI helped us move through the loop faster: observe the packet, form a hypothesis, build a fixture or simulator, test on hardware, assign confidence, and keep weak evidence weak.

### 0:46-0:50 - Recorded demos, live badges, and what attendees can build

For the presentation, we will bring recorded demos of the full flow so the talk does not depend on DEF CON RF cooperating. We will also wear live badges on stage and show the Android companion view if the room gives us anything useful. If the RF environment is too noisy or too quiet, the recorded path keeps the technical demo clean and the live noise becomes part of the lesson.

The demo ties the story back to decoded evidence: OpenDroneID fields, DJI vendor information elements, Wi-Fi Beacon RID, SSID/probe clues, confidence, RSSI, and diagnostics.

We will close by bringing the badge back to the original goal: make packet exploration reachable. Friend or Foe is open source because the point is not selling a product; it is giving people a working stack they can take apart. The badge is reflashable, so it can become a home drone/RF sensor after DEF CON, or a safe place to experiment with firmware, BLE/Wi-Fi evidence, privacy examples, parser fixtures, flashing paths, and AI-assisted packet research.

That matters because the boring setup work is what stops a lot of people. We already fought the ESP32 firmware layout, scanner/uplink split, UART protocol, badge recovery paths, Android control surface, and backend ingest. Attendees can start from a working base and build something we did not think of.

GameChangersAI is a 501(c)(3) nonprofit focused on helping people learn AI through hands-on engineering projects. This badge is meant to keep teaching after the talk is over.

Closing lessons:

- Start with the question, not the hardware.
- Cheap sensors are useful if you respect their limits.
- RSSI lies, BLE randomization matters, and SSIDs are clues, not verdicts.
- AI can accelerate engineering, but hardware still gets the final vote.
- The useful loop is: observe, hypothesize, simulate, test, assign confidence, repeat.
- The best outcome is not that people use our badge; it is that they build something better.

Packet focus: the point is not to copy our badge. The point is to build better packet-aware tools.

### Attendee Takeaways

This talk is meant to be accessible to people who are new to packet work while still giving experienced hackers concrete implementation details. We will explain packet formats before using them and call out which claims are high confidence, low confidence, or just useful hints.

- How to build a basic Android or ESP32 passive RF collector.
- How to interpret Remote ID/OpenDroneID without pretending it sees everything.
- How to avoid overclaiming from RSSI, SSIDs, BLE signatures, and probe behavior.
- How to structure evidence strings, confidence, decay, and negative cases.
- How to use simulators and fixtures to tell silence from broken parsers.
- How to design packet tools for debugging, reflashing, recovery, and field use.
- How to keep experimenting after DEF CON with the same repo, badge hardware, firmware/flashing paths, and AI-assisted workflow.
- How to use AI to get started in packet research without letting it outrun real hardware validation.

### Demo / Backup Plan

Primary demo path:

- Pre-recorded demos prepared for the presentation showing the badge, Android badge console, dashboard, parser fixtures, and Remote ID simulator output.
- Decoded examples of OpenDroneID, DJI DroneID IE, Wi-Fi Beacon RID, SSID/probe evidence, and confidence decisions.
- Redacted dashboard screenshots/API samples from a real multi-node deployment.
- Parser and policy tests for OpenDroneID, Wi-Fi Beacon RID, SSID policy, badge display policy, and firmware update behavior.

Live add-on:

- FoF badges running live nearby BLE/Wi-Fi/Remote ID/privacy evidence.
- Android badge console over USB-C showing `FOF_STATUS`, scanner roles, evidence, confidence, RSSI, and diagnostics.

Backup path:

- Pre-captured redacted JSON/API samples and screenshots if the RF environment is too noisy or too quiet.
- RID simulator output and canned packet fixtures for parser walkthroughs.
- Badge hardware photos or physical badge teardown if live RF is not appropriate.

No active attacks, deauth, jamming, credential capture, or packet injection are part of this talk.

## 13. Supporting Files

Repository:

- https://github.com/lnxgod/friendorfoe

Supporting materials currently available in the repo and attached bundle:

- `README.md` - current badge/sensor-platform overview.
- `docs/badge/README.md` - badge operator guide, version matrix, USB-C/AP/BLE control status, scanner roles.
- `docs/ARCHITECTURE.md` - end-to-end scanner, uplink, backend, Android pipeline.
- `docs/THREAT_MODEL.md` - passive-only posture, confidence calibration, limitations.
- `docs/BAYESIAN_FUSION.md` - log-odds confidence model and source likelihood ratios.
- `docs/TRIANGULATION.md` - RSSI path-loss model, calibration, multilateration limits.
- `docs/examples/api-samples/` - redacted JSON/API samples from live endpoints.
- `docs/examples/fof_redact.py` - redaction script for packet-derived telemetry.
- `backend/app/services/drone_signature_reference.py` - curated drone SSID/signature references.
- `esp32/scanner/main/detection/open_drone_id_parser.c`
- `esp32/scanner/main/detection/ble_remote_id.c`
- `esp32/scanner/main/detection/wifi_beacon_rid_parser.c`
- `esp32/scanner/main/detection/dji_drone_id_parser.c`
- `esp32/scanner/main/detection/wifi_ssid_patterns.c`
- `esp32/scanner/main/detection/wifi_scanner.c`
- `esp32/rid-simulator/` - BLE Remote ID simulator used for parser/debug validation.
- `esp32/shared/uart_protocol.h`
- `android/app/src/main/java/com/friendorfoe/detection/OpenDroneIdParser.kt`
- `android/app/src/main/java/com/friendorfoe/detection/DjiDroneIdParser.kt`
- `android/app/src/main/java/com/friendorfoe/detection/RemoteIdScanner.kt`
- `android/app/src/main/java/com/friendorfoe/detection/WifiBeaconRemoteIdParser.kt`
- `android/app/src/main/java/com/friendorfoe/detection/WifiDroneScanner.kt`
- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt`
- `scripts/fof_badge_flash.py` - badge trio flasher and scanner relay workflow.
- `esp32/uplink/tools/recover_fof_badge.py` - stubborn badge uplink recovery tool.
- `esp32/web-flasher/` - browser flashing path/manifests.

Additional supporting bundle included with the email as a zip:

- `supporting-files/README.md` - manifest and captions.
- Early ESP32 drone-detector prototype photo.
- Live badge photos showing Remote ID and privacy-device evidence.
- Final badge hardware-in-progress photo showing the copper triangle/S3/GPS layout.
- Plastic mechanical fit-check photo showing screen/GPS/USB-C placement.
- Badge PCB/render image.
- Badge boot/version screen photo.
- Seeed XIAO ESP32-S3 hardware batch photo.

Additional screenshots and badge/prototype photos are included in this email bundle under `supporting-files/`. Redacted API/sample telemetry remains available in `docs/examples/api-samples/` in the repository. Recorded demos will be prepared for the presentation if accepted; the current supporting bundle includes screenshots, photos, and repository evidence.

We are not submitting final slides yet. These supporting files are included to show that the hardware, packet pipeline, parser tests, and demo path are real.

Suggested pre-reading for attendees if accepted: the project `README.md`, `docs/badge/README.md`, `docs/THREAT_MODEL.md`, and the parser files listed above. No SDR background is required.

## Equipment

We will bring all required demo hardware, including laptops, Android devices, ESP32 sensor prototypes, and Friend or Foe badges.

Requested:

- Standard projector.
- Audio / microphones.
- HDMI or USB-C display support.
- Small table space if available.

Internet is not required. For the presentation, we will have backup captures, screenshots, packet fixtures, and recordings in case the RF environment prevents a clean live demo.

## Additional Note

Friend or Foe is open source and the badge is a reflashable educational platform created through GameChangersAI, a 501(c)(3) nonprofit. Any badge distribution is donation-supported and secondary to the talk's main goal: teaching attendees how to reason about packets, low-cost RF sensors, hardware tradeoffs, and AI-assisted engineering.

## Terms And Conditions

Yes, I, Will Hatzer, have read and agree to the Grant of Copyright Use.

I, Will Hatzer, have read and understand and agree to the terms as detailed in the Agreement to Terms of Speaking Requirements.

## Logistics

- Will Hatzer: he/him, United States, no ADA needs, no visa required.
- Charles Grow: he/him, United States.
