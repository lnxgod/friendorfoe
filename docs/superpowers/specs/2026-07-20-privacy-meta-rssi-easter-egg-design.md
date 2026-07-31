# Privacy Meta, RSSI, and Easter Egg Design

## Goal

Reduce false privacy alerts without weakening drone detection, and make the
existing one-shot badge Easter egg easier to trigger intentionally.

## Scope

This change covers four behaviors:

1. Meta Glasses require glasses-specific BLE evidence.
2. Non-drone RF detections weaker than `-85 dBm` do not reach the badge UI.
3. The exact Wi-Fi Easter-egg SSID changes to `GameChangersAI-67`.
4. Narrow surveillance-vendor OUIs appear as privacy infrastructure without
   being described as drones or active cameras.

The Hell, Michigan Remote ID trigger, its exact 666 m geodetic altitude
requirement, the button trigger, the thank-you presentation, and the one-shot
until-reboot latch remain unchanged.

A zombie mini-game is deliberately deferred to a separate change. It must not
share scanning or UART hot-path memory and requires live heap measurements on
the badge before implementation.

## Meta Evidence Contract

The scanner may classify a BLE advertisement as Meta Glasses only when at
least one glasses-specific signal is present:

- Luxottica company ID `0x0D53`;
- Ray-Ban Meta service UUID `0xFD5F`; or
- an explicit Ray-Ban, Oakley, Wayfarer, or equivalent glasses name pattern.

Generic Meta company IDs `0x01AB` and `0x058E`, and generic Meta service UUIDs
such as `0xFEB7` and `0xFEB8`, remain generic Meta-device telemetry. They must
not update the scanner's strong Meta Glasses status and must not be promoted by
the uplink to a `Meta Glasses` badge event.

Android follows the same distinction. Generic Meta evidence must not be
presented as camera-equipped smart glasses. Android's local privacy list must
also prune expired entries on time even when no new BLE result arrives.

## RSSI Display Gate

The shared badge threat policy rejects a detection when all of these are true:

- it carries a valid negative RSSI value;
- RSSI is weaker than `-85 dBm`; and
- the source is not a confirmed drone source.

`-85 dBm` itself remains eligible. Values below `-85 dBm`, such as `-86 dBm`,
are filtered. Confirmed Remote ID, DJI IE, Wi-Fi beacon Remote ID, and other
confirmed drone sources are exempt so weak drone evidence remains visible.
Unknown/non-RF RSSI values are also exempt to preserve scanner-health and
serial status behavior.

This is a badge-display admission rule. It does not change parsing of Remote
ID packets or the scanner-to-uplink update protocol.

## Easter Egg SSID

The Wi-Fi trigger is an exact, case-sensitive byte match for:

`GameChangersAI-67`

Prefixes, suffixes, case variants, and embedded-NUL forms do not match. The
existing `fof-goblue` SSID no longer triggers the Easter egg. Repeated beacons
may make discovery reliable, but the existing one-shot latch still prevents a
second display after dismissal until the badge reboots.

## Privacy Infrastructure OUIs

The shared OUI records gain an explicit role so an IEEE assignment cannot be
treated as a drone merely because it is a confident `/24` match. The following
MA-L prefixes are added when absent:

- `E0:A7:00` — Verkada;
- `CC:47:BD` — Rhombus Systems;
- `00:25:DF` — Axon Enterprise;
- `2C:42:05`, `50:DF:95`, `58:A7:48`, and `70:E4:6E` — Lytx.

The existing Flock Safety `B4:1E:52` entry is also assigned an explicit Flock
privacy role. Drone classification accepts only OUI records whose role is
`drone`. Privacy-infrastructure records produce low-priority vendor labels such
as `Axon Device` with detail `privacy infrastructure OUI`. An OUI alone must
never claim that a camera is present or recording. A camera-specific label
requires independent SSID, BLE service/name, or other product evidence.

Android and ESP32 use the same role contract. Locally administered addresses,
shared MA-M/MA-S prefixes, and generic chipset vendors remain ineligible.

## Verification

Regression tests must prove:

- generic Meta company/service evidence does not become Meta Glasses;
- Luxottica, `0xFD5F`, and explicit glasses names still do;
- the uplink status bridge cannot relabel generic Meta as glasses;
- Android removes an expired Meta row without needing another detection;
- non-drone `-86 dBm` is rejected and `-85 dBm` is accepted;
- confirmed drones remain accepted below `-85 dBm`;
- only the exact new SSID matches while the old SSID does not;
- privacy OUIs never enter Android or ESP32 drone paths;
- each added OUI produces its vendor-specific device label without camera or
  recording language;
- the existing Flock OUI retains its dedicated Flock classification;
- native ESP32 tests, Android unit tests, badge scanner build, and badge uplink
  build pass.

Hardware flashing is outside this design step and happens only after the code
and builds pass and the user explicitly asks to flash a connected badge.
