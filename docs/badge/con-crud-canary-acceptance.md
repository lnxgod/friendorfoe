# CON CRUD badge canary acceptance

Overall acceptance: **PENDING**

This ledger is the promotion boundary for the DEFCON 34 CON CRUD badge game.
Canary builds remain private unless an exact hardware-accepted artifact set is
explicitly approved by the owner. A successful compile alone is not
acceptance. The exact `.67.2` exception is recorded below; it does not mark the
broader formal physical matrix complete.

## Artifact boundary

- Production environments are exactly
  `scanner-s3-combo-fof_badge` and `uplink-s3-fof_badge`.
- Canary environments are exactly
  `scanner-s3-combo-fof_badge-con-crud-canary` and
  `uplink-s3-fof_badge-con-crud-canary`.
- CI stores canary binaries only below `private-canary/` in the short-lived
  `con-crud-canary-${GITHUB_SHA}` workflow artifact.
- The `esp32-firmware` artifact, GitHub release assets, factory bundle, and
  GitHub Pages deployment normally reference production environments only.
  The sole exception is the owner-approved, hardware-accepted
  `0.67.2-badge-defcon34` factory bundle. Public packaging must copy those
  exact bytes and verify their sizes and SHA-256 values; rebuilding is not an
  equivalent promotion.
- A canary verifier fails if either build directory has the wrong identity,
  either firmware is missing/empty/a symlink, the two directories resolve to
  the same place, or the canary and production firmware hashes are equal. It
  also validates the production CMake/SDK/embedded identity and rejects
  canary-only tokens or linked symbols in the production image. Canary radio
  evidence comes from the final ELF symbol and source-file tables, not raw or
  discarded linker-map text.

## Automated budgets and radio invariants

| Role | Internal RAM gate (`.dram0.data` + `.dram0.bss`) | App image gate | Required radio evidence | Forbidden radio evidence |
| --- | ---: | ---: | --- | --- |
| Scanner | <= 180,224 bytes (55% of 327,680) | <= 1,363,148 bytes (65% of the 2 MiB app slot) | badge observer, Remote ID, Wi-Fi promiscuous receive, NimBLE init, extended discovery | uplink VHCI advertiser object or badge uplink radio runtime |
| Uplink | <= 212,992 bytes (65% of 327,680) | <= 1,468,464 bytes (the exact accepted `.67.2` ceiling, about 70.022% of the 2 MiB app slot) | badge VHCI advertiser object/runtime and VHCI send/callback symbols | NimBLE host/init or BLE discovery symbols |

The uplink app gate is capped at exactly 1,468,464 bytes: the exact accepted
`.67.2` ceiling, not permission for another aligned block or any open-ended
relaxation. It leaves exactly 628,688 bytes free in each 2 MiB OTA app slot.
The 212,992-byte uplink internal-RAM gate is unchanged.

The verifier parses exactly one top-level `.dram0.data` and `.dram0.bss`
section from each linker map. Missing, duplicate, malformed, unreadable, or
symlinked map evidence is a failure. Generated sdkconfig evidence is also
mandatory: the scanner retains BLE observation, Wi-Fi receive, rollback, and
PSRAM capability allocation; the uplink uses controller-only VHCI advertising
with scanning, central/security roles, Bluedroid, and NimBLE disabled.

## Evidence ledger

### `.92` automated candidate — 2026-07-27

The private `0.64.92-badge-defcon34` candidate adds the infected rescue
countdown without adding a task, protocol field, timer object, or persistent
game-state record. An infected badge starts with 45 rescue points, loses one
point per minute while unrescued, loses additional RSSI-scaled points near
regular or super infected badges, gains existing RSSI-scaled cure progress
near a healer, and becomes permanently dead at zero until reboot. Dead normal
or infected seeds continue spreading at the regular rate; a dead healer
continues spreading as the existing authenticated super zombie.

| Gate | Retained evidence | Status |
| --- | --- | --- |
| Native protocol/game/update suite | 933 of 933 checks passed under the native AddressSanitizer build, including exact 44/45-minute boundaries, `uint32_t` timer wrap, RSSI-scaled regular/super pressure, healer rescue, permanent death, dead spreading, and cured-human non-decay | PASS |
| Focused host regressions | 318 updater tests, 14 private-artifact tests, and 106 firmware-version tests passed | PASS |
| `.92` scanner artifact | Strict production/canary isolation verifier passed; 158,612 bytes internal RAM (21,612 bytes headroom), 1,216,784-byte image (146,364 bytes headroom), SHA-256 `2434b5e4a20c704a886d75d6536319c7a6225aadbfdeec5c12c8a76f9e4f0615` | PASS |
| `.92` uplink artifact | Strict production/canary isolation verifier passed; 209,116 bytes internal RAM (3,876 bytes headroom), 1,467,504-byte image (502 bytes headroom), SHA-256 `9236032ad1cd1b5d176194ec6100e429525022eddef45964db1fa7e649b165aa` | PASS |
| Exact `.92` installation | Badges A (`E0:72:A1:F8:4C:68`, normal), C (`E0:72:A1:F9:47:FC`, immune/healer), and B (`E0:72:A1:F8:86:74`, infected) each report exact `.92` on the uplink and both scanner identities. Every scanner is connected, role-acknowledged, radio-active for its BLE-primary or Wi-Fi-primary role, health `ok`, rollback-clear, crash-free, and in normal recovery; all uplinks are rollback-clear with no retained update session | PASS |
| One-cable scanner relay | Final successful targeted relays transferred the exact 1,216,784-byte scanner image in 1,189 chunks with 0 NACKs: Badge A BLE 95 s, Badge C BLE 35 s, Badge B Wi-Fi 95 s, and Badge B BLE 91 s. Earlier combined campaigns updated each Wi-Fi lane but exposed recoverable BLE readiness failures; Badge C required a non-writing USB ROM identity/reset to clear a stale maintenance session, and Badge B Wi-Fi required a targeted normal-mode scanner reboot to clear its volatile `offer_manifest_mismatch` backoff before the successful retry | PASS WITH RECOVERY |
| Live `.92` transport/memory snapshot | All three uplinks retained USB/UART health after relay. Each reports 8,251,856 of 8,388,608 bytes PSRAM free, at least 36,648 bytes minimum-ever internal heap, at least 31,744 bytes largest internal block, at least 4,160 bytes scanner-UART task stack, 4,816 bytes display stack, and 14,164 bytes USB stack | PASS |
| Physical rescue-countdown game proof | Fresh reboots correctly preserved only normal/healer/infected factory seeds and reset live game state. Button/SSID activation plus observed 45-minute idle decay, RSSI acceleration, healer rescue, death presentation, and dead spreading await an attended three-badge run | PENDING |

### `.91` automated candidate — 2026-07-27

The private `0.64.91-badge-defcon34` candidate fixes the revived-human
passive-damage regression and makes game role and recent encounter effects
unmistakable in the existing health strip. It remains local-only and is not
physically accepted until the exact artifacts below pass on connected badge
graphs.

| Gate | Retained evidence | Status |
| --- | --- | --- |
| Revived-human regression | Native coverage proves a cured human at 50 remains at 50 without an infected encounter; infected cure progress still fades by one point per minute | PASS |
| Native protocol/game/update suite | 931 of 931 checks passed under the native AddressSanitizer build | PASS |
| `.91` scanner artifact | Strict production/canary isolation verifier passed; 158,612 bytes internal RAM (21,612 bytes headroom), 1,216,736-byte image (146,412 bytes headroom), SHA-256 `2b4f93827fe93245d3b86a5cf4fa02c967b1ba403d74ec284f59987d7f14b21e` | PASS |
| `.91` uplink artifact | Strict production/canary isolation verifier passed; 209,116 bytes internal RAM (3,876 bytes headroom), 1,467,440-byte image (566 bytes headroom), SHA-256 `38b68a895b8bf53ac897e46592b44a18dfddd4f64a240f70025394be4854d43d` | PASS |
| Game role/activity strip | Active badges render a large `HUMAN`, `CURED`, `INFECTED`, `HEALER`, or `SUPER ZOMBIE` role and hold `UNDER ATTACK`, `HEALING`, `CURE FADING`, `INFECTED`, or `CURED` encounter effects for about three seconds. Scanner failure and USB safe/recovery status retain display priority | PASS |
| Exact `.91` installation and physical display/game proof | Awaiting installation on the connected three-badge graphs and fresh USB/UART health plus camera evidence | PENDING |

### `.90` automated candidate — 2026-07-27

The private `0.64.90-badge-defcon34` candidate is frozen for physical
testing. It is not yet physically accepted and has not been pushed, tagged,
merged, published, or installed into the factory bundle.

| Gate | Retained evidence | Status |
| --- | --- | --- |
| Native protocol/game/update suite | 930 of 930 checks passed under the native AddressSanitizer build | PASS |
| Host, factory, backend, and Android regressions | 316 updater, 19 redaction, 71 factory, 14 artifact-acceptance, and 106 firmware-version tests passed; Android `testDebugUnitTest` completed successfully | PASS |
| `.90` scanner artifact | Strict production/canary isolation verifier passed; 158,612 bytes internal RAM (21,612 bytes headroom), 1,216,784-byte image (146,364 bytes headroom), SHA-256 `f5415ad54e66325f57ae9452004d7a9e68f8c54695d8f540bcc0e0cc440c5dcd` | PASS |
| `.90` uplink artifact | Strict production/canary isolation verifier passed; 209,100 bytes internal RAM (3,892 bytes headroom), 1,467,984-byte image (22 bytes headroom), SHA-256 `58c297afee987ebfe19cc5b002ff2ffef3336433b1ad15868eeb13164540865b` | PASS |
| Game state/UI | Human, cured, infected, immune, super, dead, and dead-super policies are bounded; the normal four-lane dashboard remains intact; dead replaces only the normal dashboard with the GameChangersAI death screen | PASS |
| Diagnostics boundary | Complete game diagnostics remain on the hardened USB status path. Duplicate canary game fields were removed from local HTTP status to preserve the immutable app-image ceiling and match the USB-only Android control decision | PASS |
| RGB LED game colors | Badge builds intentionally disable the RGB LED task/driver. Enabling its 2 KB task stack and RMT runtime is outside the `.90` memory boundary | DEFERRED |
| Exact `.90` installation on three complete badge graphs | Badges A, B, and C each report exact `.90` on the uplink, BLE-primary scanner, and Wi-Fi-primary scanner. Every scanner is connected, role-acknowledged, radio-active, health `ok`, rollback-clear, and in normal recovery; every uplink reports live USB/UART and zero crashes. Badge C relayed the frozen scanner image to both UART lanes in 1,189 chunks with 0 NACKs per lane; Badge B's interrupted final Wi-Fi chunk recovered after a complete badge power cycle and then relayed 1,189 chunks with 0 NACKs on attempt 1 | PASS |
| Factory seed/reboot proof | Badge A was descriptor-bound and seeded `normal`, Badge B `infected`, and Badge C `immune`. Each exact seed acknowledgment was followed by a commanded reboot, fresh identity/reboot proof, inactive game state, shield zero, and a healthy complete three-board graph | PASS |
| Three-badge activation and BLE propagation | All three badges activated through the physical Easter-egg button flow while their BLE-primary and Wi-Fi-primary scanner roles remained active. A normal badge became infected; both infected badges accumulated immune cure progress and returned to normal; repeated cure/infection cycles reduced maximum shield through the 50/25/12 scar caps | PASS |
| Immune damage and natural regeneration | The immune badge fell from 100 to 83 while infected peers were present. After both peers were cured, five bounded USB samples showed its shield regenerate 88/90/92/94/96, exactly one point per two seconds | PASS |
| Live game-mode transport and memory | During live role transitions all three uplinks retained USB and scanner-UART health. Badge A reported zero crashes, 37,268 bytes current internal heap, 15,580 bytes minimum-ever internal heap, 8,251,856 bytes PSRAM free, and at least 4,084 bytes free in every reported scanner/display UART task stack | PASS |
| Display coexistence | Camera inspection showed all three LCDs continuing to render the normal four-lane scanner interface under their game presentation; no blank display, UI lockup, or scanner-page replacement occurred | PASS |
| Post-game reboot reset | Each badge acknowledged an exact USB reboot after live play. Fresh post-reboot status retained only its normal/infected/immune factory seed and reset the active flag, shield, scar, cured, dead, and super fields; all three complete graphs passed the runtime gate again and require Easter activation before resuming | PASS |
| Remaining formal physical matrix | Exact close/medium/far tier measurements, three-identity stacking, physical dead-super/dead terminal presentation, retained photographs, and the bounded mixed game/scanning/USB soak are not yet complete. Terminal state-machine branches remain covered by the passing native suite, but that is not a substitute for this physical gate | PENDING |

### Retained `.89` two-badge run — 2026-07-27

This is the current local-only acceptance run. The firmware artifacts were
hashed after their final clean builds and were not rebuilt between physical
badges. Nothing from this run was pushed, tagged, merged, or published.

| Gate | Retained evidence | Status |
| --- | --- | --- |
| `.89` scanner artifact | Strict verifier passed; 158,604 bytes internal RAM (21,620 bytes gate headroom), 1,216,720-byte image (146,428 bytes gate headroom), SHA-256 `0a9aabdb97da1ec9f479b77e56fdf003346167d1e61db375c19636dff3ff2f24` | PASS |
| `.89` uplink artifact | Strict verifier passed; 209,100 bytes internal RAM (3,892 bytes gate headroom), 1,467,296-byte image (710 bytes gate headroom), SHA-256 `0022b383a23f6e5778ab7150e22d47b7a2bee49b202db4e80a50ae2cd699456b` | PASS |
| Firmware/transport source contracts | Fresh `.89` version/build and transport contract run passed 289 checks; the full transport contract passed 183 checks | PASS |
| Badge A complete hardware graph | Uplink `E0:72:A1:F8:4C:68`, BLE leaf `E0:72:A1:F8:A0:04`, and Wi-Fi leaf `E0:72:A1:F9:4B:AC` all report exact `.89`; direct USB scanner writes were verified by esptool, both roles/radios/health gates passed | PASS |
| Badge A infected seed and live game | Seed reboot generation `6 -> 7`; fresh live status reports `game_active=true`, `game_seed=infected`, `game_state=infected`, shield `86%`, scanner UART/power converged, rollback clear, safe mode false, and zero crashes | PASS |
| Badge B complete UART graph | Uplink `E0:72:A1:F8:86:74`, BLE leaf `E0:72:A1:F8:85:34`, and Wi-Fi leaf `E0:72:A1:F9:42:54` all report exact `.89`; BLE relay completed in 90 s and Wi-Fi relay in 101 s, each 1,189 chunks/0 NACKs, with final health OK | PASS |
| Badge B immune seed and live game | Seed reboot generation `4 -> 5`; fresh live status reports `game_active=true`, `game_seed=immune`, `game_state=immune`, shield `100%`, scanner UART/power converged, rollback clear, safe mode false, and zero crashes | PASS |
| Two-badge CON CRUD encounter | Both uplinks are active simultaneously; infected/immune roles and shield state are visible while both BLE-primary and Wi-Fi-primary scanners remain active on `.89` | PASS |

The Badge B UART host wrapper was intentionally bounded at 180 seconds. It
timed out only during post-relay verification after the device had completed
the transfer; the uplink autonomously finished its maintenance reboot and a
fresh USB status proved normal operation before seeding. Badge A's scanners
used direct USB writes after its earlier bounded UART readiness campaign
exhausted without sending scanner bytes.

### Retained `.88` two-badge run — historical

This is the earlier local-only run retained for comparison; it is not the
current release candidate.

| Gate | Retained evidence | Status |
| --- | --- | --- |
| Native policy/protocol suite | 918 of 918 checks passed with ASan, including inclusive PSRAM/internal-memory admission boundaries | PASS |
| Firmware identity/version contract | 106 of 106 checks passed for the private `.88` track | PASS |
| Badge firmware transport contract | 182 of 182 checks passed | PASS |
| Same-uplink host update/reconnect suite | 595 of 595 checks passed | PASS |
| Factory provisioning suite | 71 of 71 checks passed after binding default application opens to the immutable USB descriptor and accepting persistent native-USB paths only with a fresh PONG and exact successor reboot generation | PASS |
| Scanner `.88` artifact | Strict verifier passed; 158,604 bytes internal RAM (21,620 bytes gate headroom), 1,216,672-byte image (146,476 bytes gate headroom), SHA-256 `a7b64db5e02e5bf3f339c80d29ed8d27ec91f3339c0d6d257315010a5fe53317` | PASS |
| Uplink `.88` artifact | Strict verifier passed; 209,100 bytes internal RAM (3,892 bytes gate headroom), 1,467,200-byte image (806 bytes gate headroom), SHA-256 `a8cb57f7119a9b9224248000997ea22cfc2d21c0877810b6dc78522c097e6e19` | PASS |
| Badge B complete one-cable update | Uplink `E0:72:A1:F8:86:74`, BLE leaf `E0:72:A1:F8:85:34`, and Wi-Fi leaf `E0:72:A1:F9:42:54` all report exact `.88`. Uplink relayed each 1,216,672-byte scanner image on attempt 1: BLE 1,189 chunks/0 NACKs/165 s; Wi-Fi 1,189 chunks/0 NACKs/115 s. Both roles, profiles, radios, and health gates passed | PASS |
| Badge B seed/reboot proof | `normal` seed acknowledged and persisted; controlled reboot proof generation `1 -> 2`, persistent `/dev/cu.usbmodem1401` reopened through exact descriptor `E0:72:A1:F8:86:74`, reason `usb_reboot`, game inactive, shield 0, complete three-board graph healthy | PASS |
| Badge B post-seed memory/transport | Internal heap free/minimum-ever/largest 58,496/36,816/31,744 bytes; PSRAM free/total/largest 8,251,856/8,388,608/8,126,464 bytes; USB and scanner UART alive; zero crashes, rollback, safe mode, command overflow, UART line overflow, or required-response failures | PASS |
| Badge A complete one-cable update and infected seed | Awaiting physical cable swap to uplink `E0:72:A1:F8:4C:68` | PENDING |
| Physical infection/immunity/cure behavior | Awaiting both completed badges together | PENDING |

The automated numbers and connected-badge observations below were captured
from the final retained local test, build, relay, and fresh-status outputs on
2026-07-26. They seed the ledger; CI and the remaining physical gates must
reproduce the applicable evidence before promotion.

| Gate | Current evidence | Status |
| --- | --- | --- |
| Native policy/protocol suite | 917 of 917 checks passed on the current source boundary | PASS |
| Backend regression suite | Current focused firmware/transport run passed 288 of 288 checks; the earlier broader run passed 605 of 605 | PASS |
| Same-uplink host update/reconnect suite | 443 of 443 checks passed, including live normal/maintenance metrics, exact `.78` baseline binding, compact maintenance campaign, byte-zero retry, descriptor binding, and durable cycle gates | PASS |
| Factory provisioning suite | 68 of 68 checks passed, including exact role seed, transport-loss retry, hardware-ID rebind, pre-reboot response capture, and exact wrap-aware reboot generation succession | PASS |
| Android regression build | `testDebugUnitTest assembleDebug` completed successfully | PASS |
| Generic firmware parity builds | `uplink-s3` and `scanner-s3-combo` completed successfully | PASS |
| Scanner sdkconfig and linked receive path | NimBLE observation, Remote ID, Wi-Fi promiscuous receive, PSRAM, and rollback markers present; uplink broadcaster/runtime markers absent | PASS |
| Scanner internal RAM | 23,012 data + 135,592 bss = 158,604 bytes; 21,620 bytes below gate | PASS |
| Scanner app image | 1,216,288 bytes; 146,860 bytes below gate | PASS |
| Scanner production/canary isolation | Production SHA-256 `4ae1d23ce7e7df0b93694e06e39b46ca6a5f5dd2a5d26d77f37702e7dbde0995`; canary SHA-256 `9dfa021e9767775b9bb5a808d7fa5d00c01644b53395205df34480a55e437259`; exact production CMake/SDK/descriptor/bin/ELF evidence and final-linked canary observer symbols verified | PASS |
| Uplink sdkconfig and linked transmit path | Controller-only VHCI advertising markers present; NimBLE host/discovery markers absent; PSRAM capability allocation enabled | PASS |
| Uplink internal RAM | 23,492 data + 185,608 bss = 209,100 bytes; 3,892 bytes below gate | PASS |
| Uplink app image | 1,466,757 bytes; 1,249 bytes below gate | PASS |
| Uplink production/canary isolation | Fresh production SHA-256 `d62cc886c1810d93d39182b3f023854dfe8d288f159f02fc09eb27cd4f8805d4`; fresh canary SHA-256 `e947336d7d289905888cf120040ae54ca7dcacc213d1f3f7c31e60af4ea001c0`; exact production CMake/SDK/descriptor/bin/ELF evidence and final-linked canary VHCI symbols passed the strict verifier | PASS |
| Uplink strict manifest snapshot | Clean production and canary immutable snapshots and all generated aliases verified byte-for-byte | PASS |
| Connected three-board `.87` identity and health | Fresh post-campaign status bound the uplink and both scanners to exact `0.64.87-badge-defcon34` identities, healthy acknowledged BLE-primary and Wi-Fi-primary roles, idle OTA state, zero crash counts, and active role-appropriate scanning | PASS |
| One complete uplink-to-both-scanner UART campaign | One `.87` scanner image reached both slots through the `.87` uplink: BLE-primary completed in 165 seconds and Wi-Fi-primary in 101 seconds; each transferred 1,189 chunks on attempt 1 with 0 NACKs and final health OK | PASS |
| Live `.87` uplink heap, PSRAM, and stack snapshot | Internal heap free/minimum-ever/largest block: 37,500 / 26,296 / 28,672 bytes; PSRAM free/total/largest block: 8,251,856 / 8,388,608 / 8,126,464 bytes; smallest observed task-stack headroom during/following relay: 4,148 bytes. Scanner-MCU heap, PSRAM, and task-stack metrics are not exposed by the current uplink status and remain unproven | PASS |
| Exact fresh canary artifact on hardware | The physical `.87` campaign exercised uplink SHA-256 `b9e5dc2abafc7509e83b2470ae1fb89df29ceb983efc38f7c909dcc4ef98225f`; ESP-IDF compile-time metadata makes the fresh clean canary artifact hash different even though no canary C source changed in the isolation patch. The fresh `e947…` binary has not been flashed or physically accepted | PENDING |
| Clean CI canary build, strict manifests, and private artifact upload | Awaiting the workflow run containing these gates | PENDING |
| Live canary normal-mode memory | The `.87` uplink snapshot above is captured, but scanner runtime metrics and the complete normal/game-mode detection-queue promotion floors have not yet been proved | PENDING |
| Exact .78 updater baseline comparison | No exact physical `.78` updater baseline and matching `.87` maintenance sample have been captured and bound yet | PENDING |
| Three scanner update cycles | One complete uplink-to-both-scanner `.87` cycle passed; two additional consecutive complete cycles and their reboot/health evidence remain required | PENDING |
| Two-badge game behavior | Infection, immunity, shield, and disinfection behavior has not been exercised with two physical badges | PENDING |
| Dual-button reset | The ten-second dual-button reset has not been physically exercised | PENDING |
| RF/power/soak | Current draw, two-badge game/RF coexistence, disconnect recovery, and extended normal-scanning soak have not been measured | PENDING |
| Two-badge BLE propagation | Seeded infected and immune uplinks have not yet been exercised together on physical badges | PENDING |
| Scanner receive-only parity | No physical confirmation yet that both scanners retain normal BLE, Remote ID, and Wi-Fi detection while the uplink advertises | PENDING |
| No scanner transmit | Requires over-the-air capture or equivalent physical evidence that scanner boards never emit the game advertisement | PENDING |
| Infection, immunity, and shield behavior | Strong-RSSI multi-packet accumulation, shield percentage, infection, and immune disinfection require physical two-badge testing | PENDING |
| Physical Android USB | The current `.87` evidence used the laptop host path; direct Android USB status, theme, palette, display controls, disconnect/reconnect, and game-mode coexistence have not been physically exercised | PENDING |
| USB maintenance and OTA retry | One laptop-hosted `.87` uplink-and-both-scanners campaign passed, but game-active USB command preemption, interrupted-update retry, and the required repeated physical cycles remain unproved | PENDING |
| Buttons and display | Ten-second dual-button reset plus human/infected/immune screen themes require physical testing | PENDING |
| Exact `.67.2` release acceptance | The owner approved public promotion of only the hardware-accepted `.67.2` factory bytes; rebuilt or different artifacts remain rejected | PASS |

## Reproduction commands

From the repository root:

```sh
python -m pytest backend/tests/test_firmware_build_version.py -q

cd esp32/scanner
pio run -e scanner-s3-combo-fof_badge
pio run -e scanner-s3-combo-fof_badge-con-crud-canary
cd ../..
python esp32/scripts/verify_badge_scanner_build.py \
  --build-dir esp32/scanner/.pio/build/scanner-s3-combo-fof_badge-con-crud-canary \
  --partition-source esp32/scanner/partitions_s3_scanner_8mb.csv \
  --sdkconfig esp32/scanner/sdkconfig.scanner-s3-combo-fof_badge-con-crud-canary \
  --canary-production-build-dir esp32/scanner/.pio/build/scanner-s3-combo-fof_badge

cd esp32/uplink
pio run -e uplink-s3-fof_badge
pio run -e uplink-s3-fof_badge-con-crud-canary
cd ../..
python esp32/scripts/verify_badge_uplink_build.py \
  --build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge-con-crud-canary \
  --partition-source esp32/uplink/partitions_s3_fof_badge_8mb.csv \
  --sdkconfig esp32/uplink/sdkconfig.uplink-s3-fof_badge-con-crud-canary \
  --canary-production-build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge
```

The overall formal physical matrix remains **PENDING**. Do not promote an
arbitrary or rebuilt canary. The exact owner-approved embedded `.67.2` factory
bundle is the sole public promotion exception recorded below.

LOCAL EMBEDDED FACTORY PROMOTION: APPROVED on 2026-07-29

OVERALL FORMAL PHYSICAL MATRIX: PENDING

FACTORY BUNDLE: local embedded bundle promoted to `0.67.2-badge-defcon34`

PUBLIC RELEASE TARGET: `v0.67.2-badge-defcon34`

PUBLIC WEB/RELEASE PROMOTION: APPROVED by owner on 2026-07-31

TAG/PUSH/MERGE: authorized for the exact accepted bytes only

### `.67.2` accepted local factory promotion — 2026-07-29

The owner approved local promotion of the physically accepted
`0.67.2-badge-defcon34` artifacts on 2026-07-29. The factory bundle contains
only the exact accepted applications:

| Role | Application size | SHA-256 |
| --- | ---: | --- |
| Uplink | 1,468,464 bytes | `78ef3b6dafe61e8e2fdc3fb28447372aaf76da38cd57ca0961828bbbdc08c434` |
| Scanner | 1,216,800 bytes | `2d0e84501baf3bc929eed03a0b9c1f0272ed66baa9b81dd4513d6dc3fa2c032b` |

Physical acceptance covered all three complete badge graphs: each accepted
badge had one uplink and BLE-primary/Wi-Fi-primary scanners on the exact
`.67.2` version, passed topology, verified flash/readback, seed/reboot, and
healthy-radio gates. The approved per-badge seeds were HUMAN/`normal`,
INFECTED/`infected`, and HEALER/`immune`.

PROMOTION STATUS AT THIS CHECKPOINT: approved for the local embedded factory
bundle only. GitHub release assets, tags, pushes, public web-flasher
assets/manifests, and Pages were unchanged on 2026-07-29. This historical
checkpoint does not change the overall ledger's pending formal physical
matrix.

### `.67.2` final factory/reassignment canary — 2026-07-30

The frozen local embedded factory ZIP was verified as SHA-256
`038d83adcc3e6a561a9192e8bed26ec205e7e7c9374eb6ff800baf573bb44576`.
The static factory suite passed 107 of 107 checks and the bundle builder passed
3 of 3 checks before the connected-badge work. These are private local-factory
evidence only; no release asset, firmware image, or ZIP was changed.

The new-badge physical canary selected HEALER (seed `immune`) and returned
`PASS`. It completed `PROBE`, complete `GRAPH`, and all three production
`FLASH` write/readback gates, followed by three `BOOT` gates, `SEED`, and
`HEALTH`. All three boards reported exact `0.67.2-badge-defcon34` identities;
the post-reboot game state was inactive with shield `0`. USB control and both
scanner UART paths were healthy, as were the acknowledged BLE-primary and
Wi-Fi-primary roles, their active radios, rollback state, and normal recovery.

The role-only physical canaries selected new roles only after the visible
`IDENT` / `ALREADY PASSED` prompt, then completed `BOOT`, `HEALTH`, and `SEED`.
They intentionally performed no `PROBE` or `FLASH`, and their result is
`REASSIGNED`, not `PASS`. The private manufacturing ledger showed complete
devices `3`, scanners `2` before reassignment, then devices `0`, scanners `2`
after reassignment. This confirms that role-only reassignment does not claim a
new complete-device provisioning pass.

No device identifiers or receipts are copied into this public-safe ledger.
FINAL FACTORY CANARY: PASS for new-badge local-bundle provisioning and
REASSIGNED for the role-only canaries. At this 2026-07-30 checkpoint, the
broader formal physical matrix, public release assets, tag, push, merge,
Pages, and web-flasher state remained unchanged.

### `.67.2` exact-byte public promotion - 2026-07-31

The owner approved publishing the hardware-accepted factory bundle through
GitHub Pages, the web flasher, and the immutable tag
`v0.67.2-badge-defcon34`. The scanner application must remain exactly
1,216,800 bytes with SHA-256
`2d0e84501baf3bc929eed03a0b9c1f0272ed66baa9b81dd4513d6dc3fa2c032b`.
The uplink application must remain exactly 1,468,464 bytes with SHA-256
`78ef3b6dafe61e8e2fdc3fb28447372aaf76da38cd57ca0961828bbbdc08c434`.

The public manifests must identify `0.67.2-badge-defcon34` and include the
accepted bootloader, partition table, initial OTA data at `0xF000`, and
application at `0x20000`. Deployment is complete only after Pages and release
assets match every accepted bundle part by size and SHA-256. This narrow
promotion does not convert the remaining physical matrix rows to PASS.
